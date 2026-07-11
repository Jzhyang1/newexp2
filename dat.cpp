#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pipe_pair.hpp"
#include "policies/policy_api.h"

namespace {

constexpr std::size_t kPageBytes = 4096;
auto no_first_time = std::chrono::steady_clock::time_point::max();
auto no_last_time = std::chrono::steady_clock::time_point::min();

struct Args {
    std::string cache_policy = "lru";
    std::string prefetch = "none";
    std::uint64_t prefetch_amount = 0; // only used for some prefetchers; ignored otherwise
    std::size_t capacity = 4096;
    std::vector<int> r_pipes;
    std::vector<int> w_pipes;
    std::uint64_t miss_delay_ns = 0;
    std::uint64_t hit_delay_ns = 0;
    std::string file_data;
    std::uint64_t warmup_period = 0;
};

struct Stats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::vector<std::uint64_t> latencies_ns;
};

struct ResumeContext {
    std::uint64_t ready;
    std::uint64_t page;
    PipePair app;

    ~ResumeContext() = default;

    bool operator<(const ResumeContext& other) const {
        return ready < other.ready;
    }
};

std::vector<int> split_csv(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        out.push_back(std::stoi(part));
    }
    return out;
}

std::string require_value(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + argv[i]);
    }
    ++i;
    return argv[i];
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--cache-policy") {
            a.cache_policy = require_value(i, argc, argv);
        } else if (key == "--capacity") {
            a.capacity = static_cast<std::size_t>(std::stoull(require_value(i, argc, argv)));
        } else if (key == "--in-pipes") {
            a.r_pipes = split_csv(require_value(i, argc, argv));
        } else if (key == "--out-pipes") {
            a.w_pipes = split_csv(require_value(i, argc, argv));
        } else if (key == "--prefetch") {
            a.prefetch = require_value(i, argc, argv);
        } else if (key == "--miss-delay") {
            a.miss_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--hit-delay") {
            a.hit_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--file-data") {
            a.file_data = require_value(i, argc, argv);
        } else if (key == "--warmup-period") {
            a.warmup_period = std::stoull(require_value(i, argc, argv));
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }
    if (a.capacity == 0) {
        throw std::runtime_error("--capacity must be > 0");
    }
    if (a.r_pipes.empty() || a.w_pipes.empty() || a.r_pipes.size() != a.w_pipes.size()) {
        throw std::runtime_error("--in-pipes and --out-pipes must be non-empty with equal lengths");
    }
    if (a.file_data.empty()) {
        throw std::runtime_error("--file-data is required");
    }
    return a;
}

uint64_t percentile_ns(std::vector<std::uint64_t> data, double p) {
    if (data.empty()) {
        return 0;
    }
    std::sort(data.begin(), data.end());
    std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(data.size() - 1));
    return data[idx];
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);

        int backing_fd = open(args.file_data.c_str(), O_RDWR | O_CREAT, 0644);
        if (backing_fd < 0) {
            std::cerr << "dat: failed to open backing file: " << args.file_data << "\n";
            return 1;
        }

        policy::Cache cache(args.capacity, args.hit_delay_ns, args.miss_delay_ns);
        // if (args.cache_policy == "lru") {
        //     policy::create_lru(cache);
        // } else if (args.cache_policy == "lru-cxt-aware") {
        //     policy::create_lru_cxt_aware(cache);
        // } else {
        //     throw std::runtime_error("Unsupported --cache-policy: " + args.cache_policy);
        // }

        if (args.prefetch == "readahead") {
            policy::create_readahead(cache, args.prefetch_amount);
        } else if (args.prefetch != "none") {
            throw std::runtime_error("Unsupported --prefetch: " + args.prefetch);
        }

        Stats stats;
        std::atomic<std::uint64_t> request_counter{0};
        std::atomic<std::chrono::steady_clock::time_point> 
            first_request_time{no_first_time},   // cmp-xchg to set on first request
            last_request_time{no_last_time};    // swap to update on each request
        std::uint64_t sim_elapsed_ns = 0;

        std::vector<std::thread> workers;
        workers.reserve(args.w_pipes.size());
        std::priority_queue<ResumeContext> pq;
        std::shared_mutex mu;   // Protects critical sections

        for (std::size_t worker_id = 0; worker_id < args.w_pipes.size(); ++worker_id) {
            // Explicitly copy the strings by value to avoid thread reference races
            int r_fd = args.r_pipes[worker_id];
            int w_fd = args.w_pipes[worker_id];
            std::uint64_t warmup = args.warmup_period;

            workers.emplace_back([
                r_fd, w_fd, warmup, 
                &cache, &request_counter, 
                &sim_elapsed_ns, &stats,
                &first_request_time, &last_request_time, 
                &mu, &pq
            ]() {
                // dat reads requests from out_pipe_path, writes responses to in_pipe_path
                PipePair rw_pipe { w_fd, r_fd };

                std::string line;
                while (!rw_pipe.read_line(line)) {
                    if (line.rfind("DONE ", 0) == 0) {
                        break;
                    }
                    if (line.rfind("REQ ", 0) != 0) {
                        continue;
                    }

                    std::istringstream in(line);
                    std::string op;
                    std::uint32_t client_id = 0;
                    std::uint64_t seq = 0;
                    std::uint64_t page = 0;
                    if (!(in >> op >> client_id >> seq >> page)) {
                        continue;
                    }

                    mu.lock_shared();
                    auto start_time = std::chrono::steady_clock::now();
                    bool hit = cache.present(page);
                    auto data_available_ns = sim_elapsed_ns + 
                        (hit ? cache.hit_latency_ns : cache.miss_latency_ns);

                    if (!mu.try_lock()) {
                        // try to acquire write permissions to cache
                        mu.unlock_shared();
                        mu.lock();
                    }

                    // True critical section -- we update priority queue and cache
                    pq.push(ResumeContext{
                        data_available_ns,
                        page,
                        rw_pipe,
                    });

                    ResumeContext resume = pq.top(); pq.pop();
                    cache.insert(resume.page, 0);  // we presently assume infinite cache size
                    sim_elapsed_ns = resume.ready;
                    rw_pipe = resume.app;   // we currently don't coalesce in-flight requests

                    mu.unlock();
                    // Critical section end
                    
                    last_request_time.store(start_time);
                    first_request_time.compare_exchange_strong(no_first_time, start_time);

                    std::uint64_t global_idx = request_counter.fetch_add(1) + 1;
                    if (global_idx > warmup) {
                        std::lock_guard _(mu);
                        stats.hits += hit;
                        stats.misses += (1 - hit);
                        stats.evictions = 0;    // TODO evictions
                        stats.latencies_ns.push_back(sim_elapsed_ns);
                    }

                    std::ostringstream resp;
                    resp << "RESP " << kPageBytes << "\n";
                    if (rw_pipe.write_all(resp.str())) {
                        break;
                    }
                }

                rw_pipe.close();
            });
        }

        for (auto& t : workers) {
            t.join();
        }

        close(backing_fd);

        const std::uint64_t measured = stats.hits + stats.misses;
        const double avg_latency = (double)std::accumulate(stats.latencies_ns.begin(), stats.latencies_ns.end(), 0ull) /
                  stats.latencies_ns.size();
        const double runtime_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(last_request_time.load() - first_request_time.load()).count();
        const std::uint64_t p50 = percentile_ns(stats.latencies_ns, 0.50);
        const std::uint64_t p95 = percentile_ns(stats.latencies_ns, 0.95);
        const std::uint64_t p99 = percentile_ns(stats.latencies_ns, 0.99);

        std::cout << "STATS "
                  << "hits=" << stats.hits << ' '
                  << "misses=" << stats.misses << ' '
                  << "evictions=" << stats.evictions << ' '
                  << "bytes_read=" << stats.bytes_read << ' '
                  << "bytes_written=" << stats.bytes_written << ' '
                  << "avg_latency_ns=" << avg_latency << ' '
                  << "runtime_seconds=" << runtime_seconds << ' '
                  << "p50_latency_ns=" << p50 << ' '
                  << "p95_latency_ns=" << p95 << ' '
                  << "p99_latency_ns=" << p99 << ' '
                  << "hit_ratio=" << (measured == 0 ? 0.0 : (double)stats.hits / measured)
                  << "\n";

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dat: " << ex.what() << "\n";
        return 1;
    }
}
