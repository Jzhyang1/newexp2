#include "policy_api.h"

namespace policy {

QuickMinePolicy::QuickMinePolicy(Cache& cache, std::size_t window_size, std::uint32_t min_support,
                                  std::uint64_t top_k)
    : CachePolicy(cache), assoc_(window_size, min_support), top_k_(top_k) {}

void QuickMinePolicy::on_prefetch_request(std::uint64_t context, std::uint64_t page,
                                           PrefetchRequest& request) {
    // Only difference from CMinerPolicy: partitioned by the requesting
    // client's context instead of a single shared key, so associations are
    // mined from each client's own (uninterleaved) access sequence.
    assoc_.predict(context, page, request.pages, request.n_pages, top_k_);
    assoc_.observe(context, page);
}

}  // namespace policy
