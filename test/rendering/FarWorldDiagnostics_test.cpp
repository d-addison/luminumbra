#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra_client/app/RenderBenchmarkReport.h"
#include "luminumbra_client/core/IsolationConfig.h"
#include "luminumbra_client/rendering/FarWorldDiagnostics.h"
#include "luminumbra_common/core/JobSystem.h"

#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;
static_assert(std::is_trivially_copyable_v<FarWorldDiagnosticSnapshot>);

TEST(FarWorldDiagnostics, ReportsOnlyModeReasonAndSubmissionTransitions) {
    FarWorldReportingState state;
    EXPECT_TRUE(FarWorldReportChanged(std::nullopt, state));
    std::optional<FarWorldReportingState> previous = state;
    EXPECT_FALSE(FarWorldReportChanged(previous, state));
    state.coverage_settled = true;
    state.selection.advance = false;
    EXPECT_FALSE(FarWorldReportChanged(previous, state));
    state.selection.requested = FarWorldMode::Ladder;
    EXPECT_TRUE(FarWorldReportChanged(previous, state));
    previous = state;
    state.selection.effective = FarWorldMode::Ladder;
    EXPECT_TRUE(FarWorldReportChanged(previous, state));
    previous = state;
    state.selection.refusal = FarWorldRefusal::ImmutableAuthorityUnavailable;
    EXPECT_TRUE(FarWorldReportChanged(previous, state));
    previous = state;
    state.selection.refusal = FarWorldRefusal::BackendUnavailable;
    EXPECT_TRUE(FarWorldReportChanged(previous, state));
    previous = state;
    state.selection.submit = false;
    EXPECT_TRUE(state.suppressed());
    EXPECT_TRUE(FarWorldReportChanged(previous, state));
    previous = state;
    state.selection.submit = true;
    EXPECT_TRUE(FarWorldReportChanged(previous, state));
    for (const auto refusal : {FarWorldSubmissionRefusal::LiveOwnershipIncomplete,
                               FarWorldSubmissionRefusal::LogicalStoreAdmission,
                               FarWorldSubmissionRefusal::BackendUnavailable,
                               FarWorldSubmissionRefusal::DrawRejected,
                               FarWorldSubmissionRefusal::IsolationSuppressed,
                               FarWorldSubmissionRefusal::None}) {
        previous = state;
        state.submission_refusal = refusal;
        EXPECT_TRUE(FarWorldReportChanged(previous, state));
        EXPECT_EQ(state.suppressed(), refusal != FarWorldSubmissionRefusal::None);
        previous = state;
        EXPECT_FALSE(FarWorldReportChanged(previous, state));
    }
}

TEST(FarWorldDiagnostics, IsolationReportsSubmissionSuppressionForBothModes) {
    namespace SH = Luminumbra::Client::ScenarioHarness;
    for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
        FarWorldDiagnosticSnapshot snapshot;
        snapshot.selection = {mode, mode, FarWorldRefusal::None, true, true};
        for (const bool captured : {false, true}) {
            snapshot.captured = captured;
            const auto previous = FarWorldReportState(snapshot);
            EXPECT_FALSE(previous.suppressed());
            for (const char* layers : {"foliage", "farfield", "water,foliage"}) {
                const auto isolation = SH::ParseIsolationConfig(layers, "");
                const auto state =
                    FarWorldReportState(snapshot, isolation.renders(SH::IsolationLayer::Terrain));
                EXPECT_TRUE(state.selection.submit);
                EXPECT_TRUE(state.suppressed());
                EXPECT_EQ(state.submission_refusal, FarWorldSubmissionRefusal::IsolationSuppressed);
                EXPECT_STREQ(FarWorldSubmissionRefusalName(state.submission_refusal),
                             "IsolationSuppressed");
                EXPECT_TRUE(FarWorldReportChanged(previous, state));
                EXPECT_TRUE(FarWorldReportChanged(state, previous));
            }
            const auto isolation = SH::ParseIsolationConfig("terrain,foliage", "void");
            const auto restored =
                FarWorldReportState(snapshot, isolation.renders(SH::IsolationLayer::Terrain));
            EXPECT_FALSE(restored.suppressed());
            EXPECT_FALSE(FarWorldReportChanged(previous, restored));
        }
    }
}

TEST(FarWorldDiagnostics, BenchmarkFrameCountStrictlyParsesBeforeApplyingBounds) {
    using namespace Luminumbra::Client::Measurement;
    const auto parse = [](std::vector<std::string> args) {
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        return benchmark_frame_count(static_cast<int>(argv.size()), argv.data());
    };
    EXPECT_EQ(parse({"client"}), 120);
    for (const auto count : {1, 120, 18000}) {
        EXPECT_EQ(parse({"client", "--render-benchmark-frames", std::to_string(count)}), count);
        EXPECT_EQ(parse({"client", "--render-benchmark-frames=" + std::to_string(count)}), count);
    }
    EXPECT_THROW(parse({"client", "--render-benchmark-frames"}), std::invalid_argument);
    for (const char* value : {"0",
                              "-1",
                              "18001",
                              "999999999999999999999999",
                              "4junk",
                              "4.0",
                              "+4",
                              " 4",
                              "4 ",
                              "",
                              "--no-audio"}) {
        EXPECT_THROW(parse({"client", "--render-benchmark-frames", value}), std::invalid_argument);
        EXPECT_THROW(parse({"client", std::string("--render-benchmark-frames=") + value}),
                     std::invalid_argument);
    }
}

TEST(FarWorldDiagnostics, OutstandingBatchesPreventSettlingWithZeroDeferredWork) {
    using namespace Luminumbra::Client::Measurement;
    SampleRing ring(1);
    auto& far = ring.append(2).far_world;
    far.coverage_settled = true;
    ASSERT_TRUE(far.settled());
    for (const bool generation : {true, false}) {
        far.generation_outstanding = generation;
        far.meshing_outstanding = !generation;
        EXPECT_EQ(far.generation_pending, 0u);
        EXPECT_EQ(far.meshing_pending, 0u);
        EXPECT_EQ(far.terrain_uploads_pending, 0u);
        EXPECT_FALSE(far.settled());
        nlohmann::json report;
        add_far_world_reporting(report, ring);
        EXPECT_EQ(report["far_world"]["summary"]["unsettled_frames"], 1);
        const auto& streaming = report["far_world"]["frames"][0]["streaming"];
        EXPECT_EQ(streaming["generation_outstanding"], generation);
        EXPECT_EQ(streaming["meshing_outstanding"], !generation);
    }
    far.generation_outstanding = false;
    far.meshing_outstanding = false;
    EXPECT_TRUE(far.settled());
    nlohmann::json report;
    add_far_world_reporting(report, ring);
    EXPECT_EQ(report["far_world"]["summary"]["unsettled_frames"], 0);
}

TEST(FarWorldDiagnostics, StreamingCachesNewlyDispatchedBatchesBeforeReturning) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    Luminumbra::Systems::SHIELD_WorldSystem world(
        &jobs, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    world.debug_set_streaming_radius_cap(1);
    // A fixed simulation tick keeps dispatched work pending publication even
    // when the worker has completed, without depending on thread timing.
    world.begin_tick(0);
    entt::registry registry;
    const Luminumbra::Vec3 anchor(0.0f, world.GetTerrainHeightAt(0.0f, 0.0f) + 2.0f, 0.0f);
    for (int frame = 0; frame < 4; ++frame)
        world.update(registry, anchor, nullptr);
    const auto generation = world.get_last_streaming_budget_stats();
    ASSERT_GT(generation.scheduled_generation, 0u);
    EXPECT_TRUE(generation.generation_job_active);
    world.wait_for_streaming_jobs();
    world.update(registry, anchor, nullptr);
    const auto meshing = world.get_last_streaming_budget_stats();
    ASSERT_GT(meshing.scheduled_meshing, 0u);
    EXPECT_TRUE(meshing.meshing_job_active);
    world.wait_for_streaming_jobs();
}

TEST(FarWorldDiagnostics, BenchmarkSerializesEveryMeasuredFarStateAndSummary) {
    using namespace Luminumbra::Client::Measurement;
    SampleRing ring(3);
    auto& first = ring.append(4).far_world;
    first.requested_mode = "ladder";
    first.effective_mode = "legacy";
    first.refusal = "ImmutableAuthorityUnavailable";
    first.submission_refusal = "None";
    first.gns_compiled = true;
    first.active_regions = true;
    first.water_cells_per_side = 16;
    first.grass_density_scale = 1.6f;
    first.content_identity = std::numeric_limits<std::uint64_t>::max();
    ring.append(5).far_world = first;
    auto& second = ring.at(5).far_world;
    second.effective_mode = "ladder";
    second.refusal = "None";
    second.submission_refusal = "LiveOwnershipIncomplete";
    second.suppressed = true;
    ring.append(6).far_world = second;
    auto& third = ring.at(6).far_world;
    third.submission_refusal = "None";
    third.suppressed = false;
    third.coverage_settled = true;
    third.grass_density_scale.reset();
    nlohmann::json report;
    add_far_world_reporting(report, ring);
    const auto json = nlohmann::json::parse(report.dump()).at("far_world");
    const auto& rows = json.at("frames");
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0]["frame"], 4);
    EXPECT_EQ(rows[2]["frame"], 6);
    EXPECT_EQ(rows[0]["requested_mode"], "ladder");
    EXPECT_EQ(rows[0]["effective_mode"], "legacy");
    EXPECT_EQ(rows[0]["refusal"], "ImmutableAuthorityUnavailable");
    EXPECT_EQ(rows[1]["submission_refusal"], "LiveOwnershipIncomplete");
    EXPECT_EQ(rows[1]["suppressed"], true);
    EXPECT_EQ(rows[2]["coverage_settled"], true);
    EXPECT_EQ(rows[0]["content_identity"], "18446744073709551615");
    EXPECT_EQ(rows[0]["settings"]["gns_compiled"], true);
    EXPECT_EQ(rows[0]["settings"]["active_regions"], true);
    EXPECT_EQ(rows[0]["settings"]["water_cells_per_side"], 16);
    EXPECT_FLOAT_EQ(rows[0]["settings"]["grass_density_scale"].get<float>(), 1.6f);
    EXPECT_TRUE(rows[2]["settings"]["grass_density_scale"].is_null());
    EXPECT_EQ(rows[2]["streaming"]["generation_pending"], 0);
    EXPECT_EQ(rows[2]["streaming"]["meshing_pending"], 0);
    EXPECT_EQ(rows[2]["streaming"]["terrain_uploads_pending"], 0);
    EXPECT_EQ(json["summary"]["requested_mode"]["ladder"], 3);
    EXPECT_EQ(json["summary"]["effective_mode"]["legacy"], 1);
    EXPECT_EQ(json["summary"]["effective_mode"]["ladder"], 2);
    EXPECT_EQ(json["summary"]["refusal"]["ImmutableAuthorityUnavailable"], 1);
    EXPECT_EQ(json["summary"]["submission_refusal"]["LiveOwnershipIncomplete"], 1);
    EXPECT_EQ(json["summary"]["suppressed_frames"], 1);
    EXPECT_EQ(json["summary"]["unsettled_frames"], 2);
    for (int control = 0; control < 3; ++control) {
        auto& pending = ring.at(6).far_world;
        pending.generation_pending = control == 0 ? 1u : 0u;
        pending.meshing_pending = control == 1 ? 1u : 0u;
        pending.terrain_uploads_pending = control == 2 ? 1u : 0u;
        EXPECT_FALSE(pending.settled());
        add_far_world_reporting(report, ring);
        EXPECT_EQ(report["far_world"]["summary"]["unsettled_frames"], 3);
    }
}

struct Station {
    FarWorldDiagnosticSnapshot snapshot;
    std::vector<FarTileKey> expected;
    Station() {
        EXPECT_EQ(ComputeFarWantedSet(-512, 1024, expected), FarWantedError::None);
        snapshot.selection = {
            FarWorldMode::Ladder, FarWorldMode::Ladder, FarWorldRefusal::None, true, true};
        snapshot.world_epoch = 7;
        snapshot.content_identity = 42;
        snapshot.bake_generation = 5;
        snapshot.requested_cave_policy = FarCaveMode::BandLimited;
        snapshot.bound_cave_policy = FarCaveMode::BandLimited;
        snapshot.frame = 8;
        snapshot.far_plane = kFarPlaneMeters;
        snapshot.camera = {-512, 12, 1024};
        snapshot.captured = true;
        snapshot.advanced = true;
        snapshot.ladder_water_available = true;
        snapshot.water_capability = FarWorldWaterCapability::ExplicitlyDry;
        for (const auto& key : expected) {
            auto& tile = snapshot.tiles[snapshot.tile_count++];
            tile.desired = {7, 42, 0, key, 5, 2};
            tile.built = tile.desired;
            EXPECT_TRUE(ToFarVolumeTileKey(key, tile.intent.key));
            tile.intent.extra_span = FarVolumeCoverageSpan{-256, 80};
            tile.request.span = {-320, 128};
            tile.coverage = {FarTileState::Empty, true, true};
            tile.eligibility = FarVolumeEligibility::Settled;
            tile.has_pair = true;
            tile.water_complete = tile.water_known_dry = true;
            auto& tier = snapshot.tiers[key.tier - 1].resident;
            ++tier.wanted;
            ++tier.settled;
            ++tier.empty_pairs;
            ++tier.covering;
            ++tier.water_known_dry_tiles;
        }
    }
    bool Ready(double seconds = 1) const {
        return FarWorldStationReady(snapshot, expected, seconds);
    }
};

TEST(FarWorldDiagnostics, EmptyCoverageDoesNotRequireGpuStoresOrSubmission) {
    const Station station;
    ASSERT_TRUE(station.Ready(90));
    EXPECT_EQ(station.snapshot.draws, 0u);
    EXPECT_FALSE(station.snapshot.submission_occurred);
    EXPECT_EQ(station.snapshot.tiers[0].resident.memory.bytes_gpu, 0u);
    EXPECT_FALSE(station.Ready(90.001));
    EXPECT_FALSE(station.Ready(-1));
    EXPECT_FALSE(station.Ready(std::numeric_limits<double>::quiet_NaN()));
}

TEST(FarWorldDiagnostics, ReportingRequiresCompleteCurrentSettledCoverageInEveryTier) {
    const Station station;
    EXPECT_TRUE(FarWorldReportState(station.snapshot).coverage_settled);
    for (int control = 0; control < 16; ++control) {
        auto snapshot = station.snapshot;
        if (control == 0)
            snapshot.captured = false;
        if (control == 1)
            snapshot.complete = false;
        if (control == 2)
            snapshot.advanced = false;
        if (control == 3)
            snapshot.selection.effective = FarWorldMode::Legacy;
        if (control == 4)
            snapshot.upload.horizon_refused = true;
        if (control == 5)
            snapshot.ladder_water_available = false;
        if (control == 6)
            --snapshot.tiers.back().resident.settled;
        if (control == 7)
            --snapshot.tiers.back().resident.covering;
        if (control == 8)
            ++snapshot.tiers.back().resident.pending;
        if (control == 9)
            ++snapshot.tiers.back().resident.building;
        if (control == 10)
            ++snapshot.tiers.back().resident.ready;
        if (control == 11)
            ++snapshot.tiers.back().draw_refusals;
        if (control == 12)
            ++snapshot.tiers.back().resident.request_rejected;
        if (control == 13)
            ++snapshot.tiers.back().resident.budget_evicted;
        if (control == 14)
            --snapshot.tiers.back().resident.water_known_dry_tiles;
        if (control == 15)
            snapshot.tiers.back() = {};
        EXPECT_FALSE(FarWorldReportState(snapshot).coverage_settled) << control;
    }
    EXPECT_FALSE(FarWorldReportState(FarWorldDiagnosticSnapshot{}).coverage_settled);
}

TEST(FarWorldDiagnostics, WetFixtureRequiresCombinedWaterAndActualSubmittedIndices) {
    Station station;
    station.snapshot.water_capability = FarWorldWaterCapability::Wet;
    EXPECT_FALSE(station.Ready());
    auto& tile = station.snapshot.tiles[0];
    tile.coverage.state = FarTileState::Uploaded;
    tile.water_known_dry = false;
    tile.water_indices = 6;
    auto& tier = station.snapshot.tiers[tile.desired.key.tier - 1];
    --tier.resident.empty_pairs;
    ++tier.resident.uploaded_pairs;
    --tier.resident.water_known_dry_tiles;
    ++tier.resident.water_wet_tiles;
    tier.resident.retained_water_indices = 6;
    tier.draws = station.snapshot.draws = 1;
    tier.indices = station.snapshot.indices = 6;
    station.snapshot.submission_occurred = true;
    EXPECT_FALSE(station.Ready());
    tier.resident.submitted_water_indices = 6;
    ASSERT_TRUE(station.Ready());
    for (int control = 0; control < 4; ++control) {
        auto invalid = station;
        if (control == 0)
            invalid.snapshot.tiles[0].water_complete = false;
        if (control == 1)
            invalid.snapshot.water_capability = FarWorldWaterCapability::ExplicitlyDry;
        if (control == 2)
            invalid.snapshot.tiles[0].coverage.state = FarTileState::Empty;
        if (control == 3)
            invalid.snapshot.ladder_water_available = false;
        EXPECT_FALSE(invalid.Ready());
    }
    const auto on = FarWorldDiagnosticJson(station.snapshot);
    EXPECT_EQ(on["tiers"][0]["submitted_water_indices"], 6);
    const auto off = FarWorldSubmissionOverride(station.snapshot.selection, false);
    EXPECT_FALSE(off.submit);
    EXPECT_EQ(FarWorldDiagnosticJson(station.snapshot), on);
}

TEST(FarWorldDiagnostics, MissingInvalidOrMismatchedCavePoliciesRefuseStation) {
    Station station;
    station.snapshot.requested_cave_policy.reset();
    EXPECT_EQ(CheckFarWorldStation(station.snapshot, station.expected, 1),
              FarWorldStationRefusal::InvalidBinding);
    station.snapshot.requested_cave_policy = FarCaveMode::BoxFiltered;
    EXPECT_EQ(CheckFarWorldStation(station.snapshot, station.expected, 1),
              FarWorldStationRefusal::InvalidBinding);
    station.snapshot.bound_cave_policy.reset();
    EXPECT_EQ(CheckFarWorldStation(station.snapshot, station.expected, 1),
              FarWorldStationRefusal::InvalidBinding);
    station.snapshot.requested_cave_policy = static_cast<FarCaveMode>(2);
    station.snapshot.bound_cave_policy = static_cast<FarCaveMode>(2);
    EXPECT_EQ(CheckFarWorldStation(station.snapshot, station.expected, 1),
              FarWorldStationRefusal::InvalidBinding);
    station.snapshot.requested_cave_policy = FarCaveMode::BoxFiltered;
    station.snapshot.bound_cave_policy = FarCaveMode::BoxFiltered;
    EXPECT_EQ(CheckFarWorldStation(station.snapshot, station.expected, 1),
              FarWorldStationRefusal::StaleBinding);
    for (std::size_t i = 0; i < station.snapshot.tile_count; ++i) {
        auto& tile = station.snapshot.tiles[i];
        tile.desired.cave_policy = static_cast<std::uint8_t>(FarCaveMode::BoxFiltered);
        tile.built = tile.desired;
    }
    EXPECT_TRUE(station.Ready());
}

TEST(FarWorldDiagnostics, SerializerDistinguishesRequestedBoundAndAbsentCavePolicies) {
    FarWorldDiagnosticSnapshot snapshot{};
    const auto absent = FarWorldDiagnosticJson(snapshot);
    EXPECT_TRUE(absent["requested_cave_policy"].is_null());
    EXPECT_TRUE(absent["bound_cave_policy"].is_null());
    snapshot.requested_cave_policy = FarCaveMode::BoxFiltered;
    const auto unbound = FarWorldDiagnosticJson(snapshot);
    EXPECT_EQ(unbound["requested_cave_policy"], 1);
    EXPECT_TRUE(unbound["bound_cave_policy"].is_null());
    snapshot.bound_cave_policy = FarCaveMode::BandLimited;
    const auto bound = FarWorldDiagnosticJson(snapshot);
    EXPECT_EQ(bound["requested_cave_policy"], 1);
    EXPECT_EQ(bound["bound_cave_policy"], 0);
    snapshot.selection = FarWorldSubmissionOverride(snapshot.selection, false);
    const auto off = FarWorldDiagnosticJson(snapshot);
    EXPECT_EQ(off["requested_cave_policy"], bound["requested_cave_policy"]);
    EXPECT_EQ(off["bound_cave_policy"], bound["bound_cave_policy"]);
}

TEST(FarWorldDiagnostics, MixedCoverageRequiresActualSuccessfulSubmission) {
    Station station;
    auto& tile = station.snapshot.tiles[0];
    tile.coverage.state = FarTileState::Uploaded;
    tile.terrain_indices = 6;
    auto& tier = station.snapshot.tiers[tile.desired.key.tier - 1];
    --tier.resident.empty_pairs;
    ++tier.resident.uploaded_pairs;
    tier.resident.retained_gpu_bytes = 144;
    EXPECT_FALSE(station.Ready());
    station.snapshot.draws = 1;
    station.snapshot.indices = 6;
    station.snapshot.submission_occurred = true;
    tier.draws = 1;
    tier.indices = 6;
    EXPECT_TRUE(station.Ready());
    tier.draw_refusals = 1;
    EXPECT_FALSE(station.Ready());
}

TEST(FarWorldDiagnostics, TooFewTriangleIndicesForDrawsRefusesStation) {
    Station station;
    const auto tier_number = station.snapshot.tiles[0].desired.key.tier;
    std::size_t uploaded = 0;
    for (auto& tile : station.snapshot.tiles) {
        if (uploaded == 3)
            break;
        if (tile.has_pair && tile.desired.key.tier == tier_number) {
            tile.coverage.state = FarTileState::Uploaded;
            tile.terrain_indices = 3;
            ++uploaded;
        }
    }
    ASSERT_EQ(uploaded, 3u);
    auto& tier = station.snapshot.tiers[tier_number - 1];
    tier.resident.empty_pairs -= 3;
    tier.resident.uploaded_pairs = tier.draws = station.snapshot.draws = 3;
    tier.indices = station.snapshot.indices = 6;
    station.snapshot.submission_occurred = true;
    EXPECT_EQ(CheckFarWorldStation(station.snapshot, station.expected, 1),
              FarWorldStationRefusal::TierCounters);
    tier.indices = station.snapshot.indices = 9;
    EXPECT_TRUE(station.Ready());
}

TEST(FarWorldDiagnostics, WrongModeStaleGenerationSpanAndIncompleteStationRefuse) {
    using R = FarWorldStationRefusal;
    const std::array reasons{R::WrongMode,
                             R::RendererRefused,
                             R::StaleGeneration,
                             R::StaleBinding,
                             R::SpanRefused,
                             R::SpanRefused,
                             R::ExpectedSetMismatch,
                             R::RejectedTiles,
                             R::HorizonRefused,
                             R::IncompleteSnapshot,
                             R::WrongPlane};
    for (int fault = 0; fault < 11; ++fault) {
        Station station;
        auto& s = station.snapshot;
        switch (fault) {
            case 0:
                s.selection.effective = FarWorldMode::Legacy;
                break;
            case 1:
                s.selection.refusal = FarWorldRefusal::ShaderProgramUnavailable;
                break;
            case 2:
                --s.tiles[0].built.span_generation;
                break;
            case 3:
                ++s.tiles[0].desired.world_epoch;
                s.tiles[0].built = s.tiles[0].desired;
                break;
            case 4:
                s.tiles[0].request.span.min_y = -128;
                break;
            case 5:
                s.tiles[0].coverage.span_ok = false;
                break;
            case 6:
                --s.tile_count;
                break;
            case 7:
                s.tiers[0].resident.request_rejected = 1;
                break;
            case 8:
                s.upload.horizon_refused = true;
                break;
            case 9:
                s.complete = false;
                break;
            case 10:
                s.far_plane = kLegacyFarPlaneMeters;
                break;
        }
        EXPECT_FALSE(station.Ready()) << fault;
        EXPECT_EQ(CheckFarWorldStation(s, station.expected, 1), reasons[fault]) << fault;
    }
}

TEST(FarWorldDiagnostics, ExpectedSetDisagreementOrOmittedTierCannotSettle) {
    Station station;
    station.expected[0].tx += 100;
    EXPECT_FALSE(station.Ready());
    Station omitted;
    omitted.snapshot.tiers[0].resident.covering = 0;
    EXPECT_FALSE(omitted.Ready());
}

TEST(FarWorldDiagnostics, PairedOverridePreservesModePlaneAndOnSnapshot) {
    Station station;
    const auto on = FarWorldDiagnosticJson(station.snapshot);
    const auto off = FarWorldSubmissionOverride(station.snapshot.selection, false);
    EXPECT_EQ(off.effective, FarWorldMode::Ladder);
    EXPECT_EQ(off.requested, FarWorldMode::Ladder);
    EXPECT_EQ(off.refusal, FarWorldRefusal::None);
    EXPECT_EQ(FarWorldPlane(off.effective), station.snapshot.far_plane);
    EXPECT_FALSE(off.advance);
    EXPECT_FALSE(off.submit);
    // A fake dispatch uses the same value snapshot and spends no allowance.
    bool advanced = false, submitted = false;
    AdvanceSelectedFarWorld(off, [&] { advanced = true; }, [&] { advanced = true; });
    const auto result = SubmitSelectedFarWorld(
        off,
        [&] {
            submitted = true;
            return FarWorldSubmitStats{};
        },
        [&](const auto&) {
            submitted = true;
            return FarWorldSubmitStats{};
        },
        [](const auto&) { return true; });
    EXPECT_FALSE(advanced);
    EXPECT_FALSE(submitted);
    EXPECT_EQ(result.draws, 0u);
    EXPECT_EQ(on, FarWorldDiagnosticJson(station.snapshot));
}

TEST(FarWorldDiagnostics, SerializerContainsBindingPoliciesCoverageCountersAndLogicalStoreLabels) {
    const Station station;
    const auto json = FarWorldDiagnosticJson(station.snapshot);
    for (const char* field : {"requested_mode",
                              "effective_mode",
                              "refusal",
                              "suppression",
                              "selected",
                              "world_epoch",
                              "frame_id",
                              "content_identity",
                              "bake_generation",
                              "far_plane_m",
                              "requested_cave_policy",
                              "bound_cave_policy",
                              "numeric_policy",
                              "advancement_occurred",
                              "submission_occurred",
                              "camera",
                              "complete",
                              "captured",
                              "tiers",
                              "tiles",
                              "frame_upload",
                              "ladder_water_available"})
        EXPECT_TRUE(json.contains(field)) << field;
    ASSERT_EQ(json["tiers"].size(), 5u);
    for (const auto& tier : json["tiers"])
        for (const char* field : {"wanted",
                                  "pending",
                                  "building",
                                  "ready",
                                  "uploaded_pairs",
                                  "empty_pairs",
                                  "current_covering",
                                  "rejected",
                                  "budget_evicted",
                                  "evicted_range_total",
                                  "evicted_budget_total",
                                  "stale_rebuilds_total",
                                  "pool_cpu_bytes",
                                  "pool_gpu_logical_store_bytes",
                                  "retained_ready_cpu_bytes",
                                  "retained_gpu_logical_store_bytes",
                                  "draws",
                                  "indices",
                                  "draw_refusals",
                                  "water_known_dry_tiles",
                                  "water_wet_tiles",
                                  "retained_water_indices",
                                  "submitted_water_indices"})
            EXPECT_TRUE(tier.contains(field)) << field;
    EXPECT_EQ(json["tiles"].size(), station.expected.size());
    EXPECT_EQ(json["tiles"][0]["built"], json["tiles"][0]["desired"]);
    EXPECT_EQ(json["tiles"][0]["desired"]["cave_policy"], json["bound_cave_policy"]);
    EXPECT_EQ(json["gpu_bytes_kind"], "logical_stores");
    EXPECT_EQ(json["memory_sampling"], "asynchronous_diagnostic_not_atomic_admission");
    EXPECT_TRUE(json["ladder_water_available"].get<bool>());
    EXPECT_EQ(json["water_capability"], 1);
    const auto bytes = json.dump();
    EXPECT_THROW(
        {
            const auto parsed = nlohmann::json::parse(bytes.substr(0, bytes.size() - 1));
            (void)parsed;
        },
        nlohmann::json::parse_error);
    EXPECT_THROW(
        {
            const auto parsed = nlohmann::json::parse("{broken");
            (void)parsed;
        },
        nlohmann::json::parse_error);
}

TEST(FarWorldDiagnostics, CavePolicyRefusalsKeepTheirNamesInSerializedDiagnostics) {
    Station station;
    for (const auto refusal :
         {FarWorldRefusal::InvalidCavePolicy, FarWorldRefusal::CavePolicyMismatch}) {
        station.snapshot.selection.refusal = refusal;
        const auto expected = refusal == FarWorldRefusal::InvalidCavePolicy ? "InvalidCavePolicy"
                                                                            : "CavePolicyMismatch";
        EXPECT_STREQ(FarWorldRefusalName(refusal), expected);
        EXPECT_EQ(FarWorldDiagnosticJson(station.snapshot)["refusal"], expected);
        EXPECT_FALSE(station.Ready());
    }
}
} // namespace
