#include "policy_api.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace policy {

// Temporal-streaming prefetcher (see TemporalStreamPolicy in policy_api.h).
struct TemporalStreamPolicy::Impl {
    struct Follower {
        std::uint64_t ptr = 0;        // log index of the last entry this follower matched
        std::uint64_t issued = 0;     // log entries up to here have been prefetched already
        std::uint64_t last_use = 0;
        int conf = 0;
        int matches = 0;
        bool active = false;
    };
    struct Ctx {
        std::vector<std::uint64_t> log;                      // demand page sequence (immediate repeats dropped)
        std::unordered_map<std::uint64_t, std::uint64_t> last_pos;   // page -> its latest log index
        std::unordered_map<std::uint32_t, std::uint64_t> start_of;   // event id (app) -> log index its last occurrence began at
        std::vector<Follower> f;
        std::uint64_t now = 0;
    };
    std::unordered_map<std::uint64_t, Ctx> ctx;
    Params p;
    std::vector<std::uint64_t> pend;                         // scratch: pages to prefetch for the current call

    Follower& Alloc(Ctx& C) {
        if (C.f.size() < p.followers) { C.f.emplace_back(); return C.f.back(); }
        Follower* v = &C.f[0];
        for (auto& x : C.f) if (!x.active || x.last_use < v->last_use) { v = &x; if (!x.active) break; }
        return *v;
    }
    void Issue(Ctx& C, Follower& x, std::uint64_t log_end) {
        const std::uint64_t depth = std::min<std::uint64_t>(p.dmax, p.d0 << std::min(x.conf, 16));
        const std::uint64_t to = std::min<std::uint64_t>(x.ptr + depth, log_end);
        for (std::uint64_t j = std::max(x.issued, x.ptr) + 1; j <= to && j < log_end; ++j) pend.push_back(C.log[j]);
        x.issued = std::max(x.issued, to);
    }
    void Emit(PrefetchRequest& req, std::uint64_t skip_lo, std::uint64_t skip_hi) {
        std::sort(pend.begin(), pend.end());
        pend.erase(std::unique(pend.begin(), pend.end()), pend.end());
        req.fetch_count = 0;
        for (std::uint64_t pg : pend) {
            if (pg >= skip_lo && pg < skip_hi) continue;
            if (req.fetch_count && req.fetch_ranges[req.fetch_count - 1].block_offset + req.fetch_ranges[req.fetch_count - 1].block_length == pg)
                req.fetch_ranges[req.fetch_count - 1].block_length++;
            else if (req.fetch_count < MAX_PREFETCH_PAGES) req.fetch_ranges[req.fetch_count++] = FetchRange{pg, 1};
        }
        pend.clear();
    }
};

TemporalStreamPolicy::TemporalStreamPolicy(Cache& cache, const Params& params) : CachePolicy(cache), impl_(new Impl) { impl_->p = params; }
TemporalStreamPolicy::~TemporalStreamPolicy() = default;

void TemporalStreamPolicy::on_hint(std::uint64_t context, std::uint64_t, std::uint64_t npages, std::uint32_t aux, PrefetchRequest& req) {
    req.fetch_count = 0;
    if (npages != 0 || (aux >> 16) != 1) return;               // only "event" hints: aux = 1<<16 | event id
    Impl& im = *impl_;
    auto& C = im.ctx[context];
    const std::uint32_t id = aux & 0xffff;
    if (id != 0xffff) {
        auto it = C.start_of.find(id);
        if (it != C.start_of.end() && it->second < C.log.size()) {   // replay this event's previous occurrence from its start
            auto& x = im.Alloc(C);
            x = Impl::Follower{};
            x.active = true; x.conf = (int)im.p.hint_conf; x.matches = (int)im.p.confirm; x.last_use = C.now;
            x.ptr = x.issued = it->second ? it->second - 1 : 0;
            if (it->second == 0) im.pend.push_back(C.log[0]);
            im.Issue(C, x, C.log.size());
        }
        C.start_of[id] = C.log.size();
    }
    im.Emit(req, 1, 0);
}

void TemporalStreamPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t b0, std::uint64_t bn, PrefetchRequest& req) {
    req.fetch_count = 0;
    if (bn == 0) return;
    Impl& im = *impl_;
    const auto& P = im.p;
    auto& C = im.ctx[context];
    ++C.now;
    const std::uint64_t log_end = C.log.size();
    // 1. advance followers whose next few log entries contain one of this request's pages
    bool matched = false;
    for (auto& x : C.f) {
        if (!x.active) continue;
        if (C.now - x.last_use > P.idle) { x.active = false; continue; }
        bool hit = false;
        for (std::uint64_t pg = b0; pg < b0 + bn && !hit; ++pg)
            for (std::uint64_t j = x.ptr + 1; j <= x.ptr + P.win && j < log_end; ++j)
                if (C.log[j] == pg) { x.ptr = std::max(x.ptr, j); hit = true; break; }
        if (hit) {
            x.conf = std::min(x.conf + 1, 16); ++x.matches; x.last_use = C.now; matched = true;
            if (x.matches >= (int)P.confirm) im.Issue(C, x, log_end);
        }
    }
    // 2. no follower predicted this request: start one at the previous occurrence of its first known page
    if (!matched && !(P.on_miss && cache.present(b0))) {
        for (std::uint64_t pg = b0; pg < b0 + bn; ++pg) {
            auto it = C.last_pos.find(pg);
            if (it == C.last_pos.end()) continue;
            auto& x = im.Alloc(C);
            x = Impl::Follower{};
            x.active = true; x.ptr = x.issued = it->second; x.last_use = C.now;
            if (P.confirm == 0) im.Issue(C, x, log_end);
            break;
        }
    }
    // 3. append this request to the log
    if (C.log.size() > P.log_max) { C.log.clear(); C.last_pos.clear(); C.start_of.clear(); C.f.clear(); }
    for (std::uint64_t pg = b0; pg < b0 + bn; ++pg) {
        if (!C.log.empty() && C.log.back() == pg) continue;
        C.last_pos[pg] = C.log.size();
        C.log.push_back(pg);
    }
    im.Emit(req, b0, b0 + bn);
}

}  // namespace policy
