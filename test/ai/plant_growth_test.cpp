// I9-FOLIAGE: Living-World Foliage pillar — deterministic plant growth + breeding
// coverage. The sim-side plant state is integer/fixed-point and must be replay-
// exact; the genome reuses the creature GA operators. World_hash-neutral until a
// PlantGrowthSystem is wired into the tick (plants are opt-in via PlantTag).

#include "gtest/gtest.h"

#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/core/DeterministicRng.h"

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
    e.moisture    = F::clamp01(0.5f + 0.1f * t);
    e.light       = 0.8f;
    e.soil_quality= 0.6f;
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
    for (auto e : view) ents.push_back(e);
    std::sort(ents.begin(), ents.end());
    for (auto e : ents) {
        const auto& g = view.get<const C::PlantGrowthComponent>(e);
        for (std::uint64_t v : {std::uint64_t(g.stage), std::uint64_t(g.quality),
                                std::uint64_t(g.growth_points), std::uint64_t(g.stress_points)}) {
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
    for (float v : ga.genes) { EXPECT_GE(v, 0.0f); EXPECT_LT(v, 1.0f); }
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
    for (float v : c1.genes) { EXPECT_GE(v, 0.0f); EXPECT_LE(v, 1.0f); }
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
    F::EnvSampler sampler = [&](const C::TransformComponent&) { return env; };
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
    EXPECT_GT(good.stage, 0u);                          // advanced past Seed
    EXPECT_GE(good.quality, poor.quality);              // harsh env accrues stress -> lower quality
    EXPECT_GT(good.growth_points, 0u);
}
