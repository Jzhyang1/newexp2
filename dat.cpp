#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
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
    std::size_t capacity = (1 << 20);
    std::vector<int> r_pipes;
    std::vector<int> w_pipes;
    std::uint64_t miss_delay_ns = 0;
    std::uint64_t hit_delay_ns = 0;
    std::string file_data;
    std::uint64_t warmup_period = 0;
};

struct Stats {
    std::atomic<std::uint64_t> request_count{0};
    std::atomic<std::uint64_t> sim_time_elapsed{0};
    std::atomic<std::chrono::steady_clock::time_point> first_time, last_time;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> bytes_read{0};
    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint64_t> total_latencies{0};
};

struct ResumeContext {
    std::uint64_t ready;
    std::uint64_t page;
    PipePair* app;

    ~ResumeContext() = default;

    bool operator<(const ResumeContext& other) const {
        if (ready == other.ready) {
            // prefetched pages come first on tie because
            // 1. prefetched pages should show up by the time they are accessed without triggering another remote disk access
            // 2. because we will evict prefetched pages first, so we make prefetched pages older
            return app == nullptr && other.app != nullptr;
        }
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

}  // namespace

struct WorkerThread {
    PipePair* rw_pipe;
    policy::Cache& cache;
    policy::CachePolicy* evict_policy;
    policy::CachePolicy* prefetch_policy;
    Stats& stats;
    std::shared_mutex& mu;
    std::priority_queue<ResumeContext>& pq;
    std::promise<int>& res;
    const std::uint64_t warmup;

    void operator()() {
        std::string line;
        do {
            // look for a request
            if (rw_pipe->read_line(line)) {
                res.set_value(1);   // crash without terminate
                return;
            }
            if (line.rfind("DONE ", 0) == 0) {
                res.set_value(0);   // terminate
                return;
            }
        } while (line.rfind("REQ ", 0) != 0);

        std::istringstream in(line);
        std::string op;
        std::uint32_t client_id = 0;
        std::uint64_t seq = 0;
        std::uint64_t page = 0;
        if (!(in >> op >> client_id >> seq >> page)) {
            std::cerr << "Bad request: " << line << std::endl;
            res.set_value(2);   // crash without terminate
            return;
        }

        // manage real-world timings of simulation
        auto real_time = std::chrono::steady_clock::now();
        stats.last_time.store(real_time);
        stats.first_time.compare_exchange_strong(no_first_time, real_time);

        // manage simulation timings
        std::uint64_t global_idx = stats.request_count.fetch_add(1) + 1;
        std::uint64_t sim_time = stats.sim_time_elapsed.load();

        // decide on who to prefetch
        policy::PrefetchRequest prefetch_req;
        if (prefetch_policy) prefetch_policy->on_prefetch_request(rw_pipe->r_fd, page, prefetch_req);

        // manage cache
        mu.lock_shared();
        bool hit = cache.present(page);

        if (!mu.try_lock()) {
            // try to acquire write permissions to cache
            mu.unlock_shared();
            mu.lock();
        }

        // True critical section -- we update priority queue and cache
        for (std::uint64_t i = 0; i < prefetch_req.n_pages && i < MAX_PREFETCH_PAGES; ++i) {
            pq.push(ResumeContext{
                sim_time + cache.get_page_ns(prefetch_req.pages[i]),
                prefetch_req.pages[i],
                nullptr,
            });
        }
        std::uint64_t sim_elapsed_ns = cache.get_page_ns(page);
        pq.push(ResumeContext{
            sim_time + sim_elapsed_ns,
            page,
            rw_pipe,
        });

        // Pop all fetched pages that arrive by the next timestamp
        
        ResumeContext resume;
        do {
            resume = pq.top(); pq.pop();
            cache.insert(resume.page, 0);   // we presently assume infinite cache size
            stats.sim_time_elapsed.store(resume.ready);
        } while(resume.app == nullptr);     // get the next app to yield to

        mu.unlock();
        // Critical section end

        std::ostringstream resp;
        resp << "RESP " << kPageBytes << "\n";
        if (rw_pipe->write_all(resp.str())) {
            res.set_value(3);   // crash due to pipe failure
            return;
        }

        // go to next app
        std::thread(WorkerThread{
            resume.app,
            cache, evict_policy, prefetch_policy,
            stats, mu, pq, res,
            warmup
        });

        // log metrics
        if (global_idx > warmup) {
            stats.hits.fetch_add(hit);
            stats.misses.fetch_add(1 - hit);
            // stats.evictions = 0;    // TODO evictions
            stats.total_latencies.fetch_add(sim_elapsed_ns);
        }
    }
};

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);

        int backing_fd = open(args.file_data.c_str(), O_RDWR | O_CREAT, 0644);
        if (backing_fd < 0) {
            std::cerr << "dat: failed to open backing file: " << args.file_data << "\n";
            return 1;
        }

        policy::Cache cache(args.capacity, args.hit_delay_ns, args.miss_delay_ns);
        policy::CachePolicy* evict_policy = nullptr;
        policy::CachePolicy* prefetch_policy = nullptr;
        // if (args.cache_policy == "lru") {
        //     policy::create_lru(cache);
        // } else if (args.cache_policy == "lru-cxt-aware") {
        //     policy::create_lru_cxt_aware(cache);
        // } else {
        //     throw std::runtime_error("Unsupported --cache-policy: " + args.cache_policy);
        // }

        if (args.prefetch == "readahead") {
            prefetch_policy = new policy::ReadaheadPolicy(cache);
        } else if (args.prefetch != "none") {
            throw std::runtime_error("Unsupported --prefetch: " + args.prefetch);
        }

        Stats stats;
        stats.first_time.store(no_first_time);  // cmp-xchg to set on first request
        stats.last_time.store(no_last_time);    // swap to update on each request

        std::vector<std::future<int>> responses(args.w_pipes.size());
        std::vector<PipePair> pipes(args.w_pipes.size());
        std::priority_queue<ResumeContext> pq;
        std::shared_mutex mu;   // Protects critical sections

        for (std::size_t worker_id = 0; worker_id < args.w_pipes.size(); ++worker_id) {
            // Explicitly copy the strings by value to avoid thread reference races
            int r_fd = args.r_pipes[worker_id];
            int w_fd = args.w_pipes[worker_id];
            std::uint64_t warmup = args.warmup_period;
            std::promise<int> promise{};
            responses[worker_id] = promise.get_future();
            pipes[worker_id] = {w_fd, r_fd};

            std::thread(WorkerThread{
                &pipes[worker_id],
                cache, evict_policy, prefetch_policy,
                stats, mu, pq, promise,
                warmup
            });
        }

        for (std::size_t i = 0; i < responses.size(); ++i) {
            int code = responses[i].get();
            std::cout << "Thread " << i << " exited with code " << code << std::endl;
            pipes[i].close();
        }

        close(backing_fd);

        const std::uint64_t measured = stats.hits + stats.misses;
        const double avg_latency = (double) stats.total_latencies.load() / stats.request_count.load();
        const double runtime_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(stats.last_time.load() - stats.first_time.load()).count();

        std::cout << "STATS "
                  << "hits=" << stats.hits << ' '
                  << "misses=" << stats.misses << ' '
                  << "evictions=" << stats.evictions << ' '
                  << "bytes_read=" << stats.bytes_read << ' '
                  << "bytes_written=" << stats.bytes_written << ' '
                  << "avg_latency_ns=" << avg_latency << ' '
                  << "runtime_seconds=" << runtime_seconds << ' '
                  << "hit_ratio=" << (measured == 0 ? 0.0 : (double)stats.hits / measured)
                  << "\n";

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dat: " << ex.what() << "\n";
        return 1;
    }
}
