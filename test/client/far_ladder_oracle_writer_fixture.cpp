#include "../support/SimulationContentFixture.h"
#include "core/RuntimeScenarioHarness.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/FarFrozenObservation.h"
#include "luminumbra_common/world/FarWantedSet.h"
#include "luminumbra_common/world/GameSession.h"
#include "rendering/FarWorldHandoffDiagnostics.h"
#include "rendering/FarWorldRenderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Luminumbra::Client::ScenarioHarness::FarLodHorizonStationCapture;
using Luminumbra::Client::ScenarioHarness::WriteFarLodHorizonAnalysis;

std::vector<FarLodHorizonStationCapture> MakeCaptures() {
    FarLodHorizonStationCapture origin{};
    origin.station.name = "origin";
    origin.file = "origin.ppm";
    // Deliberately differ from station labels: v2 must serialize the measured pose.
    origin.camera_yaw_degrees = 12.0f;
    origin.camera_pitch_degrees = -4.0f;
    // At a 512 m grid intersection the 3000 m disc intersects 132 regions:
    // four quadrants, each with 6 + 6 + 6 + 6 + 5 + 4 nearest-point pairs.
    origin.regions_wanted = 132;
    origin.regions_resident = 132;
    origin.resident_bytes = 4096;
    origin.region_draws = 1;
    origin.far_indices_drawn = 3;
    origin.far_world.captured = true;
    origin.far_world.selection =
        Luminumbra::Rendering::SelectFarWorld(Luminumbra::Rendering::FarWorldMode::Legacy,
                                              Luminumbra::Rendering::FarWorldPurpose::World,
                                              Luminumbra::Rendering::FarWorldRefusal::None);
    origin.far_world.requested_cave_policy = Luminumbra::World::FarCaveMode::BoxFiltered;
    origin.far_world.bound_cave_policy = std::nullopt;
    origin.far_world.world_epoch = 1;
    origin.far_world.frame = 1;
    origin.far_world.advanced = true;
    origin.far_world.submission_occurred = true;
    origin.far_world.draws = 1;
    origin.far_world.indices = 3;
    origin.ready_within_deadline = true;
    origin.paired_capture_preserved = true;

    FarLodHorizonStationCapture translated = origin;
    translated.station.name = "translated";
    translated.file = "translated.ppm";
    translated.camera_world_x = -512.0f;
    translated.camera_world_z = 1024.0f;
    translated.far_world.frame = 2;
    translated.far_world.camera = {-512.0, 0.0, 1024.0};
    return {origin, translated};
}

using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;
using Luminumbra::Client::ScenarioHarness::SavedFarStationReady;

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void SavedFarHorizonPreflightOnlyBindsCommonSupport(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "worlds/atlas/presets");
    std::ofstream(root / "worlds/atlas/presets/plane.json") << R"({
        "schema_rev":6,"generation_params":{
            "terrain":{"base_frequency":0.01,"base_amplitude":0.0,"octaves":1,
                "persistence":0.5,"lacunarity":2.0,"height_offset":8.0,
                "shaping":{"peaks_amplitude":0.0,"domain_warp_amplitude":0.0,
                    "continental_spline":[[-1.0,0.0],[1.0,0.0]]},"hydro":{"enabled":false}},
            "features":{"caves_enabled":false,"cave_frequency":0.02}}})";
    Luminumbra::TestSupport::WriteSimulationContentFixture(root);
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    // Session destruction precedes job-system destruction, including on refusal.
    auto session_owner = std::make_unique<Luminumbra::world::GameSession>();
    auto& session = *session_owner;
    session.SetJobSystem(&jobs);
    session.SetRootPath(Luminumbra::Filesystem::GenericPathString(root) + "/");
    session.EnableFarWorldPersistenceSafety();
    Check(session.CreateWorld("Horizon support", "12345", "plane"), "preflight world refused");
    const std::array keys{FarTileKey{5, 0, 0}};
    Luminumbra::world::FarWorldPublicationRequest request;
    request.world_epoch = 1;
    request.wanted = keys;
    request.incremental = true;
    request.limits = Luminumbra::world::GameSession::FarWorldPublicationLimits();
    request.source_memory = std::make_shared<FarVolumeMemoryPool>(2ull * 1024 * 1024 * 1024);
    for (auto& pool : request.memory)
        pool = std::make_shared<FarVolumeMemoryPool>(256ull * 1024 * 1024);
    Luminumbra::Client::ScenarioHarness::SavedFarHorizonSupport support;
    const auto sentinel = std::make_shared<const Luminumbra::world::FarWorldPublication>();
    std::shared_ptr<const Luminumbra::world::FarWorldPublication> output = sentinel;
    std::string diagnostic;
    auto invalid = request;
    invalid.world_epoch = 0;
    Check(!Luminumbra::Client::ScenarioHarness::CaptureSavedFarHorizonPublication(
              session, invalid, support, output, diagnostic),
          "invalid preflight accepted");
    Check(output == sentinel && !support.eye_captured && !support.terrain_height &&
              support.wanted.empty() && support.world_epoch == 0,
          "refused preflight changed state or output");
    bool captured = false, seeded = false;
    for (int poll = 0; poll < 8 && !captured; ++poll) {
        captured = Luminumbra::Client::ScenarioHarness::CaptureSavedFarHorizonPublication(
            session, request, support, output, diagnostic);
        if (!captured) {
            Check(output == sentinel, "pending preflight exposed a partial publication");
            seeded = seeded || support.eye_captured;
        }
    }
    Check(seeded && captured && output && output != sentinel, "common support never published");
    Check(output->tiles.size() == 1, "preflight tile absent");
    const auto intent = output->tiles[0].span.Intent();
    if (!intent || !intent->extra_span)
        throw std::runtime_error("preflight support absent");
    Check(intent->extra_span->min_y <= 0 && intent->extra_span->max_y >= 96,
          "preflight did not retain both camera bands");
    const auto previous = output;
    Check(!Luminumbra::Client::ScenarioHarness::CaptureSavedFarHorizonPublication(
              session, request, support, output, diagnostic),
          "unchanged support recaptured");
    Check(output == previous, "unchanged preflight changed publication");
    invalid = request;
    invalid.world_epoch = 0;
    Check(!Luminumbra::Client::ScenarioHarness::CaptureSavedFarHorizonPublication(
              session, invalid, support, output, diagnostic),
          "replacement refusal accepted");
    Check(output == previous && support.eye_captured && support.world_epoch == request.world_epoch,
          "replacement refusal changed preflight state");
    const std::array shifted_keys{FarTileKey{5, 1, 0}};
    for (int change = 0; change < 3; ++change) {
        if (change == 0)
            request.wanted = shifted_keys;
        else if (change == 1)
            ++request.world_epoch;
        else
            request.caves = FarCaveMode::BoxFiltered;
        const auto retained = output;
        captured = false;
        for (int poll = 0; poll < 8 && !captured; ++poll) {
            captured = Luminumbra::Client::ScenarioHarness::CaptureSavedFarHorizonPublication(
                session, request, support, output, diagnostic);
            if (!captured)
                Check(output == retained, "restarted preflight exposed a partial publication");
        }
        Check(captured && output && output != retained,
              "changed binding did not restart preflight");
        Check(output->world_epoch == request.world_epoch && output->caves == request.caves &&
                  output->tiles.size() == 1 && output->tiles[0].key == request.wanted[0],
              "restarted preflight used old binding");
        const auto restarted_intent = output->tiles[0].span.Intent();
        if (!restarted_intent || !restarted_intent->extra_span)
            throw std::runtime_error("restarted preflight support absent");
        Check(restarted_intent->extra_span->min_y <= 0 && restarted_intent->extra_span->max_y >= 96,
              "restarted preflight lost a camera band");
    }
}

std::unique_ptr<FarWorldHandoffDiagnosticSnapshot> SavedSnapshot(bool empty) {
    auto snapshot = std::make_unique<FarWorldHandoffDiagnosticSnapshot>();
    snapshot->captured = true;
    snapshot->state.world_epoch = 7;
    snapshot->state.committed_generation = 3;
    snapshot->state.serving_committed_generation = 3;
    snapshot->state.target_serial = 5;
    snapshot->state.serving_serial = 5;
    const auto limits = FarWorldDevelopmentLimits();
    for (std::size_t i = 0; i < snapshot->memory.size(); ++i)
        snapshot->memory[i].limit = limits.budgets.bytes[i];
    std::vector<FarTileKey> wanted;
    Check(ComputeFarWantedSet(-512, -512, wanted) == FarWantedError::None,
          "saved fixture wanted-set failed");
    Check(wanted.size() <= snapshot->tiles.size(), "saved fixture diagnostic capacity");
    for (const auto& key : wanted) {
        auto& tile = snapshot->tiles[snapshot->tile_count++];
        tile.key = key;
        FarVolumeWantedRequest request;
        request.identity = {7, 42, 0, key, 3, 1};
        // extra_span stays absent: exercise the public serializer's null path.
        tile.observation.target = request;
        tile.observation.serving = request;
        tile.observation.target_coverage = {FarTileState::Empty, true, true};
        tile.observation.target_built.span = {-256, 256};
        tile.observation.target_water_complete = true;
        tile.observation.target_water_known_dry = true;
    }
    if (!empty) {
        // Two tiles in tier 1 and one in tier 2 expose both omission shapes.
        const auto tier_two = std::find_if(
            wanted.begin(), wanted.end(), [](const auto& key) { return key.tier == 2; });
        Check(wanted.size() >= 2 && tier_two != wanted.end(), "multi-target fixture keys absent");
        for (const std::size_t slot : {std::size_t{0},
                                       std::size_t{1},
                                       static_cast<std::size_t>(tier_two - wanted.begin())}) {
            auto& tile = snapshot->tiles[slot].observation;
            Check(tile.target.has_value(), "saved fixture target absent");
            tile.target_coverage.state = FarTileState::Uploaded;
            tile.target_terrain_indices = 3;
            tile.target_water_indices = 6;
            tile.target_water_known_dry = false;
            auto& draw = snapshot->draws[snapshot->draw_count++];
            draw.identity = tile.target->identity;
            draw.request.span = {-256, 256};
            draw.submitted = true;
            ++snapshot->submission.draws;
            snapshot->submission.indices += 9;
            snapshot->submission.water_indices += 6;
        }
    }
    return snapshot;
}

void OmitLastDraw(FarWorldHandoffDiagnosticSnapshot& snapshot) {
    --snapshot.draw_count;
    --snapshot.submission.draws;
    snapshot.submission.indices -= 9;
    snapshot.submission.water_indices -= 6;
}

void SavedFarReadinessExactDrawableSubmission() {
    const auto selection =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    std::vector<FarTileKey> wanted;
    Check(ComputeFarWantedSet(-512, -512, wanted) == FarWantedError::None, "wanted-set failed");
    auto snapshot = SavedSnapshot(false);
    Check(SavedFarStationReady(*snapshot, selection, wanted, 90),
          "complete multi-target submission rejected");
    OmitLastDraw(*snapshot); // Omit the entire drawable tier 2, with matching totals.
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90),
          "omitted drawable tier accepted");
    snapshot = SavedSnapshot(false);
    std::swap(snapshot->draws[1], snapshot->draws[2]);
    OmitLastDraw(*snapshot); // Omit only one of the two tier 1 tiles.
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90),
          "omitted drawable tile accepted");
    snapshot = SavedSnapshot(false);
    snapshot->draws[1] = snapshot->draws[0];
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90),
          "duplicate submitted identity accepted");
    snapshot = SavedSnapshot(false);
    snapshot->draws[3] = snapshot->draws[0];
    ++snapshot->draw_count;
    ++snapshot->submission.draws;
    snapshot->submission.indices += 9;
    snapshot->submission.water_indices += 6;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90),
          "extra submitted identity accepted");
    snapshot->draws[3].identity.key.tx += 10000;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90),
          "foreign submitted identity accepted");
    snapshot = SavedSnapshot(true);
    Check(snapshot->tiles[0].observation.target.has_value(), "Empty target absent");
    snapshot->draws[0].identity = snapshot->tiles[0].observation.target->identity;
    snapshot->draws[0].submitted = true;
    snapshot->draw_count = 1;
    snapshot->submission.draws = 1;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "Empty draw accepted");
    snapshot = SavedSnapshot(false);
    snapshot->tiles[0].observation.target_terrain_indices =
        std::numeric_limits<std::uint64_t>::max();
    snapshot->submission.indices = 23; // Wrapped sum: max + 6 + 9 + 9.
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "overflowed totals accepted");
    snapshot = SavedSnapshot(false);
    snapshot->tile_count = snapshot->tiles.size() + 1;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "oversized tile array accepted");
    snapshot = SavedSnapshot(false);
    snapshot->draw_count = snapshot->draws.size() + 1;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "oversized draw array accepted");
}

// Actual production capture fingerprints, with bounded rectangular support and
// identical source authority. Increasing support must change render currency.
std::array<std::uint64_t, 3> SavedFarRecaptureFingerprints() {
    FarFrozenCaptureSource source;
    source.pristine.field_identity = 41;
    source.content_identity = 42;
    source.durable = {3, FarEditedImageState::Committed};
    source.pristine.numeric = FarVolumeNumericProfile::LegacySigned16;
    source.render_only = true;
    source.use = FarFrozenUse::Preview;
    source.pristine.height = [](std::int64_t, std::int64_t) {
        return 0.25f;
    };
    source.pristine.density =
        [](const FarVolumePosition& p, float height, std::uint32_t, FarCaveMode) {
            return FarVolumeSample{float(p.y) - height, 3};
        };
    source.water = [](std::int64_t, std::int64_t) {
        return FarFrozenWaterSample{false, 0};
    };
    FarFrozenRegion region{1, {{-16, -16, -16}, {16, 16, 16}}};
    const FarFrozenCaptureLimits limits{1, 10000, 200000, 10000, 4 * 1024 * 1024};
    auto pool = std::make_shared<FarVolumeMemoryPool>(8 * 1024 * 1024);
    std::shared_ptr<const FarFrozenObservation> image;
    std::array<std::uint64_t, 3> hashes{};
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        if (i == 1)
            region.support.max.y = 96;
        if (i == 2)
            source.content_identity = 43; // Authority change with identical support.
        Check(CaptureFarFrozenObservation({&region, 1}, source, limits, pool, image) ==
                  FarFrozenCaptureError::None,
              "real recapture refused");
        hashes[i] = image->content_identity();
    }
    Check(hashes[0] != hashes[1], "expanded support retained the old fingerprint");
    Check(hashes[1] != hashes[2], "changed authority retained the old fingerprint");
    return hashes;
}

void SavedFarReadinessDeadlineAndWantedSet() {
    const auto snapshot = SavedSnapshot(false);
    const auto selection =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    std::vector<FarTileKey> wanted;
    Check(ComputeFarWantedSet(-512, -512, wanted) == FarWantedError::None, "wanted-set failed");
    Check(SavedFarStationReady(*snapshot, selection, wanted, 0), "zero deadline rejected");
    Check(SavedFarStationReady(*snapshot, selection, wanted, 90), "inclusive deadline rejected");
    for (const double seconds : {-1.0,
                                 90.000001,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()})
        Check(!SavedFarStationReady(*snapshot, selection, wanted, seconds),
              "invalid deadline accepted");
    std::reverse(wanted.begin(), wanted.end());
    Check(SavedFarStationReady(*snapshot, selection, wanted, 90),
          "wanted ordering affected readiness");
    wanted.pop_back();
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "missing wanted key accepted");
    wanted.push_back(wanted.front());
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "duplicate wanted key accepted");
    wanted.back().tx += 1000;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "foreign wanted key accepted");
}

void SavedFarReadinessOnSubmissionAndEmpty() {
    auto snapshot = SavedSnapshot(false);
    auto selection =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    std::vector<FarTileKey> wanted;
    Check(ComputeFarWantedSet(-512, -512, wanted) == FarWantedError::None, "wanted-set failed");
    selection.advance = false;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "suppressed advance accepted");
    selection.advance = true;
    selection.submit = false;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "suppressed submit accepted");
    selection.submit = true;
    snapshot->draws[0].submitted = false;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "unsubmitted draw accepted");
    snapshot->draws[0].submitted = true;
    snapshot->submission.indices = 0;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "missing indices accepted");
    snapshot->submission = {};
    snapshot->draw_count = 0;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90),
          "drawable without submission accepted");
    snapshot = SavedSnapshot(true);
    Check(SavedFarStationReady(*snapshot, selection, wanted, 90), "valid Empty rejected");
    snapshot->tiles[0].observation.target_water_complete = false;
    Check(!SavedFarStationReady(*snapshot, selection, wanted, 90), "incomplete water accepted");
}

std::vector<FarLodHorizonStationCapture> MakeSavedCaptures(const std::string& mode) {
    SavedFarReadinessDeadlineAndWantedSet();
    SavedFarReadinessOnSubmissionAndEmpty();
    SavedFarReadinessExactDrawableSubmission();
    std::vector<FarTileKey> wanted;
    Check(ComputeFarWantedSet(-512, -512, wanted) == FarWantedError::None, "wanted-set failed");
    auto snapshot = SavedSnapshot(mode == "saved-empty");
    if (mode == "saved-omit-tile") {
        std::swap(snapshot->draws[1], snapshot->draws[2]);
        OmitLastDraw(*snapshot);
    }
    if (mode == "saved-omit-tier")
        OmitLastDraw(*snapshot);
    const auto hashes = SavedFarRecaptureFingerprints();
    std::vector<FarLodHorizonStationCapture> captures;
    for (const auto& station : Luminumbra::Client::ScenarioHarness::BuildFarLodHorizonStations()) {
        FarLodHorizonStationCapture capture{};
        capture.station = station;
        capture.camera_world_x = -512;
        capture.camera_world_z = -512;
        capture.camera_world_y = station.eye_height_meters;
        capture.camera_yaw_degrees = station.yaw_degrees;
        capture.camera_pitch_degrees = station.pitch_degrees;
        capture.selected_far_world =
            SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
        if (mode == "saved-suppressed")
            capture.selected_far_world.submit = false;
        if (mode == "saved-unsubmitted") {
            snapshot->submission = {};
            snapshot->draws[0].submitted = false;
        }
        ++snapshot->frame;
        if (mode == "saved-common-support" || mode == "saved-recapture" ||
            mode == "saved-authority-change") {
            const bool changed = !captures.empty();
            const bool expanded = mode != "saved-recapture" || changed;
            const auto hash = mode == "saved-authority-change" && changed ? hashes[2]
                              : expanded                                  ? hashes[1]
                                                                          : hashes[0];
            for (std::size_t i = 0; i < snapshot->tile_count; ++i) {
                auto& observation = snapshot->tiles[i].observation;
                Check(observation.target.has_value(), "recapture target absent");
                observation.target->identity.content_hash = hash;
                observation.target->identity.span_generation = expanded ? 2 : 1;
                observation.target->intent.extra_span =
                    FarVolumeCoverageSpan{-16, expanded ? 96.0f : 16.0f};
                observation.target_built.span = {-16, expanded ? 96 : 16};
            }
            for (std::size_t i = 0; i < snapshot->draw_count; ++i) {
                snapshot->draws[i].identity.content_hash = hash;
                snapshot->draws[i].identity.span_generation = expanded ? 2 : 1;
            }
            if (changed)
                ++snapshot->state.target_serial;
        }
        capture.settle_seconds = 90;
        capture.ready_within_deadline = SavedFarStationReady(
            *snapshot, capture.selected_far_world, wanted, capture.settle_seconds);
        capture.paired_capture_preserved = true;
        capture.saved_far_world = FarWorldHandoffDiagnosticJson(*snapshot);
        if (mode == "saved-incomplete" && captures.empty())
            capture.saved_far_world = nullptr;
        captures.push_back(std::move(capture));
    }
    return captures;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr
            << "Usage: far_ladder_oracle_writer_fixture <artifact-dir> "
               "<complete|incomplete|ladder|mixed|refused|preview|saved|saved-empty|saved-"
               "suppressed|saved-unsubmitted|saved-incomplete|saved-omit-tile|saved-omit-tier|"
               "saved-common-support|saved-recapture|saved-authority-change>\n";
        return EXIT_FAILURE;
    }
    const std::string mode = argv[2];
    if (mode != "complete" && mode != "incomplete" && mode != "ladder" && mode != "mixed" &&
        mode != "refused" && mode != "preview" && mode != "saved" && mode != "saved-empty" &&
        mode != "saved-suppressed" && mode != "saved-unsubmitted" && mode != "saved-incomplete" &&
        mode != "saved-omit-tile" && mode != "saved-omit-tier" && mode != "saved-common-support" &&
        mode != "saved-recapture" && mode != "saved-authority-change") {
        std::cerr << "Unknown fixture mode: " << mode << '\n';
        return EXIT_FAILURE;
    }

    try {
        if (mode == "saved-common-support")
            SavedFarHorizonPreflightOnlyBindsCommonSupport(std::filesystem::path(argv[1]) /
                                                           "support-world");
        std::vector<FarLodHorizonStationCapture> captures =
            mode.starts_with("saved") ? MakeSavedCaptures(mode) : MakeCaptures();
        if (mode == "incomplete") {
            captures.back().regions_missing = 1;
            --captures.back().regions_resident;
        }
        using namespace Luminumbra::Rendering;
        for (std::size_t i = 0; i < captures.size(); ++i) {
            auto& snapshot = captures[i].far_world;
            if (mode == "ladder" || (mode == "mixed" && i != 0)) {
                snapshot.selection = SelectFarWorld(
                    FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
                snapshot.far_plane = FarWorldPlane(snapshot.selection.effective);
                // Deliberately unqualified: emission must not invent v1 evidence
                // or make retained Legacy counters a Ladder readiness proof.
                snapshot.complete = false;
            } else if (mode == "refused" || mode == "preview") {
                snapshot.selection = SelectFarWorld(FarWorldMode::Ladder,
                                                    mode == "preview" ? FarWorldPurpose::Preview
                                                                      : FarWorldPurpose::World,
                                                    FarWorldRefusal::ImmutableAuthorityUnavailable);
            }
        }
        // File-only production writer call: no window, GL context, or smoke run.
        // These synthetic captures do not establish readiness within 90 seconds.
        WriteFarLodHorizonAnalysis(std::filesystem::path(argv[1]),
                                   "mountains",
                                   0.0,
                                   captures,
                                   captures.size(),
                                   0.0,
                                   0.0,
                                   false,
                                   false);
    } catch (const std::exception& error) {
        std::cerr << "Writer fixture failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
