#pragma once

#include "../core/WaterComponents.h"
#include "../world/WaterShare.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Luminumbra::Systems::RegionalWater {

// Bounds apply before copying or sorting inputs. Planning uses only the supplied
// non-loading participation snapshot, never a world-store scan or durable lookup.
inline constexpr std::size_t kMaxResidents = std::size_t{1} << 20;
inline constexpr std::size_t kMaxSources = std::size_t{1} << 20;
struct Resident {
    ChunkID id = 0;
    bool initialized = true;
    int resolution = 8;
    bool awake = true;
    bool preparation_ready = true;
    bool available = true; // False for loading or otherwise unavailable residents.
};
struct SourceInput {
    std::uint64_t id = 0; // Unique snapshot identity, independent of input order.
    Vec3 position{0.0f};
    Components::FlowingWaterSourceComponent physical{};
};
struct Inputs {
    int resolution = 8;
    std::uint64_t cell_budget = 0;
    bool raining = false;
};
struct Source {
    ChunkID target = 0;
    std::size_t cell = 0;
    std::int32_t millimetres = 0;
    auto operator<=>(const Source&) const = default;
};
struct Face {
    world::RegionKey owner{}, neighbor{};
    ChunkID positive = 0, other = 0;
    bool is_x = false;
    std::uint8_t common_shift = 0; // Cross-owner phase is zero modulo 2^shift.
    auto operator<=>(const Face&) const = default;
};
struct RegionPlan {
    world::RegionKey owner{};
    std::uint8_t cadence_shift = 0;
    std::uint32_t cadence_phase = 0;
    std::uint64_t candidates = 0, windows = 0, next_cursor = 0, debt = 0;
    bool operator==(const RegionPlan&) const = default;
};
struct Plan {
    // IDs, sources, faces and owner outputs have canonical ascending order.
    std::vector<ChunkID> interiors, participants;
    std::vector<Source> sources; // Admitted snapshot; apply only to interior targets.
    std::vector<Face> faces;
    std::vector<RegionPlan> regions;
    std::vector<world::RegionKey> refused_preparation;
    std::uint64_t global_windows = 0, border_windows = 0, allocated_windows = 0;
    std::uint64_t preparation_cells = 0, total_cells = 0;
    std::size_t residents_visited = 0; // Snapshot admission visits; no ledger traversal.
    bool operator==(const Plan&) const = default;
};

// Pure planning: no chunk, ledger, cursor, debt or revision is changed. Invalid
// inputs throw invalid_argument before budget allocation. Unready preparation seals the
// whole owner without spending windows or crediting debt. Unknown/frozen owners
// are excluded. Interiors require due membership and the owner's cadence phase;
// cross-owner seams use their common zero phase, even off the interior cadence.
// Each window pays for preparation, one interior and four face endpoints. Keep
// one interior window when claims exist, then reserve cross-owner pairs atomically.
// Region outputs are patches for due admitted owners only (debt zero means erase);
// all other registers must be retained by the eventual publisher. Fixed positive
// eligibility inherits water_share's service bound; cursor rollover serves every
// candidate under fixed eligibility. Changing eligibility has no such guarantee.
// Work: O((R+S+D) log(kMaxRegions+R+S+D)). Storage is bounded by the input
// caps: at most R entries, S sources, D region outputs, 2*R topology faces and
// 5*R participant IDs before deduplication. All products fit 32-bit size_t.
// Callers supply participating snapshots; this API has no production tick caller.
[[nodiscard]] Plan Build(std::span<const Resident> residents,
                         std::span<const SourceInput> sources,
                         const world::ActiveRegionLedger& ledger,
                         const world::RegionSchedule& schedule,
                         const Inputs& inputs);

} // namespace Luminumbra::Systems::RegionalWater
