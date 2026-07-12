#include "policy_api.h"

#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace policy {

ReadaheadPolicy::ReadaheadPolicy(Cache& cache) : CachePolicy(cache) {}

void ReadaheadPolicy::on_access(std::uint64_t context, std::uint64_t page) {
    (void)cache;
    (void)context;
    (void)page;
}

void ReadaheadPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t page, PrefetchRequest& request) {
    (void)context;
    request.n_pages = 0;

    // Suggest the next N pages for prefetching
    for (std::uint64_t i = 1; i <= MAX_PREFETCH_PAGES && request.n_pages < MAX_PREFETCH_PAGES; ++i) {
        std::uint64_t next_page = page + i;
        if (!cache.present(next_page)) {
            request.pages[request.n_pages++] = next_page;
        }
    }
};

}  // namespace policy
