// Linux Network Block Device (NBD) server.
//
// Implements the NBD wire protocol (fixed newstyle handshake, NBD_OPT_EXPORT_NAME
// export, simple-reply transmission) over TCP so that the in-kernel nbd
// driver's `nbd-client` (or qemu-nbd, etc.) can attach to it as /dev/nbdN.
// The exported device is entirely synthetic: reads always return zeroed
// buffers and writes/trims/flushes are accepted and discarded. Nothing is
// actually stored.
//
// Linux-only (uses <endian.h>). Build with a C++17 compiler:
//   g++ -O2 -o nbd_zero nbd.cpp
// Run:
//   ./nbd_zero [port] [size]
//     port : TCP port to listen on (default 10809)
//     size : exported device size, accepts K/M/G/T suffixes (default 1G)
// Attach from the client side (as root, with the nbd module loaded):
//   nbd-client <host> <port> /dev/nbd0

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

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

// Parses a size string with an optional K/M/G/T suffix (binary units).
uint64_t ParseSize(const std::string &s, uint64_t fallback) {
  if (s.empty()) return fallback;
  char *end = nullptr;
  double val = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || val < 0) return fallback;
  uint64_t mul = 1;
  if (*end) {
    switch (end[0]) {
      case 'k': case 'K': mul = 1ULL << 10; break;
      case 'm': case 'M': mul = 1ULL << 20; break;
      case 'g': case 'G': mul = 1ULL << 30; break;
      case 't': case 'T': mul = 1ULL << 40; break;
      default: return fallback;
    }
  }
  return static_cast<uint64_t>(val * static_cast<double>(mul));
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
      // (synthetic, all-zero) export regardless of what's asked for.
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
void ServeTransmission(int fd, uint64_t size) {
  static thread_local std::vector<uint8_t> zeros(1 << 20, 0);

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
    (void)offset;
    (void)size;  // bounds are not enforced; this is a placeholder device

    switch (type) {
      case kCmdRead: {
        if (!SendReply(fd, 0, handle)) return;
        uint64_t remaining = length;
        while (remaining > 0) {
          size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, zeros.size()));
          if (!WriteFull(fd, zeros.data(), chunk)) return;
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

void HandleClient(int fd, uint64_t size) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (DoHandshake(fd, size)) {
    ServeTransmission(fd, size);
  }
  close(fd);
}

}  // namespace

int main(int argc, char **argv) {
  uint16_t port = 10809;
  uint64_t size = ParseSize("1G", 1ULL << 30);

  if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
  if (argc > 2) size = ParseSize(argv[2], size);

  signal(SIGPIPE, SIG_IGN);

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }
  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  if (listen(listen_fd, 16) < 0) {
    perror("listen");
    return 1;
  }

  std::fprintf(stderr,
               "nbd zero-device server listening on port %u, exporting %llu "
               "bytes of zeros\n",
               port, static_cast<unsigned long long>(size));

  for (;;) {
    int fd = accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) continue;
      perror("accept");
      continue;
    }
    // HandleClient owns fd for the lifetime of the connection and closes it
    // itself; the accept loop must not touch it (it's shared across all
    // threads now that connections are handled in-process rather than in
    // forked children).
    std::thread(HandleClient, fd, size).detach();
  }
}
