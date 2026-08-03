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
constexpr double kDefaultReadRatio = 0.5;

// Matches YCSB's default maxscanlength (see My-YCSB's *.yaml `scan_length: 100`).
constexpr uint64_t kDefaultScanLength = 100;
constexpr double kDefaultScanRatio = 0.0;

// `dat` only ever sees a page number, never op.value_buffer -- but My-YCSB's
// generate_value_string() writes into it for UPDATE/INSERT/READ_MODIFY_WRITE ops
// (LatestWorkload's INSERT/UPDATE branch is intrinsic to its algorithm and can't be
// disabled via OpProportion). Give every workload a real backing buffer so that path
// can't write through a null/undersized pointer.
constexpr long kScratchValueSize = 16;

constexpr double kDefaultMarkovRelativeWeight = 0.99;

struct Args {
    std::string behavior = "random-read";
    uint64_t seed = 1;
    int r_pipe;
    int w_pipe;
    uint32_t client_id = 0;
    uint64_t requests = kDefaultRequests;
    uint64_t page_span = kDefaultPageSpan;
    double zipfian_alpha = kDefaultZipfianAlpha;
    double read_ratio = kDefaultReadRatio;
    uint64_t scan_length = kDefaultScanLength;
    double scan_ratio = kDefaultScanRatio;
    std::string trace_file;
    std::string markov_base;
    uint64_t markov_samples = 0;
    double markov_relative_weight = kDefaultMarkovRelativeWeight;
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
        } else if (key == "--read-ratio") {
            a.read_ratio = std::stod(require_value(i, argc, argv));
        } else if (key == "--scan-length") {
            a.scan_length = std::stoull(require_value(i, argc, argv));
        } else if (key == "--scan-ratio") {
            a.scan_ratio = std::stod(require_value(i, argc, argv));
        } else if (key == "--trace-file") {
            a.trace_file = require_value(i, argc, argv);
        } else if (key == "--markov-base") {
            a.markov_base = require_value(i, argc, argv);
        } else if (key == "--markov-samples") {
            a.markov_samples = std::stoull(require_value(i, argc, argv));
        } else if (key == "--markov-relative-weight") {
            a.markov_relative_weight = std::stod(require_value(i, argc, argv));
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }
    if (a.page_span == 0) {
        throw std::runtime_error("--page-span must be > 0");
    }
    if (a.read_ratio < 0.0 || a.read_ratio > 1.0) {
        throw std::runtime_error("--read-ratio must be between 0 and 1");
    }
    if (a.scan_length == 0) {
        throw std::runtime_error("--scan-length must be > 0");
    }
    if (a.scan_ratio < 0.0 || a.scan_ratio > 1.0) {
        throw std::runtime_error("--scan-ratio must be between 0 and 1");
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
    if (a.behavior == "markov") {
        if (a.markov_base.empty() || a.markov_base == "markov") {
            throw std::runtime_error("--markov-base is required (and must not be 'markov') when --behavior is markov");
        }
        if (a.markov_samples < 3) {
            throw std::runtime_error("--markov-samples must be >= 3");
        }
        if (a.markov_relative_weight < 0.0 || a.markov_relative_weight > 1.0) {
            throw std::runtime_error("--markov-relative-weight must be between 0 and 1");
        }
    }
    return a;
}

// Builds the Workload for a given behavior name, emitting exactly `nr_op`
// ops. Factored out of main() so MarkovWorkload can build its training
// source with a different op count (`--markov-samples`) than the top-level
// `--requests`, while reusing every other already-parsed flag.
Workload* build_workload(const std::string& behavior, const Args& args, uint64_t nr_op) {
    // See the comment on kScratchValueSize above for why op_prop only ever
    // splits READ/SCAN.
    OpProportion op_prop{0, 0, static_cast<float>(1.0 - args.scan_ratio), static_cast<float>(args.scan_ratio), 0};

    if (behavior == "scan") {
        return new ScanWorkload(static_cast<long>(nr_op), 0l, kScratchValueSize, args.seed);
    } else if (behavior == "zipfian") {
        return new ZipfianWorkload(
            kScratchValueSize, static_cast<long>(args.scan_length), args.page_span, static_cast<long>(nr_op), op_prop, args.zipfian_alpha, args.seed
        );
    } else if (behavior == "trace") {
        return new ReaderTraceWorkload(args.trace_file, kScratchValueSize);
    } else if (behavior == "latest") {
        return new LatestWorkload(
            kScratchValueSize, args.page_span, static_cast<long>(nr_op), args.read_ratio, args.zipfian_alpha, args.seed
        );
    } else if (behavior == "markov") {
        Workload* source = build_workload(args.markov_base, args, args.markov_samples);
        return new MarkovWorkload(
            kScratchValueSize, source, static_cast<long>(args.markov_samples), static_cast<long>(nr_op),
            args.page_span, args.markov_relative_weight, args.seed
        );
    } else {
        return new UniformWorkload(
            kScratchValueSize, static_cast<long>(args.scan_length), args.page_span, static_cast<long>(nr_op), op_prop, args.seed
        );
    }
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

        // Non-SCAN op types (UPDATE/INSERT/READ_MODIFY_WRITE) are never requested here:
        // `dat` has no op-type-aware backend (every request is just a page fetch), and
        // My-YCSB's own key generation is identical across UPDATE/INSERT/READ/RMW (only
        // SCAN differs, by walking scan-length consecutive keys). We also never allocate
        // op.value_buffer, and My-YCSB's generate_value_string() would write out of bounds
        // for value_size=0, so those branches must stay unreachable. Reproducing a target
        // op mix therefore only requires a READ/SCAN split, via --scan-ratio.
        Workload* workload = build_workload(args.behavior, args, args.requests);

        char value_scratch[kScratchValueSize];
        for (uint64_t seq = 0; workload->has_next_op(); ) {
            Operation op{};
            op.value_buffer = value_scratch;
            workload->next_op(&op);

            // A SCAN op walks `scan_length` consecutive pages from the chosen start key
            // (mirrors My-YCSB's do_scan(), which reads scan_length consecutive keys from
            // an iterator seeded at op.key). Non-scan ops (and ScanWorkload's own linear
            // walk, which never sets scan_length) are a single-page request.
            uint64_t span = (op.type == SCAN && op.scan_length > 0) ? static_cast<uint64_t>(op.scan_length) : 1;
            for (uint64_t j = 0; j < span; ++j, ++seq) {
                // Only wrap when actually walking a multi-page scan chain; single-page
                // ops (scan/trace/latest/random-read, or zipfian with scan-ratio=0) pass
                // the workload's key through unchanged, preserving prior behavior.
                uint64_t page = (span > 1) ? (op.key + j) % args.page_span : op.key;

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