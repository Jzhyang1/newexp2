#include "policy_api.h"

#include <algorithm>

namespace policy {

QuickMinePolicy::QuickMinePolicy(Cache& cache, std::size_t window_size, std::uint32_t min_support,
                                  std::uint64_t top_k)
    : CachePolicy(cache), assoc_(window_size, min_support), top_k_(top_k) {}

void QuickMinePolicy::on_prefetch_request(std::uint64_t context, std::uint64_t page,
                                           PrefetchRequest& request) {
    // Only difference from CMinerPolicy: partitioned by the requesting
    // client's context instead of a single shared key, so associations are
    // mined from each client's own (uninterleaved) access sequence.
    std::uint64_t predicted[MAX_PREFETCH_PAGES];
    std::uint64_t predicted_n = 0;
    std::uint64_t capacity = std::min<std::uint64_t>(top_k_, MAX_PREFETCH_PAGES);
    assoc_.predict(context, page, predicted, predicted_n, capacity);
    request.fetch_count = predicted_n;
    for (std::uint64_t i = 0; i < predicted_n; ++i) {
        request.fetch_ranges[i] = FetchRange{static_cast<std::uint32_t>(predicted[i]), 1};
    }
    assoc_.observe(context, page);
}

}  // namespace policy
