// Hooks the local side of the simulator up to a physically remote disk.
// See fwd.hpp for the overall picture.
//
// Wire protocol between RemoteClient and RemoteDiskServer (native endian: both
// ends are meant to run on the same architecture; the "remote" part is the
// emulated link, not a different machine type):
//   request : u32 magic | u8 op | u8 prio | u16 0 | u64 offset | u64 length
//   reply   : u32 magic | u32 status (errno, 0 = ok) | u64 payload_len, then payload
//   op INFO (0): payload is the u64 disk size.   op READ (1): payload is the data.

#include "fwd.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace fwd {

namespace {

constexpr uint32_t kReqMagic = 0x46574452;  // "FWDR"
constexpr uint32_t kRepMagic = 0x46574450;  // "FWDP"
constexpr uint8_t kOpInfo = 0, kOpRead = 1;
constexpr uint64_t kMaxRequest = 64ull << 20;

struct __attribute__((packed)) ReqHdr {
    uint32_t magic;
    uint8_t op, prio;
    uint16_t pad;
    uint64_t offset, length;
};
struct __attribute__((packed)) RepHdr {
    uint32_t magic, status;
    uint64_t length;
};

bool ReadFull(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool WriteFull(int fd, const void* buf, size_t len) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (len) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void SleepNs(uint64_t ns) { SleepPrecise(ns); }

uint64_t PreadFull(int fd, void* buf, uint64_t len, uint64_t off) {
    auto* p = static_cast<uint8_t*>(buf);
    uint64_t got = 0;
    while (got < len) {
        ssize_t n = ::pread(fd, p + got, len - got, static_cast<off_t>(off + got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<uint64_t>(n);
    }
    return got;
}

}  // namespace

// ===========================================================================
// RemoteDiskServer
// ===========================================================================

// The shared link. Bandwidth is divided by serializing fixed-size chunks, so
// concurrent transfers interleave and share it. With prioritization on, no bulk
// chunk starts while any demand transfer is in progress -- the model of a
// QoS-aware link (or just a sender that doesn't queue prefetch ahead of reads).
struct RemoteDiskServer::Link {
    std::mutex m;
    std::condition_variable cv;
    bool busy = false;
    int demand_active = 0;
    double ns_per_byte = 0;
    uint64_t chunk = 65536;
    bool prioritize = true;

    void transmit(uint64_t bytes, Prio p) {
        if (ns_per_byte <= 0) return;
        if (p == kDemand) {
            std::lock_guard lk(m);
            ++demand_active;
        }
        while (bytes) {
            const uint64_t n = std::min(bytes, chunk);
            bytes -= n;
            std::unique_lock lk(m);
            cv.wait(lk, [&] { return !busy && (p == kDemand || !prioritize || demand_active == 0); });
            busy = true;
            lk.unlock();
            SleepNs(static_cast<uint64_t>(static_cast<double>(n) * ns_per_byte));
            lk.lock();
            busy = false;
            lk.unlock();
            cv.notify_all();
        }
        if (p == kDemand) {
            {
                std::lock_guard lk(m);
                --demand_active;
            }
            cv.notify_all();
        }
    }
};

RemoteDiskServer::RemoteDiskServer(const std::string& path, LinkConfig link) : cfg_(link), link_(new Link) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("remote disk: cannot open " + path + ": " + std::strerror(errno));
    struct stat st;
    if (::fstat(fd_, &st) < 0) throw std::runtime_error("remote disk: fstat failed");
    size_ = static_cast<uint64_t>(st.st_size);
    link_->ns_per_byte = cfg_.bandwidth_mbps > 0 ? 8e3 / cfg_.bandwidth_mbps : 0;  // 1 Mbit/s = 0.125 B/us
    link_->chunk = std::max<uint64_t>(cfg_.chunk_bytes, 512);
    link_->prioritize = cfg_.prioritize_demand;
}

RemoteDiskServer::~RemoteDiskServer() {
    stop();
    if (fd_ >= 0) ::close(fd_);
    delete link_;
}

int RemoteDiskServer::listen(uint16_t port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return -1;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 || ::listen(listen_fd_, 128) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }
    socklen_t len = sizeof(a);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
    return ntohs(a.sin_port);
}

void RemoteDiskServer::run() {
    while (!stopping_) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        std::thread([this, fd] {
            serve_conn(fd);
            ::close(fd);
        }).detach();
    }
}

void RemoteDiskServer::stop() {
    stopping_ = true;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

ServerStats RemoteDiskServer::stats() {
    std::lock_guard lk(stats_mu_);
    return stats_;
}

void RemoteDiskServer::serve_conn(int fd) {
    std::vector<uint8_t> buf;
    const uint64_t half_rtt_ns = cfg_.rtt_us * 1000 / 2;
    for (;;) {
        ReqHdr h;
        if (!ReadFull(fd, &h, sizeof(h)) || h.magic != kReqMagic) return;
        const Prio prio = h.prio ? kBulk : kDemand;

        if (h.op == kOpInfo) {
            RepHdr r{kRepMagic, 0, 8};
            uint8_t out[sizeof(r) + 8];
            std::memcpy(out, &r, sizeof(r));
            std::memcpy(out + sizeof(r), &size_, 8);
            if (!WriteFull(fd, out, sizeof(out))) return;
            continue;
        }
        if (h.op != kOpRead) return;

        const bool ok = h.length <= kMaxRequest && h.offset <= size_ && h.length <= size_ - h.offset;
        // Request travels to the server, then the disk does its work.
        uint64_t service_ns = half_rtt_ns + cfg_.disk_us * 1000;
        if (ok && cfg_.disk_mbps > 0) service_ns += static_cast<uint64_t>(h.length / cfg_.disk_mbps * 1e3);
        SleepNs(service_ns);

        if (!ok) {
            RepHdr r{kRepMagic, 22 /*EINVAL*/, 0};
            if (!WriteFull(fd, &r, sizeof(r))) return;
            continue;
        }
        buf.resize(h.length);
        if (PreadFull(fd_, buf.data(), h.length, h.offset) != h.length) {
            RepHdr r{kRepMagic, 5 /*EIO*/, 0};
            if (!WriteFull(fd, &r, sizeof(r))) return;
            continue;
        }
        // Reply contends for the link, then propagates back.
        link_->transmit(h.length + sizeof(RepHdr), prio);
        SleepNs(half_rtt_ns);
        RepHdr r{kRepMagic, 0, h.length};
        if (!WriteFull(fd, &r, sizeof(r)) || !WriteFull(fd, buf.data(), h.length)) return;
        {
            std::lock_guard lk(stats_mu_);
            stats_.requests[prio]++;
            stats_.bytes[prio] += h.length;
        }
    }
}

// ===========================================================================
// RemoteClient
// ===========================================================================

RemoteClient::RemoteClient(std::string host, uint16_t port, unsigned demand_conns, unsigned bulk_conns)
    : host_(std::move(host)), port_(port) {
    n_[kDemand] = std::max(1u, demand_conns);
    n_[kBulk] = std::max(1u, bulk_conns);
}

RemoteClient::~RemoteClient() {
    for (auto& p : pools_)
        for (int fd : p.all_fds) ::close(fd);
}

int RemoteClient::open_one() {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

bool RemoteClient::connect() {
    for (int c = 0; c < 2; ++c) {
        for (unsigned i = 0; i < n_[c]; ++i) {
            int fd = open_one();
            if (fd < 0) return false;
            pools_[c].all_fds.push_back(fd);
            pools_[c].free_fds.push_back(fd);
        }
    }
    int fd = pools_[kDemand].free_fds.back();
    ReqHdr h{kReqMagic, kOpInfo, 0, 0, 0, 0};
    RepHdr r;
    if (!WriteFull(fd, &h, sizeof(h)) || !ReadFull(fd, &r, sizeof(r)) || r.magic != kRepMagic ||
        r.length != 8 || !ReadFull(fd, &size_, 8))
        return false;
    return true;
}

int RemoteClient::acquire(Pool& p) {
    std::unique_lock lk(p.mu);
    p.cv.wait(lk, [&] { return !p.free_fds.empty(); });
    int fd = p.free_fds.back();
    p.free_fds.pop_back();
    return fd;
}

void RemoteClient::release(Pool& p, int fd, bool ok) {
    {
        std::lock_guard lk(p.mu);
        if (!ok) {  // stream state unknown: replace the connection
            ::close(fd);
            p.all_fds.erase(std::remove(p.all_fds.begin(), p.all_fds.end(), fd), p.all_fds.end());
            fd = open_one();
            if (fd < 0) {
                p.cv.notify_all();
                return;
            }
            p.all_fds.push_back(fd);
        }
        p.free_fds.push_back(fd);
    }
    p.cv.notify_one();
}

bool RemoteClient::read(uint64_t offset, void* buf, uint64_t len, Prio prio) {
    if (len == 0) return true;
    Pool& pool = pools_[prio];
    int fd = acquire(pool);
    ReqHdr h{kReqMagic, kOpRead, static_cast<uint8_t>(prio), 0, offset, len};
    RepHdr r{};
    bool ok = WriteFull(fd, &h, sizeof(h)) && ReadFull(fd, &r, sizeof(r)) && r.magic == kRepMagic;
    bool stream_ok = ok;
    if (ok && r.status == 0) {
        ok = r.length == len && ReadFull(fd, buf, len);
        stream_ok = ok;
    } else if (ok) {
        ok = false;  // clean error reply: stream is still in sync
    }
    release(pool, fd, stream_ok);
    if (ok) {
        reqs_[prio]++;
        bytes_[prio] += len;
    }
    return ok;
}

// ===========================================================================
// DirectReadClass
// ===========================================================================

DirectReadClass::DirectReadClass(RemoteClient& remote, nbd::Overlay* overlay)
    : remote_(remote), overlay_(overlay), start_ns_(nbd::steady_ns()) {}

uint32_t DirectReadClass::read(uint64_t, uint64_t offset, size_t length, void* buf,
                               uint64_t* miss_blocks, const nbd::ReqInfo*) {
    if (miss_blocks) *miss_blocks = 0;
    if (!length) return 0;
    const uint64_t t0 = nbd::steady_ns();
    const bool ok = remote_.read(offset, buf, length, kDemand);
    if (!ok) {
        std::fprintf(stderr, "fwd: remote read failed at offset %llu len %zu\n",
                     static_cast<unsigned long long>(offset), length);
        std::memset(buf, 0, length);
    }
    if (overlay_) overlay_->patch(offset, buf, length);
    const uint64_t lat = nbd::steady_ns() - t0;
    std::lock_guard lk(mu_);
    stats_.request_count++;
    stats_.bytes_read += length;
    stats_.miss_requests++;  // every request waits on the remote disk
    stats_.total_latencies += lat;
    stats_.miss_delay_ns += lat;
    return ok ? static_cast<uint32_t>(length) : 0;
}

uint32_t DirectReadClass::write(uint64_t offset, const void* buf, uint64_t length) {
    return overlay_ ? overlay_->write(offset, buf, length) : 0;
}

uint32_t DirectReadClass::write_zeroes(uint64_t offset, uint64_t length) {
    if (!overlay_) return 0;
    static thread_local std::vector<uint8_t> zeros(1 << 20, 0);
    while (length > 0) {
        uint64_t n = std::min<uint64_t>(length, zeros.size());
        if (uint32_t err = overlay_->write(offset, zeros.data(), n)) return err;
        offset += n;
        length -= n;
    }
    return 0;
}

const nbd::Stats& DirectReadClass::get_stats() {
    std::lock_guard lk(mu_);
    stats_.real_time_elapsed_ns = stats_.real_time_measured_ns = nbd::steady_ns() - start_ns_;
    stats_.sim_time_elapsed_ns = stats_.real_time_elapsed_ns;
    return stats_;
}

// ===========================================================================
// FwdReadClass
// ===========================================================================

FwdReadClass::FwdReadClass(RemoteClient& remote, policy::Cache& cache, policy::CachePolicy* evict_policy,
                           policy::CachePolicy* prefetch_policy, FwdConfig cfg, nbd::Overlay* overlay)
    : remote_(remote), cache_(cache), evict_policy_(evict_policy), prefetch_policy_(prefetch_policy),
      cfg_(std::move(cfg)), overlay_(overlay), psz_(cache.page_size), size_(remote.size()),
      total_pages_((remote.size() + cache.page_size - 1) / cache.page_size), start_ns_(nbd::steady_ns()) {
    std::string tmpl = cfg_.cache_dir + "/fwd_cache_XXXXXX";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    cache_fd_ = ::mkstemp(path.data());
    if (cache_fd_ < 0)
        throw std::runtime_error("fwd: cannot create cache file in " + cfg_.cache_dir + ": " + std::strerror(errno));
    ::unlink(path.data());
    // The cache file is mapped, so a page is stored with a memcpy. A pwrite() per 64 KiB page while holding mu_
    // made guest reads wait behind prefetch writes (measured: 0.6 ms per read at deep readahead). Up to 1 GiB
    // is pre-faulted so first-touch page allocation is not paid under the lock either.
    store_bytes_ = static_cast<size_t>(cache_.capacity) * psz_;
    if (store_bytes_) {
        if (::ftruncate(cache_fd_, static_cast<off_t>(store_bytes_)) != 0)
            throw std::runtime_error(std::string("fwd: cannot size cache file: ") + std::strerror(errno));
        void* m = ::mmap(nullptr, store_bytes_, PROT_READ | PROT_WRITE,
                         MAP_SHARED | (store_bytes_ <= (1ull << 30) ? MAP_POPULATE : 0), cache_fd_, 0);
        if (m == MAP_FAILED) throw std::runtime_error(std::string("fwd: cannot map cache file: ") + std::strerror(errno));
        store_ = static_cast<uint8_t*>(m);
    }
    for (unsigned i = 0; i < cfg_.prefetch_threads; ++i) workers_.emplace_back(&FwdReadClass::prefetch_loop, this);
}

FwdReadClass::~FwdReadClass() {
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
    }
    work_cv_.notify_all();
    for (auto& t : workers_) t.join();
    if (store_) ::munmap(store_, store_bytes_);
    if (cache_fd_ >= 0) ::close(cache_fd_);
}

void FwdReadClass::copy_from_slot_locked(uint64_t page, uint8_t* out) {
    auto it = slot_.find(page);
    if (it == slot_.end() || (it->second + 1) * psz_ > store_bytes_) {
        // The policy-level cache said "present" but we hold no data for it:
        // an accounting bug, not a recoverable condition.
        std::fprintf(stderr, "fwd: cache/slot desync for page %llu\n", static_cast<unsigned long long>(page));
        std::abort();
    }
    std::memcpy(out, store_ + it->second * psz_, psz_);
}

void FwdReadClass::drop_slot_locked(uint64_t worker, uint64_t page) {
    auto it = slot_.find(page);
    if (it != slot_.end()) {
        free_slots_.push_back(it->second);
        slot_.erase(it);
    }
    if (unused_pf_.erase(page)) ++fs_.pf_wasted_pages;
    if (evict_policy_) evict_policy_->on_evict(worker, page);
    if (prefetch_policy_) prefetch_policy_->on_evict(worker, page);
}

void FwdReadClass::admit_locked(uint64_t worker, uint64_t page, const uint8_t* data, size_t,
                                bool is_prefetch) {
    if (cache_.present(page) || cache_.capacity == 0) return;
    policy::EvictRequest ev;
    if (evict_policy_) evict_policy_->on_evict_request(worker, page, ev);
    for (uint64_t i = 0; i < ev.n_pages; ++i)
        if (cache_.evict(ev.pages[i])) {
            ++stats_.evictions;
            drop_slot_locked(worker, ev.pages[i]);
        }
    auto r = cache_.insert(page);
    if (!r.first) {  // the cache had to pick its own victim
        ++stats_.evictions;
        drop_slot_locked(worker, r.second);
    }
    if (!cache_.present(page)) return;
    uint64_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = next_slot_++;
    }
    if ((slot + 1) * psz_ > store_bytes_) {  // more live slots than capacity: an accounting bug
        std::fprintf(stderr, "fwd: cache slot %llu out of range\n", static_cast<unsigned long long>(slot));
        std::abort();
    }
    std::memcpy(store_ + slot * psz_, data, psz_);
    slot_[page] = slot;
    if (is_prefetch) {
        unused_pf_.insert(page);
        ++fs_.pf_admitted_pages;
    }
    if (evict_policy_) evict_policy_->on_admit(worker, page, is_prefetch);
    if (prefetch_policy_) prefetch_policy_->on_admit(worker, page, is_prefetch);
}

bool FwdReadClass::fetch_and_admit(uint64_t worker, uint64_t first, uint64_t count, Prio prio, uint8_t* out) {
    const uint64_t off = first * psz_;
    const uint64_t bytes = std::min(count * psz_, size_ - off);
    const bool ok = remote_.read(off, out, bytes, prio);
    if (!ok) std::memset(out, 0, count * psz_);
    else if (bytes < count * psz_) std::memset(out + bytes, 0, count * psz_ - bytes);  // short last page
    {
        std::lock_guard lk(mu_);
        const uint64_t th0 = nbd::steady_ns();
        if (ok)
            for (uint64_t i = 0; i < count; ++i) admit_locked(worker, first + i, out + i * psz_, psz_, prio == kBulk);
        for (uint64_t i = 0; i < count; ++i) inflight_.erase(first + i);
        if (prio == kBulk) bulk_hold_ns_ += nbd::steady_ns() - th0;
    }
    cv_.notify_all();
    return ok;
}

void FwdReadClass::enqueue_prefetch_locked(uint64_t worker, policy::PrefetchRequest& req) {
    uint64_t left = cfg_.prefetch_budget_pages ? cfg_.prefetch_budget_pages : UINT64_MAX;
    Run cur{0, 0, worker};
    auto flush = [&] {
        if (!cur.count) return;
        queue_.push_back(cur);
        fs_.pf_issued_pages += cur.count;
        cur.count = 0;
        work_cv_.notify_one();
    };
    for (uint64_t i = 0; i < req.fetch_count && left; ++i) {
        const policy::FetchRange r = req.fetch_ranges[i];
        const uint64_t end = std::min(r.block_offset + r.block_length, total_pages_);
        for (uint64_t page = r.block_offset; page < end && left; ++page) {
            if (cache_.present(page) || inflight_.count(page)) {
                flush();
                continue;
            }
            if (pf_outstanding_ >= cfg_.max_prefetch_pages) {
                ++fs_.pf_dropped_pages;
                flush();
                continue;
            }
            inflight_.insert(page);
            queued_.insert(page);
            ++pf_outstanding_;
            if (left != UINT64_MAX) --left;
            if (cur.count && (cur.first + cur.count != page || cur.count >= cfg_.max_run_pages)) flush();
            if (!cur.count) cur.first = page;
            ++cur.count;
        }
        flush();
    }
    flush();
}

void FwdReadClass::prefetch_loop() {
    std::vector<uint8_t> buf;
    for (;;) {
        Run r;
        std::vector<Run> todo;
        {
            std::unique_lock lk(mu_);
            work_cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            r = queue_.front();
            queue_.pop_front();
            // A guest read may have taken some of these pages over while they
            // waited; fetch only what is still ours, as contiguous sub-runs.
            uint64_t stolen = 0;
            for (uint64_t i = 0; i < r.count; ++i) {
                const uint64_t page = r.first + i;
                if (!queued_.erase(page)) { ++stolen; continue; }
                if (!todo.empty() && todo.back().first + todo.back().count == page) todo.back().count++;
                else todo.push_back({page, 1, r.worker});
            }
            (void)stolen;  // already subtracted from pf_outstanding_ by the thief
        }
        for (const Run& t : todo) {
            buf.resize(t.count * psz_);
            const bool ok = fetch_and_admit(t.worker, t.first, t.count, kBulk, buf.data());
            {
                std::lock_guard lk(mu_);
                pf_outstanding_ -= t.count;
                if (!ok) fs_.pf_failed_pages += t.count;
            }
            cv_.notify_all();
        }
    }
}

uint32_t FwdReadClass::read(uint64_t worker, uint64_t offset, size_t length, void* buf,
                            uint64_t* miss_blocks, const nbd::ReqInfo*) {
    if (miss_blocks) *miss_blocks = 0;
    if (!length || offset >= size_) return 0;
    length = static_cast<size_t>(std::min<uint64_t>(length, size_ - offset));
    const uint64_t t0 = nbd::steady_ns();
    const uint64_t p0 = offset / psz_, p1 = (offset + length - 1) / psz_, np = p1 - p0 + 1;

    thread_local std::vector<uint8_t> span;
    thread_local std::vector<uint8_t> have;
    span.resize(np * psz_);
    have.assign(np, 0);
    uint64_t hit = 0, late = 0, miss = 0;
    bool touched_local = false, ok = true;
    std::vector<Run> mine;
    policy::PrefetchRequest pf;

    {
        const uint64_t tl0 = nbd::steady_ns();
        std::lock_guard lk(mu_);
        demand_lock_wait_ns_ += nbd::steady_ns() - tl0;
        if (prefetch_policy_) {
            prefetch_policy_->on_request(worker, offset, length, nbd::steady_ns());
            prefetch_policy_->on_prefetch_request(worker, p0, np, pf);
        }
        for (uint64_t i = 0; i < np; ++i) {
            const uint64_t page = p0 + i;
            if (cache_.present(page)) {
                copy_from_slot_locked(page, &span[i * psz_]);
                have[i] = 1;
                ++hit;
                touched_local = true;
                if (unused_pf_.erase(page)) ++fs_.pf_used_pages;
            } else if (inflight_.count(page) && !queued_.count(page)) {
                ++late;  // a fetcher is already on it
            } else {
                // Not resident and nobody is fetching it yet. A prefetch that is
                // merely *queued* is taken over rather than waited for: it could
                // sit behind the whole prefetch backlog.
                if (queued_.erase(page)) {
                    --pf_outstanding_;
                    ++fs_.pf_stolen_pages;
                } else {
                    inflight_.insert(page);
                }
                ++miss;
                if (!mine.empty() && mine.back().first + mine.back().count == page) mine.back().count++;
                else mine.push_back({page, 1, worker});
            }
            if (evict_policy_) evict_policy_->on_access(worker, page);
            if (prefetch_policy_) prefetch_policy_->on_access(worker, page);
        }
        enqueue_prefetch_locked(worker, pf);
    }

    // Our own misses: fetched on demand-priority connections.
    for (const Run& r : mine) {
        ok &= fetch_and_admit(worker, r.first, r.count, kDemand, &span[(r.first - p0) * psz_]);
        for (uint64_t i = 0; i < r.count; ++i) have[r.first - p0 + i] = 1;
    }
    // Pages another fetch (usually a prefetch) already had in flight: wait for it.
    for (uint64_t i = 0; i < np; ++i) {
        if (have[i]) continue;
        const uint64_t page = p0 + i;
        const uint64_t tl1 = nbd::steady_ns();
        std::unique_lock lk(mu_);
        demand_lock_wait_ns_ += nbd::steady_ns() - tl1;
        cv_.wait(lk, [&] { return !inflight_.count(page); });
        if (cache_.present(page)) {
            copy_from_slot_locked(page, &span[i * psz_]);
            touched_local = true;
            if (unused_pf_.erase(page)) ++fs_.pf_used_pages;
        } else {  // evicted again (or the fetch failed) before we got to it
            inflight_.insert(page);
            lk.unlock();
            ok &= fetch_and_admit(worker, page, 1, kDemand, &span[i * psz_]);
        }
    }

    if (touched_local && cfg_.local_read_ns) SleepPrecise(cfg_.local_read_ns);
    std::memcpy(buf, &span[offset - p0 * psz_], length);
    if (overlay_) overlay_->patch(offset, buf, length);
    if (miss_blocks) *miss_blocks = miss + late;

    const uint64_t lat = nbd::steady_ns() - t0;
    {
        const uint64_t tl2 = nbd::steady_ns();
        std::lock_guard lk(mu_);
        demand_lock_wait_ns_ += nbd::steady_ns() - tl2;
        fs_.hit_pages += hit;
        fs_.late_pages += late;
        fs_.miss_pages += miss;
        fs_.hit_requests += (miss + late) == 0;
        stats_.request_count++;
        stats_.bytes_read += length;
        stats_.hits += hit;
        stats_.misses += miss + late;
        stats_.miss_requests += (miss + late) != 0;
        stats_.total_latencies += lat;
    }
    return ok ? static_cast<uint32_t>(length) : 0;
}

uint32_t FwdReadClass::write(uint64_t offset, const void* buf, uint64_t length) {
    // Writes only change data, never cache residency: the cache holds the
    // remote's (base) contents and reads are re-patched from the overlay.
    return overlay_ ? overlay_->write(offset, buf, length) : 0;
}

uint32_t FwdReadClass::write_zeroes(uint64_t offset, uint64_t length) {
    if (!overlay_) return 0;
    static thread_local std::vector<uint8_t> zeros(1 << 20, 0);
    while (length > 0) {
        uint64_t n = std::min<uint64_t>(length, zeros.size());
        if (uint32_t err = overlay_->write(offset, zeros.data(), n)) return err;
        offset += n;
        length -= n;
    }
    return 0;
}

void FwdReadClass::drain() {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [&] { return pf_outstanding_ == 0; });
}

const nbd::Stats& FwdReadClass::get_stats() {
    std::lock_guard lk(mu_);
    stats_.real_time_elapsed_ns = stats_.real_time_measured_ns = nbd::steady_ns() - start_ns_;
    stats_.sim_time_elapsed_ns = stats_.real_time_elapsed_ns;
    return stats_;
}

FwdStats FwdReadClass::fwd_stats() {
    std::lock_guard lk(mu_);
    FwdStats out = fs_;
    out.demand_lock_wait_ns = demand_lock_wait_ns_;
    out.bulk_hold_ns = bulk_hold_ns_;
    return out;
}

}  // namespace fwd
