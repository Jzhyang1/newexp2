// lapp: a twin of app.cpp for verifying the lhook/ldat/lrunner pipeline.
//
// app.cpp drives a Workload and reports each op to dat.cpp as a "REQ ..."
// line over a pipe, then blocks on the "RESP ..." line. lapp drives the
// *exact same* Workload machinery (build_workload below is a straight copy
// of app.cpp's) but, instead of describing an access, actually performs it:
// it mmap()s a real file and reads one byte per page the workload names.
//
// Run standalone, that's just a slow way to read a file -- the kernel
// services every fault itself, same as any other program. Run under lrunner
// (which sets LD_PRELOAD=liblhook.so before exec'ing this binary, same as it
// would for a real unmodified app), --file being under lrunner's
// --watch-prefix means lhook.cpp intercepts the mmap() below, and every
// fault gets routed through ldat's real policy::Cache-driven fault handling.
// That's what makes lapp useful as a verification tool: it's a known-good,
// fully-controlled access pattern (the same Zipfian/scan/trace/... generators
// used throughout this project) to confirm the uffd plumbing actually
// reproduces the intended hit/miss/eviction behavior before pointing it at a
// real, unmodified application. Unlike app.cpp, lapp always terminates on
// its own once its `--requests` ops are done -- it doesn't require an
// external timeout to bound how long lrunner waits for it.
//
// No userfaultfd/uffd code lives here at all -- lapp only ever calls plain
// mmap()/read -- so, unlike lhook.cpp/ldat.cpp, this file has no Linux-only
// dependency and builds anywhere.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include "workloads/workload.h"

namespace {

constexpr uint64_t kDefaultRequests = 20000;
constexpr uint64_t kDefaultPageSpan = 1u << 16;
constexpr std::size_t kPageBytes = 4096;

constexpr double kDefaultZipfianAlpha = 0.99;
constexpr double kDefaultReadRatio = 0.5;

// Matches YCSB's default maxscanlength (see My-YCSB's *.yaml `scan_length: 100`).
constexpr uint64_t kDefaultScanLength = 100;
constexpr double kDefaultScanRatio = 0.0;

// See app.cpp's identical constant for why every workload needs a real
// (if unused) value buffer.
constexpr long kScratchValueSize = 16;

constexpr double kDefaultMarkovRelativeWeight = 0.5;

struct Args {
    std::string behavior = "random-read";
    uint64_t seed = 1;
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
    std::string file;  // replaces app.cpp's --in-pipe/--out-pipe: the real file to mmap
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
        } else if (key == "--file") {
            a.file = require_value(i, argc, argv);
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
    if (a.file.empty()) {
        throw std::runtime_error("--file is required (the path lapp mmap()s and reads)");
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

// Verbatim copy of app.cpp's build_workload -- see there for the rationale
// behind each behavior's construction. Kept identical on purpose: lapp is
// only a useful verification tool if it can generate exactly the access
// patterns app.cpp would have sent to dat.cpp over a pipe.
Workload* build_workload(const std::string& behavior, const Args& args, uint64_t nr_op) {
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

// Opens (creating if needed) and grows args.file to at least `needed_bytes`,
// so mmap()ing that whole range never reads past EOF -- without this, a
// page beyond the file's real length would SIGBUS on touch when lapp runs
// standalone (no lhook intercepting the mmap to redirect it to anonymous
// memory). Never shrinks an existing, already-large-enough file.
int open_and_size_file(const std::string& path, std::uint64_t needed_bytes) {
    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw std::runtime_error("lapp: failed to open --file: " + path);
    }
    struct stat st{};
    if (fstat(fd, &st) != 0) {
        close(fd);
        throw std::runtime_error("lapp: fstat failed on --file: " + path);
    }
    if (static_cast<std::uint64_t>(st.st_size) < needed_bytes) {
        if (ftruncate(fd, static_cast<off_t>(needed_bytes)) != 0) {
            close(fd);
            throw std::runtime_error("lapp: ftruncate failed on --file: " + path);
        }
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);

        const std::uint64_t region_bytes = args.page_span * static_cast<std::uint64_t>(kPageBytes);
        int fd = open_and_size_file(args.file, region_bytes);

        // MAP_SHARED, not MAP_PRIVATE: real buffer-pool-style mmaps (and the
        // MAP_SHARED code path in lhook.cpp) are the more realistic and more
        // useful thing to verify than a private mapping no real app would use
        // for this purpose.
        void* region = mmap(nullptr, region_bytes, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);  // the mapping keeps the file open; the fd itself isn't needed after mmap()
        if (region == MAP_FAILED) {
            std::perror("lapp: mmap");
            return 1;
        }
        const char* base = static_cast<const char*>(region);

        Workload* workload = build_workload(args.behavior, args, args.requests);

        char value_scratch[kScratchValueSize];
        auto start = std::chrono::steady_clock::now();
        std::uint64_t pages_touched = 0;

        for (uint64_t seq = 0; workload->has_next_op();) {
            Operation op{};
            op.value_buffer = value_scratch;
            workload->next_op(&op);

            uint64_t span = (op.type == SCAN && op.scan_length > 0) ? static_cast<uint64_t>(op.scan_length) : 1;
            for (uint64_t j = 0; j < span; ++j, ++seq) {
                // Unlike app.cpp (which only wraps op.key by page_span for
                // multi-page scans, since a bare page *number* has no upper
                // bound to respect), lapp always wraps: ScanWorkload draws
                // keys from [0, requests) rather than [0, page_span), and
                // op.key here indexes real mmap'd memory of exactly
                // page_span pages, so an unwrapped out-of-range key would
                // read past the mapping instead of just being an abstract
                // cache key.
                uint64_t page = (op.key + j) % args.page_span;

                // The actual access: a real memory read. The first touch of
                // any page faults -- serviced by the kernel directly when
                // standalone, or by ldat (via lhook's uffd redirection) when
                // run under lrunner against a watched --file.
                volatile const char* p = base + page * kPageBytes;
                volatile char v = *p;
                (void)v;
                ++pages_touched;
            }
        }

        double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::chrono::steady_clock::now() - start)
                              .count();

        munmap(region, region_bytes);

        std::cout << "lapp: client=" << args.client_id
                  << " behavior=" << args.behavior
                  << " file=" << args.file
                  << " pages_touched=" << pages_touched
                  << " elapsed_seconds=" << elapsed
                  << " pages_per_sec=" << (elapsed > 0.0 ? pages_touched / elapsed : 0.0)
                  << std::endl;

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "lapp: " << ex.what() << "\n";
        return 1;
    }
}
