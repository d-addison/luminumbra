// deterministic thermal+hydraulic erosion kernel tests.
//
// Pins the three load-bearing properties before this feeds a world_hash bump:
//   1. DETERMINISM: same input + params -> bit-identical offset.
//   2. HALO-INDEPENDENCE (the brief's #1 risk): the cropped interior offset is
//      byte-identical whether baked with halo == iterations+8 or iterations+16,
//      because each sweep propagates information at most one cell.
//   3. The kernel produces non-trivial, bounded relief on steep terrain.

#include "gtest/gtest.h"

#include <cmath>
#include <vector>

#include "world/HydraulicErosion.h"

namespace {

using Luminumbra::World::BakeHydraulicErosion;
using Luminumbra::World::HydroErosionParams;

// Build a padded height field. Interior cell (i,j) in [0,n) maps to world (i,j)
// for ANY halo, so the same interior + surrounding heights are presented at
// every halo -> the interior offset must be halo-independent. A tall pyramid
// (slope 3 m/cell, well above the 1.2 m talus) gives the kernel steep terrain to
// erode, plus a gentle tilt to drive hydraulic flow.
std::vector<float> BuildHeights(int n, int halo) {
    const int m = n + 2 * halo;
    std::vector<float> h(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), 0.0f);
    const float cx = static_cast<float>(n) * 0.5f;
    const float cz = static_cast<float>(n) * 0.5f;
    for (int z = 0; z < m; ++z) {
        for (int x = 0; x < m; ++x) {
            const float wx = static_cast<float>(x - halo);
            const float wz = static_cast<float>(z - halo);
            const float dist = std::fabs(wx - cx) + std::fabs(wz - cz);
            const float peak = std::max(0.0f, 80.0f - 3.0f * dist);
            h[static_cast<std::size_t>(z) * static_cast<std::size_t>(m) +
              static_cast<std::size_t>(x)] = peak + 0.1f * wx;
        }
    }
    return h;
}

TEST(HydraulicErosion, IsDeterministic) {
    HydroErosionParams params;
    const int n = 32;
    const int halo = params.iterations + 8;
    const std::vector<float> heights = BuildHeights(n, halo);
    std::vector<float> a, b;
    BakeHydraulicErosion(heights, n, halo, params, a);
    BakeHydraulicErosion(heights, n, halo, params, b);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(a, b) << "erosion bake is non-deterministic";
}

TEST(HydraulicErosion, InteriorIsHaloIndependent) {
    HydroErosionParams params;
    const int n = 32;
    const int halo1 = params.iterations + 8;
    const int halo2 = params.iterations + 16;
    std::vector<float> o1, o2;
    BakeHydraulicErosion(BuildHeights(n, halo1), n, halo1, params, o1);
    BakeHydraulicErosion(BuildHeights(n, halo2), n, halo2, params, o2);
    ASSERT_EQ(o1.size(), o2.size());
    // Byte-identical interior across the two halos (the acceptance criterion).
    EXPECT_EQ(o1, o2)
        << "interior offset depends on halo width (erosion influence exceeds the halo)";
}

TEST(HydraulicErosion, ProducesBoundedNonTrivialRelief) {
    HydroErosionParams params;
    const int n = 32;
    const int halo = params.iterations + 8;
    std::vector<float> offset;
    BakeHydraulicErosion(BuildHeights(n, halo), n, halo, params, offset);

    bool any_negative = false; // peak should erode DOWN somewhere
    bool any_positive = false; // talus/pits should gain material somewhere
    float max_abs = 0.0f;
    for (float v : offset) {
        if (v < -1.0e-3f)
            any_negative = true;
        if (v > 1.0e-3f)
            any_positive = true;
        const float a = (v < 0.0f) ? -v : v;
        if (a > max_abs)
            max_abs = a;
    }
    EXPECT_TRUE(any_negative) << "erosion never lowered the steep peak";
    EXPECT_TRUE(any_positive) << "erosion never deposited material";
    EXPECT_LE(max_abs, params.max_offset + 1.0e-3f) << "offset exceeded the clamp";
}

} // namespace
