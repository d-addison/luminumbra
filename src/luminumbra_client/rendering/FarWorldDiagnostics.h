#pragma once

#include "FarVolumeResidency.h"
#include "FarWorldSelection.h"
#include <nlohmann/json_fwd.hpp>
#include <optional>

namespace Luminumbra::Rendering {
// Value storage only: no resource pointer, string, callback or growing container.
// Included in the renderer's integration charge. Serialization allocates only
// when a recorder/scenario explicitly requests an artifact, never at capture.
inline constexpr std::size_t kFarWorldDiagnosticTiles = 512;
struct FarWorldTierDiagnostic {
    FarVolumeTierObservation resident;
    std::uint64_t evicted_range = 0, evicted_budget = 0, stale_rebuilds = 0;
    std::size_t draws = 0, indices = 0, draw_refusals = 0;
};
struct FarWorldTileDiagnostic {
    World::FarBuildIdentity desired, built;
    World::FarVolumeCoverageIntent intent;
    World::FarVolumeRequest request;
    World::FarTileStatus coverage;
    FarVolumeEligibility eligibility = FarVolumeEligibility::Wanted;
    bool has_pair = false;
    bool water_complete = false, water_known_dry = false;
    std::uint64_t terrain_indices = 0, water_indices = 0;
};
enum class FarWorldSubmissionRefusal {
    None,
    LiveOwnershipIncomplete,
    LogicalStoreAdmission,
    BackendUnavailable,
    DrawRejected
};
struct FarWorldDiagnosticSnapshot {
    FarWorldSelection selection;
    FarWorldSubmissionRefusal submission_refusal = FarWorldSubmissionRefusal::None;
    std::uint64_t world_epoch = 0, frame = 0, content_identity = 0, bake_generation = 0;
    float far_plane = World::kLegacyFarPlaneMeters;
    // Absent means invalid request / no immutable fixture, never a fallback policy.
    std::optional<World::FarCaveMode> requested_cave_policy = std::nullopt;
    std::optional<World::FarCaveMode> bound_cave_policy = std::nullopt;
    World::FarVolumeNumericProfile numeric = World::FarVolumeNumericProfile::LegacySigned16;
    bool captured = false, advanced = false, submission_occurred = false;
    bool complete = true, ladder_water_available = false;
    FarWorldWaterCapability water_capability = FarWorldWaterCapability::Unavailable;
    std::array<double, 3> camera{};
    FarVolumeFrameResult upload;
    std::array<FarWorldTierDiagnostic, World::kFarTierCount> tiers{};
    std::array<FarWorldTileDiagnostic, kFarWorldDiagnosticTiles> tiles{};
    std::size_t tile_count = 0, draws = 0, indices = 0;
};
// A paired render keeps selection/plane and disables only scheduling/submission.
constexpr FarWorldSelection FarWorldSubmissionOverride(FarWorldSelection selected,
                                                       bool submission) noexcept {
    selected.submit = selected.submit && submission;
    selected.advance = selected.advance && submission;
    return selected;
}
const char* FarWorldModeName(FarWorldMode mode) noexcept;
const char* FarWorldRefusalName(FarWorldRefusal refusal) noexcept;
const char* FarWorldSubmissionRefusalName(FarWorldSubmissionRefusal refusal) noexcept;
nlohmann::json FarWorldDiagnosticJson(const FarWorldDiagnosticSnapshot& snapshot);
// Pure readiness: the independent gate additionally checks the expected set.
// Timeout is inclusive at 90 seconds; incomplete/stale/spans never qualify.
enum class FarWorldStationRefusal {
    None,
    Timeout,
    IncompleteSnapshot,
    WrongMode,
    RendererRefused,
    SubmissionSuppressed,
    WrongPlane,
    InvalidBinding,
    NoAdvancement,
    SubmissionRefused,
    ExpectedSetMismatch,
    HorizonRefused,
    RejectedTiles,
    MissingPair,
    StaleGeneration,
    StaleBinding,
    SpanRefused,
    TierCounters,
    DrawTotals,
    MissingSubmission,
    WaterIncomplete
};
FarWorldStationRefusal CheckFarWorldStation(const FarWorldDiagnosticSnapshot& snapshot,
                                            std::span<const World::FarTileKey> expected,
                                            double elapsed_seconds) noexcept;
bool FarWorldStationReady(const FarWorldDiagnosticSnapshot& snapshot,
                          std::span<const World::FarTileKey> expected,
                          double elapsed_seconds) noexcept;
} // namespace Luminumbra::Rendering
