#pragma once
// Forwarding to a *physically remote* disk.
//
// Everything here exists so that two ways of attaching a guest to a remote
// disk can be compared with real sockets, real threads and real sleeps (no
// simulated clock):
//
//   direct : guest NBD request -> remote disk, every read crosses the link
//   cache  : guest NBD request -> local disk cache (+ evict/prefetch policy)
//            -> remote disk only on a miss; the prefetch policy runs ahead of
//            the guest on background connections
//
// Pieces (implemented in fwd.cpp):
//   RemoteDiskServer  the far side: serves a file over TCP and charges every
//                     request the link's RTT, the disk's service time and a
//                     bandwidth-shared transmit time (demand traffic can be
//                     given priority over bulk/prefetch traffic).
//   RemoteClient      the near side's connection pool to that server.
//   DirectReadClass   nbd::BlockReadClass that forwards every read.
//   FwdReadClass      nbd::BlockReadClass with a local disk cache + policies.
//
// Both reader classes plug into disk/nbd.cpp (`source: remote`) and into the
// disk/fwd_sim.cpp benchmark driver unchanged.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nbd.hpp"
#include "overlay.hpp"
#include "../policies/policy_api.h"

namespace fwd {

// Sleeps for `ns` nanoseconds with microsecond accuracy. A plain sleep_for overshoots
// by ~55-65 us on this kernel (measured), which is as large as a rack-local round trip
// and would be paid several times per emulated request. So: sleep to ~100 us short of
// the deadline, then spin the rest.
inline void SleepPrecise(uint64_t ns) {
    if (!ns) return;
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + std::chrono::nanoseconds(ns);
    constexpr uint64_t kSpinNs = 100000;
    if (ns > kSpinNs + 50000) std::this_thread::sleep_until(deadline - std::chrono::nanoseconds(kSpinNs));
    while (clk::now() < deadline) {}
}

// Traffic class. Demand = a guest is blocked on it; bulk = speculative prefetch.
enum Prio : uint8_t { kDemand = 0, kBulk = 1 };

// Physical characteristics of the remote disk and the link to it.
struct LinkConfig {
    uint64_t rtt_us = 2000;          // round-trip propagation delay (half each way)
    double bandwidth_mbps = 1000;    // shared link capacity, megabits/s (0 = unlimited)
    uint64_t disk_us = 100;          // remote disk per-request service latency
    double disk_mbps = 0;            // remote disk media rate, megabytes/s (0 = unlimited)
    uint64_t chunk_bytes = 65536;    // link arbitration granularity (bounds demand's wait behind bulk)
    bool prioritize_demand = true;   // demand chunks jump ahead of queued bulk chunks
};

struct ServerStats {
    uint64_t requests[2] = {0, 0};   // by Prio
    uint64_t bytes[2] = {0, 0};
};

class RemoteDiskServer {
public:
    RemoteDiskServer(const std::string& path, LinkConfig link);
    ~RemoteDiskServer();
    // Binds 127.0.0.1:port (0 = pick one). Returns the bound port, or -1.
    int listen(uint16_t port);
    // Accepts and serves until stop(). Blocks.
    void run();
    void stop();
    uint64_t size() const { return size_; }
    ServerStats stats();

private:
    struct Link;
    void serve_conn(int fd);
    int fd_{-1}, listen_fd_{-1};
    uint64_t size_{0};
    LinkConfig cfg_;
    Link* link_;
    std::atomic<bool> stopping_{false};
    std::mutex stats_mu_;
    ServerStats stats_;
};

class RemoteClient {
public:
    // `demand_conns` / `bulk_conns`: separate connection pools so speculative
    // fetches can never starve a guest-blocking read of a connection.
    RemoteClient(std::string host, uint16_t port, unsigned demand_conns, unsigned bulk_conns);
    ~RemoteClient();
    bool connect();                       // opens every connection, learns the disk size
    uint64_t size() const { return size_; }
    // Blocking, thread-safe. Returns false on any I/O error.
    bool read(uint64_t offset, void* buf, uint64_t len, Prio prio);
    uint64_t requests(Prio p) const { return reqs_[p]; }
    uint64_t bytes(Prio p) const { return bytes_[p]; }

private:
    struct Pool {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<int> free_fds;
        std::vector<int> all_fds;
    };
    int open_one();
    int acquire(Pool& p);
    void release(Pool& p, int fd, bool ok);
    std::string host_;
    uint16_t port_;
    unsigned n_[2];
    Pool pools_[2];
    uint64_t size_{0};
    std::atomic<uint64_t> reqs_[2] = {0, 0};
    std::atomic<uint64_t> bytes_[2] = {0, 0};
};

// Every guest read goes straight to the remote disk. This is the "NBD hooked
// up to the remote disk with no local state" baseline.
class DirectReadClass : public nbd::BlockReadClass {
public:
    DirectReadClass(RemoteClient& remote, nbd::Overlay* overlay = nullptr);
    uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf,
                  uint64_t* miss_blocks = nullptr, const nbd::ReqInfo* info = nullptr) override;
    uint32_t write(uint64_t offset, const void* buf, uint64_t length) override;
    uint32_t write_zeroes(uint64_t offset, uint64_t length) override;
    void drain() override {}
    const nbd::Stats& get_stats() override;

private:
    RemoteClient& remote_;
    nbd::Overlay* overlay_;
    std::mutex mu_;
    nbd::Stats stats_;
    uint64_t start_ns_;
};

struct FwdStats {
    uint64_t hit_pages = 0;        // resident when the request arrived
    uint64_t late_pages = 0;       // not resident, but already being fetched (waited for the in-flight fetch)
    uint64_t miss_pages = 0;       // fetched from the remote disk by the guest's own request
    uint64_t hit_requests = 0;     // requests needing no remote wait at all
    uint64_t pf_issued_pages = 0;  // pages handed to the background fetchers
    uint64_t pf_admitted_pages = 0;
    uint64_t pf_used_pages = 0;    // prefetched pages a guest read later touched
    uint64_t pf_wasted_pages = 0;  // prefetched pages evicted before any use
    uint64_t pf_dropped_pages = 0; // prefetch asks refused by the in-flight cap
    uint64_t pf_failed_pages = 0;
    uint64_t pf_stolen_pages = 0;  // queued prefetches a guest read took over before a fetcher started them
    uint64_t demand_lock_wait_ns = 0;  // time guest reads spent waiting to take the cache lock (mu_)
    uint64_t bulk_hold_ns = 0;         // time prefetch fetchers held that lock while admitting pages into the cache file
};

struct FwdConfig {
    unsigned prefetch_threads = 4;          // background fetchers (== bulk connections)
    uint64_t max_prefetch_pages = 4096;     // pages queued+in flight at once; beyond this asks are dropped
    uint64_t max_run_pages = 64;            // longest single prefetch fetch (split longer ranges)
    uint64_t prefetch_budget_pages = 0;     // new pages one request may prefetch (0 = unlimited)
    uint64_t local_read_ns = 0;             // modeled local-device latency per request that touches the cache
    std::string cache_dir = "/tmp";         // where the (unlinked) cache file lives
};

class FwdReadClass : public nbd::BlockReadClass {
public:
    FwdReadClass(RemoteClient& remote, policy::Cache& cache, policy::CachePolicy* evict_policy,
                 policy::CachePolicy* prefetch_policy, FwdConfig cfg, nbd::Overlay* overlay = nullptr);
    ~FwdReadClass() override;
    uint32_t read(uint64_t worker_id, uint64_t offset, size_t length, void* buf,
                  uint64_t* miss_blocks = nullptr, const nbd::ReqInfo* info = nullptr) override;
    uint32_t write(uint64_t offset, const void* buf, uint64_t length) override;
    uint32_t write_zeroes(uint64_t offset, uint64_t length) override;
    void drain() override;   // waits for every queued/in-flight prefetch
    const nbd::Stats& get_stats() override;
    FwdStats fwd_stats();

private:
    struct Run { uint64_t first, count, worker; };
    void prefetch_loop();
    void admit_locked(uint64_t worker, uint64_t page, const uint8_t* data, size_t n, bool is_prefetch);
    void drop_slot_locked(uint64_t worker, uint64_t page);
    void copy_from_slot_locked(uint64_t page, uint8_t* out);
    void enqueue_prefetch_locked(uint64_t worker, policy::PrefetchRequest& req);
    // Fetches pages [first, first+count) from the remote (caller has them
    // marked in-flight and does NOT hold mu_), admits them, clears in-flight.
    bool fetch_and_admit(uint64_t worker, uint64_t first, uint64_t count, Prio prio, uint8_t* out);

    RemoteClient& remote_;
    policy::Cache& cache_;
    policy::CachePolicy* evict_policy_;
    policy::CachePolicy* prefetch_policy_;
    FwdConfig cfg_;
    nbd::Overlay* overlay_;
    const uint64_t psz_;
    uint64_t size_, total_pages_;

    std::mutex mu_;                          // guards everything below + cache_ + policies
    std::condition_variable cv_;             // page fetch finished
    std::condition_variable work_cv_;        // prefetch work queued
    int cache_fd_{-1};
    uint8_t* store_{nullptr};   // the cache file, memory-mapped: admitting/reading a page is a memcpy, not a syscall under mu_
    size_t store_bytes_{0};
    std::unordered_map<uint64_t, uint64_t> slot_;   // page -> slot in the cache file
    std::vector<uint64_t> free_slots_;
    uint64_t next_slot_{0};
    std::unordered_set<uint64_t> inflight_;         // pages being fetched right now (or queued to be)
    std::unordered_set<uint64_t> queued_;           // subset of inflight_: prefetch queued, no fetcher started yet
    std::unordered_set<uint64_t> unused_pf_;        // prefetched, resident, not yet read
    std::deque<Run> queue_;
    uint64_t pf_outstanding_{0};                    // pages queued or being fetched for prefetch
    bool stopping_{false};
    std::vector<std::thread> workers_;
    FwdStats fs_;
    std::atomic<uint64_t> demand_lock_wait_ns_{0}, bulk_hold_ns_{0};
    nbd::Stats stats_;
    uint64_t start_ns_;
};

}  // namespace fwd
