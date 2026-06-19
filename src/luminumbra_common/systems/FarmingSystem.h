#pragma once

// I9-FOLIAGE: Living-World Foliage pillar — the FARMING verbs. Deterministic sim
// operations that drive the cultivate -> grow -> tend -> harvest -> breed loop on
// top of the genome (PlantComponents.h) + growth (PlantGrowthSystem.h). The growth
// itself is the env-driven tick; these are the player/agent ACTIONS. Pure +
// integer where it matters (geometry is visual-only; this is sim truth).

#include "../components/PlantComponents.h"
#include "PlantGrowthSystem.h" // ExpressGenome, BreedPlants, Comp alias, namespace
#include "../core/DeterministicRng.h"

#include <cstdint>

namespace luminumbra::foliage {
// `Comp` (= ::Luminumbra::Components) is already aliased in PlantGrowthSystem.h.

// Plant a seed: spawn a PlantTag plant at the Seed stage with the given genome.
// The growth system (once ticking) advances it from here.
inline entt::entity PlantSeed(entt::registry& reg, const ::Luminumbra::Vec3& pos,
                              const Comp::PlantGenomeComponent& genome,
                              std::uint16_t species_id, std::uint64_t tick) {
    const auto e = reg.create();
    auto& tf = reg.emplace<Comp::TransformComponent>(e);
    tf.position = pos;
    reg.emplace<Comp::PlantTag>(e);
    reg.emplace<Comp::PlantGenomeComponent>(e, genome);
    auto& g = reg.emplace<Comp::PlantGrowthComponent>(e);
    g.species_id = species_id;
    g.planted_tick = tick;
    g.last_tick = tick;
    return e;
}

// Water: raise the tended bonus (clamped). The growth tick consumes it slowly, so
// the player must keep watering for a sustained boost.
inline void Water(Comp::PlantGrowthComponent& g, std::uint8_t amount = 120) {
    const int v = static_cast<int>(g.tended) + amount;
    g.tended = static_cast<std::uint8_t>(v > 255 ? 255 : v);
}

// Fertilize: a stronger tend that ALSO eases some already-accumulated stress
// (recovers quality), modelling richer soil husbandry.
inline void Fertilize(Comp::PlantGrowthComponent& g, std::uint8_t amount = 160) {
    Water(g, amount);
    g.stress_points = g.stress_points > 800u ? g.stress_points - 800u : 0u;
}

struct HarvestResult {
    bool harvestable = false;
    std::uint8_t quality = 0; // 0..100 (husbandry score)
    float yield = 0.0f;       // crop yield (genome base_yield x quality x ripeness)
    int seeds = 0;            // seeds returned (for replanting / breeding)
};

// Harvest: only Mature+ plants yield. Yield scales with the genome's base yield x
// quality x ripeness (Fruiting is best). Pure read — the caller decides whether to
// remove the entity (annual) or reset it to Mature (perennial regrowth).
inline HarvestResult Harvest(const Comp::PlantGrowthComponent& g,
                             const Comp::PlantGenomeComponent& genome) {
    HarvestResult r;
    const std::uint8_t mature = static_cast<std::uint8_t>(Comp::PlantStage::Mature);
    if (g.stage < mature) return r; // not ready
    r.harvestable = true;
    r.quality = g.quality;
    const PlantPhenotype ph = ExpressGenome(genome);
    const float ripe = static_cast<float>(g.stage - mature + 1) / 3.0f; // Mature..Fruiting -> ~0.33..1.0
    r.yield = ph.base_yield * (static_cast<float>(g.quality) / 100.0f) * ripe;
    r.seeds = 1 + static_cast<int>(r.yield * 3.0f);
    return r;
}

// Cross two harvested parents into a child seed-genome (reuses BreedPlants ->
// blend-crossover + Gaussian mutate). Deterministic from the supplied rng.
inline Comp::PlantGenomeComponent CrossBreed(const Comp::PlantGenomeComponent& a,
                                             const Comp::PlantGenomeComponent& b,
                                             luminumbra::core::DeterministicRng& rng) {
    return BreedPlants(a, b, rng);
}

} // namespace luminumbra::foliage
