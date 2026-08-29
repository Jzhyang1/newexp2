#include "policy_api.h"

#include <list>
#include <unordered_map>
#include <utility>

namespace policy {

ContextAwareLRUPolicy::ContextAwareLRUPolicy(Cache& cache, std::uint64_t attach_window,
                                              std::size_t max_streams_per_context)
    : CachePolicy(cache), tracker_(attach_window, max_streams_per_context) {}

// Callback for page access - promotes the page's stream to MRU.
void ContextAwareLRUPolicy::on_access(std::uint64_t context, std::uint64_t page) {
    (void)context;
    auto it = page_loc_.find(page);
    if (it == page_loc_.end()) return;  // not tracked (shouldn't happen post-admit)
    streams_.splice(streams_.begin(), streams_, it->second.stream);
}

void ContextAwareLRUPolicy::on_admit(std::uint64_t context, std::uint64_t page) {
    detail::StreamTracker::TouchResult touch = tracker_.touch(context, page);

    std::list<Stream>::iterator stream;
    auto stream_it = id_to_stream_.find(touch.id);
    if (stream_it != id_to_stream_.end()) {
        stream = stream_it->second;
    } else {
        streams_.push_front(Stream{touch.id, {}});
        stream = streams_.begin();
        id_to_stream_.emplace(touch.id, stream);
    }

    stream->pages.push_back(page);
    page_loc_.emplace(page, PageLoc{stream, std::prev(stream->pages.end())});

    // Fresh activity on the stream -- promote it to MRU.
    streams_.splice(streams_.begin(), streams_, stream);
}

// Callback for eviction - suggests a page from the least recently used
// stream (the stream at the back of the recency list), oldest member first.
void ContextAwareLRUPolicy::on_evict_request(std::uint64_t context, std::uint64_t page,
                                              EvictRequest& request) {
    (void)context;
    if (cache.present(page)) return;  // hit - page already cached, no eviction needed
    if (cache.size() < cache.capacity) return;

    // we suggest 1 page for now
    for (auto sit = streams_.rbegin(); sit != streams_.rend() && request.n_pages < 1; ++sit) {
        for (auto pit = sit->pages.begin(); pit != sit->pages.end() && request.n_pages < 1; ++pit) {
            if (cache.present(*pit)) {
                request.pages[request.n_pages++] = *pit;
            }
        }
    }
}

// Remove page from tracking when evicted; drops its stream once empty.
void ContextAwareLRUPolicy::on_evict(std::uint64_t context, std::uint64_t page) {
    (void)context;
    auto it = page_loc_.find(page);
    if (it == page_loc_.end()) return;

    std::list<Stream>::iterator stream = it->second.stream;
    stream->pages.erase(it->second.page_it);
    page_loc_.erase(it);

    if (stream->pages.empty()) {
        id_to_stream_.erase(stream->id);
        streams_.erase(stream);
    }
}

}  // namespace policy
