#include "policy_api.h"

#include <algorithm>

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

void MithrilPolicy::on_prefetch_request(std::uint64_t context, std::uint32_t block_offset,
                                         std::uint32_t block_length, PrefetchRequest& request) {
    (void)context;
    request.fetch_count = 0;

    for (std::uint32_t block_suboffset = 0; block_suboffset < block_length; ++block_suboffset) {
        std::uint64_t page = static_cast<std::uint64_t>(block_offset) + block_suboffset;
        // Frequency prior to this visit: pages already accessed often enough
        // to stay resident under plain LRU aren't worth spending mining
        // budget on. Associations are built only from the sporadic middle band.
        std::uint32_t freq_before = freq_[page]++;
        if (freq_before >= hot_threshold_) continue;

        std::uint64_t predicted[MAX_PREFETCH_PAGES];
        std::uint64_t predicted_n = 0;
        std::uint64_t capacity = std::min<std::uint64_t>(
            top_k_, MAX_PREFETCH_PAGES - request.fetch_count);
        assoc_.predict(kSharedKey, page, predicted, predicted_n, capacity);
        for (std::uint64_t i = 0; i < predicted_n; ++i) {
            request.fetch_ranges[request.fetch_count++] =
                FetchRange{static_cast<std::uint32_t>(predicted[i]), 1};
        }
        assoc_.observe(kSharedKey, page);
    }
}

}  // namespace policy
