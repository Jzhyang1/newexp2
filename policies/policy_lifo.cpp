#include "policy_api.h"

#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace policy {

LIFOPolicy::LIFOPolicy(Cache& cache): CachePolicy(cache) {
    page_to_iterator.reserve(cache.capacity);
}

void LIFOPolicy::on_admit(uint64_t context, uint64_t page) {
    (void)context;
    recency_list.push_front(page);
    page_to_iterator.emplace(page, recency_list.begin());
}

// Callback for eviction - suggests most recently added pages
void LIFOPolicy::on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request) {
    (void)context;
    (void)page;
    if (cache.size() < cache.capacity) return;  // no eviction

    // Suggest pages from the front of the recency list (most recently added)
    // we suggest 1 page for now
    auto it = recency_list.begin();
    while (it != recency_list.end() && request.n_pages < 1) {
        std::uint64_t victim_page = *it;
        request.pages[request.n_pages++] = victim_page;
        ++it;
    }
}

    // Remove page from tracking when evicted
void LIFOPolicy::on_evict(std::uint64_t context, std::uint64_t page) {
    (void)context;
    auto it = page_to_iterator.find(page);
    if (it != page_to_iterator.end()) {
        recency_list.erase(it->second);
        page_to_iterator.erase(it);
    }
}

}  // namespace policy
