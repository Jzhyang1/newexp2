#include "policy_api.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace policy {
namespace {

struct Pair {
    std::uint64_t offset;
    std::uint64_t length;
};

std::string JsonPair(Pair pair) {
    return "[" + std::to_string(pair.offset) + "," + std::to_string(pair.length) + "]";
}

std::vector<Pair> ParsePairs(const std::string& response) {
    std::vector<Pair> pairs;
    std::size_t cursor = 0;
    const auto skip_space = [&] {
        while (cursor < response.size() &&
               std::isspace(static_cast<unsigned char>(response[cursor]))) ++cursor;
    };
    const auto take = [&](char token) {
        skip_space();
        if (cursor == response.size() || response[cursor] != token) return false;
        ++cursor;
        return true;
    };
    const auto number = [&](std::uint64_t& value) {
        skip_space();
        const char* begin = response.data() + cursor;
        const auto result = std::from_chars(begin, response.data() + response.size(), value);
        if (result.ec != std::errc{}) return false;
        cursor += result.ptr - begin;
        return true;
    };
    const auto invalid = [response] {
        std::fprintf(stderr, "model prefetcher: invalid prediction response %s; expected [[offset,length],...] with uint64 offsets and positive uint64 lengths\n", response.data());
        return std::vector<Pair>{};
    };

    if (!take('[')) return invalid();
    if (!take(']')) {
        do {
            Pair pair{};
            if (!take('[') || !number(pair.offset) || !take(',') ||
                !number(pair.length) || pair.length == 0 || !take(']')) return invalid();
            pairs.push_back(pair);
        } while (take(','));
        if (!take(']')) return invalid();
    }
    skip_space();
    if (cursor != response.size()) return invalid();
    return pairs;
}

}  // namespace

struct ModelPolicy::ModelProcess {
    pid_t pid = -1;
    FILE* input = nullptr;
    FILE* output = nullptr;

    ModelProcess(const std::string& module, const std::string& type,
                 std::size_t min_len, const std::string& training) {
        int to_child[2];
        int from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0) {
            throw std::runtime_error(std::string("failed to create model prefetcher pipes: ") +
                                     std::strerror(errno));
        }
        pid = fork();
        if (pid < 0) {
            throw std::runtime_error(std::string("failed to launch model prefetcher: ") +
                                     std::strerror(errno));
        }
        if (pid == 0) {
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            close(to_child[0]); close(to_child[1]);
            close(from_child[0]); close(from_child[1]);
            const std::string min_len_arg = std::to_string(min_len);
            execlp("python3", "python3", "-m", module.c_str(),
                   "--type", type.c_str(), "--min-len", min_len_arg.c_str(),
                   "--training", training.c_str(), static_cast<char*>(nullptr));
            std::fprintf(stderr, "model prefetcher exec failed: %s\n", std::strerror(errno));
            _exit(127);
        }
        close(to_child[0]);
        close(from_child[1]);
        input = fdopen(to_child[1], "w");
        output = fdopen(from_child[0], "r");
        if (!input || !output) throw std::runtime_error("failed to open model prefetcher pipes");
        setvbuf(input, nullptr, _IOLBF, 0);
    }

    ~ModelProcess() {
        if (input) {
            std::fputs("{\"type\":\"kill\"}\n", input);
            std::fflush(input);
            std::fclose(input);
        }
        if (output) std::fclose(output);
        if (pid > 0) {
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == 0) {
                kill(pid, SIGTERM);
                waitpid(pid, &status, 0);
            }
        }
    }
};

ModelPolicy::ModelPolicy(Cache& cache, const std::string& module, const std::string& type,
                         std::size_t min_len, const std::string& training)
        : CachePolicy(cache),
            model_(std::make_unique<ModelProcess>(module, type, min_len, training)),
            min_len_(min_len) {}

ModelPolicy::~ModelPolicy() = default;

void ModelPolicy::on_access(std::uint64_t context, std::uint64_t page) {
    (void)context;
    (void)page;
}

void ModelPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t block_offset,
                                      std::uint64_t block_length, PrefetchRequest& request) {
    std::lock_guard lock(mutex_);
    request.fetch_count = 0;
    const Pair actual{block_offset, block_length};
    auto& history = history_[context];
    auto& pending = pending_[context];
    if (!pending.empty()) {
        std::fprintf(model_->input, "{\"type\":\"put\",\"predicted_accesses\":[");
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (index) std::fputc(',', model_->input);
            std::fputs(JsonPair(Pair{pending[index].first, pending[index].second}).c_str(), model_->input);
        }
        std::fprintf(model_->input, "],\"actual_accesses\":%s}\n", JsonPair(actual).c_str());
        std::fflush(model_->input);
        char put_response[256];
        if (!std::fgets(put_response, sizeof(put_response), model_->output)) return;
        pending.clear();
    }

    std::fprintf(model_->input, "{\"type\":\"get\",\"prev_accesses\":[");
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (index) std::fputc(',', model_->input);
        std::fputs(JsonPair(Pair{history[index].first, history[index].second}).c_str(), model_->input);
    }
    std::fprintf(model_->input, "],\"next_n\":16}\n");
    std::fflush(model_->input);
    char buffer[8192];
    if (!std::fgets(buffer, sizeof(buffer), model_->output)) return;
    const std::vector<Pair> predictions = ParsePairs(buffer);
    for (const Pair pair : predictions) {
        if (request.fetch_count == MAX_PREFETCH_PAGES) break;
        request.fetch_ranges[request.fetch_count++] = FetchRange{pair.offset, pair.length};
    }
    pending.reserve(predictions.size());
    for (const Pair pair : predictions) pending.emplace_back(pair.offset, pair.length);
    history.push_back({actual.offset, actual.length});
    while (history.size() > min_len_) history.pop_front();
}

}  // namespace policy
