#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <cstdio>
#include <iostream>

#include "assoc_miner.h"
#include "stream_tracker.h"

#define MAX_PREFETCH_PAGES 512
#define MAX_EVICT_PAGES 512

namespace policy {

struct FetchRange {
    std::uint64_t block_offset;
    std::uint64_t block_length;
};

struct PrefetchRequest {
    std::uint64_t fetch_count = 0;
    FetchRange fetch_ranges[MAX_PREFETCH_PAGES];
};

struct EvictRequest {
    std::uint64_t n_pages = 0;
    std::uint64_t pages[MAX_EVICT_PAGES];
};

class CachePolicy {
public:
    class Cache& cache;
    CachePolicy(Cache& cache) : cache(cache) {}
    virtual ~CachePolicy() = default;

    virtual void on_access(uint64_t context, uint64_t page) {
        (void)context;
        (void)page;
    };
    virtual void on_admit(uint64_t context, uint64_t page, bool is_prefetch) {
        (void)context;
        (void)page;
        (void)is_prefetch;
    };
    // Called just before on_prefetch_request with the request exactly as the guest issued it: byte offset and
    // length (the page-granular block_offset/length below lose the guest's window shape, e.g. 120 KiB vs 128 KiB)
    // and a monotonic arrival time in ns (0 when the caller has none).
    virtual void on_request(uint64_t context, std::uint64_t byte_offset, std::uint64_t byte_length,
                            std::uint64_t arrive_ns) {
        (void)context;
        (void)byte_offset;
        (void)byte_length;
        (void)arrive_ns;
    }
    virtual void on_prefetch_request(uint64_t context, std::uint64_t block_offset,
                                     std::uint64_t block_length, PrefetchRequest& request) {
        (void)context;
        (void)block_offset;
        (void)block_length;
        (void)request;
    };
    virtual void on_evict_request(uint64_t context, uint64_t page, EvictRequest& request) {
        (void)context;
        (void)page;
        (void)request;
    };
    virtual void on_evict(uint64_t context, uint64_t page) {
        (void)context;
        (void)page;
    };
};

class Cache {
    // Thread unsafe cache
public:
    Cache(std::size_t capacity, std::uint64_t hit_latency_ns = 0, std::uint64_t miss_latency_ns = 0,
          std::uint64_t page_size = 512);

    // We track if a page is in the cache
    bool present(std::uint64_t page) const;
    bool evict(std::uint64_t page);
    // returns false if we are out of capacity or if there was an eviction
    std::pair<bool, std::uint64_t> insert(std::uint64_t page); // we will select a random page and evict if no space; returns (evicted, the page evicted)

    inline std::uint64_t get_page_ns(std::uint64_t page) {
        return present(page) ? hit_latency_ns : miss_latency_ns;
    }

    // some data tracking
    const std::size_t capacity;
    // Bytes per page: every page number / block_offset / block_length in the
    // policy API is in these units (512 = one sector, the historical default).
    const std::uint64_t page_size;
    std::size_t size();

    // timing info
    std::uint64_t hit_latency_ns = 0;
    std::uint64_t miss_latency_ns = 0;
private:
    // This is a concurrent cache, so we need to protect the set with a mutex
    std::unordered_set<std::uint64_t> pages_{};
};

// ================================
// concrete policy implementations
// ================================

class FIFOPolicy : public CachePolicy {
    std::list<std::uint64_t> recency_list;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> page_to_iterator;
public:
    FIFOPolicy(Cache& cache);
    void on_admit(std::uint64_t context, std::uint64_t page, bool is_prefetch);
    void on_evict(std::uint64_t context, std::uint64_t page);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
};

class LIFOPolicy : public CachePolicy {
    std::list<std::uint64_t> recency_list;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> page_to_iterator;
public:
    LIFOPolicy(Cache& cache);
    void on_admit(std::uint64_t context, std::uint64_t page, bool is_prefetch);
    void on_evict(std::uint64_t context, std::uint64_t page);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
};

class LRUPolicy : public CachePolicy {
    std::list<std::uint64_t> recency_list;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> page_to_iterator;
public:
    LRUPolicy(Cache& cache);
    void on_access(std::uint64_t context, std::uint64_t page);
    void on_admit(std::uint64_t context, std::uint64_t page, bool is_prefetch);
    void on_evict(std::uint64_t context, std::uint64_t page);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
};

class MRUPolicy : public CachePolicy {
    static constexpr std::size_t kKeepAroundPages = 4;

    std::list<std::uint64_t> keep_around_list;
    std::list<std::uint64_t> main_mru_list;
    std::list<std::uint64_t> prefetch_fifo_list;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator>
        keep_around_iterators;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator>
        main_mru_iterators;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator>
        prefetch_fifo_iterators;

    std::size_t prefetch_capacity() const;
    void move_keep_around_to_main();
    void move_prefetch_to_main(std::uint64_t page);
public:
    MRUPolicy(Cache& cache);
    void on_access(std::uint64_t context, std::uint64_t page);
    void on_admit(std::uint64_t context, std::uint64_t page, bool is_prefetch);
    void on_evict(std::uint64_t context, std::uint64_t page);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
};

class ReadaheadPolicy : public CachePolicy {
public:
    // `pages`: how many pages after each request to prefetch (<= MAX_PREFETCH_PAGES)
    explicit ReadaheadPolicy(Cache& cache, std::uint64_t pages = MAX_PREFETCH_PAGES);
    void on_admit(std::uint64_t context, std::uint64_t page, bool is_prefetch);
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request);
private:
    std::uint64_t pages_;
};

// Groups reads into per-context "streams" the way a hardware stride/stream
// prefetcher does (see stream_tracker.h) and runs LRU over streams rather
// than individual pages: a stream is promoted to MRU as a whole whenever any
// of its pages is touched, and its member pages are evicted together,
// oldest-first, once it becomes the coldest stream.
class ContextAwareLRUPolicy : public CachePolicy {
    detail::StreamTracker tracker_;

    struct Stream {
        detail::StreamTracker::StreamId id;
        std::list<std::uint64_t> pages;  // member pages, oldest first
    };
    std::list<Stream> streams_;  // recency order: front = MRU stream, back = LRU stream
    std::unordered_map<detail::StreamTracker::StreamId, std::list<Stream>::iterator> id_to_stream_;

    struct PageLoc {
        std::list<Stream>::iterator stream;
        std::list<std::uint64_t>::iterator page_it;
    };
    std::unordered_map<std::uint64_t, PageLoc> page_loc_;

public:
    explicit ContextAwareLRUPolicy(Cache& cache, std::uint64_t attach_window = 8,
                                    std::size_t max_streams_per_context = 8);
    void on_access(std::uint64_t context, std::uint64_t page);
    void on_admit(std::uint64_t context, std::uint64_t page, bool is_prefetch);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
    void on_evict(std::uint64_t context, std::uint64_t page);
};

// Prefetches ahead of a detected stream's head, extrapolating in the
// stream's detected direction (see stream_tracker.h). An access that starts
// a brand-new stream -- i.e. doesn't land near any head we're already
// tracking for that context -- triggers no prefetch, since there's no
// direction yet to extrapolate from.
class ContextAwareReadaheadPolicy : public CachePolicy {
    detail::StreamTracker tracker_;
    std::uint64_t pages_;
public:
    // `pages`: how far ahead of a detected stream to prefetch, in cache pages
    // (<= MAX_PREFETCH_PAGES). It used to be fixed at MAX_PREFETCH_PAGES, which made
    // this policy 16x+ more aggressive than ReadaheadPolicy at any smaller setting.
    explicit ContextAwareReadaheadPolicy(Cache& cache, std::uint64_t attach_window = 8,
                                          std::size_t max_streams_per_context = 8,
                                          std::uint64_t pages = MAX_PREFETCH_PAGES);
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request);
};

// Replays the next request from an nbd_access.csv-shaped trace. The trace is
// partitioned by worker_id so concurrent clients retain their own request order.
class OraclePolicy : public CachePolicy {
    struct Request {
        std::uint64_t block_offset;
        std::uint64_t block_length;
    };

    std::unordered_map<std::uint64_t, std::vector<Request>> requests_;
    std::unordered_map<std::uint64_t, std::size_t> next_request_;

public:
    OraclePolicy(Cache& cache, const std::string& path);
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request);
};

// Queries an offline Python predictor over its JSON-lines stdin/stdout API.
class ModelPolicy : public CachePolicy {
    struct ModelProcess;
    std::unique_ptr<ModelProcess> model_;
    std::size_t min_len_;
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::deque<std::pair<std::uint64_t, std::uint64_t>>> history_;
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint64_t, std::uint64_t>>> pending_;
public:
    ModelPolicy(Cache& cache, const std::string& module, const std::string& type,
                std::size_t min_len, const std::string& training);
    ~ModelPolicy() override;
    void on_access(std::uint64_t context, std::uint64_t page);
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request);
};

// Baseline: mines page-follows-page associations from a single trailing
// window shared by every client, i.e. context is ignored entirely. See
// assoc_miner.h for how this relates to QuickMinePolicy and MithrilPolicy.
class CMinerPolicy : public CachePolicy {
    detail::WindowedAssociationTable assoc_;
    std::uint64_t top_k_;
public:
    explicit CMinerPolicy(Cache& cache, std::size_t window_size = 8,
                           std::uint32_t min_support = 2, std::uint64_t top_k = 4);
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request);
};

// Same windowed-association mining as CMinerPolicy, but partitioned per
// app-supplied context (here, the requesting client's id) so that unrelated
// clients' interleaved accesses never pollute each other's associations.
class QuickMinePolicy : public CachePolicy {
    detail::WindowedAssociationTable assoc_;
    std::uint64_t top_k_;
public:
    explicit QuickMinePolicy(Cache& cache, std::size_t window_size = 8,
                              std::uint32_t min_support = 2, std::uint64_t top_k = 4);
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request);
};

// Same global (context-free) windowed-association mining as CMinerPolicy,
// but skips pages that have already gone "hot" -- both as prefetch triggers
// and as window occupants -- since LRU already keeps those resident. Mining
// budget is reserved for sporadically-recurring ("sporadic") pages instead.
class MithrilPolicy : public CachePolicy {
    detail::WindowedAssociationTable assoc_;
    std::unordered_map<std::uint64_t, std::uint32_t> freq_;
    std::uint64_t top_k_;
    std::uint32_t hot_threshold_;
public:
    explicit MithrilPolicy(Cache& cache, std::size_t window_size = 8,
                            std::uint32_t min_support = 2, std::uint64_t top_k = 4,
                            std::uint32_t hot_threshold = 20);
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request);
};

// ---------------------------------------------------------------------------------------------------------------
// Learned prefetching for VM disk traffic.  The guest's own page cache and readahead already turned the
// application's accesses into windows (128 KiB read-around, 256 KiB / 1 MiB reads, ...), so what the host sees
// is a stream of those windows, in two shapes: many short sequential streams that advance a burst at a time
// (NextRequestPolicy), and windows that land around a point and fill in around it in no fixed order
// (NeighborhoodPrefetchPolicy).  `learned` (policy_factory.h) runs both.  Both learn online from the stream
// itself -- nothing is trained offline -- and both are conservative on random traffic (they learn that nothing
// near a request tends to follow it and stop prefetching).
// ---------------------------------------------------------------------------------------------------------------

// Learned neighborhood prefetcher (policy_neighborhood.cpp).  For every request it remembers which pages within
// `dmax` pages of it are requested by one of the next `window` requests, and accumulates that per request context
// (request length, length of the sequential chain it extends, whether it joins an active neighborhood), per
// distance bin and per hole class (is the candidate page enclosed by two recent requests -- a gap that a
// read-around scan will fill?) and, optionally, per what was seen around the same start page last time.  A request
// then prefetches the pages whose learned probability of being used is >= theta.  No fixed readahead depth or
// direction: sequential scans, out-of-order windows and isolated random reads each get their own learned shape.
class NeighborhoodPrefetchPolicy : public CachePolicy {
public:
    struct Params {
        std::size_t window = 32;          // requests an anchor stays open for training
        std::uint64_t theta_pct = 40;     // prefetch pages with P(used) >= theta
        std::uint64_t gap = 1;            // pages of slack for "continues the previous request"
        std::uint64_t prior_trials = 8;   // pseudo-trials behind the cold-start prior
        std::uint64_t prior_pct = 60;     // cold-start P for the first prior_fwd pages ahead
        std::uint64_t prior_fwd = 2;
        std::uint64_t use_hole = 1;       // condition on whether the candidate lies in a gap between two recent requests
        std::uint64_t hole_small = 4;     // gap (pages) up to which a hole is class 1
        std::uint64_t hole_max = 16;      // largest gap still treated as a hole (class 2)
        std::uint64_t use_mem = 1;        // condition on the footprint remembered for this request's start page
        std::uint64_t mem_entries = 262144;
        std::uint64_t dmax = 64;          // how far (pages) either side of a request to consider, <= 256
    };
    NeighborhoodPrefetchPolicy(Cache& cache, const Params& params);
    ~NeighborhoodPrefetchPolicy() override;
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Per-stream next-request predictor (policy_nextreq.cpp).  A stream is a run of requests each starting where the
// previous one ended (or a little after).  Per stream it keeps the last two request lengths (4 KiB resolution, from
// on_request), learns globally what length tends to follow a (previous, before-previous) pair -- 1 MiB then 256 KiB
// then 1 MiB ... --, how far past the previous end the next request starts, and how often a stream in a given
// state is continued at all, and prefetches the pages that the next few predicted requests would touch when that
// is likely enough.  A read is a full hit only if the *whole* request is cached, so the unit of prediction is the
// request, not the page.
class NextRequestPolicy : public CachePolicy {
public:
    struct Params {
        std::size_t streams = 2048;
        std::uint64_t gap_kb = 128;        // how far before a request's start a stream may end and still be continued by it
        std::uint64_t overlap_kb = 0;      // ... and how far after (re-reads / overlap)
        std::uint64_t theta_pct = 10;      // prefetch pages whose probability (P(continued) x P(length)) is >= theta
        float decay = 0.995f;              // forgetting of the next-length histograms
        float prior_cont = 0.4f;           // P(continued) prior, one pseudo-trial
        std::uint64_t max_pages = 64;
        std::uint32_t look = 3;            // requests ahead to predict
        std::uint64_t use_consumed = 1;    // P(continued) by bytes consumed so far (file-size prior) instead of request count
        float min_evidence = 4.f;          // observations needed before a length-distribution level is trusted
    };
    NextRequestPolicy(Cache& cache, const Params& params);
    ~NextRequestPolicy() override;
    void on_request(std::uint64_t context, std::uint64_t byte_offset, std::uint64_t byte_length, std::uint64_t arrive_ns) override;
    void on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                             std::uint64_t block_length, PrefetchRequest& request) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Runs several prefetch policies on the same stream and prefetches the union of what they ask for
// ("a+b" in a policy spec, see policy_factory.h).
class ComboPolicy : public CachePolicy {
public:
    ComboPolicy(Cache& cache, std::vector<std::unique_ptr<CachePolicy>> parts) : CachePolicy(cache), parts_(std::move(parts)) {}
    void on_access(std::uint64_t c, std::uint64_t p) override { for (auto& x : parts_) x->on_access(c, p); }
    void on_admit(std::uint64_t c, std::uint64_t p, bool pf) override { for (auto& x : parts_) x->on_admit(c, p, pf); }
    void on_evict(std::uint64_t c, std::uint64_t p) override { for (auto& x : parts_) x->on_evict(c, p); }
    void on_request(std::uint64_t c, std::uint64_t off, std::uint64_t len, std::uint64_t t) override {
        for (auto& x : parts_) x->on_request(c, off, len, t);
    }
    void on_prefetch_request(std::uint64_t c, std::uint64_t b0, std::uint64_t bn, PrefetchRequest& out) override {
        out.fetch_count = 0;
        for (auto& x : parts_) {
            PrefetchRequest r;
            x->on_prefetch_request(c, b0, bn, r);
            for (std::uint64_t i = 0; i < r.fetch_count && out.fetch_count < MAX_PREFETCH_PAGES; ++i) out.fetch_ranges[out.fetch_count++] = r.fetch_ranges[i];
        }
    }
private:
    std::vector<std::unique_ptr<CachePolicy>> parts_;
};

// LRU with a protected FIFO segment for prefetched-but-unused pages (policy_lru_seg.cpp).  Unused prefetches wait in
// their own segment and move to the main LRU list when first read; they are evicted first only once the segment holds
// more than `max_unused_pct` of the cache.  Plain LRU lets a burst of prefetches push out pages that were about to be
// used, and lets just-consumed pages push out still-unused prefetches.
class SegmentedLRUPolicy : public CachePolicy {
public:
    SegmentedLRUPolicy(Cache& cache, std::uint64_t max_unused_pct = 75);
    ~SegmentedLRUPolicy() override;
    void on_access(std::uint64_t context, std::uint64_t page) override;
    void on_admit(std::uint64_t context, std::uint64_t page, bool is_prefetch) override;
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request) override;
    void on_evict(std::uint64_t context, std::uint64_t page) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t max_unused_pct_;
};

}  // namespace policy
