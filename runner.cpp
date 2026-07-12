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
    int n = 4;
    std::uint64_t seed = 1;
    std::string cache_policy = "lru";
    std::string prefetch = "none";
    std::size_t capacity = 4096;
    std::uint64_t miss_delay_ns = 300000;
    std::uint64_t hit_delay_ns = 30000;
    std::string file_data;
    double frac_scan = 0.5;
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
        if (key == "-n") {
            a.n = std::stoi(require_value(i, argc, argv));
        } else if (key == "--seed") {
            a.seed = std::stoull(require_value(i, argc, argv));
        } else if (key == "--cache-policy") {
            a.cache_policy = require_value(i, argc, argv);
        } else if (key == "--prefetch") {
            a.prefetch = require_value(i, argc, argv);
        } else if (key == "--capacity") {
            a.capacity = static_cast<std::size_t>(std::stoull(require_value(i, argc, argv)));
        } else if (key == "--miss-delay") {
            a.miss_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--hit-delay") {
            a.hit_delay_ns = std::stoull(require_value(i, argc, argv));
        } else if (key == "--file-data") {
            a.file_data = require_value(i, argc, argv);
        } else if (key == "--frac-scan") {
            a.frac_scan = std::stod(require_value(i, argc, argv));
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

    if (a.n <= 0) {
        throw std::runtime_error("-n must be > 0");
    }
    if (a.file_data.empty()) {
        throw std::runtime_error("--file-data is required");
    }
    if (a.frac_scan < 0.0 || a.frac_scan > 1.0) {
        throw std::runtime_error("--frac-scan must be in [0, 1]");
    }
    if (a.page_span == 0) {
        throw std::runtime_error("--page-span must be > 0");
    }

    return a;
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
        // dup2(pipefd[1], STDERR_FILENO);
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

void append_log(const Args& args, const ParsedStats& s) {
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
        out << "timestamp,machine,commit_hash,n,seed,cache_policy,prefetch,capacity,miss_delay_ns,hit_delay_ns,file_data,"
               "frac_scan,warmup_period,requests,page_span,hits,misses,"
               "hit_ratio,evictions,bytes_read,bytes_written,avg_latency_ns,runtime_seconds\n";
    }

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    out << ts << ','
        << get_hostname() << ','
        << get_git_commit() << ','
        << args.n << ','
        << args.seed << ','
        << args.cache_policy << ','
        << args.prefetch << ','
        << args.capacity << ','
        << args.miss_delay_ns << ','
        << args.hit_delay_ns << ','
        << args.file_data << ','
        << args.frac_scan << ','
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
    std::vector<std::string> fifo_paths;
    try {
        Args args = parse_args(argc, argv);

        std::filesystem::path run_dir = std::filesystem::path("./build") /
                                        ("run_" + std::to_string(::getpid()) + "_" + std::to_string(args.seed));
        std::filesystem::create_directories(run_dir);

        std::size_t n = args.n;
        std::vector<PipePair> r_pipes(n);
        std::vector<PipePair> w_pipes(n);
        std::vector<int> dat_r_pipes(n);
        std::vector<int> dat_w_pipes(n);
        std::vector<int> app_r_pipes(n);
        std::vector<int> app_w_pipes(n);

        for (int i = 0; i < args.n; ++i) {
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
            "--cache-policy", args.cache_policy,
            "--prefetch", args.prefetch,
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

        std::size_t scan_apps = static_cast<std::size_t>(args.frac_scan * static_cast<double>(args.n));
        std::vector<pid_t> app_pids;
        app_pids.reserve(static_cast<std::size_t>(args.n));

        for (int i = 0; i < args.n; ++i) {
            const bool is_scan = static_cast<std::size_t>(i) < scan_apps;
            std::vector<std::string> app_cmd = {
                "./bin/app",
                "--behavior", is_scan ? "scan" : "random-read",
                "--seed", std::to_string(args.seed + static_cast<std::uint64_t>(i)),
                "--in-pipe", std::to_string(app_r_pipes[static_cast<std::size_t>(i)]),
                "--out-pipe", std::to_string(app_w_pipes[static_cast<std::size_t>(i)]),
                "--client-id", std::to_string(i),
                "--requests", std::to_string(args.requests),
                "--page-span", std::to_string(args.page_span),
            };
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

        append_log(args, stats);

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
