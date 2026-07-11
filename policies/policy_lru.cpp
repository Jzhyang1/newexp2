#include "policy_api.h"

#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace policy {

class LRUPolicy : public CachePolicy {
private:
    std::list<std::uint64_t> recency_list;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> page_to_iterator;

public:
    LRUPolicy(Cache& cache): CachePolicy(cache) {
        page_to_iterator.reserve(cache.capacity);
    }

    // Callback for page access - updates recency tracking
    void on_access(uint64_t context, uint64_t page) {
        (void)context;
        auto it = page_to_iterator.find(page);
        if (it != page_to_iterator.end()) {
            // Add to front of list
            recency_list.splice(
                recency_list.begin(),
                recency_list,
                it->second
            );
            return;
        }

        recency_list.push_front(page);
        page_to_iterator.emplace(page, recency_list.begin());
    }

    // Callback for eviction - suggests least recently used pages
    void on_eviction_request(std::uint64_t context, std::uint64_t page, EvictionRequest& request) {
        (void)context;
        (void)page;
        request.n_pages = 0;
        
        // Suggest pages from the back of the recency list (least recently used)
        auto it = recency_list.rbegin();
        while (it != recency_list.rend() && request.n_pages < MAX_EVICT_PAGES) {
            std::uint64_t victim_page = *it;
            request.pages[request.n_pages++] = victim_page;
            ++it;
        }
    }

    // Remove page from tracking when evicted
    void on_eviction(std::uint64_t context, std::uint64_t page) {
        (void)context;
        auto it = page_to_iterator.find(page);
        if (it != page_to_iterator.end()) {
            recency_list.erase(it->second);
            page_to_iterator.erase(it);
        }
    }
};

}  // namespace policy
