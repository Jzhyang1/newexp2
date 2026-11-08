// Linux Network Block Device (NBD) server.
//
// Implements the NBD wire protocol (fixed newstyle handshake, NBD_OPT_EXPORT_NAME
// export, simple-reply transmission) over TCP so that the in-kernel nbd
// driver's `nbd-client` (or qemu-nbd, etc.) can attach to it as /dev/nbdN.
// Reads are served from a real backing file through the simulator's
// BlockReadClass (nbd.hpp/sim.hpp), so an attached VM's disk traffic is
// charged simulated hit/miss latency and mined for prefetch opportunities
// under a configured policy (see policies/). Writes/trims/flushes are
// handled as follows: guest writes/write-zeroes go to a copy-on-write overlay
// (overlay.hpp: a temp file of modified blocks plus a block->slot map) that
// reads are patched from, so data persists for the life of the server while
// the backing file is never modified. Trim is a no-op and flush has nothing to
// sync. Each operation is counted per operation type (count, bytes, errors, FUA, service
// time) in Stats. Out-of-range requests are answered with EINVAL and unknown
// commands with ENOTSUP; both are counted as errors.
//
// The server listens on one TCP port per entry in `ports:` in its config
// file. Each port is its own client identity ("worker_id" in
// BlockReadClass terms): a VM's disk that dials a particular port is
// distinguished from every other VM's disk by which port it connected to,
// so the simulator can track per-client virtual time separately. See
// kvm/vms.yaml for how launch.py assigns VMs to ports.
//
// Linux-only (uses <endian.h>). Build with a C++17 compiler:
//   g++ -O2 -o nbd_server nbd.cpp ../policies/*.cpp
// Run:
//   ./nbd_server config.yaml
// See nbd.example.yaml (next to this file) for the config schema.
// Attach from the client side (as root, with the nbd module loaded):
//   nbd-client <host> <port> /dev/nbd0

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "nbd.hpp"
#include "overlay.hpp"
#include "sim.hpp"
#include "fwd.hpp"
#include "../policies/policy_api.h"

namespace nbd {

constexpr uint64_t kNbdMagic = 0x4e42444d41474943ULL;   // "NBDMAGIC"
constexpr uint64_t kIhaveOpt = 0x49484156454f5054ULL;   // "IHAVEOPT" -- client->server option-request magic
constexpr uint64_t kRepMagic = 0x0003e889045565a9ULL;   // NBD_REP_MAGIC -- server->client option-reply magic
constexpr uint32_t kRequestMagic = 0x25609513;
constexpr uint32_t kSimpleReplyMagic = 0x67446698;

constexpr uint16_t kFlagFixedNewstyle = 1 << 0;
constexpr uint16_t kFlagNoZeroes = 1 << 1;

constexpr uint32_t kClientFlagFixedNewstyle = 1 << 0;
constexpr uint32_t kClientFlagNoZeroes = 1 << 1;

constexpr uint32_t kOptExportName = 1;
constexpr uint32_t kOptAbort = 2;
constexpr uint32_t kOptList = 3;
constexpr uint32_t kOptInfo = 6;
constexpr uint32_t kOptGo = 7;

constexpr uint32_t kRepAck = 1;
constexpr uint32_t kRepErrUnsup = 0x80000001;

constexpr uint16_t kTransHasFlags = 1 << 0;
constexpr uint16_t kTransReadOnly = 1 << 1;
constexpr uint16_t kTransSendFlush = 1 << 2;
constexpr uint16_t kTransSendTrim = 1 << 5;
constexpr uint16_t kTransSendWriteZeroes = 1 << 6;

constexpr uint32_t kErrInval = 22;   // EINVAL
constexpr uint32_t kErrNotSup = 95;  // ENOTSUP

constexpr uint16_t kCmdRead = 0;
constexpr uint16_t kCmdWrite = 1;
constexpr uint16_t kCmdDisc = 2;
constexpr uint16_t kCmdFlush = 3;
constexpr uint16_t kCmdTrim = 4;
constexpr uint16_t kCmdWriteZeroes = 6;

bool ReadFull(int fd, void *buf, size_t len) {
  uint8_t *p = static_cast<uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;  // peer closed
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool WriteFull(int fd, const void *buf, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// Performs the fixed-newstyle handshake and NBD_OPT_EXPORT_NAME negotiation.
// Returns true if the connection should proceed to the transmission phase.
//
// Every failure path logs a reason via `fail()` before returning false --
// previously these were silent, so a wire-protocol bug (see this file's
// header comment about DoHandshake's past IHAVEOPT/REP_MAGIC bug) looked
// identical to an ordinary dropped connection with nothing in the log to
// tell them apart.
bool DoHandshake(int fd, uint64_t size, uint64_t worker_id) {
  auto fail = [&](const char* reason) {
    std::fprintf(stderr, "nbd: worker %llu handshake failed: %s\n",
                 static_cast<unsigned long long>(worker_id), reason);
    return false;
  };

  struct {
    uint64_t magic;
    uint64_t opts_magic;
    uint16_t handshake_flags;
  } __attribute__((packed)) hello{htobe64(kNbdMagic), htobe64(kIhaveOpt),
                                   htobe16(kFlagFixedNewstyle | kFlagNoZeroes)};
  if (!WriteFull(fd, &hello, sizeof(hello))) return fail("failed to write server hello");

  uint32_t client_flags_be;
  if (!ReadFull(fd, &client_flags_be, sizeof(client_flags_be)))
    return fail("failed to read client flags (peer closed before negotiating)");
  uint32_t client_flags = be32toh(client_flags_be);
  bool no_zeroes = client_flags & kClientFlagNoZeroes;
  (void)client_flags;  // fixed-newstyle bit is expected but not enforced

  for (;;) {
    struct {
      uint64_t magic;
      uint32_t opt;
      uint32_t len;
    } __attribute__((packed)) opt_hdr;
    if (!ReadFull(fd, &opt_hdr, sizeof(opt_hdr)))
      return fail("failed to read option header (peer closed mid-negotiation)");
    uint64_t magic = be64toh(opt_hdr.magic);
    uint32_t opt = be32toh(opt_hdr.opt);
    uint32_t len = be32toh(opt_hdr.len);
    if (magic != kIhaveOpt) return fail("bad option magic (not a well-formed NBD client)");

    std::vector<char> data(len);
    if (len > 0 && !ReadFull(fd, data.data(), len))
      return fail("failed to read option payload (peer closed mid-negotiation)");

    if (opt == kOptExportName || opt == kOptGo) {
      // Ignore the requested export name/info requests: there is only one
      // export per listening port, regardless of what's asked for.
      if (opt == kOptGo) {
        // NBD_OPT_GO expects a structured reply; fall back to telling the
        // client it's unsupported so it retries with NBD_OPT_EXPORT_NAME,
        // which every real-world client (including nbd-client) does.
        struct {
          uint64_t magic;
          uint32_t opt;
          uint32_t reply;
          uint32_t len;
        } __attribute__((packed)) rep{htobe64(kRepMagic), htobe32(opt),
                                       htobe32(kRepErrUnsup), 0};
        if (!WriteFull(fd, &rep, sizeof(rep))) return fail("failed to write NBD_OPT_GO reply");
        continue;
      }

      struct {
        uint64_t size;
        uint16_t flags;
      } __attribute__((packed)) info{htobe64(size),
                                      htobe16(kTransHasFlags | kTransSendFlush |
                                              kTransSendTrim |
                                              kTransSendWriteZeroes)};
      if (!WriteFull(fd, &info, sizeof(info))) return fail("failed to write export info");
      if (!no_zeroes) {
        char zero_pad[124] = {0};
        if (!WriteFull(fd, zero_pad, sizeof(zero_pad))) return fail("failed to write zero padding");
      }
      return true;  // handshake done; transmission phase follows
    }

    if (opt == kOptAbort) {
      // Client explicitly asked to abort -- not a failure, so no fail() log.
      struct {
        uint64_t magic;
        uint32_t opt;
        uint32_t reply;
        uint32_t len;
      } __attribute__((packed)) rep{htobe64(kRepMagic), htobe32(opt),
                                     htobe32(kRepAck), 0};
      WriteFull(fd, &rep, sizeof(rep));
      return false;
    }

    // Anything else (LIST, INFO, STARTTLS, ...): not supported.
    struct {
      uint64_t magic;
      uint32_t opt;
      uint32_t reply;
      uint32_t len;
    } __attribute__((packed)) rep{htobe64(kRepMagic), htobe32(opt),
                                   htobe32(kRepErrUnsup), 0};
    if (!WriteFull(fd, &rep, sizeof(rep))) return fail("failed to write unsupported-option reply");
    if (opt == kOptList) continue;  // client may keep negotiating
  }
}

// Sends a simple-reply header for the given request handle/error.
bool SendReply(int fd, uint32_t error, uint64_t handle) {
  struct {
    uint32_t magic;
    uint32_t error;
    uint64_t handle;
  } __attribute__((packed)) reply{htobe32(kSimpleReplyMagic), htobe32(error),
                                   handle};
  return WriteFull(fd, &reply, sizeof(reply));
}

// Transmission phase: serve requests until the client disconnects.
//
// `physical_miss_ns` is 0 in "simulated" latency mode (the simulator just
// accumulates latency in its stats). In "physical" mode it is miss_latency_ns:
// a read request containing at least one cache miss is delayed by that flat
// amount of real time before its data is sent, so the guest's measured run
// time reflects cache performance. (Flat per request, not per missed sector.)
static std::atomic<uint32_t> g_inflight{0};

// Reads served concurrently per connection (config `read_concurrency`, default 8).
// QEMU's NBD client pipelines requests (about half of a FAISS guest's reads arrive with
// another already queued: ReqInfo::queued_bytes), and NBD allows replies out of order,
// so the server overlaps them instead of making every queued read wait for the one ahead.
// 1 = strictly one request at a time, which is what nbd_server did before this option
// existed; it is kept for comparison (the `direct_c1` / `readahead_c1` runs of the VM study).
// Anything that is not a read (write, flush, trim, ...) first waits for the in-flight reads.
static uint32_t g_read_concurrency = 8;

// Per-connection state shared with the detached read workers.
struct ConnState {
  std::mutex mu;
  std::condition_variable cv;
  std::mutex write_mu;  // replies from different workers must not interleave on the socket
  uint32_t active = 0;
  bool failed = false;
};

void ServeTransmission(int fd, BlockReadClass* reader, uint64_t size, uint64_t worker_id,
                       uint64_t physical_miss_ns) {
  static thread_local std::vector<uint8_t> buf(1 << 20, 0);
  auto conn = std::make_shared<ConnState>();
  // Whatever way this function returns, no worker may outlive the connection's use of fd.
  struct DrainOnExit {
    ConnState& c;
    ~DrainOnExit() { std::unique_lock lk(c.mu); c.cv.wait(lk, [&] { return c.active == 0; }); }
  } drain_on_exit{*conn};
  auto wait_idle = [&] { std::unique_lock lk(conn->mu); conn->cv.wait(lk, [&] { return conn->active == 0; }); };

  for (;;) {
    if (g_read_concurrency > 1) {
      std::lock_guard lk(conn->mu);
      if (conn->failed) return;
    }
    struct {
      uint32_t magic;
      uint16_t flags;
      uint16_t type;
      uint64_t handle;
      uint64_t offset;
      uint32_t length;
    } __attribute__((packed)) req;
    if (!ReadFull(fd, &req, sizeof(req))) return;  // ordinary peer disconnect -- not logged, happens on every routine VM shutdown
    if (be32toh(req.magic) != kRequestMagic) {
      std::fprintf(stderr, "nbd: worker %llu bad request magic, dropping connection\n",
                   static_cast<unsigned long long>(worker_id));
      return;
    }

    const uint64_t arrive_ns = steady_ns();
    uint16_t type = be16toh(req.type);
    uint64_t handle = req.handle;  // opaque; echoed back as-is
    uint64_t offset = be64toh(req.offset);
    uint32_t length = be32toh(req.length);
    ReqInfo info;
    info.handle = be64toh(handle);
    info.type = type;
    info.flags = be16toh(req.flags);
    info.arrive_ns = arrive_ns;
    info.inflight = g_inflight.fetch_add(1);
    { int q = 0; if (ioctl(fd, FIONREAD, &q) == 0 && q > 0) info.queued_bytes = static_cast<uint32_t>(q); }
    info.req_length = length;
    struct InflightGuard { ~InflightGuard() { g_inflight.fetch_sub(1); } } inflight_guard;

    // Overflow-safe range check against the exported size.
    const bool in_range = offset <= size && length <= size - offset;
    // Completes a request: records it in the stats and sends the reply
    // (err == 0 on success).
    auto finish = [&](uint32_t err) {
      info.error = err;
      reader->on_command(worker_id, offset, length, info);
      std::lock_guard wl(conn->write_mu);
      bool ok = SendReply(fd, err, handle);
      reader->response_sent(worker_id);
      return ok;
    };

    // Everything but a read is a barrier: let in-flight reads finish first.
    if (type != kCmdRead && g_read_concurrency > 1) wait_idle();

    switch (type) {
      case kCmdRead: {
        if (!in_range) {
          if (!finish(kErrInval)) return;
          break;
        }
        if (g_read_concurrency > 1) {
          {
            std::unique_lock lk(conn->mu);
            conn->cv.wait(lk, [&] { return conn->active < g_read_concurrency; });
            ++conn->active;
          }
          std::thread([conn, fd, reader, worker_id, physical_miss_ns, handle, offset, length, info]() mutable {
            std::vector<uint8_t> data(length);
            bool ok = true;
            for (uint64_t done = 0; done < length;) {
              size_t chunk = static_cast<size_t>(std::min<uint64_t>(length - done, 1 << 20));
              uint64_t miss_blocks = 0;
              reader->read(worker_id, offset + done, chunk, data.data() + done, &miss_blocks, &info);
              if (physical_miss_ns > 0 && miss_blocks > 0) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(physical_miss_ns));
              }
              done += chunk;
            }
            {
              std::lock_guard wl(conn->write_mu);
              ok = SendReply(fd, 0, handle) && WriteFull(fd, data.data(), length);
            }
            reader->response_sent(worker_id);
            std::lock_guard lk(conn->mu);
            if (!ok) conn->failed = true;
            --conn->active;
            conn->cv.notify_all();
          }).detach();
          break;
        }
        if (!SendReply(fd, 0, handle)) return;
        uint64_t remaining = length;
        while (remaining > 0) {
          size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
          uint64_t miss_blocks = 0;
          reader->read(worker_id, offset, chunk, buf.data(), &miss_blocks, &info);
          if (physical_miss_ns > 0 && miss_blocks > 0) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(physical_miss_ns));
          }
          if (!WriteFull(fd, buf.data(), chunk)) return;
          reader->response_sent(worker_id);
          offset += chunk;
          remaining -= chunk;
        }
        break;
      }
      case kCmdWrite: {
        // Stream the payload into the overlay chunk by chunk. The payload must
        // be consumed even once a write has failed or is out of range, so the
        // stream stays in sync.
        uint32_t err = in_range ? 0 : kErrInval;
        uint64_t remaining = length, pos = offset;
        while (remaining > 0) {
          size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
          if (!ReadFull(fd, buf.data(), chunk)) return;
          if (!err) err = reader->write(pos, buf.data(), chunk);
          pos += chunk;
          remaining -= chunk;
        }
        if (!finish(err)) return;
        break;
      }
      case kCmdDisc:
        return;
      case kCmdFlush:
        // The overlay lives in an unlinked temp file that is never meant to
        // outlive the server, so there is nothing to make durable.
        if (!finish(0)) return;
        break;
      case kCmdTrim:
        // Trim is advisory: the NBD spec leaves trimmed data undefined, so
        // keeping it intact is valid.
        if (!finish(in_range ? 0 : kErrInval)) return;
        break;
      case kCmdWriteZeroes:
        if (!finish(in_range ? reader->write_zeroes(offset, length) : kErrInval)) return;
        break;
      default:
        // Unknown command: report "not supported". A command we don't know
        // may carry a payload we can't size, so the stream could desync;
        // nothing sensible to do about that besides replying.
        if (!finish(kErrNotSup)) return;
        break;
    }
  }
}

void HandleClient(int fd, BlockReadClass* reader, uint64_t size, uint64_t worker_id,
                  uint64_t physical_miss_ns) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  // Every connection runs on its own detached thread (see AcceptLoop below);
  // an uncaught exception here (e.g. from a policy hook under cache.mu)
  // would otherwise call std::terminate() and take down every VM on every
  // port with just a generic "terminate called..." line and no chance to
  // flush the CSV stats log. Contain it to this one connection instead.
  try {
    if (DoHandshake(fd, size, worker_id)) {
      ServeTransmission(fd, reader, size, worker_id, physical_miss_ns);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "nbd: worker %llu connection crashed: %s\n",
                 static_cast<unsigned long long>(worker_id), e.what());
  } catch (...) {
    std::fprintf(stderr, "nbd: worker %llu connection crashed with unknown exception\n",
                 static_cast<unsigned long long>(worker_id));
  }
  close(fd);
}

// Opens a listening TCP socket bound to `port` on all interfaces.
int Listen(uint16_t port) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    return -1;
  }
  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    close(listen_fd);
    return -1;
  }
  if (listen(listen_fd, 16) < 0) {
    perror("listen");
    close(listen_fd);
    return -1;
  }
  return listen_fd;
}

// Accepts connections on `listen_fd` forever; every connection accepted here
// is tagged with the same `worker_id` (this port's index in the config's
// `ports:` list), so the simulator can tell clients apart by which port they
// dialed. Each connection is handled on its own detached thread.
void AcceptLoop(int listen_fd, uint16_t port, BlockReadClass* reader, uint64_t size,
                 uint64_t worker_id, uint64_t physical_miss_ns) {
  for (;;) {
    int fd = accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) continue;
      perror("accept");
      continue;
    }
    std::fprintf(stderr, "nbd: client connected on port %u (worker %llu)\n",
                 port, static_cast<unsigned long long>(worker_id));
    // HandleClient owns fd for the lifetime of the connection and closes it
    // itself.
    std::thread(HandleClient, fd, reader, size, worker_id, physical_miss_ns).detach();
  }
}

// ---------------------------------------------------------------------------
// Config: a minimal YAML subset (flat `key: value` lines, `#` comments, and
// `[a, b, c]` inline lists) -- enough to describe this server's backing
// file, listening ports, and cache/policy parameters without pulling in a
// YAML library. See nbd.example.yaml for the schema.
// ---------------------------------------------------------------------------
struct Config {
  std::string source = "local";       // "local" (a backing file) | "remote" (a fwd_sim/RemoteDiskServer, see disk/fwd.cpp)
  std::string file;                   // path to the backing file
  std::vector<uint16_t> ports;        // one export/client identity per port
  std::size_t capacity = 4096;        // cache capacity, in pages of `page_size` bytes
  uint64_t page_size = 4096;          // cache/policy page size in bytes (multiple of 512, power of two).
                                      // Policies, logs' hit counts and `capacity` are all in these units;
                                      // a larger page keeps per-page state tractable on multi-TB disks.
  uint64_t hit_latency_ns = 0;
  uint64_t miss_latency_ns = 0;
  std::string latency_mode = "simulated"; // simulated | physical (sleep miss_latency_ns per missing read)
  uint64_t warmup = 0;
  std::string evict_policy = "none";      // none | fifo | lifo | lru | lru_cxt_aware | mru
  std::string prefetch_policy = "none";   // none | readahead | oracle | cminer | quickmine | mithril | readahead_cxt_aware
  std::string oracle_file = "./logs/oracle.csv";
  uint64_t prefetch_budget_pages = 0;     // max new pages one request may prefetch, for every policy (0 = unlimited)
  uint64_t readahead_pages = 512;         // readahead prefetch window, in cache pages (<= MAX_PREFETCH_PAGES)
  std::string prefetcher_path;
  std::string prefetcher_type = "predict_mean";
  std::size_t prefetcher_min_len = 1;
  std::string prefetcher_training;
  std::string log = "./logs/nbd_results.csv";  // stats CSV, appended to on shutdown
  std::string vm_log_dir = "/tmp/kvm-launch";  // kvm/launch.py's log_dir; its <name>.share/workload.info
                                 // files (guest-written over 9p) are read at shutdown
  std::string access_log = "./logs/nbd_access.csv";  // per-request (worker_id,offset,length,hits,total)
                                 // event log, for verifying capture correctness; set to "" to disable
  std::string prefetch_log = "./logs/nbd_prefetch.csv";  // per-prefetched-range (worker_id,offset,length,hits,total,req)
                                 // log, same shape as access_log (for heatmap comparison); "" to disable
  std::string feature_log;       // per-request host-side feature log (see disk/sim.hpp); "" (default) disables
  std::string overlay_dir = "/tmp";  // directory for the (unlinked) temp file holding guest-modified blocks
  uint64_t overlay_block_size = 4096; // copy-on-write granularity in bytes (power of two, multiple of 512)
  // source: remote -- the backing disk lives across a (physical) link, see disk/fwd.hpp.
  std::string remote_host = "127.0.0.1";
  uint16_t remote_port = 0;
  std::string fwd_mode = "cache";        // direct (every read crosses the link) | cache (local cache + evict/prefetch policy)
  uint32_t read_concurrency = 8;         // reads served in parallel per connection, any source (1 = one at a time)
  unsigned fwd_prefetch_threads = 4;     // background fetchers (cache mode)
  uint64_t fwd_local_read_ns = 0;        // modeled local-cache-device latency per request (cache mode)
  std::string cache_log;         // per-cache-event log (admit/evict, with prefetch + reuse info); "" disables
};

std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::vector<uint16_t> ParsePortList(const std::string& value) {
  std::vector<uint16_t> ports;
  std::string inner = Trim(value);
  if (inner.size() < 2 || inner.front() != '[' || inner.back() != ']') {
    throw std::runtime_error("ports must be a bracketed list, e.g. [10809, 10810]");
  }
  inner = inner.substr(1, inner.size() - 2);
  std::stringstream ss(inner);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = Trim(item);
    if (item.empty()) continue;
    ports.push_back(static_cast<uint16_t>(std::stoul(item)));
  }
  if (ports.empty()) throw std::runtime_error("ports list is empty");
  return ports;
}

Config LoadConfig(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("failed to open config file: " + path);

  Config cfg;
  std::string line;
  while (std::getline(in, line)) {
    size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = Trim(line);
    if (line.empty()) continue;

    size_t colon = line.find(':');
    if (colon == std::string::npos) {
      throw std::runtime_error("malformed config line (expected 'key: value'): " + line);
    }
    std::string key = Trim(line.substr(0, colon));
    std::string value = Trim(line.substr(colon + 1));
    if (value.empty()) continue;

    if (key == "source") {
      cfg.source = value;
    } else if (key == "file") {
      cfg.file = value;
    } else if (key == "ports") {
      cfg.ports = ParsePortList(value);
    } else if (key == "capacity") {
      cfg.capacity = static_cast<std::size_t>(std::stoull(value));
    } else if (key == "page_size") {
      cfg.page_size = std::stoull(value);
      if (cfg.page_size < SECTOR_SIZE || cfg.page_size % SECTOR_SIZE ||
          (cfg.page_size & (cfg.page_size - 1))) {
        throw std::runtime_error("page_size must be a power of two and a multiple of 512");
      }
    } else if (key == "remote_host") {
      cfg.remote_host = value;
    } else if (key == "remote_port") {
      cfg.remote_port = static_cast<uint16_t>(std::stoul(value));
    } else if (key == "read_concurrency") {
      cfg.read_concurrency = static_cast<uint32_t>(std::max<unsigned long>(1, std::stoul(value)));
    } else if (key == "fwd_mode") {
      if (value != "direct" && value != "cache") throw std::runtime_error("fwd_mode must be 'direct' or 'cache'");
      cfg.fwd_mode = value;
    } else if (key == "fwd_prefetch_threads") {
      cfg.fwd_prefetch_threads = static_cast<unsigned>(std::stoul(value));
    } else if (key == "fwd_local_read_ns") {
      cfg.fwd_local_read_ns = std::stoull(value);
    } else if (key == "overlay_dir") {
      cfg.overlay_dir = value;
    } else if (key == "overlay_block_size") {
      cfg.overlay_block_size = std::stoull(value);
      if (cfg.overlay_block_size < SECTOR_SIZE || cfg.overlay_block_size % SECTOR_SIZE ||
          (cfg.overlay_block_size & (cfg.overlay_block_size - 1))) {
        throw std::runtime_error("overlay_block_size must be a power of two and a multiple of 512");
      }
    } else if (key == "feature_log") {
      cfg.feature_log = value;
    } else if (key == "cache_log") {
      cfg.cache_log = value;
    } else if (key == "hit_latency_ns") {
      cfg.hit_latency_ns = std::stoull(value);
    } else if (key == "miss_latency_ns") {
      cfg.miss_latency_ns = std::stoull(value);
    } else if (key == "latency_mode") {
      if (value != "simulated" && value != "physical") {
        throw std::runtime_error("latency_mode must be 'simulated' or 'physical'");
      }
      cfg.latency_mode = value;
    } else if (key == "warmup") {
      cfg.warmup = std::stoull(value);
    } else if (key == "evict_policy") {
      cfg.evict_policy = value;
    } else if (key == "prefetch_policy") {
      cfg.prefetch_policy = value;
    } else if (key == "oracle_file") {
      cfg.oracle_file = value;
    } else if (key == "prefetch_budget_pages") {
      cfg.prefetch_budget_pages = std::stoull(value);
    } else if (key == "readahead_pages") {
      cfg.readahead_pages = std::stoull(value);
    } else if (key == "prefetcher_path") {
      cfg.prefetcher_path = value;
    } else if (key == "prefetcher_type") {
      cfg.prefetcher_type = value;
    } else if (key == "prefetcher_min_len") {
      cfg.prefetcher_min_len = static_cast<std::size_t>(std::stoull(value));
    } else if (key == "prefetcher_training") {
      cfg.prefetcher_training = value;
    } else if (key == "log") {
      cfg.log = value;
    } else if (key == "vm_log_dir") {
      cfg.vm_log_dir = value;
    } else if (key == "access_log") {
      cfg.access_log = value;
    } else if (key == "prefetch_log") {
      cfg.prefetch_log = value;
    } else {
      throw std::runtime_error("unknown config key: " + key);
    }
  }

  if (cfg.source == "remote") {
    if (!cfg.remote_port) throw std::runtime_error("source: remote requires remote_port");
  } else if (cfg.file.empty()) {
    throw std::runtime_error("config is missing required key: file");
  }
  if (cfg.ports.empty()) throw std::runtime_error("config is missing required key: ports");
  if ((cfg.prefetch_policy == "model" || cfg.prefetch_policy == "model-prefetcher") &&
      cfg.prefetcher_path.empty()) {
    throw std::runtime_error("model-prefetcher requires prefetcher_path");
  }
  if ((cfg.prefetch_policy == "model" || cfg.prefetch_policy == "model-prefetcher") &&
      cfg.prefetcher_training.empty()) {
    throw std::runtime_error("model-prefetcher requires prefetcher_training");
  }
  return cfg;
}

policy::CachePolicy* MakeEvictPolicy(const std::string& name, policy::Cache& cache) {
  if (name == "fifo") return new policy::FIFOPolicy(cache);
  if (name == "lifo") return new policy::LIFOPolicy(cache);
  if (name == "lru") return new policy::LRUPolicy(cache);
  if (name == "lru_cxt_aware") return new policy::ContextAwareLRUPolicy(cache);
  if (name == "mru") return new policy::MRUPolicy(cache);
  if (name == "none") return nullptr;
  throw std::runtime_error("Unsupported evict_policy: " + name);
}

policy::CachePolicy* MakePrefetchPolicy(const Config& cfg, policy::Cache& cache) {
  const std::string& name = cfg.prefetch_policy;
  if (name == "readahead") return new policy::ReadaheadPolicy(cache, cfg.readahead_pages);
  if (name == "oracle") return new policy::OraclePolicy(cache, cfg.oracle_file);
  if (name == "model" || name == "model-prefetcher") return new policy::ModelPolicy(
      cache, cfg.prefetcher_path, cfg.prefetcher_type,
      cfg.prefetcher_min_len, cfg.prefetcher_training);
  if (name == "cminer") return new policy::CMinerPolicy(cache);
  if (name == "quickmine") return new policy::QuickMinePolicy(cache);
  if (name == "mithril") return new policy::MithrilPolicy(cache);
  if (name == "readahead_cxt_aware") return new policy::ContextAwareReadaheadPolicy(cache, 8, 8, cfg.readahead_pages);
  if (name == "none") return nullptr;
  throw std::runtime_error("Unsupported prefetch_policy: " + name);
}

// ---------------------------------------------------------------------------
// Stats logging: modeled on dat.cpp's "STATS key=value ..." summary line and
// runner.cpp's append_log (see their last version in git history, e.g.
// `git show HEAD:runner.cpp`) -- printed once on a clean shutdown (SIGINT/
// SIGTERM), since nbd_server has no fixed request count to run out of the
// way dat.cpp did.
// ---------------------------------------------------------------------------

std::string GetHostname() {
  char host[256];
  if (gethostname(host, sizeof(host)) != 0) return "unknown-host";
  host[sizeof(host) - 1] = '\0';
  return host;
}

std::string GetGitCommit() {
  FILE* fp = popen("git rev-parse --short HEAD 2>/dev/null", "r");
  if (!fp) return "unknown";
  char buf[128];
  std::string out;
  if (fgets(buf, sizeof(buf), fp) != nullptr) out = buf;
  pclose(fp);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out.empty() ? "unknown" : out;
}

std::string JoinPorts(const std::vector<uint16_t>& ports) {
  std::string out;
  for (size_t i = 0; i < ports.size(); ++i) {
    if (i) out += ';';
    out += std::to_string(ports[i]);
  }
  return out;
}

void PrintStatsLine(const Stats& stats) {
  const uint64_t measured = stats.hits + stats.misses;
  const uint64_t all_requests = stats.request_count + stats.writes.count + stats.flushes.count +
                                stats.trims.count + stats.write_zeroes.count + stats.unknown_ops.count;
  const double avg_latency_ns = all_requests == 0
      ? 0.0 : static_cast<double>(stats.total_latencies) / static_cast<double>(all_requests);
  auto op = [](const char* name, const OpStats& s) {
    std::ostringstream o;
    o << name << "=" << s.count << ' ' << name << "_bytes=" << s.bytes << ' '
      << name << "_errors=" << s.errors << ' ' << name << "_fua=" << s.fua << ' '
      << name << "_avg_service_ns=" << (s.count ? static_cast<double>(s.service_ns) / s.count : 0.0) << ' ';
    return o.str();
  };
  std::cout << "STATS "
            << "reads=" << stats.request_count << ' '
            << "read_errors=" << stats.read_errors << ' '
            << op("writes", stats.writes) << op("flushes", stats.flushes)
            << op("trims", stats.trims) << op("write_zeroes", stats.write_zeroes)
            << op("unknown_ops", stats.unknown_ops)
            << "hits=" << stats.hits << ' '
            << "misses=" << stats.misses << ' '
            << "evictions=" << stats.evictions << ' '
            << "bytes_read=" << stats.bytes_read << ' '
            << "bytes_written=" << stats.bytes_written << ' '
            << "overlay_bytes=" << stats.overlay_bytes << ' '
            << "miss_requests=" << stats.miss_requests << ' '
            << "avg_latency_ns=" << avg_latency_ns << ' '
            << "total_latency_ns=" << stats.total_latencies << ' '
            << "worker_latency_ns=" << stats.worker_latency_ns << ' '
            << "policy_latency_ns=" << stats.policy_latency_ns << ' '
            << "miss_delay_ns=" << stats.miss_delay_ns << ' '
            << "runtime_seconds=" << static_cast<double>(stats.real_time_elapsed_ns) / 1e9 << ' '
            << "measured_runtime_seconds=" << static_cast<double>(stats.real_time_measured_ns) / 1e9 << ' '
            << "simulated_time_seconds=" << static_cast<double>(stats.sim_time_elapsed_ns) / 1e9 << ' '
            << "hit_ratio=" << (measured == 0 ? 0.0 : static_cast<double>(stats.hits) / measured)
            << "\n";
}

// Reads <vm_log_dir>/<vm>.share/workload.info ("name=<n> runtime_seconds=<s>"),
// which the guest's run.sh (kvm/guest/build_image.sh) writes into the 9p
// directory kvm/launch.py shares with each VM. Only files written since this
// server started count (older ones are leftovers from a previous run). One
// entry per VM, joined with ';' so the CSV stays one row; both strings are
// empty if nothing was found.
void ReadWorkloadInfo(const std::string& dir, std::filesystem::file_time_type since,
                      std::string* names, std::string* runtimes) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<fs::path> infos;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path info = it->path() / "workload.info";
    std::error_code tec;
    if (it->path().extension() != ".share") continue;
    auto mtime = fs::last_write_time(info, tec);
    if (tec || mtime < since) continue;
    infos.push_back(info);
  }
  std::sort(infos.begin(), infos.end());
  for (const auto& p : infos) {
    std::ifstream in(p);
    std::string name, runtime, tok;
    while (in >> tok) {
      if (tok.rfind("name=", 0) == 0) name = tok.substr(5);
      else if (tok.rfind("runtime_seconds=", 0) == 0) runtime = tok.substr(16);
    }
    if (name.empty() && runtime.empty()) continue;
    if (!names->empty()) { *names += ';'; *runtimes += ';'; }
    *names += name;
    *runtimes += runtime;
  }
}

// Appends one CSV row to cfg.log, writing a header first if the file is new
// or empty -- same convention runner.cpp used for its results log.
void AppendLog(const Config& cfg, const Stats& stats,
               std::filesystem::file_time_type server_start) {
  std::filesystem::path log_path(cfg.log);
  if (!log_path.parent_path().empty()) {
    std::filesystem::create_directories(log_path.parent_path());
  }

  static const std::string kHeader =
      "timestamp,machine,commit_hash,file,ports,capacity,hit_latency_ns,miss_latency_ns,"
      "warmup,evict_policy,prefetch_policy,latency_mode,workload,"
      "requests,miss_requests,blk_hits,blk_misses,hit_ratio,"
      "evictions,bytes_read,bytes_written,overlay_bytes,read_errors,"
      "writes,write_errors,write_fua,write_service_s,"
      "flushes,flush_errors,flush_service_s,"
      "trims,trim_bytes,trim_errors,trim_service_s,"
      "write_zeroes,write_zeroes_bytes,write_zeroes_errors,write_zeroes_service_s,"
      "unknown_ops,"
      "total_s,worker_s,policy_s,miss_s,physical_s,simulated_s,"
      "experiment_s,workload_s";

  // A log written under an older column layout can't be appended to without
  // misaligning columns, so move it aside and start a fresh one.
  {
    std::ifstream in(cfg.log);
    std::string first;
    if (in.good() && std::getline(in, first) && !first.empty() && first != kHeader) {
      in.close();
      const std::string old = cfg.log + ".old-" + std::to_string(std::time(nullptr));
      std::filesystem::rename(cfg.log, old);
      std::fprintf(stderr, "nbd: %s has an older column layout; moved to %s\n",
                   cfg.log.c_str(), old.c_str());
    }
  }

  bool need_header = true;
  {
    std::ifstream in(cfg.log);
    need_header = !in.good() || in.peek() == std::ifstream::traits_type::eof();
  }

  std::ofstream out(cfg.log, std::ios::app);
  if (!out) {
    std::fprintf(stderr, "nbd: failed to open log file: %s\n", cfg.log.c_str());
    return;
  }

  if (need_header) {
    out << kHeader << '\n';
  }

  const uint64_t measured = stats.hits + stats.misses;
  const auto now = std::chrono::system_clock::now();
  const auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  std::string prefetch_policy = cfg.prefetch_policy == "model-prefetcher" ? cfg.prefetcher_path : cfg.prefetch_policy;
  std::string workload, workload_runtime;
  ReadWorkloadInfo(cfg.vm_log_dir, server_start, &workload, &workload_runtime);

  out << ts << ','
      << GetHostname() << ','
      << GetGitCommit() << ','
      << cfg.file << ','
      << JoinPorts(cfg.ports) << ','
      << cfg.capacity << ','
      << cfg.hit_latency_ns << ','
      << cfg.miss_latency_ns << ','
      << cfg.warmup << ','
      << cfg.evict_policy << ','
      << prefetch_policy << ','
      << cfg.latency_mode << ','
      << workload << ','
      << stats.request_count << ','
      << stats.miss_requests << ','
      << stats.hits << ','
      << stats.misses << ','
      << (measured == 0 ? 0.0 : static_cast<double>(stats.hits) / measured) << ','
      << stats.evictions << ','
      << stats.bytes_read << ','
      << stats.bytes_written << ','
      << stats.overlay_bytes << ','
      << stats.read_errors << ','
      << stats.writes.count << ',' << stats.writes.errors << ',' << stats.writes.fua << ','
      << static_cast<double>(stats.writes.service_ns) / 1e9 << ','
      << stats.flushes.count << ',' << stats.flushes.errors << ','
      << static_cast<double>(stats.flushes.service_ns) / 1e9 << ','
      << stats.trims.count << ',' << stats.trims.bytes << ',' << stats.trims.errors << ','
      << static_cast<double>(stats.trims.service_ns) / 1e9 << ','
      << stats.write_zeroes.count << ',' << stats.write_zeroes.bytes << ','
      << stats.write_zeroes.errors << ','
      << static_cast<double>(stats.write_zeroes.service_ns) / 1e9 << ','
      << stats.unknown_ops.count << ','
      << static_cast<double>(stats.total_latencies) / 1e9 << ','
      << static_cast<double>(stats.worker_latency_ns) / 1e9 << ','
      << static_cast<double>(stats.policy_latency_ns) / 1e9 << ','
      << static_cast<double>(stats.miss_delay_ns) / 1e9 << ','
      << static_cast<double>(stats.real_time_measured_ns) / 1e9 << ','
      // Virtual clock; excludes warmup, and includes miss delay.
      << static_cast<double>(stats.sim_time_elapsed_ns) / 1e9 << ','
      << static_cast<double>(stats.real_time_elapsed_ns) / 1e9 << ','
      << workload_runtime
      << '\n';

  std::fprintf(stderr, "nbd: appended stats to %s\n", cfg.log.c_str());
}

}  // namespace nbd

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <config.yaml> [ports]\n", argv[0]);
    return 1;
  }

  std::vector<uint16_t> ports;
  for (int i = 2; i < argc; i++) {
    try {
      int port = std::stoi(argv[i]);
      ports.push_back(port);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", argv[1], e.what());
      return 1;
    }
  }

  nbd::Config cfg;
  cfg.ports = ports;
  try {
    cfg = nbd::LoadConfig(argv[1]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[1], e.what());
    return 1;
  }

  const bool remote = strcasecmp(cfg.source.c_str(), "remote") == 0;
  if (!remote && strcasecmp(cfg.source.c_str(), "local") != 0) {
    std::fprintf(stderr, "unsupported source: %s (expected 'local' or 'remote')\n", cfg.source.c_str());
    return 1;
  }

  FILE* f = nullptr;
  fwd::RemoteClient* remote_client = nullptr;
  uint64_t size = 0;
  if (remote) {
    // One demand connection per exported port (each guest has one request in
    // flight per connection), plus a separate pool for speculative fetches.
    remote_client = new fwd::RemoteClient(
        cfg.remote_host, cfg.remote_port,
        static_cast<unsigned>(std::max<size_t>(2, cfg.ports.size() * cfg.read_concurrency)),
        cfg.fwd_mode == "cache" ? std::max(1u, cfg.fwd_prefetch_threads) : 1u);
    if (!remote_client->connect()) {
      std::fprintf(stderr, "cannot reach remote disk %s:%u\n", cfg.remote_host.c_str(), cfg.remote_port);
      return 1;
    }
    size = remote_client->size();  // the exported device is exactly the remote disk
  } else {
    f = fopen(cfg.file.c_str(), "rb");
    if (!f) {
      perror(("failed to open backing file " + cfg.file).c_str());
      return 1;
    }
    struct stat st;
    if (fstat(fileno(f), &st) < 0) {
      perror("fstat");
      return 1;
    }
    // The exported device size is exactly the backing file's size.
    size = static_cast<uint64_t>(st.st_size);
  }

  static policy::Cache cache(cfg.capacity, cfg.hit_latency_ns, cfg.miss_latency_ns, cfg.page_size);
  policy::CachePolicy* evict_policy = nbd::MakeEvictPolicy(cfg.evict_policy, cache);
  policy::CachePolicy* prefetch_policy = nbd::MakePrefetchPolicy(cfg, cache);

  FILE* access_log = nullptr;
  if (!cfg.access_log.empty()) {
    std::filesystem::path access_log_path(cfg.access_log);
    if (!access_log_path.parent_path().empty()) {
      std::filesystem::create_directories(access_log_path.parent_path());
    }
    access_log = fopen(cfg.access_log.c_str(), "w");
    if (!access_log) {
      perror(("failed to open access_log " + cfg.access_log).c_str());
      return 1;
    }
    std::fprintf(access_log, "worker_id,offset,length,hits,total\n");
    std::fflush(access_log);
  }

  FILE* prefetch_log = nullptr;
  if (!cfg.prefetch_log.empty()) {
    std::filesystem::path prefetch_log_path(cfg.prefetch_log);
    if (!prefetch_log_path.parent_path().empty()) {
      std::filesystem::create_directories(prefetch_log_path.parent_path());
    }
    prefetch_log = fopen(cfg.prefetch_log.c_str(), "w");
    if (!prefetch_log) {
      perror(("failed to open prefetch_log " + cfg.prefetch_log).c_str());
      return 1;
    }
    std::fprintf(prefetch_log, "worker_id,offset,length,hits,total,req\n");
    std::fflush(prefetch_log);
  }

  auto open_log = [](const std::string& path) -> FILE* {
    if (path.empty()) return nullptr;
    std::filesystem::path p(path);
    if (!p.parent_path().empty()) std::filesystem::create_directories(p.parent_path());
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) { perror(("failed to open log " + path).c_str()); std::exit(1); }
    return fp;
  };
  FILE* feature_log = open_log(cfg.feature_log);
  FILE* cache_log = open_log(cfg.cache_log);

  nbd::Overlay* overlay = nullptr;
  try {
    overlay = new nbd::Overlay(f ? fileno(f) : -1, size, cfg.overlay_block_size, cfg.overlay_dir);
    if (remote_client) {  // partial-block writes read-modify-write against the remote disk
      overlay->set_base_reader([remote_client](void* buf, size_t len, uint64_t off) -> ssize_t {
        return remote_client->read(off, buf, len, fwd::kDemand) ? static_cast<ssize_t>(len) : -1;
      });
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }

  nbd::BlockReadClass* reader = nullptr;
  fwd::FwdReadClass* fwd_cache = nullptr;
  if (remote && cfg.fwd_mode == "direct") {
    reader = new fwd::DirectReadClass(*remote_client, overlay);
  } else if (remote) {
    fwd::FwdConfig fc;
    fc.prefetch_threads = std::max(1u, cfg.fwd_prefetch_threads);
    fc.prefetch_budget_pages = cfg.prefetch_budget_pages;
    fc.local_read_ns = cfg.fwd_local_read_ns;
    fc.cache_dir = cfg.overlay_dir;
    fwd_cache = new fwd::FwdReadClass(*remote_client, cache, evict_policy, prefetch_policy, fc, overlay);
    reader = fwd_cache;
  } else {
    auto* sim = new nbd::SimReadClass(
        f, cfg.ports.size(), cfg.warmup, cache, evict_policy, prefetch_policy, access_log, prefetch_log,
        feature_log, cache_log, overlay);
    sim->prefetch_budget_pages = cfg.prefetch_budget_pages;
    reader = sim;
  }

  nbd::g_read_concurrency = cfg.read_concurrency;
  signal(SIGPIPE, SIG_IGN);

  // Block SIGINT/SIGTERM/SIGUSR1 here, before spawning any threads, so
  // every accept thread inherits the mask and only the sigwait() below ever
  // receives them (same pattern ldat.cpp used) -- lets the server log final
  // stats on a clean shutdown instead of just dying mid-request. SIGUSR1
  // additionally requests an on-demand stats snapshot without shutting
  // down (see `make nbd-stats` / the sigwait loop below) -- useful for
  // telling "still working" from "stuck" on a server left running
  // unattended, without the cost of a periodic background timer thread.
  sigset_t wait_set;
  sigemptyset(&wait_set);
  sigaddset(&wait_set, SIGTERM);
  sigaddset(&wait_set, SIGINT);
  sigaddset(&wait_set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &wait_set, nullptr);

  std::vector<int> listen_fds(cfg.ports.size());
  for (size_t i = 0; i < cfg.ports.size(); ++i) {
    listen_fds[i] = nbd::Listen(cfg.ports[i]);
    if (listen_fds[i] < 0) return 1;
  }

  if (remote) {
    std::fprintf(stderr, "nbd server: exporting %llu bytes from remote disk %s:%u (%s) over %zu port(s)\n",
                 static_cast<unsigned long long>(size), cfg.remote_host.c_str(), cfg.remote_port,
                 cfg.fwd_mode.c_str(), cfg.ports.size());
  } else {
    std::fprintf(stderr, "nbd server: exporting %llu bytes from %s over %zu port(s)\n",
                 static_cast<unsigned long long>(size), cfg.file.c_str(), cfg.ports.size());
  }

  const uint64_t physical_miss_ns =
      cfg.latency_mode == "physical" ? cfg.miss_latency_ns : 0;
  for (size_t i = 0; i < cfg.ports.size(); ++i) {
    std::thread(nbd::AcceptLoop, listen_fds[i], cfg.ports[i], reader, size,
                static_cast<uint64_t>(i), physical_miss_ns).detach();
  }

  const auto server_start = std::filesystem::file_time_type::clock::now();
  int sig = 0;
  for (;;) {
    sigwait(&wait_set, &sig);
    if (sig == SIGUSR1) {
      // Snapshot only -- stderr/NBD_SERVER_LOG, not AppendLog, so repeated
      // snapshots don't append duplicate rows to cfg.log.
      nbd::PrintStatsLine(reader->get_stats());
      continue;
    }
    break;
  }
  std::fprintf(stderr, "nbd server: received signal %d, shutting down\n", sig);

  const nbd::Stats& stats = reader->get_stats();
  nbd::PrintStatsLine(stats);
  nbd::AppendLog(cfg, stats, server_start);
  if (remote_client) {
    std::fprintf(stderr, "FWD mode=%s remote_demand_reqs=%llu remote_demand_bytes=%llu remote_bulk_reqs=%llu remote_bulk_bytes=%llu",
                 cfg.fwd_mode.c_str(), (unsigned long long)remote_client->requests(fwd::kDemand),
                 (unsigned long long)remote_client->bytes(fwd::kDemand), (unsigned long long)remote_client->requests(fwd::kBulk),
                 (unsigned long long)remote_client->bytes(fwd::kBulk));
    if (fwd_cache) {
      const fwd::FwdStats fs = fwd_cache->fwd_stats();
      std::fprintf(stderr, " hit_pages=%llu late_pages=%llu miss_pages=%llu pf_issued=%llu pf_used=%llu pf_wasted=%llu lock_wait_ms=%llu bulk_hold_ms=%llu",
                   (unsigned long long)fs.hit_pages, (unsigned long long)fs.late_pages, (unsigned long long)fs.miss_pages,
                   (unsigned long long)fs.pf_issued_pages, (unsigned long long)fs.pf_used_pages,
                   (unsigned long long)fs.pf_wasted_pages, (unsigned long long)(fs.demand_lock_wait_ns / 1000000),
                   (unsigned long long)(fs.bulk_hold_ns / 1000000));
    }
    std::fprintf(stderr, "\n");
    delete reader;  // joins the prefetch threads before the policies they call into go away
  }
  delete prefetch_policy;
  delete evict_policy;
  if (access_log) std::fclose(access_log);
  if (prefetch_log) std::fclose(prefetch_log);
  if (feature_log) std::fclose(feature_log);
  if (cache_log) std::fclose(cache_log);
  if (f) std::fclose(f);
  return 0;
}
