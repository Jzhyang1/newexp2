#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "pipe_pair.hpp"

namespace {

struct Args {
    std::uint64_t seed = 1;
    std::string evict_policy = "lru";
    std::string prefetch_policy = "none";
    std::size_t capacity = 4096;
    std::uint64_t miss_delay_ns = 300000;
    std::uint64_t hit_delay_ns = 30000;
    std::string file_data;
    std::string config_file;
    std::uint64_t warmup_period = 0;
    std::string log_file = "./logs/results.csv";
    std::uint64_t requests = 20000;
    std::uint64_t page_span = (1u << 16);
};

struct ParsedStats {
    bool ok = false;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    double hit_ratio = 0.0;
    double avg_latency_ns = 0.0;
    double runtime_seconds = 0.0;
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
        if (key == "--seed") {
            a.seed = std::stoull(require_value(i, argc, argv));
        } else if (key == "--evict-policy") {
            a.evict_policy = require_value(i, argc, argv);
        } else if (key == "--prefetch-policy") {
            a.prefetch_policy = require_value(i, argc, argv);
        } else if (key == "--capacity") {
            a.capacity = static_cast<std::size_t>(std::stoull(require_value(i, argc, argv)));
        } else if (key == "--miss-delay") {
            a.miss_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--hit-delay") {
            a.hit_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--file-data") {
            a.file_data = require_value(i, argc, argv);
        } else if (key == "--config") {
            a.config_file = require_value(i, argc, argv);
        } else if (key == "--warmup-period") {
            a.warmup_period = std::stoull(require_value(i, argc, argv));
        } else if (key == "--log") {
            a.log_file = require_value(i, argc, argv);
        } else if (key == "--requests") {
            a.requests = std::stoull(require_value(i, argc, argv));
        } else if (key == "--page-span") {
            a.page_span = std::stoull(require_value(i, argc, argv));
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }

    if (a.file_data.empty()) {
        throw std::runtime_error("--file-data is required");
    }
    if (a.config_file.empty()) {
        throw std::runtime_error("--config is required");
    }
    if (a.page_span == 0) {
        throw std::runtime_error("--page-span must be > 0");
    }

    return a;
}

struct AppSpec {
    std::string behavior;
    std::vector<std::string> args;  // behavior-specific: trace path, zipfian alpha, or latest read ratio
};

std::vector<AppSpec> load_app_behaviors(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Failed to open config file: " + path);
    }
    std::vector<AppSpec> behaviors;
    std::string line;
    while (std::getline(in, line)) {
        // Trim simple whitespace from edges if present
        std::size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue; // Skip empty lines
        if (line[start] == '#') continue;         // Allow comments

        std::size_t end = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(start, end - start + 1);

        std::istringstream tokens(trimmed);
        AppSpec spec;
        tokens >> spec.behavior;
        std::string arg;
        while (tokens >> arg) {
            spec.args.push_back(arg);
        }

        if (spec.behavior != "scan" && spec.behavior != "random-read" &&
            spec.behavior != "zipfian" && spec.behavior != "trace" && spec.behavior != "latest") {
            throw std::runtime_error("Invalid behavior in config file: " + spec.behavior);
        }
        if (spec.behavior == "trace" && spec.args.size() != 1) {
            throw std::runtime_error("trace behavior requires exactly one arg (trace file path): " + trimmed);
        }
        if (spec.behavior == "zipfian" && spec.args.size() > 1) {
            throw std::runtime_error("zipfian behavior accepts at most one arg (alpha): " + trimmed);
        }
        if (spec.behavior == "latest" && spec.args.size() > 1) {
            throw std::runtime_error("latest behavior accepts at most one arg (read ratio): " + trimmed);
        }
        if ((spec.behavior == "scan" || spec.behavior == "random-read") && !spec.args.empty()) {
            throw std::runtime_error(spec.behavior + " behavior does not accept args: " + trimmed);
        }
        if (spec.behavior == "zipfian" && spec.args.size() == 1) {
            try {
                std::size_t consumed = 0;
                std::stod(spec.args[0], &consumed);
                if (consumed != spec.args[0].size()) {
                    throw std::invalid_argument("trailing characters");
                }
            } catch (const std::exception&) {
                throw std::runtime_error("zipfian alpha is not a valid number: " + spec.args[0]);
            }
        }
        if (spec.behavior == "latest" && spec.args.size() == 1) {
            try {
                std::size_t consumed = 0;
                double ratio = std::stod(spec.args[0], &consumed);
                if (consumed != spec.args[0].size() || ratio < 0.0 || ratio > 1.0) {
                    throw std::invalid_argument("out of range");
                }
            } catch (const std::exception&) {
                throw std::runtime_error("latest read ratio is not a valid number in [0, 1]: " + spec.args[0]);
            }
        }

        behaviors.push_back(std::move(spec));
    }

    if (behaviors.empty()) {
        throw std::runtime_error("Config file contained no valid app behaviors");
    }
    return behaviors;
}

std::string join_csv(const std::vector<int>& v) {
    std::ostringstream out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        out << v[i];
    }
    return out.str();
}

pid_t spawn_child(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& s : args) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        execv(argv[0], argv.data());
        std::perror("execv");
        _exit(127);
    }
    return pid;
}

pid_t spawn_dat_with_stdout_pipe(const std::vector<std::string>& args, int* read_fd) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        throw std::runtime_error("pipe() failed for dat stdout");
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& s : args) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error("fork failed for dat");
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execv(argv[0], argv.data());
        std::perror("execv dat");
        _exit(127);
    }

    close(pipefd[1]);
    *read_fd = pipefd[0];
    return pid;
}

std::string read_all_fd(int fd) {
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

std::string extract_value(const std::string& line, const std::string& key) {
    std::string needle = key + "=";
    std::size_t pos = line.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    pos += needle.size();
    std::size_t end = line.find(' ', pos);
    if (end == std::string::npos) {
        end = line.size();
    }
    return line.substr(pos, end - pos);
}

ParsedStats parse_stats_line(const std::string& all_output) {
    ParsedStats s;
    std::istringstream in(all_output);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("STATS ", 0) != 0) {
            continue;
        }
        s.ok = true;
        s.hits = std::stoull(extract_value(line, "hits"));
        s.misses = std::stoull(extract_value(line, "misses"));
        s.evictions = std::stoull(extract_value(line, "evictions"));
        s.bytes_read = std::stoull(extract_value(line, "bytes_read"));
        s.bytes_written = std::stoull(extract_value(line, "bytes_written"));
        s.hit_ratio = std::stod(extract_value(line, "hit_ratio"));
        s.avg_latency_ns = std::stod(extract_value(line, "avg_latency_ns"));
        s.runtime_seconds = std::stod(extract_value(line, "runtime_seconds"));
        break;
    }
    return s;
}

int wait_exit_code(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

std::string get_hostname() {
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) {
        return "unknown-host";
    }
    host[sizeof(host) - 1] = '\0';
    return host;
}

std::string get_git_commit() {
    FILE* fp = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!fp) {
        return "unknown";
    }
    char buf[128];
    std::string out;
    if (fgets(buf, sizeof(buf), fp) != nullptr) {
        out = buf;
    }
    (void)pclose(fp);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (out.empty()) {
        return "unknown";
    }
    return out;
}

void ensure_parent_dir(const std::string& path) {
    std::filesystem::path p(path);
    auto parent = p.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

void append_log(const Args& args, std::size_t app_count, const ParsedStats& s) {
    ensure_parent_dir(args.log_file);

    bool need_header = true;
    {
        std::ifstream in(args.log_file);
        need_header = !in.good() || in.peek() == std::ifstream::traits_type::eof();
    }

    std::ofstream out(args.log_file, std::ios::app);
    if (!out) {
        throw std::runtime_error("Failed to open log file: " + args.log_file);
    }

    if (need_header) {
        out << "timestamp,machine,commit_hash,n,seed,evict_policy,prefetch_policy,capacity,miss_delay_ns,hit_delay_ns,file_data,"
               "config_file,warmup_period,requests,page_span,hits,misses,"
               "hit_ratio,evictions,bytes_read,bytes_written,avg_latency_ns,runtime_seconds\n";
    }

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    out << ts << ','
        << get_hostname() << ','
        << get_git_commit() << ','
        << app_count << ','
        << args.seed << ','
        << args.evict_policy << ','
        << args.prefetch_policy << ','
        << args.capacity << ','
        << args.miss_delay_ns << ','
        << args.hit_delay_ns << ','
        << args.file_data << ','
        << args.config_file << ','
        << args.warmup_period << ','
        << args.requests << ','
        << args.page_span << ','
        << s.hits << ','
        << s.misses << ','
        << s.hit_ratio << ','
        << s.evictions << ','
        << s.bytes_read << ','
        << s.bytes_written << ','
        << s.avg_latency_ns << ','
        << s.runtime_seconds
        << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        std::vector<AppSpec> app_behaviors = load_app_behaviors(args.config_file);
        std::size_t n = app_behaviors.size();

        std::filesystem::path run_dir = std::filesystem::path("./build") /
                                        ("run_" + std::to_string(::getpid()) + "_" + std::to_string(args.seed));
        std::filesystem::create_directories(run_dir);

        std::vector<PipePair> r_pipes(n);
        std::vector<PipePair> w_pipes(n);
        std::vector<int> dat_r_pipes(n);
        std::vector<int> dat_w_pipes(n);
        std::vector<int> app_r_pipes(n);
        std::vector<int> app_w_pipes(n);

        for (std::size_t i = 0; i < n; ++i) {
            if (r_pipes[i].open() || w_pipes[i].open()) {
                throw std::runtime_error("mkfifo failed");
            }
            dat_r_pipes[i] = r_pipes[i].w_fd;
            dat_w_pipes[i] = w_pipes[i].r_fd;
            app_r_pipes[i] = r_pipes[i].r_fd;
            app_w_pipes[i] = w_pipes[i].w_fd;
        }

        std::vector<std::string> dat_cmd = {
            "./bin/dat",
            "--evict-policy", args.evict_policy,
            "--prefetch-policy", args.prefetch_policy,
            "--capacity", std::to_string(args.capacity),
            "--in-pipes", join_csv(dat_r_pipes),
            "--out-pipes", join_csv(dat_w_pipes),
            "--miss-delay", std::to_string(args.miss_delay_ns),
            "--hit-delay", std::to_string(args.hit_delay_ns),
            "--file-data", args.file_data,
            "--warmup-period", std::to_string(args.warmup_period),
        };

        int dat_stdout_fd = -1;
        pid_t dat_pid = spawn_dat_with_stdout_pipe(dat_cmd, &dat_stdout_fd);

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        std::vector<pid_t> app_pids;
        app_pids.reserve(n);

        for (std::size_t i = 0; i < n; ++i) {
            const AppSpec& spec = app_behaviors[i];
            std::vector<std::string> app_cmd = {
                "./bin/app",
                "--behavior", spec.behavior,
                "--seed", std::to_string(args.seed + static_cast<std::uint64_t>(i)),
                "--in-pipe", std::to_string(app_r_pipes[i]),
                "--out-pipe", std::to_string(app_w_pipes[i]),
                "--client-id", std::to_string(i),
                "--requests", std::to_string(args.requests),
                "--page-span", std::to_string(args.page_span),
            };
            if (spec.behavior == "trace") {
                app_cmd.push_back("--trace-file");
                app_cmd.push_back(spec.args[0]);
            } else if (spec.behavior == "zipfian" && !spec.args.empty()) {
                app_cmd.push_back("--zipfian-alpha");
                app_cmd.push_back(spec.args[0]);
            } else if (spec.behavior == "latest" && !spec.args.empty()) {
                app_cmd.push_back("--read-ratio");
                app_cmd.push_back(spec.args[0]);
            }
            app_pids.push_back(spawn_child(app_cmd));
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        int app_failures = 0;
        for (pid_t pid : app_pids) {
            if (wait_exit_code(pid) != 0) {
                ++app_failures;
            }
        }

        int dat_exit = wait_exit_code(dat_pid);
        std::string dat_output;
        if (dat_stdout_fd >= 0) {
            dat_output = read_all_fd(dat_stdout_fd);
            close(dat_stdout_fd);
        }

        ParsedStats stats = parse_stats_line(dat_output);
        if (!stats.ok) {
            std::cerr << "runner: failed to parse dat stats output\n";
            std::cerr << dat_output << "\n";
            return 1;
        }

        append_log(args, n, stats);

        if (app_failures) {
            std::cout << "runner: apps_failed=" << app_failures << std::endl;
        }
        if (dat_exit) {
            std::cout << "dat: exit_code=" << dat_exit << std::endl;
        }

        std::cout << "hits=" << stats.hits
                  << " misses=" << stats.misses
                  << " hit_ratio=" << stats.hit_ratio
                  << " duration=" << stats.runtime_seconds
                  << " log=" << args.log_file
                  << std::endl;

        return (app_failures == 0 && dat_exit == 0) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "runner: " << ex.what() << "\n";
        return 1;
    }
}