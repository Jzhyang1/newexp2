// lrunner: userfaultfd-backed twin of runner.cpp.
//
// runner.cpp spawns our synthetic ./bin/dat and one ./bin/app per configured
// client, wires them together with pipes, and logs dat's STATS line to CSV.
// lrunner spawns ./bin/ldat the same way (same spawn-with-stdout-pipe,
// wait-for-exit-code, STATS-line-parsing and CSV-append helpers, copied over
// nearly verbatim below), but in place of synthetic ./bin/app clients it
// launches exactly one *unmodified* real application command under
// LD_PRELOAD=./lib/liblhook.so, so its file-backed mmap() accesses to a
// watched path get routed through ldat's userfaultfd handling instead of
// synthetic REQ/RESP traffic.
//
// Usage:
//   ./bin/lrunner --watch-prefix /mnt/remote [options] -- /path/to/app arg...
//
// Everything after the first bare `--` is exec'd (via PATH lookup, since
// real-world apps like `redis-server` are normally invoked by bare name) as
// the watched application; lrunner does not modify it in any way beyond the
// three LDAT_*/LD_PRELOAD environment variables lhook.cpp reads.
//
// Linux-only, like ldat.cpp/lhook.cpp -- see their file headers. lrunner
// itself doesn't touch userfaultfd directly, but it's useless without them,
// so it lives alongside them rather than pretending to be portable.
#ifndef __linux__
#error "lrunner.cpp launches Linux-only ldat/lhook binaries; only builds on Linux"
#endif

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "uffd_protocol.h"

namespace {

struct Args {
    std::string evict_policy = "lru";
    std::string prefetch_policy = "none";
    std::size_t capacity = 4096;
    std::uint64_t miss_delay_ns = 300000;
    std::uint64_t hit_delay_ns = 30000;
    std::string watch_prefix;
    std::string socket_path = uffdproto::kDefaultSocketPath;
    std::string hook_lib = "./lib/liblhook.so";
    std::uint64_t warmup_period = 0;
    std::string log_file = "./logs/lresults.csv";
    std::vector<std::string> app_cmd;
};

// ---- verbatim (modulo naming) reuse of runner.cpp's small helpers --------

std::string require_value(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + argv[i]);
    }
    ++i;
    return argv[i];
}

Args parse_args(int argc, char** argv) {
    Args a;
    int i = 1;
    for (; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--") {
            ++i;
            break;
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
        } else if (key == "--watch-prefix") {
            a.watch_prefix = require_value(i, argc, argv);
        } else if (key == "--socket") {
            a.socket_path = require_value(i, argc, argv);
        } else if (key == "--hook-lib") {
            a.hook_lib = require_value(i, argc, argv);
        } else if (key == "--warmup-period") {
            a.warmup_period = std::stoull(require_value(i, argc, argv));
        } else if (key == "--log") {
            a.log_file = require_value(i, argc, argv);
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }
    for (; i < argc; ++i) {
        a.app_cmd.push_back(argv[i]);
    }

    if (a.watch_prefix.empty()) {
        throw std::runtime_error("--watch-prefix is required (colon-separated path prefixes to intercept)");
    }
    if (a.app_cmd.empty()) {
        throw std::runtime_error("an application command is required after `--`, e.g. -- redis-server /etc/redis.conf");
    }

    return a;
}

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

pid_t spawn_ldat_with_stdout_pipe(const std::vector<std::string>& args, int* read_fd) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        throw std::runtime_error("pipe() failed for ldat stdout");
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
        throw std::runtime_error("fork failed for ldat");
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execv(argv[0], argv.data());
        std::perror("execv ldat");
        _exit(127);
    }

    close(pipefd[1]);
    *read_fd = pipefd[0];
    return pid;
}

// Real apps are normally invoked by bare name (`redis-server`, `postgres`,
// ...) relying on $PATH, unlike our own ./bin/app -- so this one deliberately
// uses execvp rather than the execv the rest of the codebase uses for its
// own known-path binaries.
pid_t spawn_app_with_preload(const Args& args) {
    std::vector<char*> argv;
    argv.reserve(args.app_cmd.size() + 1);
    for (const std::string& s : args.app_cmd) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed for app");
    }
    if (pid == 0) {
        setenv("LD_PRELOAD", args.hook_lib.c_str(), 1);
        setenv(uffdproto::kEnvSocketPath, args.socket_path.c_str(), 1);
        setenv(uffdproto::kEnvWatchPrefix, args.watch_prefix.c_str(), 1);
        execvp(argv[0], argv.data());
        std::perror("execvp app");
        _exit(127);
    }
    return pid;
}

std::string read_all_fd(int fd) {
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

std::string extract_value(const std::string& line, const std::string& key) {
    std::string needle = key + "=";
    std::size_t pos = line.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::size_t end = line.find(' ', pos);
    if (end == std::string::npos) end = line.size();
    return line.substr(pos, end - pos);
}

ParsedStats parse_stats_line(const std::string& all_output) {
    ParsedStats s;
    std::istringstream in(all_output);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("STATS ", 0) != 0) continue;
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
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

std::string get_hostname() {
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return "unknown-host";
    host[sizeof(host) - 1] = '\0';
    return host;
}

std::string get_git_commit() {
    FILE* fp = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!fp) return "unknown";
    char buf[128];
    std::string out;
    if (fgets(buf, sizeof(buf), fp) != nullptr) out = buf;
    (void)pclose(fp);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out.empty() ? "unknown" : out;
}

void ensure_parent_dir(const std::string& path) {
    std::filesystem::path p(path);
    auto parent = p.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
}

std::string join_cmd(const std::vector<std::string>& cmd) {
    std::ostringstream out;
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        if (i > 0) out << ' ';
        out << cmd[i];
    }
    return out.str();
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
        out << "timestamp,machine,commit_hash,evict_policy,prefetch_policy,capacity,miss_delay_ns,hit_delay_ns,"
               "watch_prefix,socket_path,warmup_period,app_cmd,hits,misses,"
               "hit_ratio,evictions,bytes_read,bytes_written,avg_latency_ns,runtime_seconds\n";
    }

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    out << ts << ','
        << get_hostname() << ','
        << get_git_commit() << ','
        << args.evict_policy << ','
        << args.prefetch_policy << ','
        << args.capacity << ','
        << args.miss_delay_ns << ','
        << args.hit_delay_ns << ','
        << args.watch_prefix << ','
        << args.socket_path << ','
        << args.warmup_period << ','
        << '"' << join_cmd(args.app_cmd) << '"' << ','
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

        std::vector<std::string> ldat_cmd = {
            "./bin/ldat",
            "--evict-policy", args.evict_policy,
            "--prefetch-policy", args.prefetch_policy,
            "--capacity", std::to_string(args.capacity),
            "--miss-delay", std::to_string(args.miss_delay_ns),
            "--hit-delay", std::to_string(args.hit_delay_ns),
            "--socket", args.socket_path,
            "--warmup-period", std::to_string(args.warmup_period),
        };

        int ldat_stdout_fd = -1;
        pid_t ldat_pid = spawn_ldat_with_stdout_pipe(ldat_cmd, &ldat_stdout_fd);

        // Give ldat time to bind()/listen() before the watched app's first
        // mmap() tries to connect to it (same role as runner.cpp's sleep
        // before spawning apps against dat's pipes).
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        pid_t app_pid = spawn_app_with_preload(args);
        int app_exit = wait_exit_code(app_pid);

        // ldat only prints its STATS line and exits once told to (it has no
        // other way to know the one app we launched is done); ask it to stop
        // now. Each lrunner run owns its own ldat instance/socket, so this is
        // safe even if other lrunner runs are using different --socket paths
        // concurrently.
        kill(ldat_pid, SIGTERM);
        int ldat_exit = wait_exit_code(ldat_pid);

        std::string ldat_output;
        if (ldat_stdout_fd >= 0) {
            ldat_output = read_all_fd(ldat_stdout_fd);
            close(ldat_stdout_fd);
        }

        ParsedStats stats = parse_stats_line(ldat_output);
        if (!stats.ok) {
            std::cerr << "lrunner: failed to parse ldat stats output\n";
            std::cerr << ldat_output << "\n";
            return 1;
        }

        append_log(args, stats);

        if (app_exit) std::cout << "app: exit_code=" << app_exit << std::endl;
        if (ldat_exit) std::cout << "ldat: exit_code=" << ldat_exit << std::endl;

        std::cout << "hits=" << stats.hits
                  << " misses=" << stats.misses
                  << " hit_ratio=" << stats.hit_ratio
                  << " duration=" << stats.runtime_seconds
                  << " log=" << args.log_file
                  << std::endl;

        return (app_exit == 0 && ldat_exit == 0) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "lrunner: " << ex.what() << "\n";
        return 1;
    }
}
