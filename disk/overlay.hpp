#pragma once
// Copy-on-write overlay over a read-only backing file.
//
// Guest writes land in a temporary file that stores only the modified blocks,
// plus an in-memory map block -> slot (the block's position in that file).
// Reads start from the backing file and are patched with any overlaid blocks.
// The temp file is unlinked right after creation, so it vanishes when the
// server exits: persistence lasts for the experiment, never beyond it, and the
// backing file is never modified.

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace nbd {

class Overlay {
public:
    static constexpr uint32_t kEio = 5;

    // `block_size`: granularity of copy-on-write (power of two, multiple of 512).
    Overlay(int base_fd, uint64_t base_size, uint64_t block_size, const std::string& dir)
        : base_fd_(base_fd), base_size_(base_size), bs_(block_size) {
        std::string tmpl = dir + "/nbd_overlay_XXXXXX";
        std::vector<char> path(tmpl.begin(), tmpl.end());
        path.push_back('\0');
        fd_ = mkstemp(path.data());
        if (fd_ < 0) throw std::runtime_error("failed to create overlay file in " + dir + ": " + std::strerror(errno));
        unlink(path.data());
    }
    ~Overlay() { if (fd_ >= 0) close(fd_); }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Stores `len` bytes at `offset`. Returns 0 or an NBD errno. Caller has
    // already bounds-checked against the export size.
    uint32_t write(uint64_t offset, const void* data, uint64_t len) {
        std::unique_lock lk(mu_);
        static thread_local std::vector<uint8_t> blk;
        blk.resize(bs_);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint64_t pos = offset;
        const uint64_t end = offset + len;
        while (pos < end) {
            const uint64_t b = pos / bs_, boff = pos % bs_;
            const uint64_t n = std::min<uint64_t>(bs_ - boff, end - pos);
            auto it = map_.find(b);
            const uint64_t slot = it != map_.end() ? it->second : next_slot_;
            const uint8_t* out = p;
            if (n != bs_) {  // partial block: read-modify-write
                if (it != map_.end()) {
                    if (PreadFull(fd_, blk.data(), bs_, slot * bs_) != static_cast<ssize_t>(bs_)) return kEio;
                } else {
                    std::memset(blk.data(), 0, bs_);
                    const uint64_t base_off = b * bs_;
                    if (base_off < base_size_) {
                        size_t want = static_cast<size_t>(std::min<uint64_t>(bs_, base_size_ - base_off));
                        if (PreadFull(base_fd_, blk.data(), want, base_off) < 0) return kEio;
                    }
                }
                std::memcpy(blk.data() + boff, p, n);
                out = blk.data();
            }
            if (!PwriteFull(fd_, out, bs_, slot * bs_)) return kEio;
            if (it == map_.end()) { map_[b] = slot; ++next_slot_; }
            p += n;
            pos += n;
        }
        return 0;
    }

    // Overwrites the parts of buf (the base file's bytes for [offset, offset+len))
    // that fall in overlaid blocks.
    void patch(uint64_t offset, void* buf, uint64_t len) {
        std::shared_lock lk(mu_);
        if (map_.empty()) return;
        uint8_t* p = static_cast<uint8_t*>(buf);
        uint64_t pos = offset;
        const uint64_t end = offset + len;
        while (pos < end) {
            const uint64_t b = pos / bs_, boff = pos % bs_;
            const uint64_t n = std::min<uint64_t>(bs_ - boff, end - pos);
            auto it = map_.find(b);
            if (it != map_.end() &&
                PreadFull(fd_, p, n, it->second * bs_ + boff) != static_cast<ssize_t>(n)) {
                std::fprintf(stderr, "nbd: overlay read failed for block %llu\n",
                             static_cast<unsigned long long>(b));
            }
            p += n;
            pos += n;
        }
    }

    uint64_t blocks() const {
        std::shared_lock lk(mu_);
        return map_.size();
    }
    uint64_t bytes() const { return blocks() * bs_; }

private:
    // Returns bytes read (short only at EOF), or -1 on error.
    static ssize_t PreadFull(int fd, void* buf, size_t len, uint64_t off) {
        uint8_t* p = static_cast<uint8_t*>(buf);
        size_t got = 0;
        while (got < len) {
            ssize_t n = pread(fd, p + got, len - got, static_cast<off_t>(off + got));
            if (n < 0) { if (errno == EINTR) continue; return -1; }
            if (n == 0) break;
            got += static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(got);
    }
    static bool PwriteFull(int fd, const void* buf, size_t len, uint64_t off) {
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        size_t done = 0;
        while (done < len) {
            ssize_t n = pwrite(fd, p + done, len - done, static_cast<off_t>(off + done));
            if (n < 0) { if (errno == EINTR) continue; return false; }
            done += static_cast<size_t>(n);
        }
        return true;
    }

    int base_fd_;
    uint64_t base_size_;
    uint64_t bs_;
    int fd_{-1};
    uint64_t next_slot_{0};
    std::unordered_map<uint64_t, uint64_t> map_;  // block index -> slot in the overlay file
    mutable std::shared_mutex mu_;
};

}  // namespace nbd
