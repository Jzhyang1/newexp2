#include "policy_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace policy {

// IVF-geometry prefetcher (see IvfGeoPolicy in policy_api.h).
struct IvfGeoPolicy::Impl {
    std::uint32_t nlist = 0, d = 0;
    std::vector<float> cent;                       // nlist x d
    std::vector<float> c2;                         // |centroid|^2
    std::vector<std::uint64_t> start, nbytes;      // byte extent of each list on the data disk
    std::vector<std::uint32_t> by_start;           // list ids sorted by start offset
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> nn;   // list -> neighbours, nearest first (lazy)
    struct Open { std::uint32_t list; std::uint64_t born; std::size_t slot; bool hit; };
    struct Ctx {
        std::deque<Open> open;
        std::unordered_map<std::uint32_t, std::uint32_t> touches;      // list -> host requests that touched it
        std::unordered_map<std::uint32_t, std::uint64_t> last_touch, last_prop;
        std::array<float, 8 * 6 * 6 * 3> hits{}, trials{};
        std::uint64_t now = 0, last_t = 0;
        bool new_query = true;
    };
    std::unordered_map<std::uint64_t, Ctx> ctx;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_page;   // first cache page of a list -> list ids (lazy)
    std::vector<std::uint8_t> resident;           // guest-reported residency 0..14 per list, 255 = never reported
    Params p;

    const std::vector<std::uint32_t>& Neighbours(std::uint32_t a) {
        auto it = nn.find(a);
        if (it != nn.end()) return it->second;
        std::vector<std::pair<float, std::uint32_t>> v;
        v.reserve(nlist);
        for (std::uint32_t b = 0; b < nlist; ++b) {
            if (b == a) continue;
            float dot = 0;
            for (std::uint32_t k = 0; k < d; ++k) dot += cent[(std::size_t)a * d + k] * cent[(std::size_t)b * d + k];
            v.emplace_back(c2[b] - 2 * dot, b);
        }
        const std::size_t keep = std::min<std::size_t>(v.size(), p.max_k);
        std::partial_sort(v.begin(), v.begin() + keep, v.end());
        std::vector<std::uint32_t> out(keep);
        for (std::size_t i = 0; i < keep; ++i) out[i] = v[i].second;
        return nn.emplace(a, std::move(out)).first->second;
    }
    // lists whose byte extent intersects [lo, hi]
    template <class F> void Touched(std::uint64_t lo, std::uint64_t hi, F&& f) const {
        auto it = std::upper_bound(by_start.begin(), by_start.end(), hi, [&](std::uint64_t x, std::uint32_t l) { return x < start[l]; });
        while (it != by_start.begin()) {
            --it;
            const std::uint32_t l = *it;
            if (start[l] + nbytes[l] <= lo) break;
            f(l);
        }
    }
    static int RankB(std::size_t r) { return r < 2 ? 0 : r < 4 ? 1 : r < 8 ? 2 : r < 12 ? 3 : r < 20 ? 4 : r < 32 ? 5 : r < 48 ? 6 : 7; }
    static int PopB(std::uint32_t c) { return c == 0 ? 0 : c <= 1 ? 1 : c <= 3 ? 2 : c <= 8 ? 3 : c <= 20 ? 4 : 5; }
    static int RecB(std::uint64_t ago) { return ago < 64 ? 0 : ago < 512 ? 1 : 2; }
};

IvfGeoPolicy::IvfGeoPolicy(Cache& cache, const Params& params, const std::string& geo_file) : CachePolicy(cache), impl_(new Impl) {
    auto& I = *impl_;
    I.p = params;
    FILE* f = std::fopen(geo_file.c_str(), "rb");
    if (!f) throw std::runtime_error("ivfgeo: cannot open " + geo_file);
    std::uint32_t hdr[2];
    if (std::fread(hdr, 4, 2, f) != 2) throw std::runtime_error("ivfgeo: short file " + geo_file);
    I.nlist = hdr[0]; I.d = hdr[1];
    I.cent.resize((std::size_t)I.nlist * I.d);
    I.start.resize(I.nlist); I.nbytes.resize(I.nlist);
    bool ok = std::fread(I.cent.data(), 4, I.cent.size(), f) == I.cent.size() && std::fread(I.start.data(), 8, I.nlist, f) == I.nlist &&
              std::fread(I.nbytes.data(), 8, I.nlist, f) == I.nlist;
    std::fclose(f);
    if (!ok) throw std::runtime_error("ivfgeo: truncated " + geo_file);
    I.c2.resize(I.nlist);
    for (std::uint32_t l = 0; l < I.nlist; ++l) { float s = 0; for (std::uint32_t k = 0; k < I.d; ++k) s += I.cent[(std::size_t)l * I.d + k] * I.cent[(std::size_t)l * I.d + k]; I.c2[l] = s; }
    I.by_start.resize(I.nlist);
    for (std::uint32_t l = 0; l < I.nlist; ++l) I.by_start[l] = l;
    std::sort(I.by_start.begin(), I.by_start.end(), [&](std::uint32_t a, std::uint32_t b) { return I.start[a] < I.start[b]; });
}
IvfGeoPolicy::~IvfGeoPolicy() = default;

// A guest agent's residency report (mincore on the index file): hint [first_page, first_page + npages) = one list's extent,
// aux low nibble = resident fraction in fourteenths.  No application is involved.
void IvfGeoPolicy::on_hint(std::uint64_t, std::uint64_t first, std::uint64_t n, std::uint32_t aux, PrefetchRequest& req) {
    auto& I = *impl_;
    req.fetch_count = 0;
    if (I.resident.empty()) I.resident.assign(I.nlist, 255);
    if (I.by_page.empty()) {
        const std::uint64_t psz = cache.page_size;
        for (std::uint32_t l = 0; l < I.nlist; ++l) I.by_page[I.start[l] / psz].push_back(l);
    }
    auto it = I.by_page.find(first);
    if (it == I.by_page.end()) return;
    const std::uint64_t psz = cache.page_size;
    for (auto l : it->second)
        if ((I.start[l] + I.nbytes[l] - 1) / psz - first + 1 == n) { I.resident[l] = (std::uint8_t)std::min<std::uint32_t>(14, aux & 15); break; }
}

void IvfGeoPolicy::on_request(std::uint64_t context, std::uint64_t off, std::uint64_t len, std::uint64_t t) {
    cur_off_ = off; cur_len_ = len; have_ = true;
    auto& C = impl_->ctx[context];
    // a long silence on the host stream marks the start of a new query (the guest computed on cached lists meanwhile)
    C.new_query = impl_->p.gap_us == 0 || t == 0 || C.last_t == 0 || t - C.last_t > impl_->p.gap_us * 1000;
    C.last_t = t;
}

void IvfGeoPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t b0, std::uint64_t bn, PrefetchRequest& req) {
    auto& I = *impl_;
    auto& P = I.p;
    auto& C = I.ctx[context];
    req.fetch_count = 0;
    if (bn == 0) return;
    ++C.now;
    const std::uint64_t psz = cache.page_size;
    const std::uint64_t lo = have_ ? cur_off_ : b0 * psz, hi = have_ ? cur_off_ + cur_len_ - 1 : (b0 + bn) * psz - 1;
    have_ = false;

    // label: which open proposals did this request touch; retire old ones
    std::vector<std::uint32_t> touched;
    I.Touched(lo, hi, [&](std::uint32_t l) { touched.push_back(l); });
    for (auto& o : C.open)
        if (!o.hit) for (auto l : touched) if (l == o.list) { o.hit = true; C.hits[o.slot] += 1.f; break; }
    while (!C.open.empty() && C.now - C.open.front().born > P.window) C.open.pop_front();
    for (auto l : touched) { ++C.touches[l]; C.last_touch[l] = C.now; }

    // propose: the nearest lists to every list this request touched
    const double theta = P.theta_pct / 100.0, k0 = (double)P.prior_trials;
    if (!C.new_query) return;                      // only the first list a query shows the host anchors proposals
    if (touched.size() > 1) touched.resize(1);
    for (auto a : touched) {
        const auto& nb = I.Neighbours(a);
        const std::size_t K = std::min<std::size_t>(nb.size(), P.k);
        for (std::size_t r = 0; r < K; ++r) {
            const std::uint32_t l = nb[r];
            auto lp = C.last_prop.find(l);
            if (lp != C.last_prop.end() && C.now - lp->second < P.cooldown) continue;
            const std::uint64_t p0 = I.start[l] / psz, p1 = (I.start[l] + I.nbytes[l] - 1) / psz;
            bool all = true;
            for (std::uint64_t pg = p0; pg <= p1 && all; ++pg) all = cache.present(pg);
            if (all) continue;                                                // already cached: nothing to decide
            if (!I.resident.empty() && I.resident[l] != 255 && I.resident[l] * 100 >= P.res_thr_pct * 14) continue;   // the guest holds it: it will never be asked for
            auto lt = C.last_touch.find(l);
            const int rec = lt == C.last_touch.end() ? 2 : Impl::RecB(C.now - lt->second);
            auto tc = C.touches.find(l);
            const std::size_t slot = ((Impl::RankB(r) * 6 + Impl::PopB(tc == C.touches.end() ? 0 : tc->second)) * 6 + (std::size_t)std::min<std::uint64_t>(5, (p1 - p0)/4)) * 3 + rec;
            const double pr = (C.hits[slot] + k0 * P.prior_pct / 100.0) / (C.trials[slot] + k0);
            C.trials[slot] += 1.f;
            C.open.push_back({l, C.now, slot, false});
            C.last_prop[l] = C.now;
            if (P.filter && pr < theta) continue;
            if (req.fetch_count < MAX_PREFETCH_PAGES) req.fetch_ranges[req.fetch_count++] = FetchRange{p0, p1 - p0 + 1};
        }
    }
    if (C.touches.size() > (1u << 20)) { C.touches.clear(); C.last_touch.clear(); C.last_prop.clear(); }
}

}  // namespace policy
