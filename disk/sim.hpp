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
#include "overlay.hpp"
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
    // Optional per-request host-side feature log (signals #1-12 of
    // offline/SIGNALS.md): one row per read()/non-read command, timestamps are
    // CLOCK_MONOTONIC ns (steady_ns()) so they can be joined with guest traces.
    FILE* feature_log;
    // Optional per-cache-event log (admit/evict with prefetch provenance and
    // how often the page was reused while resident).
    FILE* cache_log;
    // Modified-block overlay (guest writes); null = writes are discarded.
    Overlay* overlay;
    // Residency metadata, only maintained when feature_log/cache_log is on.
    struct PageMeta { uint64_t admit_ns; uint64_t admit_req; uint32_t hits; bool prefetched; bool used; };
    std::unordered_map<uint64_t, PageMeta> page_meta;
    uint64_t cur_req_idx{0};   // request currently being served (cache_log context; protected by mu)
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
            FILE* prefetch_log = nullptr,
            FILE* feature_log = nullptr,
            FILE* cache_log = nullptr,
            Overlay* overlay = nullptr
        ):  f(f), warmup(warmup), cache(cache),
            evict_policy(evict_policy), prefetch_policy(prefetch_policy),
            access_log(access_log), prefetch_log(prefetch_log),
            feature_log(feature_log), cache_log(cache_log), overlay(overlay) {
                sim_start = steady_ns();
                if (feature_log) std::fprintf(feature_log,
                    "req,worker,type,flags,handle,offset,length,req_length,arrive_ns,start_ns,end_ns,"
                    "pread_ns,policy_ns,think_ns,inflight,queued_bytes,hits,total,pf_hits,pf_issued\n");
                if (cache_log) std::fprintf(cache_log, "ts_ns,req,event,page,prefetched,resident_ns,hits,used\n");
            }

    bool track_meta() const { return feature_log || cache_log; }

    // Max number of *new* pages (pages not already resident) any one request may
    // prefetch, whatever the policy asks for; 0 = unlimited. Applied here, after
    // the policy has answered, so every policy is held to the same backend
    // traffic per request and comparisons between them are not decided by how
    // much data each is willing to pull in. Ranges are cut in the order the
    // policy proposed them.
    uint64_t prefetch_budget_pages{0};

    // Caller holds mu, after the demand pages have been admitted.
    void apply_prefetch_budget(policy::PrefetchRequest& req) {
        if (prefetch_budget_pages == 0) return;
        uint64_t left = prefetch_budget_pages;
        uint64_t keep = 0;
        for (uint64_t i = 0; i < req.fetch_count && left > 0; ++i) {
            policy::FetchRange r = req.fetch_ranges[i];
            uint64_t len = 0;
            for (; len < r.block_length; ++len) {
                if (!cache.present(r.block_offset + len)) {
                    if (left == 0) break;
                    --left;
                }
            }
            if (len == 0) continue;
            r.block_length = len;
            req.fetch_ranges[keep++] = r;
        }
        req.fetch_count = keep;
    }

    // Writes only change data, never cache residency: the cache tracks page
    // presence, not contents, and reads re-patch from the overlay every time.
    uint32_t write(uint64_t offset, const void* buf, uint64_t length) override {
        return overlay ? overlay->write(offset, buf, length) : 0;
    }
    uint32_t write_zeroes(uint64_t offset, uint64_t length) override {
        if (!overlay) return 0;
        static thread_local std::vector<uint8_t> zeros(1 << 20, 0);
        while (length > 0) {
            uint64_t n = std::min<uint64_t>(length, zeros.size());
            if (uint32_t err = overlay->write(offset, zeros.data(), n)) return err;
            offset += n;
            length -= n;
        }
        return 0;
    }

    void on_command(uint64_t worker_id, uint64_t offset, uint64_t length, const ReqInfo& info) override {
        std::lock_guard _{mu};
        const uint64_t now = steady_ns();

        // Think time since this worker's previous response, charged like
        // read() does so total_latencies == worker + policy + miss delay holds.
        uint64_t worker_run_ns = 0;
        auto ws = worker_real_time_start.find(worker_id);
        if (ws != worker_real_time_start.end()) {
            worker_run_ns = now - ws->second;
            worker_real_time_start.erase(ws);
        }
        auto wv = worker_virtual_time.find(worker_id);
        uint64_t virt_end = (wv != worker_virtual_time.end() ? wv->second : steady_max_virt_ns) + worker_run_ns;
        if (virt_end > steady_max_virt_ns) steady_max_virt_ns = virt_end;
        worker_virtual_time[worker_id] = virt_end;

        // Reads gate warmup by request index; other ops count once warmup is over.
        if (global_idx.load() >= warmup) {
            const uint64_t service_ns = info.arrive_ns ? now - info.arrive_ns : 0;
            OpStats* op = nullptr;
            switch (info.type) {
                case 0: stats.read_errors += info.error != 0; break;  // rejected read
                case 1: op = &stats.writes; break;
                case 3: op = &stats.flushes; break;
                case 4: op = &stats.trims; break;
                case 6: op = &stats.write_zeroes; break;
                default: op = &stats.unknown_ops; break;
            }
            if (op) {
                op->count++;
                op->service_ns += service_ns;
                op->fua += (info.flags & 1) != 0;  // NBD_CMD_FLAG_FUA = 1 << 0
                if (info.error) op->errors++; else op->bytes += length;
                if (info.type == 1 && !info.error) stats.bytes_written += length;
            }
            stats.total_latencies += worker_run_ns;
            stats.worker_latency_ns += worker_run_ns;
        }

        if (!feature_log) return;
        std::fprintf(feature_log, "-1,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,0,0,0,%u,%u,0,0,0,0\n",
            (unsigned long long)worker_id, info.type, info.flags, (unsigned long long)info.handle,
            (unsigned long long)offset, (unsigned long long)length, (unsigned long long)info.req_length,
            (unsigned long long)info.arrive_ns, (unsigned long long)now, (unsigned long long)now,
            info.inflight, info.queued_bytes);
    }

    uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf,
                  uint64_t* miss_blocks = nullptr, const ReqInfo* info = nullptr) override {
        const uint64_t start_ns = steady_ns();
        // manage warmup period
        uint64_t req_idx = global_idx.fetch_add(1);
        bool count_stats = req_idx >= warmup;

        // bytes to cache pages (page_size bytes each); unaligned requests cover every page they touch
        const std::uint64_t psz = cache.page_size;
        std::uint64_t block_offset = offset / psz;
        std::uint64_t block_length = length ? (offset + length - 1) / psz - block_offset + 1 : 0;
        uint64_t pf_hits = 0;

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
            cur_req_idx = req_idx;
            if (prefetch_policy) {
                policy_ns += charge_policy_ns([&]{
                    prefetch_policy->on_request(worker_id, offset, length, info && info->arrive_ns ? info->arrive_ns : start_ns);
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
            if (hit && track_meta()) {
                auto m = page_meta.find(block_offset + block_suboffset);
                if (m != page_meta.end()) {
                    m->second.hits++;
                    if (m->second.prefetched && !m->second.used) pf_hits++;  // first use of a prefetched page
                    m->second.used = true;
                }
            }

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
            apply_prefetch_budget(prefetch_req);
            for (uint64_t i = 0; i < prefetch_req.fetch_count; ++i) {
                if (prefetch_log) {
                    const auto& r = prefetch_req.fetch_ranges[i];
                    uint64_t cached = 0;
                    for (std::uint64_t b = 0; b < r.block_length; ++b) cached += cache.present(r.block_offset + b);
                    std::fprintf(prefetch_log, "%llu,%llu,%llu,%llu,%llu,%llu\n",
                        static_cast<unsigned long long>(worker_id),
                        static_cast<unsigned long long>(r.block_offset) * psz,
                        static_cast<unsigned long long>(r.block_length) * psz,
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
        const uint64_t pread_start_ns = steady_ns();
        ssize_t bytes_got = pread(fileno(f), buf, length, static_cast<off_t>(offset));
        if (bytes_got < 0) bytes_got = 0;
        if (overlay) overlay->patch(offset, buf, length);
        if (feature_log) {
            const uint64_t end_ns = steady_ns();
            std::lock_guard _{mu};
            const ReqInfo zero{};
            const ReqInfo& ri = info ? *info : zero;
            std::fprintf(feature_log, "%llu,%llu,%u,%u,%llu,%llu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%llu,%llu\n",
                (unsigned long long)req_idx, (unsigned long long)worker_id, ri.type, ri.flags,
                (unsigned long long)ri.handle, (unsigned long long)offset, length,
                (unsigned long long)(ri.req_length ? ri.req_length : length),
                (unsigned long long)(ri.arrive_ns ? ri.arrive_ns : start_ns), (unsigned long long)start_ns,
                (unsigned long long)end_ns, (unsigned long long)(end_ns - pread_start_ns),
                (unsigned long long)policy_ns, (unsigned long long)worker_run_ns,
                ri.inflight, ri.queued_bytes, (unsigned long long)hit_count, (unsigned long long)total_count,
                (unsigned long long)pf_hits, (unsigned long long)prefetch_req.fetch_count);
        }
        
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
        stats.overlay_bytes = overlay ? overlay->bytes() : 0;
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
                log_evict(evictions.pages[i]);
                latency += charge_policy_ns([&]{
                    if (evict_policy) evict_policy->on_evict(worker_id, evictions.pages[i]);
                    if (prefetch_policy) prefetch_policy->on_evict(worker_id, evictions.pages[i]);
                });
            }
            if (count_stats) stats.evictions += evictions.n_pages;
            auto evict = cache.insert(range.block_offset + block_suboffset);
            if (count_stats && evict.first) stats.evictions++;
            if (evict.first) log_evict(evict.second);
            log_admit(range.block_offset + block_suboffset, is_prefetch);
            latency += charge_policy_ns([&]{
                if (evict.first && evict_policy) evict_policy->on_evict(worker_id, evict.second);
                if (evict.first && prefetch_policy) prefetch_policy->on_evict(worker_id, evict.second);
                if (evict_policy) evict_policy->on_admit(worker_id, range.block_offset + block_suboffset, is_prefetch);
                if (prefetch_policy) prefetch_policy->on_admit(worker_id, range.block_offset + block_suboffset, is_prefetch);
            });
        }
        return latency;
    }


    // Cache-residency bookkeeping for cache_log (callers hold mu).
    void log_admit(uint64_t page, bool is_prefetch) {
        if (!track_meta()) return;
        uint64_t now = steady_ns();
        page_meta[page] = PageMeta{now, cur_req_idx, 0, is_prefetch, false};
        if (cache_log) std::fprintf(cache_log, "%llu,%llu,admit,%llu,%d,0,0,0\n", (unsigned long long)now,
            (unsigned long long)cur_req_idx, (unsigned long long)page, (int)is_prefetch);
    }
    void log_evict(uint64_t page) {
        if (!track_meta()) return;
        auto m = page_meta.find(page);
        if (m == page_meta.end()) return;
        uint64_t now = steady_ns();
        if (cache_log) std::fprintf(cache_log, "%llu,%llu,evict,%llu,%d,%llu,%u,%d\n", (unsigned long long)now,
            (unsigned long long)cur_req_idx, (unsigned long long)page, (int)m->second.prefetched,
            (unsigned long long)(now - m->second.admit_ns), m->second.hits, (int)m->second.used);
        page_meta.erase(m);
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