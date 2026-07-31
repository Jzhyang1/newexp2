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

#define MAX_PREFETCH_PAGES 32
#define MAX_EVICT_PAGES 128

namespace policy {

struct PrefetchRequest {
    std::uint64_t n_pages = 0;
    std::uint64_t pages[MAX_PREFETCH_PAGES];
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

}  // namespace policy
