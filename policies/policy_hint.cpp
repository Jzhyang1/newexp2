#include "policy_api.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace policy {

struct HintPolicy::Impl {
    struct Open { std::uint64_t first, n; std::uint64_t born; std::size_t slot; bool hit; };
    struct Ctx {
        std::deque<Open> open;                               // hints awaiting their label
        std::unordered_map<std::uint64_t, std::uint32_t> pop;   // hinted-range key -> times hinted
        std::unordered_map<std::uint64_t, std::uint64_t> seen;  // page / 16 -> request counter when the host last requested it
        std::array<float, 8 * 6 * 6 * 3> hits{}, trials{};
        std::uint64_t now = 0;
    };
    std::unordered_map<std::uint64_t, Ctx> ctx;
    Params p;
    static int PopB(std::uint32_t c) { return c == 0 ? 0 : c == 1 ? 1 : c == 2 ? 2 : c <= 4 ? 3 : c <= 8 ? 4 : c <= 16 ? 5 : c <= 32 ? 6 : 7; }
    static int AuxB(std::uint32_t a) { return a == 255 ? 5 : std::min<int>(4, (int)(a / 4)); }          // unknown / quarters resident
    static int SizeB(std::uint64_t n) { return n <= 1 ? 0 : n <= 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : 5; }
};

HintPolicy::HintPolicy(Cache& cache, const Params& params) : CachePolicy(cache), impl_(new Impl) { impl_->p = params; }
HintPolicy::~HintPolicy() = default;

void HintPolicy::on_hint(std::uint64_t context, std::uint64_t first, std::uint64_t n, std::uint32_t aux, PrefetchRequest& req) {
    auto& P = impl_->p;
    auto& C = impl_->ctx[context];
    req.fetch_count = 0;
    if (n == 0) return;
    const std::uint64_t key = first / std::max<std::uint64_t>(1, P.bucket_pages);
    std::uint32_t& pop = C.pop[key];
    // host recency: was any page of the range requested by the host in the last few thousand requests?
    int recent = 0;
    for (std::uint64_t q = first / 16; q <= (first + n - 1) / 16; ++q) {
        auto it = C.seen.find(q);
        if (it != C.seen.end()) { recent = (C.now - it->second < 2000) ? 2 : 1; if (recent == 2) break; }
    }
    const std::size_t slot = ((Impl::PopB(pop) * 6 + Impl::AuxB(aux)) * 6 + Impl::SizeB(n)) * 3 + recent;
    ++pop;
    const double k0 = (double)P.prior_trials;
    const double pr = (C.hits[slot] + k0 * P.prior_pct / 100.0) / (C.trials[slot] + k0);
    C.trials[slot] += 1.f;
    C.open.push_back({first, n, C.now, slot, false});
    if (P.filter && pr < P.theta_pct / 100.0) return;
    req.fetch_count = 1;
    req.fetch_ranges[0] = FetchRange{first, n};
}

void HintPolicy::on_prefetch_request(std::uint64_t context, std::uint64_t b0, std::uint64_t bn, PrefetchRequest& req) {
    auto& C = impl_->ctx[context];
    auto& P = impl_->p;
    req.fetch_count = 0;
    ++C.now;
    if (bn == 0) return;
    const std::uint64_t b1 = b0 + bn - 1;
    for (auto& o : C.open)
        if (!o.hit && b0 <= o.first + o.n - 1 && b1 >= o.first) { o.hit = true; C.hits[o.slot] += 1.f; }
    while (!C.open.empty() && C.now - C.open.front().born > P.window) C.open.pop_front();
    for (std::uint64_t q = b0 / 16; q <= b1 / 16; ++q) C.seen[q] = C.now;
    if (C.seen.size() > (1u << 22)) C.seen.clear();
}

}  // namespace policy
