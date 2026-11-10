#pragma once
// Block-level ports of published prefetchers, for comparison against `learned` (see policy_lit.cpp).
//   bop      Best-Offset Prefetcher        (Michaud, HPCA 2016)
//   spp      Signature Path Prefetcher     (Kim et al., MICRO 2016)
//   ppf      SPP + Perceptron Prefetch Filter (Bhatia et al., ISCA 2019)
//   bingo    Bingo spatial prefetcher      (Bakhshalipour et al., HPCA 2019)
//   pythia   Pythia RL prefetcher          (Bera et al., MICRO 2021)
//   leap     Leap majority-trend prefetcher (Al Maruf & Chowdhury, USENIX ATC 2020)
#include <cstdint>
#include <map>
#include <string>

namespace policy {
class Cache;
class CachePolicy;
// nullptr if `name` is not one of the policies above.  `kv` = the spec's numeric k=v pairs.
CachePolicy* MakeLitPolicy(const std::string& name, const std::map<std::string, std::uint64_t>& kv, Cache& cache);
}  // namespace policy
