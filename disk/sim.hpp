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

auto no_first_time = std::chrono::steady_clock::time_point::max();
auto no_last_time = std::chrono::steady_clock::time_point::min();


struct AccessTime {
    // these are adjusted wall clock times
    uint64_t start, end;
};


class SimReadClass : public BlockReadClass {
    // handles the priority queue blocking
    // + time measurements
    FILE* f;

    Stats stats;
    uint64_t warmup;
    policy::Cache& cache;
    policy::CachePolicy* evict_policy;
    policy::CachePolicy* prefetch_policy;
    uint64_t sim_start;
    std::atomic<uint64_t> global_idx;

    volatile uint64_t steady_max_virt_ns;   // equivalent to total virt time elapsed
    std::unordered_map<uint64_t, uint64_t> worker_virtual_time;
    std::unordered_map<uint64_t, uint64_t> worker_real_time_start;
    std::vector<AccessTime> disk_access_times;   // tracks all prior disk accesses to coalesce
    std::shared_mutex mu;   // Protects critical sections

public:
    SimReadClass(
            FILE* f,
            uint64_t vm_count,
            uint64_t warmup,
            policy::Cache& cache,
            policy::CachePolicy* evict_policy, 
            policy::CachePolicy* prefetch_policy
        ):  f(f), warmup(warmup), cache(cache),
            evict_policy(evict_policy), prefetch_policy(prefetch_policy) {
                sim_start = steady_ns();
            }

    uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf) override {
        // manage warmup period
        bool count_stats = global_idx.fetch_add(1) >= warmup;

        // bytes to disk units
        if (offset % SECTOR_SIZE) {
            std::cerr << "got invalid (offset=" << offset << ")\n";
        }
        uint32_t block_offset = offset / SECTOR_SIZE;

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

            // Real (wall-clock) time this app spent between receiving its
            // previous response and issuing this request -- i.e. the VM's
            // own think/compute time.
            auto worker_start = worker_real_time_start.find(worker_id);
            if (worker_start != worker_real_time_start.end()) {
                worker_run_ns += now_ns - worker_start->second;
                worker_real_time_start.erase(worker_start);
            }
        }

        // manage cache
        uint64_t total_count = 0;
        uint64_t hit_count = 0;
        uint64_t prefetch_ns = 0;  // Time skew from prefetching work
        uint64_t evict_ns = 0;  // Time skew from eviction work
        for (int block_suboffset = 0; block_suboffset + SECTOR_SIZE <= length; block_suboffset += SECTOR_SIZE) {
            std::lock_guard _{mu};
            bool hit = cache.present(block_offset + block_suboffset);
            total_count += 1;
            hit_count += hit;

            // decide on who to prefetch
            // (must run under `mu`: the prefetch policy keeps state)
            policy::PrefetchRequest prefetch_req;
            if (prefetch_policy) {
                prefetch_ns += charge_policy_ns([&]{
                    prefetch_policy->on_prefetch_request(worker_id, block_offset, prefetch_req);
                });
            }

            // admit the requested pages
            for (uint64_t i = 0; i < prefetch_req.fetch_count; ++i) {
                add_to_cache(worker_id, prefetch_req.fetch_ranges[i]);
            }
            add_to_cache(worker_id, policy::FetchRange{block_offset + block_suboffset, 1});

            // acknowledge access
            if (evict_policy) {
                evict_ns += charge_policy_ns([&]{
                    evict_policy->on_access(worker_id, block_offset);
                });
            }
            if (prefetch_policy) {
                evict_ns += charge_policy_ns([&]{
                    prefetch_policy->on_access(worker_id, block_offset);
                });
            }

            // TODO find the earliest worker
            // block if the worker_run_ns is more than MAX_SKEW_NS greater than
            // the earliest worker
        }
        
        uint64_t total_elapsed_ns;
        {
            std::lock_guard _{mu};
            // Make sure all read requests are charged
            total_elapsed_ns = worker_run_ns + prefetch_ns + evict_ns;
            worker_virt_end_ns = worker_virt_start_ns + total_elapsed_ns;
            if (worker_virt_end_ns > steady_max_virt_ns) steady_max_virt_ns = worker_virt_end_ns;
            worker_virtual_time[worker_id] = worker_virt_end_ns;
            disk_access_times.emplace_back(worker_virt_start_ns + worker_run_ns, worker_virt_end_ns);
        }

        // perform the actual response
        size_t bytes_got = fread(buf, length, 1, f);
        uint64_t total_blocked_time = 0;
        
        // final metric - tracking
        if (count_stats) {
            std::lock_guard _{mu};
            worker_real_time_start[worker_id] = steady_ns();

            stats.hits += hit_count;
            stats.misses += total_count - hit_count;
            stats.total_latencies += total_elapsed_ns;
            stats.worker_latency_ns += worker_run_ns;
            stats.policy_latency_ns += evict_ns;
        }
    }

    const Stats& get_stats() override {
        stats.real_time_elapsed_ns = steady_ns() - sim_start;
        return stats;
    }

    /*
     * Unsafe. Must be called in a protected block. Returns the latency
     */
    uint64_t add_to_cache(uint64_t worker_id, policy::FetchRange range) {
        for (uint32_t block_suboffset = 0; block_suboffset < range.block_length; ++block_suboffset) {
            if (cache.present(range.block_offset + block_suboffset)) continue;
            // TODO consider what happens if we
            // instead of noop-ing eviction when ctx is present,
            // we still allow eviction, just don't add ctx.page into cache.
            // this is helpful if we ever allow batch flushing to remote volume
            // (we get to evict faster later) but for simulation, it will not matter.
            
            uint64_t latency = 0;
            policy::EvictRequest evictions;
            if(evict_policy) {
                latency += charge_policy_ns([&]{
                    evict_policy->on_evict_request(worker_id, range.block_offset + block_suboffset, evictions);
                });
            }

            for (uint64_t i = 0; i < evictions.n_pages; ++i) {
                cache.evict(evictions.pages[i]);
            }
            cache.insert(range.block_offset + block_suboffset);
        }
    }


    // Times `fn` (a policy hook invocation) and charges its real wall-clock
    // cost as simulated delay, added onto `sink` -- so a computationally
    // expensive policy shows up as real added latency in the simulation
    // instead of being free. Also folds the same measurement into
    // stats.policy_latency_ns for visibility, unless still in warmup.
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