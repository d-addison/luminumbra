#pragma once

//   ( germination): CropLifecycleSystem — advances ripe plants through
// SENESCENCE and reseeds the next generation. Each tick, for every CropLifecycleComponent plant:
//   * below the terminal Fruiting stage -> still growing; reset the ripe counter.
//   * at Fruiting -> ripen (++ripe_ticks). Once ripe_ticks >= lifespan_ticks it SENESCES:
//       - germinate a CHILD seed (the pollination cross PollinationComponent.next_genome if the
//         plant was pollinated, else a self copy) at a deterministically dispersed nearby cell, and
//       - ANNUAL: remove the parent; PERENNIAL: reset it to a vegetative stage to regrow.
// A shorter-lived annual thus turns over generations; a perennial persists and also reseeds. The
// child carries a fresh CropLifecycleComponent so the loop continues.
//
// DETERMINISM: id-ordered read; births/deaths collected then applied after the read pass (no view
// invalidation); the per-plant dispersal RNG is seeded ONLY from integers (offset + parent id +
// tick); DeterministicMath Cos/Sin (libm-free). OPT-IN: no CropLifecycleComponent -> no-op, so the
// canonical roster stays byte-identical. SEED OFFSET REGISTRY: germination uses +36;
// season phase is derived from tick % year and consumes no RNG offset.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "entt/entt.hpp"

#include "../components/CoreComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/GrazeableComponent.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../core/DeterministicMath.h"
#include "../core/DeterministicRng.h"
#include "FarmingSystem.h"     // PlantSeed
#include "PlantGrowthSystem.h" // StageThreshold, Comp alias

#include "RegionConsumerGate.h"
#include "RegionalPlantTicks.h"

namespace luminumbra::foliage {

inline constexpr std::uint64_t kGerminationSeedOffset = 36;
inline constexpr std::uint64_t kSeasonSeedOffset =
    37; // reserved (season phase is tick%year, no RNG)
// Registry continues: +38 energy-field-state (fields/EnergyFieldState.h — RNG-free,
// collision-avoidance only); +39 ambient cells; +40 regional weather with separate
// spawn/strike domains (fields/FieldPageState.h). Next free: +41.

struct CropLifecycleStats {
    int ripening = 0; // plants currently at the terminal stage
    int reseeded = 0; // senescence events (a child germinated)
    int reset = 0;    // perennials regrown
    int died = 0;     // annuals removed
};

// No allocator preserves legacy births, including calls with a default region gate.
struct LegacyPlantIdentityAllocator {
    bool operator()(std::uint64_t&) const {
        return true;
    }
};

namespace crop_detail {

// Validate the whole admitted roster before allocating IDs or changing parents.
inline std::vector<entt::entity> Collect(entt::registry& reg,
                                         const ::Luminumbra::world::RegionConsumerGate& gate,
                                         const ::Luminumbra::world::ActiveRegionLedger* allocator) {
    namespace Comp = ::Luminumbra::Components;
    if (gate.UsesSimulationPlantIdentity() != (allocator != nullptr))
        throw std::invalid_argument("Plant reproduction identity and allocator must agree");

    auto view = reg.view<Comp::CropLifecycleComponent,
                         Comp::PlantGrowthComponent,
                         const Comp::PlantGenomeComponent,
                         const Comp::TransformComponent>();
    // The participating component pools are bounded by EnTT's entity mask.
    std::vector<entt::entity> ents;
    for (auto e : view) {
        if (gate.Allows(e)) {
            ents.push_back(e);
        }
    }
    if (ents.empty())
        return ents; // opt-in: no participants -> byte-identical no-op
    if (allocator) {
        OrderPlantsBySimulationIdentity(reg, ents, gate.PlantIdentityScratch());
        // All participants must already have issued IDs before any parent changes.
        for (const auto entity : ents) {
            if (RequirePlantSimulationId(reg, entity) >= allocator->next_persistent_id())
                throw std::runtime_error("Unissued plant reproduction identity");
        }
    } else {
        std::sort(ents.begin(), ents.end(), [&reg, &gate](entt::entity a, entt::entity b) {
            return gate.UsesPersistentPlantIdentity() ? PlantIdentityLess(reg, a, b)
                                                      : entt::to_integral(a) < entt::to_integral(b);
        });
    }

    return ents;
}

struct Scratch {
    struct Birth {
        entt::entity parent;
        std::uint32_t ripe_ticks;
        ::Luminumbra::Vec3 pos;
        Comp::PlantGenomeComponent genome;
        bool perennial;
        std::uint32_t lifespan;
        std::uint16_t species;
        bool grazeable;
        bool owned;
        Comp::PlantRegionComponent identity;
    };
    std::vector<Birth> births;
    std::vector<entt::entity> deaths;
};

// An installed allocator supplies a shared persistent ID or refuses before parent
// mutation. remaining == 0 selects the exact legacy one-tick behaviour.
template<typename IdentityAllocator>
inline CropLifecycleStats RunCropLifecycle(entt::registry& reg,
                                           std::span<const entt::entity> ents,
                                           std::uint64_t tick,
                                           std::uint64_t world_seed,
                                           const ::Luminumbra::world::RegionConsumerGate& gate,
                                           const IdentityAllocator& allocate_identity,
                                           ::Luminumbra::world::ActiveRegionLedger* allocator,
                                           Scratch& scratch,
                                           std::uint32_t remaining = 0) {
    namespace Comp = ::Luminumbra::Components;
    namespace dm = ::Luminumbra::DeterministicMath;
    CropLifecycleStats stats;
    auto view = reg.view<Comp::CropLifecycleComponent,
                         Comp::PlantGrowthComponent,
                         const Comp::PlantGenomeComponent,
                         const Comp::TransformComponent>();
    const std::uint8_t kFruiting = static_cast<std::uint8_t>(Comp::PlantStage::Fruiting);

    auto& births = scratch.births;
    auto& deaths = scratch.deaths;
    births.clear();
    deaths.clear();
    // At most one birth and death per admitted parent. Regional callers reuse
    // these capacities across all 65,536 bounded substeps.
    if (allocator || remaining != 0) {
        births.reserve(ents.size());
        deaths.reserve(ents.size());
    }
    const auto complete_senescence = [&](entt::entity parent, std::uint32_t ripe_ticks) {
        auto& lc = reg.get<Comp::CropLifecycleComponent>(parent);
        auto& growth = reg.get<Comp::PlantGrowthComponent>(parent);
        ++stats.ripening;
        lc.ripe_ticks = ripe_ticks;
        ++lc.generations;
        ++stats.reseeded;
        if (lc.perennial) {
            growth.stage = static_cast<std::uint8_t>(Comp::PlantStage::Juvenile);
            growth.growth_points =
                StageThreshold(static_cast<std::uint8_t>(Comp::PlantStage::Sprout));
            growth.stress_points /= 2u;
            lc.ripe_ticks = 0;
            ++stats.reset;
        } else {
            deaths.push_back(parent);
            ++stats.died;
        }
    };

    for (auto e : ents) {
        auto& lc = view.get<Comp::CropLifecycleComponent>(e);
        auto& g = view.get<Comp::PlantGrowthComponent>(e);

        if (g.stage < kFruiting) {
            lc.ripe_ticks = 0;
            continue;
        } // still growing
        const std::uint32_t ripe_ticks =
            (allocator || remaining != 0) &&
                    lc.ripe_ticks == std::numeric_limits<std::uint32_t>::max()
                ? lc.ripe_ticks
                : lc.ripe_ticks + 1u;
        if (ripe_ticks < lc.lifespan_ticks) {
            ++stats.ripening;
            lc.ripe_ticks = ripe_ticks;
            continue; // ripening, not yet senescent
        }

        // SENESCENCE: pick the child genome (pollination cross if pollinated, else a self copy).
        Comp::PlantGenomeComponent childGenome = view.get<const Comp::PlantGenomeComponent>(e);
        if (const auto* pc = reg.try_get<const Comp::PollinationComponent>(e);
            pc && pc->pollinated) {
            childGenome = pc->next_genome;
        }

        // Deterministic seed DISPERSAL: a nearby cell from a per-plant integer-seeded RNG.
        luminumbra::core::DeterministicRng rng = luminumbra::core::DeterministicRng::seeded(
            kGerminationSeedOffset ^ world_seed,
            allocator ? RequirePlantSimulationId(reg, e)
                      : (gate.UsesPersistentPlantIdentity()
                             ? PlantSimulationId(reg, e)
                             : static_cast<std::uint64_t>(entt::to_integral(e))),
            tick);
        const float ang = rng.next_unit() * 6.2831853f;
        const float rad = 1.0f + rng.next_unit() * 2.0f; // 1..3 m from the parent
        const auto& tf = view.get<const Comp::TransformComponent>(e);
        const ::Luminumbra::Vec3 childPos(
            tf.position.x + rad * dm::Cos(ang), tf.position.y, tf.position.z + rad * dm::Sin(ang));
        if (!gate.AllowsPosition(childPos) ||
            (remaining != 0 && gate.ElapsedTicksAtPosition(childPos) < remaining)) {
            continue;
        }
        Comp::PlantRegionComponent identity;
        const bool owned = reg.all_of<Comp::PlantRegionComponent>(e);
        if (owned) {
            // A disabled regional gate admits every position. Resolve ownership before
            // either allocator can consume an ID or the birth factory can create a child.
            if (!::Luminumbra::world::ActiveRegionLedger::valid_anchor(childPos)) {
                continue;
            }
            const auto owner = ::Luminumbra::world::ActiveRegionLedger::region_at(childPos);
            identity.owner_x = owner.x;
            identity.owner_z = owner.z;
            identity.region_stamp = tick;
        }
        if (remaining != 0 && !FarmingActionsAllowed(reg, childPos))
            continue;
        if (owned && !allocator) {
            if (!allocate_identity(identity.persistent_id)) {
                continue;
            }
        }
        births.push_back({e,
                          ripe_ticks,
                          childPos,
                          childGenome,
                          lc.perennial,
                          lc.lifespan_ticks,
                          lc.species_id,
                          reg.all_of<Comp::GrazeableComponent>(e),
                          owned,
                          identity});
        if (!allocator && remaining == 0)
            complete_senescence(e, ripe_ticks);
    }

    // Apply AFTER the read pass (no view invalidation mid-iteration).
    for (const auto& b : births) {
        // Candidate births allocate through the shared factory, in parent identity order;
        // each parent has one child (ordinal zero).
        // A refused birth leaves that parent's lifecycle, growth and grazing untouched.
        const auto child = allocator ? PlantSeed(reg, b.pos, b.genome, b.species, tick, *allocator)
                                     : PlantSeed(reg, b.pos, b.genome, b.species, tick);
        if (child == entt::null)
            continue;
        if (allocator && b.owned) {
            auto identity = b.identity;
            identity.persistent_id = RequirePlantSimulationId(reg, child);
            reg.emplace<Comp::PlantRegionComponent>(child, identity);
        } else if (b.identity.persistent_id != 0) {
            reg.emplace<Comp::PlantRegionComponent>(child, b.identity);
        }
        auto& clc = reg.emplace<Comp::CropLifecycleComponent>(child);
        clc.perennial = b.perennial;
        clc.lifespan_ticks = b.lifespan;
        clc.species_id = b.species;
        if (b.grazeable) {
            reg.emplace<Comp::GrazeableComponent>(child);
        }
        // Carry the cross-pollination opt-in so the generational loop keeps drifting (a child that
        // could not itself be pollinated would freeze the field's genetics after one generation). A
        // fresh PollinationComponent starts unpollinated; it crosses once the child flowers near a
        // donor.
        reg.emplace<Comp::PollinationTag>(child);
        reg.emplace<Comp::PollinationComponent>(child);
        reg.emplace<Comp::SoilFeederComponent>(
            child); // children feed on soil too (FarmingSystem opt-in parity)
        if (allocator || remaining != 0)
            complete_senescence(b.parent, b.ripe_ticks);
    }
    for (auto e : deaths)
        reg.destroy(e);
    return stats;
}

} // namespace crop_detail

// Shipping callers retain legacy ordering and allocation until identity activation.
template<typename IdentityAllocator = LegacyPlantIdentityAllocator>
inline CropLifecycleStats
RunCropLifecycleOnTick(entt::registry& reg,
                       std::uint64_t tick,
                       std::uint64_t world_seed = 0,
                       const ::Luminumbra::world::RegionConsumerGate& gate = {},
                       const IdentityAllocator& allocate_identity = {}) {
    const auto participants = crop_detail::Collect(reg, gate, nullptr);
    crop_detail::Scratch scratch;
    return crop_detail::RunCropLifecycle(
        reg, participants, tick, world_seed, gate, allocate_identity, nullptr, scratch);
}

// Dormant candidate lane, in both regional modes. Detached callers use the stage's
// identity_allocator(); publication admits its cursor before exposing any births.
inline CropLifecycleStats
RunCropLifecycleOnTick(entt::registry& reg,
                       std::uint64_t tick,
                       std::uint64_t world_seed,
                       const ::Luminumbra::world::RegionConsumerGate& gate,
                       ::Luminumbra::world::ActiveRegionLedger& allocator) {
    const auto participants = crop_detail::Collect(reg, gate, &allocator);
    crop_detail::Scratch scratch;
    return crop_detail::RunCropLifecycle(reg,
                                         participants,
                                         tick,
                                         world_seed,
                                         gate,
                                         LegacyPlantIdentityAllocator{},
                                         &allocator,
                                         scratch);
}

namespace crop_detail {

template<typename IdentityAllocator>
inline CropLifecycleStats RunRegional(entt::registry& reg,
                                      std::uint64_t due_tick,
                                      std::uint64_t world_seed,
                                      const ::Luminumbra::world::RegionConsumerGate& gate,
                                      const IdentityAllocator& allocate_identity,
                                      ::Luminumbra::world::ActiveRegionLedger* allocator) {
    const auto participants = Collect(reg, gate, allocator);
    plant_elapsed_detail::RegionalPlantTicks schedule(reg, participants, gate, due_tick);
    CropLifecycleStats total;
    Scratch scratch;
    schedule.Run([&](std::span<const entt::entity> active,
                     std::uint64_t tick,
                     std::uint32_t remaining) {
        const auto stats = RunCropLifecycle(
            reg, active, tick, world_seed, gate, allocate_identity, allocator, scratch, remaining);
        plant_elapsed_detail::AddCount(total.ripening, stats.ripening);
        plant_elapsed_detail::AddCount(total.reseeded, stats.reseeded);
        plant_elapsed_detail::AddCount(total.reset, stats.reset);
        plant_elapsed_detail::AddCount(total.died, stats.died);
    });
    return total;
}

} // namespace crop_detail

// Ripening uses saturating integer substeps. A successful event resets/removes the
// parent once; no growth is simulated here, so reset perennials and newborn seeds
// cannot ripen again in this call. A refused destination/allocation leaves the
// parent unchanged and is retried next virtual tick. Both source and destination
// windows must cover the event tick. Dispersal RNG, planted_tick, last_tick and
// region_stamp use that tick; IDs are issued in tick then parent-identity order.
// Genomes/stages are sampled at due time, as in the other regional plant consumers.
template<typename IdentityAllocator = LegacyPlantIdentityAllocator>
inline CropLifecycleStats
RunCropLifecycleRegional(entt::registry& reg,
                         std::uint64_t due_tick,
                         const ::Luminumbra::world::RegionConsumerGate& gate,
                         std::uint64_t world_seed = 0,
                         const IdentityAllocator& allocate_identity = {}) {
    return crop_detail::RunRegional(reg, due_tick, world_seed, gate, allocate_identity, nullptr);
}

inline CropLifecycleStats
RunCropLifecycleRegional(entt::registry& reg,
                         std::uint64_t due_tick,
                         const ::Luminumbra::world::RegionConsumerGate& gate,
                         std::uint64_t world_seed,
                         ::Luminumbra::world::ActiveRegionLedger& allocator) {
    return crop_detail::RunRegional(
        reg, due_tick, world_seed, gate, LegacyPlantIdentityAllocator{}, &allocator);
}

} // namespace luminumbra::foliage
