#include "policy_api.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace policy {

// Learned neighborhood prefetcher (see NeighborhoodPrefetchPolicy in policy_api.h).
//
// The model.  Every request is an "anchor".  For `window` requests after it, we record which pages within +-dmax
// pages of the anchor were requested (the label).  When the anchor retires, each candidate page of it becomes one
// training example for a table of counts indexed by
//     (request context, side, distance bin, candidate state)
//   request context  = request length bucket x length of the sequential chain the request extends x whether it
//                      lands in an already active neighborhood
//   candidate state  = hole class (is the candidate page enclosed by two recent requests, and how far apart are
//                      they?) x footprint class (was the candidate inside what followed the last request that
//                      started at the same page?)
// A new request is scored with the same table and prefetches the pages whose P(requested soon) >= theta.
//
// Why these features.  The hole class is what out-of-order windows need: the guest's mmap read-around centres a
// 128 KiB window on the first fault of a list and the rest of the list fills in around it in no fixed order, but
// pages enclosed by two windows are always asked for.  The footprint class lets a revisited region be prefetched
// from memory instead of re-learned.  Everything else (shape of the request, chain length) separates sequential
// scans, where far-ahead pages are likely, from isolated reads, where nothing near is.
struct NeighborhoodPrefetchPolicy::Impl {
    static constexpr int kD = 256;                 // largest distance (pages) any table can hold; Params::dmax <= kD
    static constexpr int kBins = 20;
    static constexpr int kLenB = 6, kChainB = 6, kNear = 2;
    static constexpr int kCtx = kLenB * kChainB * kNear;
    static constexpr int kState = 9;               // hole class (3) x footprint class (3)

    struct BinDef { int lo, hi; };                 // distances [lo, hi] (pages from the request edge)
    std::array<BinDef, kBins> bins;
    std::array<int, kD + 1> bin_of{};

    struct Anchor {
        std::uint64_t lo, hi;                      // pages of the request
        int ctx;
        int chain;
        std::bitset<kD> fwd, bwd;                  // label so far: page at distance d (1-based) was requested
        std::array<std::uint8_t, 2 * kD> state;    // candidate state of every page at anchor time (fwd, then bwd)
    };
    using Footprint = std::vector<std::pair<std::uint64_t, std::uint64_t>>;   // page ranges, absolute

    struct Ctx {
        std::deque<Anchor> ring;
        std::vector<double> hits, trials;          // [ctx][side][bin][state]
        std::unordered_map<std::uint64_t, Footprint> foot;   // start page -> what followed it last time
        Ctx() : hits(kCtx * 2 * kBins * kState, 0.0), trials(kCtx * 2 * kBins * kState, 0.0) {}
    };
    std::unordered_map<std::uint64_t, Ctx> ctx;
    std::vector<char> cov;                         // scratch: covered pages around the current request
    std::vector<int> prv, nxt;                     // scratch: nearest covered page below / above each page
    Params p;

    Impl() {
        const int edges[kBins] = {1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 17, 21, 25, 33, 41, 49, 65, 97, 129, 193};
        for (int b = 0; b < kBins; ++b) {
            bins[b].lo = edges[b];
            bins[b].hi = (b + 1 < kBins ? edges[b + 1] - 1 : kD);
            for (int d = bins[b].lo; d <= bins[b].hi; ++d) bin_of[d] = b;
        }
    }
    static int LenBucket(std::uint64_t n) { return n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : 5; }
    static int ChainBucket(int c) { return c <= 1 ? 0 : c == 2 ? 1 : c == 3 ? 2 : c <= 5 ? 3 : c <= 9 ? 4 : 5; }
    static std::size_t Idx(int ctx, int side, int bin, int state) {
        return (((std::size_t)ctx * 2 + side) * kBins + bin) * kState + state;
    }
};

NeighborhoodPrefetchPolicy::NeighborhoodPrefetchPolicy(Cache& cache, const Params& params)
    : CachePolicy(cache), impl_(new Impl) {
    impl_->p = params;
    impl_->p.dmax = std::min<std::uint64_t>(impl_->p.dmax, Impl::kD);
}
NeighborhoodPrefetchPolicy::~NeighborhoodPrefetchPolicy() = default;

void NeighborhoodPrefetchPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t b0, std::uint64_t bn,
                                                     PrefetchRequest& req) {
    using I = Impl;
    I& im = *impl_;
    const auto& P = im.p;
    req.fetch_count = 0;
    if (bn == 0) return;                           // zero-length read: nothing to learn from or prefetch for
    I::Ctx& C = im.ctx[context];
    const std::uint64_t b1 = b0 + bn - 1;
    const int DM = (int)P.dmax;

    // ---- 1. label: credit every open anchor with the pages this request covers; find the chain it extends
    int chain = 1;
    bool near = false;
    for (auto it = C.ring.rbegin(); it != C.ring.rend(); ++it) {
        I::Anchor& a = *it;
        const std::uint64_t o_lo = std::max(b0, a.hi + 1), o_hi = std::min<std::uint64_t>(b1, a.hi + DM);
        for (std::uint64_t pg = o_lo; pg <= o_hi && o_lo <= o_hi; ++pg) a.fwd[pg - a.hi - 1] = true;
        if (a.lo > 0) {
            const std::uint64_t q_lo = std::max<std::uint64_t>(b0, a.lo > (std::uint64_t)DM ? a.lo - DM : 0), q_hi = std::min(b1, a.lo - 1);
            for (std::uint64_t pg = q_lo; pg <= q_hi && q_lo <= q_hi; ++pg) a.bwd[a.lo - pg - 1] = true;
        }
        if (b1 + DM >= a.lo && b0 <= a.hi + DM) {
            near = true;
            // direct continuation of a's run: starts right after a's end (within `gap`)
            if (b0 + P.gap >= a.hi + 1 && b0 <= a.hi + 1 + P.gap && a.chain + 1 > chain) chain = a.chain + 1;
        }
    }

    // ---- 2. retire the oldest anchor: its neighborhood is now fully labelled, so train on every candidate
    if (C.ring.size() >= P.window) {
        I::Anchor& old = C.ring.front();
        for (int d = 1; d <= DM; ++d) {
            const int b = im.bin_of[d];
            C.trials[I::Idx(old.ctx, 0, b, old.state[d - 1])] += 1;
            C.trials[I::Idx(old.ctx, 1, b, old.state[I::kD + d - 1])] += 1;
            if (old.fwd[d - 1]) C.hits[I::Idx(old.ctx, 0, b, old.state[d - 1])] += 1;
            if (old.bwd[d - 1]) C.hits[I::Idx(old.ctx, 1, b, old.state[I::kD + d - 1])] += 1;
        }
        if (P.use_mem) {                           // remember what followed this start page
            I::Footprint f;
            auto add = [&](std::uint64_t pg) {
                if (!f.empty() && f.back().second + 1 == pg) f.back().second = pg;
                else if (f.size() < 8) f.emplace_back(pg, pg);
            };
            for (int d = DM; d >= 1; --d) if (old.bwd[d - 1] && old.lo >= (std::uint64_t)d) add(old.lo - d);
            for (int d = 1; d <= DM; ++d) if (old.fwd[d - 1]) add(old.hi + d);
            if (C.foot.size() > P.mem_entries) C.foot.clear();
            C.foot[old.lo] = std::move(f);
        }
        C.ring.pop_front();
    }

    // ---- 3. this request becomes an anchor; compute the state of each of its candidate pages
    const int ctx = (I::LenBucket(bn) * I::kChainB + I::ChainBucket(chain)) * I::kNear + (near ? 1 : 0);
    I::Anchor a;
    a.lo = b0; a.hi = b1; a.ctx = ctx; a.chain = chain;
    a.state.fill(0);
    if (P.use_hole) {                              // hole class
        const std::uint64_t base = b0 > (std::uint64_t)DM ? b0 - DM : 0;
        const std::size_t span = (std::size_t)(b1 + DM - base + 1);
        im.cov.assign(span, 0);
        auto mark = [&](std::uint64_t lo, std::uint64_t hi) {
            if (hi < base || lo > b1 + DM) return;
            for (std::uint64_t q = std::max(lo, base); q <= std::min<std::uint64_t>(hi, b1 + DM); ++q) im.cov[q - base] = 1;
        };
        mark(b0, b1);
        for (auto& r : C.ring) mark(r.lo, r.hi);
        im.prv.assign(span, -1);
        im.nxt.assign(span, -1);
        int last = -1;
        for (std::size_t i = 0; i < span; ++i) { if (im.cov[i]) last = (int)i; im.prv[i] = last; }
        last = -1;
        for (std::size_t i = span; i-- > 0;) { if (im.cov[i]) last = (int)i; im.nxt[i] = last; }
        auto hole_class = [&](std::uint64_t pg) -> std::uint8_t {
            const std::size_t i = (std::size_t)(pg - base);
            if (im.cov[i] || im.prv[i] < 0 || im.nxt[i] < 0) return 0;      // covered, or not enclosed
            const int gap = im.nxt[i] - im.prv[i] - 1;
            return gap <= (int)P.hole_small ? 1 : gap <= (int)P.hole_max ? 2 : 0;
        };
        for (int d = 1; d <= DM; ++d) {
            a.state[d - 1] = hole_class(b1 + d);
            a.state[I::kD + d - 1] = (b0 >= (std::uint64_t)d) ? hole_class(b0 - d) : 0;
        }
    }
    if (P.use_mem) {                               // footprint class: inside / outside what followed this start page last time
        auto fit = C.foot.find(b0);
        if (fit != C.foot.end()) {
            auto inside = [&](std::uint64_t pg) {
                for (auto& r : fit->second) if (pg >= r.first && pg <= r.second) return true;
                return false;
            };
            for (int d = 1; d <= DM; ++d) {
                a.state[d - 1] += 3 * (inside(b1 + d) ? 1 : 2);
                if (b0 >= (std::uint64_t)d) a.state[I::kD + d - 1] += 3 * (inside(b0 - d) ? 1 : 2);
            }
        }
    }

    // ---- 4. prefetch every page whose learned probability clears theta (backward pages first: ascending addresses)
    const double theta = P.theta_pct / 100.0;
    const double k0 = (double)P.prior_trials;
    auto prob = [&](int side, int d, int state) {
        const int b = im.bin_of[d];
        const double prior = (side == 0 && d <= (int)P.prior_fwd) ? (double)P.prior_pct / 100.0 : 0.0;   // cold start: plain readahead
        return (C.hits[I::Idx(ctx, side, b, state)] + prior * k0) / (C.trials[I::Idx(ctx, side, b, state)] + k0);
    };
    std::uint64_t run_first = 0, run_last = 0;
    bool in_run = false;
    auto flush = [&]() {
        if (!in_run) return;
        in_run = false;
        if (req.fetch_count < MAX_PREFETCH_PAGES) req.fetch_ranges[req.fetch_count++] = FetchRange{run_first, run_last - run_first + 1};
    };
    auto visit = [&](std::uint64_t pg, bool take) {
        if (take && in_run && run_last + 1 == pg) run_last = pg;
        else { flush(); if (take) { in_run = true; run_first = run_last = pg; } }
    };
    for (int d = DM; d >= 1; --d)
        if (b0 >= (std::uint64_t)d) visit(b0 - d, prob(1, d, a.state[I::kD + d - 1]) >= theta);
    flush();
    for (int d = 1; d <= DM; ++d) visit(b1 + d, prob(0, d, a.state[d - 1]) >= theta);
    flush();
    C.ring.push_back(a);
}

}  // namespace policy
