#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include "entt/entity/fwd.hpp"
#include "luminumbra/core/Types.h"
#include "luminumbra_client/app/RenderBenchmarkReport.h"
#include "luminumbra_client/app/RenderMeasurement.h"
#include "luminumbra_client/core/IsolationConfig.h"
#include "luminumbra_client/rendering/FarVolumeResidency.h"
#include "luminumbra_client/rendering/FarWorldDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldHandoffDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldSelection.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "world/FarHorizon.h"
#include "world/FarVolume.h"
#include "world/FarVolumeRuntime.h"
#include "world/FarWantedSet.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;
static_assert(std::is_trivially_copyable_v<FarWorldDiagnosticSnapshot>);

TEST(RenderBenchmarkOverrides, WorkloadAssignmentsDoNotConsumeSuccessorsOrHideOverrides) {
    for (const char* option : {"--render-benchmark",
                               "--render-benchmark-schema",
                               "--render-benchmark-frames",
                               "--render-benchmark-warmup",
                               "--render-benchmark-screenshot",
                               "--capture-size",
                               "--load-world",
                               "--perf-profile",
                               "--traversal",
                               "--runtime-root",
                               "--save-root",
                               "--declare-render-overrides",
                               "--world-preset",
                               "--cam-pos",
                               "--cam-yaw",
                               "--cam-pitch",
                               "--window-mode"}) {
        for (const bool assigned : {false, true}) {
            for (const std::string value : {"4", "0", "", "a=b"}) {
                SCOPED_TRACE(::testing::Message()
                             << option << " value: " << value << " assigned: " << assigned);
                std::vector<std::string> arguments{"client", option};
                if (assigned)
                    arguments.back() += "=" + value;
                else
                    arguments.push_back(value);
                arguments.insert(arguments.end(),
                                 {"successor",
                                  "--render-scale=0.5",
                                  "tail",
                                  "--systems-config",
                                  "systems.json"});
                std::vector<char*> argv;
                argv.reserve(arguments.size());
                for (auto& argument : arguments)
                    argv.push_back(argument.data());
                const auto active = Luminumbra::Client::Measurement::render_overrides(
                    static_cast<int>(argv.size()), argv.data());
                nlohmann::json cli = nlohmann::json::object();
                for (const auto& item : active.items())
                    if (item.key().rfind("cli:", 0) == 0)
                        cli[item.key()] = item.value();
                const nlohmann::json expected{{"cli:successor", "true"},
                                              {"cli:--render-scale", "0.5"},
                                              {"cli:tail", "true"},
                                              {"cli:--systems-config", "systems.json"}};
                EXPECT_EQ(cli, expected);
            }
        }
        // A missing workload value must leave the next option visible, and a
        // terminal workload option must not read beyond argv.
        for (const bool terminal : {false, true}) {
            SCOPED_TRACE(::testing::Message() << option << " terminal: " << terminal);
            std::vector<std::string> arguments{"client", option};
            if (!terminal)
                arguments.push_back("--render-scale=0");
            std::vector<char*> argv;
            argv.reserve(arguments.size());
            for (auto& argument : arguments)
                argv.push_back(argument.data());
            const auto active = Luminumbra::Client::Measurement::render_overrides(
                static_cast<int>(argv.size()), argv.data());
            EXPECT_FALSE(active.contains(std::string("cli:") + option));
            if (!terminal)
                EXPECT_EQ(active.at("cli:--render-scale"), "0");
        }
    }
}

TEST(RenderBenchmarkOverrides, OverrideSyntaxPreservesValuesAndSuccessors) {
    for (const bool assigned : {false, true}) {
        for (const std::string value : {"0.5", "0", "", "a=b"}) {
            SCOPED_TRACE(::testing::Message() << "value: " << value << " assigned: " << assigned);
            std::vector<std::string> arguments{"client", "--render-scale"};
            if (assigned)
                arguments.back() += "=" + value;
            else
                arguments.push_back(value);
            arguments.insert(
                arguments.end(),
                {"tail", "--systems-config=systems.json", "--no-audio=false", "--terminal"});
            std::vector<char*> argv;
            argv.reserve(arguments.size());
            for (auto& argument : arguments)
                argv.push_back(argument.data());
            const auto active = Luminumbra::Client::Measurement::render_overrides(
                static_cast<int>(argv.size()), argv.data());
            nlohmann::json cli = nlohmann::json::object();
            for (const auto& item : active.items())
                if (item.key().rfind("cli:", 0) == 0)
                    cli[item.key()] = item.value();
            const nlohmann::json expected{{"cli:--render-scale", value},
                                          {"cli:tail", "true"},
                                          {"cli:--systems-config", "systems.json"},
                                          {"cli:--no-audio", "false"},
                                          {"cli:--terminal", "true"}};
            EXPECT_EQ(cli, expected);
        }
    }
}

class IncompleteBenchmarkBuffer : public std::streambuf {
    bool fail_flush_;

public:
    explicit IncompleteBenchmarkBuffer(bool fail_flush)
        : fail_flush_(fail_flush) {}
    std::streamsize xsputn(const char*, std::streamsize count) override {
        return fail_flush_ ? count : count - 1;
    }
    int sync() override {
        return fail_flush_ ? -1 : 0;
    }
};

TEST(RenderBenchmarkOutput, BothSchemasRejectPartialWritesAndFlushFailures) {
    using Luminumbra::Client::Measurement::write_render_benchmark;
    for (const char* schema : {"luminumbra.render_benchmark.v2",
                               "luminumbra.render_benchmark.v3",
                               "luminumbra.render_benchmark.v4"}) {
        const nlohmann::json report{{"schema", schema}, {"frames", 4}};
        std::ostringstream complete;
        EXPECT_TRUE(write_render_benchmark(complete, report));
        EXPECT_EQ(nlohmann::json::parse(complete.str()), report);
        for (const bool fail_flush : {false, true}) {
            IncompleteBenchmarkBuffer buffer(fail_flush);
            std::ostream incomplete(&buffer);
            EXPECT_FALSE(write_render_benchmark(incomplete, report));
        }
    }
}

TEST(RenderBenchmarkOutput, BothSchemasCheckFileOpenAndDirectoryCreation) {
    using Luminumbra::Client::Measurement::write_render_benchmark;
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("render-report-");
    for (const char* schema : {"luminumbra.render_benchmark.v2",
                               "luminumbra.render_benchmark.v3",
                               "luminumbra.render_benchmark.v4"}) {
        const bool binary = std::string_view(schema) != "luminumbra.render_benchmark.v2";
        const nlohmann::json report{{"schema", schema}};
        const auto output = directory / schema / "report.json";
        EXPECT_TRUE(write_render_benchmark(output, report, binary));
        {
            std::ifstream input(output, std::ios::binary);
            EXPECT_EQ(nlohmann::json::parse(input), report);
        }
        EXPECT_FALSE(write_render_benchmark(directory, report, binary));
        EXPECT_FALSE(write_render_benchmark(output / "child.json", report, binary));
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    EXPECT_FALSE(error);
}

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
    EXPECT_TRUE(FarWorldReportSuppressed(state));
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
        EXPECT_EQ(FarWorldReportSuppressed(state), refusal != FarWorldSubmissionRefusal::None);
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
            EXPECT_FALSE(FarWorldReportSuppressed(previous));
            for (const char* layers : {"foliage", "farfield", "water,foliage"}) {
                const auto isolation = SH::ParseIsolationConfig(layers, "");
                const auto state =
                    FarWorldReportState(snapshot, isolation.renders(SH::IsolationLayer::Terrain));
                EXPECT_TRUE(state.selection.submit);
                EXPECT_TRUE(FarWorldReportSuppressed(state));
                EXPECT_EQ(state.submission_refusal, FarWorldSubmissionRefusal::IsolationSuppressed);
                EXPECT_STREQ(FarWorldSubmissionRefusalName(state.submission_refusal),
                             "IsolationSuppressed");
                EXPECT_TRUE(FarWorldReportChanged(previous, state));
                EXPECT_TRUE(FarWorldReportChanged(state, previous));
            }
            const auto isolation = SH::ParseIsolationConfig("terrain,foliage", "void");
            const auto restored =
                FarWorldReportState(snapshot, isolation.renders(SH::IsolationLayer::Terrain));
            EXPECT_FALSE(FarWorldReportSuppressed(restored));
            EXPECT_FALSE(FarWorldReportChanged(previous, restored));
        }
    }
}

TEST(FarWorldDiagnostics, BenchmarkFrameCountStrictlyParsesBeforeApplyingBounds) {
    using namespace Luminumbra::Client::Measurement;
    const auto parse = [](std::vector<std::string> args) {
        std::vector<char*> argv;
        argv.reserve(args.size());
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
    ASSERT_TRUE(far_frame_settled(far));
    for (const bool generation : {true, false}) {
        far.generation_outstanding = generation;
        far.meshing_outstanding = !generation;
        EXPECT_EQ(far.generation_pending, 0u);
        EXPECT_EQ(far.meshing_pending, 0u);
        EXPECT_EQ(far.terrain_uploads_pending, 0u);
        EXPECT_FALSE(far_frame_settled(far));
        nlohmann::json report;
        add_far_world_reporting(report, ring);
        EXPECT_EQ(report["far_world"]["summary"]["unsettled_frames"], 1);
        const auto& streaming = report["far_world"]["frames"][0]["streaming"];
        EXPECT_EQ(streaming["generation_outstanding"], generation);
        EXPECT_EQ(streaming["meshing_outstanding"], !generation);
    }
    far.generation_outstanding = false;
    far.meshing_outstanding = false;
    EXPECT_TRUE(far_frame_settled(far));
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
        EXPECT_FALSE(far_frame_settled(pending));
        add_far_world_reporting(report, ring);
        EXPECT_EQ(report["far_world"]["summary"]["unsettled_frames"], 3);
    }
}

class Station {
public:
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
    FarWorldDiagnosticSnapshot& Snapshot() noexcept {
        return snapshot;
    }
    const FarWorldDiagnosticSnapshot& Snapshot() const noexcept {
        return snapshot;
    }
    std::vector<FarTileKey>& Expected() noexcept {
        return expected;
    }
    const std::vector<FarTileKey>& Expected() const noexcept {
        return expected;
    }

private:
    FarWorldDiagnosticSnapshot snapshot;
    std::vector<FarTileKey> expected;
};

TEST(FarWorldDiagnostics, EmptyCoverageDoesNotRequireGpuStoresOrSubmission) {
    const Station station;
    ASSERT_TRUE(station.Ready(90));
    EXPECT_EQ(station.Snapshot().draws, 0u);
    EXPECT_FALSE(station.Snapshot().submission_occurred);
    EXPECT_EQ(station.Snapshot().tiers[0].resident.memory.bytes_gpu, 0u);
    EXPECT_FALSE(station.Ready(90.001));
    EXPECT_FALSE(station.Ready(-1));
    EXPECT_FALSE(station.Ready(std::numeric_limits<double>::quiet_NaN()));
}

TEST(FarWorldDiagnostics, ReportingRequiresCompleteCurrentSettledCoverageInEveryTier) {
    const Station station;
    EXPECT_TRUE(FarWorldReportState(station.Snapshot()).coverage_settled);
    for (int control = 0; control < 16; ++control) {
        auto snapshot = station.Snapshot();
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
    station.Snapshot().water_capability = FarWorldWaterCapability::Wet;
    EXPECT_FALSE(station.Ready());
    auto& tile = station.Snapshot().tiles[0];
    tile.coverage.state = FarTileState::Uploaded;
    tile.water_known_dry = false;
    tile.water_indices = 6;
    auto& tier = station.Snapshot().tiers[tile.desired.key.tier - 1];
    --tier.resident.empty_pairs;
    ++tier.resident.uploaded_pairs;
    --tier.resident.water_known_dry_tiles;
    ++tier.resident.water_wet_tiles;
    tier.resident.retained_water_indices = 6;
    tier.draws = station.Snapshot().draws = 1;
    tier.indices = station.Snapshot().indices = 6;
    station.Snapshot().submission_occurred = true;
    EXPECT_FALSE(station.Ready());
    tier.resident.submitted_water_indices = 6;
    ASSERT_TRUE(station.Ready());
    for (int control = 0; control < 4; ++control) {
        auto invalid = station;
        if (control == 0)
            invalid.Snapshot().tiles[0].water_complete = false;
        if (control == 1)
            invalid.Snapshot().water_capability = FarWorldWaterCapability::ExplicitlyDry;
        if (control == 2)
            invalid.Snapshot().tiles[0].coverage.state = FarTileState::Empty;
        if (control == 3)
            invalid.Snapshot().ladder_water_available = false;
        EXPECT_FALSE(invalid.Ready());
    }
    const auto on = FarWorldDiagnosticJson(station.Snapshot());
    EXPECT_EQ(on["tiers"][0]["submitted_water_indices"], 6);
    const auto off = FarWorldSubmissionOverride(station.Snapshot().selection, false);
    EXPECT_FALSE(off.submit);
    EXPECT_EQ(FarWorldDiagnosticJson(station.Snapshot()), on);
}

TEST(FarWorldDiagnostics, MissingInvalidOrMismatchedCavePoliciesRefuseStation) {
    Station station;
    station.Snapshot().requested_cave_policy.reset();
    EXPECT_EQ(CheckFarWorldStation(station.Snapshot(), station.Expected(), 1),
              FarWorldStationRefusal::InvalidBinding);
    station.Snapshot().requested_cave_policy = FarCaveMode::BoxFiltered;
    EXPECT_EQ(CheckFarWorldStation(station.Snapshot(), station.Expected(), 1),
              FarWorldStationRefusal::InvalidBinding);
    station.Snapshot().bound_cave_policy.reset();
    EXPECT_EQ(CheckFarWorldStation(station.Snapshot(), station.Expected(), 1),
              FarWorldStationRefusal::InvalidBinding);
    station.Snapshot().requested_cave_policy = std::bit_cast<FarCaveMode>(std::uint8_t{2});
    station.Snapshot().bound_cave_policy = std::bit_cast<FarCaveMode>(std::uint8_t{2});
    EXPECT_EQ(CheckFarWorldStation(station.Snapshot(), station.Expected(), 1),
              FarWorldStationRefusal::InvalidBinding);
    station.Snapshot().requested_cave_policy = FarCaveMode::BoxFiltered;
    station.Snapshot().bound_cave_policy = FarCaveMode::BoxFiltered;
    EXPECT_EQ(CheckFarWorldStation(station.Snapshot(), station.Expected(), 1),
              FarWorldStationRefusal::StaleBinding);
    for (std::size_t i = 0; i < station.Snapshot().tile_count; ++i) {
        auto& tile = station.Snapshot().tiles[i];
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
    auto& tile = station.Snapshot().tiles[0];
    tile.coverage.state = FarTileState::Uploaded;
    tile.terrain_indices = 6;
    auto& tier = station.Snapshot().tiers[tile.desired.key.tier - 1];
    --tier.resident.empty_pairs;
    ++tier.resident.uploaded_pairs;
    tier.resident.retained_gpu_bytes = 144;
    EXPECT_FALSE(station.Ready());
    station.Snapshot().draws = 1;
    station.Snapshot().indices = 6;
    station.Snapshot().submission_occurred = true;
    tier.draws = 1;
    tier.indices = 6;
    EXPECT_TRUE(station.Ready());
    tier.draw_refusals = 1;
    EXPECT_FALSE(station.Ready());
}

TEST(FarWorldDiagnostics, TooFewTriangleIndicesForDrawsRefusesStation) {
    Station station;
    const auto tier_number = station.Snapshot().tiles[0].desired.key.tier;
    std::size_t uploaded = 0;
    for (auto& tile : station.Snapshot().tiles) {
        if (uploaded == 3)
            break;
        if (tile.has_pair && tile.desired.key.tier == tier_number) {
            tile.coverage.state = FarTileState::Uploaded;
            tile.terrain_indices = 3;
            ++uploaded;
        }
    }
    ASSERT_EQ(uploaded, 3u);
    auto& tier = station.Snapshot().tiers[tier_number - 1];
    tier.resident.empty_pairs -= 3;
    tier.resident.uploaded_pairs = tier.draws = station.Snapshot().draws = 3;
    tier.indices = station.Snapshot().indices = 6;
    station.Snapshot().submission_occurred = true;
    EXPECT_EQ(CheckFarWorldStation(station.Snapshot(), station.Expected(), 1),
              FarWorldStationRefusal::TierCounters);
    tier.indices = station.Snapshot().indices = 9;
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
        auto& s = station.Snapshot();
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
            default:
                FAIL() << "Unknown diagnostic fault: " << fault;
        }
        EXPECT_FALSE(station.Ready()) << fault;
        EXPECT_EQ(CheckFarWorldStation(s, station.Expected(), 1), reasons[fault]) << fault;
    }
}

TEST(FarWorldDiagnostics, ExpectedSetDisagreementOrOmittedTierCannotSettle) {
    Station station;
    station.Expected()[0].tx += 100;
    EXPECT_FALSE(station.Ready());
    Station omitted;
    omitted.Snapshot().tiers[0].resident.covering = 0;
    EXPECT_FALSE(omitted.Ready());
}

TEST(FarWorldDiagnostics, PairedOverridePreservesModePlaneAndOnSnapshot) {
    Station const station;
    const auto on = FarWorldDiagnosticJson(station.Snapshot());
    const auto off = FarWorldSubmissionOverride(station.Snapshot().selection, false);
    EXPECT_EQ(off.effective, FarWorldMode::Ladder);
    EXPECT_EQ(off.requested, FarWorldMode::Ladder);
    EXPECT_EQ(off.refusal, FarWorldRefusal::None);
    EXPECT_EQ(FarWorldPlane(off.effective), station.Snapshot().far_plane);
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
    EXPECT_EQ(on, FarWorldDiagnosticJson(station.Snapshot()));
}

TEST(FarWorldDiagnostics, SerializerContainsBindingPoliciesCoverageCountersAndLogicalStoreLabels) {
    const Station station;
    const auto json = FarWorldDiagnosticJson(station.Snapshot());
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
                              "ladder_water_available"}) {
        EXPECT_TRUE(json.contains(field)) << field;
    }
    ASSERT_EQ(json["tiers"].size(), 5u);
    for (const auto& tier : json["tiers"]) {
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
                                  "submitted_water_indices"}) {
            EXPECT_TRUE(tier.contains(field)) << field;
        }
    }
    EXPECT_EQ(json["tiles"].size(), station.Expected().size());
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
        station.Snapshot().selection.refusal = refusal;
        const auto expected = refusal == FarWorldRefusal::InvalidCavePolicy ? "InvalidCavePolicy"
                                                                            : "CavePolicyMismatch";
        EXPECT_STREQ(FarWorldRefusalName(refusal), expected);
        EXPECT_EQ(FarWorldDiagnosticJson(station.Snapshot())["refusal"], expected);
        EXPECT_FALSE(station.Ready());
    }
}
} // namespace

TEST(FarWorldHandoffDiagnostics, ResetClearsAllSlotsAndReleasesOwnedValuesInPlace) {
    auto snapshot = std::make_unique<FarWorldHandoffDiagnosticSnapshot>();
    auto water = std::make_shared<const FarWorldConstantWaterObservation>(42, false, 0);
    std::weak_ptr<const FarWorldWaterObservation> const weak = water;
    snapshot->captured = true;
    snapshot->complete = false;
    snapshot->frame = 7;
    snapshot->state.target_serial = 4;
    snapshot->upload.upload_attempts = 3;
    snapshot->submission.draws = 2;
    snapshot->memory.back().bytes_total = 1;
    snapshot->tile_count = snapshot->tiles.size();
    snapshot->draw_count = snapshot->draws.size();
    for (auto& tile : snapshot->tiles) {
        tile.key = {5, -1, -2};
        tile.observation.target.emplace().water = water;
        tile.serving_arrival = tile.target_arrival = true;
    }
    for (auto& draw : snapshot->draws) {
        draw.submitted = draw.arrival.eligible = true;
        draw.child_identities.back().content_hash = 42;
    }
    water.reset();
    snapshot->Reset();
    EXPECT_TRUE(weak.expired());
    const auto empty = std::make_unique<FarWorldHandoffDiagnosticSnapshot>();
    EXPECT_EQ(FarWorldHandoffDiagnosticJson(*snapshot), FarWorldHandoffDiagnosticJson(*empty));
    for (const auto& tile : snapshot->tiles) {
        EXPECT_FALSE(tile.observation.target);
        EXPECT_EQ(tile.key, FarTileKey{});
        EXPECT_FALSE(tile.serving_arrival);
        EXPECT_FALSE(tile.target_arrival);
    }
    for (const auto& draw : snapshot->draws) {
        EXPECT_FALSE(draw.submitted);
        EXPECT_FALSE(draw.arrival.eligible);
        EXPECT_EQ(draw.child_identities.back().content_hash, 0u);
    }
    snapshot->Reset();
    EXPECT_EQ(snapshot->tile_count, 0u);
    EXPECT_EQ(snapshot->draw_count, 0u);
}

TEST(FarWorldDiagnostics, ResetRestoresDefaultsIncludingUnusedTileSlots) {
    auto snapshot = std::make_unique<FarWorldDiagnosticSnapshot>();
    snapshot->captured = snapshot->advanced = true;
    snapshot->complete = false;
    snapshot->world_epoch = 5;
    snapshot->far_plane = 1;
    snapshot->bound_cave_policy = FarCaveMode::BandLimited;
    snapshot->tiles.back().desired.content_hash = 42;
    snapshot->tiles.back().water_complete = true;
    snapshot->tiers.back().indices = 7;
    snapshot->Reset();
    const auto empty = std::make_unique<FarWorldDiagnosticSnapshot>();
    EXPECT_EQ(FarWorldDiagnosticJson(*snapshot), FarWorldDiagnosticJson(*empty));
    EXPECT_FALSE(snapshot->tiles.back().water_complete);
    EXPECT_EQ(snapshot->tiles.back().desired.content_hash, 0u);
    EXPECT_EQ(snapshot->tiers.back().indices, 0u);
}

TEST(FarWorldHandoffDiagnostics, ServingContinuityCannotQualifyLatestTarget) {
    FarWorldHandoffDiagnosticSnapshot snapshot;
    snapshot.captured = true;
    snapshot.state.world_epoch = 1;
    snapshot.state.serving_serial = 4;
    snapshot.state.target_serial = 5;
    snapshot.state.committed_generation = 9;
    snapshot.state.awaiting_replacement_fallback = true;
    snapshot.tile_count = 1;
    auto& tile = snapshot.tiles[0];
    tile.key = {5, -1, 0};
    auto& value = tile.observation;
    auto& serving = value.serving.emplace();
    auto& target = value.target.emplace();
    serving.identity = {1, 42, 0, tile.key, 4, 1};
    target.identity = {1, 43, 0, tile.key, 5, 2};
    serving.intent.extra_span = FarVolumeCoverageSpan{-256, 64};
    target.intent.extra_span = FarVolumeCoverageSpan{-512, 128};
    value.serving_built.span = {-256, 256};
    value.target_built.span = {-512, 256};
    value.serving_coverage = {FarTileState::Uploaded, true, true};
    value.serving_water_complete = true;
    value.target_eligibility = FarVolumeEligibility::RequestRejected;
    EXPECT_FALSE(FarWorldHandoffTargetReady(snapshot));
    const auto json = FarWorldHandoffDiagnosticJson(snapshot);
    EXPECT_EQ(json["schema"], "luminumbra.far_handoff.v1");
    EXPECT_EQ(json["serving_serial"], 4u);
    EXPECT_EQ(json["target_serial"], 5u);
    EXPECT_TRUE(json["tiles"][0]["serving_covering"].get<bool>());
    EXPECT_FALSE(json["tiles"][0]["target_covering"].get<bool>());
    EXPECT_TRUE(json["tiles"][0]["serving_older_than_target"].get<bool>());
    EXPECT_EQ(json["tiles"][0]["serving"]["content_hash"], 42u);
    EXPECT_EQ(json["tiles"][0]["target"]["content_hash"], 43u);
    value.target_coverage = {FarTileState::Empty, true, true};
    value.target_water_complete = true;
    EXPECT_TRUE(FarWorldHandoffTargetReady(snapshot));
    value.target_water_complete = false;
    EXPECT_FALSE(FarWorldHandoffTargetReady(snapshot));
    value.target_water_complete = true;
    snapshot.complete = false;
    EXPECT_FALSE(FarWorldHandoffTargetReady(snapshot));
}

TEST(FarWorldHandoffDiagnostics, DrawArtifactKeepsServingChildIdentitiesAndAttemptBytes) {
    FarWorldHandoffDiagnosticSnapshot snapshot;
    snapshot.captured = true;
    snapshot.state.serving_serial = 1;
    snapshot.state.target_serial = 2;
    snapshot.upload.upload_attempts = 1;
    snapshot.upload.upload_bytes = 640;
    snapshot.upload.successful_upload_bytes = 0;
    snapshot.draw_count = 1;
    auto& draw = snapshot.draws[0];
    draw.identity = {7, 42, 0, {5, -1, -1}, 1, 3};
    draw.arrival.eligible = true;
    draw.submitted = true;
    snapshot.submission.draws = 1;
    for (std::size_t i = 0; i < 4; ++i) {
        draw.child_identities[i] = {
            7, 50 + i, 0, {4, -2 + std::int64_t(i % 2), -2 + std::int64_t(i / 2)}, 1, 3};
        draw.arrival.children[i] = {-256, 128 + std::int64_t(i) * 128};
    }
    const auto json = FarWorldHandoffDiagnosticJson(snapshot);
    EXPECT_EQ(json["upload_attempts"], 1u);
    EXPECT_EQ(json["attempted_upload_bytes"], 640u);
    EXPECT_EQ(json["successful_upload_bytes"], 0u);
    EXPECT_EQ(json["submitted_draws"], 1u);
    EXPECT_TRUE(json["draws"][0]["submitted"].get<bool>());
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(json["draws"][0]["children"][i]["bake_generation"], 1u);
        EXPECT_EQ(json["draws"][0]["children"][i]["content_hash"], 50 + i);
        EXPECT_EQ(json["draws"][0]["children"][i]["built_span"][1], 128 + i * 128);
    }
}
