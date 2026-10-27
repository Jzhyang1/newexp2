/**
 * Things we do:
 * - measure the total latency of several running workers (VMs) (accounting for overlap)
 * - measure the total latency of all disk reads (accounting for VM's adjusted time and overlap)
 * - forward the data from a disk file
 * - measure the time the simulation took to run
 * 
 * Things we don't do:
 * - preserve ordering between workers (e.g. P1 wants x,z and P2 wants y,x P2's x may be
 *      returned after P1's z even if it is a cache hit and faster)
 * - in-flight coalescing (no in-flight requests)
 * 
 * Things we plan to do:
 * - prevent workers from getting too far out of sync by having all processes whose virtual
 *      time is more than delta from the latest virtual time to catch up
 */

#include "nbd.hpp"
#include "../policies/policy_api.h"
#include <stdio.h>
#include <atomic>
#include <vector>
#include <future>

namespace nbd {

// struct AccessTime {
//     // these are adjusted wall clock times
//     uint64_t start, end;
// };


class SimReadClass : public BlockReadClass {
    // handles the priority queue blocking
    // + time measurements
    FILE* f;

    Stats stats;
    uint64_t warmup;
    policy::Cache& cache;
    policy::CachePolicy* evict_policy;
    policy::CachePolicy* prefetch_policy;
    // Optional per-request event log (worker_id,offset,length,hits,total),
    // independent of warmup/Stats -- lets a test cross-check exactly which
    // NBD read requests this class saw against what a client/guest actually
    // issued. Null (the common case) disables it entirely.
    FILE* access_log;
    // Optional per-prefetch-range log (worker_id,offset,length,hits,total,req),
    // same shape as access_log so disk/heatmap.py renders it unchanged. One row
    // per range the prefetch policy asked for; `hits` = pages already cached
    // (wasted prefetch), `req` = index of the read request that triggered it.
    FILE* prefetch_log;
    uint64_t sim_start;
    std::atomic<uint64_t> global_idx;

    volatile uint64_t steady_max_virt_ns;   // equivalent to total virt time elapsed
    uint64_t warmup_end_real_ns{0};         // steady_ns() at the last warmup request (0 = no warmup seen)
    uint64_t warmup_end_virt_ns{0};         // steady_max_virt_ns as of the last warmup request
    std::unordered_map<uint64_t, uint64_t> worker_virtual_time;
    std::unordered_map<uint64_t, uint64_t> worker_real_time_start;
    // std::vector<AccessTime> disk_access_times;   // tracks all prior disk accesses to coalesce
    std::shared_mutex mu;   // Protects critical sections

public:
    SimReadClass(
            FILE* f,
            uint64_t vm_count,
            uint64_t warmup,
            policy::Cache& cache,
            policy::CachePolicy* evict_policy,
            policy::CachePolicy* prefetch_policy,
            FILE* access_log = nullptr,
            FILE* prefetch_log = nullptr
        ):  f(f), warmup(warmup), cache(cache),
            evict_policy(evict_policy), prefetch_policy(prefetch_policy),
            access_log(access_log), prefetch_log(prefetch_log) {
                sim_start = steady_ns();
            }

    uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf,
                  uint64_t* miss_blocks = nullptr) override {
        // manage warmup period
        uint64_t req_idx = global_idx.fetch_add(1);
        bool count_stats = req_idx >= warmup;

        // bytes to disk units
        if (offset % SECTOR_SIZE || length % SECTOR_SIZE) {
            std::cerr << "got invalid (offset=" << offset << ",length=" << length << ")\n";
        }
        std::uint64_t block_offset = offset / SECTOR_SIZE;
        std::uint64_t block_length = length / SECTOR_SIZE;

        // timing (worker)
        uint64_t now_ns = steady_ns();
        uint64_t worker_virt_start_ns, worker_virt_end_ns;
        uint64_t worker_run_ns = 0;
        {
            std::lock_guard _{mu};

            // Virtual (simulation) time -- i.e. the time this app is living at
            // if it were run in a real system
            auto worker_virt = worker_virtual_time.find(worker_id);
            if (worker_virt != worker_virtual_time.end()) {
                worker_virt_start_ns = worker_virt->second;
            } else {
                worker_virt_start_ns = steady_max_virt_ns;
            }

            // Real (wall-clock) time this app spent between its previous
            // response being fully sent (see response_sent) and issuing this
            // request -- i.e. the VM's own think/compute time, excluding any
            // disk-side delay.
            auto worker_start = worker_real_time_start.find(worker_id);
            if (worker_start != worker_real_time_start.end()) {
                worker_run_ns += now_ns - worker_start->second;
                worker_real_time_start.erase(worker_start);
            }
        }

        // manage cache
        uint64_t total_count = 0;
        uint64_t hit_count = 0;
        uint64_t policy_ns = 0;  // Real compute time of every evict/prefetch policy hook this request
        policy::PrefetchRequest prefetch_req;
        {
            std::lock_guard _{mu};
            if (prefetch_policy) {
                policy_ns += charge_policy_ns([&]{
                    prefetch_policy->on_prefetch_request(
                        worker_id, block_offset, block_length, prefetch_req);
                });
            }
        }
        for (std::uint64_t block_suboffset = 0; block_suboffset < block_length; ++block_suboffset) {
            std::lock_guard _{mu};
            bool hit = cache.present(block_offset + block_suboffset);
            total_count += 1;
            hit_count += hit;

            policy_ns += add_to_cache(worker_id, policy::FetchRange{block_offset + block_suboffset, 1}, false, count_stats);

            // acknowledge access
            if (evict_policy) {
                policy_ns += charge_policy_ns([&]{
                    evict_policy->on_access(worker_id, block_offset + block_suboffset);
                });
            }
            if (prefetch_policy) {
                policy_ns += charge_policy_ns([&]{
                    prefetch_policy->on_access(worker_id, block_offset + block_suboffset);
                });
            }

            // TODO find the earliest worker
            // block if the worker_run_ns is more than MAX_SKEW_NS greater than
            // the earliest worker
        }
        {
            // admit the prefetched pages last
            std::lock_guard _{mu};
            for (uint64_t i = 0; i < prefetch_req.fetch_count; ++i) {
                if (prefetch_log) {
                    const auto& r = prefetch_req.fetch_ranges[i];
                    uint64_t cached = 0;
                    for (std::uint64_t b = 0; b < r.block_length; ++b) cached += cache.present(r.block_offset + b);
                    std::fprintf(prefetch_log, "%llu,%llu,%llu,%llu,%llu,%llu\n",
                        static_cast<unsigned long long>(worker_id),
                        static_cast<unsigned long long>(r.block_offset) * SECTOR_SIZE,
                        static_cast<unsigned long long>(r.block_length) * SECTOR_SIZE,
                        static_cast<unsigned long long>(cached),
                        static_cast<unsigned long long>(r.block_length),
                        static_cast<unsigned long long>(req_idx));
                    std::fflush(prefetch_log);
                }
                policy_ns += add_to_cache(worker_id, prefetch_req.fetch_ranges[i], true, count_stats);
            }
        }
        
        uint64_t total_elapsed_ns;
        uint64_t miss_ns = 0;  // modeled miss delay charged to this request
        {
            std::lock_guard _{mu};
            // Make sure all read requests are charged
            total_elapsed_ns = worker_run_ns + policy_ns;
            // Modeled miss latency: flat per request with >=1 miss, matching
            // the physical delay applied in nbd.cpp's ServeTransmission.
            if (hit_count < total_count) miss_ns = cache.miss_latency_ns;
            total_elapsed_ns += miss_ns;
            worker_virt_end_ns = worker_virt_start_ns + total_elapsed_ns;
            if (worker_virt_end_ns > steady_max_virt_ns) steady_max_virt_ns = worker_virt_end_ns;
            worker_virtual_time[worker_id] = worker_virt_end_ns;
            // Everything up to the last warmup request is excluded from the
            // reported simulated time.
            if (!count_stats) {
                warmup_end_virt_ns = steady_max_virt_ns;
                warmup_end_real_ns = steady_ns();
            }
            // disk_access_times.emplace_back(worker_virt_start_ns + worker_run_ns, worker_virt_end_ns);

            // Logged unconditionally (unlike Stats, which skips warmup) so
            // the log is a faithful record of every request this class saw.
            if (access_log) {
                std::fprintf(access_log, "%llu,%llu,%zu,%llu,%llu\n",
                    static_cast<unsigned long long>(worker_id),
                    static_cast<unsigned long long>(offset),
                    length,
                    static_cast<unsigned long long>(hit_count),
                    static_cast<unsigned long long>(total_count));
                std::fflush(access_log);
            }
        }

        if (miss_blocks) *miss_blocks = total_count - hit_count;

        // perform the actual response. pread (rather than fseek+fread) is
        // used because `f` is shared by every client thread: fseek would
        // race with concurrent reads at other offsets, while pread's offset
        // argument makes each read atomic and lock-free.
        ssize_t bytes_got = pread(fileno(f), buf, length, static_cast<off_t>(offset));
        if (bytes_got < 0) bytes_got = 0;
        
        // final metric - tracking
        if (count_stats) {
            std::lock_guard _{mu};

            stats.request_count++;
            stats.bytes_read += length;
            stats.hits += hit_count;
            stats.misses += total_count - hit_count;
            stats.miss_requests += hit_count < total_count;
            stats.miss_delay_ns += miss_ns;
            stats.total_latencies += total_elapsed_ns;
            stats.worker_latency_ns += worker_run_ns;
            stats.policy_latency_ns += policy_ns;
        }
        return bytes_got;
    }

    // Start the worker's think-time clock. Done here (after the physical miss
    // delay and the socket write) rather than at the end of read(), so
    // neither counts as worker time. Runs during warmup too, so the first
    // post-warmup request measures a real gap rather than 0.
    void response_sent(uint64_t worker_id) override {
        std::lock_guard _{mu};
        worker_real_time_start[worker_id] = steady_ns();
    }

    void drain() override {
        // nothing to do right now because we aren't blocking anywhere
    }

    const Stats& get_stats() override {
        // Previously unlocked -- safe only because this was called exactly
        // once, after sigwait() had already stopped accepting new work at
        // shutdown. Now also called on demand (SIGUSR1, see nbd.cpp's
        // main()) while request threads may still be mutating `stats` under
        // `mu`, so it needs the same lock.
        std::lock_guard _{mu};
        stats.real_time_elapsed_ns = steady_ns() - sim_start;
        stats.real_time_measured_ns = steady_ns() - (warmup_end_real_ns ? warmup_end_real_ns : sim_start);
        stats.sim_time_elapsed_ns = steady_max_virt_ns - warmup_end_virt_ns;
        return stats;
    }

    /*
     * Unsafe. Must be called in a protected block. Returns the real time (ns)
     * spent inside policy hooks (not cache bookkeeping).
     */
    uint64_t add_to_cache(uint64_t worker_id, policy::FetchRange range, bool is_prefetch,
                          bool count_stats) {
        uint64_t latency = 0;
        for (std::uint64_t block_suboffset = 0; block_suboffset < range.block_length; ++block_suboffset) {
            if (cache.present(range.block_offset + block_suboffset)) continue;
            // TODO consider what happens if we
            // instead of noop-ing eviction when ctx is present,
            // we still allow eviction, just don't add ctx.page into cache.
            // this is helpful if we ever allow batch flushing to remote volume
            // (we get to evict faster later) but for simulation, it will not matter.

            policy::EvictRequest evictions;
            if(evict_policy) {
                latency += charge_policy_ns([&]{
                    evict_policy->on_evict_request(worker_id, range.block_offset + block_suboffset, evictions);
                });
            }

            for (uint64_t i = 0; i < evictions.n_pages; ++i) {
                cache.evict(evictions.pages[i]);
                latency += charge_policy_ns([&]{
                    if (evict_policy) evict_policy->on_evict(worker_id, evictions.pages[i]);
                    if (prefetch_policy) prefetch_policy->on_evict(worker_id, evictions.pages[i]);
                });
            }
            if (count_stats) stats.evictions += evictions.n_pages;
            auto evict = cache.insert(range.block_offset + block_suboffset);
            if (count_stats && evict.first) stats.evictions++;
            latency += charge_policy_ns([&]{
                if (evict.first && evict_policy) evict_policy->on_evict(worker_id, evict.second);
                if (evict.first && prefetch_policy) prefetch_policy->on_evict(worker_id, evict.second);
                if (evict_policy) evict_policy->on_admit(worker_id, range.block_offset + block_suboffset, is_prefetch);
                if (prefetch_policy) prefetch_policy->on_admit(worker_id, range.block_offset + block_suboffset, is_prefetch);
            });
        }
        return latency;
    }


    // Times `fn` (a policy hook invocation) and charges its real wall-clock
    // cost as simulated delay, added onto `sink` -- so a computationally
    // expensive policy shows up as real added latency in the simulation
    // instead of being free. Also folds the same measurement into
    // stats.policy_latency_ns (via the caller's per-request total) for
    // visibility, unless still in warmup.
    // Returns the measured ns in case the caller needs it too (e.g. to also
    // advance stats.sim_time_elapsed).
    template <typename Fn>
    inline uint64_t charge_policy_ns(Fn&& fn) {
        auto perf_start = std::chrono::steady_clock::now();
        fn();
        uint64_t ns = elapsed_ns(perf_start);
        return ns;
    }
};

}