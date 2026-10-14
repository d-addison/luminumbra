#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_common/world/FarLodStore.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;

struct LegacyRuntime {
    unsigned schedules = 0, writes = 0, submissions = 0, water_supplements = 0;
    void Advance() {
        ++schedules;
        ++writes;
    }
    FarWorldSubmitStats Submit() {
        ++submissions;
        ++water_supplements;
        return {1, 3, 0, 3};
    }
};

std::string Source(const char* relative) {
    const auto path = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / relative;
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open()) << path;
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void AppendLittleEndian(std::string& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<char>(value & 0xffu));
        value >>= 8;
    }
}

// Independent FSD2 v3 pristine F2 record, negative region, no SDF bricks.
// Kept inline so the compatibility decoder is exercised without a runtime writer.
std::string CompatibilityPayload() {
    std::string bytes = "FSD2";
    AppendLittleEndian(bytes, 3, 2);
    AppendLittleEndian(bytes, 2, 1);
    AppendLittleEndian(bytes, 0xffffffffu, 4); // rx = -1
    AppendLittleEndian(bytes, 2, 4);
    AppendLittleEndian(bytes, 65, 4);
    AppendLittleEndian(bytes, 42, 8);
    AppendLittleEndian(bytes, 0, 1); // pristine
    constexpr std::uint32_t count = 65u * 65u;
    for (const unsigned element_bytes : {2u, 1u, 1u}) {
        AppendLittleEndian(bytes, count, 4);
        AppendLittleEndian(bytes, count * element_bytes, 4);
        bytes.append(static_cast<std::size_t>(count) * element_bytes, '\0');
    }
    AppendLittleEndian(bytes, 0, 4); // brick count
    for (unsigned field = 0; field < 4; ++field)
        AppendLittleEndian(bytes, 0, 4); // density/material counts and lengths
    return bytes;
}
} // namespace

TEST(FarWorldLegacyIsolation, CompatibilityFollowsEffectiveSelectionIncludingRefusalsAndPreview) {
    LegacyRuntime runtime;
    const FarWorldRenderer defaults;
    EXPECT_EQ(defaults.Selection(FarWorldPurpose::World).requested, FarWorldMode::Legacy);
    const std::array selections{
        defaults.Selection(FarWorldPurpose::World),
        SelectFarWorld(FarWorldMode::Ladder,
                       FarWorldPurpose::World,
                       FarWorldRefusal::ImmutableAuthorityUnavailable),
        SelectFarWorld(FarWorldMode::Ladder,
                       FarWorldPurpose::World,
                       FarWorldRefusal::ShaderProgramUnavailable),
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::Preview, FarWorldRefusal::None),
        SelectFarWorld(
            FarWorldMode::Ladder, FarWorldPurpose::OffscreenPreview, FarWorldRefusal::None)};
    for (const auto& selection : selections) {
        EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
        EXPECT_EQ(FarWorldPlane(selection.effective), 3200.0f);
        EXPECT_EQ(LegacyFarWorldCompatibility(selection, &runtime), &runtime);
        const auto disabled = ApplyLegacyFarWorldEnable(selection, false);
        EXPECT_FALSE(disabled.advance);
        EXPECT_FALSE(disabled.submit);
    }
    EXPECT_FALSE(selections.back().advance);
    EXPECT_FALSE(selections.back().submit);
    const LegacyRuntime* const_runtime = &runtime;
    EXPECT_EQ(LegacyFarWorldCompatibility(selections.front(), const_runtime), const_runtime);
}

TEST(FarWorldLegacyIsolation,
     LadderDispatchHasOneSchedulerAndSubmissionWithoutLegacyWriterOrWater) {
    LegacyRuntime runtime;
    unsigned ladder_schedules = 0, ladder_submissions = 0;
    const auto selected = ApplyLegacyFarWorldEnable(
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None), false);
    auto* compatibility = LegacyFarWorldCompatibility(selected, &runtime);
    ASSERT_EQ(compatibility, nullptr);
    EXPECT_TRUE(selected.advance);
    EXPECT_TRUE(selected.submit);
    AdvanceSelectedFarWorld(selected, [&] { runtime.Advance(); }, [&] { ++ladder_schedules; });
    const auto submitted = SubmitSelectedFarWorld(
        selected,
        [&] { return runtime.Submit(); },
        [&](const auto&) {
            ++ladder_submissions;
            return FarWorldSubmitStats{1, 6, 0, 6};
        },
        {});
    EXPECT_EQ(ladder_schedules, 1u);
    EXPECT_EQ(ladder_submissions, 1u);
    EXPECT_EQ(submitted.water_indices, 6u); // selected combined water, no supplement
    EXPECT_EQ(runtime.schedules, 0u);
    EXPECT_EQ(runtime.writes, 0u);
    EXPECT_EQ(runtime.submissions, 0u);
    EXPECT_EQ(runtime.water_supplements, 0u);
}

TEST(FarWorldLegacyIsolation,
     CompatibilityDispatchRetainsOneLegacySchedulerWriterAndWaterSubmission) {
    LegacyRuntime runtime;
    unsigned ladder_calls = 0;
    const FarWorldRenderer defaults;
    const auto selection = defaults.Selection(FarWorldPurpose::World);
    AdvanceSelectedFarWorld(selection, [&] { runtime.Advance(); }, [&] { ++ladder_calls; });
    const auto submitted = SubmitSelectedFarWorld(
        selection,
        [&] { return runtime.Submit(); },
        [&](const auto&) {
            ++ladder_calls;
            return FarWorldSubmitStats{};
        },
        {});
    EXPECT_EQ(runtime.schedules, 1u);
    EXPECT_EQ(runtime.writes, 1u);
    EXPECT_EQ(runtime.submissions, 1u);
    EXPECT_EQ(runtime.water_supplements, 1u);
    EXPECT_EQ(submitted.water_indices, 3u);
    EXPECT_EQ(ladder_calls, 0u);
}

TEST(FarWorldLegacyIsolation, PairedOffDispatchPreservesSelectedFramePlanePolicyAndOnEvidence) {
    FarWorldRenderer renderer(FarWorldMode::Ladder);
    renderer.SetRequestedCavePolicy(FarCaveMode::BoxFiltered);
    const auto selection =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    renderer.CaptureDiagnostic(selection, 37, {-512, 20, 1024}, kFarPlaneMeters, true, 2, 6);
    const auto before = FarWorldDiagnosticJson(renderer.Diagnostic());
    const auto off = FarWorldSubmissionOverride(selection, false);
    unsigned calls = 0;
    AdvanceSelectedFarWorld(off, [&] { ++calls; }, [&] { ++calls; });
    SubmitSelectedFarWorld(
        off,
        [&] {
            ++calls;
            return FarWorldSubmitStats{};
        },
        [&](const auto&) {
            ++calls;
            return FarWorldSubmitStats{};
        },
        {});
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(off.effective, FarWorldMode::Ladder);
    EXPECT_EQ(FarWorldPlane(off.effective), 17408.0f);
    EXPECT_EQ(renderer.Diagnostic().frame, 37u);
    EXPECT_EQ(renderer.Diagnostic().requested_cave_policy, FarCaveMode::BoxFiltered);
    EXPECT_EQ(FarWorldDiagnosticJson(renderer.Diagnostic()), before);
    EXPECT_EQ(before["effective_mode"], "ladder");
    EXPECT_EQ(before["far_plane_m"], 17408.0f);
    EXPECT_EQ(before["frame_id"], 37u);
    for (const char* field : {"update_frame",
                              "resident_wanted",
                              "missing_after_eviction",
                              "clip_inner_radius_m",
                              "clip_outer_radius_m",
                              "camera_neighbourhood"})
        EXPECT_FALSE(before.contains(field)) << field;
    EXPECT_EQ(CheckFarWorldStation(renderer.Diagnostic(), {}, 1),
              FarWorldStationRefusal::InvalidBinding); // dispatch alone cannot qualify
}

TEST(FarWorldLegacyIsolation,
     RuntimeConsumersUseCompatibilityBoundaryAndNeverWireLegacySaveDirectory) {
    const auto pipeline = Source("src/luminumbra_client/rendering/RenderPipeline.cpp");
    EXPECT_NE(pipeline.find("gbuffer_input.legacy_far = legacy_far_compatibility();"),
              std::string::npos);
    EXPECT_EQ(pipeline.find("set_save_dir("), std::string::npos);
    EXPECT_EQ(pipeline.find("m_farlod->stats()"), std::string::npos);
    EXPECT_EQ(pipeline.find("m_farlod->resident_vertex_array_count()"), std::string::npos);
    for (const char* path : {"src/luminumbra_client/app/RuntimeStateRecorder.cpp",
                             "src/luminumbra_client/core/ScenarioRunnerCapture.cpp",
                             "src/luminumbra_client/rendering/TerrainCoverageCapture.cpp"}) {
        const auto caller = Source(path);
        EXPECT_NE(caller.find("legacy_far_compatibility()"), std::string::npos);
        EXPECT_EQ(caller.find(".farlod()"), std::string::npos);
        EXPECT_EQ(caller.find("->farlod()"), std::string::npos);
        EXPECT_EQ(caller.find("m_farlod"), std::string::npos);
    }
}

TEST(FarWorldLegacyIsolation, TerrainCoverageLadderUsesSelectedSnapshotAndGuardsLegacySetter) {
    const auto capture = Source("src/luminumbra_client/rendering/TerrainCoverageCapture.cpp");
    EXPECT_NE(capture.find(R"(if (auto* legacy = legacy_far_compatibility()) {
        legacy->set_coverage_diagnostics_enabled(enabled);
        legacy->set_coverage_camera_region_guard_bypass(bypass_camera_region_guard);
    })"),
              std::string::npos);
    // Effective mode includes refused activation and preview overrides. The
    // retained ON snapshot also survives paired submission suppression.
    const auto ladder_begin =
        capture.find(R"(} else if (far_world_selection().effective == FarWorldMode::Ladder) {
        out["far"] = FarWorldDiagnosticJson(far_world_diagnostic());
    } else {
        out["far"] = nullptr;
    })");
    const bool has_ladder_branch = ladder_begin != std::string::npos;
    ASSERT_TRUE(has_ladder_branch);
    if (!has_ladder_branch)
        return;
    EXPECT_EQ(capture.find("legacy->", ladder_begin), std::string::npos);
    EXPECT_EQ(capture.find("FarLodSystem::", ladder_begin), std::string::npos);
    EXPECT_EQ(capture.find("m_farlod"), std::string::npos);
}

TEST(FarWorldLegacyIsolation, TerrainCoverageLegacyRetainsPayloadFieldValuesAndOrder) {
    const auto capture = Source("src/luminumbra_client/rendering/TerrainCoverageCapture.cpp");
    const auto legacy_begin = capture.find(R"(if (const auto* legacy = legacy_far_compatibility()) {
        const auto& d = legacy->coverage_diagnostics();
        const auto& s = legacy->stats();)");
    const auto ladder_begin =
        capture.find("} else if (far_world_selection().effective == FarWorldMode::Ladder)");
    const bool has_branches = legacy_begin != std::string::npos &&
                              ladder_begin != std::string::npos && legacy_begin < ladder_begin;
    ASSERT_TRUE(has_branches);
    if (!has_branches)
        return;
    // Pin the historical serialization initializer, including its values and
    // field order. Only the owner lookup changes for default/refused/preview.
    const std::string expected = R"(out["far"] = {{"update_frame", d.update_frame},
                      {"enabled", s.enabled},
                      {"draw_observed", d.draw_observed},
                      {"camera", Vec(d.camera)},
                      {"wanted", d.wanted},
                      {"resident_wanted", d.resident},
                      {"missing_after_eviction", d.missing},
                      {"stale_after_eviction", d.stale},
                      {"pending", d.pending},
                      {"resident_bytes", s.resident_bytes},
                      {"draws", s.region_draws},
                      {"indices", s.indices_drawn},
                      {"builds_failed_total", s.builds_failed_total},
                      {"authority_build_failures", s.authority_build_failures},
                      {"stale_results_rejected", s.stale_results_rejected},
                      {"camera_region_guard_bypassed", d.camera_region_guard_bypassed},
                      {"preview_anchored", d.preview_anchored},
                      {"preview_anchor", Vec(d.preview_anchor)},
                      {"preview_inner_radius_m", d.preview_inner_radius},
                      {"clip_inner_radius_m", FarLodSystem::kFarClipInnerRadiusMeters},
                      {"clip_outer_radius_m", FarLodSystem::kFarClipOuterRadiusMeters},
                      {"camera_neighbourhood", std::move(rows)}};)";
    const auto legacy_branch = capture.substr(legacy_begin, ladder_begin - legacy_begin);
    EXPECT_NE(legacy_branch.find(expected), std::string::npos);
    EXPECT_EQ(legacy_branch.find("FarWorldDiagnosticJson("), std::string::npos);
}

TEST(FarWorldLegacyIsolation, LegacyRangesAndHistoricalThresholdStayInsideCompatibility) {
    const auto legacy = Source("src/luminumbra_client/rendering/FarLodSystem.h");
    EXPECT_NE(legacy.find("kF1OuterRangeMeters = 768.0f"), std::string::npos);
    EXPECT_NE(legacy.find("kF2OuterRangeMeters = 3000.0f"), std::string::npos);
    EXPECT_NE(legacy.find("kFarClipOuterRadiusMeters = 3050.0f"), std::string::npos);
    EXPECT_EQ(FarWorldPlane(FarWorldMode::Legacy), 3200.0f);
    EXPECT_EQ(FarWorldPlane(FarWorldMode::Ladder), 17408.0f);
    const auto writer = Source("src/luminumbra_client/core/scenarios/FarLodHorizonSmoke.cpp");
    const auto legacy_writer = writer.find("WriteLegacyFarLodHorizonAnalysis(");
    const auto selected_writer = writer.find("void WriteFarLodHorizonAnalysis(");
    const auto threshold = writer.find("{\"f2_outer_range_m\", 1536.0}");
    ASSERT_NE(legacy_writer, std::string::npos);
    ASSERT_NE(selected_writer, std::string::npos);
    ASSERT_NE(threshold, std::string::npos);
    EXPECT_LT(legacy_writer, threshold);
    EXPECT_LT(threshold, selected_writer);
    EXPECT_EQ(writer.find("luminumbra.farlod_horizon.v1", selected_writer), std::string::npos);
}

TEST(FarWorldLegacyIsolation, PipelineDrainsBothRetainedOwnersDuringSwapAndShutdown) {
    const auto pipeline = Source("src/luminumbra_client/rendering/RenderPipeline.cpp");
    for (const char* signature : {"void RenderPipeline::prepare_world_swap()",
                                  "void RenderPipeline::cleanup_gpu_resources()"}) {
        const auto begin = pipeline.find(signature);
        ASSERT_NE(begin, std::string::npos);
        // Both drain calls are at entry, before live GPU/world resources retire.
        const auto entry = pipeline.substr(begin, 800);
        EXPECT_NE(entry.find("m_far_world->"), std::string::npos);
        EXPECT_NE(entry.find("m_farlod->"), std::string::npos);
        EXPECT_EQ(entry.find("legacy_far_compatibility()"), std::string::npos);
        EXPECT_EQ(entry.find(".effective"), std::string::npos);
    }
}

TEST(FarWorldLegacyIsolation, Fsd2Version3DecodeRemainsAvailableUnderEitherSelection) {
    const auto payload = CompatibilityPayload();
    for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
        SCOPED_TRACE(FarWorldModeName(mode));
        FarLodTile tile;
        std::vector<std::string> errors;
        ASSERT_TRUE(FarLodStore::decode_payload(payload, FarLodTier::F2, -1, 2, 0, tile, &errors));
        EXPECT_TRUE(errors.empty());
        EXPECT_EQ(tile.samples_per_side, 65u);
        EXPECT_EQ(tile.params_hash, 42u);
        EXPECT_EQ(tile.height_q.size(), 65u * 65u);
        EXPECT_EQ(tile.material.size(), tile.height_q.size());
        EXPECT_EQ(tile.flags.size(), tile.height_q.size());
        EXPECT_TRUE(tile.sdf_bricks.empty());
        EXPECT_FALSE(FarLodStore::decode_payload(payload, FarLodTier::F2, 0, 2, 0, tile, &errors));
        EXPECT_TRUE(tile.height_q.empty());
        EXPECT_FALSE(errors.empty());
    }
}
