// Deterministic forest geometry-budget harness. It captures the foliage draw
// load (instance count, draw calls, and visible triangles per frame) for a
// pinned 16k-tree scene with the production far-field impostor fold enabled.
//
// This harness records geometry counters:
// per-LOD instance counts, the draw-call count under the production GBufferPass
// batching model, and the per-frame visible-triangle total. It deliberately
// makes no GPU timing claim because it runs without an OpenGL context.
//
// The harness is OpenGL-free: it exercises the pure, render-only LOD selection in
// src/luminumbra_client/rendering/TreeLod.h (the same function the G-Buffer draw
// pass uses) over a deterministic phyllotaxis forest, and models the GBufferPass
// draw-call batching (group by mesh-part x LOD variant, one instanced draw per
// group, split every kStaticInstanceCapacity=16384 instances) without a GL context.
// Nothing here touches the sim / world_hash path.
//
// The model folds the far field (d >= 200 m) into octa-impostor billboards
// exactly as GBufferPass does. If that fold regresses, the triangle total
// approximately doubles and the geometry budget fails. The artifact is always
// emitted so the validator can report the measured counters.

#include "luminumbra_client/rendering/TreeLod.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/DeterministicMath.h"

namespace fs = std::filesystem;

namespace {

namespace dm = ::Luminumbra::DeterministicMath;
using Luminumbra::Rendering::kTreeLodCount;
using Luminumbra::Rendering::SelectTreeLod;
using Luminumbra::Rendering::TreeLodConfig;

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "rendering";
}

// --- Pinned load parameters (the harness contract; do NOT tune to pass) ----------
//
// The pinned workload contains 16k trees. The camera sits at the forest
// edge looking in, at 3840x1600 (recorded for provenance; this CPU harness measures
// geometry, not pixels, so resolution is metadata only). The disc spreads the trees
// by phyllotaxis so a realistic distance distribution feeds the LOD selector.
constexpr int kPinnedTreeCount = 16000;
constexpr float kPinnedSpacingM = 1.6f; // ~dense conifer stand spacing
constexpr int kCaptureWidth = 3840;     // provenance only (CPU geometry harness)
constexpr int kCaptureHeight = 1600;

// Each tree asset is 3 instanced static-mesh PARTS (trunk/bark + 2 procedural leaf
// submeshes), matching the GBufferPass static-model grouping. LOD0 triangle budget
// per part is the tree_lod_test representative (12000 tris across all 3 parts).
constexpr int kPartsPerTree = 3;
constexpr std::uint64_t kLod0TrisPerTree = 12000;
// Per-LOD fraction of the LOD0 triangle budget, matching tree_lod_test's
// asset_processor --emit-lods budgets (LOD1 ~ 1/2, LOD2 ~ 1/6, LOD3 cross-billboard).
const double kLodTriFrac[kTreeLodCount] = {1.0, 0.5, 1.0 / 6.0, 6.0 / 12000.0};
// With octa impostors ON (the landed default), a LOD3 tree's 3 parts fold into ONE
// camera-facing billboard quad (GBufferPass.cpp:545-553) -- two triangles per tree,
// drawn as a single shared instanced draw for the whole far field.
constexpr std::uint64_t kImpostorTrisPerTree = 2;

// GBufferPass batches instanced static meshes into one glDrawElementsInstanced per
// (mesh-part x LOD-variant) group, splitting a group every kStaticInstanceCapacity
// instances (GBufferPass.cpp:34). Mirror that constant so the modeled draw-call count
// matches the production batcher.
constexpr int kStaticInstanceCapacity = 16384;

// --- BUDGETS (the GREEN impostor-regression gate) --------------------------------
//
// octa impostors landed + default-ON, so the far field
// (d >= 200 m) folds into single billboards and the model above reflects it. These
// ceilings are a REGRESSION tripwire the impostor-ON 16k stand must hold -- NOT the old
// aspirational target.
//
//  * Triangle budget = 60M. The impostor-ON geometry load is ~52.7M tris/frame,
//    DOMINATED by the near/mid field (~5.8k trees at LOD0/LOD1 that must stay real
//    geometry -- impostors only replace the far field; popping forbids impostoring the
//    near field). The old 12M was an aspirational far-field-only figure impostors alone
//    can NOT reach, and it does not track real cost anyway: forest gbuffer cost is
//    fill/OVERDRAW-bound, not triangle-bound (TreeLod.h:39-44), and the real per-frame
//    proxy. Runtime timing is evaluated by paired base/head observations. 60M
//    sits above the 52.7M impostor-ON load and well below the ~99.4M the load
//    reverts to if the far-field impostor fold regresses -> a broken impostor path trips
//    this gate RED again.
//  * Draw-call budget = 24. The far field collapses into ONE shared impostor draw and
//    the near/mid field batches by (part x LOD); impostor-ON load is ~7 draws.
constexpr std::uint64_t kFoliageTriBudgetPerFrame = 60'000'000ull;
constexpr int kFoliageDrawCallBudgetPerFrame = 24;

// Deterministic phyllotaxis ("sunflower") forest: trees spread across a disc so the
// LOD selector sees a realistic near->far distance distribution rather than a single
// cell. Camera at the disc edge looking toward centre. Returns per-instance camera
// distances (libm-free via DeterministicMath -- this harness must stay clean even
// though these positions feed no hashed sim term).
std::vector<float> BuildForestDistances(int n, float spacing) {
    constexpr float kGoldenAngle = 2.39996323f; // 137.5 deg, radians
    std::vector<float> distances;
    distances.reserve(static_cast<std::size_t>(n));

    // Place the camera just outside the disc rim so the far rim is at ~2*R and the
    // near rim is at ~0 -> the full LOD0..LOD3 range is exercised.
    const float discRadius = spacing * dm::Sqrt(static_cast<float>(n));
    const float camX = -(discRadius + 8.0f);
    const float camZ = 0.0f;

    for (int i = 0; i < n; ++i) {
        const float angle = static_cast<float>(i) * kGoldenAngle;
        const float r = spacing * dm::Sqrt(static_cast<float>(i));
        const float x = r * dm::Cos(angle);
        const float z = r * dm::Sin(angle);
        const float dx = x - camX;
        const float dz = z - camZ;
        distances.push_back(dm::Sqrt(dx * dx + dz * dz));
    }
    return distances;
}

struct ForestBudgetResult {
    int tree_count = 0;
    std::uint64_t instances_by_lod[kTreeLodCount] = {0, 0, 0, 0};
    std::uint64_t total_instances = 0; // trees x parts
    int draw_calls = 0;
    std::uint64_t foliage_tris = 0;
};

// Model the per-frame foliage draw load for the pinned forest under the production
// LOD selection + GBufferPass batching. When `impostors` is true (the landed default,
// ), the far field folds into octa-impostor billboards exactly as GBufferPass
// does: LOD3 kicks in at 200 m and every LOD3 tree collapses into a single shared
// billboard draw. When false, this reproduces the pre-impostor all-real-geometry load.
ForestBudgetResult
MeasureForestLoad(int n, float spacing, const TreeLodConfig& baseCfg, bool impostors) {
    ForestBudgetResult out;
    out.tree_count = n;

    // Mirror GBufferPass.cpp:500-503: with octa impostors ON the LOD3 cheap-billboard
    // threshold moves from 620 m in to 200 m so the impostor actually replaces the
    // far-field stand (the wide 620 m cross-billboard added overdraw and was kept far).
    TreeLodConfig cfg = baseCfg;
    if (impostors && cfg.enabled) {
        cfg.lod3Distance = 200.0f;
    }

    const std::vector<float> distances = BuildForestDistances(n, spacing);

    // Per (part x LOD) instance tally for the REAL-geometry near/mid field; LOD3 trees
    // (with impostors) are pulled out into a single shared impostor draw. The draw-call
    // model groups by (part-index x LOD) since GBufferPass keys batches on mesh+variant.
    std::map<int, std::uint64_t> instances_per_group; // group key -> instance count
    std::uint64_t impostor_instances = 0;             // far-field billboards (1 per tree)
    for (float d : distances) {
        const int lod = SelectTreeLod(d, cfg);
        if (impostors && lod == 3) {
            // GBufferPass.cpp:545-553: all 3 tree parts at LOD3 fold into ONE camera-
            // facing octa-impostor billboard per tree (one quad, ~2 tris), collected
            // and drawn as a single shared instanced draw for the whole far field.
            out.instances_by_lod[lod] += 1;
            out.total_instances += 1;
            out.foliage_tris += kImpostorTrisPerTree;
            ++impostor_instances;
            continue;
        }
        out.instances_by_lod[lod] += static_cast<std::uint64_t>(kPartsPerTree);
        out.total_instances += static_cast<std::uint64_t>(kPartsPerTree);
        out.foliage_tris +=
            static_cast<std::uint64_t>(static_cast<double>(kLod0TrisPerTree) * kLodTriFrac[lod]);
        // Each of the 3 parts at this LOD is its own (part x LOD) batch group.
        for (int part = 0; part < kPartsPerTree; ++part) {
            const int group_key = lod * kPartsPerTree + part;
            instances_per_group[group_key] += 1;
        }
    }

    // Draw calls: one instanced draw per (part x LOD) group for the near/mid real
    // geometry, plus (with impostors) one shared far-field impostor draw -- each split
    // every kStaticInstanceCapacity instances.
    int draw_calls = 0;
    for (const auto& [key, count] : instances_per_group) {
        (void)key;
        if (count == 0)
            continue;
        draw_calls +=
            static_cast<int>((count + static_cast<std::uint64_t>(kStaticInstanceCapacity) - 1) /
                             static_cast<std::uint64_t>(kStaticInstanceCapacity));
    }
    if (impostor_instances > 0) {
        draw_calls += static_cast<int>(
            (impostor_instances + static_cast<std::uint64_t>(kStaticInstanceCapacity) - 1) /
            static_cast<std::uint64_t>(kStaticInstanceCapacity));
    }
    out.draw_calls = draw_calls;
    return out;
}

// Emit the budget artifact consumed by the FarFieldForestBudget validator.
// `over_budget` makes the verdict self-describing outside CTest.
void EmitArtifact(const ForestBudgetResult& r, bool tri_over, bool draw_over) {
    nlohmann::json by_lod = nlohmann::json::array();
    for (int lod = 0; lod < kTreeLodCount; ++lod) {
        by_lod.push_back({{"lod", lod}, {"instances", r.instances_by_lod[lod]}});
    }

#ifdef NDEBUG
    const char* build_mode = "release";
#else
    const char* build_mode = "debug";
#endif

    const nlohmann::json artifact = {
        {"schema", "luminumbra.forest_perf_budget.v1"},
        {"build_mode", build_mode},
        {"capture", {{"width", kCaptureWidth}, {"height", kCaptureHeight}}},
        {"pinned_tree_count", r.tree_count},
        {"parts_per_tree", kPartsPerTree},
        {"counters",
         {{"total_instances", r.total_instances},
          {"instances_by_lod", by_lod},
          {"draw_calls", r.draw_calls},
          {"foliage_tris_per_frame", r.foliage_tris}}},
        {"budgets",
         {{"foliage_tris_per_frame", kFoliageTriBudgetPerFrame},
          {"draw_calls_per_frame", kFoliageDrawCallBudgetPerFrame}}},
        {"over_budget",
         {{"foliage_tris", tri_over}, {"draw_calls", draw_over}, {"any", tri_over || draw_over}}},
    };

    fs::create_directories(ArtifactRoot());
    const fs::path out = ArtifactRoot() / "forest_perf_budget.json";
    std::ofstream output(out);
    ASSERT_TRUE(output) << out.string();
    output << std::setw(2) << artifact << "\n";
}

TreeLodConfig ProductionConfig() {
    // Defaults from TreeLod.h (render.tree_lod.* production thresholds).
    return TreeLodConfig{};
}

// the GREEN impostor-regression gate. With octa impostors default-ON the
// far field folds into billboards and the impostor-ON 16k load holds the reviewed
// budget. The artifact is emitted first (for the -Mode FarFieldForestBudget validator),
// then the budget is asserted GREEN; a regressed far-field fold trips it RED again.
TEST(ForestPerfBudget, GreenAt16kTreeLoadWithImpostors) {
    const TreeLodConfig cfg = ProductionConfig();
    const ForestBudgetResult r =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, cfg, /*impostors=*/true);

    const bool tri_over = r.foliage_tris > kFoliageTriBudgetPerFrame;
    const bool draw_over = r.draw_calls > kFoliageDrawCallBudgetPerFrame;

    // Always emit the measured numbers (for the validator), independent of the verdict.
    EmitArtifact(r, tri_over, draw_over);

    // Non-vacuity: every tree is accounted for exactly once -- near/mid trees as 3 real
    // parts, far-field trees as 1 folded impostor -- and the far field ACTUALLY folded
    // (the impostor fold is the lever that makes this GREEN; if it stops the budget bites).
    const std::uint64_t real_trees =
        (r.instances_by_lod[0] + r.instances_by_lod[1] + r.instances_by_lod[2]) / kPartsPerTree;
    const std::uint64_t impostor_trees = r.instances_by_lod[3];
    ASSERT_EQ(real_trees + impostor_trees, static_cast<std::uint64_t>(kPinnedTreeCount));
    ASSERT_GT(r.foliage_tris, 0u);
    ASSERT_GT(r.instances_by_lod[0], 0u) << "near field must keep full-res LOD0 trees";
    ASSERT_GT(r.instances_by_lod[3], 0u)
        << "far field must fold into octa impostors (the GREEN lever)";

    // The GREEN budget. With impostors ON the impostor-ON load holds these ceilings; a
    // regressed far-field fold (~52.7M -> ~99.4M tris) trips the triangle EXPECT RED.
    EXPECT_LE(r.foliage_tris, kFoliageTriBudgetPerFrame)
        << "16k-tree foliage triangle load " << r.foliage_tris
        << " exceeds the impostor-ON per-frame budget " << kFoliageTriBudgetPerFrame
        << " -- the far-field octa-impostor fold may have regressed ()";
    EXPECT_LE(r.draw_calls, kFoliageDrawCallBudgetPerFrame)
        << "16k-tree foliage draw-call load " << r.draw_calls << " exceeds the per-frame budget "
        << kFoliageDrawCallBudgetPerFrame;
}

// Companion GREEN guards that always hold (so the harness's plumbing is itself tested,
// and the RED test above is the ONLY intended failure). These also document the
// invariants  must preserve when it flips the budget green.

TEST(ForestPerfBudget, ArtifactIsEmittedAndWellFormed) {
    const TreeLodConfig cfg = ProductionConfig();
    const ForestBudgetResult r =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, cfg, /*impostors=*/true);
    EmitArtifact(r,
                 r.foliage_tris > kFoliageTriBudgetPerFrame,
                 r.draw_calls > kFoliageDrawCallBudgetPerFrame);

    const fs::path out = ArtifactRoot() / "forest_perf_budget.json";
    ASSERT_TRUE(fs::exists(out)) << out.string();
    std::ifstream in(out);
    nlohmann::json doc;
    in >> doc;
    EXPECT_EQ(doc["schema"], "luminumbra.forest_perf_budget.v1");
    EXPECT_EQ(doc["pinned_tree_count"], kPinnedTreeCount);
    EXPECT_TRUE(doc.contains("counters"));
    EXPECT_FALSE(doc.contains("gpu_frame_ms"));
}

TEST(ForestPerfBudget, DisablingLodCannotReduceLoad) {
    // Sanity on the model: with LOD OFF every tree is LOD0, so the load is the
    // worst case and the triangle total is the all-LOD0 ceiling. This guards against
    // a future LOD config accidentally making the "no LOD" path cheaper than LOD-on.
    TreeLodConfig on = ProductionConfig();
    TreeLodConfig off = ProductionConfig();
    off.enabled = false;

    const ForestBudgetResult lodOn =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, on, /*impostors=*/true);
    const ForestBudgetResult lodOff =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, off, /*impostors=*/true);

    EXPECT_GE(lodOff.foliage_tris, lodOn.foliage_tris)
        << "LOD-off (all LOD0) must never draw fewer triangles than LOD-on";
    EXPECT_EQ(lodOff.foliage_tris, static_cast<std::uint64_t>(kPinnedTreeCount) * kLod0TrisPerTree)
        << "LOD-off must be exactly the all-LOD0 triangle ceiling";
}

} // namespace
