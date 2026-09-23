#include "policy_api.h"

#include <algorithm>

namespace policy {

QuickMinePolicy::QuickMinePolicy(Cache& cache, std::size_t window_size, std::uint32_t min_support,
                                  std::uint64_t top_k)
    : CachePolicy(cache), assoc_(window_size, min_support), top_k_(top_k) {}

void QuickMinePolicy::on_prefetch_request(std::uint64_t context, std::uint32_t block_offset,
                                           std::uint32_t block_length, PrefetchRequest& request) {
    // Only difference from CMinerPolicy: partitioned by the requesting
    // client's context instead of a single shared key, so associations are
    // mined from each client's own (uninterleaved) access sequence.
    request.fetch_count = 0;
    for (std::uint32_t block_suboffset = 0; block_suboffset < block_length; ++block_suboffset) {
        std::uint64_t page = static_cast<std::uint64_t>(block_offset) + block_suboffset;
        std::uint64_t predicted[MAX_PREFETCH_PAGES];
        std::uint64_t predicted_n = 0;
        std::uint64_t capacity = std::min<std::uint64_t>(
            top_k_, MAX_PREFETCH_PAGES - request.fetch_count);
        assoc_.predict(context, page, predicted, predicted_n, capacity);
        for (std::uint64_t i = 0; i < predicted_n; ++i) {
            request.fetch_ranges[request.fetch_count++] =
                FetchRange{static_cast<std::uint32_t>(predicted[i]), 1};
        }
        assoc_.observe(context, page);
    }
}

}  // namespace policy
