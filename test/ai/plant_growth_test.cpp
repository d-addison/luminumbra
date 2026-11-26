//  living-world foliage system — deterministic plant growth + breeding
// coverage. The sim-side plant state is integer/fixed-point and must be replay-
// exact; the genome reuses the creature GA operators. World_hash-neutral until a
// PlantGrowthSystem is wired into the tick (plants are opt-in via PlantTag).

#include "gtest/gtest.h"

#include "luminumbra_common/components/DiseaseComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/SoilComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/systems/IrrigationSystem.h"
#include "luminumbra_common/systems/PlantCoarse.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/SoilNutrientSystem.h"

#include <array>
#include <functional>
#include <map>
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

TEST(PlantGrowth, NoncopyableSamplerPreservesAdmissionOrderAndState) {
    entt::registry concrete;
    entt::registry erased;
    for (auto* registry : {&concrete, &erased}) {
        SpawnPlants(*registry, 6, 2024);
        registry->emplace<C::PlantTag>(registry->create());
        for (const auto entity : registry->view<C::PlantGrowthComponent>()) {
            const auto& transform = registry->get<C::TransformComponent>(entity);
            const int index = static_cast<int>(transform.position.x / 7.0f);
            registry->emplace<C::PlantRegionComponent>(
                entity,
                C::PlantRegionComponent{.persistent_id = static_cast<std::uint64_t>(6 - index),
                                        .owner_x = index % 2,
                                        .owner_z = 0,
                                        .region_stamp = 0});
            registry->get<C::PlantGrowthComponent>(entity).tended = 20;
        }
    }

    struct Sampler {
        explicit Sampler(std::vector<float>& positions)
            : visited(positions) {}
        Sampler(const Sampler&) = delete;
        Sampler& operator=(const Sampler&) = delete;
        explicit operator bool() const {
            return false;
        }
        F::PlantEnvSample operator()(const C::TransformComponent& transform) const {
            visited.push_back(transform.position.x);
            return EnvAt(transform);
        }
        std::vector<float>& visited;
    };
    std::vector<float> concrete_positions;
    const Sampler sample(concrete_positions);
    std::vector<float> erased_positions;
    const F::EnvSampler reference = [&](const C::TransformComponent& transform) {
        erased_positions.push_back(transform.position.x);
        return EnvAt(transform);
    };
    const std::array<Luminumbra::world::RegionKey, 1> due{{{0, 0}}};
    const Luminumbra::world::RegionConsumerGate concrete_gate(concrete, due);
    const Luminumbra::world::RegionConsumerGate erased_gate(erased, due);
    const auto actual = F::RunPlantGrowthSystemOnTick(concrete, 5, sample, concrete_gate);
    const auto expected = F::RunPlantGrowthSystemOnTick(erased, 5, reference, erased_gate);
    EXPECT_EQ(actual.updated, 3);
    EXPECT_EQ(actual.updated, expected.updated);
    EXPECT_EQ(actual.advanced, expected.advanced);
    EXPECT_EQ(concrete_positions, (std::vector<float>{0.0f, 14.0f, 28.0f}));
    EXPECT_EQ(concrete_positions, erased_positions);
    EXPECT_EQ(concrete_gate.owner_visits(), 7u);
    EXPECT_EQ(concrete_gate.owner_visits(), erased_gate.owner_visits());
    EXPECT_EQ(PlantStateDigest(concrete), PlantStateDigest(erased));
    for (const auto entity : concrete.view<C::PlantGrowthComponent>()) {
        const auto& growth = concrete.get<C::PlantGrowthComponent>(entity);
        const auto& identity = concrete.get<C::PlantRegionComponent>(entity);
        const bool admitted = identity.owner_x == 0;
        EXPECT_EQ(growth.last_tick, admitted ? 5u : 0u);
        EXPECT_EQ(growth.tended, admitted ? 19u : 20u);
        EXPECT_EQ(identity.region_stamp, admitted ? 5u : 0u);
    }
}

TEST(PlantGrowth, ConcreteEnvironmentMatchesErasedGrowthAndSaveBytesAcrossRegionSchedules) {
    entt::registry concrete;
    entt::registry erased;
    for (auto* registry : {&concrete, &erased}) {
        SpawnPlants(*registry, 16, 2024);
        for (const auto entity : registry->view<C::PlantGrowthComponent>()) {
            const auto index = static_cast<std::uint32_t>(entt::to_integral(entity));
            auto& growth = registry->get<C::PlantGrowthComponent>(entity);
            growth.tended = 20;
            growth.growth_points = 995;
            registry->emplace<C::PlantHealthComponent>(entity).infection =
                static_cast<std::uint16_t>((index % 3) * 500);
            registry->emplace<C::PlantRegionComponent>(
                entity,
                C::PlantRegionComponent{.persistent_id = index + 1,
                                        .owner_x = static_cast<std::int32_t>(index % 2),
                                        .owner_z = 0,
                                        .region_stamp = 0});
        }
    }
    const auto sample = [](const C::TransformComponent& transform) {
        return EnvAt(transform);
    };
    const F::EnvSampler reference = sample;
    for (std::uint64_t tick = 1; tick <= 64; ++tick) {
        SCOPED_TRACE(tick);
        std::vector<Luminumbra::world::RegionKey> due;
        if (tick % 4 != 0) {
            due.push_back({0, 0});
        }
        if (tick % 4 == 2) {
            due.push_back({1, 0});
        }
        const Luminumbra::world::RegionConsumerGate concrete_gate(concrete, due);
        const Luminumbra::world::RegionConsumerGate erased_gate(erased, due);
        const auto actual = F::RunPlantGrowthSystemOnTick(concrete, tick, sample, concrete_gate);
        const auto expected = F::RunPlantGrowthSystemOnTick(erased, tick, reference, erased_gate);
        EXPECT_EQ(actual.updated, expected.updated);
        EXPECT_EQ(actual.updated, static_cast<int>(due.size() * 8));
        EXPECT_EQ(actual.advanced, expected.advanced);
        EXPECT_EQ(PlantStateDigest(concrete), PlantStateDigest(erased));
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                      F::BuildPlantEntitySnapshot(concrete, true)),
                  Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                      F::BuildPlantEntitySnapshot(erased, true)));
    }
}

TEST(PlantGrowth, NullableSamplersThrowOnlyForAdmittedPlants) {
    const auto check = [](const auto& sample) {
        entt::registry registry;
        EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, 5, sample).updated, 0);
        SpawnPlants(registry, 1, 2024);
        const Luminumbra::world::RegionConsumerGate gate(registry, {});
        EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, 5, sample, gate).updated, 0);
        const auto before = PlantStateDigest(registry);
        EXPECT_THROW(F::RunPlantGrowthSystemOnTick(registry, 5, sample), std::bad_function_call);
        EXPECT_EQ(PlantStateDigest(registry), before);
        for (const auto entity : registry.view<C::PlantGrowthComponent>()) {
            EXPECT_EQ(registry.get<C::PlantGrowthComponent>(entity).last_tick, 0u);
        }
    };
    check(F::EnvSampler{});
    check(nullptr);
    check(static_cast<decltype(&EnvAt)>(nullptr));
    entt::registry registry;
    EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, 5, {}).updated, 0);
    SpawnPlants(registry, 1, 2024);
    EXPECT_THROW(F::RunPlantGrowthSystemOnTick(registry, 5, {}), std::bad_function_call);
    entt::registry reference;
    SpawnPlants(reference, 1, 2024);
    const F::EnvSampler erased = EnvAt;
    const auto expected = F::RunPlantGrowthSystemOnTick(reference, 5, erased);
    const auto actual = F::RunPlantGrowthSystemOnTick(registry, 5, &EnvAt);
    EXPECT_EQ(actual.updated, 1);
    EXPECT_EQ(actual.updated, expected.updated);
    EXPECT_EQ(actual.advanced, expected.advanced);
    EXPECT_EQ(PlantStateDigest(registry), PlantStateDigest(reference));
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

TEST(PlantCoarseGrowth, MatchesIndependentExistingSingleTickConsumer) {
    namespace C = Luminumbra::Components;
    using namespace luminumbra::foliage;
    const std::array windows{0u, 1u, 2u, 255u, 256u, 36000u};
    for (const auto elapsed : windows) {
        for (const auto tended : {0u, 1u, 255u}) {
            for (const auto initial : {0u, 999u, 17999u, 0xFFFFFFF0u}) {
                entt::registry reg;
                const auto e = reg.create();
                reg.emplace<C::PlantTag>(e);
                auto& genome = reg.emplace<C::PlantGenomeComponent>(e);
                genome.genes.fill(0.6f);
                auto& original = reg.emplace<C::PlantGrowthComponent>(e);
                original.growth_points = initial;
                original.stress_points = initial;
                original.tended = static_cast<std::uint8_t>(tended);
                reg.emplace<C::TransformComponent>(e);
                auto& health = reg.emplace<C::PlantHealthComponent>(e);
                health.infection = 437;
                auto bulk = original;
                const PlantEnvSample env{0.3f, 0.1f, 0.7f, 0.9f};
                std::uint32_t oracle_advanced = 0;
                for (std::uint32_t i = 0; i < elapsed; ++i)
                    oracle_advanced += static_cast<std::uint32_t>(
                        RunPlantGrowthSystemOnTick(reg, 100 + i, [&](const auto&) {
                            return env;
                        }).advanced);
                const auto result = AdvancePlantGrowthCoarse(
                    bulk, genome, env, health.infection, elapsed, elapsed ? 99 + elapsed : 0);
                EXPECT_EQ(result, oracle_advanced);
                EXPECT_EQ(bulk.growth_points, original.growth_points);
                EXPECT_EQ(bulk.stress_points, original.stress_points);
                EXPECT_EQ(bulk.stage, original.stage);
                EXPECT_EQ(bulk.quality, original.quality);
                EXPECT_EQ(bulk.tended, original.tended);
                EXPECT_EQ(bulk.last_tick, original.last_tick);
            }
        }
    }
}

TEST(PlantCoarseGrowth, RefusesBeyondDayBeforeChangingState) {
    using namespace luminumbra::foliage;
    Luminumbra::Components::PlantGrowthComponent growth;
    Luminumbra::Components::PlantGenomeComponent genome;
    EXPECT_THROW(AdvancePlantGrowthCoarse(growth, genome, {}, 0, 36001, 99), std::invalid_argument);
    EXPECT_EQ(growth.last_tick, 0u);
    EXPECT_EQ(growth.growth_points, 0u);
}

TEST(PlantCoarseGrowth, VaryingInputsStayWithinOneStageForCadencesThrough64Ticks) {
    // For coherent nonterminal growth state, every tick adds 0..20 points.
    // At k<=64 either trajectory spans at most 1280 points; adjacent stage
    // thresholds are >=2000 points apart. Therefore their stage difference is
    // <=1 even under an adversarial environment, infection or tending change.
    // This qualifies that range only, not arbitrarily longer configured cadence.
    for (const auto k : {1u, 4u, 16u, 64u})
        for (const auto initial : {0u, 999u, 2999u, 5999u, 9999u, 17999u})
            for (const auto tended : {0u, 255u}) {
                entt::registry reg;
                const auto e = reg.create();
                reg.emplace<C::PlantTag>(e);
                reg.emplace<C::TransformComponent>(e);
                auto& genome = reg.emplace<C::PlantGenomeComponent>(e);
                genome.genes.fill(.5f);
                genome.genes[static_cast<std::size_t>(C::PlantGene::GrowthRate)] = 1;
                auto& g = reg.emplace<C::PlantGrowthComponent>(e);
                g.growth_points = initial;
                while (g.stage + 1 < C::kPlantStageCount && initial >= F::StageThreshold(g.stage))
                    ++g.stage;
                g.tended = static_cast<std::uint8_t>(tended);
                reg.emplace<C::PlantHealthComponent>(e);
                auto coarse = g;
                F::PlantEnvSample current{.5f, .5f, 1, 1};
                for (std::uint32_t n = 1; n <= k; ++n) {
                    const bool harsh = n % 2 == 0;
                    reg.get<C::PlantHealthComponent>(e).infection = harsh ? 1000 : 0;
                    const F::PlantEnvSample sampled =
                        harsh ? F::PlantEnvSample{0, 0, 0, 0} : current;
                    F::RunPlantGrowthSystemOnTick(reg, n, [&](const auto&) { return sampled; });
                }
                F::AdvancePlantGrowthCoarse(coarse, genome, current, 0, k, k);
                EXPECT_LE(std::abs(static_cast<int>(coarse.stage) - static_cast<int>(g.stage)), 1);
            }
}

namespace {
void ExpectSamePlantGrowth(const C::PlantGrowthComponent& actual,
                           const C::PlantGrowthComponent& expected) {
    EXPECT_EQ(actual.species_id, expected.species_id);
    EXPECT_EQ(actual.stage, expected.stage);
    EXPECT_EQ(actual.quality, expected.quality);
    EXPECT_EQ(actual.growth_points, expected.growth_points);
    EXPECT_EQ(actual.stress_points, expected.stress_points);
    EXPECT_EQ(actual.tended, expected.tended);
    EXPECT_EQ(actual.planted_tick, expected.planted_tick);
    EXPECT_EQ(actual.last_tick, expected.last_tick);
}

void ExpectSamePlants(entt::registry& actual, entt::registry& expected) {
    for (const auto entity : actual.view<C::PlantGrowthComponent>()) {
        ExpectSamePlantGrowth(actual.get<C::PlantGrowthComponent>(entity),
                              expected.get<C::PlantGrowthComponent>(entity));
        const auto& owner = actual.get<C::PlantRegionComponent>(entity);
        const auto& expected_owner = expected.get<C::PlantRegionComponent>(entity);
        EXPECT_EQ(owner.persistent_id, expected_owner.persistent_id);
        EXPECT_EQ(owner.owner_x, expected_owner.owner_x);
        EXPECT_EQ(owner.owner_z, expected_owner.owner_z);
        EXPECT_EQ(owner.region_stamp, expected_owner.region_stamp);
    }
}

void AddGrowthOwners(entt::registry& registry) {
    for (const auto entity : registry.view<C::PlantGrowthComponent>()) {
        const auto index = static_cast<std::uint32_t>(entt::to_integral(entity));
        registry.emplace<C::PlantRegionComponent>(
            entity,
            C::PlantRegionComponent{
                .persistent_id = 100u - index, .owner_x = 0, .owner_z = 0, .region_stamp = 10});
        registry.emplace<C::SimulationIdentityComponent>(
            entity, C::SimulationIdentityComponent{.id = 100u - index});
        auto& growth = registry.get<C::PlantGrowthComponent>(entity);
        growth.planted_tick = 3;
        growth.last_tick = 10;
        growth.tended = index == 0 ? 255 : 1;
        if (index != 0)
            registry.emplace<C::PlantHealthComponent>(entity).infection = 437;
    }
}
} // namespace

TEST(PlantCoarseGrowth, RegionalWindowsMatchSingleTicksAtCadenceBoundaries) {
    const std::array windows{1u,
                             7u,
                             8u,
                             9u,
                             31u,
                             32u,
                             33u,
                             63u,
                             64u,
                             65u,
                             255u,
                             256u,
                             257u,
                             35999u,
                             36000u,
                             36001u,
                             65536u};
    for (const auto elapsed : windows) {
        SCOPED_TRACE(elapsed);
        entt::registry regional;
        entt::registry oracle;
        for (auto* registry : {&regional, &oracle}) {
            SpawnPlants(*registry, 2, 2024);
            AddGrowthOwners(*registry);
        }
        const std::array<Luminumbra::world::RegionKey, 1> due{{{0, 0}}};
        const std::map<Luminumbra::world::RegionKey, std::uint32_t> windows_by_owner{
            {{0, 0}, elapsed}};
        const Luminumbra::world::RegionElapsedTicks context(windows_by_owner);
        const Luminumbra::world::RegionConsumerGate gate(regional, due, context);
        std::vector<entt::entity> entities;
        std::vector<float> sampled_positions;
        const auto due_tick = 10u + elapsed;
        // This captures the due-time input once, including across the split at 36,000.
        const auto sample = [&](const C::TransformComponent& transform) {
            sampled_positions.push_back(transform.position.x);
            auto env = EnvAt(transform);
            env.light = static_cast<float>(due_tick % 5) * 0.2f;
            return env;
        };
        int advanced = 0;
        for (std::uint32_t step = 1; step <= elapsed; ++step) {
            advanced +=
                F::RunPlantGrowthSystemOnTick(oracle,
                                              10u + step,
                                              [&](const C::TransformComponent& transform) {
                                                  auto env = EnvAt(transform);
                                                  env.light =
                                                      static_cast<float>(due_tick % 5) * 0.2f;
                                                  return env;
                                              })
                    .advanced;
        }
        const auto stats =
            F::RunRegionalPlantGrowthOnTick(regional, due_tick, sample, gate, entities);
        EXPECT_EQ(stats.updated, 2);
        EXPECT_EQ(stats.advanced, advanced);
        EXPECT_EQ(sampled_positions, (std::vector<float>{0.0f, 7.0f}));
        EXPECT_EQ(gate.owner_visits(), 5u); // one membership build, two queries per plant
        ExpectSamePlants(regional, oracle);
    }
}

TEST(PlantCoarseGrowth, SplitWindowPreservesWrapTendingAndStageCrossings) {
    for (const auto elapsed : {36001u, 65536u}) {
        SCOPED_TRACE(elapsed);
        entt::registry whole;
        entt::registry split;
        entt::registry oracle;
        for (auto* registry : {&whole, &split, &oracle}) {
            SpawnPlants(*registry, 3, 2024);
            AddGrowthOwners(*registry);
            for (const auto entity : registry->view<C::PlantGrowthComponent>()) {
                const auto index = static_cast<std::uint32_t>(entt::to_integral(entity));
                auto& growth = registry->get<C::PlantGrowthComponent>(entity);
                growth.growth_points = index == 0 ? 0xFFFFFFE0u : 999u;
                growth.stress_points = index == 2 ? 0xFFFFFFFEu : 49u;
                registry->get<C::PlantGenomeComponent>(entity).genes.fill(0.6f);
            }
        }
        const F::PlantEnvSample env{0.3f, 0.1f, 0.7f, 0.9f};
        const auto sample = [&](const auto&) {
            return env;
        };
        int oracle_advanced = 0;
        for (std::uint32_t step = 1; step <= elapsed; ++step)
            oracle_advanced += F::RunPlantGrowthSystemOnTick(oracle, 10u + step, sample).advanced;
        const std::array<Luminumbra::world::RegionKey, 1> due{{{0, 0}}};
        std::vector<std::uint64_t> membership;
        std::vector<entt::entity> entities;
        const auto advance = [&](entt::registry& registry,
                                 std::uint32_t window,
                                 std::uint64_t tick) {
            const std::map<Luminumbra::world::RegionKey, std::uint32_t> windows{{{0, 0}, window}};
            const Luminumbra::world::RegionElapsedTicks context(windows);
            const Luminumbra::world::RegionConsumerGate gate(
                registry,
                due,
                membership,
                Luminumbra::world::RegionConsumerGate::PlantIdentity::Simulation,
                context);
            int samples = 0;
            const auto stats = F::RunRegionalPlantGrowthOnTick(
                registry,
                tick,
                [&](const C::TransformComponent& transform) {
                    // Simulation identities reverse handle order.
                    EXPECT_EQ(transform.position.x, static_cast<float>((2 - samples) * 7));
                    ++samples;
                    return env;
                },
                gate,
                entities);
            EXPECT_EQ(samples, 3);
            EXPECT_EQ(stats.updated, 3);
            return stats.advanced;
        };
        EXPECT_EQ(advance(whole, elapsed, 10u + elapsed), oracle_advanced);
        const auto first = advance(split, 32768, 32778);
        EXPECT_EQ(first + advance(split, elapsed - 32768, 10u + elapsed), oracle_advanced);
        ExpectSamePlants(whole, oracle);
        ExpectSamePlants(split, oracle);
        for (const auto entity : whole.view<C::PlantGrowthComponent>())
            EXPECT_EQ(whole.get<C::PlantGrowthComponent>(entity).tended, 0u);
    }
}

TEST(PlantCoarseGrowth, ZeroWindowDoesNotSampleOrStamp) {
    entt::registry registry;
    SpawnPlants(registry, 3, 2024);
    AddGrowthOwners(registry);
    std::vector<C::PlantGrowthComponent> before(3);
    for (const auto entity : registry.view<C::PlantGrowthComponent>()) {
        const auto index = static_cast<std::uint32_t>(entt::to_integral(entity));
        registry.get<C::PlantRegionComponent>(entity).owner_x = static_cast<std::int32_t>(index);
        before[index] = registry.get<C::PlantGrowthComponent>(entity);
        registry.remove<C::SimulationIdentityComponent>(entity);
        registry.emplace_or_replace<C::PlantHealthComponent>(entity).infection = 1001;
    }
    registry.emplace<C::PlantTag>(registry.create()); // incomplete participant
    const std::array<Luminumbra::world::RegionKey, 2> due{{{0, 0}, {2, 0}}};
    // Owner 0 has an explicit zero, owner 1 is frozen, owner 2 has no window.
    const std::map<Luminumbra::world::RegionKey, std::uint32_t> windows{{{0, 0}, 0}, {{1, 0}, 8}};
    const Luminumbra::world::RegionElapsedTicks context(windows);
    std::vector<std::uint64_t> membership;
    const Luminumbra::world::RegionConsumerGate gate(
        registry,
        due,
        membership,
        Luminumbra::world::RegionConsumerGate::PlantIdentity::Simulation,
        context);
    std::vector<entt::entity> entities;
    int samples = 0;
    const auto sample = [&](const auto&) {
        ++samples;
        return F::PlantEnvSample{};
    };
    const auto stats = F::RunRegionalPlantGrowthOnTick(registry, 100, sample, gate, entities);
    EXPECT_EQ(stats.updated, 0);
    EXPECT_EQ(stats.advanced, 0);
    EXPECT_EQ(samples, 0);
    EXPECT_TRUE(entities.empty());
    const auto null_sample =
        static_cast<F::PlantEnvSample (*)(const C::TransformComponent&)>(nullptr);
    EXPECT_EQ(F::RunRegionalPlantGrowthOnTick(registry, 100, null_sample, gate, entities).updated,
              0);
    const F::EnvSampler empty_sample;
    EXPECT_EQ(F::RunRegionalPlantGrowthOnTick(registry, 100, empty_sample, gate, entities).updated,
              0);
    for (const auto entity : registry.view<C::PlantGrowthComponent>()) {
        ExpectSamePlantGrowth(registry.get<C::PlantGrowthComponent>(entity),
                              before[static_cast<std::size_t>(entt::to_integral(entity))]);
        EXPECT_EQ(registry.get<C::PlantRegionComponent>(entity).region_stamp, 10u);
    }
}

TEST(PlantCoarseGrowth, RegionalRefusalsLeavePlantsUnchanged) {
    for (const auto elapsed : {65537u, 0xFFFFFFFFu, 1u}) {
        SCOPED_TRACE(elapsed);
        entt::registry registry;
        SpawnPlants(registry, 2, 2024);
        AddGrowthOwners(registry);
        // The component view visits the valid plant first. A late refusal must
        // still leave both plants unchanged, before any sampler call.
        registry.get<C::PlantRegionComponent>(entt::entity{0}).owner_x = 1;
        if (elapsed == 1)
            registry.emplace<C::PlantHealthComponent>(entt::entity{0}).infection = 1001;
        const std::array<Luminumbra::world::RegionKey, 2> due{{{0, 0}, {1, 0}}};
        const std::map<Luminumbra::world::RegionKey, std::uint32_t> windows{{{0, 0}, 1},
                                                                            {{1, 0}, elapsed}};
        const Luminumbra::world::RegionElapsedTicks context(windows);
        const Luminumbra::world::RegionConsumerGate gate(registry, due, context);
        std::vector<entt::entity> entities;
        int samples = 0;
        EXPECT_THROW(F::RunRegionalPlantGrowthOnTick(
                         registry,
                         70000,
                         [&](const auto&) {
                             ++samples;
                             return F::PlantEnvSample{};
                         },
                         gate,
                         entities),
                     std::invalid_argument);
        EXPECT_EQ(samples, 0);
        for (const auto entity : registry.view<C::PlantGrowthComponent>()) {
            const auto& growth = registry.get<C::PlantGrowthComponent>(entity);
            EXPECT_EQ(growth.last_tick, 10u);
            EXPECT_EQ(growth.growth_points, 0u);
            EXPECT_EQ(growth.stress_points, 0u);
            EXPECT_EQ(growth.tended, entt::to_integral(entity) == 0 ? 255u : 1u);
            EXPECT_EQ(registry.get<C::PlantRegionComponent>(entity).region_stamp, 10u);
        }
    }
}

TEST(PlantCoarseGrowth, RegionalLegacyAdmissionAndSamplerFailures) {
    entt::registry registry;
    SpawnPlants(registry, 1, 2024);
    const auto entity = entt::entity{0};
    const auto before = registry.get<C::PlantGrowthComponent>(entity);
    const Luminumbra::world::RegionConsumerGate gate;
    std::vector<entt::entity> entities;
    const auto null_sample =
        static_cast<F::PlantEnvSample (*)(const C::TransformComponent&)>(nullptr);
    EXPECT_THROW(F::RunRegionalPlantGrowthOnTick(registry, 1, null_sample, gate, entities),
                 std::bad_function_call);
    const F::EnvSampler empty_sample;
    EXPECT_THROW(F::RunRegionalPlantGrowthOnTick(registry, 1, empty_sample, gate, entities),
                 std::bad_function_call);
    EXPECT_THROW(
        F::RunRegionalPlantGrowthOnTick(registry, 1, EnvAt, gate, gate.PlantIdentityScratch()),
        std::invalid_argument);
    ExpectSamePlantGrowth(registry.get<C::PlantGrowthComponent>(entity), before);
    struct Sampler {
        Sampler() = default;
        Sampler(const Sampler&) = delete;
        Sampler& operator=(const Sampler&) = delete;
        explicit operator bool() const {
            return false;
        }
        F::PlantEnvSample operator()(const C::TransformComponent& transform) const {
            return EnvAt(transform);
        }
    };
    const Sampler sample;
    const auto stats = F::RunRegionalPlantGrowthOnTick(registry, 1, sample, gate, entities);
    EXPECT_EQ(stats.updated, 1);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(entity).last_tick, 1u);
    entt::registry oracle;
    SpawnPlants(oracle, 1, 2024);
    F::RunPlantGrowthSystemOnTick(oracle, 1, EnvAt);
    F::RunPlantGrowthSystemOnTick(oracle, 2, EnvAt);
    EXPECT_EQ(F::RunRegionalPlantGrowthOnTick(registry, 2, &EnvAt, gate, entities).updated, 1);
    ExpectSamePlantGrowth(registry.get<C::PlantGrowthComponent>(entity),
                          oracle.get<C::PlantGrowthComponent>(entity));

    entt::registry empty;
    EXPECT_EQ(F::RunRegionalPlantGrowthOnTick(empty, 1, null_sample, gate, entities).updated, 0);
    const Luminumbra::world::RegionConsumerGate identity_gate(
        Luminumbra::world::RegionConsumerGate::PlantIdentity::Simulation);
    const auto admitted = registry.get<C::PlantGrowthComponent>(entity);
    EXPECT_THROW(F::RunRegionalPlantGrowthOnTick(registry, 2, sample, identity_gate, entities),
                 std::runtime_error);
    ExpectSamePlantGrowth(registry.get<C::PlantGrowthComponent>(entity), admitted);
}

TEST(PlantCoarseGrowth, RegionalOwnerWindowsVisitOnlyParticipatingPlants) {
    entt::registry registry;
    SpawnPlants(registry, 4, 2024);
    AddGrowthOwners(registry);
    for (const auto entity : registry.view<C::PlantGrowthComponent>()) {
        const auto index = static_cast<std::uint32_t>(entt::to_integral(entity));
        registry.get<C::PlantRegionComponent>(entity).owner_x = static_cast<std::int32_t>(index);
    }
    // Captured ownership takes precedence over position for plant 0; plant 1
    // exercises transform-based admission in owner 1 without a persistent owner.
    registry.get<C::TransformComponent>(entt::entity{0}).position.x = 8192.0f;
    registry.get<C::TransformComponent>(entt::entity{1}).position.x = 512.0f;
    registry.remove<C::PlantRegionComponent>(entt::entity{1});
    registry.get<C::PlantHealthComponent>(entt::entity{1}).infection = C::kDiseaseUnitScale;
    for (int index = 0; index < 64; ++index)
        registry.emplace<C::TransformComponent>(registry.create());
    const std::array<Luminumbra::world::RegionKey, 3> due{{{0, 0}, {1, 0}, {3, 0}}};
    const std::map<Luminumbra::world::RegionKey, std::uint32_t> windows{
        {{0, 0}, 8}, {{1, 0}, 32}, {{2, 0}, 64}, {{3, 0}, 0}};
    const Luminumbra::world::RegionElapsedTicks context(windows);
    const Luminumbra::world::RegionConsumerGate gate(registry, due, context);
    std::vector<entt::entity> entities;
    std::vector<float> positions;
    const F::EnvSampler sample = [&](const C::TransformComponent& transform) {
        positions.push_back(transform.position.x);
        return EnvAt(transform);
    };
    auto first_expected = registry.get<C::PlantGrowthComponent>(entt::entity{0});
    auto second_expected = registry.get<C::PlantGrowthComponent>(entt::entity{1});
    const auto first_env = EnvAt(registry.get<C::TransformComponent>(entt::entity{0}));
    const auto second_env = EnvAt(registry.get<C::TransformComponent>(entt::entity{1}));
    F::AdvancePlantGrowthCoarse(first_expected,
                                registry.get<C::PlantGenomeComponent>(entt::entity{0}),
                                first_env,
                                0,
                                8,
                                100);
    F::AdvancePlantGrowthCoarse(second_expected,
                                registry.get<C::PlantGenomeComponent>(entt::entity{1}),
                                second_env,
                                C::kDiseaseUnitScale,
                                32,
                                100);
    const auto stats = F::RunRegionalPlantGrowthOnTick(registry, 100, sample, gate, entities);
    EXPECT_EQ(stats.updated, 2);
    EXPECT_EQ(positions, (std::vector<float>{8192.0f, 512.0f}));
    EXPECT_EQ(gate.owner_visits(), 9u); // three due owners, four admissions, two elapsed reads
    ExpectSamePlantGrowth(registry.get<C::PlantGrowthComponent>(entt::entity{0}), first_expected);
    ExpectSamePlantGrowth(registry.get<C::PlantGrowthComponent>(entt::entity{1}), second_expected);
    for (const auto entity : {entt::entity{2}, entt::entity{3}}) {
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(entity).last_tick, 10u);
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(entity).growth_points, 0u);
        EXPECT_EQ(registry.get<C::PlantRegionComponent>(entity).region_stamp, 10u);
    }
}
