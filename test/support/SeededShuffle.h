#pragma once

// test-audit: a tiny, shared, deterministic Fisher-Yates shuffle
// for ORDER-INDEPENDENCE / set-purity test variants.
//
// WHY: many hardening tests assert order-independence by reversing or rotating the
// input list (ad-hoc std::reverse/std::rotate).  requires the reorder be a
// DETERMINISTIC, integer-SEEDED permutation so the variant is itself reproducible
// (a flaky shuffle would give a flaky test). This helper reuses the SIM RNG
// (luminumbra::core::DeterministicRng, splitmix64) so the permutation is a pure
// function of an integer seed: same seed -> same permutation, every run.
//
// Test-support code ONLY (mirrors EntitySnapshotFixture.h): never include from src/.
// Both hardening test executables (common_tests, frontier_gates_test) carry../src
// on their include path, so the root-relative include below resolves in both.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "luminumbra_common/core/DeterministicRng.h"

namespace Luminumbra::TestSupport {

// In-place Fisher-Yates shuffle of `v` driven by a seeded DeterministicRng. The
// classic backward sweep: for i = n-1.. 1, swap v[i] with v[rng % (i+1)]. Pure +
// reproducible: SeededShuffle(v, s) twice on equal inputs yields equal results.
// Seed 0 maps to the RNG's non-zero fallback (DeterministicRng's ctor), so a 0 seed
// is still a valid, fixed permutation.
template<typename T>
inline void SeededShuffle(std::vector<T>& v, std::uint32_t seed) {
    luminumbra::core::DeterministicRng rng(static_cast<std::uint64_t>(seed));
    const std::size_t n = v.size();
    for (std::size_t i = n; i-- > 1;) {
        const std::size_t j = static_cast<std::size_t>(rng.next_u64() % (i + 1));
        if (j != i) {
            T tmp = v[i];
            v[i] = v[j];
            v[j] = tmp;
        }
    }
}

// Convenience: return a shuffled COPY (leaves the source untouched). Used by
// variants that need both the baseline order and the shuffled order.
template<typename T>
[[nodiscard]] inline std::vector<T> SeededShuffled(std::vector<T> v, std::uint32_t seed) {
    SeededShuffle(v, seed);
    return v;
}

} // namespace Luminumbra::TestSupport
