#pragma once

// Shared stream-detection core for the context-aware LRU and readahead
// policies (policy_lru_cxt_aware.cpp, policy_readahead_cxt_aware.cpp).
//
// This mirrors how a hardware stride/stream prefetcher tracks streams: for
// each context (e.g. a client/VM id) it keeps a small, fixed number of
// "heads" -- each just the address most recently seen on that stream. A new
// access joins whichever head lies within `attach_window` of it (advancing
// that head to the new address); an access that isn't within range of any
// live head starts a new stream, evicting the least-recently-touched head if
// the context is already tracking the maximum number of streams.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace policy::detail {

class StreamTracker {
public:
    using StreamId = std::uint64_t;

    struct TouchResult {
        StreamId id;
        bool is_new;    // true if `page` didn't join an existing stream
        int direction;  // +1 ascending, -1 descending, 0 undetermined
    };

    explicit StreamTracker(std::uint64_t attach_window = 8, std::size_t max_streams_per_context = 8)
        : attach_window_(attach_window), max_streams_per_context_(max_streams_per_context) {}

    // Attaches `page` to a live stream for `context` if one's head is within
    // `attach_window_` of it, or starts a new stream otherwise. Always
    // returns a stable stream id for `page`.
    TouchResult touch(std::uint64_t context, std::uint64_t page) {
        auto& heads = context_streams_[context];

        for (std::size_t i = 0; i < heads.size(); ++i) {
            Head& head = heads[i];
            std::uint64_t dist = head.addr > page ? head.addr - page : page - head.addr;
            if (dist > attach_window_) continue;

            if (page != head.addr) {
                head.direction = (page > head.addr) ? 1 : -1;
            }
            head.addr = page;
            TouchResult result{head.id, false, head.direction};
            touch_recency(heads, i);
            return result;
        }

        // No attach: start a new stream. Streams-per-context is bounded the
        // way a real stride prefetcher's fixed number of tracking entries
        // is -- the least-recently-touched head is reclaimed first.
        if (heads.size() >= max_streams_per_context_) {
            heads.erase(heads.begin());
        }
        StreamId id = next_id_++;
        heads.push_back(Head{id, page, 0});
        return TouchResult{id, true, 0};
    }

private:
    struct Head {
        StreamId id;
        std::uint64_t addr;
        int direction;
    };

    // Moves heads[i] to the back (most-recently-touched), so the eviction
    // above always reclaims the least-recently-touched head first.
    static void touch_recency(std::vector<Head>& heads, std::size_t i) {
        if (i + 1 == heads.size()) return;
        Head h = heads[i];
        heads.erase(heads.begin() + static_cast<long>(i));
        heads.push_back(h);
    }

    std::uint64_t attach_window_;
    std::size_t max_streams_per_context_;
    StreamId next_id_ = 1;
    std::unordered_map<std::uint64_t, std::vector<Head>> context_streams_;
};

}  // namespace policy::detail
