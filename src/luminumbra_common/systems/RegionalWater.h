#pragma once

#include "../world/WaterShare.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Luminumbra {
class Chunk;
}

namespace Luminumbra::Systems::RegionalWater {

// Derived planning state only; never persisted or hashed. Sorted arrays define
// execution order; the hash index is used exclusively for point lookups.
inline constexpr std::size_t missing = std::numeric_limits<std::size_t>::max();
struct Resident {
    ChunkID id{};
    // Borrowed from the current resident span. The weak identity detects replacements
    // without keeping a parked/evicted live chunk alive between ticks.
    Chunk* chunk = nullptr;
    std::weak_ptr<Chunk> identity{};
    IVec3 coords{};
    world::RegionKey owner{};
    std::array<std::size_t, 4> neighbors{missing, missing, missing, missing};
    std::size_t candidate = missing;
    bool admitted = false, preparation = false, awake = false, source = false;
    bool selected = false, staged = false, changed = false;
    std::shared_ptr<Chunk> workspace{};
};

struct Face {
    world::RegionKey owner{}, neighbor{};
    ChunkID positive{}, other{};
    bool is_x = false;
    auto operator<=>(const Face&) const = default;
};
struct IndexedFace {
    Face face{};
    std::size_t positive = missing, other = missing;
};
struct Candidates {
    world::RegionKey owner{};
    std::vector<std::size_t> ids{};
    bool admitted = false, due = false;
};
struct Source {
    ChunkID target{};
    std::size_t cell = 0;
    std::int32_t millimetres = 0;
    auto operator<=>(const Source&) const = default;
};

class PlanningCache {
public:
    // Checks the complete non-loading identity roster, including frozen/unknown owners.
    // Geometry is rebuilt only when that roster changes. Admission and prior flags are
    // refreshed every tick, so filtering cached faces equals rebuilding admitted faces.
    void Refresh(std::span<const std::shared_ptr<Chunk>> resident,
                 const world::ActiveRegionLedger& ledger,
                 int resolution);
    Resident* Find(ChunkID id);
    const Resident* Find(ChunkID id) const;
    Candidates* FindCandidates(world::RegionKey owner);
    void PlanCandidates(const world::ActiveRegionLedger& ledger,
                        const world::RegionSchedule& schedule,
                        bool raining);

    std::vector<Resident> entries;  // Strictly ascending unique ChunkID, including sealed owners.
    std::vector<IndexedFace> faces; // Strict Face order, including sealed endpoints.
    std::vector<Candidates> candidates; // Strict RegionKey order; stable topology indexes.
    std::vector<Source> sources;
    std::vector<world::RegionKey> due;
    std::vector<world::water_share::Claim> claims;
    std::vector<std::size_t> reserved, executed, selected, staged;
    std::vector<world::RegionWork> work;
    std::vector<Chunk*> participants;

private:
    void Rebuild(std::span<const std::shared_ptr<Chunk>> resident);
    std::unordered_map<ChunkID, std::size_t> m_index;
    // A byte per cached identity detects duplicates without constructing a per-tick set.
    std::vector<unsigned char> m_seen;
    // Capture Loading eligibility once per call, including on a rebuilding tick.
    std::vector<std::size_t> m_roster;
};
} // namespace Luminumbra::Systems::RegionalWater
