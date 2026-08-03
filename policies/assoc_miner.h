#pragma once

// Shared core for the C-Miner / QuickMine / Mithril prefetch policies.
//
// All three papers boil down to the same mechanism: within a trailing window
// of recently seen pages, remember which pages tend to be followed by which
// other pages, then prefetch the strongest followers of the page just seen.
// What differs between the papers is what "recently seen" is scoped to:
//   - C-Miner:   one window/table shared by every client (context ignored).
//   - QuickMine: one window/table per app-supplied context (e.g. client id),
//                so unrelated clients' interleaved accesses never pollute
//                each other's associations.
//   - Mithril:   one global window/table like C-Miner, but pages that have
//                already gone "hot" are skipped (both as trigger and as
//                window occupants) since LRU already keeps those resident;
//                mining budget is reserved for sporadically-recurring pages.
// That distinction is expressed by which `key` callers pass to observe()/
// predict(), not by different mining logic, so it lives in one place.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace policy::detail {

class WindowedAssociationTable {
public:
    // `max_tracked` bounds how many distinct followers are remembered per
    // (key, page) pair. Without a bound, a page that recurs often under a
    // skewed workload (e.g. a hot Zipfian key) accumulates an ever-growing
    // followers list, and re-ranking it on every single prediction call
    // turns into quadratic-total-time -- this hit exactly that wall in
    // testing at 20k+ requests/client before the cap was added. Tracking
    // only the top `max_tracked` followers seen so far keeps observe()/
    // predict() O(max_tracked) regardless of how long the trace runs, at
    // the cost of being approximate for rarely-seen followers.
    WindowedAssociationTable(std::size_t window_size, std::uint32_t min_support,
                              std::size_t max_tracked = 16)
        : window_size_(window_size), min_support_(min_support), max_tracked_(max_tracked) {}

    // Records that `page` was just observed under `key`: correlates it with
    // everything still in key's trailing window, then slides that window.
    void observe(std::uint64_t key, std::uint64_t page) {
        KeyState& state = states_[key];
        for (std::uint64_t prior : state.window) {
            bump(state.followers[prior], page);
        }
        state.window.push_back(page);
        if (state.window.size() > window_size_) {
            state.window.pop_front();
        }
    }

    // Fills `out_pages` (capacity `out_capacity`) with the pages most
    // strongly associated with `page` under `key`, ranked by descending
    // count and filtered to those meeting min_support. Sets `out_n` to the
    // number written.
    void predict(std::uint64_t key, std::uint64_t page, std::uint64_t* out_pages,
                 std::uint64_t& out_n, std::uint64_t out_capacity) const {
        out_n = 0;

        auto key_it = states_.find(key);
        if (key_it == states_.end()) return;
        auto page_it = key_it->second.followers.find(page);
        if (page_it == key_it->second.followers.end()) return;

        // Bounded by max_tracked_, so a full sort here stays cheap.
        std::vector<Candidate> candidates = page_it->second;
        std::sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) { return a.count > b.count; });

        for (const Candidate& c : candidates) {
            if (c.count < min_support_) break;
            if (out_n >= out_capacity) break;
            out_pages[out_n++] = c.page;
        }
    }

private:
    struct Candidate {
        std::uint64_t page;
        std::uint32_t count;
    };

    // Increments `page`'s count if it's already tracked; otherwise adds it
    // while there's room, and otherwise only displaces the weakest tracked
    // candidate if that candidate is itself just a one-off (count <= 1) --
    // a repeatedly-confirmed association never gets evicted by a newcomer.
    void bump(std::vector<Candidate>& followers, std::uint64_t page) {
        for (Candidate& c : followers) {
            if (c.page == page) {
                ++c.count;
                return;
            }
        }
        if (followers.size() < max_tracked_) {
            followers.push_back({page, 1});
            return;
        }
        auto weakest = std::min_element(
            followers.begin(), followers.end(),
            [](const Candidate& a, const Candidate& b) { return a.count < b.count; });
        if (weakest->count <= 1) {
            *weakest = {page, 1};
        }
    }

    struct KeyState {
        std::deque<std::uint64_t> window;
        std::unordered_map<std::uint64_t, std::vector<Candidate>> followers;
    };

    std::size_t window_size_;
    std::uint32_t min_support_;
    std::size_t max_tracked_;
    std::unordered_map<std::uint64_t, KeyState> states_;
};

}  // namespace policy::detail
