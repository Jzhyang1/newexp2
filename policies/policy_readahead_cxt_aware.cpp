#include "policy_api.h"

#include <algorithm>

namespace policy {

ContextAwareReadaheadPolicy::ContextAwareReadaheadPolicy(Cache& cache, std::uint64_t attach_window,
                                                           std::size_t max_streams_per_context)
    : CachePolicy(cache), tracker_(attach_window, max_streams_per_context) {}

void ContextAwareReadaheadPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t page,
                                                        PrefetchRequest& request) {
    request.fetch_count = 0;

    detail::StreamTracker::TouchResult touch = tracker_.touch(context, page);

    // A brand-new stream has no established direction to extrapolate --
    // don't guess and prefetch on what may just be a one-off random access.
    if (touch.is_new) return;

    request.fetch_count = 1;
    if (touch.direction >= 0) {
        // Ascending (or flat, e.g. a re-read at the head): continue forward.
        request.fetch_ranges[0] =
            FetchRange{static_cast<std::uint32_t>(page + 1), MAX_PREFETCH_PAGES};
    } else {
        // Descending: continue backward, i.e. prefetch the range just below
        // `page`, clamped so it doesn't wrap past address 0.
        std::uint32_t length = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(MAX_PREFETCH_PAGES, page));
        request.fetch_ranges[0] = FetchRange{static_cast<std::uint32_t>(page - length), length};
        if (length == 0) request.fetch_count = 0;
    }
}

}  // namespace policy
