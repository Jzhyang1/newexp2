#include "policy_api.h"

#include <algorithm>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace policy {

class ContextAwareLRUPolicy : CachePolicy {
// private:
// 	struct ContextState {
// 		std::list<std::uint64_t> recency_list;
// 		std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> page_to_iterator;
// 	};

// 	std::unordered_map<std::uint64_t, ContextState> context_state_;

// 	ContextState& state_for(std::uint64_t context) {
// 		return context_state_[context];
// 	}

// public:
// 	void on_admit(Cache&, std::uint64_t context, std::uint64_t page) {
// 		auto& state = state_for(context);
// 		auto it = state.page_to_iterator.find(page);
// 		if (it != state.page_to_iterator.end()) {
// 			state.recency_list.erase(it->second);
// 		}
// 		state.recency_list.push_front(page);
// 		state.page_to_iterator[page] = state.recency_list.begin();
// 	}

// 	void on_evict_request(Cache& cache, std::uint64_t context, std::uint64_t, EvictRequest& request) {
// 		request.n_pages = 0;

// 		auto state_it = context_state_.find(context);
// 		if (state_it == context_state_.end()) {
// 			return;
// 		}

// 		auto& state = state_it->second;
// 		for (auto it = state.recency_list.rbegin(); it != state.recency_list.rend() && request.n_pages < MAX_EVICT_PAGES; ++it) {
// 			std::uint64_t victim_page = *it;
// 			if (cache.present(victim_page)) {
// 				request.pages[request.n_pages++] = victim_page;
// 			}
// 		}
// 	}

// 	void remove_page(std::uint64_t context, std::uint64_t page) {
// 		auto state_it = context_state_.find(context);
// 		if (state_it == context_state_.end()) {
// 			return;
// 		}
// 		auto& state = state_it->second;
// 		auto it = state.page_to_iterator.find(page);
// 		if (it != state.page_to_iterator.end()) {
// 			state.recency_list.erase(it->second);
// 			state.page_to_iterator.erase(it);
// 		}
// 	}
};


}  // namespace policy
