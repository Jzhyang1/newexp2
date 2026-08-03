#include "policy_api.h"

namespace policy {

namespace {
// Context is ignored, like CMinerPolicy -- Mithril's distinguishing idea is
// the hot-page filter below, not context partitioning.
constexpr std::uint64_t kSharedKey = 0;
}  // namespace

MithrilPolicy::MithrilPolicy(Cache& cache, std::size_t window_size, std::uint32_t min_support,
                              std::uint64_t top_k, std::uint32_t hot_threshold)
    : CachePolicy(cache),
      assoc_(window_size, min_support),
      top_k_(top_k),
      hot_threshold_(hot_threshold) {}

void MithrilPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t page,
                                         PrefetchRequest& request) {
    (void)context;
    request.n_pages = 0;

    // Frequency prior to this visit: pages already accessed often enough to
    // stay resident under plain LRU aren't worth spending mining budget on
    // (as either a trigger or a window occupant), and one-off cold pages
    // won't recur often enough to be predictable. Associations are built
    // only from the "sporadic" middle band.
    std::uint32_t freq_before = freq_[page]++;
    if (freq_before >= hot_threshold_) return;

    assoc_.predict(kSharedKey, page, request.pages, request.n_pages, top_k_);
    assoc_.observe(kSharedKey, page);
}

}  // namespace policy
