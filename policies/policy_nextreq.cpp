#include "policy_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace policy {

// Per-stream next-request predictor (see NextRequestPolicy in policy_api.h).
struct NextRequestPolicy::Impl {
    static constexpr int kLenBins = 257;       // request length in 4 KiB units (1 MiB and longer share the last bin)
    static constexpr int kGapBins = 12;

    struct Stream {
        std::uint64_t end;                     // byte just past the stream's last request
        std::uint64_t start = 0;               // first byte of the stream's first request
        std::uint32_t l1 = 0, l2 = 0;          // last two request lengths, 4 KiB units
        std::uint32_t nreq = 1;
        std::uint64_t last = 0;
        std::list<std::uint64_t>::iterator pos;   // place in Ctx::lru
        int trial = -1;                        // continuation-table slot opened by the last request, -1 once resolved
    };
    struct LenDist {                           // what the length of a stream's next request tends to be
        std::vector<float> cnt, suf;           // suf[i] = weight of lengths >= i (valid while !dirty)
        float total = 0;
        int max_bin = 0;                       // longest length seen
        bool dirty = true;
        void add(std::uint32_t k, float decay) {
            if (cnt.empty()) cnt.assign(kLenBins, 0.f);
            if (decay < 1.f) { for (auto& c : cnt) c *= decay; total *= decay; }
            int b = (int)std::min<std::uint32_t>(k, kLenBins - 1);
            cnt[b] += 1.f; total += 1.f; max_bin = std::max(max_bin, b); dirty = true;
        }
        const std::vector<float>& Suffix() {
            if (dirty) {
                suf.assign(kLenBins + 1, 0.f);
                for (int i = max_bin; i >= 0; --i) suf[i] = suf[i + 1] + cnt[i];
                dirty = false;
            }
            return suf;
        }
    };
    struct GapDist {                           // how far past the previous end the next request starts
        std::array<float, kGapBins> w{};
        std::array<double, kGapBins> sum{};    // bytes, to recover a representative gap per bin
        float total = 0;
        static int Bin(std::uint64_t g4k) { return g4k == 0 ? 0 : g4k == 1 ? 1 : g4k == 2 ? 2 : std::min<int>(kGapBins - 1, 3 + (int)std::floor(std::log2((double)g4k - 0.5)) - 1); }
        void add(std::uint64_t gap_bytes, float decay) {
            for (auto& x : w) x *= decay;
            for (auto& x : sum) x *= decay;
            total *= decay;
            int b = Bin((gap_bytes + 4095) / 4096);
            w[b] += 1.f; sum[b] += (double)gap_bytes; total += 1.f;
        }
    };
    struct Ctx {
        std::unordered_map<std::uint64_t, Stream> streams;
        std::map<std::uint64_t, std::uint64_t> by_end;    // end byte -> stream id
        std::list<std::uint64_t> lru;                     // stream ids, most recently used first
        std::unordered_map<std::uint32_t, LenDist> len_exact;   // (l1,l2)
        std::array<LenDist, 144> len_bucket2;                    // (bucket l1, bucket l2)
        std::array<LenDist, 12> len_bucket1;
        LenDist len_global;
        GapDist gap;
        std::array<float, 16 * 12> trials{}, succ{};       // P(a stream state gets another request)
        std::uint64_t next_id = 1, now = 0;
        std::uint64_t cur_off = 0, cur_len = 0;
        bool have = false;
    };
    std::unordered_map<std::uint64_t, Ctx> ctx;
    Params p;
    static int LenBucket(std::uint32_t k) { return k <= 1 ? 0 : k <= 2 ? 1 : k <= 4 ? 2 : k <= 8 ? 3 : k <= 15 ? 4 : k == 16 ? 5 : k <= 31 ? 6 : k == 32 ? 7 : k <= 63 ? 8 : k <= 64 ? 9 : k <= 127 ? 10 : 11; }
    static std::uint32_t Key(std::uint32_t l1, std::uint32_t l2) { return (std::min<std::uint32_t>(l1, 2047) << 11) | std::min<std::uint32_t>(l2, 2047); }
};

NextRequestPolicy::NextRequestPolicy(Cache& cache, const Params& params) : CachePolicy(cache), impl_(new Impl) { impl_->p = params; }
NextRequestPolicy::~NextRequestPolicy() = default;

void NextRequestPolicy::on_request(std::uint64_t context, std::uint64_t off, std::uint64_t len, std::uint64_t) {
    auto& C = impl_->ctx[context];
    C.cur_off = off; C.cur_len = len; C.have = true;
}

void NextRequestPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t b0, std::uint64_t bn, PrefetchRequest& req) {
    using I = Impl;
    auto& P = impl_->p;
    auto& C = impl_->ctx[context];
    req.fetch_count = 0;
    if (bn == 0) { C.have = false; return; }       // zero-length read
    ++C.now;
    const std::uint64_t psz = cache.page_size;
    const std::uint64_t off = C.have ? C.cur_off : b0 * psz;
    const std::uint64_t len = C.have ? C.cur_len : bn * psz;
    C.have = false;
    const std::uint32_t k = (std::uint32_t)std::max<std::uint64_t>(1, (len + 4095) / 4096);

    // 1. which stream does this request continue?  a stream whose end lies just before our start (or inside it)
    std::uint64_t sid = 0;
    {
        const std::uint64_t slack = P.gap_kb * 1024;
        const std::uint64_t lo_key = off > slack ? off - slack : 0;
        std::uint64_t best_d = UINT64_MAX;
        for (auto it = C.by_end.lower_bound(lo_key); it != C.by_end.end() && it->first <= off + P.overlap_kb * 1024; ++it) {
            std::uint64_t d = it->first > off ? it->first - off : off - it->first;
            if (d < best_d) { best_d = d; sid = it->second; }
        }
    }
    I::Stream* s;
    if (sid) {
        s = &C.streams[sid];
        if (s->trial >= 0) { C.succ[s->trial] += 1.f; s->trial = -1; }
        const std::uint64_t gap = off > s->end ? off - s->end : 0;
        C.gap.add(gap, P.decay);
        if (s->l1) {
            if (C.len_exact.size() > 4096) C.len_exact.clear();      // bounded: ~2 KiB per (l1, l2) pair
            C.len_exact[I::Key(s->l1, s->l2)].add(k, P.decay);
            C.len_bucket2[I::LenBucket(s->l1) * 12 + (s->l2 ? I::LenBucket(s->l2) : 0)].add(k, P.decay);
            C.len_bucket1[I::LenBucket(s->l1)].add(k, P.decay);
        }
        C.len_global.add(k, P.decay);
        { auto bi = C.by_end.find(s->end); if (bi != C.by_end.end() && bi->second == sid) C.by_end.erase(bi); }
        s->l2 = s->l1; s->l1 = k; ++s->nreq;
        s->end = off + len; s->last = C.now;
        C.lru.splice(C.lru.begin(), C.lru, s->pos);
    } else {
        if (C.streams.size() >= P.streams) {
            const std::uint64_t victim = C.lru.back();
            C.lru.pop_back();
            auto bi = C.by_end.find(C.streams[victim].end);
            if (bi != C.by_end.end() && bi->second == victim) C.by_end.erase(bi);
            C.streams.erase(victim);
        }
        sid = C.next_id++;
        I::Stream ns; ns.end = off + len; ns.start = off; ns.l1 = k; ns.last = C.now;
        C.lru.push_front(sid);
        ns.pos = C.lru.begin();
        s = &(C.streams[sid] = ns);
    }
    C.by_end[s->end] = sid;
    auto sizeb = [&](std::uint64_t consumed) { return std::min<int>(15, (int)std::floor(std::log2((double)(consumed / 65536 + 1)) * 1.5)); };
    const int slot = (P.use_consumed ? sizeb(s->end - s->start) : std::min<int>(7, (int)s->nreq - 1)) * 12 + I::LenBucket(s->l1);
    C.trials[slot] += 1.f;
    s->trial = slot;

    // 2. predict the next `look` requests of this stream: P(each exists) x P(gap) x P(length), page by page
    const double theta = P.theta_pct / 100.0;
    struct PageP { std::uint64_t pg; float p; };
    thread_local std::vector<PageP> pages;
    pages.clear();
    std::uint32_t l1 = s->l1, l2 = s->l2;
    std::uint64_t e = s->end;
    double reach = 1.0;
    struct GapC { double gap; float p; };
    std::array<GapC, 4> gaps{};
    int ngap = 0;
    if (C.gap.total >= 4.f) {                    // the (up to) four most common gap sizes
        std::array<int, I::kGapBins> order;
        for (int b = 0; b < I::kGapBins; ++b) order[b] = b;
        std::partial_sort(order.begin(), order.begin() + 4, order.end(), [&](int x, int y) { return C.gap.w[x] > C.gap.w[y]; });
        for (int i = 0; i < 4; ++i) {
            int b = order[i];
            if (C.gap.w[b] > 0.03f * C.gap.total) gaps[ngap++] = {C.gap.sum[b] / C.gap.w[b], C.gap.w[b] / C.gap.total};
        }
    }
    if (!ngap) gaps[ngap++] = {0.0, 1.f};
    double gmax = 0;
    for (int i = 0; i < ngap; ++i) gmax = std::max(gmax, gaps[i].gap);
    for (std::uint32_t step = 0; step < P.look; ++step) {
        const int sl = (P.use_consumed ? sizeb(e - s->start) : std::min<int>(7, (int)s->nreq - 1 + (int)step)) * 12 + I::LenBucket(l1);
        const float pc = step == 0 ? (C.succ[slot] + P.prior_cont) / (C.trials[slot] + 1.f)
                                   : (C.trials[sl] > 0 ? (C.succ[sl] + P.prior_cont) / (C.trials[sl] + 1.f) : P.prior_cont);
        reach *= pc;
        if (reach < theta * 0.5) break;
        // length distribution: most specific level with enough evidence
        I::LenDist* ld = nullptr;
        { auto it = C.len_exact.find(I::Key(l1, l2)); if (it != C.len_exact.end() && it->second.total >= P.min_evidence) ld = &it->second; }
        if (!ld) { auto& b = C.len_bucket2[I::LenBucket(l1) * 12 + (l2 ? I::LenBucket(l2) : 0)]; if (b.total >= P.min_evidence) ld = &b; }
        if (!ld) { auto& b = C.len_bucket1[I::LenBucket(l1)]; if (b.total >= P.min_evidence) ld = &b; }
        if (!ld && C.len_global.total >= 2.f) ld = &C.len_global;
        const float* suf = nullptr;
        float inv_total = 0;
        double mode_len = l1 * 4096.0;
        int max_bin = (int)l1;
        if (ld) {
            suf = ld->Suffix().data();
            inv_total = 1.f / std::max(1.f, ld->total);
            max_bin = ld->max_bin;
            float best = 0;
            for (int i = 0; i <= max_bin; ++i) if (ld->cnt[i] > best) { best = ld->cnt[i]; mode_len = i * 4096.0; }
        }
        // S(x) = P(len > x bytes) = suf[floor(x/4096) + 1] / total   (len4k = ceil(len/4096))
        auto S = [&](double x_bytes) -> float {
            if (x_bytes < 0) return 1.f;
            long idx = (long)(x_bytes * (1.0 / 4096.0)) + 1;
            if (!suf) return x_bytes < (double)l1 * 4096.0 ? 0.6f : 0.f;
            return idx > max_bin ? 0.f : suf[idx] * inv_total;
        };
        const std::uint64_t first_pg = e / psz;
        const std::uint64_t last_pg = std::min<std::uint64_t>(first_pg + P.max_pages, (e + (std::uint64_t)gmax + (std::uint64_t)max_bin * 4096) / psz);
        for (std::uint64_t pg = first_pg; pg <= last_pg; ++pg) {
            const double Pj = (double)(pg * psz);
            float pj = 0;
            for (int i = 0; i < ngap; ++i) {
                const double start = (double)e + gaps[i].gap;
                if (start >= Pj + (double)psz) continue;      // starts after this page
                pj += gaps[i].p * S(Pj - start);
            }
            const float pr = (float)(pj * reach);
            if (pr >= theta) pages.push_back({pg, pr});
        }
        // advance along the most likely continuation
        double g_top = 0; float gp = -1;
        for (int i = 0; i < ngap; ++i) if (gaps[i].p > gp) { gp = gaps[i].p; g_top = gaps[i].gap; }
        e += (std::uint64_t)(g_top + mode_len);
        l2 = l1; l1 = (std::uint32_t)std::max(1.0, mode_len / 4096.0);
    }
    // pages that cleared theta, merged into ranges (steps overlap, so sort and fuse)
    std::sort(pages.begin(), pages.end(), [](const PageP& a, const PageP& b) { return a.pg < b.pg; });
    std::uint64_t run_first = 0, run_last = 0;
    bool in_run = false;
    for (const PageP& pp : pages) {
        if (in_run && pp.pg <= run_last + 1) { run_last = std::max(run_last, pp.pg); continue; }
        if (in_run && req.fetch_count < MAX_PREFETCH_PAGES) req.fetch_ranges[req.fetch_count++] = FetchRange{run_first, run_last - run_first + 1};
        in_run = true; run_first = run_last = pp.pg;
    }
    if (in_run && req.fetch_count < MAX_PREFETCH_PAGES) req.fetch_ranges[req.fetch_count++] = FetchRange{run_first, run_last - run_first + 1};
}

}  // namespace policy
