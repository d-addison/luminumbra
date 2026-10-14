#pragma once

#include "components/CreatureComponents.h"
#include "world/ActiveRegionLedger.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace luminumbra::ai {

using CoarseRegionRecords =
    std::map<Luminumbra::world::RegionKey, Luminumbra::world::ActiveRegionRecord>;

inline constexpr std::uint32_t kCoarsePpm = 1000000;
inline constexpr std::int64_t kCreatureRegionMm = 512000;
inline constexpr std::uint64_t kCoarseMaturityCadences = 90;
inline constexpr std::uint64_t kCoarseCourtshipCadences = 3;
inline constexpr std::uint32_t kCoarseHealthyStaminaPpm = 500000;
inline constexpr std::uint32_t kCoarseMateThirstLimitPpm = 300000;
inline constexpr std::int64_t kCoarseCourtshipRadiusMm = 2600;

// Integer projection of persisted components and the adopted species policy.
// No runtime handles, registry order, anchors, elapsed time, or hidden RNG state.
struct CoarseCreature {
    Luminumbra::Components::CreatureRegionComponent region;
    Luminumbra::Components::CreatureCoarseComponent coarse;
    Luminumbra::world::CreatureDistantPolicy policy;
    std::uint16_t species_id = 0;
    bool alive = true;
    bool has_genome = false;
    bool is_predator = false;
    bool female = false;
    std::uint32_t stamina_ppm = kCoarsePpm;
    std::uint32_t mate_hunger_limit_ppm = 300000;
    std::uint32_t generation = 0;
};

// A staged command, without an allocated ID. The adapter breeds both persisted
// parental genomes from seed, then admits the child after the complete pass.
struct CoarseBirth {
    std::uint64_t mother_id = 0, father_id = 0, seed = 0;
    Luminumbra::world::RegionKey owner;
    std::int64_t x_mm = 0, y_mm = 0, z_mm = 0;
    std::uint16_t species_id = 0;
    std::uint32_t generation = 0;
    bool female = false;
    bool operator==(const CoarseBirth&) const = default;
};

struct CoarseStepResult {
    // Complete next snapshot in PersistentId order, including unchanged rows.
    std::vector<CoarseCreature> creatures;
    std::vector<CoarseBirth> births;
    std::vector<std::uint64_t> deaths;
    // One entity update per accepted cadence, charged to the previous owner.
    // Birth/removal work is added by the adapter only after successful admission.
    std::vector<Luminumbra::world::RegionWork> work;
};

// Mathematical floor ownership, including negative millimetres. Rejects XZ
// outside the persisted world domain before narrowing to a RegionKey.
[[nodiscard]] Luminumbra::world::RegionKey CreatureRegionAtMillimetres(std::int64_t x,
                                                                       std::int64_t z);

// Pure, integer-only pass over a sealed previous snapshot. due contains only
// eligible owners; regions supplies existence, frozen state and cadence separately.
// Travel can enter non-frozen off-due owners. Transfers publish after the pass;
// the last tick stamp also prevents another step in the destination cadence window
// (tick >> cadence_shift). Stamp zero means no completed step. No ResumeHook.
// Invalid input throws before a result is published; input is never mutated.
[[nodiscard]] CoarseStepResult
StepCoarseCreatures(std::span<const CoarseCreature> previous,
                    std::span<const Luminumbra::world::RegionKey> due,
                    const CoarseRegionRecords& regions,
                    std::uint64_t world_seed,
                    std::uint64_t tick);

} // namespace luminumbra::ai
