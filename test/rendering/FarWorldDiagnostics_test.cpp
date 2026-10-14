#include "luminumbra_client/rendering/FarWorldDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldHandoffDiagnostics.h"

#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;
static_assert(std::is_trivially_copyable_v<FarWorldDiagnosticSnapshot>);

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
