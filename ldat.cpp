// ldat: userfaultfd-backed twin of dat.cpp.
//
// dat.cpp drives policy::Cache / policy::CachePolicy from synthetic REQ
// messages sent by our own app.cpp over a pipe, and reports a simulated
// clock's worth of hit/miss latency back over the same pipe. ldat drives the
// *same* policy code (same headers, same classes, unmodified) from real page
// faults raised by unmodified applications (Postgres, RocksDB, Redis, ...)
// mmap()'ing files under a watched path, via the lhook.cpp LD_PRELOAD shim.
//
// Because the "requests" here are real CPU threads genuinely blocked in the
// kernel on a page fault, there is no need (and no way) to reproduce dat.cpp's
// simulated-clock / priority-queue machinery: hit/miss delay is injected as
// an actual std::this_thread::sleep_for on the real fault-handling thread,
// and "eviction" has to be made real too -- see evict_one_locked() below --
// or the process's memory footprint would just grow unbounded while the
// simulated cache pretended pages were gone.
//
// Requires:
//   - Linux with userfaultfd(2) (any modern kernel).
//   - process_madvise(2) (Linux 5.10+) to make evictions physically real by
//     dropping the evicted page from the app's address space (so its next
//     touch re-faults through us). Without it (older kernel, or missing
//     CAP_SYS_NICE / mismatched uid), evictions still update the simulated
//     cache and stats, but the physical page quietly stays resident in the
//     app -- i.e. the simulation's admission/eviction decisions are still
//     exercised and logged, but the memory *budget* isn't enforced for real.
#ifndef __linux__
#error "ldat.cpp uses userfaultfd(2)/process_madvise(2) and only builds on Linux"
#endif

// Must land before any system header: guarantees CMSG_*, syscall(),
// pidfd/process_madvise, sigwait()/pthread_sigmask(), etc. are declared
// regardless of glibc's default feature-test-macro guess under -std=c++20.
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "policies/policy_api.h"
#include "uffd_protocol.h"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434  // x86_64
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440  // x86_64
#endif

namespace {

using uffdproto::kPageBytes;
using PageBuf = std::array<char, kPageBytes>;

inline std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
}

struct Args {
    std::string evict_policy = "lru";
    std::string prefetch_policy = "none";
    std::size_t capacity = (1 << 20);
    std::uint64_t miss_delay_ns = 0;
    std::uint64_t hit_delay_ns = 0;
    std::uint64_t warmup_period = 0;
    std::string socket_path = uffdproto::kDefaultSocketPath;
};

std::string require_value(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + argv[i]);
    }
    ++i;
    return argv[i];
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--evict-policy") {
            a.evict_policy = require_value(i, argc, argv);
        } else if (key == "--prefetch-policy") {
            a.prefetch_policy = require_value(i, argc, argv);
        } else if (key == "--capacity") {
            a.capacity = static_cast<std::size_t>(std::stoull(require_value(i, argc, argv)));
        } else if (key == "--miss-delay") {
            a.miss_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--hit-delay") {
            a.hit_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--warmup-period") {
            a.warmup_period = std::stoull(require_value(i, argc, argv));
        } else if (key == "--socket") {
            a.socket_path = require_value(i, argc, argv);
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }
    if (a.capacity == 0) {
        throw std::runtime_error("--capacity must be > 0");
    }
    return a;
}

// ---- global daemon state --------------------------------------------------
// Unlike dat.cpp (one WorkerThread struct instance per pipe, references
// threaded through explicitly), ldat services an a-priori-unknown, changing
// set of processes/connections over its whole lifetime, so this state is
// naturally process-global rather than per-request. `mu` is the single lock
// guarding all of it, playing the same role as dat.cpp's `mu`.

std::shared_mutex mu;

policy::Cache* g_cache = nullptr;
policy::CachePolicy* g_evict_policy = nullptr;
policy::CachePolicy* g_prefetch_policy = nullptr;
std::uint64_t g_hit_delay_ns = 0;
std::uint64_t g_miss_delay_ns = 0;
std::uint64_t g_warmup = 0;

struct Stats {
    std::atomic<std::uint64_t> request_count{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> bytes_read{0};
    std::atomic<std::uint64_t> total_latency_ns{0};  // real wall time per served fault
    std::chrono::steady_clock::time_point start_time;
} stats;

// Files are identified the same way the original suggestion asks for: every
// fault is mapped back to (File_ID, Page_Offset), with File_ID assigned the
// first time ldat sees a given path (across every watched process).
std::unordered_map<std::string, std::uint32_t> g_file_ids;
std::vector<std::string> g_file_paths;  // index == file_id - 1
constexpr int kOffsetBits = 40;

std::uint32_t get_or_assign_file_id_locked(const std::string& path) {
    auto it = g_file_ids.find(path);
    if (it != g_file_ids.end()) return it->second;
    std::uint32_t id = static_cast<std::uint32_t>(g_file_paths.size() + 1);
    g_file_ids.emplace(path, id);
    g_file_paths.push_back(path);
    return id;
}

std::string file_path_for_locked(std::uint32_t file_id) {
    return g_file_paths.at(file_id - 1);
}

inline std::uint64_t make_key(std::uint32_t file_id, std::uint64_t page_offset) {
    return (static_cast<std::uint64_t>(file_id) << kOffsetBits) | (page_offset & ((1ULL << kOffsetBits) - 1));
}

std::mutex g_file_fd_mu;
std::unordered_map<std::string, int> g_open_files;

int get_file_fd(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_file_fd_mu);
    auto it = g_open_files.find(path);
    if (it != g_open_files.end()) return it->second;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::perror(("ldat: open() backing file " + path).c_str());
    }
    g_open_files.emplace(path, fd);
    return fd;
}

struct FileRange {
    std::uint32_t file_id;
    std::uintptr_t base;
    std::uint64_t len;  // bytes
};

struct ProcConn {
    pid_t pid = 0;
    int sock_fd = -1;
    int uffd = -1;
    int pidfd = -1;              // -1 if pidfd_open failed / unsupported kernel
    std::vector<FileRange> ranges;  // guarded by mu
    std::atomic<bool> alive{true};
};

// Kept alive for the whole daemon lifetime (simplest correct ownership: we
// never know when the last reference -- an in-flight thread -- drops).
std::vector<std::unique_ptr<ProcConn>> g_conns;  // guarded by mu

struct PageLocation {
    ProcConn* conn;
    std::uintptr_t va;
};
std::unordered_map<std::uint64_t, std::vector<PageLocation>> g_page_locations;  // guarded by mu

struct InFlightEntry {
    std::shared_future<std::shared_ptr<PageBuf>> data;
};
std::unordered_map<std::uint64_t, InFlightEntry> g_in_flight;  // guarded by mu

// Drops the physical page at `loc` from its owning process, so the next
// touch re-faults through uffd. Mirrors what dat.cpp's cache.evict() means
// in a world where "the cache" is real physical memory in someone else's
// address space.
void real_evict(const PageLocation& loc) {
    if (loc.conn->pidfd < 0) return;  // best-effort only, see file header comment
    struct iovec iov {
        reinterpret_cast<void*>(loc.va), kPageBytes
    };
    // process_madvise(pidfd, iovec[], count, advice, flags)
    syscall(SYS_process_madvise, loc.conn->pidfd, &iov, 1, MADV_DONTNEED, 0);
}

// Must be called with mu held exclusively. Evicts `victim_key` from the
// simulated cache, the eviction policy's own bookkeeping, and (best-effort)
// physical memory everywhere it's currently resident. `context` is the id of
// the request that triggered this eviction (mirrors dat.cpp's
// ctx.trigger_app), passed through even though none of the current policies
// use it on the evict path.
void evict_one_locked(std::uint64_t context, std::uint64_t victim_key, bool count_stats) {
    g_cache->evict(victim_key);
    if (g_evict_policy) g_evict_policy->on_evict(context, victim_key);
    if (count_stats) stats.evictions.fetch_add(1);

    auto it = g_page_locations.find(victim_key);
    if (it != g_page_locations.end()) {
        for (const auto& loc : it->second) real_evict(loc);
        g_page_locations.erase(it);
    }
}

// Must be called with mu held exclusively. Direct analog of dat.cpp's
// WorkerThread::add_to_cache: asks the eviction policy for victims, evicts
// them, and admits `key`. There's no cache-population *delay* modeled here
// (that already happened in the caller, via a real sleep) -- this just
// mutates the bookkeeping.
void admit_locked(std::uint64_t context, std::uint64_t key, bool count_stats) {
    if (g_cache->present(key)) return;

    policy::EvictRequest evictions;
    if (g_evict_policy) g_evict_policy->on_evict_request(context, key, evictions);
    for (std::uint64_t i = 0; i < evictions.n_pages; ++i) {
        evict_one_locked(context, evictions.pages[i], count_stats);
    }

    auto forced = g_cache->insert(key);
    if (!forced.first) {
        evict_one_locked(context, forced.second, count_stats);
    }

    if (g_evict_policy) g_evict_policy->on_admit(context, key);
}

// Best-effort background prefetch: proactively resolve a candidate page in
// `conn`'s own address space before it ever faults, exactly like readahead
// under a real page cache. Runs detached; if the app faults on the same page
// first, our UFFDIO_COPY here just loses the race harmlessly (EEXIST).
void maybe_prefetch(ProcConn* conn, std::uint32_t file_id, const policy::PrefetchRequest& req, std::uint64_t context) {
    for (std::uint64_t i = 0; i < req.n_pages && i < MAX_PREFETCH_PAGES; ++i) {
        std::uint64_t page_offset = req.pages[i];
        std::thread([conn, file_id, page_offset, context] {
            if (!conn->alive.load()) return;
            std::uint64_t key = make_key(file_id, page_offset);
            std::uintptr_t va = 0;
            std::string path;

            {
                std::unique_lock lk(mu);
                if (g_cache->present(key) || g_in_flight.count(key)) return;
                for (const auto& r : conn->ranges) {
                    if (r.file_id != file_id) continue;
                    std::uint64_t off_bytes = page_offset * kPageBytes;
                    if (off_bytes + kPageBytes <= r.len) {
                        va = r.base + off_bytes;
                        break;
                    }
                }
                if (!va) return;  // this process never mapped that offset of the file
                path = file_path_for_locked(file_id);
                admit_locked(context, key, /*count_stats=*/false);
            }

            std::this_thread::sleep_for(std::chrono::nanoseconds(g_miss_delay_ns));
            PageBuf buf{};
            int fd = get_file_fd(path);
            if (fd >= 0) pread(fd, buf.data(), kPageBytes, static_cast<off_t>(page_offset * kPageBytes));
            stats.bytes_read.fetch_add(kPageBytes);

            {
                std::unique_lock lk(mu);
                if (!conn->alive.load()) return;
                g_page_locations[key].push_back({conn, va});
            }

            struct uffdio_copy copy {};
            copy.dst = va;
            copy.src = reinterpret_cast<__u64>(buf.data());
            copy.len = kPageBytes;
            copy.mode = 0;
            ioctl(conn->uffd, UFFDIO_COPY, &copy);  // EEXIST here just means the app got there first
        }).detach();
    }
}

// Services one real page fault. This is ldat's analog of dat.cpp's
// WorkerThread::operator(): where that function ran the cache/policy hooks
// for one REQ line under `mu` then handed simulated latency to a priority
// queue, this runs them for one uffd fault under `mu` then sleeps the same
// latency on the real, actually-blocked calling thread -- there's no need
// for dat.cpp's async resume machinery once the "client" is a real kernel
// thread that's happy to simply block.
void handle_fault(ProcConn* conn, std::uintptr_t fault_addr) {
    auto t0 = std::chrono::steady_clock::now();
    std::uintptr_t page_addr = fault_addr & ~(std::uintptr_t(kPageBytes) - 1);

    FileRange range{};
    bool found = false;
    {
        std::shared_lock lk(mu);
        for (const auto& r : conn->ranges) {
            if (page_addr >= r.base && page_addr < r.base + r.len) {
                range = r;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        // Shouldn't happen: every registered VMA came from a RANGE message
        // sent before the mmap() call that created it returned. Resolve with
        // zero data so the faulting thread doesn't hang forever.
        static PageBuf zero{};
        struct uffdio_copy copy {};
        copy.dst = page_addr;
        copy.src = reinterpret_cast<__u64>(zero.data());
        copy.len = kPageBytes;
        copy.mode = 0;
        ioctl(conn->uffd, UFFDIO_COPY, &copy);
        std::cerr << "ldat: fault at untracked address 0x" << std::hex << page_addr
                  << std::dec << " for pid " << conn->pid << "\n";
        return;
    }

    std::uint64_t page_offset = (page_addr - range.base) / kPageBytes;
    std::uint64_t key = make_key(range.file_id, page_offset);
    std::uint64_t context = static_cast<std::uint64_t>(conn->pid);

    std::shared_future<std::shared_ptr<PageBuf>> future;
    bool am_fetcher = false;
    bool hit = false;
    std::promise<std::shared_ptr<PageBuf>> promise;
    policy::PrefetchRequest prefetch_req;

    {
        std::unique_lock lk(mu);
        // Counted once per fault regardless of coalescing, same as dat.cpp
        // counting every REQ line -- a page currently being fetched by
        // someone else is still absent from the cache right now, so this
        // fault is honestly a miss too, even though it won't trigger a
        // second fetch.
        hit = g_cache->present(key);
        std::uint64_t gidx = stats.request_count.fetch_add(1) + 1;
        bool count_stats = gidx > g_warmup;
        if (count_stats) {
            if (hit) stats.hits.fetch_add(1);
            else stats.misses.fetch_add(1);
        }

        auto it = g_in_flight.find(key);
        if (it != g_in_flight.end()) {
            future = it->second.data;
        } else {
            // Must run under `mu`, same as dat.cpp: mining/recency state in
            // the policies is shared across concurrently-faulting threads.
            if (g_prefetch_policy) g_prefetch_policy->on_prefetch_request(context, key, prefetch_req);
            if (g_evict_policy) g_evict_policy->on_access(context, key);
            if (g_prefetch_policy) g_prefetch_policy->on_access(context, key);

            if (!hit) admit_locked(context, key, count_stats);

            future = promise.get_future().share();
            g_in_flight[key] = InFlightEntry{future};
            am_fetcher = true;
        }
    }

    std::shared_ptr<PageBuf> buf;
    if (am_fetcher) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(hit ? g_hit_delay_ns : g_miss_delay_ns));

        buf = std::make_shared<PageBuf>();
        std::string path;
        {
            std::shared_lock lk(mu);
            path = file_path_for_locked(range.file_id);
        }
        int fd = get_file_fd(path);
        if (fd >= 0) pread(fd, buf->data(), kPageBytes, static_cast<off_t>(page_offset * kPageBytes));
        stats.bytes_read.fetch_add(kPageBytes);

        promise.set_value(buf);
        {
            std::unique_lock lk(mu);
            g_in_flight.erase(key);
        }
        if (g_prefetch_policy) maybe_prefetch(conn, range.file_id, prefetch_req, context);
    } else {
        buf = future.get();
    }

    {
        std::unique_lock lk(mu);
        g_page_locations[key].push_back({conn, page_addr});
    }

    struct uffdio_copy copy {};
    copy.dst = page_addr;
    copy.src = reinterpret_cast<__u64>(buf->data());
    copy.len = kPageBytes;
    copy.mode = 0;
    if (ioctl(conn->uffd, UFFDIO_COPY, &copy) < 0 && errno != EEXIST) {
        std::perror("ldat: UFFDIO_COPY");
    }

    stats.total_latency_ns.fetch_add(elapsed_ns(t0));
}

// One per registered process: blocks on that process's uffd, dispatching a
// fresh thread per fault (mirrors dat.cpp spawning a fresh WorkerThread per
// request) so concurrent faults -- even within the same process -- don't
// serialize behind each other's hit/miss delay.
void fault_reader_thread(ProcConn* conn) {
    while (conn->alive.load()) {
        struct pollfd pfd {
            conn->uffd, POLLIN, 0
        };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;  // timeout; recheck conn->alive
        if (pfd.revents & (POLLERR | POLLHUP)) break;

        struct uffd_msg msg;
        ssize_t n = read(conn->uffd, &msg, sizeof(msg));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;

        std::uintptr_t addr = static_cast<std::uintptr_t>(msg.arg.pagefault.address);
        std::thread(handle_fault, conn, addr).detach();
    }
}

// Handles one hook connection for its entire lifetime: the PROC handshake
// (SCM_RIGHTS uffd handoff), then a stream of RANGE lines as the app mmap()s
// more watched files, until EOF (process exited or hook detached).
void connection_worker(int client_fd) {
    char buf[256];
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct iovec iov {
        buf, sizeof(buf) - 1
    };
    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = recvmsg(client_fd, &msg, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    int uffd = -1;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            std::memcpy(&uffd, CMSG_DATA(c), sizeof(int));
        }
    }

    pid_t pid = 0;
    {
        std::istringstream iss(buf);
        std::string tag;
        iss >> tag >> pid;
        if (tag != "PROC" || pid <= 0 || uffd < 0) {
            std::cerr << "ldat: malformed PROC handshake: " << buf << "\n";
            close(client_fd);
            if (uffd >= 0) close(uffd);
            return;
        }
    }

    auto conn_owner = std::make_unique<ProcConn>();
    ProcConn* conn = conn_owner.get();
    conn->pid = pid;
    conn->sock_fd = client_fd;
    conn->uffd = uffd;
    conn->pidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));  // -1 is fine, see real_evict()

    {
        std::unique_lock lk(mu);
        g_conns.push_back(std::move(conn_owner));
    }

    std::cerr << "ldat: registered pid=" << pid << " uffd=" << uffd
              << (conn->pidfd < 0 ? " (process_madvise unavailable: evictions won't be physically enforced)" : "")
              << "\n";

    std::thread(fault_reader_thread, conn).detach();

    std::string line;
    char ch;
    while (true) {
        line.clear();
        bool eof = false;
        while (true) {
            ssize_t r = read(client_fd, &ch, 1);
            if (r <= 0) {
                eof = true;
                break;
            }
            if (ch == '\n') break;
            line.push_back(ch);
        }
        if (eof) break;

        std::istringstream iss(line);
        std::string tag;
        iss >> tag;
        if (tag == "RANGE") {
            std::uintptr_t base = 0;
            std::uint64_t len = 0;
            std::string path;
            iss >> base >> len;
            std::getline(iss, path);
            if (!path.empty() && path.front() == ' ') path.erase(0, 1);
            if (!path.empty()) {
                std::unique_lock lk(mu);
                std::uint32_t id = get_or_assign_file_id_locked(path);
                conn->ranges.push_back(FileRange{id, base, len});
            }
        } else if (tag == "UNMAP") {
            std::uintptr_t base = 0;
            std::uint64_t len = 0;
            iss >> base >> len;
            std::unique_lock lk(mu);
            auto& ranges = conn->ranges;
            ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                         [&](const FileRange& r) { return r.base == base && r.len == len; }),
                         ranges.end());
        }
    }

    conn->alive.store(false);
    close(conn->uffd);  // wakes fault_reader_thread's poll() with POLLHUP/EBADF
    close(conn->sock_fd);
}

int make_listen_socket(const std::string& path) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("ldat: socket() failed");

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error("ldat: --socket path too long: " + path);
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("ldat: bind() failed on " + path);
    }
    if (listen(fd, 32) < 0) {
        throw std::runtime_error("ldat: listen() failed on " + path);
    }
    return fd;
}

void accept_loop(int listen_fd) {
    while (true) {
        int client = accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread(connection_worker, client).detach();
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);

        policy::Cache cache(args.capacity, args.hit_delay_ns, args.miss_delay_ns);
        g_cache = &cache;
        g_hit_delay_ns = args.hit_delay_ns;
        g_miss_delay_ns = args.miss_delay_ns;
        g_warmup = args.warmup_period;

        // Same policy construction as dat.cpp, verbatim -- this is the
        // whole point: identical eviction/prefetch decisions, driven by real
        // faults instead of synthetic requests.
        policy::CachePolicy* evict_policy = nullptr;
        policy::CachePolicy* prefetch_policy = nullptr;
        if (args.evict_policy == "fifo") {
            evict_policy = new policy::FIFOPolicy(cache);
        } else if (args.evict_policy == "lifo") {
            evict_policy = new policy::LIFOPolicy(cache);
        } else if (args.evict_policy == "lru") {
            evict_policy = new policy::LRUPolicy(cache);
        } else if (args.evict_policy != "none") {
            throw std::runtime_error("Unsupported --evict-policy: " + args.evict_policy);
        }

        if (args.prefetch_policy == "readahead") {
            prefetch_policy = new policy::ReadaheadPolicy(cache);
        } else if (args.prefetch_policy == "cminer") {
            prefetch_policy = new policy::CMinerPolicy(cache);
        } else if (args.prefetch_policy == "quickmine") {
            prefetch_policy = new policy::QuickMinePolicy(cache);
        } else if (args.prefetch_policy == "mithril") {
            prefetch_policy = new policy::MithrilPolicy(cache);
        } else if (args.prefetch_policy != "none") {
            throw std::runtime_error("Unsupported --prefetch-policy: " + args.prefetch_policy);
        }
        g_evict_policy = evict_policy;
        g_prefetch_policy = prefetch_policy;

        sigset_t wait_set;
        sigemptyset(&wait_set);
        sigaddset(&wait_set, SIGTERM);
        sigaddset(&wait_set, SIGINT);
        pthread_sigmask(SIG_BLOCK, &wait_set, nullptr);

        int listen_fd = make_listen_socket(args.socket_path);
        std::thread(accept_loop, listen_fd).detach();

        stats.start_time = std::chrono::steady_clock::now();
        std::cerr << "ldat: listening on " << args.socket_path << "\n";

        int sig = 0;
        sigwait(&wait_set, &sig);

        const std::uint64_t measured = stats.hits + stats.misses;
        const double avg_latency =
            stats.request_count.load() == 0 ? 0.0 : (double)stats.total_latency_ns.load() / stats.request_count.load();
        const double runtime_seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - stats.start_time)
                .count();

        std::cout << "STATS "
                  << "hits=" << stats.hits << ' '
                  << "misses=" << stats.misses << ' '
                  << "evictions=" << stats.evictions << ' '
                  << "bytes_read=" << stats.bytes_read << ' '
                  << "bytes_written=0 "
                  << "avg_latency_ns=" << avg_latency << ' '
                  << "runtime_seconds=" << runtime_seconds << ' '
                  << "hit_ratio=" << (measured == 0 ? 0.0 : (double)stats.hits / measured)
                  << "\n";
        std::cout.flush();

        unlink(args.socket_path.c_str());
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ldat: " << ex.what() << "\n";
        return 1;
    }
}
