// Block-level ports of published hardware/remote-memory prefetchers (see policy_lit.h for the list).
//
// Porting rules, applied the same way to every policy:
//   * a cache *line* of the original becomes one cache page here (64 KiB by default), and the original's 4 KiB
//     *page* (the region a spatial/delta prefetcher works inside) becomes a REGION of 2^rbits pages
//     (64 pages for SPP/Pythia as in the papers' 64 lines per 4 KiB page, 32 for Bingo's 2 KiB region);
//   * the original is triggered per L2 access; an NBD request covering n pages is fed as n accesses in
//     ascending order (that is what an L2 sees for a multi-line burst);
//   * there is no PC on a disk.  Features that are PC-only are dropped; PC+X features become X alone
//     (or X + request-length bucket where noted, the closest thing to "which code issued this").
//   * prefetches fill instantly (nbd_server/psim semantics), so "late" outcomes never occur here.
// Each class keeps its published table sizes, thresholds and update rules; deviations are commented.
#include "policy_lit.h"
#include "policy_api.h"
#include "policy_factory.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace policy {
namespace {

using u64 = std::uint64_t;
using KV = std::map<std::string, std::uint64_t>;
u64 Get(const KV& kv, const char* k, u64 d) { auto it = kv.find(k); return it == kv.end() ? d : it->second; }

// sort, drop pages of the current request and resident pages, merge into ranges
void Emit(Cache& cache, std::vector<u64>& pg, u64 b0, u64 b1, PrefetchRequest& req) {
    req.fetch_count = 0;
    std::sort(pg.begin(), pg.end());
    pg.erase(std::unique(pg.begin(), pg.end()), pg.end());
    for (u64 p : pg) {
        if ((p >= b0 && p <= b1) || cache.present(p)) continue;
        if (req.fetch_count && req.fetch_ranges[req.fetch_count - 1].block_offset + req.fetch_ranges[req.fetch_count - 1].block_length == p)
            ++req.fetch_ranges[req.fetch_count - 1].block_length;
        else if (req.fetch_count < MAX_PREFETCH_PAGES) req.fetch_ranges[req.fetch_count++] = FetchRange{p, 1};
    }
}

// pages admitted by a prefetch and not yet demanded ("prefetched hit" detection, accuracy counters)
struct PfTags {
    std::unordered_set<u64> s;
    void admit(u64 p, bool pf) { if (pf) s.insert(p); }
    bool take(u64 p) { return s.erase(p) > 0; }
};

int LenBucket(u64 n) { return n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : 5; }

// ============================================================================================================
// Best-Offset Prefetcher (Michaud, HPCA 2016).  Offsets 1..256 with prime factors {2,3,5} (52 of them, as in the
// paper's 256-offset list), 256-entry RR table, SCOREMAX 31, ROUNDMAX 100, BADSCORE 1, degree 1.  Triggered by a
// miss or a prefetched hit; on a trigger X it tests one offset d per access (score++ if X-d is in RR) and
// prefetches X+D.  RR receives Y-D when prefetched page Y fills, or the demand page when off.  Fills are instant
// in psim/nbd_server; `lat=K` delays the RR insertion by K requests to match `psim --delay K`, which is what makes
// BOP pick offsets that are far enough ahead to be timely (its main idea).
// ============================================================================================================
class BopPolicy : public CachePolicy {
    std::vector<int> offs_;
    int scoremax_, roundmax_, badscore_, degree_;
    struct Ctx {
        std::vector<int> score;
        std::vector<u64> rr = std::vector<u64>(256, 0);
        std::size_t idx = 0; int round = 0; int D = 1; bool on = true;
        u64 now = 0; std::deque<std::pair<u64, u64>> pending;   // (ready request index, RR base)
    };
    int lat_;
    std::unordered_map<u64, Ctx> ctx_;
    PfTags tags_;
    static std::size_t RRi(u64 p) { return (std::size_t)((p ^ (p >> 8) ^ (p >> 16)) & 255); }
    void rr_put(Ctx& C, u64 p) { C.rr[RRi(p)] = p + 1; }
    void rr_insert(Ctx& C, u64 p) { if (lat_) C.pending.emplace_back(C.now + lat_, p); else rr_put(C, p); }
    bool rr_has(Ctx& C, u64 p) { return C.rr[RRi(p)] == p + 1; }
    void learn(Ctx& C, u64 x) {
        const int d = offs_[C.idx];
        if (x >= (u64)d && rr_has(C, x - d)) ++C.score[C.idx];
        bool end = C.score[C.idx] >= scoremax_;
        if (++C.idx == offs_.size()) { C.idx = 0; if (++C.round >= roundmax_) end = true; }
        if (end) {
            std::size_t b = 0;
            for (std::size_t i = 1; i < offs_.size(); ++i) if (C.score[i] > C.score[b]) b = i;
            C.D = offs_[b]; C.on = C.score[b] > badscore_;
            std::fill(C.score.begin(), C.score.end(), 0); C.round = 0; C.idx = 0;
        }
    }
public:
    BopPolicy(Cache& c, const KV& kv) : CachePolicy(c) {
        const int maxoff = (int)Get(kv, "maxoff", 256);
        for (int n = 1; n <= maxoff; ++n) { int m = n; for (int f : {2, 3, 5}) while (m % f == 0) m /= f; if (m == 1) offs_.push_back(n); }
        scoremax_ = (int)Get(kv, "scoremax", 31); roundmax_ = (int)Get(kv, "roundmax", 100);
        badscore_ = (int)Get(kv, "badscore", 1); degree_ = (int)Get(kv, "degree", 1); lat_ = (int)Get(kv, "lat", 0);
    }
    void on_admit(u64, u64 p, bool pf) override { tags_.admit(p, pf); }
    void on_access(u64, u64 p) override { tags_.take(p); }
    void on_evict(u64, u64 p) override { tags_.take(p); }
    void on_prefetch_request(u64 cx, u64 b0, u64 bn, PrefetchRequest& req) override {
        req.fetch_count = 0;
        if (!bn) return;
        Ctx& C = ctx_[cx];
        if (C.score.empty()) C.score.assign(offs_.size(), 0);
        ++C.now;
        while (!C.pending.empty() && C.pending.front().first <= C.now) { rr_put(C, C.pending.front().second); C.pending.pop_front(); }
        std::vector<u64> out;
        for (u64 x = b0; x < b0 + bn; ++x) {
            const bool miss = !cache.present(x), pfhit = !miss && tags_.s.count(x);
            if (!miss && !pfhit) continue;
            learn(C, x);
            if (C.on) {
                for (int k = 1; k <= degree_; ++k) out.push_back(x + (u64)C.D * k);
                rr_insert(C, x);              // Y - D for Y = x + D (fill is instantaneous)
            } else {
                rr_insert(C, x);              // prefetch off: RR gets demand fills
            }
        }
        Emit(cache, out, b0, b0 + bn - 1, req);
    }
};

// ============================================================================================================
// Signature Path Prefetcher (Kim et al., MICRO 2016).  256-entry signature table (per region: last offset,
// 12-bit signature, sig' = (sig << 3) ^ delta), 512-entry pattern table (c_sig + 4 deltas with 4-bit c_delta,
// halved on saturation), 8-entry global history register for region crossings, global accuracy alpha =
// useful/issued.  Lookahead: P_1 = c_d/c_sig, P_d = alpha * c_d/c_sig * P_{d-1}; issue every delta with P >= Tp
// (25%), follow the most confident delta, stop when it drops below Tp.  No L2/LLC split (one cache level), so the
// fill threshold Tf is unused.  `cross=1` lets lookahead run into the next region (disks have no page boundary).
// The PPF subclass below replaces the Tp throttle with a perceptron.
// ============================================================================================================
class SppPolicy : public CachePolicy {
protected:
    struct Cand { u64 page, trigger; std::uint32_t sig; int delta, conf, depth; };
    int rbits_, R_, tp_, maxdepth_; bool cross_;
    struct STE { int last; std::uint32_t sig; std::list<u64>::iterator pos; };
    struct PTE { int c_sig = 0; std::array<int, 4> d{}, c{}; };
    struct GHR { std::uint32_t sig = 0; int conf = 0, last = 0, delta = 0; bool valid = false; };
    struct Ctx {
        std::unordered_map<u64, STE> st; std::list<u64> lru;
        std::vector<PTE> pt = std::vector<PTE>(512);
        std::array<GHR, 8> ghr{}; int ghr_next = 0;
    };
    std::unordered_map<u64, Ctx> ctx_;
    PfTags tags_;
    double c_total_ = 0, c_useful_ = 0;
    static std::uint32_t Enc(int d) { return (std::uint32_t)(d < 0 ? (64 | (-d & 63)) : (d & 63)); }
    static std::uint32_t Next(std::uint32_t sig, int d) { return ((sig << 3) ^ Enc(d)) & 0xFFF; }
    double alpha() const { return c_total_ < 1 ? 1.0 : std::min(1.0, c_useful_ / c_total_); }
    void pt_update(Ctx& C, std::uint32_t sig, int delta) {
        PTE& e = C.pt[sig & 511];
        int k = -1;
        for (int i = 0; i < 4; ++i) if (e.c[i] && e.d[i] == delta) k = i;
        if (k < 0) { k = 0; for (int i = 1; i < 4; ++i) if (e.c[i] < e.c[k]) k = i; e.d[k] = delta; e.c[k] = 0; }
        ++e.c[k]; ++e.c_sig;
        if (e.c_sig >= 15 || e.c[k] >= 15) { e.c_sig /= 2; for (int& x : e.c) x /= 2; if (!e.c_sig) e.c_sig = 1; }
    }
    // one demand access: update ST/PT, then lookahead; candidates appended to `out`
    void access(Ctx& C, u64 page, std::vector<Cand>& out) {
        const u64 reg = page >> rbits_; const int off = (int)(page & (R_ - 1));
        std::uint32_t sig;
        auto it = C.st.find(reg);
        if (it != C.st.end()) {
            STE& s = it->second;
            const int delta = off - s.last;
            C.lru.splice(C.lru.begin(), C.lru, s.pos);
            if (delta == 0) return;
            if (s.sig) pt_update(C, s.sig, delta);
            s.sig = Next(s.sig, delta); s.last = off; sig = s.sig;
        } else {
            sig = 0;
            for (auto& g : C.ghr)                      // a lookahead crossed into this region: inherit its signature
                if (g.valid && ((g.last + g.delta) & (R_ - 1)) == off) { sig = Next(g.sig, g.delta); g.valid = false; break; }
            if (C.st.size() >= 256) { C.st.erase(C.lru.back()); C.lru.pop_back(); }
            C.lru.push_front(reg);
            C.st[reg] = STE{off, sig, C.lru.begin()};
            if (!sig) return;                          // no history for a fresh region
        }
        // lookahead
        double P = 1.0;
        int base = off; u64 breg = reg; std::uint32_t cs = sig;
        for (int depth = 0; depth < maxdepth_; ++depth) {
            const PTE& e = C.pt[cs & 511];
            if (!e.c_sig) break;
            double best = -1; int bd = 0;
            for (int i = 0; i < 4; ++i) {
                if (!e.c[i]) continue;
                const double conf = (depth == 0 ? 1.0 : alpha()) * P * e.c[i] / e.c_sig;
                if (conf * 100 >= tp_) {
                    const long t = (long)base + e.d[i];
                    if ((t >= 0 && t < R_) || cross_) {
                        const long abs_pg = (long)(breg << rbits_) + t;
                        if (abs_pg >= 0) out.push_back(Cand{(u64)abs_pg, page, cs, e.d[i], (int)(conf * 100), depth});
                    }
                }
                if (conf > best) { best = conf; bd = e.d[i]; }
            }
            if (best * 100 < tp_) break;
            const long nb = (long)base + bd;
            if ((nb < 0 || nb >= R_) && !cross_) {     // leaves the region: remember for the next region (GHR)
                GHR& g = C.ghr[C.ghr_next++ & 7];
                g = GHR{cs, (int)(best * 100), base, bd, true};
                break;
            }
            long abs_nb = (long)(breg << rbits_) + nb;
            if (abs_nb < 0) break;
            breg = (u64)abs_nb >> rbits_; base = (int)((u64)abs_nb & (R_ - 1));
            P = best; cs = Next(cs, bd);
        }
    }
    virtual bool accept(Ctx&, const Cand&) { return true; }
    virtual void on_demand(u64) {}
public:
    SppPolicy(Cache& c, const KV& kv, int tp_default) : CachePolicy(c) {
        rbits_ = (int)Get(kv, "rbits", 6); R_ = 1 << rbits_;
        tp_ = (int)Get(kv, "tp", tp_default); maxdepth_ = (int)Get(kv, "depth", 64); cross_ = Get(kv, "cross", 0);
    }
    void on_admit(u64, u64 p, bool pf) override { tags_.admit(p, pf); }
    void on_access(u64, u64 p) override { if (tags_.take(p)) c_useful_ += 1; }
    void on_evict(u64, u64 p) override { tags_.take(p); }
    void on_prefetch_request(u64 cx, u64 b0, u64 bn, PrefetchRequest& req) override {
        req.fetch_count = 0;
        if (!bn) return;
        Ctx& C = ctx_[cx];
        std::vector<Cand> cands;
        std::vector<u64> out;
        for (u64 x = b0; x < b0 + bn; ++x) {
            on_demand(x);
            cands.clear();
            access(C, x, cands);
            for (const Cand& k : cands) {
                if ((k.page >= b0 && k.page < b0 + bn) || cache.present(k.page)) continue;
                if (std::find(out.begin(), out.end(), k.page) != out.end()) continue;
                if (!accept(C, k)) continue;
                out.push_back(k.page);
            }
        }
        c_total_ += (double)out.size();
        if (c_total_ > 1023) { c_total_ /= 2; c_useful_ /= 2; }
        Emit(cache, out, b0, b0 + bn - 1, req);
    }
};

// ============================================================================================================
// Perceptron-based Prefetch Filtering on SPP (Bhatia et al., ISCA 2019).  SPP is made aggressive (Tp lowered to
// `tp`, default 5) and every candidate is scored by a perceptron: one table of 5-bit weights (-16..15) per
// feature, summed.  Accept if sum >= tau (tau_hi; offset +100 in the spec, default -5 -> tau=95).  1024-entry
// direct-mapped prefetch and reject tables keep the features for training: a demand hit on a prefetched page or a
// demand to a rejected page trains up (if sum < theta_p), an eviction of a never-used prefetch trains down
// (if sum > theta_n).  theta_p = 90, theta_n = -80 (the public DPC-3 PPF code; the paper does not print them).
// Features (paper's 9 minus the 3 PC-based ones; PC^Depth -> Depth, PC^Delta -> Delta):
//   trigger address (hashed), trigger page ("cache line"), trigger region ("page address"), confidence,
//   region ^ confidence, signature ^ delta, depth, delta.
// ============================================================================================================
class PpfPolicy : public SppPolicy {
    static constexpr int kF = 8;
    std::array<std::vector<int>, kF> w_;
    std::array<int, kF> size_{4096, 4096, 4096, 128, 4096, 4096, 16, 128};
    int tau_, thp_, thn_;
    struct Rec { u64 tag = UINT64_MAX; std::array<int, kF> idx{}; };
    std::vector<Rec> pft_ = std::vector<Rec>(1024), rjt_ = std::vector<Rec>(1024);
    std::array<int, kF> Features(const Cand& k) const {
        const u64 t = k.trigger, reg = t >> rbits_;
        const int conf = std::min(127, k.conf);
        std::array<int, kF> f{
            (int)((t ^ (t >> 12) ^ (t >> 24)) & 4095), (int)(t & 4095), (int)(reg & 4095), conf,
            (int)((reg ^ (u64)conf) & 4095), (int)((k.sig ^ Enc(k.delta)) & 4095), std::min(15, k.depth), (int)(Enc(k.delta) & 127)};
        return f;
    }
    int Sum(const std::array<int, kF>& idx) const { int s = 0; for (int i = 0; i < kF; ++i) s += w_[i][idx[i]]; return s; }
    void Train(const std::array<int, kF>& idx, int dir) {
        for (int i = 0; i < kF; ++i) { int& x = w_[i][idx[i]]; x = std::clamp(x + dir, -16, 15); }
    }
protected:
    bool accept(Ctx&, const Cand& k) override {
        auto f = Features(k);
        const int s = Sum(f);
        Rec& r = (s >= tau_ ? pft_ : rjt_)[k.page & 1023];
        r.tag = k.page; r.idx = f;
        return s >= tau_;
    }
    void on_demand(u64 p) override {
        Rec& a = pft_[p & 1023];
        if (a.tag == p) { if (Sum(a.idx) < thp_) Train(a.idx, +1); a.tag = UINT64_MAX; }
        Rec& b = rjt_[p & 1023];
        if (b.tag == p) { if (Sum(b.idx) < thp_) Train(b.idx, +1); b.tag = UINT64_MAX; }
    }
public:
    PpfPolicy(Cache& c, const KV& kv) : SppPolicy(c, kv, 5) {
        for (int i = 0; i < kF; ++i) w_[i].assign(size_[i], 0);
        tau_ = (int)Get(kv, "tau", 95) - 100; thp_ = (int)Get(kv, "thp", 90); thn_ = (int)Get(kv, "thn", 20) - 100;
    }
    void on_evict(u64 cx, u64 p) override {
        Rec& a = pft_[p & 1023];
        if (a.tag == p && tags_.s.count(p)) { if (Sum(a.idx) > thn_) Train(a.idx, -1); a.tag = UINT64_MAX; }
        SppPolicy::on_evict(cx, p);
    }
};

// ============================================================================================================
// Bingo (Bakhshalipour et al., HPCA 2019).  Region = 32 pages (the paper's 2 KiB = 32 lines).  A trigger access
// (first access to a region not being tracked) opens a generation in the 128-entry accumulation table; every
// later access sets a footprint bit; the generation ends when any page of the region is evicted from the cache
// or the region falls out of the accumulation table, and its footprint is stored in the 16K-entry, 16-way
// pattern history table.  PHT sets are indexed by the short event and tagged by the long event, so one lookup
// finds both: a long-event match prefetches that footprint; otherwise all short-event matches vote and pages in
// >= vote% of them are prefetched (20%).  Events: long = PC+Address, short = PC+Offset in the paper; with no PC,
// `pc=1` (default) substitutes the trigger request's length bucket, `pc=0` uses no PC at all.
// ============================================================================================================
class BingoPolicy : public CachePolicy {
    static constexpr int kR = 32;
    int vote_; bool use_pc_;
    std::size_t at_cap_, sets_, ways_;
    struct AT { u64 trig; int toff, pcs; std::bitset<kR> fp; std::list<u64>::iterator pos; };
    struct PH { u64 ltag = UINT64_MAX; std::uint32_t stag = 0; std::bitset<kR> fp; u64 stamp = 0; };
    struct Ctx { std::unordered_map<u64, AT> at; std::list<u64> lru; std::vector<PH> pht; u64 clock = 0; };
    std::unordered_map<u64, Ctx> ctx_;
    std::unordered_map<u64, u64> region_ctx_;          // region -> context, for evictions
    int cur_pcs_ = 0;
    static u64 Mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }
    std::uint32_t Short(int pcs, int off) const { return (std::uint32_t)((pcs << 5) | off); }
    u64 Long(int pcs, u64 trig) const { return Mix(trig * 8 + (u64)pcs); }
    void commit(Ctx& C, u64 reg, const AT& a) {
        const std::uint32_t s = Short(a.pcs, a.toff);
        const std::size_t set = (std::size_t)(Mix(s) % sets_);
        PH* base = &C.pht[set * ways_];
        PH* victim = base;
        const u64 lt = Long(a.pcs, a.trig);
        for (std::size_t i = 0; i < ways_; ++i) {
            if (base[i].ltag == lt) { victim = &base[i]; break; }
            if (base[i].stamp < victim->stamp) victim = &base[i];
        }
        *victim = PH{lt, s, a.fp, ++C.clock};
        (void)reg;
    }
    void close(Ctx& C, u64 reg) {
        auto it = C.at.find(reg);
        if (it == C.at.end()) return;
        commit(C, reg, it->second);
        C.lru.erase(it->second.pos);
        C.at.erase(it);
        region_ctx_.erase(reg);
    }
public:
    BingoPolicy(Cache& c, const KV& kv) : CachePolicy(c) {
        vote_ = (int)Get(kv, "vote", 20); use_pc_ = Get(kv, "pc", 1);
        at_cap_ = Get(kv, "at", 128); ways_ = 16; sets_ = Get(kv, "pht", 16384) / ways_;
    }
    void on_request(u64, u64, u64 len, u64) override { cur_pcs_ = use_pc_ ? LenBucket((len + 65535) / 65536) : 0; }
    void on_evict(u64, u64 p) override {
        const u64 reg = p / kR;
        auto it = region_ctx_.find(reg);
        if (it != region_ctx_.end()) close(ctx_[it->second], reg);
    }
    void on_prefetch_request(u64 cx, u64 b0, u64 bn, PrefetchRequest& req) override {
        req.fetch_count = 0;
        if (!bn) return;
        Ctx& C = ctx_[cx];
        if (C.pht.empty()) C.pht.assign(sets_ * ways_, PH{});
        std::vector<u64> out;
        for (u64 x = b0; x < b0 + bn; ++x) {
            const u64 reg = x / kR; const int off = (int)(x % kR);
            auto it = C.at.find(reg);
            if (it != C.at.end()) { it->second.fp.set(off); C.lru.splice(C.lru.begin(), C.lru, it->second.pos); continue; }
            // trigger access: predict
            const std::uint32_t s = Short(cur_pcs_, off);
            const u64 lt = Long(cur_pcs_, x);
            const std::size_t set = (std::size_t)(Mix(s) % sets_);
            PH* base = &C.pht[set * ways_];
            const PH* exact = nullptr;
            std::array<int, kR> votes{}; int nmatch = 0;
            for (std::size_t i = 0; i < ways_; ++i) {
                if (base[i].ltag == UINT64_MAX) continue;
                if (base[i].ltag == lt) exact = &base[i];
                if (base[i].stag == s) { ++nmatch; for (int b = 0; b < kR; ++b) votes[b] += base[i].fp[b]; }
            }
            if (exact) {
                for (int b = 0; b < kR; ++b) if (exact->fp[b]) out.push_back(reg * kR + b);
            } else if (nmatch) {
                for (int b = 0; b < kR; ++b) if (votes[b] * 100 >= vote_ * nmatch && votes[b]) out.push_back(reg * kR + b);
            }
            // open a generation
            if (C.at.size() >= at_cap_) close(C, C.lru.back());
            C.lru.push_front(reg);
            AT a{x, off, cur_pcs_, {}, C.lru.begin()};
            a.fp.set(off);
            C.at[reg] = a;
            region_ctx_[reg] = cx;
        }
        Emit(cache, out, b0, b0 + bn - 1, req);
    }
};

// ============================================================================================================
// Pythia (Bera et al., MICRO 2021), basic configuration (Table 2): features PC+Delta and sequence of last-4
// deltas; actions {-6,-3,-1,0,1,3,4,5,10,11,12,16,22,23,30,32}; rewards R_AT=20, R_AL=12, R_CL=-12,
// R_IN^H=-14, R_IN^L=-8, R_NP^H=-2, R_NP^L=-4; alpha=0.0065, gamma=0.556, epsilon=0.002; QVStore of 2 vaults x 3
// planes x 128 feature rows x 16 actions, Q(S,A) = max over vaults of the vault's sum over planes, initialised to
// 1/(1-gamma); 256-entry FIFO evaluation queue, SARSA update when an entry leaves the EQ.  Deltas are per region
// of 64 pages (the paper's 4 KiB page of 64 lines).  No PC: "PC+Delta" -> request-length bucket + delta.
// No bandwidth signal in psim: `bw=0` (default) uses the low-bandwidth rewards, `bw=1` the high-bandwidth ones.
// `alpha` (x1e-4, default 65) can be raised to test whether the short streams starve the learning rate.
// ============================================================================================================
class PythiaPolicy : public CachePolicy {
    static constexpr int kA = 16, kRows = 128, kPlanes = 3, kVaults = 2;
    static constexpr std::array<int, kA> kAct{-6, -3, -1, 0, 1, 3, 4, 5, 10, 11, 12, 16, 22, 23, 30, 32};
    double alpha_, gamma_ = 0.556, eps_ = 0.002;
    double r_at_ = 20, r_cl_ = -12, r_in_, r_np_;
    int rbits_ = 6;
    struct EQ { std::array<std::array<int, kPlanes>, kVaults> rows; int a; u64 pf; bool has = false; double r = 0; };
    struct Trk { int last; std::array<int, 4> d{}; std::list<u64>::iterator pos; };
    struct Ctx {
        std::vector<double> q = std::vector<double>((std::size_t)kVaults * kPlanes * kRows * kA, 0.0);
        std::deque<EQ> eq;
        std::unordered_map<u64, Trk> trk; std::list<u64> lru;
        bool init = false;
    };
    std::unordered_map<u64, Ctx> ctx_;
    std::mt19937_64 rng_{12345};
    int cur_len_ = 0;
    static u64 H(u64 x, u64 seed) { x += seed * 0x9e3779b97f4a7c15ULL; x ^= x >> 31; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 29; return x; }
    double& Q(Ctx& C, int v, int p, int row, int a) { return C.q[(((std::size_t)v * kPlanes + p) * kRows + row) * kA + a]; }
    double VaultQ(Ctx& C, const EQ& e, int v, int a) { double s = 0; for (int p = 0; p < kPlanes; ++p) s += Q(C, v, p, e.rows[v][p], a); return s; }
    double StateQ(Ctx& C, const EQ& e, int a) { double m = -1e9; for (int v = 0; v < kVaults; ++v) m = std::max(m, VaultQ(C, e, v, a)); return m; }
    void sarsa(Ctx& C, const EQ& s1, const EQ& s2) {
        for (int v = 0; v < kVaults; ++v) {
            const double q1 = VaultQ(C, s1, v, s1.a), q2 = VaultQ(C, s2, v, s2.a);
            const double d = alpha_ * (s1.r + gamma_ * q2 - q1) / kPlanes;
            for (int p = 0; p < kPlanes; ++p) Q(C, v, p, s1.rows[v][p], s1.a) += d;
        }
    }
public:
    PythiaPolicy(Cache& c, const KV& kv) : CachePolicy(c) {
        alpha_ = Get(kv, "alpha", 65) * 1e-4;
        const bool high = Get(kv, "bw", 0);
        r_in_ = high ? -14 : -8; r_np_ = high ? -2 : -4;
    }
    void on_request(u64, u64, u64 len, u64) override { cur_len_ = LenBucket((len + 65535) / 65536); }
    void on_prefetch_request(u64 cx, u64 b0, u64 bn, PrefetchRequest& req) override {
        req.fetch_count = 0;
        if (!bn) return;
        Ctx& C = ctx_[cx];
        if (!C.init) {
            const double q0 = 1.0 / (1.0 - gamma_) / kPlanes;
            for (auto& x : C.q) x = q0;
            C.init = true;
        }
        std::vector<u64> out;
        const int R = 1 << rbits_;
        for (u64 x = b0; x < b0 + bn; ++x) {
            // 1. reward EQ entries whose prefetch this demand consumes (fills are instantaneous: always timely)
            for (auto& e : C.eq) if (!e.has && e.pf == x) { e.has = true; e.r = r_at_; }
            // 2. state: delta within the region and the last 4 deltas
            const u64 reg = x >> rbits_; const int off = (int)(x & (R - 1));
            int delta = 0; std::array<int, 4> hist{};
            auto it = C.trk.find(reg);
            if (it != C.trk.end()) {
                Trk& t = it->second;
                delta = off - t.last;
                for (int i = 3; i > 0; --i) t.d[i] = t.d[i - 1];
                t.d[0] = delta; t.last = off; hist = t.d;
                C.lru.splice(C.lru.begin(), C.lru, t.pos);
            } else {
                if (C.trk.size() >= 64) { C.trk.erase(C.lru.back()); C.lru.pop_back(); }
                C.lru.push_front(reg);
                C.trk[reg] = Trk{off, {}, C.lru.begin()};
            }
            const u64 f0 = (u64)cur_len_ * 256 + (u64)(delta + 128);
            u64 f1 = 0; for (int d : hist) f1 = f1 * 131 + (u64)(d + 64);
            EQ e;
            for (int p = 0; p < kPlanes; ++p) { e.rows[0][p] = (int)(H(f0, p + 1) % kRows); e.rows[1][p] = (int)(H(f1, p + 11) % kRows); }
            // 3. epsilon-greedy action
            int a;
            if (std::uniform_real_distribution<double>(0, 1)(rng_) <= eps_) a = (int)(rng_() % kA);
            else { a = 0; double best = -1e18; for (int k = 0; k < kA; ++k) { double v = StateQ(C, e, k); if (v > best) { best = v; a = k; } } }
            e.a = a;
            const int o = kAct[a];
            if (o == 0) { e.has = true; e.r = r_np_; e.pf = UINT64_MAX; }
            else {
                const long t = (long)off + o;
                if (t < 0 || t >= R) { e.has = true; e.r = r_cl_; e.pf = UINT64_MAX; }   // leaves the region: not issued
                else { e.pf = (reg << rbits_) + (u64)t; out.push_back(e.pf); }
            }
            // 4. EQ insert; the leaving entry is scored and trained against its successor (SARSA)
            C.eq.push_back(e);
            if (C.eq.size() > 256) {
                EQ old = C.eq.front(); C.eq.pop_front();
                if (!old.has) { old.has = true; old.r = r_in_; }
                sarsa(C, old, C.eq.front());
            }
        }
        Emit(cache, out, b0, b0 + bn - 1, req);
    }
};

// ============================================================================================================
// Leap (Al Maruf & Chowdhury, USENIX ATC 2020), Algorithms 1-2.  Access history of H=32 page deltas; FindTrend
// runs Boyer-Moore majority over the last w deltas, w = 4, 8, 16, 32 (a delta is the trend only if it occurs
// >= floor(w/2)+1 times).  Prefetch window: no prefetched hits since the last prefetch -> 1 if a trend exists,
// else 0 (suspend); hits -> next power of two >= C_hit+1; capped at PW_max = 8; never below half the previous
// window.  With a trend, prefetch PW pages along it; without one, PW pages along the last trend.  Leap acts on
// faults (misses); every page of a request is logged as an access, and a request with any missing page triggers.
// ============================================================================================================
class LeapPolicy : public CachePolicy {
    int hsize_, w0_, pwmax_;
    struct Ctx { std::deque<long> hist; u64 last = 0; bool have = false; int chit = 0, pw_prev = 0; long last_trend = 1; };
    std::unordered_map<u64, Ctx> ctx_;
    PfTags tags_;
    long FindTrend(const Ctx& C, bool& found) const {
        found = false;
        for (int w = w0_; w <= hsize_; w *= 2) {
            if ((int)C.hist.size() < w) break;
            long cand = 0; int cnt = 0;
            for (int i = 0; i < w; ++i) { long d = C.hist[C.hist.size() - 1 - i]; if (!cnt) { cand = d; cnt = 1; } else cnt += (d == cand) ? 1 : -1; }
            int occ = 0;
            for (int i = 0; i < w; ++i) occ += C.hist[C.hist.size() - 1 - i] == cand;
            if (occ >= w / 2 + 1) { found = true; return cand; }
        }
        return 0;
    }
public:
    LeapPolicy(Cache& c, const KV& kv) : CachePolicy(c) {
        hsize_ = (int)Get(kv, "h", 32); w0_ = (int)Get(kv, "w0", 4); pwmax_ = (int)Get(kv, "pwmax", 8);
    }
    void on_admit(u64, u64 p, bool pf) override { tags_.admit(p, pf); }
    void on_evict(u64, u64 p) override { tags_.take(p); }
    void on_prefetch_request(u64 cx, u64 b0, u64 bn, PrefetchRequest& req) override {
        req.fetch_count = 0;
        if (!bn) return;
        Ctx& C = ctx_[cx];
        bool fault = false;
        for (u64 x = b0; x < b0 + bn; ++x) {
            if (!cache.present(x)) fault = true;
            else if (tags_.take(x)) ++C.chit;
            if (C.have) { C.hist.push_back((long)x - (long)C.last); if ((int)C.hist.size() > hsize_) C.hist.pop_front(); }
            C.last = x; C.have = true;
        }
        if (!fault) return;
        bool found;
        const long trend = FindTrend(C, found);
        int pw;
        if (C.chit == 0) pw = found ? 1 : 0;
        else { pw = 1; while (pw < C.chit + 1) pw <<= 1; }
        pw = std::min(pw, pwmax_);
        if (pw < C.pw_prev / 2) pw = C.pw_prev / 2;
        C.chit = 0; C.pw_prev = pw;
        if (found && trend != 0) C.last_trend = trend;
        const long step = found && trend != 0 ? trend : C.last_trend;
        std::vector<u64> out;
        const u64 pt = b0 + bn - 1;
        for (int k = 1; k <= pw; ++k) { long p = (long)pt + step * k; if (p >= 0) out.push_back((u64)p); }
        Emit(cache, out, b0, b0 + bn - 1, req);
    }
};

// ============================================================================================================
// `learned` + a PPF-style perceptron filter (our hybrid, not a published design): run nextreq + neighborhood
// aggressively (theta, default 15) and pass each candidate through an online perceptron exactly as PPF does for
// SPP (5-bit weights, prefetch/reject tables, train up on a demand to a prefetched or rejected page when
// sum < theta_p, down when an unused prefetch is evicted and sum > theta_n).  The tables are 64K entries instead
// of 1K because one disk request proposes up to hundreds of pages.  Features (no PC on a disk):
//   which part proposed it (nextreq / neighborhood / both) x signed distance bin; request length bucket x
//   distance bin; is the request contiguous with the previous one; trigger page; trigger region (32 pages);
//   candidate region; candidate page.
// ============================================================================================================
class LearnedPpfPolicy : public CachePolicy {
    static constexpr int kF = 7;
    std::unique_ptr<CachePolicy> nr_, nb_;
    std::array<std::vector<int>, kF> w_;
    std::array<int, kF> size_{3 * 64, 6 * 64, 2 * 64, 4096, 4096, 4096, 4096};
    int tau_, thp_, thn_; bool addr_;
    struct Rec { u64 tag = UINT64_MAX; std::array<int, kF> idx{}; };
    std::vector<Rec> pft_ = std::vector<Rec>(65536), rjt_ = std::vector<Rec>(65536);
    PfTags tags_;
    u64 prev_end_ = UINT64_MAX; int len_b_ = 0;
    static int DistBin(long d) {               // signed distance from the request edge, 32 log-ish bins per side
        const long a = d < 0 ? -d : d;
        const int b = a <= 8 ? (int)a : std::min(31, 8 + (int)std::floor(std::log2((double)a / 8.0) * 4));
        return (d < 0 ? 32 : 0) + b;
    }
    static u64 Mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }
    int Sum(const std::array<int, kF>& f) const { int s = 0; for (int i = 0; i < kF; ++i) s += w_[i][f[i]]; return s; }
    void Train(const std::array<int, kF>& f, int dir) { for (int i = 0; i < kF; ++i) { int& x = w_[i][f[i]]; x = std::clamp(x + dir, -16, 15); } }
public:
    LearnedPpfPolicy(Cache& c, const KV& kv) : CachePolicy(c) {
        const u64 theta = Get(kv, "theta", 15);
        nr_.reset(new NextRequestPolicy(c, NextRequestParams(Spec{}, std::max<u64>(1, theta / 4))));
        nb_.reset(new NeighborhoodPrefetchPolicy(c, NeighborhoodParams(Spec{}, theta)));
        for (int i = 0; i < kF; ++i) w_[i].assign(size_[i], 0);
        tau_ = (int)Get(kv, "tau", 100) - 100; thp_ = (int)Get(kv, "thp", 90); thn_ = (int)Get(kv, "thn", 20) - 100;
        addr_ = Get(kv, "addr", 1);                // addr=0: drop the four address features (shape-only filter)
    }
    void on_request(u64 cx, u64 off, u64 len, u64 t) override {
        nr_->on_request(cx, off, len, t); nb_->on_request(cx, off, len, t);
        len_b_ = LenBucket((len + 65535) / 65536);
    }
    void on_admit(u64 cx, u64 p, bool pf) override { nr_->on_admit(cx, p, pf); nb_->on_admit(cx, p, pf); tags_.admit(p, pf); }
    void on_access(u64 cx, u64 p) override {
        nr_->on_access(cx, p); nb_->on_access(cx, p);
        tags_.take(p);
    }
    void on_evict(u64 cx, u64 p) override {
        nr_->on_evict(cx, p); nb_->on_evict(cx, p);
        Rec& a = pft_[Mix(p) & 65535];
        if (a.tag == p && tags_.s.count(p)) { if (Sum(a.idx) > thn_) Train(a.idx, -1); a.tag = UINT64_MAX; }
        tags_.take(p);
    }
    void on_prefetch_request(u64 cx, u64 b0, u64 bn, PrefetchRequest& req) override {
        req.fetch_count = 0;
        if (!bn) return;
        const u64 b1 = b0 + bn - 1;
        for (u64 x = b0; x <= b1; ++x) {           // feedback from this demand
            Rec& a = pft_[Mix(x) & 65535];
            if (a.tag == x) { if (Sum(a.idx) < thp_) Train(a.idx, +1); a.tag = UINT64_MAX; }
            Rec& b = rjt_[Mix(x) & 65535];
            if (b.tag == x) { if (Sum(b.idx) < thp_) Train(b.idx, +1); b.tag = UINT64_MAX; }
        }
        const bool contig = prev_end_ != UINT64_MAX && (b0 == prev_end_ + 1 || b0 == prev_end_);
        prev_end_ = b1;
        PrefetchRequest r1, r2;
        nr_->on_prefetch_request(cx, b0, bn, r1);
        nb_->on_prefetch_request(cx, b0, bn, r2);
        std::unordered_map<u64, int> src;          // page -> bit 1 nextreq, bit 2 neighborhood
        for (u64 i = 0; i < r1.fetch_count; ++i) for (u64 k = 0; k < r1.fetch_ranges[i].block_length; ++k) src[r1.fetch_ranges[i].block_offset + k] |= 1;
        for (u64 i = 0; i < r2.fetch_count; ++i) for (u64 k = 0; k < r2.fetch_ranges[i].block_length; ++k) src[r2.fetch_ranges[i].block_offset + k] |= 2;
        std::vector<u64> out;
        for (auto& [p, s] : src) {
            if ((p >= b0 && p <= b1) || cache.present(p)) continue;
            const long d = p > b1 ? (long)(p - b1) : -(long)(b0 - p);
            const int db = DistBin(d);
            std::array<int, kF> f{(s - 1) * 64 + db, len_b_ * 64 + db, (contig ? 64 : 0) + db,
                                  (int)(Mix(b0) & 4095), (int)(Mix(b0 >> 5) & 4095), (int)(Mix(p >> 5) & 4095), (int)(Mix(p) & 4095)};
            if (!addr_) f[3] = f[4] = f[5] = f[6] = 0;
            const int sum = Sum(f);
            Rec& r = (sum >= tau_ ? pft_ : rjt_)[Mix(p) & 65535];
            r.tag = p; r.idx = f;
            if (sum >= tau_) out.push_back(p);
        }
        Emit(cache, out, b0, b1, req);
    }
};

}  // namespace

CachePolicy* MakeLitPolicy(const std::string& n, const KV& kv, Cache& cache) {
    if (n == "learnedppf") return new LearnedPpfPolicy(cache, kv);
    if (n == "bop") return new BopPolicy(cache, kv);
    if (n == "spp") return new SppPolicy(cache, kv, 25);
    if (n == "ppf") return new PpfPolicy(cache, kv);
    if (n == "bingo") return new BingoPolicy(cache, kv);
    if (n == "pythia") return new PythiaPolicy(cache, kv);
    if (n == "leap") return new LeapPolicy(cache, kv);
    return nullptr;
}

}  // namespace policy
