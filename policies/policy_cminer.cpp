#include "policy_api.h"

namespace policy {

namespace {
// Context is deliberately ignored, so every client shares one partition.
constexpr std::uint64_t kSharedKey = 0;
}  // namespace

CMinerPolicy::CMinerPolicy(Cache& cache, std::size_t window_size, std::uint32_t min_support,
                            std::uint64_t top_k)
    : CachePolicy(cache), assoc_(window_size, min_support), top_k_(top_k) {}

void CMinerPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t page,
                                        PrefetchRequest& request) {
    (void)context;
    // Predict from associations learned so far, before folding this access in
    // -- otherwise `page` would trivially "predict" itself.
    assoc_.predict(kSharedKey, page, request.pages, request.n_pages, top_k_);
    assoc_.observe(kSharedKey, page);
}

}  // namespace policy
