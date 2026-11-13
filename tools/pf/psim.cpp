// psim: fast trace-driven evaluator for evict/prefetch policies.
//
// Links the real policies/*.cpp and mirrors SimReadClass::read()'s order of
// operations (prefetch hook first, then per-page demand admit + on_access, then
// the prefetched pages are admitted), but without sockets, locks or pread, so
// millions of requests replay in seconds.  Extras over SimReadClass:
//   * --delay K     a prefetched page only counts as a hit once K further
//                   requests have been served (models fetch latency; K=0 is
//                   the optimistic "instant prefetch" nbd_server does today)
//   * traffic accounting (pages admitted by demand vs. by prefetch, and how many
//                   prefetched pages were ever used)
//
// Trace: CSV with header; columns offset,length (bytes) are required, worker /
// worker_id optional.  Lines whose length is 0 are skipped.
//
//   psim --trace T.csv --policy readahead:pages=32 [--evict lru] [--page 65536]
//        [--cap 4096] [--delay 0] [--limit N] [--warm N] [--label X] [--csv]
#include "../../policies/policy_api.h"
#include "../../policies/policy_factory.h"

#include <algorithm>
#include <set>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using namespace policy;

struct Req { uint64_t worker, off, len, t; };

static std::vector<Req> LoadTrace(const std::string& path, uint64_t limit) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    std::vector<std::string> cols;
    { std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) cols.push_back(c); }
    int ci_off = -1, ci_len = -1, ci_w = -1, ci_type = -1, ci_t = -1;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == "offset") ci_off = (int)i;
        else if (cols[i] == "length") ci_len = (int)i;
        else if (cols[i] == "worker" || cols[i] == "worker_id") ci_w = (int)i;
        else if (cols[i] == "type") ci_type = (int)i;
        else if (cols[i] == "arrive_ns") ci_t = (int)i;
    }
    if (ci_off < 0 || ci_len < 0) { std::fprintf(stderr, "trace needs offset,length columns\n"); std::exit(2); }
    std::vector<Req> out;
    std::vector<uint64_t> v(cols.size());
    while (std::getline(in, line)) {
        if (limit && out.size() >= limit) break;
        const char* p = line.c_str();
        size_t n = 0;
        while (n < cols.size()) {
            char* e;
            v[n++] = std::strtoull(p, &e, 10);
            if (*e != ',') break;
            p = e + 1;
        }
        if (n < cols.size() && (size_t)ci_off >= n) continue;
        if (ci_type >= 0 && v[ci_type] != 0) continue;
        uint64_t len = v[ci_len];
        if (!len) continue;
        out.push_back({ci_w >= 0 ? v[ci_w] : 0, v[ci_off], len, ci_t >= 0 ? v[ci_t] : 0});
    }
    return out;
}

// Analysis-only bound: knows the next W requests; prefetches exactly the pages they will touch that lie
// within D pages of the current request (the best any neighborhood-kernel prefetcher could do).
class LocalOracle : public CachePolicy {
    const std::vector<Req>& reqs_;
    uint64_t W_, D_, i_ = 0, psz_;
public:
    LocalOracle(Cache& c, const std::vector<Req>& r, uint64_t W, uint64_t D) : CachePolicy(c), reqs_(r), W_(W), D_(D), psz_(c.page_size) {}
    void on_prefetch_request(uint64_t, uint64_t b0, uint64_t bn, PrefetchRequest& req) override {
        uint64_t b1 = b0 + bn - 1;
        uint64_t lo = b0 > D_ ? b0 - D_ : 0, hi = b1 + D_;
        std::vector<std::pair<uint64_t, uint64_t>> iv;
        for (uint64_t k = i_ + 1; k <= i_ + W_ && k < reqs_.size(); ++k) {
            uint64_t a = reqs_[k].off / psz_, b = (reqs_[k].off + reqs_[k].len - 1) / psz_;
            if (b < lo || a > hi) continue;
            iv.emplace_back(std::max(a, lo), std::min(b, hi));
        }
        ++i_;
        std::sort(iv.begin(), iv.end());
        req.fetch_count = 0;
        for (auto& x : iv) {
            if (req.fetch_count && x.first <= req.fetch_ranges[req.fetch_count - 1].block_offset + req.fetch_ranges[req.fetch_count - 1].block_length) {
                auto& f = req.fetch_ranges[req.fetch_count - 1];
                uint64_t e = std::max(f.block_offset + f.block_length, x.second + 1);
                f.block_length = e - f.block_offset;
            } else if (req.fetch_count < MAX_PREFETCH_PAGES) {
                req.fetch_ranges[req.fetch_count++] = FetchRange{x.first, x.second - x.first + 1};
            }
        }
    }
};

// Analysis-only: prefetch pages from an external predictor's output (binary records {u64 request idx, u64 page, f32 prob}).
class FilePolicy : public CachePolicy {
    struct Rec { uint64_t i, pg; float p; };
    std::vector<Rec> recs_;
    size_t pos_ = 0;
    uint64_t i_ = 0;
    float theta_;
public:
    FilePolicy(Cache& c, const std::string& path, float theta) : CachePolicy(c), theta_(theta) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
        unsigned char b[20];
        while (std::fread(b, 1, 20, f) == 20) {
            Rec r; std::memcpy(&r.i, b, 8); std::memcpy(&r.pg, b + 8, 8); std::memcpy(&r.p, b + 16, 4); recs_.push_back(r);
        }
        std::fclose(f);
    }
    void on_prefetch_request(uint64_t, uint64_t, uint64_t, PrefetchRequest& req) override {
        req.fetch_count = 0;
        std::vector<uint64_t> pgs;
        while (pos_ < recs_.size() && recs_[pos_].i < i_) ++pos_;
        for (size_t k = pos_; k < recs_.size() && recs_[k].i == i_; ++k) if (recs_[k].p >= theta_) pgs.push_back(recs_[k].pg);
        ++i_;
        std::sort(pgs.begin(), pgs.end());
        for (uint64_t pg : pgs) {
            if (req.fetch_count && req.fetch_ranges[req.fetch_count - 1].block_offset + req.fetch_ranges[req.fetch_count - 1].block_length == pg)
                req.fetch_ranges[req.fetch_count - 1].block_length++;
            else if (req.fetch_count < MAX_PREFETCH_PAGES) req.fetch_ranges[req.fetch_count++] = FetchRange{pg, 1};
        }
    }
};

// Analysis-only: Belady (farthest next use) eviction.  Knows each page's future request indices.
class BeladyEvict : public CachePolicy {
    std::unordered_map<uint64_t, std::vector<uint32_t>> uses_;   // page -> request indices (ascending)
    std::unordered_map<uint64_t, size_t> ptr_;
    std::set<std::pair<uint64_t, uint64_t>> by_next_;            // (next use, page) of resident pages
    std::unordered_map<uint64_t, uint64_t> next_of_;
    uint64_t cur_ = 0;
    uint64_t NextUse(uint64_t pg) {
        auto it = uses_.find(pg);
        if (it == uses_.end()) return UINT64_MAX;
        size_t& p = ptr_[pg];
        while (p < it->second.size() && it->second[p] <= cur_) ++p;
        return p < it->second.size() ? it->second[p] : UINT64_MAX;
    }
public:
    BeladyEvict(Cache& c, const std::vector<Req>& reqs) : CachePolicy(c) {
        for (size_t i = 0; i < reqs.size(); ++i) {
            uint64_t a = reqs[i].off / c.page_size, b = (reqs[i].off + reqs[i].len - 1) / c.page_size;
            for (uint64_t q = a; q <= b; ++q) uses_[q].push_back((uint32_t)i);
        }
    }
    void set_now(uint64_t i) { cur_ = i; }
    void refresh(uint64_t pg) {
        auto it = next_of_.find(pg);
        if (it == next_of_.end()) return;
        by_next_.erase({it->second, pg});
        it->second = NextUse(pg);
        by_next_.insert({it->second, pg});
    }
    void on_access(uint64_t, uint64_t page) override { refresh(page); }
    void on_admit(uint64_t, uint64_t page, bool) override { uint64_t n = NextUse(page); next_of_[page] = n; by_next_.insert({n, page}); }
    void on_evict(uint64_t, uint64_t page) override {
        auto it = next_of_.find(page);
        if (it == next_of_.end()) return;
        by_next_.erase({it->second, page});
        next_of_.erase(it);
    }
    void on_evict_request(uint64_t, uint64_t page, EvictRequest& r) override {
        if (cache.present(page) || cache.size() < cache.capacity || by_next_.empty()) return;
        r.pages[r.n_pages++] = by_next_.rbegin()->second;
    }
};


// Analysis-only oracle evictions sharing one code path (so they are directly comparable):
//   min      Belady: evict the resident page whose next demand use is furthest (dead pages first)
//   dmin     Demand-MIN (Jain & Lin, ISCA 2018): first evict pages the prefetcher will bring back in time before
//            their next demand use (the one re-prefetched furthest in the future first), else fall back to MIN
//   dmin_nh  dmin + Cao et al.'s "do no harm": a prefetch is not admitted if it would itself be the next victim
// Needs the prefetcher's future requests: `learned` and its parts never read cache state, so a pre-pass of the same
// policy on the request stream alone yields every (request idx, page) it will ask for.
class OracleEvict : public CachePolicy {
    std::unordered_map<uint64_t, std::vector<uint32_t>> uses_, pfs_;   // page -> demand / prefetch request indices
    std::unordered_map<uint64_t, size_t> up_, pp_;
    std::unordered_map<uint64_t, uint64_t> key_, acc_;                 // resident page -> eviction key; last access idx+1
    std::set<std::pair<uint64_t, uint64_t>> by_key_;
    uint64_t cur_ = 0, lag_;
    bool demand_aware_;
    static uint64_t First(std::unordered_map<uint64_t, std::vector<uint32_t>>& m, std::unordered_map<uint64_t, size_t>& ptrs, uint64_t pg, uint64_t from) {
        auto it = m.find(pg);
        if (it == m.end()) return UINT64_MAX;
        size_t& p = ptrs[pg];
        while (p < it->second.size() && it->second[p] < from) ++p;
        return p < it->second.size() ? it->second[p] : UINT64_MAX;
    }
public:
    bool bypass = false;
    uint64_t n_bypass = 0;
    OracleEvict(Cache& c, const std::vector<Req>& reqs, const std::string& pol, bool demand_aware, uint64_t delay)
        : CachePolicy(c), lag_(std::max<uint64_t>(1, delay)), demand_aware_(demand_aware) {
        for (size_t i = 0; i < reqs.size(); ++i) {
            uint64_t a = reqs[i].off / c.page_size, b = (reqs[i].off + reqs[i].len - 1) / c.page_size;
            for (uint64_t q = a; q <= b; ++q) uses_[q].push_back((uint32_t)i);
        }
        if (!demand_aware) return;
        Cache dummy(c.capacity, 0, 0, c.page_size);
        std::unique_ptr<CachePolicy> shadow(MakePolicy(pol, dummy, ""));
        if (!shadow) return;
        for (size_t i = 0; i < reqs.size(); ++i) {
            const Req& r = reqs[i];
            uint64_t b0 = r.off / c.page_size, bn = (r.off + r.len - 1) / c.page_size - b0 + 1;
            PrefetchRequest pr;
            shadow->on_request(r.worker, r.off, r.len, r.t);
            shadow->on_prefetch_request(r.worker, b0, bn, pr);
            for (uint64_t k = 0; k < pr.fetch_count; ++k)
                for (uint64_t q = pr.fetch_ranges[k].block_offset; q < pr.fetch_ranges[k].block_offset + pr.fetch_ranges[k].block_length; ++q) {
                    auto& v = pfs_[q];
                    if (v.empty() || v.back() != i) v.push_back((uint32_t)i);
                }
        }
    }
    void set_now(uint64_t i) { cur_ = i; }
    uint64_t Key(uint64_t pg) {             // larger = evict first
        auto ai = acc_.find(pg);
        const uint64_t from = (ai != acc_.end() && ai->second == cur_ + 1) ? cur_ + 1 : cur_;
        const uint64_t d = First(uses_, up_, pg, from);
        if (d == UINT64_MAX) return UINT64_MAX;
        if (demand_aware_) {
            const uint64_t f = First(pfs_, pp_, pg, cur_ + 1);
            if (f != UINT64_MAX && f + lag_ <= d) return (1ull << 62) + f;     // comes back by prefetch in time: free
        }
        return d;
    }
    void refresh(uint64_t pg) {
        auto it = key_.find(pg);
        if (it == key_.end()) return;
        by_key_.erase({it->second, pg});
        it->second = Key(pg);
        by_key_.insert({it->second, pg});
    }
    bool would_bypass(uint64_t pg) {         // incoming prefetch would be the victim itself
        if (!bypass || cache.size() < cache.capacity || by_key_.empty()) return false;
        if (Key(pg) >= by_key_.rbegin()->first) { ++n_bypass; return true; }
        return false;
    }
    void on_access(uint64_t, uint64_t page) override { acc_[page] = cur_ + 1; refresh(page); }
    void on_admit(uint64_t, uint64_t page, bool) override { uint64_t k = Key(page); key_[page] = k; by_key_.insert({k, page}); }
    void on_evict(uint64_t, uint64_t page) override {
        auto it = key_.find(page);
        if (it == key_.end()) return;
        by_key_.erase({it->second, page});
        key_.erase(it);
    }
    void on_evict_request(uint64_t, uint64_t page, EvictRequest& r) override {
        if (cache.present(page) || cache.size() < cache.capacity || by_key_.empty()) return;
        r.pages[r.n_pages++] = by_key_.rbegin()->second;
    }
};

// Analysis-only learned eviction (LRB-style, but an online count table instead of a GBM).  At each eviction it samples
// K cached pages, scores each by the learned P(demand-accessed within the next H requests | features) and evicts the
// lowest.  Every sampled candidate is also a training example, labelled H requests later from the (exogenous) request
// stream.  H tracks the cache's turnover time (capacity / pages admitted per request).
// Features: age since last touch (log2), accesses since admission (0..3), prefetched-and-unused, and with `guest` the
// GuestStateHook class (guest holds none / part / all of the page, or dropped it recently).
class LearnedEvict : public CachePolicy {
    struct Info { uint64_t last, nacc; bool pf; };
    std::vector<uint64_t> pages_; std::unordered_map<uint64_t, size_t> pos_;
    std::unordered_map<uint64_t, Info> info_;
    std::unordered_map<uint64_t, uint64_t> last_demand_;
    struct Trial { uint64_t idx, page; int cell; };
    std::deque<Trial> pend_;
    std::vector<double> hit_, tri_;
    uint64_t cur_ = 0, rng_ = 0x9e3779b97f4a7c15ull, K_;
    double admits_ = 0, H_ = 1024;
    bool guest_;
    uint64_t Rand() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17; return rng_; }
    int Cell(uint64_t pg) {
        const Info& f = info_[pg];
        int age = 0; for (uint64_t a = cur_ - f.last + 1; a > 1 && age < 15; a >>= 1) ++age;
        int g = guest_ && GuestStateHook() ? GuestStateHook()(pg) : 0;
        return ((age * 4 + (int)std::min<uint64_t>(f.nacc, 3)) * 2 + (f.pf ? 1 : 0)) * 4 + g;
    }
public:
    LearnedEvict(Cache& c, bool guest, uint64_t K) : CachePolicy(c), hit_(16 * 4 * 2 * 4, 0), tri_(16 * 4 * 2 * 4, 0), K_(K), guest_(guest) {}
    void set_now(uint64_t i) {
        if (i != cur_) {                                      // per request: update turnover horizon, resolve labels
            H_ = std::clamp(cache.capacity / std::max(0.05, admits_), 64.0, 65536.0);
            admits_ *= 0.999;
        }
        cur_ = i;
        while (!pend_.empty() && (double)pend_.front().idx + H_ < (double)cur_) {
            const Trial& t = pend_.front();
            auto it = last_demand_.find(t.page);
            tri_[t.cell] += 1;
            if (it != last_demand_.end() && it->second > t.idx) hit_[t.cell] += 1;
            pend_.pop_front();
        }
    }
    void on_access(uint64_t, uint64_t pg) override {
        last_demand_[pg] = cur_;
        auto it = info_.find(pg);
        if (it != info_.end()) { it->second.last = cur_; ++it->second.nacc; it->second.pf = false; }
    }
    void on_admit(uint64_t, uint64_t pg, bool is_pf) override {
        admits_ += 0.001;
        pos_[pg] = pages_.size(); pages_.push_back(pg);
        info_[pg] = Info{cur_, 0, is_pf};
    }
    void on_evict(uint64_t, uint64_t pg) override {
        auto it = pos_.find(pg);
        if (it == pos_.end()) return;
        size_t i = it->second; pos_.erase(it);
        if (i + 1 != pages_.size()) { pages_[i] = pages_.back(); pos_[pages_[i]] = i; }
        pages_.pop_back();
        info_.erase(pg);
    }
    void on_evict_request(uint64_t, uint64_t page, EvictRequest& r) override {
        if (cache.present(page) || cache.size() < cache.capacity || pages_.empty()) return;
        uint64_t best = 0; double bs = 2; uint64_t blast = UINT64_MAX;
        for (uint64_t k = 0; k < K_; ++k) {
            uint64_t pg = pages_[Rand() % pages_.size()];
            int c = Cell(pg);
            pend_.push_back({cur_, pg, c});
            double s = (hit_[c] + 0.5) / (tri_[c] + 1.0);
            uint64_t l = info_[pg].last;
            if (s < bs || (s == bs && l < blast)) { bs = s; best = pg; blast = l; }
        }
        r.pages[r.n_pages++] = best;
    }
};

int main(int argc, char** argv) {
    std::string trace, pol = "none", ev = "lru", label;
    uint64_t page = 65536, cap = 4096, delay = 0, limit = 0, warm = 0, skip = 0, tail_off = 0;
    bool csv = false, header = false;
    std::string dump_path, dumpreq_path, clip_path, hints_path, guest_path;
    uint64_t g_evict = 0, g_filter = 0;   // --guest-evict / --guest-filter thresholds (resident 4 KiB blocks per page; 0 = off)
    bool g_refault = false;
    uint64_t g_recent = 2048;             // requests after a guest drop during which the page counts as "recently dropped"
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--trace") trace = next();
        else if (a == "--policy") pol = next();
        else if (a == "--evict") ev = next();
        else if (a == "--page") page = std::stoull(next());
        else if (a == "--cap") cap = std::stoull(next());
        else if (a == "--delay") delay = std::stoull(next());
        else if (a == "--limit") limit = std::stoull(next());
        else if (a == "--warm") warm = std::stoull(next());
        else if (a == "--skip") skip = std::stoull(next());
        else if (a == "--max-offset") tail_off = std::stoull(next());
        else if (a == "--label") label = next();
        else if (a == "--csv") csv = true;
        else if (a == "--header") header = true;
        else if (a == "--dump") dump_path = next();
        else if (a == "--dump-req") dumpreq_path = next();
        else if (a == "--clip") clip_path = next();
        else if (a == "--hints") hints_path = next();
        else if (a == "--guest") guest_path = next();
        else if (a == "--guest-evict") g_evict = std::stoull(next());
        else if (a == "--guest-filter") g_filter = std::stoull(next());
        else if (a == "--guest-refault") g_refault = true;
        else if (a == "--guest-recent") g_recent = std::stoull(next());
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if (header) {
        std::puts("label,policy,evict,page,cap_pages,delay,reqs,full_hit_pct,page_hit_pct,pf_pages,pf_used,pf_useful_pct,"
                  "traffic_ratio,demand_miss_pages,late_pages,wall_s,futile_pages,policy_ns_per_req");
        if (trace.empty()) return 0;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Req> reqs = LoadTrace(trace, limit ? limit + skip : 0);
    if (skip) reqs.erase(reqs.begin(), reqs.begin() + std::min<size_t>(skip, reqs.size()));
    if (tail_off) {
        std::vector<Req> f;
        for (auto& r : reqs) if (r.off < tail_off) f.push_back(r);
        reqs.swap(f);
    }

    // --clip F: binary {u64 lo_page, u64 hi_page} intervals (sorted, disjoint) = extents of the objects (files) being read.
    // Analysis of "what if the policy knew where each file ends": prefetches triggered by a request are cut to its extent.
    std::vector<std::pair<uint64_t, uint64_t>> clip;
    if (!clip_path.empty()) {
        FILE* cf = std::fopen(clip_path.c_str(), "rb"); uint64_t v[2];
        while (cf && std::fread(v, 8, 2, cf) == 2) clip.emplace_back(v[0], v[1]);
        if (cf) std::fclose(cf);
    }
    // --hints F: binary records {u64 request idx, u64 first_page, u64 npages, u32 aux, u32 pad}: delivered to on_hint before that request
    struct Hint { uint64_t i, first, n; uint32_t aux, pad; };
    std::vector<Hint> hints; size_t hpos = 0;
    if (!hints_path.empty()) {
        FILE* hf = std::fopen(hints_path.c_str(), "rb"); Hint h;
        while (hf && std::fread(&h, sizeof h, 1, hf) == 1) hints.push_back(h);
        if (hf) std::fclose(hf);
    }
    Cache cache(cap, 30000, 300000, page);
    OracleEvict* oev = nullptr;
    if (ev == "min" || ev == "dmin" || ev == "dmin_nh") {
        oev = new OracleEvict(cache, reqs, pol, ev != "min", delay);
        oev->bypass = ev == "dmin_nh";
    }
    LearnedEvict* lev = (ev == "lev" || ev == "lev_g") ? new LearnedEvict(cache, ev == "lev_g", 64) : nullptr;
    CachePolicy* evict = lev ? (CachePolicy*)lev : oev ? (CachePolicy*)oev : ev == "belady" ? (CachePolicy*)new BeladyEvict(cache, reqs) : MakeEvictPolicy(ev, cache);
    std::string oracle_file;
    if (pol.rfind("oracle", 0) == 0) {
        oracle_file = "/tmp/claude-1000/-home-jonathan-yang-newexp2/0e888e7a-7f13-410b-94ae-3a8bcb0cf58f/scratchpad/oracle_" + std::to_string(getpid()) + ".csv";
        FILE* o = std::fopen(oracle_file.c_str(), "w");
        std::fputs("worker_id,offset,length,hits,total\n", o);
        for (auto& r : reqs) std::fprintf(o, "%llu,%llu,%llu,0,0\n", (unsigned long long)r.worker, (unsigned long long)r.off, (unsigned long long)r.len);
        std::fclose(o);
    }
    ExtraPolicyHook() = [&](const std::string& spec, Cache& c) -> CachePolicy* {
        Spec sp = ParseSpec(spec);
        if (sp.name == "localoracle") return new LocalOracle(c, reqs, sp.get("w", 64), sp.get("d", 256));
        if (sp.name == "file") {
            auto k = spec.find("path="); auto e = spec.find(',', k);
            return new FilePolicy(c, spec.substr(k + 5, e == std::string::npos ? e : e - k - 5), sp.get("theta", 40) / 100.f);
        }
        return nullptr;
    };
    CachePolicy* pf = MakePolicy(pol, cache, oracle_file);

    // last request index touching each page: lets us split unused prefetches into "never requested again" (futile)
    std::unordered_map<uint64_t, uint64_t> last_use;
    last_use.reserve(reqs.size() * 4);
    for (size_t i = 0; i < reqs.size(); ++i) {
        uint64_t a = reqs[i].off / page, b = (reqs[i].off + reqs[i].len - 1) / page;
        for (uint64_t q = a; q <= b; ++q) last_use[q] = i;
    }
    uint64_t futile = 0;
    struct Meta { bool prefetched; bool used; uint64_t ready; uint64_t born; int64_t dist; bool futile; };
    std::unordered_map<uint64_t, Meta> meta;
    meta.reserve(cap * 2);

    uint64_t n_req = 0, full_hit = 0, pg_hit = 0, pg_tot = 0, pf_pages = 0, pf_used = 0, dm_pages = 0, late = 0;
    uint64_t idx = 0, pol_ns = 0;
    int64_t cur_dist = 0; bool cur_futile = false;
    FILE* dreq = dumpreq_path.empty() ? nullptr : std::fopen(dumpreq_path.c_str(), "w");
    if (dreq) std::fputs("idx,off,len,hits,pages,pf,dm\n", dreq);
    FILE* dump = dump_path.empty() ? nullptr : std::fopen(dump_path.c_str(), "w");
    if (dump) std::fputs("born,page,dist,used,futile\n", dump);
    auto emit_dump = [&](uint64_t pg, const Meta& m) { if (dump && m.prefetched) std::fprintf(dump, "%llu,%llu,%lld,%d,%d\n", (unsigned long long)m.born, (unsigned long long)pg, (long long)m.dist, (int)m.used, (int)m.futile); };

    // --guest F: host-side reconstruction of the guest page cache (tools/capture/hv_residency.py: t_ns,page,delta =
    // change in the number of the page's 4 KiB blocks held in guest RAM).  Used by
    //   --guest-evict T   evict cached pages the guest holds >= T blocks of first (oldest touch first): exclusive caching
    //   --guest-filter T  do not prefetch pages the guest holds >= T blocks of
    //   --guest-refault   prefetch a page as soon as the guest drops its last block of it
    struct GEv { uint64_t t, page; int d; };
    std::vector<GEv> gev; size_t gpos = 0;
    if (!guest_path.empty()) {
        std::ifstream gi(guest_path); std::string ln; std::getline(gi, ln);
        while (std::getline(gi, ln)) {
            unsigned long long t, pg; int d;
            if (std::sscanf(ln.c_str(), "%llu,%llu,%d", &t, &pg, &d) == 3) gev.push_back({t, pg, d});
        }
        std::stable_sort(gev.begin(), gev.end(), [](const GEv& x, const GEv& y) { return x.t < y.t; });
    }
    std::unordered_map<uint64_t, int> gcnt;
    std::unordered_map<uint64_t, uint64_t> touch, gx_key;     // cached page -> last touch idx; page -> its key in gxset
    std::set<std::pair<uint64_t, uint64_t>> gxset;            // (last touch, page) of cached pages the guest holds
    auto gx_update = [&](uint64_t pg) {
        if (!g_evict) return;
        auto k = gx_key.find(pg);
        if (k != gx_key.end()) { gxset.erase({k->second, pg}); gx_key.erase(k); }
        if (cache.present(pg)) {
            auto c = gcnt.find(pg);
            if (c != gcnt.end() && c->second >= (int)g_evict) { uint64_t t = touch[pg]; gxset.insert({t, pg}); gx_key[pg] = t; }
        } else touch.erase(pg);
    };
    uint64_t gx_evictions = 0, g_filtered = 0, g_refaults = 0;
    std::unordered_map<uint64_t, uint64_t> gdrop;             // page -> request idx at which the guest dropped its last block
    const uint64_t per_page = page / 4096;
    if (!guest_path.empty())
        GuestStateHook() = [&](uint64_t pg) -> int {
            auto c = gcnt.find(pg);
            if (c != gcnt.end() && c->second > 0) return c->second >= (int)per_page ? 2 : 1;
            auto d = gdrop.find(pg);
            return d != gdrop.end() && idx - d->second <= g_recent ? 3 : 0;
        };

    auto admit = [&](uint64_t ctx, uint64_t pg, bool is_pf) {
        if (cache.present(pg)) { if (oev && is_pf) oev->refresh(pg); return false; }
        if (oev && is_pf && oev->would_bypass(pg)) return false;
        if (g_evict && cache.size() >= cache.capacity && !gxset.empty()) {   // exclusive caching: drop what the guest holds
            const uint64_t v = gxset.begin()->second;
            cache.evict(v);
            { auto mi = meta.find(v); if (mi != meta.end()) emit_dump(v, mi->second); }
            meta.erase(v);
            if (evict) evict->on_evict(ctx, v);
            if (pf) pf->on_evict(ctx, v);
            gx_update(v);
            ++gx_evictions;
        }
        EvictRequest er;
        if (evict) evict->on_evict_request(ctx, pg, er);
        for (uint64_t i = 0; i < er.n_pages; ++i) {
            cache.evict(er.pages[i]);
            { auto mi = meta.find(er.pages[i]); if (mi != meta.end()) emit_dump(er.pages[i], mi->second); }
            meta.erase(er.pages[i]);
            if (evict) evict->on_evict(ctx, er.pages[i]);
            if (pf) pf->on_evict(ctx, er.pages[i]);
            gx_update(er.pages[i]);
        }
        auto e = cache.insert(pg);
        if (e.first) {  // capacity eviction (random victim) if policy didn't make room
            { auto mi = meta.find(e.second); if (mi != meta.end()) emit_dump(e.second, mi->second); }
            meta.erase(e.second);
            if (evict) evict->on_evict(ctx, e.second);
            if (pf) pf->on_evict(ctx, e.second);
            gx_update(e.second);
        }
        meta[pg] = Meta{is_pf, false, idx + delay, idx, cur_dist, cur_futile};
        if (evict) evict->on_admit(ctx, pg, is_pf);
        if (pf) pf->on_admit(ctx, pg, is_pf);
        if (g_evict) { touch[pg] = idx; gx_update(pg); }
        return true;
    };

    uint64_t pf_req = 0, dm_req = 0;   // per-request admissions, for --dump-req
    for (const Req& r : reqs) {
        pf_req = dm_req = 0;
        if (ev == "belady") static_cast<BeladyEvict*>(evict)->set_now(idx);
        if (oev) oev->set_now(idx);
        if (lev) lev->set_now(idx);
        while (gpos < gev.size() && gev[gpos].t <= r.t) {
            const GEv& g = gev[gpos++];
            int& c = gcnt[g.page];
            const int before = c;
            c += g.d;
            if (before > 0 && c == 0) gdrop[g.page] = idx;
            gx_update(g.page);
            if (g_refault && before > 0 && c == 0 && admit(r.worker, g.page, true)) { ++g_refaults; if (idx >= warm) ++pf_pages; }
        }
        const bool count = idx >= warm;
        uint64_t b0 = r.off / page;
        uint64_t bn = (r.off + r.len - 1) / page - b0 + 1;
        const uint64_t b1x = b0 + bn - 1;
        // hints addressed to this request: their pages are prefetched before the request is served
        PrefetchRequest hreq;
        while (hpos < hints.size() && hints[hpos].i <= idx) {
            if (pf) {
                PrefetchRequest hr;
                pf->on_hint(r.worker, hints[hpos].first, hints[hpos].n, hints[hpos].aux, hr);
                for (uint64_t k = 0; k < hr.fetch_count; ++k)
                    for (uint64_t pg = hr.fetch_ranges[k].block_offset; pg < hr.fetch_ranges[k].block_offset + hr.fetch_ranges[k].block_length; ++pg)
                        if (admit(r.worker, pg, true)) { ++pf_req; if (count) ++pf_pages; }
            }
            ++hpos;
        }
        PrefetchRequest* preq = new PrefetchRequest();
        if (pf) {
            auto ta = std::chrono::steady_clock::now();
            pf->on_request(r.worker, r.off, r.len, r.t);
            pf->on_prefetch_request(r.worker, b0, bn, *preq);
            pol_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ta).count();
        }
        uint64_t hits = 0;
        for (uint64_t k = 0; k < bn; ++k) {
            uint64_t pg = b0 + k;
            bool hit = cache.present(pg);
            if (hit) {
                auto it = meta.find(pg);
                if (it != meta.end()) {
                    if (it->second.prefetched && it->second.ready > idx) {
                        hit = false;
                        if (count) ++late;
                    }
                    if (hit && it->second.prefetched && !it->second.used) {
                        it->second.used = true;
                        if (count) ++pf_used;
                    }
                }
            }
            hits += hit;
            if (!cache.present(pg)) {
                if (count) ++dm_pages;
                if (admit(r.worker, pg, false)) ++dm_req;
            }
            if (evict) evict->on_access(r.worker, pg);
            if (pf) pf->on_access(r.worker, pg);
            if (g_evict && cache.present(pg)) { touch[pg] = idx; gx_update(pg); }
        }
        if (!clip.empty()) {
            auto it = std::upper_bound(clip.begin(), clip.end(), std::make_pair(b0, UINT64_MAX));
            uint64_t clo = 0, chi = UINT64_MAX;
            if (it != clip.begin() && std::prev(it)->second >= b0) { clo = std::prev(it)->first; chi = std::prev(it)->second; }
            uint64_t kept = 0;
            for (uint64_t i = 0; i < preq->fetch_count; ++i) {
                FetchRange fr = preq->fetch_ranges[i];
                uint64_t lo = std::max(fr.block_offset, clo), hi = std::min(fr.block_offset + fr.block_length - 1, chi);
                if (hi < lo || hi == UINT64_MAX - 1) continue;
                preq->fetch_ranges[kept++] = FetchRange{lo, hi - lo + 1};
            }
            preq->fetch_count = kept;
        }
        for (uint64_t i = 0; i < preq->fetch_count; ++i) {
            const FetchRange& fr = preq->fetch_ranges[i];
            for (uint64_t k = 0; k < fr.block_length; ++k) {
                uint64_t pgx = fr.block_offset + k;
                if (g_filter) { auto c = gcnt.find(pgx); if (c != gcnt.end() && c->second >= (int)g_filter) { ++g_filtered; continue; } }
                cur_dist = pgx > b1x ? (int64_t)(pgx - b1x) : -(int64_t)(b0 - pgx);
                { auto lu0 = last_use.find(pgx); cur_futile = (lu0 == last_use.end() || lu0->second <= idx); }
                const bool adm = admit(r.worker, pgx, true);
                if (adm) ++pf_req;
                if (adm && count) {
                    ++pf_pages;
                    auto lu = last_use.find(fr.block_offset + k);
                    if (lu == last_use.end() || lu->second <= idx) ++futile;
                }
            }
        }
        delete preq;
        if (dreq) std::fprintf(dreq, "%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)idx, (unsigned long long)r.off, (unsigned long long)r.len, (unsigned long long)hits, (unsigned long long)bn, (unsigned long long)pf_req, (unsigned long long)dm_req);
        if (count) {
            ++n_req;
            pg_hit += hits;
            pg_tot += bn;
            full_hit += (hits == bn);
        }
        ++idx;
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // traffic ratio: pages fetched from the backend relative to demand misses of the same stream
    // (a prefetcher that fetches exactly the demand misses, earlier, has ratio 1.0)
    double traffic = (double)(dm_pages + pf_pages);
    if (csv) {
        { std::string pc = pol; for (auto& ch : pc) if (ch == ',') ch = ';'; std::printf("%s,%s,%s,%llu,%llu,%llu,%llu,%.3f,%.3f,%llu,%llu,%.2f,%.3f,%llu,%llu,%.1f,%llu,%.0f\n", label.c_str(), pc.c_str(),
                    ev.c_str(), (unsigned long long)page, (unsigned long long)cap, (unsigned long long)delay,
                    (unsigned long long)n_req, 100.0 * full_hit / std::max<uint64_t>(1, n_req),
                    100.0 * pg_hit / std::max<uint64_t>(1, pg_tot), (unsigned long long)pf_pages, (unsigned long long)pf_used,
                    100.0 * pf_used / std::max<uint64_t>(1, pf_pages), traffic, (unsigned long long)dm_pages,
                    (unsigned long long)late, wall, (unsigned long long)futile, (double)pol_ns / std::max<uint64_t>(1, idx)); }
    } else {
        std::printf("%-34s reqs=%llu full_hit=%.2f%% page_hit=%.2f%% pf=%llu used=%.1f%% traffic=%.1fK pages (demand-miss %.1fK) late=%llu futile=%.1f%% pol=%.0fns/req [%.1fs]\n",
                    pol.c_str(), (unsigned long long)n_req, 100.0 * full_hit / std::max<uint64_t>(1, n_req),
                    100.0 * pg_hit / std::max<uint64_t>(1, pg_tot), (unsigned long long)pf_pages,
                    100.0 * pf_used / std::max<uint64_t>(1, pf_pages), traffic / 1e3, dm_pages / 1e3,
                    (unsigned long long)late, 100.0 * futile / std::max<uint64_t>(1, pf_pages), (double)pol_ns / std::max<uint64_t>(1, idx), wall);
    }
    for (auto& kv : meta) emit_dump(kv.first, kv.second);
    if (dump) std::fclose(dump);
    if (dreq) std::fclose(dreq);
    if (!guest_path.empty()) std::fprintf(stderr, "guest: %zu events, gx_evictions %llu, filtered %llu, refault_prefetches %llu\n", gev.size(), (unsigned long long)gx_evictions, (unsigned long long)g_filtered, (unsigned long long)g_refaults);
    delete pf;
    delete evict;
    if (!oracle_file.empty()) std::remove(oracle_file.c_str());
    return 0;
}
