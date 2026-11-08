// fwd_sim: physical remote-disk experiment driver.
//
//   fwd_sim mkdisk --file F --size-mb N        write a verifiable pattern disk
//   fwd_sim serve  --file F --port P [link opts]   the remote disk + its link
//   fwd_sim bench  --port P --mode direct|cache [opts]   a guest hammering it
//
// `bench` drives a nbd::BlockReadClass (the same interface nbd_server hands
// every guest NBD read to) from worker threads that each issue a read, "think"
// for a while, and issue the next. Everything is wall-clock: the remote disk is
// a separate process whose link RTT / bandwidth are enforced with real sleeps,
// so the numbers are what a guest attached that way would actually wait.
// tools: disk/fwd_experiment.py runs the matrix; see README.md.

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fwd.hpp"

namespace {

using Args = std::map<std::string, std::string>;

Args ParseArgs(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) {
            std::fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            std::exit(2);
        }
        k = k.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a[k] = argv[++i];
        else a[k] = "1";
    }
    return a;
}

std::string Str(const Args& a, const char* k, const std::string& d = "") {
    auto it = a.find(k);
    return it == a.end() ? d : it->second;
}
uint64_t U64(const Args& a, const char* k, uint64_t d) {
    auto it = a.find(k);
    return it == a.end() ? d : std::stoull(it->second);
}
double Dbl(const Args& a, const char* k, double d) {
    auto it = a.find(k);
    return it == a.end() ? d : std::stod(it->second);
}

// ---- verifiable disk contents ---------------------------------------------
// 8-byte word w of the disk holds Mix(w): a client can recompute any byte it
// reads, so cache/readahead bugs (wrong page, stale slot, bad offset math)
// show up as mismatches rather than silently-wrong latency numbers.
inline uint64_t Mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

int g_verify_fd = -1;  // --verify-file: compare against this file instead of the pattern

bool Verify(uint64_t offset, const uint8_t* buf, size_t len) {
    if (g_verify_fd >= 0) {
        static thread_local std::vector<uint8_t> ref;
        ref.resize(len);
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::pread(g_verify_fd, ref.data() + got, len - got, off_t(offset + got));
            if (n <= 0) break;
            got += size_t(n);
        }
        return got == len && std::memcmp(ref.data(), buf, len) == 0;
    }
    for (size_t i = 0; i < len;) {
        const uint64_t o = offset + i, w = o / 8;
        const unsigned sh = static_cast<unsigned>(o % 8);
        const uint64_t v = Mix(w);
        const size_t n = std::min<size_t>(8 - sh, len - i);
        for (size_t j = 0; j < n; ++j)
            if (buf[i + j] != static_cast<uint8_t>(v >> (8 * (sh + j)))) return false;
        i += n;
    }
    return true;
}

int MkDisk(const Args& a) {
    const std::string file = Str(a, "file");
    const uint64_t size = U64(a, "size-mb", 1024) << 20;
    if (file.empty()) { std::fprintf(stderr, "mkdisk: --file required\n"); return 2; }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    std::vector<uint64_t> chunk((1 << 20) / 8);
    for (uint64_t base = 0; base < size; base += chunk.size() * 8) {
        const uint64_t words = std::min<uint64_t>(chunk.size(), (size - base) / 8);
        for (uint64_t i = 0; i < words; ++i) chunk[i] = Mix(base / 8 + i);
        out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(words * 8));
    }
    return out ? 0 : 1;
}

// ---- serve ----------------------------------------------------------------
fwd::RemoteDiskServer* g_server = nullptr;
void OnSignal(int) { if (g_server) g_server->stop(); }

int Serve(const Args& a) {
    fwd::LinkConfig link;
    link.rtt_us = U64(a, "rtt-us", link.rtt_us);
    link.bandwidth_mbps = Dbl(a, "bw-mbps", link.bandwidth_mbps);
    link.disk_us = U64(a, "disk-us", link.disk_us);
    link.disk_mbps = Dbl(a, "disk-mbps", link.disk_mbps);
    link.chunk_bytes = U64(a, "chunk", link.chunk_bytes);
    link.prioritize_demand = U64(a, "prio", 1) != 0;
    try {
        fwd::RemoteDiskServer server(Str(a, "file"), link);
        int port = server.listen(static_cast<uint16_t>(U64(a, "port", 0)));
        if (port < 0) { std::perror("listen"); return 1; }
        g_server = &server;
        struct sigaction sa{};
        sa.sa_handler = OnSignal;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        std::printf("listening %d size %llu rtt_us %llu bw_mbps %g\n", port,
                    (unsigned long long)server.size(), (unsigned long long)link.rtt_us, link.bandwidth_mbps);
        std::fflush(stdout);
        server.run();
        auto s = server.stats();
        std::fprintf(stderr, "remote disk: demand %llu reqs / %llu B, bulk %llu reqs / %llu B\n",
                     (unsigned long long)s.requests[0], (unsigned long long)s.bytes[0],
                     (unsigned long long)s.requests[1], (unsigned long long)s.bytes[1]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "serve: %s\n", e.what());
        return 1;
    }
    return 0;
}

// ---- workloads ------------------------------------------------------------
constexpr uint64_t kNoThink = UINT64_MAX;
// think_ns: the guest's own compute time before its next request, when the workload is a
// recorded trace that carries it (kNoThink = use --think-us).
struct Req { uint64_t offset; uint32_t length; uint64_t think_ns = kNoThink; };

struct TraceRows { std::vector<std::vector<Req>> by_worker; };

class Workload {
public:
    Workload(const std::string& kind, uint64_t disk, uint64_t req_bytes, unsigned worker, unsigned workers,
             uint64_t seed, const Args& a, const std::vector<Req>* trace)
        : kind_(kind), disk_(disk), rb_(req_bytes), rng_(seed * 1000003 + worker), trace_(trace) {
        region_ = (disk_ / workers) / rb_ * rb_;
        base_ = region_ * worker;
        pos_ = 0;
        run_len_ = U64(a, "run-len", 64);
        stride_ = U64(a, "stride", 4);
        hot_region_ = U64(a, "hot-region", 262144);
        if (kind_ == "zipf") {
            const uint64_t n = std::max<uint64_t>(1, disk_ / hot_region_);
            const double s = Dbl(a, "zipf-s", 1.0);
            cdf_.resize(n);
            double sum = 0;
            for (uint64_t i = 0; i < n; ++i) cdf_[i] = (sum += 1.0 / std::pow(double(i + 1), s));
            for (auto& c : cdf_) c /= sum;
            // Spread rank -> region so the hot set isn't one contiguous prefix.
            perm_.resize(n);
            for (uint64_t i = 0; i < n; ++i) perm_[i] = i;
            std::mt19937_64 pr(seed);
            std::shuffle(perm_.begin(), perm_.end(), pr);
        }
    }

    Req next() {
        if (kind_ == "seq") {
            Req r{base_ + pos_, uint32_t(rb_)};
            pos_ = (pos_ + rb_) % region_;
            return r;
        }
        if (kind_ == "stride") {
            Req r{base_ + pos_, uint32_t(rb_)};
            pos_ = (pos_ + stride_ * rb_) % region_;
            return r;
        }
        if (kind_ == "seqjump") {  // sequential runs of ~run_len requests, then a random jump
            if (left_ == 0) {
                pos_ = (rng_() % (region_ / rb_)) * rb_;
                left_ = 1 + rng_() % (2 * run_len_);
            }
            --left_;
            Req r{base_ + pos_, uint32_t(rb_)};
            pos_ = (pos_ + rb_) % region_;
            return r;
        }
        if (kind_ == "random") return {(rng_() % (disk_ / rb_)) * rb_, uint32_t(rb_)};
        if (kind_ == "zipf") {
            double u = std::uniform_real_distribution<double>(0, 1)(rng_);
            size_t rank = std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin();
            if (rank >= cdf_.size()) rank = cdf_.size() - 1;
            uint64_t slots = std::max<uint64_t>(1, hot_region_ / rb_);
            return {perm_[rank] * hot_region_ + (rng_() % slots) * rb_, uint32_t(rb_)};
        }
        if (kind_ == "faiss") return (*trace_)[tpos_++ % trace_->size()];  // recorded offsets/lengths/think, verbatim
        // trace: replay in recorded order, wrapping
        const Req t = (*trace_)[tpos_++ % trace_->size()];
        uint64_t len = std::min<uint64_t>(std::max<uint64_t>(t.length, 512), 1 << 20);
        uint64_t off = t.offset % (disk_ - len);
        off &= ~uint64_t(511);
        return {off, uint32_t(len)};
    }

private:
    std::string kind_;
    uint64_t disk_, rb_, region_, base_, pos_, left_{0}, run_len_, stride_, hot_region_;
    std::mt19937_64 rng_;
    std::vector<double> cdf_;
    std::vector<uint64_t> perm_;
    const std::vector<Req>* trace_;
    size_t tpos_{0};
};

std::vector<Req> LoadTrace(const std::string& path, uint64_t worker, uint64_t workers, uint64_t cap) {
    // nbd_access.csv: worker_id,offset,length,hits,total. If the trace recorded
    // several VMs (worker_id), bench worker w replays the rows of VM w % VMs, so
    // each keeps its own request order. A single-VM trace is instead cut into
    // `workers` contiguous slices (each slice is a different stretch of that VM's life).
    std::vector<std::pair<uint64_t, Req>> all;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    uint64_t max_id = 0;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string f[3];
        if (!std::getline(ss, f[0], ',') || !std::getline(ss, f[1], ',') || !std::getline(ss, f[2], ',')) continue;
        const uint64_t id = std::stoull(f[0]);
        max_id = std::max(max_id, id);
        all.push_back({id, {std::stoull(f[1]), uint32_t(std::stoull(f[2]))}});
    }
    std::vector<Req> rows;
    if (max_id > 0) {
        for (auto& [id, r] : all)
            if (id % workers == worker && rows.size() < cap) rows.push_back(r);
    } else {
        const size_t slice = all.size() / workers;
        for (size_t i = worker * slice; i < (worker + 1) * slice && rows.size() < cap; ++i) rows.push_back(all[i].second);
    }
    return rows;
}

// features.csv from tools/capture (a real VM's NBD reads): offset, length and the
// guest's think time before the next request, for reads (type 0) only. Replayed
// verbatim, no address scaling -- serve the same disk image the trace was taken on.
std::vector<Req> LoadFeatures(const std::string& path, uint64_t skip, uint64_t cap) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    std::vector<std::string> cols;
    { std::stringstream hs(line); std::string c; while (std::getline(hs, c, ',')) cols.push_back(c); }
    auto idx = [&](const char* n) {
        for (size_t i = 0; i < cols.size(); ++i) if (cols[i] == n) return int(i);
        std::fprintf(stderr, "features file lacks column %s\n", n);
        std::exit(2);
    };
    const int ct = idx("type"), co = idx("offset"), cl = idx("length"), ck = idx("think_ns");
    std::vector<Req> rows;
    uint64_t seen = 0;
    while (std::getline(in, line) && rows.size() < cap) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) f.push_back(c);
        if (f.size() != cols.size() || f[ct] != "0") continue;
        if (seen++ < skip) continue;
        rows.push_back({std::stoull(f[co]), uint32_t(std::stoull(f[cl])), std::stoull(f[ck])});
    }
    return rows;
}

// ---- policies (the subset that makes sense for a forwarding cache) ---------
policy::CachePolicy* MakeEvict(const std::string& n, policy::Cache& c) {
    if (n == "lru") return new policy::LRUPolicy(c);
    if (n == "fifo") return new policy::FIFOPolicy(c);
    if (n == "lifo") return new policy::LIFOPolicy(c);
    if (n == "mru") return new policy::MRUPolicy(c);
    if (n == "lru_cxt_aware") return new policy::ContextAwareLRUPolicy(c);
    if (n == "none") return nullptr;
    throw std::runtime_error("unknown evict policy " + n);
}
policy::CachePolicy* MakePrefetch(const std::string& n, policy::Cache& c, uint64_t ra_pages) {
    if (n == "readahead") return new policy::ReadaheadPolicy(c, ra_pages);
    if (n == "readahead_cxt_aware") return new policy::ContextAwareReadaheadPolicy(c, 8, 8, ra_pages);
    if (n == "cminer") return new policy::CMinerPolicy(c);
    if (n == "quickmine") return new policy::QuickMinePolicy(c);
    if (n == "mithril") return new policy::MithrilPolicy(c);
    if (n == "none") return nullptr;
    throw std::runtime_error("unknown prefetch policy " + n);
}

// ---- bench ----------------------------------------------------------------
int Bench(const Args& a) {
    const std::string mode = Str(a, "mode", "direct");
    const std::string wl = Str(a, "workload", "seq");
    const unsigned workers = static_cast<unsigned>(U64(a, "workers", 1));
    uint64_t requests = U64(a, "requests", 1000);  // per worker
    const uint64_t req_bytes = U64(a, "req-bytes", 4096);
    const uint64_t think_us = U64(a, "think-us", 200);
    const uint64_t page_size = U64(a, "page-size", 16384);
    const uint64_t capacity = U64(a, "capacity", 16384);
    const uint64_t ra_pages = U64(a, "readahead-pages", 32);
    const bool verify = U64(a, "verify", 1) != 0;
    const double think_scale = Dbl(a, "think-scale", 1.0);
    const std::string evict_name = Str(a, "evict", "lru");
    const std::string pf_name = mode == "cache" ? Str(a, "prefetch", "readahead") : "-";
    const std::string host = Str(a, "host", "127.0.0.1");
    const uint16_t port = static_cast<uint16_t>(U64(a, "port", 0));

    fwd::RemoteClient remote(host, port, std::max(workers, 4u), static_cast<unsigned>(U64(a, "pf-threads", 4)));
    if (!remote.connect()) { std::fprintf(stderr, "bench: cannot reach remote disk %s:%u\n", host.c_str(), port); return 1; }
    const uint64_t disk = remote.size();

    policy::Cache cache(mode == "cache" ? capacity : 0, 0, 0, page_size);
    std::unique_ptr<policy::CachePolicy> evict_policy, pf_policy;
    std::unique_ptr<nbd::BlockReadClass> reader;
    fwd::FwdReadClass* fwd_reader = nullptr;
    try {
        if (mode == "cache") {
            evict_policy.reset(MakeEvict(evict_name, cache));
            pf_policy.reset(MakePrefetch(pf_name, cache, ra_pages));
            fwd::FwdConfig fc;
            fc.prefetch_threads = static_cast<unsigned>(U64(a, "pf-threads", 4));
            fc.max_prefetch_pages = U64(a, "max-prefetch-pages", fc.max_prefetch_pages);
            fc.max_run_pages = U64(a, "max-run-pages", fc.max_run_pages);
            fc.prefetch_budget_pages = U64(a, "prefetch-budget", 0);
            fc.local_read_ns = U64(a, "local-read-us", 0) * 1000;
            fc.cache_dir = Str(a, "cache-dir", "/tmp");
            fwd_reader = new fwd::FwdReadClass(remote, cache, evict_policy.get(), pf_policy.get(), fc);
            reader.reset(fwd_reader);
        } else if (mode == "direct") {
            reader.reset(new fwd::DirectReadClass(remote));
        } else {
            std::fprintf(stderr, "bench: --mode must be direct or cache\n");
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bench: %s\n", e.what());
        return 1;
    }

    std::vector<std::vector<Req>> traces(workers);
    if (!Str(a, "verify-file").empty()) {
        g_verify_fd = ::open(Str(a, "verify-file").c_str(), O_RDONLY);
        if (g_verify_fd < 0) { std::perror("verify-file"); return 1; }
    }
    if (wl == "faiss") {
        traces[0] = LoadFeatures(Str(a, "features", "logs/capture/base/features.csv"), U64(a, "trace-skip", 0), requests);
        if (traces[0].empty()) { std::fprintf(stderr, "bench: no reads in features file\n"); return 1; }
        requests = std::min<uint64_t>(requests, traces[0].size());
        for (auto& r : traces[0]) if (r.offset + r.length > disk) { std::fprintf(stderr, "bench: trace reads past the remote disk (wrong image?)\n"); return 1; }
    }
    if (wl == "trace") {
        for (unsigned w = 0; w < workers; ++w) traces[w] = LoadTrace(Str(a, "trace", "logs/nbd_access.csv"), w, workers, requests);
        for (unsigned w = 0; w < workers; ++w)
            if (traces[w].empty()) { std::fprintf(stderr, "bench: trace has no rows for worker %u\n", w); return 1; }
    }

    std::vector<std::vector<uint64_t>> lats(workers);
    std::atomic<uint64_t> mismatches{0};
    std::vector<std::thread> threads;
    const uint64_t t_start = nbd::steady_ns();
    for (unsigned w = 0; w < workers; ++w) {
        threads.emplace_back([&, w] {
            Workload gen(wl, disk, req_bytes, w, workers, U64(a, "seed", 1), a, &traces[w]);
            std::vector<uint8_t> buf(1 << 20);
            lats[w].reserve(requests);
            for (uint64_t i = 0; i < requests; ++i) {
                Req r = gen.next();
                const uint64_t t0 = nbd::steady_ns();
                reader->read(w, r.offset, r.length, buf.data());
                lats[w].push_back(nbd::steady_ns() - t0);
                if (verify && !Verify(r.offset, buf.data(), r.length)) mismatches++;
                const uint64_t think_ns = r.think_ns != kNoThink ? uint64_t(double(r.think_ns) * think_scale) : think_us * 1000;
                if (think_ns) fwd::SleepPrecise(think_ns);
            }
        });
    }
    for (auto& t : threads) t.join();
    const double wall_s = double(nbd::steady_ns() - t_start) / 1e9;

    std::vector<uint64_t> all;
    uint64_t sum = 0;
    for (auto& v : lats) for (uint64_t x : v) { all.push_back(x); sum += x; }
    std::sort(all.begin(), all.end());
    auto pct = [&](double p) { return double(all[std::min(all.size() - 1, size_t(p * all.size()))]) / 1e3; };

    const nbd::Stats& st = reader->get_stats();
    const uint64_t bytes_read = st.bytes_read;
    fwd::FwdStats fs;
    if (fwd_reader) fs = fwd_reader->fwd_stats();
    const uint64_t rd = remote.bytes(fwd::kDemand), rb = remote.bytes(fwd::kBulk);
    const uint64_t reqs_remote = remote.requests(fwd::kDemand) + remote.requests(fwd::kBulk);

    std::printf("mode=%s workload=%s workers=%u requests=%llu wall_s=%.3f io_wait_s=%.3f mean_us=%.1f p50_us=%.1f "
                "p95_us=%.1f p99_us=%.1f max_us=%.1f remote_reqs=%llu remote_MB=%.1f amp=%.2f mismatches=%llu\n",
                mode.c_str(), wl.c_str(), workers, (unsigned long long)all.size(), wall_s, double(sum) / 1e9,
                double(sum) / all.size() / 1e3, pct(0.5), pct(0.95), pct(0.99), double(all.back()) / 1e3,
                (unsigned long long)reqs_remote, double(rd + rb) / 1e6,
                bytes_read ? double(rd + rb) / bytes_read : 0.0, (unsigned long long)mismatches.load());

    const std::string csv = Str(a, "csv");
    if (!csv.empty()) {
        bool fresh = !std::ifstream(csv).good();
        FILE* f = std::fopen(csv.c_str(), "a");
        if (!f) { std::perror("csv"); return 1; }
        if (fresh)
            std::fprintf(f, "scenario,label,mode,workload,workers,requests,req_bytes,think_us,page_size,capacity,evict,prefetch,"
                            "readahead_pages,wall_s,io_wait_s,mean_us,p50_us,p95_us,p99_us,max_us,bytes_read,remote_reqs,"
                            "remote_demand_bytes,remote_bulk_bytes,hit_pages,late_pages,miss_pages,hit_requests,"
                            "pf_issued_pages,pf_used_pages,pf_wasted_pages,pf_dropped_pages,pf_stolen_pages,mismatches,rep\n");
        std::fprintf(f, "%s,%s,%s,%s,%u,%llu,%llu,%llu,%llu,%llu,%s,%s,%llu,%.4f,%.4f,%.1f,%.1f,%.1f,%.1f,%.1f,%llu,%llu,%llu,%llu,"
                        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                     Str(a, "scenario", "-").c_str(), Str(a, "label", mode).c_str(), mode.c_str(), wl.c_str(), workers,
                     (unsigned long long)requests, (unsigned long long)req_bytes, (unsigned long long)think_us,
                     (unsigned long long)page_size, (unsigned long long)(mode == "cache" ? capacity : 0), evict_name.c_str(),
                     pf_name.c_str(), (unsigned long long)ra_pages, wall_s, double(sum) / 1e9, double(sum) / all.size() / 1e3,
                     pct(0.5), pct(0.95), pct(0.99), double(all.back()) / 1e3, (unsigned long long)bytes_read,
                     (unsigned long long)reqs_remote, (unsigned long long)rd, (unsigned long long)rb,
                     (unsigned long long)fs.hit_pages, (unsigned long long)fs.late_pages, (unsigned long long)fs.miss_pages,
                     (unsigned long long)fs.hit_requests, (unsigned long long)fs.pf_issued_pages,
                     (unsigned long long)fs.pf_used_pages, (unsigned long long)fs.pf_wasted_pages,
                     (unsigned long long)fs.pf_dropped_pages, (unsigned long long)fs.pf_stolen_pages,
                     (unsigned long long)mismatches.load(), (unsigned long long)U64(a, "rep", 0));
        std::fclose(f);
    }
    return mismatches.load() ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s mkdisk|serve|bench [--opt value ...]\n", argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    Args a = ParseArgs(argc, argv, 2);
    if (cmd == "mkdisk") return MkDisk(a);
    if (cmd == "serve") return Serve(a);
    if (cmd == "bench") return Bench(a);
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
