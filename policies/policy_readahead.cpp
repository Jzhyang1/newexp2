#include "policy_api.h"

#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace policy {

ReadaheadPolicy::ReadaheadPolicy(Cache& cache) : CachePolicy(cache) {}

void ReadaheadPolicy::on_admit(std::uint64_t context, std::uint64_t page) {
    (void)cache;
    (void)context;
    (void)page;
}

void ReadaheadPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t page, PrefetchRequest& request) {
    (void)context;
    request.n_pages = MAX_PREFETCH_PAGES;

    // Suggest the next N pages for prefetching
    for (std::uint64_t i = 0; i < MAX_PREFETCH_PAGES; ++i) {
        std::uint64_t next_page = page + i + 1;
        request.pages[i] = next_page;
    }
};

}  // namespace policy
