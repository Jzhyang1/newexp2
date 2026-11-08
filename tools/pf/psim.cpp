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

int main(int argc, char** argv) {
    std::string trace, pol = "none", ev = "lru", label;
    uint64_t page = 65536, cap = 4096, delay = 0, limit = 0, warm = 0, skip = 0, tail_off = 0;
    bool csv = false, header = false;
    std::string dump_path, dumpreq_path, clip_path;
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
    Cache cache(cap, 30000, 300000, page);
    CachePolicy* evict = ev == "belady" ? (CachePolicy*)new BeladyEvict(cache, reqs) : MakeEvictPolicy(ev, cache);
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
    if (dreq) std::fputs("idx,off,len,hits,pages\n", dreq);
    FILE* dump = dump_path.empty() ? nullptr : std::fopen(dump_path.c_str(), "w");
    if (dump) std::fputs("born,page,dist,used,futile\n", dump);
    auto emit_dump = [&](uint64_t pg, const Meta& m) { if (dump && m.prefetched) std::fprintf(dump, "%llu,%llu,%lld,%d,%d\n", (unsigned long long)m.born, (unsigned long long)pg, (long long)m.dist, (int)m.used, (int)m.futile); };

    auto admit = [&](uint64_t ctx, uint64_t pg, bool is_pf) {
        if (cache.present(pg)) return false;
        EvictRequest er;
        if (evict) evict->on_evict_request(ctx, pg, er);
        for (uint64_t i = 0; i < er.n_pages; ++i) {
            cache.evict(er.pages[i]);
            { auto mi = meta.find(er.pages[i]); if (mi != meta.end()) emit_dump(er.pages[i], mi->second); }
            meta.erase(er.pages[i]);
            if (evict) evict->on_evict(ctx, er.pages[i]);
            if (pf) pf->on_evict(ctx, er.pages[i]);
        }
        auto e = cache.insert(pg);
        if (e.first) {  // capacity eviction (random victim) if policy didn't make room
            { auto mi = meta.find(e.second); if (mi != meta.end()) emit_dump(e.second, mi->second); }
            meta.erase(e.second);
            if (evict) evict->on_evict(ctx, e.second);
            if (pf) pf->on_evict(ctx, e.second);
        }
        meta[pg] = Meta{is_pf, false, idx + delay, idx, cur_dist, cur_futile};
        if (evict) evict->on_admit(ctx, pg, is_pf);
        if (pf) pf->on_admit(ctx, pg, is_pf);
        return true;
    };

    for (const Req& r : reqs) {
        if (ev == "belady") static_cast<BeladyEvict*>(evict)->set_now(idx);
        const bool count = idx >= warm;
        uint64_t b0 = r.off / page;
        uint64_t bn = (r.off + r.len - 1) / page - b0 + 1;
        const uint64_t b1x = b0 + bn - 1;
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
                admit(r.worker, pg, false);
            }
            if (evict) evict->on_access(r.worker, pg);
            if (pf) pf->on_access(r.worker, pg);
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
                cur_dist = pgx > b1x ? (int64_t)(pgx - b1x) : -(int64_t)(b0 - pgx);
                { auto lu0 = last_use.find(pgx); cur_futile = (lu0 == last_use.end() || lu0->second <= idx); }
                if (admit(r.worker, pgx, true) && count) {
                    ++pf_pages;
                    auto lu = last_use.find(fr.block_offset + k);
                    if (lu == last_use.end() || lu->second <= idx) ++futile;
                }
            }
        }
        delete preq;
        if (dreq) std::fprintf(dreq, "%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)idx, (unsigned long long)r.off, (unsigned long long)r.len, (unsigned long long)hits, (unsigned long long)bn);
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
    delete pf;
    delete evict;
    if (!oracle_file.empty()) std::remove(oracle_file.c_str());
    return 0;
}
