#include "policy_api.h"

#include <algorithm>

namespace policy {

ContextAwareReadaheadPolicy::ContextAwareReadaheadPolicy(Cache& cache, std::uint64_t attach_window,
                                                           std::size_t max_streams_per_context,
                                                           std::uint64_t pages)
    : CachePolicy(cache), tracker_(attach_window, max_streams_per_context),
      pages_(pages > MAX_PREFETCH_PAGES ? MAX_PREFETCH_PAGES : pages) {}

void ContextAwareReadaheadPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                                                        std::uint64_t block_length, PrefetchRequest& request) {
    request.fetch_count = 0;
    if (pages_ == 0) return;

    std::uint64_t block_end = static_cast<std::uint64_t>(block_offset) + block_length - 1;
    detail::StreamTracker::TouchResult touch = tracker_.touch(context, block_end);

    // A brand-new stream has no established direction to extrapolate --
    // don't guess and prefetch on what may just be a one-off random access.
    if (touch.is_new) return;

    request.fetch_count = 1;
    if (touch.direction >= 0) {
        // Ascending (or flat, e.g. a re-read at the head): continue forward.
        request.fetch_ranges[0] =
            FetchRange{static_cast<std::uint64_t>(block_end + 1), pages_};
    } else {
        // Descending: continue backward, i.e. prefetch the range just below
        // the request's final block, clamped so it doesn't wrap past address 0.
        std::uint64_t length = static_cast<std::uint64_t>(
            std::min<std::uint64_t>(pages_, block_end));
        request.fetch_ranges[0] = FetchRange{static_cast<std::uint64_t>(block_end - length), length};
        if (length == 0) request.fetch_count = 0;
    }
}

}  // namespace policy
