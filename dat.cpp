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

// Real (wall-clock) nanoseconds elapsed since `start`. Used to measure the
// actual compute cost of policy hooks and app think-time, both of which are
// otherwise invisible to the simulated clock (`Stats::sim_time_elapsed`).
inline std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
}

// A steady_clock::time_point, as ns since that clock's epoch -- the form
// app_real_time_start stores, since it's shared across WorkerThreads that
// only agree on a common clock, not on any particular time_point object.
inline std::uint64_t to_ns(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}
inline std::uint64_t steady_ns() {
    return to_ns(std::chrono::steady_clock::now());
}

struct Args {
    std::string evict_policy = "lru";
    std::string prefetch_policy = "none";
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
    std::atomic<std::uint64_t> app_latency_ns{0};     // real time apps spent between handoff and their next request
    std::atomic<std::uint64_t> policy_latency_ns{0};  // real compute time spent inside evict/prefetch policy hooks
};

struct App {
    PipePair* rw_pipe;
    std::promise<int>* res;
    std::size_t id;
};

struct BlockedApp : public App {
    std::uint64_t block_time;
    
    BlockedApp(PipePair* rw_pipe, std::promise<int>* res, std::size_t id, std::uint64_t block_time):
        App(rw_pipe, res, id), block_time(block_time) {}
};

struct ResumeContext {
    std::uint64_t page = 0;
    std::uint64_t ready = 0;    // when the data will be ready
    std::vector<BlockedApp>* blocked_apps = nullptr;
    std::uint64_t trigger_app = 0;  // the app that triggered the fetch
        // we assume that a single trigger app is sufficient
        // because we assume that it is infrequent that 2 different
        // processes request the same page.
        // this is used for eviction.

    ~ResumeContext() = default;

    bool operator<(const ResumeContext& other) const {
        // reverse-ordering
        return ready > other.ready;
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
        if (key == "--evict-policy") {
            a.evict_policy = require_value(i, argc, argv);
        } else if (key == "--capacity") {
            a.capacity = static_cast<std::size_t>(std::stoull(require_value(i, argc, argv)));
        } else if (key == "--in-pipes") {
            a.r_pipes = split_csv(require_value(i, argc, argv));
        } else if (key == "--out-pipes") {
            a.w_pipes = split_csv(require_value(i, argc, argv));
        } else if (key == "--prefetch-policy") {
            a.prefetch_policy = require_value(i, argc, argv);
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


/*
 Invariants:
 - Every app will have a corresponding PipePair* throughout
   the lifetime of dat
 - There is a unique pointer to PipePair* that is either in
   a ResumeContext within pq or in-flight with a WorkerThread
   at all times
 - Every request of each app has a corresponding WorkerThread
 - Access to values in rmap are unique either by accessing 
   or popping the value during the critical section
 - Termination occurs exactly once per app if there are no errors
   by the thread that receives the DONE marker
 */

struct WorkerThread {
    App self; // the app that spawned the worker - all fields are non-null
    policy::Cache& cache;
    policy::CachePolicy* evict_policy;
    policy::CachePolicy* prefetch_policy;
    Stats& stats;
    std::shared_mutex& mu;
    std::priority_queue<ResumeContext>& pq;
    std::unordered_map<std::uint64_t, std::vector<BlockedApp>*>& rmap;    // in-flight map
    std::unordered_map<std::uint64_t, std::uint64_t>& app_real_time_start;    // when we handed off to each app (so that we can get the actual time when control is handed to the worker)
    const std::uint64_t warmup;

    // Times `fn` (a policy hook invocation) and charges its real wall-clock
    // cost as simulated delay, added onto `sink` -- so a computationally
    // expensive policy shows up as real added latency in the simulation
    // instead of being free. Also folds the same measurement into
    // stats.policy_latency_ns for visibility, unless still in warmup.
    // Returns the measured ns in case the caller needs it too (e.g. to also
    // advance stats.sim_time_elapsed).
    template <typename Fn>
    inline std::uint64_t charge_policy_ns(std::uint64_t& sink, bool count_stats, Fn&& fn) {
        auto perf_start = std::chrono::steady_clock::now();
        fn();
        std::uint64_t ns = elapsed_ns(perf_start);
        sink += ns;
        if (count_stats) stats.policy_latency_ns.fetch_add(ns);
        return ns;
    }

    // Same, but the delay belongs to a ResumeContext's `ready` time rather
    // than a bare accumulator (used everywhere past the point where the
    // ResumeContext for this fetch exists).
    template <typename Fn>
    inline std::uint64_t charge_policy_ns(ResumeContext& ctx, bool count_stats, Fn&& fn) {
        return charge_policy_ns(ctx.ready, count_stats, std::forward<Fn>(fn));
    }

    void operator()() {
        std::string line;
        do {
            // look for a request
            if (self.rw_pipe->read_line(line)) {
                self.res->set_value(1);   // crash without terminate
                return;
            }
            if (line.rfind("DONE ", 0) == 0) {
                self.res->set_value(0);   // terminate
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
            self.res->set_value(2);   // crash without terminate
            return;
        }

        // manage real-world timings of simulation
        auto real_time = std::chrono::steady_clock::now();
        auto expected_first_time = no_first_time;
        stats.last_time.store(real_time);
        stats.first_time.compare_exchange_strong(expected_first_time, real_time);

        // manage simulation timings
        std::uint64_t global_idx = stats.request_count.fetch_add(1) + 1;
        bool count_stats = global_idx > warmup;

        // manage cache
        policy::PrefetchRequest prefetch_req;
        bool hit;
        ResumeContext resume;
        std::uint64_t app_latency_ns = 0;
        {
            std::lock_guard _{mu};
            hit = cache.present(page);
            std::uint64_t prefetch_skew_ns = 0;  // Time skew from prefetching work

            // Real (wall-clock) time this app spent between receiving its
            // previous response and issuing this request -- i.e. the app's
            // own think/compute time, entirely outside dat's critical path.
            // Only present once this app has been handed a response before
            // (see the handoff site below where the entry is written).
            auto app_start = app_real_time_start.find(self.id);
            if (app_start != app_real_time_start.end()) {
                std::uint64_t now_ns = to_ns(real_time);
                app_latency_ns = now_ns > app_start->second ? now_ns - app_start->second : 0;
                app_real_time_start.erase(app_start);
            }

            // True critical section -- we update priority queue and cache

            // decide on who to prefetch
            // (must run under `mu`: any prefetch policy that keeps its own
            // mining/recency state -- e.g. an association table -- is shared
            // across concurrently-running WorkerThreads, so calling this
            // outside the lock is a data race even though it looks read-only)
            if (prefetch_policy) {
                charge_policy_ns(prefetch_skew_ns, count_stats, [&]{
                    prefetch_policy->on_prefetch_request(self.id, page, prefetch_req);
                });
            }

            add_to_queue(page, false, prefetch_skew_ns);
            for (std::uint64_t i = 0; i < prefetch_req.n_pages && i < MAX_PREFETCH_PAGES; ++i) {
                add_to_queue(prefetch_req.pages[i], true, prefetch_skew_ns);
            }

            // Pop all fetched pages that arrive by the next timestamp
            resume = pop_from_queue(count_stats);
            // Once popped, resume.ready is no longer picked up by any later
            // store into stats.sim_time_elapsed (unlike inside pop_from_queue
            // itself), so on_access overhead has to bump both explicitly.
            if (evict_policy) {
                stats.sim_time_elapsed.fetch_add(charge_policy_ns(resume, count_stats, [&]{
                    evict_policy->on_access(resume.trigger_app, resume.page);
                }));
            }
            if (prefetch_policy) {
                stats.sim_time_elapsed.fetch_add(charge_policy_ns(resume, count_stats, [&]{
                    prefetch_policy->on_access(resume.trigger_app, resume.page);
                }));
            }

            if (resume.blocked_apps) {
                // this cleanup is unsafe outside of critical section
                rmap.erase(resume.page);
            }
        } // Critical section end

        std::ostringstream resp;
        resp << "RESP " << kPageBytes << "\n";

        std::uint64_t total_blocked_time = 0;
        if (resume.blocked_apps) {
            // respond and yield to blocked apps
            for (auto app : *resume.blocked_apps) {
                if (app.rw_pipe->write_all(resp.str())) {
                    app.res->set_value(3);   // crash due to pipe failure
                    return;
                }

                // Record when we handed off control to this app so that,
                // when it comes back with its next request, we can measure
                // the actual (real) time it took -- see app_latency_ns above.
                {
                    std::lock_guard _{mu};
                    app_real_time_start[app.id] = steady_ns();
                }

                std::thread(WorkerThread{
                    app,
                    cache, evict_policy, prefetch_policy,
                    stats, mu, pq, rmap, app_real_time_start,
                    warmup
                }).detach();
                total_blocked_time += resume.ready - app.block_time;
            }
            delete resume.blocked_apps;
        }

        // log metrics
        if (count_stats) {
            stats.hits.fetch_add(hit);
            stats.misses.fetch_add(1 - hit);
            stats.total_latencies.fetch_add(total_blocked_time);
            stats.app_latency_ns.fetch_add(app_latency_ns);
        }
    }

    void add_to_queue(std::uint64_t page, bool is_prefetch, std::uint64_t skew_ns) {
        // non-thread safe
        // adds the page to in-flight queue
        // returns the time elapsed before the page will be available again

        // crashes when (page=0, is_prefetch=false)

        // Check if rmap already has the entry
        auto in_flight = rmap.find(page);
        std::vector<BlockedApp>* resume_apps;
        std::uint64_t sim_time = stats.sim_time_elapsed.load();
        if (in_flight == rmap.end()) {
            std::uint64_t sim_elapsed_ns = cache.get_page_ns(page) + skew_ns;
            resume_apps = new std::vector<BlockedApp>{};

            pq.emplace(
                page,
                sim_time + sim_elapsed_ns,
                resume_apps,
                self.id
            );
            rmap[page] = resume_apps;
        } else {
            resume_apps = in_flight->second;
        }

        if (!is_prefetch)
            resume_apps->emplace_back(
                self.rw_pipe,
                self.res,
                self.id,
                sim_time
            );
    }

    ResumeContext pop_from_queue(bool count_stats) {
        // not thread safe
        // pops until we have threads to yield to; prefetch requests will also be handled

        if (pq.empty()) return ResumeContext{};

        ResumeContext resume = pq.top(); pq.pop();
        while(resume.blocked_apps->empty()) {
            add_to_cache(resume, count_stats);
            stats.sim_time_elapsed.store(resume.ready);
            // clean up for metadata discarded
            rmap.erase(resume.page);
            delete resume.blocked_apps;
            if (pq.empty()) return ResumeContext{};
            resume = pq.top(); pq.pop();
        }
        add_to_cache(resume, count_stats);   // this is the actual page that things are blocked on
        std::uint64_t cur_time = stats.sim_time_elapsed.load();
        if (resume.ready > cur_time) stats.sim_time_elapsed.store(resume.ready);
        return resume;
    }

    void add_to_cache(ResumeContext& ctx, bool count_stats) {
        if (cache.present(ctx.page)) return;    // TODO consider what happens if we
            // instead of noop-ing eviction when ctx is present,
            // we still allow eviction, just don't add ctx.page into cache.
            // this is helpful if we ever allow batch flushing to remote volume
            // (we get to evict faster later) but for simulation, it will not matter.

        policy::EvictRequest evictions;
        if(evict_policy) {
            charge_policy_ns(ctx, count_stats, [&]{
                evict_policy->on_evict_request(ctx.trigger_app, ctx.page, evictions);
            });
        }

        // we will accept all suggested evictions for now
        for (std::uint64_t i = 0; i < evictions.n_pages; ++i) {
            auto evict_page = evictions.pages[i];
            cache.evict(evict_page);
            notify_evict(ctx, evict_page, count_stats);
        }

        // admit
        auto force_evicted = cache.insert(ctx.page);
        if (!force_evicted.first) {
            std::cerr << "random eviction: " << force_evicted.second << std::endl;
            // if the eviction request doesn't free up space, then
            // we need to evict another page
            notify_evict(ctx, force_evicted.second, count_stats);
        }
        notify_admit(ctx, ctx.page, count_stats);
    }

    // Notify the eviction policy of a page transition exactly once, regardless
    // of whether any app is currently blocked waiting on it. A page that
    // finishes prefetching before any real request arrives for it still needs
    // to be tracked here -- otherwise it stays resident in `cache` but becomes
    // invisible to the policy's own eviction-candidate bookkeeping.
    inline void notify_evict(ResumeContext& ctx, std::uint64_t page, bool count_stats) {
        if (evict_policy) {
            charge_policy_ns(ctx, count_stats, [&]{ evict_policy->on_evict(ctx.trigger_app, page); });
        }
        if (count_stats) stats.evictions.fetch_add(1);
    }

    inline void notify_admit(ResumeContext& ctx, std::uint64_t page, bool count_stats) {
        if (evict_policy) {
            charge_policy_ns(ctx, count_stats, [&]{ evict_policy->on_admit(ctx.trigger_app, page); });
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
        if (args.evict_policy == "fifo") {
            evict_policy = new policy::FIFOPolicy(cache);
        } else if (args.evict_policy == "lifo") {
            evict_policy = new policy::LIFOPolicy(cache);
        } else if (args.evict_policy == "lru") {
            evict_policy = new policy::LRUPolicy(cache);
        } else if (args.evict_policy != "none") {
            throw std::runtime_error("Unsupported --evict-policy: " + args.evict_policy);
        }

        if (args.prefetch_policy == "readahead") {
            prefetch_policy = new policy::ReadaheadPolicy(cache);
        } else if (args.prefetch_policy == "cminer") {
            prefetch_policy = new policy::CMinerPolicy(cache);
        } else if (args.prefetch_policy == "quickmine") {
            prefetch_policy = new policy::QuickMinePolicy(cache);
        } else if (args.prefetch_policy == "mithril") {
            prefetch_policy = new policy::MithrilPolicy(cache);
        } else if (args.prefetch_policy != "none") {
            throw std::runtime_error("Unsupported --prefetch-policy: " + args.prefetch_policy);
        }

        Stats stats;
        stats.first_time.store(no_first_time);  // cmp-xchg to set on first request
        stats.last_time.store(no_last_time);    // swap to update on each request

        std::size_t n = args.w_pipes.size();
        std::vector<std::promise<int>> promises(n);
        std::vector<std::future<int>> responses(n);
        std::vector<PipePair> pipes(n);
        std::priority_queue<ResumeContext> pq;
        std::unordered_map<std::uint64_t, std::vector<BlockedApp>*> rmap;
        std::unordered_map<std::uint64_t, std::uint64_t> app_real_time_start;
        std::shared_mutex mu;   // Protects critical sections

        for (std::size_t worker_id = 0; worker_id < args.w_pipes.size(); ++worker_id) {
            // Explicitly copy the strings by value to avoid thread reference races
            int r_fd = args.r_pipes[worker_id];
            int w_fd = args.w_pipes[worker_id];
            std::uint64_t warmup = args.warmup_period;
            responses[worker_id] = promises[worker_id].get_future();
            pipes[worker_id] = {w_fd, r_fd};

            std::thread(WorkerThread{
                {&pipes[worker_id], &promises[worker_id], worker_id},
                cache, evict_policy, prefetch_policy,
                stats, mu, pq, rmap, app_real_time_start,
                warmup
            }).detach();
        }

        for (std::size_t i = 0; i < responses.size(); ++i) {
            int code = responses[i].get();
            std::cerr << "Thread " << i << " exited with code " << code << std::endl;
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
                  << "app_latency_ns=" << stats.app_latency_ns << ' '
                  << "policy_latency_ns=" << stats.policy_latency_ns << ' '
                  << "runtime_seconds=" << runtime_seconds << ' '
                  << "hit_ratio=" << (measured == 0 ? 0.0 : (double)stats.hits / measured)
                  << "\n";

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dat: " << ex.what() << "\n";
        return 1;
    }
}
