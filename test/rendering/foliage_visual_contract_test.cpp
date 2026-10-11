#include "core/FoliageVisualReport.h"

#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace Luminumbra::Client::ScenarioHarness;

FoliagePhaseEvidence Sample(std::uint32_t phase) {
    FoliagePhaseEvidence s;
    s.sampled = true;
    s.from_gpu_readback = true;
    s.phase = phase;
    s.capture_frame = phase == 1 ? 100 : 200;
    s.build_frame = phase == 1 ? 80 : 150;
    s.available_frame = phase == 1 ? 82 : 153;
    s.build_generation = phase == 1 ? 7 : 9;
    s.instance_generation = s.build_generation;
    s.camera = {8.0f, 12.4f, -8.0f};
    s.terrain_height_m = 10.0f;
    s.yaw_degrees = 35.0f;
    s.pitch_degrees = -18.0f;
    s.fov_degrees = 60.0f;
    s.time_of_day = 0.04f;
    s.fade_start_m = 48.0f;
    s.fade_end_m = 92.0f;
    s.density_scale = 1.6f;
    s.wind_input = {phase == 1 ? 0.0f : 6.0f, 0.0f};
    s.maximum_instance_wind = s.wind_input[0];
    s.width = 3840;
    s.height = 1600;
    s.screenshot = phase == 1 ? "screenshots/calm.ppm" : "screenshots/windy.ppm";
    return s;
}

FoliageInstancingResult Result() {
    FoliageInstancingResult r;
    r.calm = Sample(1);
    r.windy = Sample(2);
    r.instances_total = 262144;
    r.instances_within_ring = 262144;
    r.measured_density = 262144.0 / 873813.0;
    r.biome_density = 0.3;
    r.fade_start_m = 48;
    r.fade_end_m = 92;
    r.live_ring_radius_m = 92;
    r.foliage_draws = 1;
    r.foliage_instances_drawn = 262144;
    r.gpu_timers_supported = true;
    r.foliage_gpu_ms = 0.238;
    return r;
}

TEST(FoliageVisualContract, OnlyFunctionalScenarioOwnsFinalUpdate) {
    EXPECT_FALSE(FoliageScenarioOwnsFrame(false, false)); // normal or other scenario
    EXPECT_FALSE(FoliageScenarioOwnsFrame(false, true));
    EXPECT_FALSE(
        FoliageScenarioOwnsFrame(true, true)); // --play-paths keeps ordinary readback policy
    EXPECT_TRUE(FoliageScenarioOwnsFrame(true, false));
}

TEST(FoliageVisualContract, MissingCalmCannotMasqueradeAsZeroWind) {
    auto r = Result();
    ASSERT_TRUE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r.calm.sampled = false;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    const auto report = BuildFoliageInstancingReport(r, 0);
    EXPECT_FALSE(report["functional_control"]["passed"].get<bool>());
    EXPECT_EQ(report["phases"]["calm"]["status"], "unavailable");
    EXPECT_FALSE(report["phases"]["calm"].contains("maximum_instance_wind_magnitude"));
}

TEST(FoliageVisualContract, RejectsReadbackFromPriorBuildAndInvalidFrameOrder) {
    EXPECT_TRUE(FoliageInstanceMatches(7, 7, 80, 82, 100));
    EXPECT_FALSE(FoliageInstanceMatches(7, 6, 80, 82, 100));
    EXPECT_FALSE(FoliageInstanceMatches(0, 0, 80, 82, 100));
    EXPECT_FALSE(FoliageInstanceMatches(7, 7, 0, 82, 100));
    EXPECT_FALSE(FoliageInstanceMatches(7, 7, 83, 82, 100));
    EXPECT_FALSE(FoliageInstanceMatches(7, 7, 80, 101, 100));
    auto r = Result();
    r.windy.instance_generation = r.calm.build_generation;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
}

TEST(FoliageVisualContract, SameBuildOrReversedPhasesDoNotPass) {
    auto r = Result();
    r.windy.build_generation = r.calm.build_generation;
    r.windy.instance_generation = r.calm.build_generation;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r = Result();
    r.windy.phase = 1;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r = Result();
    r.windy.build_frame = r.calm.capture_frame;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
}

TEST(FoliageVisualContract, RejectsOldSkyCameraNightAndLiveWindOverrides) {
    auto r = Result();
    r.windy.pitch_degrees = 30.0f;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r = Result();
    r.windy.time_of_day = 0.5f;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r = Result();
    r.windy.camera[1] = 34.0f;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r = Result();
    r.calm.wind_input = {9.319f, 0.0f};
    r.calm.maximum_instance_wind = 9.319;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r = Result();
    r.windy.camera[0] += 1.0f;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
}

TEST(FoliageVisualContract, DensityMultiplierDoesNotReplaceEffectiveBaseProfile) {
    auto r = Result();
    r.calm.density_scale = r.windy.density_scale = 3.2f;
    EXPECT_TRUE(FoliageControlsMatch(r.calm, r.windy, 2.0f));
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r.calm.density_scale = r.windy.density_scale = 2.0f;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 2.0f));
    r = Result();
    r.windy.fade_start_m = 60.0f;
    r.windy.fade_end_m = 96.0f;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
}

TEST(FoliageVisualContract, PreservesHardFloorAndRejectsInvalidNumericEvidence) {
    EXPECT_FALSE(FoliageCoveragePasses(0, 0.3, 0.3, 0.6));
    EXPECT_FALSE(FoliageCoveragePasses(99999, 0.3, 0.3, 0.6));
    EXPECT_TRUE(FoliageCoveragePasses(100000, 0.3, 0.3, 0.6));
    EXPECT_FALSE(FoliageCoveragePasses(100000, 1.0, 0.3, 0.6));
    EXPECT_FALSE(FoliageCoveragePasses(100000, std::numeric_limits<double>::quiet_NaN(), 0.3, 0.6));
    auto r = Result();
    r.windy.maximum_instance_wind = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
    r = Result();
    r.windy.width = 0;
    EXPECT_FALSE(FoliageControlsMatch(r.calm, r.windy, 1.0f));
}

TEST(FoliageVisualContract, PassingControlsDoNotCloseMissingQualifications) {
    auto r = Result();
    r.calm.instance_hash = r.windy.instance_hash = 123; // Same hash is not a second reconstruction.
    auto report = BuildFoliageInstancingReport(r, 0);
    EXPECT_TRUE(report["functional_control"]["passed"].get<bool>());
    EXPECT_FALSE(report["passed"].get<bool>());
    EXPECT_EQ(report["qualification"]["status"], "incomplete");
    EXPECT_FALSE(report["qualification"]["visual_approved"].get<bool>());
    EXPECT_EQ(report["determinism"]["status"], "unevaluated");
    EXPECT_FALSE(report["determinism"]["passed"].get<bool>());
    EXPECT_FALSE(report["rendered_motion"]["passed"].get<bool>());
    EXPECT_FALSE(report.contains("wind_sway"));
    EXPECT_EQ(report["gpu_timer"]["budget_ms"], 0.6);
    EXPECT_FALSE(report["gpu_timer"]["within_budget"].get<bool>());
    EXPECT_FALSE(report["gpu_timer"]["qualified"].get<bool>());
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 1)["functional_control"]["passed"].get<bool>());
    r.instances_within_ring = 99999;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["functional_control"]["passed"].get<bool>());
}

TEST(FoliageVisualContract, CpuFallbackIsNotReportedAsGpuReadback) {
    auto r = Result();
    r.calm.from_gpu_readback = false;
    r.windy.from_gpu_readback = false;
    const auto report = BuildFoliageInstancingReport(r, 0);
    EXPECT_TRUE(report["functional_control"]["passed"].get<bool>());
    const auto& phase = report["phases"]["windy"];
    EXPECT_EQ(phase["instance_source"], "cpu_scatter");
    EXPECT_TRUE(phase["readback_generation"].is_null());
    EXPECT_TRUE(phase["readback_consumed_frame"].is_null());
    EXPECT_EQ(phase["instance_generation"], 9);
    EXPECT_TRUE(phase["instance_matches_drawn_build"].get<bool>());
}

TEST(FoliageVisualContract, MissingAndOverBudgetTimersRemainTruthful) {
    auto r = Result();
    r.foliage_gpu_ms = 0.7;
    auto report = BuildFoliageInstancingReport(r, 0);
    EXPECT_FALSE(report["gpu_timer"]["qualified"].get<bool>());
    EXPECT_EQ(report["gpu_timer"]["status"], "unavailable");
    r.gpu_timers_supported = false;
    report = BuildFoliageInstancingReport(r, 0);
    EXPECT_EQ(report["gpu_timer"]["status"], "unavailable");
    EXPECT_TRUE(report["gpu_timer"]["mean_ms"].is_null());
    EXPECT_FALSE(report["gpu_timer"]["within_budget"].get<bool>());
    EXPECT_FALSE(report["gpu_timer"]["qualified"].get<bool>());
    r.gpu_timers_supported = true;
    r.foliage_gpu_ms = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(BuildFoliageInstancingReport(r, 0)["gpu_timer"]["status"], "unavailable");
}

FoliageInstancingResult QualifiedResult() {
    auto r = Result();
    r.calm.capture_frame = 140;
    r.windy.capture_frame = 240;
    r.calm.instance_hash = 123;
    r.windy.instance_hash = 456;
    r.calm.shader_time_seconds = r.windy.shader_time_seconds = 1;
    auto& e = r.evidence;
    e.frozen_inputs = true;
    e.gl_vendor = "fixture vendor";
    e.gl_renderer = "fixture renderer";
    e.gl_version = "fixture version";
    e.rebuild = {99, 6, 7, 70, 80, 123, 123, 262144, 262144, true, true, true};
    for (std::size_t phase = 0; phase < 2; ++phase) {
        auto& v = e.vertices[phase];
        const auto& p = phase == 0 ? r.calm : r.windy;
        v.source_frame = p.capture_frame;
        v.available_frame = p.capture_frame + 1;
        v.generation = p.build_generation;
        v.instance_hash = p.instance_hash;
        v.phase = phase + 1;
        v.shader_time = 1;
        const auto projection = FoliageExpectedViewProjection(p);
        for (std::size_t k = 0; k < 16; ++k)
            v.view_projection[k] = static_cast<float>(projection[k]);
        for (std::size_t blade = 0; blade < 64; ++blade) {
            const bool sways = blade != 63;
            v.blade_heights.push_back(.2f);
            v.sways.push_back(sways);
            for (std::size_t j = 0; j < 12; ++j) {
                const bool tip = j % 6 == 2 || j % 6 == 4 || j % 6 == 5;
                const float x = phase && tip && sways ? .05f : 0;
                const float y = tip ? .2f : 0;
                std::array<float, 8> vertex{20 + x, 10 + y, 0, tip ? 1.f : 0.f, 0, 0, 0, 0};
                for (std::size_t row = 0; row < 4; ++row) {
                    vertex[4 + row] = v.view_projection[12 + row];
                    for (std::size_t col = 0; col < 3; ++col)
                        vertex[4 + row] += v.view_projection[col * 4 + row] * vertex[col];
                }
                v.vertices.push_back(vertex);
            }
        }
        for (std::size_t j = 0; j < 32; ++j)
            e.gpu.push_back({phase * 32 + j + 1,
                             p.available_frame + j,
                             p.build_generation,
                             p.instance_generation,
                             static_cast<std::uint32_t>(phase + 1),
                             262144,
                             .2,
                             1});
    }
    return r;
}

TEST(FoliageQualification, CompleteIndependentEvidencePassesWithoutVisualApproval) {
    const auto report = BuildFoliageInstancingReport(QualifiedResult(), 0);
    EXPECT_TRUE(report["passed"].get<bool>());
    EXPECT_EQ(report["qualification"]["status"], "complete");
    EXPECT_FALSE(report["qualification"]["visual_approved"].get<bool>());
    EXPECT_EQ(report["gpu_timer"]["samples"].size(), 64);
}
TEST(FoliageQualification, CachedOrUnclearedSnapshotCannotClaimReconstruction) {
    auto r = QualifiedResult();
    r.evidence.rebuild.output_cleared = false;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.rebuild.second_generation = r.evidence.rebuild.first_generation;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.rebuild.byte_equal = false;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
}
TEST(FoliageQualification, ShiftedCameraCannotRelabelCapturedMatrix) {
    auto r = QualifiedResult();
    r.calm.camera[0] += 30;
    r.windy.camera[0] += 30;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
}
TEST(FoliageQualification, LaterStillCannotReplaceActualVertexFrame) {
    auto r = QualifiedResult();
    ++r.evidence.vertices[1].source_frame;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    --r.evidence.vertices[1].generation;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
}
TEST(FoliageQualification, RequiresGeometricResponsePinnedRootsAndNonSwayingControls) {
    auto r = QualifiedResult();
    r.evidence.vertices[1].vertices = r.evidence.vertices[0].vertices;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.vertices[1].vertices[0][0] += .01f;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.vertices[1].vertices[63 * 12 + 2][0] += .01f;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
}
TEST(FoliageQualification, RefusesUnboundedOrNonFiniteVertexOutputs) {
    auto r = QualifiedResult();
    r.evidence.vertices[1].vertices[2][0] = 1;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.vertices[1].vertices[2][0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.vertices[1].view_projection[0] = 2;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
}
TEST(FoliageQualification, RefusesDuplicateStaleAndInstrumentedGpuFrames) {
    auto r = QualifiedResult();
    r.evidence.gpu[1].source_frame = r.evidence.gpu[0].source_frame;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    --r.evidence.gpu[0].generation;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.gpu[31].source_frame = r.calm.capture_frame;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
}
TEST(FoliageQualification, MissingSamplesAndSingleOverBudgetFrameRefuse) {
    auto r = QualifiedResult();
    r.evidence.gpu.pop_back();
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.gpu[0].milliseconds = -1;
    EXPECT_FALSE(BuildFoliageInstancingReport(r, 0)["passed"].get<bool>());
    r = QualifiedResult();
    r.evidence.gpu[0].milliseconds = .601;
    const auto report = BuildFoliageInstancingReport(r, 0);
    EXPECT_FALSE(report["passed"].get<bool>());
    EXPECT_EQ(report["qualification"]["status"],
              "complete"); // Evidence is complete; budget failed.
}
} // namespace
