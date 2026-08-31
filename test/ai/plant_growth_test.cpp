//  living-world foliage system — deterministic plant growth + breeding
// coverage. The sim-side plant state is integer/fixed-point and must be replay-
// exact; the genome reuses the creature GA operators. World_hash-neutral until a
// PlantGrowthSystem is wired into the tick (plants are opt-in via PlantTag).

#include "gtest/gtest.h"

#include "luminumbra_common/components/DiseaseComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/SoilComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/systems/IrrigationSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/SoilNutrientSystem.h"

#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
using luminumbra::core::DeterministicRng;

// A pure, position-dependent environment so growth is deterministic AND varies by
// location (so the replay test exercises per-plant divergence).
F::PlantEnvSample EnvAt(const C::TransformComponent& tf) {
    F::PlantEnvSample e;
    // Map x into [0,1]-ish bands; deterministic, no RNG.
    const float t = tf.position.x * 0.01f;
    e.temperature = F::clamp01(0.5f + 0.2f * t);
    e.moisture = F::clamp01(0.5f + 0.1f * t);
    e.light = 0.8f;
    e.soil_quality = 0.6f;
    return e;
}

// Build a deterministic registry of N plants at spaced positions, seeded genomes.
void SpawnPlants(entt::registry& reg, int n, std::uint64_t seed) {
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, seed);
    for (int i = 0; i < n; ++i) {
        const auto e = reg.create();
        auto& tf = reg.emplace<C::TransformComponent>(e);
        tf.position = Luminumbra::Vec3(static_cast<float>(i * 7), 0.0f, static_cast<float>(i * 3));
        reg.emplace<C::PlantTag>(e);
        reg.emplace<C::PlantGenomeComponent>(e, F::RandomGenome(rng));
        auto& g = reg.emplace<C::PlantGrowthComponent>(e);
        g.species_id = static_cast<std::uint16_t>(i % 3);
        g.planted_tick = 0;
    }
}

// A cheap order-independent state digest of all plants (stage/quality/points).
std::uint64_t PlantStateDigest(entt::registry& reg) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto view = reg.view<C::PlantTag, const C::PlantGrowthComponent>();
    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end());
    for (auto e : ents) {
        const auto& g = view.get<const C::PlantGrowthComponent>(e);
        for (std::uint64_t v : {std::uint64_t(g.stage),
                                std::uint64_t(g.quality),
                                std::uint64_t(g.growth_points),
                                std::uint64_t(g.stress_points)}) {
            h = (h ^ v) * 0x100000001b3ull;
        }
    }
    return h;
}
} // namespace

TEST(PlantGenome, RandomGenomeIsReproducibleAndInBounds) {
    DeterministicRng a = DeterministicRng::seeded(F::kPlantSeedOffset, 99);
    DeterministicRng b = DeterministicRng::seeded(F::kPlantSeedOffset, 99);
    const auto ga = F::RandomGenome(a);
    const auto gb = F::RandomGenome(b);
    EXPECT_EQ(ga.genes, gb.genes); // same seed -> identical genome
    for (float v : ga.genes) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LT(v, 1.0f);
    }
}

TEST(PlantGenome, BreedingIsReproducibleAndStaysInBounds) {
    DeterministicRng src = DeterministicRng::seeded(F::kPlantSeedOffset, 7);
    const auto pa = F::RandomGenome(src);
    const auto pb = F::RandomGenome(src);
    auto breed = [&]() {
        DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 1234, 5);
        return F::BreedPlants(pa, pb, rng);
    };
    const auto c1 = breed();
    const auto c2 = breed();
    EXPECT_EQ(c1.genes, c2.genes); // same parents + seed -> identical child
    for (float v : c1.genes) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
}

TEST(PlantGrowth, DeterministicReplay) {
    auto run = []() {
        entt::registry reg;
        SpawnPlants(reg, 16, 2024);
        for (std::uint64_t tick = 1; tick <= 400; ++tick) {
            F::RunPlantGrowthSystemOnTick(reg, tick, EnvAt);
        }
        return PlantStateDigest(reg);
    };
    EXPECT_EQ(run(), run()); // identical inputs + tick sequence -> identical state
}

// Grow one identical-genome plant under a fixed env for 300 ticks, return its state.
static C::PlantGrowthComponent GrowOnePlant(const C::PlantGenomeComponent& genome,
                                            const F::PlantEnvSample& env) {
    entt::registry reg;
    const auto e = reg.create();
    reg.emplace<C::TransformComponent>(e).position = Luminumbra::Vec3(0, 0, 0);
    reg.emplace<C::PlantTag>(e);
    reg.emplace<C::PlantGenomeComponent>(e, genome);
    reg.emplace<C::PlantGrowthComponent>(e);
    F::EnvSampler sampler = [&](const C::TransformComponent&) {
        return env;
    };
    for (std::uint64_t tick = 1; tick <= 300; ++tick) {
        F::RunPlantGrowthSystemOnTick(reg, tick, sampler);
    }
    return reg.get<C::PlantGrowthComponent>(e);
}

TEST(PlantGrowth, GrowsThroughStagesAndFavourableEnvironmentGrowsFaster) {
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 55);
    const auto genome = F::RandomGenome(rng);

    const auto good = GrowOnePlant(genome, F::PlantEnvSample{0.5f, 0.9f, 0.95f, 0.9f});  // ideal
    const auto poor = GrowOnePlant(genome, F::PlantEnvSample{0.95f, 0.05f, 0.2f, 0.2f}); // harsh

    EXPECT_GT(good.growth_points, poor.growth_points); // favourable env grows faster
    EXPECT_GT(good.stage, 0u);                         // advanced past Seed
    EXPECT_GE(good.quality, poor.quality);             // harsh env accrues stress -> lower quality
    EXPECT_GT(good.growth_points, 0u);
}

//  (season): ideal_temp derives from the genome's heat/cold tolerance, so a heat-tolerant plant
// thrives in a WARM env and a cold-tolerant plant in a COOL env — the basis of season-driven growth
// (different plants peak in different seasons when the env temperature swings annually).
TEST(PlantGrowth, HeatVsColdGenomePreferDifferentTemperatures) {
    using G = C::PlantGene;
    auto makeGenome = [](float heat, float cold) {
        C::PlantGenomeComponent g;
        g.genes.fill(0.5f);
        g.genes[static_cast<std::size_t>(G::HeatTolerance)] = heat;
        g.genes[static_cast<std::size_t>(G::ColdTolerance)] = cold;
        return g;
    };
    const auto heatLover = makeGenome(0.9f, 0.1f); // ideal_temp -> warm (~0.82)
    const auto coldLover = makeGenome(0.1f, 0.9f); // ideal_temp -> cool (~0.18)
    EXPECT_GT(F::ExpressGenome(heatLover).ideal_temp, F::ExpressGenome(coldLover).ideal_temp)
        << "a heat-tolerant genome prefers a warmer temperature";

    const F::PlantEnvSample warm{0.85f, 0.7f, 0.8f, 0.7f};
    const F::PlantEnvSample cool{0.15f, 0.7f, 0.8f, 0.7f};
    EXPECT_GT(GrowOnePlant(heatLover, warm).growth_points,
              GrowOnePlant(heatLover, cool).growth_points)
        << "the heat-lover grows more in warmth (summer)";
    EXPECT_GT(GrowOnePlant(coldLover, cool).growth_points,
              GrowOnePlant(coldLover, warm).growth_points)
        << "the cold-lover grows more in the cool (winter)";
}

TEST(Farming, WateringBoostsGrowthInHarshEnv) {
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 77);
    const auto genome = F::RandomGenome(rng);
    entt::registry reg;
    const auto watered = F::PlantSeed(reg, Luminumbra::Vec3(0, 0, 0), genome, 0, 0);
    const auto dry = F::PlantSeed(reg, Luminumbra::Vec3(5, 0, 0), genome, 0, 0);
    F::EnvSampler harsh = [](const C::TransformComponent&) {
        return F::PlantEnvSample{0.5f, 0.10f, 0.6f, 0.4f};
    }; // dry
    for (std::uint64_t t = 1; t <= 300; ++t) {
        F::Water(reg.get<C::PlantGrowthComponent>(watered), 200); // tend the watered one
        F::RunPlantGrowthSystemOnTick(reg, t, harsh);
    }
    const auto& w = reg.get<C::PlantGrowthComponent>(watered);
    const auto& d = reg.get<C::PlantGrowthComponent>(dry);
    EXPECT_GT(w.growth_points, d.growth_points); // watering boosts growth in a dry cell
    EXPECT_GE(w.quality, d.quality);             // and eases stress -> higher quality
}

TEST(Farming, PlantGrowHarvestBreedLoop) {
    entt::registry reg;
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 88);
    const auto gA = F::RandomGenome(rng);
    const auto gB = F::RandomGenome(rng);
    const auto a = F::PlantSeed(reg, Luminumbra::Vec3(0, 0, 0), gA, 1, 0);
    const auto b = F::PlantSeed(reg, Luminumbra::Vec3(3, 0, 0), gB, 1, 0);
    F::EnvSampler ideal = [](const C::TransformComponent&) {
        return F::PlantEnvSample{0.5f, 0.95f, 0.95f, 0.95f};
    };
    for (std::uint64_t t = 1; t <= 2500; ++t) {
        F::Water(reg.get<C::PlantGrowthComponent>(a), 120);
        F::Water(reg.get<C::PlantGrowthComponent>(b), 120);
        F::RunPlantGrowthSystemOnTick(reg, t, ideal);
    }
    const auto& ga = reg.get<C::PlantGrowthComponent>(a);
    EXPECT_GE(ga.stage, static_cast<std::uint8_t>(C::PlantStage::Mature)); // grew to harvestable

    const auto h = F::Harvest(ga, reg.get<C::PlantGenomeComponent>(a));
    EXPECT_TRUE(h.harvestable);
    EXPECT_GT(h.yield, 0.0f);
    EXPECT_GT(h.seeds, 0);

    // A seedling (Seed stage) is NOT harvestable.
    const auto seedling = F::PlantSeed(reg, Luminumbra::Vec3(9, 0, 0), gA, 1, 0);
    EXPECT_FALSE(F::Harvest(reg.get<C::PlantGrowthComponent>(seedling),
                            reg.get<C::PlantGenomeComponent>(seedling))
                     .harvestable);

    // Cross the two grown parents -> an in-bounds child seed-genome.
    DeterministicRng brng = DeterministicRng::seeded(F::kPlantSeedOffset, 999, 2);
    const auto child = F::CrossBreed(gA, gB, brng);
    for (float v : child.genes) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
}

// ---: env-loop couplings — blight slows growth, monoculture starves the soil,
// irrigation moistens a cell. contract for the closed environment loop. ---

// Grow one plant under a fixed env with an optional infection load (PlantHealthComponent).
static C::PlantGrowthComponent GrowOnePlantInfected(const C::PlantGenomeComponent& genome,
                                                    const F::PlantEnvSample& env,
                                                    std::uint16_t infection) {
    entt::registry reg;
    const auto e = reg.create();
    reg.emplace<C::TransformComponent>(e).position = Luminumbra::Vec3(0, 0, 0);
    reg.emplace<C::PlantTag>(e);
    reg.emplace<C::PlantGenomeComponent>(e, genome);
    reg.emplace<C::PlantGrowthComponent>(e);
    reg.emplace<C::PlantHealthComponent>(e).infection = infection;
    F::EnvSampler sampler = [&](const C::TransformComponent&) {
        return env;
    };
    for (std::uint64_t t = 1; t <= 300; ++t)
        F::RunPlantGrowthSystemOnTick(reg, t, sampler);
    return reg.get<C::PlantGrowthComponent>(e);
}

TEST(PlantGrowth, BlightSlowsGrowth) {
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 41);
    const auto genome = F::RandomGenome(rng);
    const F::PlantEnvSample env{0.5f, 0.9f, 0.95f, 0.9f};          // otherwise-ideal env
    const auto healthy = GrowOnePlantInfected(genome, env, 0);     // no infection
    const auto blighted = GrowOnePlantInfected(genome, env, 1000); // fully infected
    EXPECT_GT(healthy.growth_points, blighted.growth_points) << "blight saps growth";
    EXPECT_GE(healthy.stage, blighted.stage);
    // A healthy plant (infection 0) is byte-identical to one with no health component at all.
    F::EnvSampler sampler = [&](const C::TransformComponent&) {
        return env;
    };
    const auto noHealth = GrowOnePlant(genome, env);
    EXPECT_EQ(healthy.growth_points, noHealth.growth_points);
    EXPECT_EQ(healthy.stress_points, noHealth.stress_points);
}

TEST(PlantGrowth, MonocultureStarvesSharedSoil) {
    // A CROWDED cell (a monoculture of mature feeders) drains its soil; a far UNFED cell stays at
    // the soil baseline. Growth reading the depleted soil grows measurably less -> monoculture
    // starves.
    F::SoilGrid soil(16, 16);
    entt::registry sreg;
    auto feeder = [&](float x, float z) {
        auto e = sreg.create();
        sreg.emplace<C::TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
        sreg.emplace<C::PlantTag>(e);
        sreg.emplace<C::SoilFeederComponent>(e);
        sreg.emplace<C::PlantGrowthComponent>(e).stage =
            static_cast<std::uint8_t>(C::PlantStage::Fruiting); // feeds hardest
    };
    // A dense monoculture (eight fruiting feeders) on one cell drains it far below the baseline.
    for (int i = 0; i < 8; ++i)
        feeder(0.5f, 0.5f);
    for (int t = 0; t < 400; ++t)
        F::RunSoilNutrientOnTick(sreg, soil, 0.0f, 0.0f, 1.0f);
    const int nutCrowded = F::NutrientAt(soil, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f);
    const int nutUnfed =
        F::NutrientAt(soil, 12.5f, 12.5f, 0.0f, 0.0f, 1.0f); // no feeders -> baseline
    EXPECT_LT(nutCrowded, nutUnfed)
        << "the monoculture depletes its shared cell below the baseline";

    // Coupling: growth reading the (now frozen) soil grows LESS on the starved cell. Soil is the
    // limiting factor here, so the nutrient gap shows through as a growth gap.
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 63);
    const auto genome = F::RandomGenome(rng);
    auto growAt = [&](float x, float z) {
        entt::registry reg;
        const auto e = reg.create();
        reg.emplace<C::TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
        reg.emplace<C::PlantTag>(e);
        reg.emplace<C::PlantGenomeComponent>(e, genome);
        reg.emplace<C::PlantGrowthComponent>(e);
        F::EnvSampler s = [&](const C::TransformComponent& tf) {
            F::PlantEnvSample env{0.5f, 0.5f, 0.7f, 0.0f};
            const int nut = F::NutrientAt(soil, tf.position.x, tf.position.z, 0.0f, 0.0f, 1.0f);
            // Normalise by kSoilBaseline (the 1.0 level), NOT 1000 — soil-limited growth.
            env.soil_quality =
                F::clamp01(static_cast<float>(nut) / static_cast<float>(F::kSoilBaseline));
            return env;
        };
        for (std::uint64_t t = 1; t <= 400; ++t)
            F::RunPlantGrowthSystemOnTick(reg, t, s);
        return reg.get<C::PlantGrowthComponent>(e).growth_points;
    };
    EXPECT_LT(growAt(0.5f, 0.5f), growAt(12.5f, 12.5f))
        << "the monoculture starves itself -> slower growth";
}

TEST(PlantGrowth, IrrigationMoistensCellForGrowth) {
    // A water source raises moisture in its cell above a far-off dry cell.
    F::IrrigationGrid grid(16, 16);
    entt::registry wreg;
    auto src = wreg.create();
    wreg.emplace<C::TransformComponent>(src).position = Luminumbra::Vec3(0.5f, 0.0f, 0.5f);
    wreg.emplace<C::WaterSourceComponent>(src);
    for (int t = 0; t < 60; ++t)
        F::RunIrrigationOnTick(wreg, grid, 0.0f, 0.0f, 1.0f);
    const int moistNear = F::MoistureAt(grid, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f);
    const int moistFar = F::MoistureAt(grid, 14.5f, 14.5f, 0.0f, 0.0f, 1.0f);
    EXPECT_GT(moistNear, moistFar) << "irrigation raises moisture near the water source";
}
