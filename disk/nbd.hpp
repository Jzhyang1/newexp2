#pragma once

#include <unistd.h>
#include <chrono>

/**
 * 512-byte sector disk device
 */
#define SECTOR_SIZE 512

namespace nbd {

// Per-operation counters for non-read commands (post-warmup only).
struct OpStats {
    uint64_t count{0};       // requests of this type (including failed ones)
    uint64_t bytes{0};       // payload/range bytes covered by successful requests
    uint64_t errors{0};      // requests answered with a non-zero NBD error
    uint64_t service_ns{0};  // real time from header arrival to reply (incl. payload drain)
    uint64_t fua{0};         // requests carrying NBD_CMD_FLAG_FUA
};

struct Stats {
    uint64_t request_count{0};  // read requests
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint64_t bytes_read{0};
    uint64_t bytes_written{0};  // payload bytes of accepted writes (same as writes.bytes)
    uint64_t overlay_bytes{0};  // size of the modified-block overlay right now (not warmup-gated)
    uint64_t read_errors{0};    // reads rejected (e.g. out of range)
    OpStats writes, flushes, trims, write_zeroes, unknown_ops;
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

// Per-request facts only the wire layer knows. Passed to read()/on_command()
// so the simulator can log them (signals #1-9 in offline/SIGNALS.md).
struct ReqInfo {
    uint64_t handle{0};
    uint16_t type{0};        // NBD command (0=read, 1=write, 3=flush, 4=trim, 6=write-zeroes)
    uint16_t flags{0};       // NBD command flags (FUA etc.)
    uint64_t arrive_ns{0};   // steady_ns() right after the request header was read
    uint32_t inflight{0};    // requests being served by other connections when this one arrived
    uint32_t queued_bytes{0};// bytes already waiting in this connection's socket (pipelined requests => queue depth)
    uint64_t req_length{0};  // length of the whole NBD request (read() may be called in chunks)
    uint32_t error{0};       // NBD errno the request is answered with (0 = success); set before on_command()
};

class BlockReadClass {
public:
    virtual ~BlockReadClass() = default;
    // Non-read commands (write/flush/trim/write-zeroes/unknown, plus rejected
    // reads): tracked in Stats and logged; data is never persisted.
    virtual void on_command(uint64_t /*worker_id*/, uint64_t /*offset*/, uint64_t /*length*/,
                            const ReqInfo& /*info*/) {}
    // Persist guest data for the lifetime of the server. Return 0 or an NBD errno.
    virtual uint32_t write(uint64_t /*offset*/, const void* /*buf*/, uint64_t /*length*/) { return 0; }
    virtual uint32_t write_zeroes(uint64_t /*offset*/, uint64_t /*length*/) { return 0; }
    // returns the amount actually read; length == buf_sz
    // If non-null, *miss_blocks is set to the number of sectors in this read
    // that were not already cached (before this request's own admissions).
    virtual uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf,
                          uint64_t* miss_blocks = nullptr, const ReqInfo* info = nullptr) = 0;
    // Called once a read's data has been fully handed back to the client
    // (after any physical miss delay and the socket write). Marks the point
    // where the worker's own think time starts, so disk-side time never
    // counts as worker time.
    virtual void response_sent(uint64_t /*worker_id*/) {}
    // An application hint arrived on the hint port: bytes [first_byte, first_byte + nbytes) of the data disk will be
    // read soon. `aux`: 0..14 = sixteenths-ish of the range the guest page cache holds (mincore), 255 = not reported.
    virtual void hint(uint64_t /*worker_id*/, uint64_t /*first_byte*/, uint64_t /*nbytes*/, uint32_t /*aux*/) {}
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