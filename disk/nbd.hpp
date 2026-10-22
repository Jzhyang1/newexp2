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
    uint64_t miss_requests{0};      // requests with >=1 missing sector (each is charged miss_latency_ns once)
    // total_latencies == worker_latency_ns + policy_latency_ns + miss_delay_ns
    uint64_t worker_latency_ns{0};  // real time apps spent between handoff and their next request (no disk-side time)
    uint64_t policy_latency_ns{0};  // real compute time spent inside evict/prefetch policy hooks
    uint64_t miss_delay_ns{0};      // modeled miss latency charged (miss_requests * miss_latency_ns)
    // Clocks. Everything above counts post-warmup requests only.
    uint64_t real_time_elapsed_ns{0};   // wall clock since the server started (includes boot and warmup)
    uint64_t real_time_measured_ns{0};  // wall clock since the last warmup request finished
    uint64_t sim_time_elapsed_ns{0};    // virtual clock since the last warmup request finished (includes miss delay)
};

class BlockReadClass {
public:
    // returns the amount actually read; length == buf_sz
    // If non-null, *miss_blocks is set to the number of sectors in this read
    // that were not already cached (before this request's own admissions).
    virtual uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf,
                          uint64_t* miss_blocks = nullptr) = 0;
    // Called once a read's data has been fully handed back to the client
    // (after any physical miss delay and the socket write). Marks the point
    // where the worker's own think time starts, so disk-side time never
    // counts as worker time.
    virtual void response_sent(uint64_t /*worker_id*/) {}
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