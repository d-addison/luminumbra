#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace luminumbra::simulation {

// Diagnostic work items, separate from the versioned timed-stage contract.
// Never used for scheduling, persistence, configuration identity or hashes.
enum class SimWork : std::uint8_t {
    WaterCells,
    StreamingGeneration,
    StreamingMeshing,
    PhysicsBodies,
    PhysicsCharacters,
    ActiveRegionRecords,
    ActiveRegionDue,
    SnapshotCopies,
    ActivationQueries,
    Count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(SimWork::Count)>
    kSimWorkNames{"water_cells",
                  "streaming_generation",
                  "streaming_meshing",
                  "physics_bodies",
                  "physics_characters",
                  "active_region_records",
                  "active_region_due",
                  "snapshot_copies",
                  "activation_queries"};
struct SimWorkSample {
    std::uint64_t tick = 0;
    std::uint64_t work = 0;
    bool operator==(const SimWorkSample&) const = default;
};

} // namespace luminumbra::simulation
