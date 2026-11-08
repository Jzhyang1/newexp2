#pragma once
// Name -> policy construction shared by nbd_server (disk/nbd.cpp) and the offline evaluator (tools/pf/psim.cpp).
//
// A spec is "name" or "name:key=value,key=value" (numeric values); "a+b" runs both and prefetches the union.
//
//   learned[:theta=T]   nextreq + neighborhood, one knob.  T (percent, default 40) is the minimum probability a
//                       page must have to be prefetched: lower = more hits and more traffic.  This is the policy
//                       tuned for VM workloads (FAISS on-disk IVF, Thesios storage trace); see DECISIONS.md.
//   ivfgeo:path=F       FAISS-IVF only: neighbours (by centroid distance) of the lists the host sees; F from tools/pf/geo_export.py
//   nextreq             per-stream next-request predictor (policy_nextreq.cpp)
//   neighborhood        learned neighborhood prefetcher (policy_neighborhood.cpp)
//   readahead[:pages=N], cminer, quickmine, mithril, readahead_cxt_aware, oracle   (the older policies)
//
// Evict specs: lru, fifo, lifo, mru, lru_cxt_aware, lru_seg[:unused=PCT].
#include "policy_api.h"

#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace policy {

struct Spec {
    std::string name;
    std::map<std::string, std::uint64_t> kv;
    std::uint64_t get(const std::string& k, std::uint64_t def) const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
};

inline Spec ParseSpec(const std::string& s) {
    Spec sp;
    auto c = s.find(':');
    sp.name = s.substr(0, c);
    while (c != std::string::npos) {
        auto e = s.find(',', c + 1);
        std::string item = s.substr(c + 1, e == std::string::npos ? std::string::npos : e - c - 1);
        auto eq = item.find('=');
        if (eq != std::string::npos) {
            try { sp.kv[item.substr(0, eq)] = std::stoull(item.substr(eq + 1)); } catch (const std::exception&) {}  // non-numeric: caller reads the raw string
        }
        c = e;
    }
    return sp;
}

inline CachePolicy* MakeEvictPolicy(const std::string& spec, Cache& cache) {
    Spec sp = ParseSpec(spec);
    const std::string& name = sp.name;
    if (name == "lru_seg") return new SegmentedLRUPolicy(cache, sp.get("unused", 75));
    if (name == "fifo") return new FIFOPolicy(cache);
    if (name == "lifo") return new LIFOPolicy(cache);
    if (name == "lru") return new LRUPolicy(cache);
    if (name == "lru_cxt_aware") return new ContextAwareLRUPolicy(cache);
    if (name == "mru") return new MRUPolicy(cache);
    if (name == "none") return nullptr;
    throw std::runtime_error("unsupported evict policy " + name);
}

// Hook for analysis-only policies defined outside policies/ (tools/pf/psim.cpp registers `file`, `localoracle`);
// lets them take part in "a+b" combos. Returns nullptr if it does not know the name.
inline std::function<CachePolicy*(const std::string&, Cache&)>& ExtraPolicyHook() {
    static std::function<CachePolicy*(const std::string&, Cache&)> h;
    return h;
}

inline CachePolicy* MakePolicy(const std::string& spec, Cache& cache, const std::string& oracle_file = "");

// "a+b" runs both and prefetches the union.
inline CachePolicy* MakeCombo(const std::string& spec, Cache& cache, const std::string& oracle_file) {
    std::vector<std::unique_ptr<CachePolicy>> parts;
    std::size_t pos = 0;
    while (pos <= spec.size()) {
        auto e = spec.find('+', pos);
        std::string one = spec.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (CachePolicy* p = MakePolicy(one, cache, oracle_file)) parts.emplace_back(p);
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return new ComboPolicy(cache, std::move(parts));
}

inline NeighborhoodPrefetchPolicy::Params NeighborhoodParams(const Spec& sp, std::uint64_t theta_default) {
    NeighborhoodPrefetchPolicy::Params q;
    q.theta_pct = sp.get("theta", theta_default);
    q.window = sp.get("window", q.window);
    q.gap = sp.get("gap", q.gap);
    q.prior_trials = sp.get("k0", q.prior_trials);
    q.prior_pct = sp.get("prior", q.prior_pct);
    q.prior_fwd = sp.get("pfwd", q.prior_fwd);
    q.use_hole = sp.get("hole", q.use_hole);
    q.hole_small = sp.get("hsmall", q.hole_small);
    q.hole_max = sp.get("hmax", q.hole_max);
    q.use_mem = sp.get("mem", q.use_mem);
    q.mem_entries = sp.get("mementries", q.mem_entries);
    q.dmax = std::min<std::uint64_t>(256, sp.get("dmax", q.dmax));
    return q;
}

inline NextRequestPolicy::Params NextRequestParams(const Spec& sp, std::uint64_t theta_default) {
    NextRequestPolicy::Params q;
    q.theta_pct = sp.get("theta", theta_default);
    q.streams = sp.get("streams", q.streams);
    q.gap_kb = sp.get("gapkb", q.gap_kb);
    q.overlap_kb = sp.get("ovkb", q.overlap_kb);
    q.max_pages = sp.get("max", q.max_pages);
    q.look = (std::uint32_t)sp.get("look", q.look);
    q.min_evidence = (float)sp.get("minev", (std::uint64_t)q.min_evidence);
    q.use_consumed = sp.get("cons", q.use_consumed);
    q.prior_cont = sp.get("prior", (std::uint64_t)(q.prior_cont * 100)) / 100.f;
    q.decay = 1.f - sp.get("decay", 5) / 1000.f;
    return q;
}

inline CachePolicy* MakePolicy(const std::string& spec, Cache& cache, const std::string& oracle_file) {
    if (spec.find('+') != std::string::npos) return MakeCombo(spec, cache, oracle_file);
    Spec sp = ParseSpec(spec);
    const std::string& n = sp.name;
    if (n == "none") return nullptr;
    if (ExtraPolicyHook()) if (CachePolicy* x = ExtraPolicyHook()(spec, cache)) return x;
    if (n == "learned") {
        // one knob: the neighborhood part prefetches pages with P >= theta; the stream part is probability-of-a-
        // whole-request based and runs at a quarter of that (its probabilities are products of several factors)
        const std::uint64_t theta = sp.get("theta", 40);
        std::vector<std::unique_ptr<CachePolicy>> parts;
        parts.emplace_back(new NextRequestPolicy(cache, NextRequestParams(Spec{}, std::max<std::uint64_t>(1, theta / 4))));
        parts.emplace_back(new NeighborhoodPrefetchPolicy(cache, NeighborhoodParams(Spec{}, theta)));
        return new ComboPolicy(cache, std::move(parts));
    }
    if (n == "hint") {
        HintPolicy::Params q;
        q.theta_pct = sp.get("theta", q.theta_pct); q.window = sp.get("window", q.window); q.filter = sp.get("filter", q.filter);
        q.bucket_pages = sp.get("bucket", q.bucket_pages); q.prior_trials = sp.get("k0", q.prior_trials); q.prior_pct = sp.get("prior", q.prior_pct);
        return new HintPolicy(cache, q);
    }
    if (n == "ivfgeo") {
        IvfGeoPolicy::Params q;
        q.k = sp.get("k", q.k); q.theta_pct = sp.get("theta", q.theta_pct); q.window = sp.get("window", q.window);
        q.cooldown = sp.get("cooldown", q.cooldown); q.filter = sp.get("filter", q.filter); q.res_thr_pct = sp.get("res", q.res_thr_pct); q.gap_us = sp.get("gap", q.gap_us);
        auto at = spec.find("path=");
        if (at == std::string::npos) throw std::runtime_error("ivfgeo needs path=<geo file> (tools/pf/geo_export.py)");
        auto end = spec.find(',', at);
        return new IvfGeoPolicy(cache, q, spec.substr(at + 5, end == std::string::npos ? end : end - at - 5));
    }
    if (n == "nextreq") return new NextRequestPolicy(cache, NextRequestParams(sp, 10));
    if (n == "neighborhood") return new NeighborhoodPrefetchPolicy(cache, NeighborhoodParams(sp, 40));
    if (n == "readahead") return new ReadaheadPolicy(cache, sp.get("pages", MAX_PREFETCH_PAGES));
    if (n == "readahead_cxt_aware")
        return new ContextAwareReadaheadPolicy(cache, sp.get("window", 8), sp.get("streams", 8), sp.get("pages", MAX_PREFETCH_PAGES));
    if (n == "cminer") return new CMinerPolicy(cache);
    if (n == "quickmine") return new QuickMinePolicy(cache);
    if (n == "mithril") return new MithrilPolicy(cache);
    if (n == "oracle") return new OraclePolicy(cache, oracle_file);
    throw std::runtime_error("unsupported prefetch policy " + n);
}

}  // namespace policy
