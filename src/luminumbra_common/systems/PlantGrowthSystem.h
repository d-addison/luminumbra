#pragma once

//  living-world foliage system — phenotype expansion, the deterministic
// per-tick GROWTH update, and breeding. The genome (components/PlantComponents.h)
// reuses the creature GA operators (ai/Evolution.h); growth is driven by the
// ATMOSPHERIC environment (temperature / moisture / light / soil) modulated by the
// genome's tolerances, accruing DST-style stress that lowers harvest quality.
//
// DETERMINISM: integer/fixed-point state, id-ordered traversal, no wall-clock, no
// libm transcendentals. The environment is supplied by the caller as a sampler
// functor so this header stays dependency-light + unit-testable (the real tick
// binds it to WeatherSystem + SHIELD_WorldSystem; a test binds a pure function).

#include "../ai/Evolution.h"
#include "../components/CoreComponents.h"
#include "../components/DiseaseComponents.h" // blight saps growth (opt-in PlantHealthComponent)
#include "../components/PlantComponents.h"
#include "../core/DeterministicRng.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace luminumbra::foliage {

// The component types live in the capital-L `Luminumbra::Components` namespace
// (the engine mixes `luminumbra::` for ai/core and `Luminumbra::` for components).
namespace Comp = ::Luminumbra::Components;

// Seed-stream offset for plant genomes/mutation (wind+11, weather+12/13, aether+14
// taken; plants claim +15 in the stable subsystem allocation.
inline constexpr std::uint64_t kPlantSeedOffset = 15;

// Expanded phenotype: real growth parameters from the normalized genome. PURE
// function of the genome (deterministic, no env here — env enters at the tick).
struct PlantPhenotype {
    float growth_per_tick; // fixed-point growth milli-units/tick at ideal conditions
    float max_scale;       // mature visual scale (consumed by the renderer)
    float ideal_temp;      // preferred normalized temperature [0,1]
    float drought_tol;     // [0,1]
    float cold_tol;        // [0,1]
    float heat_tol;        // [0,1]
    float leaf_density;    // [0,1] (visual canopy fullness)
    float base_yield;      // harvest yield baseline
};

inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
inline float fabs_det(float v) {
    return v < 0.0f ? -v : v;
} // libm-free abs

inline PlantPhenotype ExpressGenome(const Comp::PlantGenomeComponent& g) {
    using G = Comp::PlantGene;
    const float hardiness = g.gene(G::Hardiness);
    PlantPhenotype p;
    p.growth_per_tick = 4.0f + g.gene(G::GrowthRate) * 16.0f; // 4..20 milli/tick
    p.max_scale = 0.6f + g.gene(G::MaxScale) * 1.8f;          // 0.6..2.4
    //  (season): preferred temperature from the heat/cold tolerance balance — a heat-tolerant
    // genome likes it WARM (ideal -> ~0.9, thrives in summer), a cold-tolerant one likes it COOL
    // (ideal -> ~0.1, thrives in winter). Combined with the seasonal env-temperature swing,
    // different plants thrive in different seasons (emergent, genome-driven). Pure ->
    // deterministic.
    p.ideal_temp = clamp01(0.5f + 0.4f * (g.gene(G::HeatTolerance) - g.gene(G::ColdTolerance)));
    p.drought_tol = clamp01(g.gene(G::DroughtTolerance) * 0.7f + hardiness * 0.3f);
    p.cold_tol = clamp01(g.gene(G::ColdTolerance) * 0.7f + hardiness * 0.3f);
    p.heat_tol = clamp01(g.gene(G::HeatTolerance) * 0.7f + hardiness * 0.3f);
    p.leaf_density = g.gene(G::LeafDensity);
    p.base_yield = 0.4f + g.gene(G::Yield) * 0.6f;
    return p;
}

// Per-tick environment at a plant's location, normalized [0,1]. Supplied by the
// caller (real tick: WeatherSystem temperature/precip + SHIELD_WorldSystem soil +
// time-of-day/season light).
struct PlantEnvSample {
    float temperature = 0.5f;  // 0 cold, 1 hot
    float moisture = 0.5f;     // rain + soil water
    float light = 0.7f;        // sun exposure / season
    float soil_quality = 0.6f; // biome surface material suitability
};

// Environmental SUITABILITY in [0,1]: how favourable the cell is for THIS plant.
// 1 = ideal (max growth, no stress); 0 = lethal (no growth, max stress). The
// genome's tolerances raise the floor for the matching stress axis (G x E).
inline float Suitability(const PlantPhenotype& ph, const PlantEnvSample& env) {
    const float tempDist = fabs_det(env.temperature - ph.ideal_temp);
    const float tempTol = (env.temperature < ph.ideal_temp) ? ph.cold_tol : ph.heat_tol;
    const float tempOk = clamp01(1.0f - tempDist * (2.0f - tempTol));
    const float moistOk = clamp01(env.moisture + ph.drought_tol * 0.5f);
    const float lightOk = clamp01(0.3f + env.light * 0.7f);
    const float soilOk = clamp01(0.4f + env.soil_quality * 0.6f);
    return clamp01(tempOk * moistOk * lightOk * soilOk);
}

// how much a FULLY infected plant (infection == 1000) loses off its growth
// suitability. Scales linearly with infection load; a healthy plant (0) is unaffected, so this is
// byte-identical for any roster whose plants carry no PlantHealthComponent.
inline constexpr float kBlightSuitPenalty = 0.85f;

// Fixed-point growth thresholds (milli-units) to advance OUT of each stage.
inline std::uint32_t StageThreshold(std::uint8_t stage) {
    static const std::array<std::uint32_t, Comp::kPlantStageCount> kThresh = {
        1000u, 3000u, 7000u, 12000u, 18000u, 0xFFFFFFFFu /* Fruiting: terminal */};
    return stage < Comp::kPlantStageCount ? kThresh[stage] : 0xFFFFFFFFu;
}

struct PlantGrowthStats {
    int updated = 0;
    int advanced = 0;
};

using EnvSampler = std::function<PlantEnvSample(const Comp::TransformComponent&)>;

// Advance every PlantTag plant by ONE fixed tick. Deterministic: entities are
// visited in ascending-id order, all accumulation is integer, env is a pure
// function of position. Returns telemetry (also useful for the world sub-hash).
inline PlantGrowthStats RunPlantGrowthSystemOnTick(entt::registry& reg,
                                                   std::uint64_t current_tick,
                                                   const EnvSampler& sample_env) {
    PlantGrowthStats stats;
    auto view = reg.view<Comp::PlantTag,
                         Comp::PlantGrowthComponent,
                         const Comp::PlantGenomeComponent,
                         const Comp::TransformComponent>();
    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end());

    for (auto e : ents) {
        auto& growth = view.get<Comp::PlantGrowthComponent>(e);
        const auto& genome = view.get<const Comp::PlantGenomeComponent>(e);
        const auto& tf = view.get<const Comp::TransformComponent>(e);

        const PlantPhenotype ph = ExpressGenome(genome);
        const PlantEnvSample env = sample_env(tf);
        // FARMING husbandry: tending (water/fertilize) lifts the cell's suitability
        // and is consumed slowly, so the player must keep tending (DST-style).
        const float tendBonus = static_cast<float>(growth.tended) * (1.0f / 255.0f) * 0.45f;
        float suit = clamp01(Suitability(ph, env) + tendBonus);
        if (growth.tended > 0)
            --growth.tended; // wears off (deterministic decay)

        // active disease saps growth. Infection is fixed-point milli (0..1000); a fully
        // infected plant loses kBlightSuitPenalty of its suitability. Opt-in via
        // PlantHealthComponent (no health component -> no penalty -> byte-identical for health-free
        // plant rosters).
        if (const auto* health = reg.try_get<const Comp::PlantHealthComponent>(e)) {
            const float infFrac =
                static_cast<float>(health->infection) / static_cast<float>(Comp::kDiseaseUnitScale);
            suit = clamp01(suit * (1.0f - kBlightSuitPenalty * infFrac));
        }

        // Fixed-point growth increment (truncated; deterministic). Poor cells grow
        // slower AND accrue stress (the DST husbandry model: stress -> low quality).
        const std::uint32_t inc = static_cast<std::uint32_t>(ph.growth_per_tick * suit);
        const std::uint32_t stress = static_cast<std::uint32_t>((1.0f - suit) * 6.0f);
        growth.growth_points += inc;
        growth.stress_points += stress;

        // Advance through any thresholds crossed this tick (Fruiting is terminal).
        while (growth.stage + 1u < Comp::kPlantStageCount &&
               growth.growth_points >= StageThreshold(growth.stage)) {
            ++growth.stage;
            ++stats.advanced;
        }

        // Quality is the inverse of accumulated stress (0..100), DST-style.
        const int q = 100 - static_cast<int>(growth.stress_points / 50u);
        growth.quality = static_cast<std::uint8_t>(q < 0 ? 0 : q);
        growth.last_tick = current_tick;
        ++stats.updated;
    }
    return stats;
}

// --- Breeding & seeding (reuse the creature GA operators) ---

// A fresh wild/seed genome: each gene a deterministic uniform draw.
inline Comp::PlantGenomeComponent RandomGenome(luminumbra::core::DeterministicRng& rng) {
    Comp::PlantGenomeComponent g;
    for (auto& gene : g.genes)
        gene = rng.next_unit();
    return g;
}

// Cross two parent genomes: blend-crossover + Gaussian mutate (the SAME operators
// the creature evolution uses), deterministic from the supplied seeded rng.
inline Comp::PlantGenomeComponent BreedPlants(const Comp::PlantGenomeComponent& a,
                                              const Comp::PlantGenomeComponent& b,
                                              luminumbra::core::DeterministicRng& rng,
                                              float mutation_frac = 0.06f) {
    const std::vector<float> va(a.genes.begin(), a.genes.end());
    const std::vector<float> vb(b.genes.begin(), b.genes.end());
    std::vector<float> child = luminumbra::ai::BlendCrossover(va, vb, rng);
    const std::vector<luminumbra::ai::GeneBound> bounds(child.size(),
                                                        luminumbra::ai::GeneBound{0.0f, 1.0f});
    luminumbra::ai::GaussianMutate(child, bounds, mutation_frac, rng);
    Comp::PlantGenomeComponent out;
    for (std::size_t i = 0; i < out.genes.size() && i < child.size(); ++i)
        out.genes[i] = child[i];
    return out;
}

} // namespace luminumbra::foliage
