// Linux Network Block Device (NBD) server.
//
// Implements the NBD wire protocol (fixed newstyle handshake, NBD_OPT_EXPORT_NAME
// export, simple-reply transmission) over TCP so that the in-kernel nbd
// driver's `nbd-client` (or qemu-nbd, etc.) can attach to it as /dev/nbdN.
// Reads are served from a real backing file through the simulator's
// BlockReadClass (nbd.hpp/sim.hpp) -- this is the same cache/policy
// simulation used by dat.cpp, so an attached VM's disk traffic is charged
// simulated hit/miss latency and mined for prefetch opportunities exactly
// like a synthetic workload run through dat.cpp would be. Writes/trims/
// flushes are accepted and discarded; nothing written by a guest is ever
// persisted.
//
// The server listens on one TCP port per entry in `ports:` in its config
// file. Each port is its own client identity ("worker_id" in
// BlockReadClass terms): a VM's disk that dials a particular port is
// distinguished from every other VM's disk by which port it connected to,
// so the simulator can track per-client virtual time separately. See
// kvm/vms.example.yaml for how launch.py assigns VMs to ports.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "nbd.hpp"
#include "sim.hpp"
#include "../policies/policy_api.h"

namespace nbd {

constexpr uint64_t kNbdMagic = 0x4e42444d41474943ULL;   // "NBDMAGIC"
constexpr uint64_t kIhaveOpt = 0x49484156454f5054ULL;   // "IHAVEOPT"
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

// Drains and discards exactly `len` bytes from fd (used to consume write
// payloads we don't actually store).
bool DiscardFull(int fd, uint64_t len) {
  static thread_local std::vector<uint8_t> scratch(1 << 16);
  while (len > 0) {
    size_t chunk = static_cast<size_t>(std::min<uint64_t>(len, scratch.size()));
    if (!ReadFull(fd, scratch.data(), chunk)) return false;
    len -= chunk;
  }
  return true;
}

// Performs the fixed-newstyle handshake and NBD_OPT_EXPORT_NAME negotiation.
// Returns true if the connection should proceed to the transmission phase.
bool DoHandshake(int fd, uint64_t size) {
  struct {
    uint64_t magic;
    uint64_t opts_magic;
    uint16_t handshake_flags;
  } __attribute__((packed)) hello{htobe64(kNbdMagic), htobe64(kIhaveOpt),
                                   htobe16(kFlagFixedNewstyle | kFlagNoZeroes)};
  if (!WriteFull(fd, &hello, sizeof(hello))) return false;

  uint32_t client_flags_be;
  if (!ReadFull(fd, &client_flags_be, sizeof(client_flags_be))) return false;
  uint32_t client_flags = be32toh(client_flags_be);
  bool no_zeroes = client_flags & kClientFlagNoZeroes;
  (void)client_flags;  // fixed-newstyle bit is expected but not enforced

  for (;;) {
    struct {
      uint64_t magic;
      uint32_t opt;
      uint32_t len;
    } __attribute__((packed)) opt_hdr;
    if (!ReadFull(fd, &opt_hdr, sizeof(opt_hdr))) return false;
    uint64_t magic = be64toh(opt_hdr.magic);
    uint32_t opt = be32toh(opt_hdr.opt);
    uint32_t len = be32toh(opt_hdr.len);
    if (magic != kIhaveOpt) return false;

    std::vector<char> data(len);
    if (len > 0 && !ReadFull(fd, data.data(), len)) return false;

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
        } __attribute__((packed)) rep{htobe64(kIhaveOpt), htobe32(opt),
                                       htobe32(kRepErrUnsup), 0};
        if (!WriteFull(fd, &rep, sizeof(rep))) return false;
        continue;
      }

      struct {
        uint64_t size;
        uint16_t flags;
      } __attribute__((packed)) info{htobe64(size),
                                      htobe16(kTransHasFlags | kTransSendFlush |
                                              kTransSendTrim |
                                              kTransSendWriteZeroes)};
      if (!WriteFull(fd, &info, sizeof(info))) return false;
      if (!no_zeroes) {
        char zero_pad[124] = {0};
        if (!WriteFull(fd, zero_pad, sizeof(zero_pad))) return false;
      }
      return true;  // handshake done; transmission phase follows
    }

    if (opt == kOptAbort) {
      struct {
        uint64_t magic;
        uint32_t opt;
        uint32_t reply;
        uint32_t len;
      } __attribute__((packed)) rep{htobe64(kIhaveOpt), htobe32(opt),
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
    } __attribute__((packed)) rep{htobe64(kIhaveOpt), htobe32(opt),
                                   htobe32(kRepErrUnsup), 0};
    if (!WriteFull(fd, &rep, sizeof(rep))) return false;
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
void ServeTransmission(int fd, BlockReadClass* reader, uint64_t worker_id) {
  static thread_local std::vector<uint8_t> buf(1 << 20, 0);

  for (;;) {
    struct {
      uint32_t magic;
      uint16_t flags;
      uint16_t type;
      uint64_t handle;
      uint64_t offset;
      uint32_t length;
    } __attribute__((packed)) req;
    if (!ReadFull(fd, &req, sizeof(req))) return;
    if (be32toh(req.magic) != kRequestMagic) return;

    uint16_t type = be16toh(req.type);
    uint64_t handle = req.handle;  // opaque; echoed back as-is
    uint64_t offset = be64toh(req.offset);
    uint32_t length = be32toh(req.length);

    switch (type) {
      case kCmdRead: {
        if (!SendReply(fd, 0, handle)) return;
        uint64_t remaining = length;
        while (remaining > 0) {
          size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
          reader->read(worker_id, offset, chunk, buf.data());
          if (!WriteFull(fd, buf.data(), chunk)) return;
          offset += chunk;
          remaining -= chunk;
        }
        break;
      }
      case kCmdWrite: {
        if (!DiscardFull(fd, length)) return;
        if (!SendReply(fd, 0, handle)) return;
        break;
      }
      case kCmdDisc:
        return;
      case kCmdFlush:
      case kCmdTrim:
      case kCmdWriteZeroes:
        if (!SendReply(fd, 0, handle)) return;
        break;
      default:
        // Unknown command: report "not supported" (ENOTSUP = 95).
        if (!SendReply(fd, 95, handle)) return;
        break;
    }
  }
}

void HandleClient(int fd, BlockReadClass* reader, uint64_t size, uint64_t worker_id) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (DoHandshake(fd, size)) {
    ServeTransmission(fd, reader, worker_id);
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
                 uint64_t worker_id) {
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
    std::thread(HandleClient, fd, reader, size, worker_id).detach();
  }
}

// ---------------------------------------------------------------------------
// Config: a minimal YAML subset (flat `key: value` lines, `#` comments, and
// `[a, b, c]` inline lists) -- enough to describe this server's backing
// file, listening ports, and cache/policy parameters without pulling in a
// YAML library. See nbd.example.yaml for the schema.
// ---------------------------------------------------------------------------
struct Config {
  std::string source = "local";       // only "local" (a real backing file) is supported
  std::string file;                   // path to the backing file
  std::vector<uint16_t> ports;        // one export/client identity per port
  std::size_t capacity = 4096;        // cache capacity, in SECTOR_SIZE-sized pages
  uint64_t hit_latency_ns = 0;
  uint64_t miss_latency_ns = 0;
  uint64_t warmup = 0;
  std::string evict_policy = "none";      // none | fifo | lifo | lru
  std::string prefetch_policy = "none";   // none | readahead | cminer | quickmine | mithril
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
  bool have_ports = false;
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
      have_ports = true;
    } else if (key == "capacity") {
      cfg.capacity = static_cast<std::size_t>(std::stoull(value));
    } else if (key == "hit_latency_ns") {
      cfg.hit_latency_ns = std::stoull(value);
    } else if (key == "miss_latency_ns") {
      cfg.miss_latency_ns = std::stoull(value);
    } else if (key == "warmup") {
      cfg.warmup = std::stoull(value);
    } else if (key == "evict_policy") {
      cfg.evict_policy = value;
    } else if (key == "prefetch_policy") {
      cfg.prefetch_policy = value;
    } else {
      throw std::runtime_error("unknown config key: " + key);
    }
  }

  if (cfg.file.empty()) throw std::runtime_error("config is missing required key: file");
  if (!have_ports) throw std::runtime_error("config is missing required key: ports");
  return cfg;
}

policy::CachePolicy* MakeEvictPolicy(const std::string& name, policy::Cache& cache) {
  if (name == "fifo") return new policy::FIFOPolicy(cache);
  if (name == "lifo") return new policy::LIFOPolicy(cache);
  if (name == "lru") return new policy::LRUPolicy(cache);
  if (name == "none") return nullptr;
  throw std::runtime_error("Unsupported evict_policy: " + name);
}

policy::CachePolicy* MakePrefetchPolicy(const std::string& name, policy::Cache& cache) {
  if (name == "readahead") return new policy::ReadaheadPolicy(cache);
  if (name == "cminer") return new policy::CMinerPolicy(cache);
  if (name == "quickmine") return new policy::QuickMinePolicy(cache);
  if (name == "mithril") return new policy::MithrilPolicy(cache);
  if (name == "none") return nullptr;
  throw std::runtime_error("Unsupported prefetch_policy: " + name);
}

}  // namespace nbd

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <config.yaml>\n", argv[0]);
    return 1;
  }

  nbd::Config cfg;
  try {
    cfg = nbd::LoadConfig(argv[1]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[1], e.what());
    return 1;
  }

  if (strcasecmp(cfg.source.c_str(), "local") != 0) {
    std::fprintf(stderr, "unsupported source: %s (only 'local' is implemented)\n",
                 cfg.source.c_str());
    return 1;
  }

  FILE* f = fopen(cfg.file.c_str(), "rb");
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
  uint64_t size = static_cast<uint64_t>(st.st_size);

  static policy::Cache cache(cfg.capacity, cfg.hit_latency_ns, cfg.miss_latency_ns);
  policy::CachePolicy* evict_policy = nbd::MakeEvictPolicy(cfg.evict_policy, cache);
  policy::CachePolicy* prefetch_policy = nbd::MakePrefetchPolicy(cfg.prefetch_policy, cache);

  nbd::BlockReadClass* reader = new nbd::SimReadClass(
      f, cfg.ports.size(), cfg.warmup, cache, evict_policy, prefetch_policy);

  signal(SIGPIPE, SIG_IGN);

  std::vector<int> listen_fds(cfg.ports.size());
  for (size_t i = 0; i < cfg.ports.size(); ++i) {
    listen_fds[i] = nbd::Listen(cfg.ports[i]);
    if (listen_fds[i] < 0) return 1;
  }

  std::fprintf(stderr, "nbd server: exporting %llu bytes from %s over %zu port(s)\n",
               static_cast<unsigned long long>(size), cfg.file.c_str(), cfg.ports.size());

  std::vector<std::thread> accept_threads;
  for (size_t i = 0; i + 1 < cfg.ports.size(); ++i) {
    accept_threads.emplace_back(nbd::AcceptLoop, listen_fds[i], cfg.ports[i], reader, size,
                                 static_cast<uint64_t>(i));
  }
  // Run the last port's accept loop on the main thread instead of detaching
  // every one of them, so the process has something to block on.
  nbd::AcceptLoop(listen_fds.back(), cfg.ports.back(), reader, size,
                   static_cast<uint64_t>(cfg.ports.size() - 1));
  for (auto& t : accept_threads) t.join();
}
