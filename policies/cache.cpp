#include "policy_api.h"
#include <chrono>
#include <thread>

namespace policy {

Cache::Cache(std::size_t capacity, std::uint64_t hit_latency_ns, std::uint64_t miss_latency_ns)
	: capacity(capacity), hit_latency_ns(hit_latency_ns), miss_latency_ns(miss_latency_ns) {}

bool Cache::present(std::uint64_t page) const {
	return pages_.find(page) != pages_.end();
}

bool Cache::evict(std::uint64_t page) {
	auto it = pages_.find(page);
	if (it == pages_.end()) return false;
	pages_.erase(it);
	return true;
}

bool Cache::insert(std::uint64_t page, std::uint64_t victim_page) {
	if (pages_.find(page) != pages_.end()) return true; // already present

	if (pages_.size() < capacity) {
		pages_.insert(page);
		return true;
	}

	// No space: evict the provided victim if present, then insert
	auto vit = pages_.find(victim_page);
	if (vit != pages_.end()) {
		pages_.erase(vit);
		pages_.insert(page);
	}
	// if victim not present and cache full, do nothing
    return false;
}

} // namespace policy