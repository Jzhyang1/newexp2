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

void ReadaheadPolicy::on_prefetch_request(std::uint64_t context, std::uint32_t block_offset,
                                          std::uint32_t block_length, PrefetchRequest& request) {
    (void)context;
    // Suggest the next MAX_PREFETCH_PAGES pages after the requested range.
    request.fetch_count = 1;
    request.fetch_ranges[0] =
        FetchRange{block_offset + block_length, MAX_PREFETCH_PAGES};
};

}  // namespace policy
