#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <cstdio>
#include <iostream>

#include "assoc_miner.h"
#include "stream_tracker.h"

#define MAX_PREFETCH_PAGES 32
#define MAX_EVICT_PAGES 128

namespace policy {

struct FetchRange {
    std::uint32_t block_offset;
    std::uint32_t block_length;
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
    virtual void on_admit(uint64_t context, uint64_t page) {
        (void)context;
        (void)page;
    };
    virtual void on_prefetch_request(uint64_t context, uint64_t page, PrefetchRequest& request) {
        (void)context;
        (void)page;
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
    Cache(std::size_t capacity, std::uint64_t hit_latency_ns = 0, std::uint64_t miss_latency_ns = 0);

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
    void on_admit(std::uint64_t context, std::uint64_t page);
    void on_evict(std::uint64_t context, std::uint64_t page);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
};

class LIFOPolicy : public CachePolicy {
    std::list<std::uint64_t> recency_list;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> page_to_iterator;
public:
    LIFOPolicy(Cache& cache);
    void on_admit(std::uint64_t context, std::uint64_t page);
    void on_evict(std::uint64_t context, std::uint64_t page);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
};

class LRUPolicy : public CachePolicy {
    std::list<std::uint64_t> recency_list;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> page_to_iterator;
public:
    LRUPolicy(Cache& cache);
    void on_access(std::uint64_t context, std::uint64_t page);
    void on_admit(std::uint64_t context, std::uint64_t page);
    void on_evict(std::uint64_t context, std::uint64_t page);
    void on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request);
};

class ReadaheadPolicy : public CachePolicy {
public:
    ReadaheadPolicy(Cache& cache);
    void on_admit(std::uint64_t context, std::uint64_t page);
    void on_prefetch_request(std::uint64_t context, std::uint64_t page, PrefetchRequest& request);
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
    void on_admit(std::uint64_t context, std::uint64_t page);
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
public:
    explicit ContextAwareReadaheadPolicy(Cache& cache, std::uint64_t attach_window = 8,
                                          std::size_t max_streams_per_context = 8);
    void on_prefetch_request(std::uint64_t context, std::uint64_t page, PrefetchRequest& request);
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
    void on_prefetch_request(std::uint64_t context, std::uint64_t page, PrefetchRequest& request);
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
    void on_prefetch_request(std::uint64_t context, std::uint64_t page, PrefetchRequest& request);
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
    void on_prefetch_request(std::uint64_t context, std::uint64_t page, PrefetchRequest& request);
};

}  // namespace policy
