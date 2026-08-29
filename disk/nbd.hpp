#pragma once

#include <unistd.h>
#include <chrono>

/**
 * 512-byte sector disk device
 */
#define SECTOR_SIZE 512

namespace nbd {

struct Stats {
    uint64_t request_count{0};
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint64_t bytes_read{0};
    uint64_t bytes_written{0};
    uint64_t total_latencies{0};
    uint64_t real_time_elapsed_ns{0};
    uint64_t worker_latency_ns{0};  // real time apps spent between handoff and their next request
    uint64_t policy_latency_ns{0};  // real compute time spent inside evict/prefetch policy hooks
};

class BlockReadClass {
public:
    // returns the amount actually read; length == buf_sz
    virtual uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf) = 0;
    // the simulator needs to be drained before we get its stats
    virtual void drain() = 0;
    virtual const Stats& get_stats() = 0;
};

// A steady_clock::time_point, as ns since that clock's epoch -- the form
// app_real_time_start stores, since it's shared across WorkerThreads that
// only agree on a common clock, not on any particular time_point object.
inline uint64_t to_ns(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}
inline uint64_t steady_ns() {
    return to_ns(std::chrono::steady_clock::now());
}

// Real (wall-clock) nanoseconds elapsed since `start`. Used to measure the
// actual compute cost of policy hooks and app think-time, both of which are
// otherwise invisible to the simulated clock (`Stats::sim_time_elapsed`).
inline uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
}

}