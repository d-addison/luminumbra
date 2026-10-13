#include "luminumbra_client/rendering/FarWorldDiagnostics.h"

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
        snapshot.frame = 8;
        snapshot.far_plane = kFarPlaneMeters;
        snapshot.camera = {-512, 12, 1024};
        snapshot.captured = true;
        snapshot.advanced = true;
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
            auto& tier = snapshot.tiers[key.tier - 1].resident;
            ++tier.wanted;
            ++tier.settled;
            ++tier.empty_pairs;
            ++tier.covering;
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

TEST(FarWorldDiagnostics, MixedCoverageRequiresActualSuccessfulSubmission) {
    Station station;
    auto& tile = station.snapshot.tiles[0];
    tile.coverage.state = FarTileState::Uploaded;
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
                              "cave_policy",
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
                                  "draw_refusals"})
            EXPECT_TRUE(tier.contains(field)) << field;
    EXPECT_EQ(json["tiles"].size(), station.expected.size());
    EXPECT_EQ(json["tiles"][0]["built"], json["tiles"][0]["desired"]);
    EXPECT_EQ(json["gpu_bytes_kind"], "logical_stores");
    EXPECT_EQ(json["memory_sampling"], "asynchronous_diagnostic_not_atomic_admission");
    EXPECT_FALSE(json["ladder_water_available"].get<bool>());
    const auto bytes = json.dump();
    EXPECT_THROW((void)nlohmann::json::parse(bytes.substr(0, bytes.size() - 1)),
                 nlohmann::json::parse_error);
    EXPECT_THROW((void)nlohmann::json::parse("{broken"), nlohmann::json::parse_error);
}
} // namespace
