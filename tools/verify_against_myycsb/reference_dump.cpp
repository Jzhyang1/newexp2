// Same purpose/CLI as our_dump.cpp, built against the pinned My-YCSB commit's real
// core/workload.h + core/workload.cpp (fetched and lightly patched by run.sh -- see
// that script for exactly what's patched and why). Numeric keys are recovered from
// My-YCSB's zero-padded decimal key_buffer with atoll().
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "workload.h"

namespace {

struct Args {
    std::string workload;
    long nr_entry = 0;
    long nr_op = 0;
    unsigned int seed = 0;
    double alpha = 0.99;
    double scan_ratio = 0.0;
    long scan_length = 100;
    double read_ratio = 0.5;
    long value_size = 8;
    long key_size = 16;
};

std::string require_value(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
    }
    ++i;
    return argv[i];
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--workload") a.workload = require_value(i, argc, argv);
        else if (key == "--nr-entry") a.nr_entry = std::stol(require_value(i, argc, argv));
        else if (key == "--nr-op") a.nr_op = std::stol(require_value(i, argc, argv));
        else if (key == "--seed") a.seed = static_cast<unsigned int>(std::stoul(require_value(i, argc, argv)));
        else if (key == "--alpha") a.alpha = std::stod(require_value(i, argc, argv));
        else if (key == "--scan-ratio") a.scan_ratio = std::stod(require_value(i, argc, argv));
        else if (key == "--scan-length") a.scan_length = std::stol(require_value(i, argc, argv));
        else if (key == "--read-ratio") a.read_ratio = std::stod(require_value(i, argc, argv));
        else if (key == "--value-size") a.value_size = std::stol(require_value(i, argc, argv));
        else if (key == "--key-size") a.key_size = std::stol(require_value(i, argc, argv));
        else { std::fprintf(stderr, "unknown arg: %s\n", key.c_str()); std::exit(2); }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    char* key_buffer = new char[a.key_size];
    char* value_buffer = new char[a.value_size];

    Workload* w = nullptr;
    OpProportion op_prop{0, 0, static_cast<float>(1.0 - a.scan_ratio), static_cast<float>(a.scan_ratio), 0};

    if (a.workload == "zipfian") {
        w = new ZipfianWorkload(a.key_size, a.value_size, a.scan_length, a.nr_entry, a.nr_op, op_prop, a.alpha, a.seed);
    } else if (a.workload == "uniform") {
        w = new UniformWorkload(a.key_size, a.value_size, a.scan_length, a.nr_entry, a.nr_op, op_prop, a.seed);
    } else if (a.workload == "latest") {
        w = new LatestWorkload(a.key_size, a.value_size, a.nr_entry, a.nr_op, a.read_ratio, a.alpha, a.seed);
    } else {
        std::fprintf(stderr, "unknown workload: %s\n", a.workload.c_str());
        return 2;
    }

    while (w->has_next_op()) {
        Operation op{};
        op.key_buffer = key_buffer;
        op.value_buffer = value_buffer;
        w->next_op(&op);
        unsigned long long key = std::strtoull(op.key_buffer, nullptr, 10);
        if (op.type == SCAN) {
            std::printf("SCAN %llu %ld\n", key, op.scan_length);
        } else {
            std::printf("%s %llu\n", operation_type_name[op.type], key);
        }
    }
    return 0;
}
