#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <shared_mutex>

#define MAX_PREFETCH_PAGES 32
#define MAX_EVICT_PAGES 128

namespace policy {

struct PrefetchRequest {
    std::uint64_t n_pages = 0;
    std::uint64_t pages[MAX_PREFETCH_PAGES];
};

struct EvictionRequest {
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
    virtual void on_prefetch_request(uint64_t context, uint64_t page, PrefetchRequest& request) {
        (void)context;
        (void)page;
        (void)request;
    };
    virtual void on_eviction_request(uint64_t context, uint64_t page, EvictionRequest& request) {
        (void)context;
        (void)page;
        (void)request;
    };
    virtual void on_eviction(uint64_t context, uint64_t page) {
        (void)context;
        (void)page;
    };
};

class Cache {
    // Thread unsafe cache
public:
    Cache(std::size_t capacity, std::uint64_t hit_latency_ns = 0, std::uint64_t miss_latency_ns = 0);

    // We track if a page is in the cache
    bool present(std::uint64_t page);
    bool evict(std::uint64_t page);
    // returns false if we are out of capacity or if there was an eviction
    bool insert(std::uint64_t page, std::uint64_t victim_page); // victim is evicted only if no space

    // some data tracking
    const std::size_t capacity;
    // timing info
    std::uint64_t hit_latency_ns = 0;
    std::uint64_t miss_latency_ns = 0;
private:
    // This is a concurrent cache, so we need to protect the set with a mutex
    std::unordered_set<std::uint64_t> pages_{};
};

}  // namespace policy
