// tree rendering (tree rendering): unit + budget gate for the render-only tree LOD
// distance selection. These tests are OpenGL-free -- they exercise the pure
// selection logic in src/luminumbra_client/rendering/TreeLod.h that the G-Buffer
// pass uses to pick a per-instance LOD mesh by camera distance.
//
// Acceptance criteria covered:
//   * Near instances pick LOD0 (the unchanged full mesh) so the near field looks
//     identical to today.
//   * Selection is monotonic non-decreasing in distance and lands in the right
//     bucket at each threshold.
//   * Disabling LOD is a hard no-op (always LOD0).
//   * Path derivation matches the asset processor's "<stem>.lodN.lmesh" naming and
//     leaves LOD0 paths untouched.
//   * Visible-triangle BUDGET: a representative scattered grove draws strictly
//     fewer triangles with LOD on than the all-LOD0 baseline.

#include "luminumbra_client/rendering/TreeLod.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

using Luminumbra::Rendering::LodMeshPath;
using Luminumbra::Rendering::SelectTreeLod;
using Luminumbra::Rendering::TreeLodConfig;

namespace {

TreeLodConfig MakeConfig() {
    TreeLodConfig c;
    c.enabled = true;
    c.lod1Distance = 140.0f;
    c.lod2Distance = 320.0f;
    return c;
}

} // namespace

TEST(TreeLod, NearInstancesUseLod0) {
    const auto cfg = MakeConfig();
    EXPECT_EQ(SelectTreeLod(0.0f, cfg), 0);
    EXPECT_EQ(SelectTreeLod(10.0f, cfg), 0);
    EXPECT_EQ(SelectTreeLod(cfg.lod1Distance - 0.001f, cfg), 0);
}

TEST(TreeLod, MidDistancePicksLod1) {
    const auto cfg = MakeConfig();
    EXPECT_EQ(SelectTreeLod(cfg.lod1Distance, cfg), 1);
    EXPECT_EQ(SelectTreeLod((cfg.lod1Distance + cfg.lod2Distance) * 0.5f, cfg), 1);
    EXPECT_EQ(SelectTreeLod(cfg.lod2Distance - 0.001f, cfg), 1);
}

TEST(TreeLod, FarDistancePicksLod2) {
    const auto cfg = MakeConfig();
    EXPECT_EQ(SelectTreeLod(cfg.lod2Distance, cfg), 2);
    EXPECT_EQ(SelectTreeLod(cfg.lod3Distance - 0.001f, cfg), 2);
}

TEST(TreeLod, VeryFarPicksLod3Billboard) {
    const auto cfg = MakeConfig();
    EXPECT_EQ(SelectTreeLod(cfg.lod3Distance, cfg), 3);
    EXPECT_EQ(SelectTreeLod(5000.0f, cfg), 3);
}

TEST(TreeLod, SelectionIsMonotonicInDistance) {
    const auto cfg = MakeConfig();
    int prev = 0;
    for (float d = 0.0f; d <= 1000.0f; d += 5.0f) {
        const int lod = SelectTreeLod(d, cfg);
        EXPECT_GE(lod, prev) << "LOD must not decrease as distance grows (d=" << d << ")";
        EXPECT_GE(lod, 0);
        EXPECT_LT(lod, Luminumbra::Rendering::kTreeLodCount);
        prev = lod;
    }
}

TEST(TreeLod, DisabledIsAlwaysLod0) {
    TreeLodConfig cfg = MakeConfig();
    cfg.enabled = false;
    EXPECT_EQ(SelectTreeLod(0.0f, cfg), 0);
    EXPECT_EQ(SelectTreeLod(500.0f, cfg), 0);
    EXPECT_EQ(SelectTreeLod(99999.0f, cfg), 0);
}

TEST(TreeLod, NegativeOrNanDistanceClampsToLod0) {
    const auto cfg = MakeConfig();
    EXPECT_EQ(SelectTreeLod(-50.0f, cfg), 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // All `>=` comparisons against NaN are false, so the function returns LOD0.
    EXPECT_EQ(SelectTreeLod(nan, cfg), 0);
}

TEST(TreeLod, Lod0PathIsUnchanged) {
    const std::string base = "data/models/trees/tree_small_02_leaves.lmesh";
    EXPECT_EQ(LodMeshPath(base, 0), base);
    // Defensive: negative also maps to base.
    EXPECT_EQ(LodMeshPath(base, -1), base);
}

TEST(TreeLod, LodPathInsertsSuffixBeforeExtension) {
    const std::string base = "data/models/trees/tree_small_02_leaves.lmesh";
    EXPECT_EQ(LodMeshPath(base, 1), "data/models/trees/tree_small_02_leaves.lod1.lmesh");
    EXPECT_EQ(LodMeshPath(base, 2), "data/models/trees/tree_small_02_leaves.lod2.lmesh");
}

TEST(TreeLod, LodPathAppendsWhenNoLmeshExtension) {
    EXPECT_EQ(LodMeshPath("tree_part", 1), "tree_part.lod1");
}

// Budget gate: model a scattered grove at increasing distances and confirm the
// per-frame visible-triangle count drops once LOD is on. We approximate the LOD
// triangle counts as monotonically decreasing fractions of the LOD0 budget
// (LOD0 = full, LOD1 = ~1/2, LOD2 = ~1/6) -- matching the asset_processor
// --emit-lods budgets -- and assert the LOD-on total is strictly smaller while
// the NEAREST trees still cost their full LOD0 triangle count (near field
// unchanged).
TEST(TreeLod, VisibleTriangleBudgetDropsWithLod) {
    const auto cfg = MakeConfig();
    // Representative LOD0 triangle count for one tree (all 3 parts combined).
    constexpr std::uint64_t kLod0Tris = 12000;
    // Fraction of LOD0 tris per LOD bucket (LOD1 ~ 1/2, LOD2 ~ 1/6, LOD3 billboard ~ 1/2000).
    const double kFrac[Luminumbra::Rendering::kTreeLodCount] = {1.0, 0.5, 1.0 / 6.0, 6.0 / 12000.0};

    std::uint64_t baselineTris = 0; // everything at LOD0
    std::uint64_t lodTris = 0;      // distance-selected LOD
    std::uint64_t nearBaseline = 0, nearLod = 0;

    // 400 trees scattered from the camera out to 600 units.
    const int kTrees = 400;
    for (int i = 0; i < kTrees; ++i) {
        const float dist = static_cast<float>(i) / static_cast<float>(kTrees) * 600.0f;
        const int lod = SelectTreeLod(dist, cfg);
        baselineTris += kLod0Tris;
        lodTris += static_cast<std::uint64_t>(kLod0Tris * kFrac[lod]);
        if (dist < cfg.lod1Distance) {
            nearBaseline += kLod0Tris;
            nearLod += static_cast<std::uint64_t>(kLod0Tris * kFrac[lod]);
        }
    }

    // Far-field optimization: LOD on draws strictly fewer triangles overall.
    EXPECT_LT(lodTris, baselineTris);
    // Near field is byte-identical: trees inside the LOD0 radius still cost full tris.
    EXPECT_EQ(nearLod, nearBaseline);
    // Sanity: meaningful savings (> 25% reduction) on this spread.
    EXPECT_LT(lodTris, baselineTris * 3 / 4);
}
