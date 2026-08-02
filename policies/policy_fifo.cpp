#include "policy_api.h"

#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace policy {

FIFOPolicy::FIFOPolicy(Cache& cache): CachePolicy(cache) {
    page_to_iterator.reserve(cache.capacity);
}

void FIFOPolicy::on_admit(uint64_t context, uint64_t page) {
    (void)context;
    recency_list.push_front(page);
    page_to_iterator.emplace(page, recency_list.begin());
}

// Callback for eviction - suggests least recently added pages
void FIFOPolicy::on_evict_request(std::uint64_t context, std::uint64_t page, EvictRequest& request) {
    (void)context;
    if (cache.present(page)) return; // hit - page already cached, no eviction needed
    if (cache.size() < cache.capacity) return;  // no eviction

    // Suggest pages from the back of the recency list (least recently added)
    // we suggest 1 page for now
    auto it = recency_list.rbegin();
    while (it != recency_list.rend() && request.n_pages < 1) {
        std::uint64_t victim_page = *it;
        request.pages[request.n_pages++] = victim_page;
        ++it;
    }
}

    // Remove page from tracking when evicted
void FIFOPolicy::on_evict(std::uint64_t context, std::uint64_t page) {
    (void)context;
    auto it = page_to_iterator.find(page);
    if (it != page_to_iterator.end()) {
        recency_list.erase(it->second);
        page_to_iterator.erase(it);
    }
}

}  // namespace policy
