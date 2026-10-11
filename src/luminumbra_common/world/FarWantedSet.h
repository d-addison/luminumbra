#pragma once

// Pure wanted-set, arrival and residency policy for the volumetric far ladder
// declared in FarTierTable.h; see docs/distant-world.md. No GL, no I/O and no
// scheduler state: results depend only on the arguments and the tier table.

#include "FarTierTable.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Luminumbra::World {

// Camera coordinates with a larger magnitude (metres) are refused.
inline constexpr double kFarWantedCameraLimitMeters = 8388608.0; // 2^23

struct FarTileKey {
    std::uint8_t tier = 1; // 1..kFarTierCount
    std::int64_t tx = 0;
    std::int64_t tz = 0;

    friend constexpr bool operator==(const FarTileKey&, const FarTileKey&) = default;
    // Canonical order: tier, then tz, then tx.
    friend constexpr bool operator<(const FarTileKey& a, const FarTileKey& b) {
        if (a.tier != b.tier) {
            return a.tier < b.tier;
        }
        if (a.tz != b.tz) {
            return a.tz < b.tz;
        }
        return a.tx < b.tx;
    }
};

enum class FarWantedError : std::uint8_t {
    None,
    NonFiniteCamera,
    CameraOutOfRange
};

// W_t = outer_radius + brick_edge for tiers 1..4 (the one-brick overlap) and
// outer_radius for tier 5. Invalid tier numbers return 0.0.
double FarWantedRadiusMeters(std::size_t tier);

// Tile rectangle edge in metres for a valid tier, 0 for an invalid tier.
std::int64_t FarTileEdgeMeters(std::size_t tier);

// Horizontal (XZ) distance from (x, z) to the nearest point of the tile
// rectangle [tx*E, (tx+1)*E) x [tz*E, (tz+1)*E); 0 when inside. An invalid
// tier returns infinity.
double FarTileNearestDistance(const FarTileKey& tile, double x, double z);

// The tier-(t+1) tile that contains this tile (floor division by 2). Tier 5 and
// invalid tiers return the key unchanged.
FarTileKey FarTileParent(const FarTileKey& tile);

// Every tile of every tier whose nearest-point distance to the camera is
// <= FarWantedRadiusMeters(tier), in canonical order. On error `out` is cleared.
FarWantedError ComputeFarWantedSet(double camera_x, double camera_z, std::vector<FarTileKey>& out);

enum class FarTileState : std::uint8_t {
    Wanted,
    Building,
    Ready,
    Uploaded,
    Empty,
    Rejected
};

struct FarTileStatus {
    FarTileState state = FarTileState::Wanted;
    bool current = false; // build identity matches the present world binding and bake generation
    bool span_ok = false; // built vertical span covers the requested span
};

// Uploaded or Empty, and current, and span_ok.
constexpr bool IsFarTileCovering(const FarTileStatus& status) {
    return (status.state == FarTileState::Uploaded || status.state == FarTileState::Empty) &&
           status.current && status.span_ok;
}

struct FarResidentTile {
    FarTileKey key;
    FarTileStatus status;
    std::uint64_t cpu_bytes = 0;
    std::uint64_t gpu_bytes = 0;
};

// Identity of an asynchronous build result; a result is current only when all
// fields equal the present values.
struct FarBuildIdentity {
    std::uint64_t world_epoch = 0;
    std::uint64_t content_hash = 0;
    std::uint8_t cave_policy = 0;
    FarTileKey key;
    std::uint64_t bake_generation = 0;
    std::uint64_t span_generation = 0;

    friend constexpr bool operator==(const FarBuildIdentity&, const FarBuildIdentity&) = default;
};

// True when a coarse tile's surface may be suppressed: the coarse tile is tier
// 2..5 and all four tier-(t-1) tiles that tile its footprint are in `wanted` and
// have a resident entry that IsFarTileCovering. A partly wanted footprint keeps
// the coarse surface, so tier 1 and any missing child return false.
bool CoarseSurfaceMayYield(const FarTileKey& coarse,
                           std::span<const FarTileKey> wanted,
                           std::span<const FarResidentTile> resident);

// Per-tier byte budgets (cpu_bytes + gpu_bytes); 0 means unlimited.
struct FarTierBudgets {
    std::array<std::uint64_t, kFarTierCount> bytes{};
};

struct FarEvictionPlan {
    std::vector<FarTileKey> evict;    // in eviction order
    std::vector<FarTileKey> rejected; // wanted tiles evicted for budget (subset of evict)
    bool horizon_overflow = false;    // tier 5 alone exceeds its budget
};

// Ordering functions below (PlanFarEviction, SelectFar*) treat a non-finite camera
// coordinate as 0 so the order stays a strict weak order. A parent that this plan
// already evicts does not count as covering underlay for a budget eviction.
FarEvictionPlan PlanFarEviction(double camera_x,
                                double camera_z,
                                std::span<const FarResidentTile> resident,
                                std::span<const FarTileKey> wanted,
                                const FarTierBudgets& budgets);

// Wanted tiles that need a build started: no resident entry, or an entry in state
// Wanted, or a Ready/Uploaded/Empty entry that is not current or lacks the span. Rejected and
// Building entries are skipped. Order: tier descending (coarsest first), then nearest-point
// distance ascending, then key. At most max_start entries.
std::vector<FarTileKey> SelectFarBuilds(double camera_x,
                                        double camera_z,
                                        std::span<const FarTileKey> wanted,
                                        std::span<const FarResidentTile> resident,
                                        std::size_t max_start);

// Resident tiles in state Ready, same ordering as SelectFarBuilds. Stops after
// max_count tiles or once the running cpu_bytes total would exceed max_bytes, but
// always returns the first tile even when it alone exceeds max_bytes.
std::vector<FarTileKey> SelectFarUploads(double camera_x,
                                         double camera_z,
                                         std::span<const FarResidentTile> resident,
                                         std::size_t max_count,
                                         std::uint64_t max_bytes);

struct FarTierCounters {
    std::uint32_t wanted = 0;
    std::uint32_t building = 0;
    std::uint32_t ready = 0;
    std::uint32_t uploaded = 0;
    std::uint32_t empty = 0;
    std::uint32_t rejected = 0;
    std::uint64_t cpu_bytes = 0;
    std::uint64_t gpu_bytes = 0;
};

// Snapshot counters per tier (index = tier - 1): `wanted` counts wanted keys; the
// state counts and byte totals come from `resident` (entries with an invalid tier
// are ignored).
std::array<FarTierCounters, kFarTierCount>
AccumulateFarCounters(std::span<const FarResidentTile> resident,
                      std::span<const FarTileKey> wanted);

} // namespace Luminumbra::World
