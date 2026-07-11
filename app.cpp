#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include "pipe_pair.hpp"

namespace {

constexpr uint64_t kDefaultRequests = 20000;
constexpr uint64_t kDefaultPageSpan = 1u << 16;

struct Args {
    std::string behavior = "random-read";
    uint64_t seed = 1;
    int r_pipe;
    int w_pipe;
    uint32_t client_id = 0;
    uint64_t requests = kDefaultRequests;
    uint64_t page_span = kDefaultPageSpan;
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
        if (key == "--behavior") {
            a.behavior = require_value(i, argc, argv);
        } else if (key == "--seed") {
            a.seed = std::stoull(require_value(i, argc, argv));
        } else if (key == "--in-pipe") {
            a.r_pipe = std::stoi(require_value(i, argc, argv));
        } else if (key == "--out-pipe") {
            a.w_pipe = std::stoi(require_value(i, argc, argv));
        } else if (key == "--client-id") {
            a.client_id = static_cast<uint32_t>(std::stoul(require_value(i, argc, argv)));
        } else if (key == "--requests") {
            a.requests = std::stoull(require_value(i, argc, argv));
        } else if (key == "--page-span") {
            a.page_span = std::stoull(require_value(i, argc, argv));
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }
    if (a.behavior != "scan" && a.behavior != "random-read") {
        throw std::runtime_error("--behavior must be scan or random-read");
    }
    if (a.page_span == 0) {
        throw std::runtime_error("--page-span must be > 0");
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        PipePair rw_pipe{
            args.r_pipe,
            args.w_pipe,
        };

        std::mt19937_64 rng(args.seed);
        std::uniform_int_distribution<uint64_t> random_page(0, args.page_span - 1);
        uint64_t scan_cursor = 0;

        for (uint64_t seq = 0; seq < args.requests; ++seq) {
            uint64_t page = 0;
            if (args.behavior == "scan") {
                page = scan_cursor;
                scan_cursor = (scan_cursor + 1) % args.page_span;
            } else {
                page = random_page(rng);
            }

            std::ostringstream request;
            request << "REQ " << args.client_id << " " << seq << " " << page << "\n";
            if (rw_pipe.write_all(request.str())) {
                std::cerr << "app: write request failed \n";
                rw_pipe.close();
                return 1;
            }

            std::string response;
            if (rw_pipe.read_line(response)) {
                std::cerr << "app: read response failed\n";
                rw_pipe.close();
                return 1;
            }
            if (response.rfind("RESP ", 0) != 0) {
                std::cerr << "app: malformed response: " << response << "\n";
                rw_pipe.close();
                return 1;
            }
        }

        std::ostringstream done;
        done << "DONE " << args.client_id << "\n";
        (void)rw_pipe.write_all(done.str());

        rw_pipe.close();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "app: " << ex.what() << "\n";
        return 1;
    }
}
