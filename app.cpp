#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "pipe_pair.hpp"
#include "workloads/workload.h"

namespace {

constexpr uint64_t kDefaultRequests = 20000;
constexpr uint64_t kDefaultPageSpan = 1u << 16;

constexpr double kDefaultZipfianAlpha = 0.99;

struct Args {
    std::string behavior = "random-read";
    uint64_t seed = 1;
    int r_pipe;
    int w_pipe;
    uint32_t client_id = 0;
    uint64_t requests = kDefaultRequests;
    uint64_t page_span = kDefaultPageSpan;
    double zipfian_alpha = kDefaultZipfianAlpha;
    std::string trace_file;
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
        } else if (key == "--zipfian-alpha") {
            a.zipfian_alpha = std::stod(require_value(i, argc, argv));
        } else if (key == "--trace-file") {
            a.trace_file = require_value(i, argc, argv);
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }
    if (a.behavior != "scan" && a.behavior != "random-read" && a.behavior != "zipfian" && a.behavior != "trace") {
        throw std::runtime_error("--behavior must be scan, random-read, zipfian, or trace");
    }
    if (a.page_span == 0) {
        throw std::runtime_error("--page-span must be > 0");
    }
    if (a.behavior == "trace") {
        if (a.trace_file.empty()) {
            throw std::runtime_error("--trace-file is required when --behavior is trace");
        }
        // Fail before the pipe to `dat` is set up: once it's open, an app that
        // exits early leaves `dat` blocked reading from a peer that never writes.
        if (!std::ifstream(a.trace_file).is_open()) {
            throw std::runtime_error("failed to open trace file: " + a.trace_file);
        }
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

        Workload* workload;
        if (args.behavior == "scan") {
            workload = new ScanWorkload(args.requests, 0l, 0l, args.seed);
        } else if (args.behavior == "zipfian") {
            workload = new ZipfianWorkload(
                0l, 0l, args.page_span, args.requests, OpProportion{0,0,1,0,0}, args.zipfian_alpha, args.seed
            );
        } else if (args.behavior == "trace") {
            workload = new ReaderTraceWorkload(args.trace_file, 0l);
        } else {
            workload = new UniformWorkload(
                0l, 1l, args.page_span, args.requests, OpProportion{0,0,1,0,0}, args.seed
            );
        }

        for (uint64_t seq = 0; workload->has_next_op(); ++seq) {
            Operation op;
            workload->next_op(&op);
            uint64_t page = op.key;

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