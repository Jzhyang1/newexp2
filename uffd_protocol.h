#pragma once

// Wire protocol shared between the LD_PRELOAD mmap() hook (lhook.cpp), which
// runs inside unmodified apps, and the userfaultfd cache/policy daemon
// (ldat.cpp), which services their page faults. Both are Linux-only -- see
// the comments at the top of lhook.cpp and ldat.cpp for kernel requirements.
//
// A hook process opens exactly one connection to ldat's Unix domain socket
// per process (lazily, on its first watched mmap()) and keeps it open for
// the life of the process:
//
//   1. It sends one sendmsg() carrying a "PROC <pid>\n" line as regular data
//      plus an SCM_RIGHTS ancillary message carrying the userfaultfd fd it
//      created and registered the mapping's VMA against. Handing that fd to
//      ldat is what lets ldat -- a different process -- read page-fault
//      events for this process's registered ranges (the standard userfaultfd
//      "monitor process" pattern; see `man userfaultfd`).
//   2. For every watched mmap() in that process (including the one that
//      triggered step 1, and any later ones), it sends a plain
//      "RANGE <base_dec> <len_dec> <path>\n" line -- no ancillary data.
//
// ldat keeps the connection open for the process's lifetime; EOF means the
// process exited (or the hook's destructor ran), at which point ldat drops
// its ranges and closes its copy of the uffd.
//
// This header only carries the line-formatting helpers both sides need to
// agree on; the SCM_RIGHTS/recvmsg plumbing lives in each .cpp since it's
// genuinely different code on the send side vs. the receive side.

#include <cstdint>
#include <string>

namespace uffdproto {

constexpr std::size_t kPageBytes = 4096;

// Env var the hook reads for where to connect, and ldat reads for where to
// listen. Keeping the name identical on both sides is the point.
constexpr const char* kEnvSocketPath = "LDAT_SOCK";
constexpr const char* kDefaultSocketPath = "/tmp/ldat_uffd.sock";

// Colon-separated list of path prefixes (like $PATH). A file is only
// intercepted -- and its mmap() rerouted through uffd -- if its resolved
// path starts with one of these. Sizing this to the remote-disk mount point
// is what limits the hook to "files on the remote disk" per the original ask,
// while leaving every other mmap() (binaries, shared libs, local scratch
// files) to behave completely normally.
constexpr const char* kEnvWatchPrefix = "LDAT_WATCH_PREFIX";

inline std::string proc_line(long pid) {
    return "PROC " + std::to_string(pid) + "\n";
}

inline std::string range_line(std::uintptr_t base, std::uint64_t len, const std::string& path) {
    return "RANGE " + std::to_string(base) + " " + std::to_string(len) + " " + path + "\n";
}

inline std::string unmap_line(std::uintptr_t base, std::uint64_t len) {
    return "UNMAP " + std::to_string(base) + " " + std::to_string(len) + "\n";
}

}  // namespace uffdproto
