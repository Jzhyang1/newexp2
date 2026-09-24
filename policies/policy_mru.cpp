#include "policy_api.h"

namespace policy {

MRUPolicy::MRUPolicy(Cache& cache): CachePolicy(cache) {
    keep_around_iterators.reserve(cache.capacity);
    main_mru_iterators.reserve(cache.capacity);
    prefetch_fifo_iterators.reserve(cache.capacity);
}

std::size_t MRUPolicy::prefetch_capacity() const {
    return cache.capacity / 2;
}

void MRUPolicy::move_keep_around_to_main() {
    auto page = keep_around_list.back();
    keep_around_list.pop_back();
    keep_around_iterators.erase(page);
    main_mru_list.push_front(page);
    main_mru_iterators.emplace(page, main_mru_list.begin());
}

void MRUPolicy::move_prefetch_to_main(std::uint64_t page) {
    auto it = prefetch_fifo_iterators.find(page);
    if (it == prefetch_fifo_iterators.end()) return;

    prefetch_fifo_list.erase(it->second);
    prefetch_fifo_iterators.erase(it);
    main_mru_list.push_front(page);
    main_mru_iterators.emplace(page, main_mru_list.begin());
}

void MRUPolicy::on_access(std::uint64_t context, std::uint64_t page) {
    (void)context;

    auto prefetch_it = prefetch_fifo_iterators.find(page);
    if (prefetch_it != prefetch_fifo_iterators.end()) {
        move_prefetch_to_main(page);
        return;
    }

    auto keep_it = keep_around_iterators.find(page);
    if (keep_it != keep_around_iterators.end()) {
        keep_around_list.splice(keep_around_list.begin(), keep_around_list,
                                keep_it->second);
        return;
    }

    auto main_it = main_mru_iterators.find(page);
    if (main_it != main_mru_iterators.end()) {
        main_mru_list.splice(main_mru_list.begin(), main_mru_list,
                             main_it->second);
    }
}

void MRUPolicy::on_admit(std::uint64_t context, std::uint64_t page,
                         bool is_prefetch) {
    (void)context;

    if (is_prefetch) {
        prefetch_fifo_list.push_back(page);
        prefetch_fifo_iterators.emplace(page, std::prev(prefetch_fifo_list.end()));
        if (prefetch_fifo_list.size() > prefetch_capacity()) {
            move_prefetch_to_main(prefetch_fifo_list.front());
        }
        return;
    }

    keep_around_list.push_front(page);
    keep_around_iterators.emplace(page, keep_around_list.begin());
    if (keep_around_list.size() > kKeepAroundPages) {
        move_keep_around_to_main();
    }
}

void MRUPolicy::on_evict_request(std::uint64_t context, std::uint64_t page,
                                 EvictRequest& request) {
    (void)context;
    if (cache.present(page) || cache.size() < cache.capacity) return;

    if (!main_mru_list.empty()) {
        request.pages[request.n_pages++] = main_mru_list.front();
    } else if (!prefetch_fifo_list.empty()) {
        request.pages[request.n_pages++] = prefetch_fifo_list.front();
    } else if (!keep_around_list.empty()) {
        request.pages[request.n_pages++] = keep_around_list.back();
    }
}

void MRUPolicy::on_evict(std::uint64_t context, std::uint64_t page) {
    (void)context;

    auto keep_it = keep_around_iterators.find(page);
    if (keep_it != keep_around_iterators.end()) {
        keep_around_list.erase(keep_it->second);
        keep_around_iterators.erase(keep_it);
        return;
    }

    auto main_it = main_mru_iterators.find(page);
    if (main_it != main_mru_iterators.end()) {
        main_mru_list.erase(main_it->second);
        main_mru_iterators.erase(main_it);
        return;
    }

    auto prefetch_it = prefetch_fifo_iterators.find(page);
    if (prefetch_it != prefetch_fifo_iterators.end()) {
        prefetch_fifo_list.erase(prefetch_it->second);
        prefetch_fifo_iterators.erase(prefetch_it);
    }
}

}  // namespace policy
