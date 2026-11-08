#include "policy_api.h"

#include <list>
#include <unordered_map>

namespace policy {

// Segmented LRU for prefetching workloads: pages prefetched but not yet used wait in their own FIFO
// segment (`unused_`); a page leaves it to the main LRU list the first time it is read.  Eviction takes
// from the unused segment only when it has grown past `max_unused_pct` of the cache (a prefetch that
// has waited that long is stale), otherwise from the LRU end of the main list.  Plain LRU lets a burst
// of prefetches push out the pages that were *about* to be used or, worse, lets just-consumed pages
// push out still-unused prefetches.
struct SegmentedLRUPolicy::Impl {
    enum class Where : char { Main, Unused };
    struct Loc { Where w; std::list<std::uint64_t>::iterator it; };
    std::list<std::uint64_t> main, unused;
    std::unordered_map<std::uint64_t, Loc> loc;
};

SegmentedLRUPolicy::SegmentedLRUPolicy(Cache& cache, std::uint64_t max_unused_pct)
    : CachePolicy(cache), impl_(new Impl), max_unused_pct_(max_unused_pct) { impl_->loc.reserve(cache.capacity * 2); }
SegmentedLRUPolicy::~SegmentedLRUPolicy() = default;

void SegmentedLRUPolicy::on_access(std::uint64_t, std::uint64_t page) {
    auto it = impl_->loc.find(page);
    if (it == impl_->loc.end()) return;
    auto& l = it->second;
    if (l.w == Impl::Where::Unused) {
        impl_->unused.erase(l.it);
        impl_->main.push_front(page);
        l = {Impl::Where::Main, impl_->main.begin()};
    } else {
        impl_->main.splice(impl_->main.begin(), impl_->main, l.it);
    }
}

void SegmentedLRUPolicy::on_admit(std::uint64_t, std::uint64_t page, bool is_prefetch) {
    if (is_prefetch) {
        impl_->unused.push_front(page);
        impl_->loc[page] = {Impl::Where::Unused, impl_->unused.begin()};
    } else {
        impl_->main.push_front(page);
        impl_->loc[page] = {Impl::Where::Main, impl_->main.begin()};
    }
}

void SegmentedLRUPolicy::on_evict_request(std::uint64_t, std::uint64_t page, EvictRequest& request) {
    if (cache.present(page) || cache.size() < cache.capacity) return;
    const std::size_t cap_unused = (std::size_t)(cache.capacity * max_unused_pct_ / 100);
    if (!impl_->unused.empty() && (impl_->unused.size() > cap_unused || impl_->main.empty()))
        request.pages[request.n_pages++] = impl_->unused.back();
    else if (!impl_->main.empty())
        request.pages[request.n_pages++] = impl_->main.back();
}

void SegmentedLRUPolicy::on_evict(std::uint64_t, std::uint64_t page) {
    auto it = impl_->loc.find(page);
    if (it == impl_->loc.end()) return;
    if (it->second.w == Impl::Where::Unused) impl_->unused.erase(it->second.it);
    else impl_->main.erase(it->second.it);
    impl_->loc.erase(it);
}

}  // namespace policy
