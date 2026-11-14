#include "../fixtures/TerrainFixtureParams.h"
#include "../performance/shieldrt_gl_harness.h"
#include "../support/UniqueTempPath.h"
#include "core/JobSystem.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarWorldHandoffDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "world/FarWorldAuthorityChanges.h"
#include "world/GameSession.h"
#include "world/WorldStreamingState.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace Luminumbra;
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
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    // Windows checkouts may use CRLF; the source patterns below are LF-only.
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
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
    const FarWorldRenderer legacy(FarWorldMode::Legacy);
    EXPECT_EQ(legacy.Selection(FarWorldPurpose::World).requested, FarWorldMode::Legacy);
    const std::array selections{
        legacy.Selection(FarWorldPurpose::World),
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
    const FarWorldRenderer legacy(FarWorldMode::Legacy);
    const auto selection = legacy.Selection(FarWorldPurpose::World);
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
                              "camera_neighbourhood"}) {
        EXPECT_FALSE(before.contains(field)) << field;
    }
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

namespace {
// No graphics calls or hidden buffer allocations. The selected submission is
// observable even while the first immutable pair is still queued.
class SessionDriver final : public FarVolumeGpuDriver {
public:
    bool ValidContext() const noexcept override {
        return true;
    }
    bool HasError() noexcept override {
        return false;
    }
    void GetVertexArrayBinding(std::uint32_t& name) noexcept override {
        name = vao_;
    }
    void GetVertexBufferBinding(std::uint32_t& name) noexcept override {
        name = vertex_;
    }
    void CreateVertexArray(std::uint32_t& name) noexcept override {
        name = next_++;
    }
    void CreateBuffer(std::uint32_t& name) noexcept override {
        name = next_++;
    }
    void DeleteVertexArray(std::uint32_t) noexcept override {}
    void DeleteBuffer(std::uint32_t) noexcept override {}
    void BindVertexArray(std::uint32_t name) noexcept override {
        vao_ = name;
    }
    void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept override {
        if (kind == FarVolumeBufferKind::Vertex) {
            vertex_ = name;
        } else {
            index_ = name;
        }
    }
    void
    UploadBuffer(FarVolumeBufferKind kind, std::intptr_t bytes, const void*) noexcept override {
        const auto name = kind == FarVolumeBufferKind::Vertex ? vertex_ : index_;
        if (name < bytes_.size()) {
            bytes_[name] = bytes;
        }
    }
    void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept override {
        const auto name = kind == FarVolumeBufferKind::Vertex ? vertex_ : index_;
        bytes = name < bytes_.size() ? bytes_[name] : 0;
    }
    void SetAttribute(FarVolumeVertexAttribute, std::int32_t, std::size_t) noexcept override {}
    void EnableAttribute(FarVolumeVertexAttribute) noexcept override {}
    bool DrawIndexed(std::int32_t count) noexcept override {
        return count > 0;
    }

private:
    std::uint32_t next_ = 1, vao_ = 0, vertex_ = 0, index_ = 0;
    std::array<std::int64_t, 32> bytes_{};
};

class SavedPlayDispatch : public testing::Test {
protected:
    using Producer = FarWorldAuthorityProducer;
    using Save = Persistence::WorldSaveService;
    std::filesystem::path root = TestSupport::CreateUniqueTempDirectory("saved_play_dispatch_");
    JobSystem jobs;
    FarWorldRenderer renderer;
    Producer producer{{512,
                       4ULL * 1024 * 1024,
                       300'000,
                       std::make_shared<FarVolumeMemoryPool>(64ULL * 1024 * 1024)}};
    std::unique_ptr<Systems::SHIELD_WorldSystem> world;
    Producer::FrozenPolicy policy;
    FarSessionHandoffTile tile;
    void SetUp() override {
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.caves_enabled = false;
        params.base_amplitude = params.peaks_amplitude = 0;
        params.height_offset = -8;
        params.continental_spline = {{-1, 0}, {1, 0}};
        world = std::make_unique<Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
        policy = {WorldFarVolumeIdentity(
                      *world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4),
                  FarCaveMode::BandLimited,
                  FarVolumeNumericProfile::RawFloat,
                  4};
        ASSERT_TRUE(Save{}.save_world(WorldStreamingState{}, root));
        const Save::AuthorityReadLimits reads{
            512, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072};
        std::string error;
        ASSERT_TRUE(producer.admit_saved_source(root, reads, error)) << error;
        ASSERT_TRUE(producer.configure_capture(
            {reads, 4, {[](const void*) noexcept -> std::uint64_t { return 0; }, nullptr, 1}}));
        const std::array<Save::AuthorityWindow, 1> windows{{{-6, 518, -6, 518}}};
        ASSERT_TRUE(producer.request_capture(windows));
        producer.pump_capture();
        producer.pump_capture();
        ASSERT_TRUE(producer.capture_status().latest_ready);
        renderer.InstallBackend(std::make_unique<SessionDriver>());
        ASSERT_EQ(renderer.BindSession(
                      jobs, 1, policy.caves, policy.numeric, FarWorldDevelopmentLimits()),
                  FarWorldRefusal::None);
        tile.wanted = {{1, policy.pristine_identity, 0, {5, 0, 0}, 0, 1},
                       {{5, 0, 0}, FarVolumeCoverageSpan{-512, 128}},
                       64ULL * 1024 * 1024,
                       {},
                       {}};
        tile.capture = {{5, 0, 0}, {-512, 256}, policy.pristine_identity};
        ASSERT_TRUE(Stage());
    }
    bool Stage() {
        const FarFrozenCaptureLimits limits{1, 100'000, 20'000'000, 200'000, 64ULL * 1024 * 1024};
        return renderer.StageSessionTarget(
            producer, *world, {&tile, 1}, policy, limits, {200'000, 20'000'000, 240'000});
    }
    void TearDown() override {
        renderer.PrepareWorldSwap();
        renderer.Shutdown();
        jobs.shutdown();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
} // namespace

TEST_F(SavedPlayDispatch, QualifiedSessionDispatchesOneSchedulerAndSelectedSubmission) {
    LegacyRuntime legacy;
    unsigned ladder_schedules = 0, ladder_submissions = 0;
    const auto selection =
        ApplyLegacyFarWorldEnable(renderer.Selection(FarWorldPurpose::World), false);
    ASSERT_EQ(selection.requested, FarWorldMode::Ladder);
    ASSERT_EQ(selection.effective, FarWorldMode::Ladder);
    EXPECT_EQ(selection.refusal, FarWorldRefusal::None);
    renderer.BeginLiveOwnership(1);
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    AdvanceSelectedFarWorld(
        selection,
        [&] { legacy.Advance(); },
        [&] {
            ++ladder_schedules;
            EXPECT_TRUE(renderer.PrepareFrame(1, {0, 0, 0}));
        });
    SubmitSelectedFarWorld(
        selection,
        [&] { return legacy.Submit(); },
        [&](const auto&) {
            ++ladder_submissions;
            return renderer.Submit({0, 0, 0}, [](const auto&) { return true; });
        },
        {});
    EXPECT_EQ(ladder_schedules, 1u);
    EXPECT_EQ(ladder_submissions, 1u);
    EXPECT_EQ(legacy.schedules + legacy.writes + legacy.submissions + legacy.water_supplements, 0u);
    renderer.CaptureDiagnostic(selection, 1, {0, 0, 0}, kFarPlaneMeters, true, 0, 0);
    const auto diagnostic = FarWorldDiagnosticJson(renderer.Diagnostic());
    EXPECT_EQ(diagnostic["requested_mode"], "ladder");
    EXPECT_EQ(diagnostic["effective_mode"], "ladder");
    EXPECT_EQ(renderer.Diagnostic().authority_kind, FarWorldAuthorityKind::Session);
}

TEST_F(SavedPlayDispatch, SourceRefusalDispatchesOnlyLegacyAndReportsBothModes) {
    producer.invalidate_source();
    ASSERT_FALSE(Stage());
    const auto selection = renderer.Selection(FarWorldPurpose::World);
    EXPECT_EQ(selection.requested, FarWorldMode::Ladder);
    EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
    EXPECT_EQ(selection.refusal, FarWorldRefusal::ImmutableAuthorityUnavailable);
    LegacyRuntime legacy;
    unsigned ladder_calls = 0;
    AdvanceSelectedFarWorld(selection, [&] { legacy.Advance(); }, [&] { ++ladder_calls; });
    SubmitSelectedFarWorld(
        selection,
        [&] { return legacy.Submit(); },
        [&](const auto&) {
            ++ladder_calls;
            return FarWorldSubmitStats{};
        },
        {});
    EXPECT_EQ(legacy.schedules, 1u);
    EXPECT_EQ(legacy.submissions, 1u);
    EXPECT_EQ(ladder_calls, 0u);
    renderer.CaptureDiagnostic(
        selection, 2, {0, 0, 0}, FarWorldPlane(selection.effective), true, 1, 3);
    const auto diagnostic = FarWorldDiagnosticJson(renderer.Diagnostic());
    EXPECT_EQ(diagnostic["requested_mode"], "ladder");
    EXPECT_EQ(diagnostic["effective_mode"], "legacy");
    EXPECT_EQ(renderer.Diagnostic().selection.refusal,
              FarWorldRefusal::ImmutableAuthorityUnavailable);
}

TEST_F(SavedPlayDispatch, CompletedSourceStagesWhileNewerCapturesQueueAndBecomeActive) {
    const std::array<Save::AuthorityWindow, 1> windows{{{-6, 518, -6, 518}}};
    const auto serial = producer.capture()->serial;
    ASSERT_TRUE(producer.request_capture(windows));
    ASSERT_TRUE(producer.capture_status().queued);
    ASSERT_FALSE(producer.capture_status().latest_ready);
    ASSERT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, serial);
    producer.pump_capture();
    ASSERT_TRUE(producer.capture_status().active);
    ASSERT_TRUE(producer.request_capture(windows));
    ASSERT_TRUE(producer.capture_status().queued);
    ASSERT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, serial);
    producer.pump_capture();
    ASSERT_GT(producer.capture()->serial, serial);
    ASSERT_TRUE(producer.capture_status().queued);
    ASSERT_TRUE(Stage());
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, producer.capture()->serial);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
}

TEST_F(SavedPlayDispatch, FreshCompletedSerialStagesDuringActiveCaptureButFencedReadsRefuse) {
    const auto target = renderer.SessionDiagnostic()->state.target_serial;
    const std::array<Save::AuthorityWindow, 1> windows{{{-6, 518, -6, 518}}};
    ASSERT_TRUE(producer.request_capture(windows));
    producer.pump_capture();
    producer.pump_capture();
    const auto fresh = producer.capture()->serial;
    ASSERT_GT(fresh, target);
    ASSERT_TRUE(producer.request_capture(windows));
    producer.pump_capture();
    ASSERT_TRUE(producer.capture_status().active);
    producer.fence_source_reads(true);
    EXPECT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, target);
    producer.fence_source_reads(false);
    ASSERT_TRUE(Stage());
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, fresh);
}

TEST(FarWorldLegacyIsolation, SavedWorldActivationDecisionsBindPumpRefuseAndReport) {
    // Exhaust the context-free owner-frame decisions. SavedPipelinePreparation
    // below also drives attachment and discovery through the production pipeline.
    std::uint32_t retry_frames = 0;
    bool retryable = false;
    EXPECT_TRUE(RetryFarWorldSessionAttachment(retry_frames, retryable, false, false));
    FarWorldSessionAttachmentFailed(retry_frames, retryable, true);
    for (std::uint32_t frame = 0; frame < 30; ++frame) {
        EXPECT_FALSE(RetryFarWorldSessionAttachment(retry_frames, retryable, true, false));
    }
    EXPECT_TRUE(RetryFarWorldSessionAttachment(retry_frames, retryable, true, false));
    EXPECT_FALSE(RetryFarWorldSessionAttachment(retry_frames, retryable, true, true));
    EXPECT_TRUE(FarWorldSessionCaptureAdmission(false, true));
    EXPECT_FALSE(FarWorldSessionCaptureAdmission(false, false));
    EXPECT_TRUE(FarWorldSessionCaptureAge(false, false, true));
    const auto selected =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    const auto refused = FarWorldSessionSelection(selected,
                                                  FarWorldPurpose::World,
                                                  FarWorldRefusal::None,
                                                  FarWorldRefusal::AdmissionFailed,
                                                  FarWorldRefusal::None);
    EXPECT_EQ(refused.requested, FarWorldMode::Ladder);
    EXPECT_EQ(refused.effective, FarWorldMode::Legacy);
    EXPECT_EQ(refused.refusal, FarWorldRefusal::AdmissionFailed);
    const auto recovered = FarWorldSessionSelection(selected,
                                                    FarWorldPurpose::World,
                                                    FarWorldRefusal::None,
                                                    FarWorldRefusal::None,
                                                    FarWorldRefusal::None);
    EXPECT_EQ(recovered.effective, FarWorldMode::Ladder);
    EXPECT_EQ(recovered.refusal, FarWorldRefusal::None);
}

TEST(FarWorldLegacyIsolation, AttachmentRetryAndCaptureDecisionFailureMatrix) {
    for (const bool attached : {false, true}) {
        for (const bool enabled : {false, true}) {
            for (const bool recoverable : {false, true}) {
                std::uint32_t retry_frames = 0;
                bool retryable = false;
                FarWorldSessionAttachmentFailed(retry_frames, retryable, recoverable);
                EXPECT_EQ(retry_frames, recoverable ? 30u : 0u);
                for (std::uint32_t frame = 0; frame < 30; ++frame) {
                    if (recoverable) {
                        EXPECT_FALSE(RetryFarWorldSessionAttachment(
                            retry_frames, retryable, attached, enabled));
                    }
                }
                EXPECT_EQ(
                    RetryFarWorldSessionAttachment(retry_frames, retryable, attached, enabled),
                    !attached || (!enabled && recoverable));
            }
        }
    }
    for (const bool discovering : {false, true}) {
        for (const bool admitted : {false, true}) {
            for (const bool active : {false, true}) {
                EXPECT_EQ(FarWorldSessionCaptureAdmission(discovering, admitted),
                          !discovering && admitted);
                EXPECT_EQ(FarWorldSessionCaptureAge(discovering, admitted, active),
                          active && (discovering || !admitted));
            }
        }
    }
    const auto selected =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    for (const auto purpose :
         {FarWorldPurpose::World, FarWorldPurpose::Preview, FarWorldPurpose::OffscreenPreview}) {
        const auto refusal = FarWorldSessionSelection(selected,
                                                      purpose,
                                                      FarWorldRefusal::InvalidCavePolicy,
                                                      FarWorldRefusal::AdmissionFailed,
                                                      FarWorldRefusal::WaterUnavailable);
        EXPECT_EQ(refusal.refusal,
                  purpose == FarWorldPurpose::World ? FarWorldRefusal::InvalidCavePolicy
                                                    : selected.refusal);
    }
    std::uint32_t retry_frames = 0;
    bool retryable = false;
    FarWorldSessionAttachmentFailed(retry_frames, retryable, true);
    for (std::uint32_t frame = 0; frame < 30; ++frame) {
        EXPECT_FALSE(RetryFarWorldSessionAttachment(retry_frames, retryable, false, false));
    }
    EXPECT_TRUE(RetryFarWorldSessionAttachment(retry_frames, retryable, false, false));
}

TEST_F(SavedPlayDispatch, QueuedWindowChangeRefusesAnUncoveredCompletedSourceTransactionally) {
    const auto serial = renderer.SessionDiagnostic()->state.target_serial;
    const std::array<Save::AuthorityWindow, 1> windows{{{506, 1030, -6, 518}}};
    ASSERT_TRUE(producer.request_capture(windows));
    tile.wanted.identity.key.tx = 1;
    tile.wanted.intent.key.x = 1;
    tile.capture.key.x = 1;
    ASSERT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::SpanRejected);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, serial);
}

TEST_F(SavedPlayDispatch, PausedCompletedSourceRefusesUntilReadsResume) {
    const auto serial = renderer.SessionDiagnostic()->state.target_serial;
    producer.fence_source_reads(true);
    ASSERT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, serial);
    producer.fence_source_reads(false);
    ASSERT_TRUE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
}

TEST_F(SavedPlayDispatch, SessionBindingKeepsLegacyPreviewAndOffReplayIsolated) {
    const auto on = renderer.Selection(FarWorldPurpose::World);
    for (const auto purpose : {FarWorldPurpose::Preview, FarWorldPurpose::OffscreenPreview}) {
        const auto selected = renderer.Selection(purpose);
        EXPECT_EQ(selected.requested, FarWorldMode::Ladder);
        EXPECT_EQ(selected.effective, FarWorldMode::Legacy);
        LegacyRuntime legacy;
        unsigned ladder_calls = 0;
        AdvanceSelectedFarWorld(selected, [&] { legacy.Advance(); }, [&] { ++ladder_calls; });
        EXPECT_EQ(ladder_calls, 0u);
        EXPECT_EQ(legacy.schedules, purpose == FarWorldPurpose::Preview ? 1u : 0u);
    }
    const auto off = FarWorldSubmissionOverride(on, false);
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
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, producer.capture()->serial);
    renderer.SetRequested(FarWorldMode::Legacy);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).requested, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.SessionDiagnostic(), nullptr);
}

namespace {
class SavedPipelinePreparation : public testing::Test {
protected:
    luminumbra_shieldrt::HiddenGlContext context{"saved pipeline preparation"};
    std::filesystem::path root = TestSupport::CreateUniqueTempDirectory("saved_pipeline_");
    JobSystem jobs;
    world::GameSession session;
    std::unique_ptr<RenderPipeline> pipeline;
    Camera camera;
    void SetUp() override {
        ASSERT_TRUE(context.ready()) << context.error();
        jobs.startup(1);
        session.SetRootPath(std::string(LUMINUMBRA_SOURCE_ROOT) +
                            static_cast<char>(std::filesystem::path::preferred_separator));
        session.SetSaveRootPath(root);
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(false);
        session.SetAetherStateEnabled(false);
        ASSERT_TRUE(session.CreateWorld("Saved pipeline", "1337", "default"));
        ASSERT_TRUE(session.SaveWorldState());
        pipeline = std::make_unique<RenderPipeline>();
        pipeline->enable_static_model_content(false);
        ASSERT_TRUE(pipeline->startup(32, 32, std::filesystem::path(LUMINUMBRA_SOURCE_ROOT)));
        pipeline->set_far_world_mode(FarWorldMode::Ladder);
        camera.Position = {0, 0, 0};
    }
    void Pump() {
        pipeline->pump_far_world_session(camera, FarWorldPurpose::World);
    }
    void TearDown() override {
        // Drain while session, worker and GL context still belong to this fixture.
        pipeline.reset();
        session.SetJobSystem(nullptr);
        jobs.shutdown();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
} // namespace

TEST_F(SavedPipelinePreparation, TemporaryAttachmentFailureRetriesAfterBoundedBackoff) {
    // Source admission fails at the real saved baseline, then recovers on a later
    // owner frame without replacing the world or calling bind again.
    const auto save = session.GetWorldSaveDir();
    const auto held = root / "held-save";
    std::filesystem::rename(save, held);
    pipeline->bind_far_world_session(session, jobs);
    ASSERT_TRUE(pipeline->far_world_session_preparation_stats().attached);
    EXPECT_FALSE(pipeline->far_world_session_preparation_stats().source_enabled);
    EXPECT_EQ(pipeline->far_world_reporting_state().selection.refusal,
              FarWorldRefusal::AdmissionFailed);
    std::filesystem::rename(held, save);
    for (unsigned frame = 0; frame < 30; ++frame) {
        pipeline->resume_far_world_session(session, jobs);
        EXPECT_FALSE(pipeline->far_world_session_preparation_stats().source_enabled);
    }
    pipeline->resume_far_world_session(session, jobs);
    ASSERT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
    ASSERT_NE(session.GetFarWorldAuthorityProducer(), nullptr);
    Pump();
    EXPECT_TRUE(pipeline->far_world_session_preparation_stats().capture_admitted);
    EXPECT_EQ(pipeline->far_world_reporting_state().selection.requested, FarWorldMode::Ladder);
    EXPECT_FALSE(pipeline->far_world_reporting_state().coverage_settled);
}

TEST_F(SavedPipelinePreparation, DiscoveryCountsActualWorkAndResumesAcrossFrames) {
    pipeline->bind_far_world_session(session, jobs);
    ASSERT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
    auto* producer = session.GetFarWorldAuthorityProducer();
    ASSERT_NE(producer, nullptr);
    const Persistence::WorldSaveService::AuthorityReadLimits reads{32'768,
                                                                   512,
                                                                   65'536,
                                                                   131'072,
                                                                   8ULL * 1024 * 1024,
                                                                   32ULL * 1024 * 1024,
                                                                   16ULL * 1024 * 1024,
                                                                   64ULL * 1024 * 1024,
                                                                   4ULL * 1024 * 1024};
    // Keep the production frame deadline while eliminating elapsed-time variance.
    ASSERT_TRUE(producer->configure_capture(
        {reads, 120, {[](const void*) noexcept -> std::uint64_t { return 0; }, nullptr, 1}}));
    std::size_t columns = 0;
    for (unsigned frame = 0; frame < 8; ++frame) {
        Pump();
        const auto stats = pipeline->far_world_session_preparation_stats();
        EXPECT_LE(stats.discovery_work, 65'536u);
        EXPECT_LE(stats.discovery_columns, stats.discovery_work);
        columns += stats.discovery_columns;
    }
    EXPECT_GT(columns, 0u);
    EXPECT_EQ(pipeline->far_world_reporting_state().selection.requested, FarWorldMode::Ladder);
    EXPECT_FALSE(pipeline->far_world_reporting_state().coverage_settled);
    for (const auto purpose : {FarWorldPurpose::Preview, FarWorldPurpose::OffscreenPreview}) {
        const auto before = session.GetFarWorldAuthorityProducer()->capture_status();
        pipeline->pump_far_world_session(camera, purpose);
        EXPECT_EQ(session.GetFarWorldAuthorityProducer()->capture_status().serving_serial,
                  before.serving_serial);
    }
    pipeline->prepare_world_swap();
    EXPECT_FALSE(pipeline->far_world_session_preparation_stats().attached);
    pipeline->resume_far_world_session(session, jobs);
    EXPECT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
}

TEST_F(SavedPipelinePreparation, CaptureCooldownRetainsDirtyWorkWithoutAdmittingCopies) {
    pipeline->bind_far_world_session(session, jobs);
    ASSERT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
    auto* producer = session.GetFarWorldAuthorityProducer();
    ASSERT_NE(producer, nullptr);
    // Expire an active capture at its next frame, using public finite limits.
    // Then dirty publications must remain queued without another source copy.
    const Persistence::WorldSaveService::AuthorityReadLimits reads{32'768,
                                                                   512,
                                                                   65'536,
                                                                   131'072,
                                                                   8ULL * 1024 * 1024,
                                                                   32ULL * 1024 * 1024,
                                                                   16ULL * 1024 * 1024,
                                                                   64ULL * 1024 * 1024,
                                                                   4ULL * 1024 * 1024};
    ASSERT_TRUE(producer->configure_capture(
        {reads, 1, {[](const void*) noexcept -> std::uint64_t { return 0; }, nullptr, 1}}));
    Pump();
    ASSERT_TRUE(producer->capture_status().active);
    Pump();
    ASSERT_EQ(producer->capture_status().error, FarWorldAuthorityProducer::Error::DeadlineExceeded);
    ASSERT_EQ(pipeline->far_world_session_preparation_stats().capture_retry_frames, 30u);
    const auto before = producer->counters();
    for (unsigned frame = 0; frame < 8; ++frame) {
        producer->changes()->notify({0, 0, 0}, FarWorldAuthorityChange::Terrain);
        producer->changes()->notify({0, 0, 0}, FarWorldAuthorityChange::Water);
        Pump();
        const auto stats = pipeline->far_world_session_preparation_stats();
        EXPECT_FALSE(stats.capture_admitted);
        EXPECT_EQ(stats.discovery_work, 0u);
        EXPECT_FALSE(producer->capture_status().active);
        EXPECT_EQ(producer->counters().tick_items, before.tick_items);
        EXPECT_EQ(producer->counters().copied_records, before.copied_records);
    }
    EXPECT_TRUE(producer->needs_capture_boundary());
    for (unsigned frame = 0;
         frame < 21 && pipeline->far_world_session_preparation_stats().capture_retry_frames > 1;
         ++frame) {
        Pump();
    }
    ASSERT_EQ(pipeline->far_world_session_preparation_stats().capture_retry_frames, 1u);
    Pump();
    EXPECT_TRUE(pipeline->far_world_session_preparation_stats().capture_admitted);
    EXPECT_TRUE(producer->capture_status().active);
    EXPECT_EQ(pipeline->far_world_reporting_state().selection.effective, FarWorldMode::Legacy);
    EXPECT_FALSE(pipeline->far_world_reporting_state().coverage_settled);
}

TEST_F(SavedPipelinePreparation, PolicyChangesDrainAndReattachAndInvalidCameraRefuses) {
    pipeline->bind_far_world_session(session, jobs);
    ASSERT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
    camera.Position.y = std::numeric_limits<float>::infinity();
    Pump();
    EXPECT_EQ(pipeline->far_world_reporting_state().selection.refusal,
              FarWorldRefusal::InvalidRequests);
    EXPECT_FALSE(pipeline->far_world_session_preparation_stats().capture_admitted);
    pipeline->set_far_world_mode(FarWorldMode::Legacy);
    EXPECT_FALSE(pipeline->far_world_session_preparation_stats().attached);
    pipeline->resume_far_world_session(session, jobs);
    EXPECT_FALSE(pipeline->far_world_session_preparation_stats().attached);
    pipeline->set_far_world_mode(FarWorldMode::Ladder);
    pipeline->resume_far_world_session(session, jobs);
    ASSERT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
    pipeline->set_far_world_cave_policy(std::nullopt);
    EXPECT_FALSE(pipeline->far_world_session_preparation_stats().attached);
    pipeline->resume_far_world_session(session, jobs);
    EXPECT_EQ(pipeline->far_world_reporting_state().selection.refusal,
              FarWorldRefusal::InvalidCavePolicy);
    EXPECT_FALSE(pipeline->far_world_session_preparation_stats().source_enabled);
    pipeline->set_far_world_cave_policy(FarCaveMode::BoxFiltered);
    pipeline->resume_far_world_session(session, jobs);
    EXPECT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
}

TEST_F(SavedPipelinePreparation, WrongThreadLifecycleRefusalsPreserveSourceAndAttachment) {
    pipeline->bind_far_world_session(session, jobs);
    ASSERT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
    auto* producer = session.GetFarWorldAuthorityProducer();
    ASSERT_NE(producer, nullptr);
    ASSERT_FALSE(producer->capture_status().paused);
    const auto refused = [&](const auto& operation) {
        std::thread worker([&] { EXPECT_THROW(operation(), std::logic_error); });
        worker.join();
        EXPECT_FALSE(producer->capture_status().paused);
        EXPECT_TRUE(pipeline->far_world_session_preparation_stats().attached);
        EXPECT_TRUE(pipeline->far_world_session_preparation_stats().source_enabled);
    };
    refused([&] { pipeline->set_far_world_mode(FarWorldMode::Legacy); });
    refused([&] { pipeline->set_far_world_cave_policy(std::nullopt); });
    refused([&] { pipeline->wait_for_world_readers(); });
    refused([&] { pipeline->prepare_world_swap(); });
    refused([&] { pipeline->shutdown(); });
}

TEST(FarWorldLegacyIsolation, DiscoveryAdmissionHonorsWorkAndExistingTimeBudget) {
    for (const std::size_t work : {0u, 1u, 65'536u}) {
        for (const std::size_t used : {0u, 1u, 65'535u}) {
            for (const double elapsed : {0.0,
                                         0.5,
                                         1.0,
                                         5.0,
                                         std::numeric_limits<double>::infinity(),
                                         std::numeric_limits<double>::quiet_NaN()}) {
                EXPECT_EQ(FarWorldSessionDiscoveryAdmission(work, used, elapsed),
                          work != 0 && (used == 0 || elapsed < kWaterUploadLimits.milliseconds));
            }
        }
    }
}

TEST(FarWorldLegacyIsolation, SpanHistoryAdmitsMoreThan512DistinctRangeKeys) {
    auto history = std::make_unique<FarWorldSessionSpanHistory>();
    auto snapshot = std::make_unique<FarWorldHandoffDiagnosticSnapshot>();
    snapshot->captured = true;
    const FarVolumeSpanLedgerLimits limits{-8192, 8192, 2048};
    // Model the complete post-retirement consumer observation: serving and target
    // stay protected while distinct ranges cross four lifetime ledger capacities.
    for (std::int64_t range = 0; range < 2048; ++range) {
        const FarTileKey serving{5, range - 1, 0}, target{5, range, 0};
        snapshot->tile_count = range == 0 ? 0u : 1u;
        snapshot->tiles[0].key = serving;
        const std::array wanted{target};
        ASSERT_TRUE(history->Reclaim(snapshot.get(), wanted));
        if (range != 0) {
            ASSERT_EQ(history->count, 1u);
            EXPECT_EQ(history->keys[0], serving);
            EXPECT_EQ(history->spans[0].generation(), 1u);
        }
        ASSERT_TRUE(history->Extend(target, {-256, 128}, limits));
        EXPECT_LE(history->count, 2u);
    }
    snapshot->tile_count = 0;
    ASSERT_TRUE(history->Reclaim(snapshot.get(), {}));
    EXPECT_EQ(history->count, 0u);
    EXPECT_TRUE(history->Extend({5, 0, 0}, {-16, 16}, limits)); // Fresh narrower return.
}

TEST(FarWorldLegacyIsolation, PartialOrMissingHistoryObservationPreservesAllConstraints) {
    auto history = std::make_unique<FarWorldSessionSpanHistory>();
    auto snapshot = std::make_unique<FarWorldHandoffDiagnosticSnapshot>();
    const FarVolumeSpanLedgerLimits limits{-8192, 8192, 2048};
    const FarTileKey key{5, 0, 0};
    EXPECT_TRUE(history->Reclaim(nullptr, {})); // Empty bootstrap does not prune.
    ASSERT_TRUE(history->Extend(key, {-256, 128}, limits));
    for (unsigned state = 0; state < 4; ++state) {
        snapshot->captured = state != 0;
        snapshot->complete = state != 1;
        snapshot->tile_count = state == 2 ? 513u : 0u;
        EXPECT_FALSE(history->Reclaim(state == 3 ? nullptr : snapshot.get(), {}));
        EXPECT_EQ(history->count, 1u);
        EXPECT_EQ(history->spans[0].generation(), 1u);
        const auto intent = history->spans[0].Intent();
        if (!intent.has_value()) {
            FAIL() << "Missing span intent";
            return;
        }
        if (!intent->extra_span.has_value()) {
            FAIL() << "Missing span extrema";
            return;
        }
        EXPECT_EQ(intent->extra_span->min_y, -256);
        EXPECT_EQ(intent->extra_span->max_y, 128);
    }
    EXPECT_FALSE(history->Extend(key, {128, -128}, limits));
    EXPECT_FALSE(history->Extend({0, 0, 0}, {-16, 16}, limits));
    EXPECT_EQ(history->count, 1u);
    snapshot->captured = snapshot->complete = true;
    snapshot->tile_count = 1;
    snapshot->tiles[0].key = key;
    ASSERT_TRUE(history->Reclaim(snapshot.get(), {}));
    EXPECT_EQ(history->count, 1u); // Target/serving evidence survives empty requests.
    snapshot->tile_count = 0;
    ASSERT_TRUE(history->Reclaim(snapshot.get(), {}));
    EXPECT_EQ(history->count, 0u);
}

TEST(FarWorldLegacyIsolation, HistoryCapacityAndMalformedRequestsRefuseWithoutMutation) {
    auto history = std::make_unique<FarWorldSessionSpanHistory>();
    auto snapshot = std::make_unique<FarWorldHandoffDiagnosticSnapshot>();
    const FarVolumeSpanLedgerLimits limits{-8192, 8192, 2048};
    for (std::int64_t key = 0; key < 512; ++key) {
        ASSERT_TRUE(history->Extend({5, key, 0}, {-256, 128}, limits));
    }
    EXPECT_FALSE(history->Extend({5, 512, 0}, {-256, 128}, limits));
    EXPECT_EQ(history->count, 512u);
    EXPECT_TRUE(history->Extend({5, 0, 0}, {-256, 128}, limits));
    EXPECT_EQ(history->spans[0].generation(), 1u);
    snapshot->captured = true;
    const std::array<FarTileKey, 513> oversized{};
    EXPECT_FALSE(history->Reclaim(snapshot.get(), oversized));
    EXPECT_EQ(history->count, 512u);
    history->count = 513;
    EXPECT_FALSE(history->Reclaim(snapshot.get(), {}));
    EXPECT_FALSE(history->Extend({5, 0, 0}, {-256, 128}, limits));
    history->count = 512;
    EXPECT_EQ(history->keys[0], (FarTileKey{5, 0, 0}));
    EXPECT_EQ(history->spans[0].generation(), 1u);
}
