#include "policy_api.h"

#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace policy {

LRUPolicy::LRUPolicy(Cache& cache): CachePolicy(cache) {
    page_to_iterator.reserve(cache.capacity);
}

// Callback for page access - updates recency tracking
void LRUPolicy::on_access(uint64_t context, uint64_t page) {
    (void)context;
    auto it = page_to_iterator.find(page);
    if (it == page_to_iterator.end()) {
        return; // not found
    }
    recency_list.splice(
        recency_list.begin(),
        recency_list,
        it->second
    );
}

void LRUPolicy::on_admit(uint64_t context, uint64_t page) {
    (void)context;
    recency_list.push_front(page);
    page_to_iterator.emplace(page, recency_list.begin());
}

    // Callback for eviction - suggests least recently used pages
void LRUPolicy::on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request) {
    (void)context;
    (void)page;
    if (cache.size() < cache.capacity) return;
    
    // Suggest pages from the back of the recency list (least recently used)
    // we suggest 1 page for now
    auto it = recency_list.rbegin();
    while (it != recency_list.rend() && request.n_pages < 1) {
        std::uint64_t victim_page = *it;
        request.pages[request.n_pages++] = victim_page;
        ++it;
    }
}

    // Remove page from tracking when evicted
void LRUPolicy::on_evict(std::uint64_t context, std::uint64_t page) {
    (void)context;
    auto it = page_to_iterator.find(page);
    if (it != page_to_iterator.end()) {
        recency_list.erase(it->second);
        page_to_iterator.erase(it);
    }
}

}  // namespace policy
