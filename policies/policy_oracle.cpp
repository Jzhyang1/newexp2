#include "policy_api.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace policy {
namespace {

std::uint64_t ParseField(const std::string& field, const char* name,
                         const std::string& path, std::size_t line_number) {
    try {
        std::size_t parsed = 0;
        std::uint64_t value = std::stoull(field, &parsed);
        if (parsed != field.size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid " + std::string(name) + " in " + path +
                                 " at line " + std::to_string(line_number));
    }
}

}  // namespace

OraclePolicy::OraclePolicy(Cache& cache, const std::string& path) : CachePolicy(cache) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("failed to open oracle trace: " + path);

    std::string line;
    std::size_t line_number = 0;
    if (!std::getline(input, line)) {
        throw std::runtime_error("oracle trace is empty: " + path);
    }

    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) continue;
        std::stringstream fields(line);
        std::string worker;
        std::string offset;
        std::string length;
        if (!std::getline(fields, worker, ',') || !std::getline(fields, offset, ',') ||
            !std::getline(fields, length, ',')) {
            throw std::runtime_error("malformed oracle trace row in " + path +
                                     " at line " + std::to_string(line_number + 1));
        }

        const std::uint64_t worker_id = ParseField(worker, "worker_id", path, line_number + 1);
        const std::uint64_t byte_offset = ParseField(offset, "offset", path, line_number + 1);
        const std::uint64_t byte_length = ParseField(length, "length", path, line_number + 1);
        if (byte_offset % 512 != 0 || byte_length == 0 || byte_length % 512 != 0 ||
            byte_offset / 512 > UINT32_MAX || byte_length / 512 > UINT32_MAX) {
            throw std::runtime_error("oracle trace row is not a valid sector-aligned request in " +
                                     path + " at line " + std::to_string(line_number + 1));
        }
        requests_[worker_id].push_back(Request{
            static_cast<std::uint32_t>(byte_offset / 512),
            static_cast<std::uint32_t>(byte_length / 512)});
    }
}

void OraclePolicy::on_prefetch_request(std::uint64_t context, std::uint32_t block_offset,
                                       std::uint32_t block_length, PrefetchRequest& request) {
    auto trace = requests_.find(context);
    if (trace == requests_.end()) return;

    std::size_t& cursor = next_request_[context];
    if (cursor >= trace->second.size()) return;

    // The live workload can diverge from the captured trace during boot or
    // across runs. Align to the closest request in a bounded lookahead window
    // instead of requiring byte-for-byte replay of the capture.
    constexpr std::size_t kLookahead = (1 << 12);
    const std::size_t end = std::min(cursor + kLookahead, trace->second.size());
    std::size_t best = cursor;
    std::uint64_t best_distance = UINT64_MAX;
    for (std::size_t candidate = cursor; candidate < end; ++candidate) {
        const Request& observed = trace->second[candidate];
        const std::uint64_t offset_distance = observed.block_offset > block_offset
            ? observed.block_offset - block_offset : block_offset - observed.block_offset;
        const std::uint64_t length_distance = observed.block_length > block_length
            ? observed.block_length - block_length : block_length - observed.block_length;
        const std::uint64_t distance = offset_distance + length_distance;
        if (distance < best_distance) {
            best = candidate;
            best_distance = distance;
        }
    }

    if (best + 1 >= trace->second.size()) return;

    const Request& next = trace->second[best + 1];
    request.fetch_count = 1;
    request.fetch_ranges[0] = FetchRange{next.block_offset, next.block_length};

    if (best_distance == 0) {
        // only advance on confirmed match
        cursor = best + 1;
    }
}

}  // namespace policy