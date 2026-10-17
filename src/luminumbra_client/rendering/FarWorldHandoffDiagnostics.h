#pragma once

#include "FarVolumeHandoff.h"
#include "FarWorldSelection.h"
#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace Luminumbra::Rendering {
inline constexpr std::size_t kFarHandoffDiagnosticSlots = 512;
struct FarHandoffTileDiagnostic {
    World::FarTileKey key;
    FarHandoffTileObservation observation;
    bool serving_arrival = false, target_arrival = false;
};
struct FarHandoffDrawDiagnostic {
    World::FarBuildIdentity identity;
    World::FarVolumeRequest request;
    FarWorldArrival arrival;
    std::array<World::FarBuildIdentity, 4> child_identities{};
    bool submitted = false;
};
struct FarWorldHandoffDiagnosticSnapshot {
    FarHandoffState state;
    FarVolumeFrameResult upload;
    FarWorldSubmitStats submission;
    std::array<World::FarVolumeTierMemoryStats, World::kFarTierCount> memory{};
    std::uint64_t frame = 0;
    bool captured = false, complete = true;
    std::size_t tile_count = 0, draw_count = 0;
    std::array<FarHandoffTileDiagnostic, kFarHandoffDiagnosticSlots> tiles{};
    std::array<FarHandoffDrawDiagnostic, kFarHandoffDiagnosticSlots> draws{};
    // Reconstruct in place: a whole-snapshot temporary can exhaust debug stacks.
    void Reset() noexcept {
        std::destroy_at(this);
        std::construct_at(this);
    }
};
void CaptureFarWorldHandoffDiagnostic(FarVolumeHandoff&,
                                      std::uint64_t frame,
                                      const FarVolumeFrameResult&,
                                      FarWorldHandoffDiagnosticSnapshot&);
// Latest-target oracle; stale serving coverage never qualifies.
bool FarWorldHandoffTargetReady(const FarWorldHandoffDiagnosticSnapshot&) noexcept;
nlohmann::json FarWorldHandoffDiagnosticJson(const FarWorldHandoffDiagnosticSnapshot&);
} // namespace Luminumbra::Rendering
