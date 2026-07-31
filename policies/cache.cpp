#include "policy_api.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <iostream>

namespace policy {

Cache::Cache(std::size_t capacity, std::uint64_t hit_latency_ns, std::uint64_t miss_latency_ns)
	: capacity(capacity), hit_latency_ns(hit_latency_ns), miss_latency_ns(miss_latency_ns) {}

bool Cache::present(std::uint64_t page) const {
	return pages_.find(page) != pages_.end();
}

std::size_t Cache::size() {
	return pages_.size();
}

bool Cache::evict(std::uint64_t page) {
	auto it = pages_.find(page);
	if (it == pages_.end()) return false;
	pages_.erase(it);
	return true;
}

std::pair<bool, std::uint64_t> Cache::insert(std::uint64_t page) {
	// returns false if eviction happened
	if (pages_.find(page) != pages_.end()) return {true, 0}; // already present

	if (pages_.size() < capacity) {
		pages_.insert(page);
		return {true, 0};
	}

	// No space: evict the provided victim if present, then insert
	auto iter = pages_.begin();
	std::uint64_t evicted = *iter;
	pages_.erase(iter);
	pages_.insert(page);
    return {false, evicted};
}

} // namespace policy