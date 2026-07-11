#pragma once
#include <unistd.h>
#include <iostream>
#include <cstdio>

struct PipePair {
    int r_fd, w_fd;

    inline int open() {
        return pipe((int*)this);
    }
    inline void close() {
        ::close(r_fd);
        ::close(w_fd);
        r_fd = w_fd = -1;
    }

    int write_all(const std::string& payload) const {
        const char* data = payload.data();
        size_t remaining = payload.size();

        while (remaining > 0) {
            ssize_t n = ::write(w_fd, data, remaining);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return 1;
            }
            data += n;
            remaining -= static_cast<size_t>(n);
        }
        return 0;
    }

    // Returns:
    //   0 = got a full line
    //   1 = clean EOF before any bytes, or EOF after partial line if you treat that as failure
    //  -1 = error
    int read_line(std::string& line) const {
        line.clear();

        while (true) {
            char ch = 0;
            ssize_t n = ::read(r_fd, &ch, 1);

            if (n == 0) {
                return line.empty() ? 1 : 0;
            }

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }

            if (ch == '\n') {
                return 0;
            }

            line.push_back(ch);
        }
    }
};