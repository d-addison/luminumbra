#pragma once

//  living-world foliage system — the FARMING verbs. Deterministic sim
// operations that drive the cultivate -> grow -> tend -> harvest -> breed loop on
// top of the genome (PlantComponents.h) + growth (PlantGrowthSystem.h). The growth
// itself is the env-driven tick; these are the player/agent ACTIONS. Pure +
// integer where it matters (geometry is visual-only; this is sim truth).

#include "../components/CropLifecycleComponents.h" // CropLifecycleComponent (perennial/lifespan stamp)
#include "../components/GrazeableComponent.h"      // edible-by-herbivores opt-in
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h" // PollinationTag/Component — cross-pollination opt-in
#include "../components/SoilComponents.h" // SoilFeederComponent — soil-nutrient draw opt-in
#include "../core/DeterministicRng.h"
#include "../foliage/SpeciesRegistry.h" // SpeciesTemplate, SampleGenome, SpeciesId16
#include "PlantGrowthSystem.h"          // ExpressGenome, BreedPlants, Comp alias, namespace

#include <cstdint>

namespace luminumbra::foliage {
// `Comp` (=::Luminumbra::Components) is already aliased in PlantGrowthSystem.h.

// Plant a seed: spawn a PlantTag plant at the Seed stage with the given genome.
// The growth system (once ticking) advances it from here.
inline entt::entity PlantSeed(entt::registry& reg,
                              const ::Luminumbra::Vec3& pos,
                              const Comp::PlantGenomeComponent& genome,
                              std::uint16_t species_id,
                              std::uint64_t tick) {
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

// Spawn a plant FROM A SPECIES TEMPLATE: sample an in-bounds, heritable genome from the species'
// per-gene ranges (seeded), plant the seed, and stamp the crop lifecycle (annual/perennial +
// lifespan) so the generational loop (CropLifecycleSystem) applies. This is the single wiring point
// between the data-driven SpeciesRegistry and the live sim. Deterministic from `rng` + `tick`.
// species_id is the stable hash of the template id (round-trips through persistence + the plant
// sub-hash). Geometry stays visual-only; world_hash is unaffected until this is actually called
// (plants are opt-in via PlantTag).
inline entt::entity MakePlantFromSpecies(entt::registry& reg,
                                         const ::Luminumbra::Vec3& pos,
                                         const SpeciesTemplate& tmpl,
                                         luminumbra::core::DeterministicRng& rng,
                                         std::uint64_t tick) {
    const std::uint16_t sid = SpeciesId16(tmpl.id);
    const Comp::PlantGenomeComponent genome = SpeciesRegistry::SampleGenome(tmpl, rng);
    const entt::entity e = PlantSeed(reg, pos, genome, sid, tick);
    auto& cl = reg.emplace<Comp::CropLifecycleComponent>(e);
    cl.perennial = tmpl.perennial;
    cl.lifespan_ticks = tmpl.lifespan_ticks;
    cl.species_id = sid;
    // Cross-pollination opt-in: a real plant DRIFTS genetically over seasons — once it flowers it
    // donates pollen to (and receives from) neighbours, and the cross seeds the next generation
    // (PollinationSystem -> next_genome -> CropLifecycleSystem germination). Without this opt-in
    // the pollination tick skips the plant and germination falls back to a self copy (the loop runs
    // but never drifts). Additive: no plants -> both ticks are no-ops, canonical world_hash
    // unchanged.
    reg.emplace<Comp::PollinationTag>(e);
    reg.emplace<Comp::PollinationComponent>(e);
    // Soil-feeder opt-in: a real plant DRAWS nutrient from its cell (scaled by growth stage), so a
    // dense monoculture depletes the shared soil and self-limits (the growth tick reads NutrientAt
    // back into suitability) — the player must rotate / fertilise. Additive: no feeders -> the soil
    // tick is a no-op and the grid is never touched, so the canonical world_hash is unchanged.
    reg.emplace<Comp::SoilFeederComponent>(e);
    //  ( live feeding): grazeable opt-in — a real plant is EDIBLE. Nearby
    // herbivores draw its biomass down and sate hunger through kFeedPerGraze
    // (WildlifeFoliageSystem, previously wired-but-dormant: nothing emplaced this);
    // left alone it regrows. Same additive discipline: no plants (the canonical
    // smoke) = byte-identical; planted worlds re-pin the populated golden once.
    reg.emplace<Comp::GrazeableComponent>(e);
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
    if (g.stage < mature)
        return r; // not ready
    r.harvestable = true;
    r.quality = g.quality;
    const PlantPhenotype ph = ExpressGenome(genome);
    const float ripe =
        static_cast<float>(g.stage - mature + 1) / 3.0f; // Mature..Fruiting -> ~0.33..1.0
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

//  a small, deterministic PLAYER-FACING farming controller. Picks the nearest
// plant to a world position (the player's aim point) and applies a verb, tracking a seed + harvest
// inventory. CLIENT-AGNOSTIC (no input / render / GL deps) so it is unit-testable; the client binds
// InputActions to these methods and reads the counters for the HUD. The verbs themselves are the
// pure FarmingSystem functions above, so the sim stays integer/deterministic.
struct FarmingController {
    int seeds = 5;    // plantable seeds on hand
    int harvests = 0; // successful harvests (telemetry / HUD)
    float total_yield = 0.0f;

    // Nearest PlantTag entity within `reach` metres (horizontal), or entt::null. id-ordered
    // tiebreak.
    [[nodiscard]] static entt::entity
    NearestPlant(const entt::registry& reg, const ::Luminumbra::Vec3& pos, float reach) {
        entt::entity best = entt::null;
        float bestD = reach * reach; // only plants within reach qualify
        auto view = reg.view<const Comp::PlantTag, const Comp::TransformComponent>();
        for (const entt::entity e : view) {
            const auto& tf = view.get<const Comp::TransformComponent>(e);
            const float dx = tf.position.x - pos.x, dz = tf.position.z - pos.z;
            const float d = dx * dx + dz * dz;
            if (d > bestD)
                continue;
            if (best == entt::null || d < bestD || (d == bestD && e < best)) {
                bestD = d;
                best = e;
            }
        }
        return best;
    }

    // Plant a seed from a species template (consumes one seed). entt::null if out of seeds.
    entt::entity Seed(entt::registry& reg,
                      const ::Luminumbra::Vec3& pos,
                      const SpeciesTemplate& tmpl,
                      luminumbra::core::DeterministicRng& rng,
                      std::uint64_t tick) {
        if (seeds <= 0)
            return entt::null;
        --seeds;
        return MakePlantFromSpecies(reg, pos, tmpl, rng, tick);
    }

    bool Water(entt::registry& reg, entt::entity e) {
        if (!reg.valid(e) || !reg.all_of<Comp::PlantGrowthComponent>(e))
            return false;
        luminumbra::foliage::Water(reg.get<Comp::PlantGrowthComponent>(e));
        return true;
    }
    bool Fertilize(entt::registry& reg, entt::entity e) {
        if (!reg.valid(e) || !reg.all_of<Comp::PlantGrowthComponent>(e))
            return false;
        luminumbra::foliage::Fertilize(reg.get<Comp::PlantGrowthComponent>(e));
        return true;
    }

    // Harvest a mature plant: bank yield + returned seeds, then remove the plant (annual). Returns
    // the HarvestResult (harvestable=false if the target is not ready / invalid).
    HarvestResult Harvest(entt::registry& reg, entt::entity e) {
        HarvestResult r;
        if (!reg.valid(e) || !reg.all_of<Comp::PlantGrowthComponent, Comp::PlantGenomeComponent>(e))
            return r;
        r = luminumbra::foliage::Harvest(reg.get<Comp::PlantGrowthComponent>(e),
                                         reg.get<Comp::PlantGenomeComponent>(e));
        if (!r.harvestable)
            return r;
        ++harvests;
        total_yield += r.yield;
        seeds += r.seeds;
        // Perennial (e.g. a promoted wild tree / oak) REGROWS: reset to a vegetative stage instead
        // of dying, bumping the generation. Annual is removed. (CropLifecycleSystem owns the
        // senescence loop; this is the player-harvest counterpart so harvesting a perennial doesn't
        // delete it.)
        auto* cl = reg.try_get<Comp::CropLifecycleComponent>(e);
        if (cl != nullptr && cl->perennial) {
            auto& g = reg.get<Comp::PlantGrowthComponent>(e);
            g.stage = static_cast<std::uint8_t>(Comp::PlantStage::Sprout);
            g.growth_points = 0u;
            ++cl->generations;
            cl->ripe_ticks = 0u;
        } else {
            reg.destroy(e); // annual: consumed
        }
        return r;
    }
};

} // namespace luminumbra::foliage
