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
    // Suggest the next MAX_PREFETCH_PAGES pages for prefetching, as one
    // contiguous range starting just past `page`.
    request.fetch_count = 1;
    request.fetch_ranges[0] =
        FetchRange{static_cast<std::uint32_t>(page + 1), MAX_PREFETCH_PAGES};
};

}  // namespace policy
