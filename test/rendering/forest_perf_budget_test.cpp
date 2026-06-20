// far-field-source-unification (FR-003 / AC-003) + perf-lane-and-ecology-tick
// FR-001 forest scenario: the forest PERF-BUDGET harness.
//
// This is pillar-B's FIRST slice per the spec: a RED budget gate that captures the
// foliage geometry draw load (instance count, draw calls, foliage triangles/frame)
// at a PINNED 16k-tree load and FAILS at today's load -- BEFORE octahedral impostors
// (Wave 3) exist. It is the harness impostors must flip GREEN; it is intended RED now.
//
// SHARED, not duplicated: the GPU FRAME-MS forest scenario already lives in the
// release `initial_world_loading_perf_test` "forest" benchmark gated by -Mode
// PerfFloor (fail-until-baselined on the target GPU). This harness adds the
// FOLIAGE-SPECIFIC GEOMETRY counters that the GPU frame-ms floor does not measure:
// per-LOD instance counts, the draw-call count under the production GBufferPass
// batching model, and the per-frame visible-triangle total. GPU frame-ms is NOT
// measured here (no headless GL context) -> it is emitted as null and marked
// "gpu_ms_unblessed".
//
// The harness is OpenGL-free: it exercises the pure, render-only LOD selection in
// src/luminumbra_client/rendering/TreeLod.h (the same function the G-Buffer draw
// pass uses) over a deterministic phyllotaxis forest, and models the GBufferPass
// draw-call batching (group by mesh-part x LOD variant, one instanced draw per
// group, split every kStaticInstanceCapacity=16384 instances) WITHOUT a GL context.
// Nothing here touches the sim / world_hash path.
//
// RED-by-design: with today's geometry (no LOD3 hemi-octa impostor atlas folding the
// far field into a shared cross-billboard, no per-frame static-instance dirty cache),
// the 16k-tree visible-triangle total at the pinned camera EXCEEDS the budget. The
// budget assertions therefore FAIL today on purpose; the artifact is still emitted so
// the validator (-Mode FarFieldForestBudget) and Wave-3 impostors can read the numbers
// and certify "done" only when draw-calls/triangles drop under budget with impostors ON.

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
using Luminumbra::Rendering::TreeLodConfig;
using Luminumbra::Rendering::SelectTreeLod;
using Luminumbra::Rendering::kTreeLodCount;

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "rendering";
}

// --- Pinned load parameters (the harness contract; do NOT tune to pass) ----------
//
// 16k trees is the spec's pinned forest load (FR-003). The camera sits at the forest
// edge looking in, at 3840x1600 (recorded for provenance; this CPU harness measures
// geometry, not pixels, so resolution is metadata only). The disc spreads the trees
// by phyllotaxis so a realistic distance distribution feeds the LOD selector.
constexpr int kPinnedTreeCount = 16000;
constexpr float kPinnedSpacingM = 1.6f;     // ~dense conifer stand spacing
constexpr int kCaptureWidth = 3840;         // provenance only (CPU geometry harness)
constexpr int kCaptureHeight = 1600;

// Each tree asset is 3 instanced static-mesh PARTS (trunk/bark + 2 procedural leaf
// submeshes), matching the GBufferPass static-model grouping. LOD0 triangle budget
// per part is the tree_lod_test representative (12000 tris across all 3 parts).
constexpr int kPartsPerTree = 3;
constexpr std::uint64_t kLod0TrisPerTree = 12000;
// Per-LOD fraction of the LOD0 triangle budget, matching tree_lod_test's
// asset_processor --emit-lods budgets (LOD1 ~ 1/2, LOD2 ~ 1/6, LOD3 cross-billboard).
const double kLodTriFrac[kTreeLodCount] = {1.0, 0.5, 1.0 / 6.0, 6.0 / 12000.0};

// GBufferPass batches instanced static meshes into one glDrawElementsInstanced per
// (mesh-part x LOD-variant) group, splitting a group every kStaticInstanceCapacity
// instances (GBufferPass.cpp:34). Mirror that constant so the modeled draw-call count
// matches the production batcher.
constexpr int kStaticInstanceCapacity = 16384;

// --- BUDGETS (the RED gate) ------------------------------------------------------
//
// These are the geometry ceilings the far field must hold at the 16k load. Today's
// load (no impostor atlas, no dirty cache) EXCEEDS them -> the gate is RED by design.
// They are deliberately tight enough that the current all-real-geometry far field
// fails, and loose enough that a LOD3 hemi-octa impostor far field (Wave 3) can pass.
//
//  * Triangle budget: a vast 16k-tree forest must stay well under ~12M visible tris/
//    frame for the far field to fit a 300fps target alongside terrain/sky/water. With
//    NO impostors, even distance-LOD'd, 16k trees blow past this (see the emitted
//    number); with a LOD3 impostor atlas folding the bulk of the stand into a few
//    quads, it drops under.
//  * Draw-call budget: the static-mesh foliage far field must batch into a small,
//    fixed number of instanced draws. Today the distant trees still issue real
//    per-part instanced draws; a shared impostor atlas collapses them.
constexpr std::uint64_t kFoliageTriBudgetPerFrame = 12'000'000ull;
constexpr int kFoliageDrawCallBudgetPerFrame = 24;

// Deterministic phyllotaxis ("sunflower") forest: trees spread across a disc so the
// LOD selector sees a realistic near->far distance distribution rather than a single
// cell. Camera at the disc edge looking toward centre. Returns per-instance camera
// distances (libm-free via DeterministicMath -- this harness must stay clean even
// though these positions feed no hashed sim term).
std::vector<float> BuildForestDistances(int n, float spacing) {
    constexpr float kGoldenAngle = 2.39996323f;  // 137.5 deg, radians
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
    std::uint64_t total_instances = 0;   // trees x parts
    int draw_calls = 0;
    std::uint64_t foliage_tris = 0;
};

// Model the per-frame foliage draw load for the pinned forest under the production
// LOD selection + GBufferPass batching, with NO impostor atlas / dirty cache (today).
ForestBudgetResult MeasureForestLoad(int n, float spacing, const TreeLodConfig& cfg) {
    ForestBudgetResult out;
    out.tree_count = n;

    const std::vector<float> distances = BuildForestDistances(n, spacing);

    // Per (LOD bucket) instance tally. Today every tree -- including the far field --
    // still emits kPartsPerTree real instanced parts at its selected LOD; LOD3 here is
    // the cross-billboard placeholder, NOT a shared impostor atlas. The draw-call model
    // groups by (part-index x LOD) since GBufferPass keys batches on mesh+variant path.
    std::map<int, std::uint64_t> instances_per_group;  // group key -> instance count
    for (float d : distances) {
        const int lod = SelectTreeLod(d, cfg);
        out.instances_by_lod[lod] += static_cast<std::uint64_t>(kPartsPerTree);
        out.total_instances += static_cast<std::uint64_t>(kPartsPerTree);
        out.foliage_tris += static_cast<std::uint64_t>(
            static_cast<double>(kLod0TrisPerTree) * kLodTriFrac[lod]);
        // Each of the 3 parts at this LOD is its own (part x LOD) batch group.
        for (int part = 0; part < kPartsPerTree; ++part) {
            const int group_key = lod * kPartsPerTree + part;
            instances_per_group[group_key] += 1;
        }
    }

    // Draw calls: one instanced draw per group, split every kStaticInstanceCapacity.
    int draw_calls = 0;
    for (const auto& [key, count] : instances_per_group) {
        (void)key;
        if (count == 0) continue;
        draw_calls += static_cast<int>(
            (count + static_cast<std::uint64_t>(kStaticInstanceCapacity) - 1) /
            static_cast<std::uint64_t>(kStaticInstanceCapacity));
    }
    out.draw_calls = draw_calls;
    return out;
}

// Emit the budget artifact the -Mode FarFieldForestBudget validator + Wave-3
// impostors read. `over_budget` records the RED state explicitly so the artifact is
// self-describing even when read outside ctest.
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
         {{"foliage_tris", tri_over},
          {"draw_calls", draw_over},
          {"any", tri_over || draw_over}}},
        // GPU frame-ms is NOT measured by this CPU geometry harness (no headless GL
        // context). It stays null + flagged unblessed -> the release frame-ms floor
        // is carried by -Mode PerfFloor's "forest" scenario, not duplicated here.
        {"gpu_frame_ms", nullptr},
        {"gpu_ms_unblessed", true},
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

// FR-003 / AC-003: the RED budget gate. At the pinned 16k load the foliage triangle
// total EXCEEDS the per-frame budget today (no impostor atlas), so this test FAILS by
// design. The artifact is emitted first so the numbers are always available, then the
// budget is asserted (the failing assertion is the RED signal Wave-3 impostors flip).
TEST(ForestPerfBudget, RedAt16kTreeLoad) {
    const TreeLodConfig cfg = ProductionConfig();
    const ForestBudgetResult r =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, cfg);

    const bool tri_over = r.foliage_tris > kFoliageTriBudgetPerFrame;
    const bool draw_over = r.draw_calls > kFoliageDrawCallBudgetPerFrame;

    // Always emit the measured numbers (for the validator + Wave-3 certification),
    // independent of the budget verdict below.
    EmitArtifact(r, tri_over, draw_over);

    // Non-vacuity: the harness must have exercised the real pinned load (all parts
    // counted, a positive triangle total spread across LOD buckets). If this trips,
    // the budget verdict below is meaningless.
    ASSERT_EQ(r.total_instances,
              static_cast<std::uint64_t>(kPinnedTreeCount) * kPartsPerTree);
    ASSERT_GT(r.foliage_tris, 0u);
    ASSERT_GT(r.instances_by_lod[0], 0u) << "near field must keep full-res LOD0 trees";

    // The RED budget. With today's far field (no LOD3 impostor atlas, no dirty cache)
    // the 16k load is over the triangle budget -> these EXPECTs FAIL on purpose. They
    // become the GREEN target for Wave-3 octahedral impostors.
    EXPECT_LE(r.foliage_tris, kFoliageTriBudgetPerFrame)
        << "16k-tree foliage triangle load " << r.foliage_tris
        << " exceeds the per-frame budget " << kFoliageTriBudgetPerFrame
        << " (RED until LOD3 hemi-octa impostors land -- far-field-source-unification "
           "FR-004 / AC-003)";
    EXPECT_LE(r.draw_calls, kFoliageDrawCallBudgetPerFrame)
        << "16k-tree foliage draw-call load " << r.draw_calls
        << " exceeds the per-frame budget " << kFoliageDrawCallBudgetPerFrame;
}

// Companion GREEN guards that always hold (so the harness's plumbing is itself tested,
// and the RED test above is the ONLY intended failure). These also document the
// invariants Wave-3 must preserve when it flips the budget green.

TEST(ForestPerfBudget, ArtifactIsEmittedAndWellFormed) {
    const TreeLodConfig cfg = ProductionConfig();
    const ForestBudgetResult r =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, cfg);
    EmitArtifact(r, r.foliage_tris > kFoliageTriBudgetPerFrame,
                 r.draw_calls > kFoliageDrawCallBudgetPerFrame);

    const fs::path out = ArtifactRoot() / "forest_perf_budget.json";
    ASSERT_TRUE(fs::exists(out)) << out.string();
    std::ifstream in(out);
    nlohmann::json doc;
    in >> doc;
    EXPECT_EQ(doc["schema"], "luminumbra.forest_perf_budget.v1");
    EXPECT_EQ(doc["pinned_tree_count"], kPinnedTreeCount);
    EXPECT_TRUE(doc["gpu_frame_ms"].is_null());
    EXPECT_TRUE(doc["gpu_ms_unblessed"].get<bool>());
}

TEST(ForestPerfBudget, DisablingLodCannotReduceLoad) {
    // Sanity on the model: with LOD OFF every tree is LOD0, so the load is the
    // worst case and the triangle total is the all-LOD0 ceiling. This guards against
    // a future LOD config accidentally making the "no LOD" path cheaper than LOD-on.
    TreeLodConfig on = ProductionConfig();
    TreeLodConfig off = ProductionConfig();
    off.enabled = false;

    const ForestBudgetResult lodOn =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, on);
    const ForestBudgetResult lodOff =
        MeasureForestLoad(kPinnedTreeCount, kPinnedSpacingM, off);

    EXPECT_GE(lodOff.foliage_tris, lodOn.foliage_tris)
        << "LOD-off (all LOD0) must never draw fewer triangles than LOD-on";
    EXPECT_EQ(lodOff.foliage_tris,
              static_cast<std::uint64_t>(kPinnedTreeCount) * kLod0TrisPerTree)
        << "LOD-off must be exactly the all-LOD0 triangle ceiling";
}

}  // namespace
