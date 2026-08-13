// _GNU_SOURCE must land before any system header (including transitively,
// via <dlfcn.h>) so RTLD_NEXT is declared.
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "uffd_protocol.h"

#ifndef __linux__
#error "lhook.cpp uses userfaultfd(2) and /proc, and only builds on Linux"
#endif

#ifndef SYS_userfaultfd
#define SYS_userfaultfd 323  // x86_64; correct on every mainline kernel since 4.3
#endif

// LD_PRELOAD hook: `LD_PRELOAD=./lib/liblhook.so app args...` (normally set
// up for you by lrunner). Interposes mmap()/munmap() so that any file the
// app maps from underneath a watched path prefix (LDAT_WATCH_PREFIX) is
// transparently redirected through userfaultfd instead of the kernel's own
// page cache -- ldat then services every resulting page fault the same way
// dat.cpp services a synthetic REQ, running it through the same
// policy::Cache / CachePolicy eviction and prefetch code.
//
// The app is never modified, relinked, or made aware any of this is
// happening: mmap() still returns a valid pointer into which it can read and
// (for MAP_PRIVATE mappings) write; it just now takes a real page fault
// (routed to ldat) instead of a free ride on the kernel's readahead.
//
// Requires a Linux kernel with userfaultfd (essentially any kernel in
// current use); see ldat.cpp for the additional process_madvise()
// requirement (5.10+) needed to make *evictions* physically real too.

namespace {

using RealMmapFn = void* (*)(void*, std::size_t, int, int, int, off_t);
using RealMunmapFn = int (*)(void*, std::size_t);

RealMmapFn g_real_mmap = nullptr;
RealMunmapFn g_real_munmap = nullptr;

// Resolved eagerly at library-load time (before the app's own code, and
// before any thread but this one exists) rather than lazily via a
// function-local static: dlsym() itself can allocate, which would recurse
// back into our mmap() hook mid-initialization if resolved lazily on first
// use -- C++'s thread-safe statics detect that as recursive initialization
// and abort. Resolving up front sidesteps the whole problem.
__attribute__((constructor)) void resolve_real_symbols() {
    g_real_mmap = reinterpret_cast<RealMmapFn>(dlsym(RTLD_NEXT, "mmap"));
    g_real_munmap = reinterpret_cast<RealMunmapFn>(dlsym(RTLD_NEXT, "munmap"));
}

std::mutex g_mu;
int g_sock = -1;  // connection to ldat, shared by every watched mapping in this process
int g_uffd = -1;  // this process's userfaultfd, shared the same way

// One entry per watched mapping still live in this process, so munmap() can
// tell ldat which range went away. Small (one per remote-disk file mmap'd),
// so a linear scan on munmap is fine.
struct WatchedRange {
    std::uintptr_t base;
    std::uint64_t len;
};
std::vector<WatchedRange> g_watched;  // guarded by g_mu

// Parses LDAT_WATCH_PREFIX once per process (colon-separated, like $PATH).
const std::vector<std::string>& watch_prefixes() {
    static const std::vector<std::string> prefixes = [] {
        std::vector<std::string> out;
        const char* raw = std::getenv(uffdproto::kEnvWatchPrefix);
        if (!raw || !*raw) return out;
        std::string s(raw);
        std::size_t start = 0;
        while (start <= s.size()) {
            std::size_t colon = s.find(':', start);
            std::string part = s.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            if (!part.empty()) out.push_back(part);
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
        return out;
    }();
    return prefixes;
}

bool is_watched(const std::string& path) {
    for (const auto& prefix : watch_prefixes()) {
        if (path.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

bool resolve_fd_path(int fd, std::string& out) {
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    char buf[4096];
    ssize_t n = readlink(link, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    out.assign(buf, static_cast<std::size_t>(n));
    return true;
}

// Must be called with g_mu held. Lazily creates this process's userfaultfd
// and hands it to ldat over a fresh connection, via the PROC handshake in
// uffd_protocol.h. A no-op after the first successful call.
bool ensure_uffd_and_socket_locked() {
    if (g_uffd >= 0 && g_sock >= 0) return true;

    if (g_uffd < 0) {
        long fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            std::perror("lhook: userfaultfd()");
            return false;
        }
        g_uffd = static_cast<int>(fd);

        struct uffdio_api api {};
        api.api = UFFD_API;
        api.features = 0;
        if (ioctl(g_uffd, UFFDIO_API, &api) < 0) {
            std::perror("lhook: UFFDIO_API");
            close(g_uffd);
            g_uffd = -1;
            return false;
        }
    }

    if (g_sock < 0) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) {
            std::perror("lhook: socket()");
            return false;
        }
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        const char* sock_path = std::getenv(uffdproto::kEnvSocketPath);
        if (!sock_path || !*sock_path) sock_path = uffdproto::kDefaultSocketPath;
        std::strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

        if (connect(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("lhook: connect() to ldat");
            close(s);
            return false;
        }

        // Hand the uffd fd to ldat via SCM_RIGHTS, alongside the PROC line
        // identifying which process it belongs to.
        std::string line = uffdproto::proc_line(getpid());
        struct iovec iov { const_cast<char*>(line.data()), line.size() };
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        struct msghdr msg {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &g_uffd, sizeof(int));

        if (sendmsg(s, &msg, 0) < 0) {
            std::perror("lhook: sendmsg() PROC handshake");
            close(s);
            return false;
        }

        g_sock = s;
    }

    return true;
}

// Must be called with g_mu held, after ensure_uffd_and_socket_locked().
void send_range_line_locked(std::uintptr_t base, std::uint64_t len, const std::string& path) {
    std::string line = uffdproto::range_line(base, len, path);
    std::size_t off = 0;
    while (off < line.size()) {
        ssize_t n = write(g_sock, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("lhook: write() RANGE line");
            return;
        }
        off += static_cast<std::size_t>(n);
    }
}

void send_unmap_line_locked(std::uintptr_t base, std::uint64_t len) {
    if (g_sock < 0) return;
    std::string line = uffdproto::unmap_line(base, len);
    (void)write(g_sock, line.data(), line.size());  // best-effort
}

__attribute__((destructor)) void on_process_exit() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_sock >= 0) close(g_sock);
    if (g_uffd >= 0) close(g_uffd);
}

}  // namespace

extern "C" void* mmap(void* addr, std::size_t length, int prot, int flags, int fd, off_t offset) {
    // Anonymous mappings, and file mappings we're not watching, pass through
    // untouched -- this hook only ever changes behavior for files under
    // LDAT_WATCH_PREFIX.
    if (fd < 0 || (flags & MAP_ANONYMOUS) || length == 0) {
        return g_real_mmap(addr, length, prot, flags, fd, offset);
    }

    std::string path;
    if (!resolve_fd_path(fd, path) || !is_watched(path)) {
        return g_real_mmap(addr, length, prot, flags, fd, offset);
    }

    // Round up to whole pages: uffd registration and ldat's fault handling
    // both operate in uffdproto::kPageBytes units.
    std::size_t reg_len = ((length + uffdproto::kPageBytes - 1) / uffdproto::kPageBytes) * uffdproto::kPageBytes;

    // Back the mapping with anonymous memory instead of the real file. This
    // is what makes UFFDIO_REGISTER's MISSING mode reliable regardless of
    // the underlying filesystem (general-purpose file-backed uffd MISSING
    // faults are not supported on most filesystems -- only shmem/hugetlbfs
    // support "minor" faults). ldat supplies the real file's bytes itself
    // (see ldat.cpp's handle_fault) via UFFDIO_COPY, so the app still reads
    // real data -- it just arrives through a fault instead of the page
    // cache. Original `prot` is preserved so read-only mappings stay
    // read-only to the app.
    //
    // Caveat: MAP_SHARED write-back to the underlying file is not
    // reproduced -- writes stay local to this anonymous copy. Fine for the
    // read-heavy workloads (Postgres/RocksDB/Redis page/block reads) this is
    // built to observe; not a general mmap(2) replacement.
    //
    // MAP_SHARED and MAP_PRIVATE must both be cleared before setting
    // MAP_PRIVATE: they're different bits (0x1 / 0x2 on Linux), so if the
    // app asked for MAP_SHARED and we only OR'd in MAP_PRIVATE, the result
    // (0x3) isn't "both" -- it's MAP_SHARED_VALIDATE, a different mapping
    // mode entirely.
    int anon_flags = (flags & ~(MAP_ANONYMOUS | MAP_SHARED | MAP_PRIVATE)) | MAP_ANONYMOUS | MAP_PRIVATE;
    void* result = g_real_mmap(addr, reg_len, prot, anon_flags, -1, 0);
    if (result == MAP_FAILED) return result;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!ensure_uffd_and_socket_locked()) {
            // Best-effort: app still gets valid (just unmonitored) memory.
            return result;
        }

        struct uffdio_register reg {};
        reg.range.start = reinterpret_cast<__u64>(result);
        reg.range.len = reg_len;
        reg.mode = UFFDIO_REGISTER_MODE_MISSING;
        if (ioctl(g_uffd, UFFDIO_REGISTER, &reg) < 0) {
            std::perror("lhook: UFFDIO_REGISTER");
            return result;
        }

        g_watched.push_back(WatchedRange{reinterpret_cast<std::uintptr_t>(result), reg_len});
        send_range_line_locked(reinterpret_cast<std::uintptr_t>(result), reg_len, path);
    }

    return result;
}

extern "C" int munmap(void* addr, std::size_t length) {
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(addr);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto it = g_watched.begin(); it != g_watched.end(); ++it) {
            if (it->base == base) {
                send_unmap_line_locked(it->base, it->len);
                if (g_uffd >= 0) {
                    struct uffdio_range range { it->base, it->len };
                    ioctl(g_uffd, UFFDIO_UNREGISTER, &range);  // best-effort
                }
                g_watched.erase(it);
                break;
            }
        }
    }
    return g_real_munmap(addr, length);
}
