//   integration: the LIVE spawn path (FarmingSystem MakePlantFromSpecies) must wire
// the cross-pollination opt-in, so a field of GAME-spawned plants actually DRIFTS genetically over
// seasons — flowering plants cross-pollinate (PollinationSystem -> next_genome) and senescence
// germinates the crossed child (CropLifecycleSystem). Without the opt-in those ticks skip every
// plant and the field's genetics never drift (germination falls back to a self copy). These tests
// guard the wiring end-to-end on the SAME spawn the game uses, not on hand-assembled rosters.
// Deterministic.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <entt/entt.hpp>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CropLifecycleComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/PollinationComponents.h"
#include "luminumbra_common/components/SoilComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/systems/PollinationSystem.h"
#include "luminumbra_common/systems/SoilNutrientSystem.h"

namespace {

namespace C = ::Luminumbra::Components;
namespace F = luminumbra::foliage;
using luminumbra::core::DeterministicRng;

// A minimal engine-generic species: full [0,1] gene ranges so successive samples differ.
F::SpeciesTemplate testCrop(bool perennial = false, std::uint32_t lifespan = 1) {
    F::SpeciesTemplate t;
    t.id = "test_crop";
    t.perennial = perennial;
    t.lifespan_ticks = lifespan;
    t.gene_lo.fill(0.0f);
    t.gene_hi.fill(1.0f);
    return t;
}

void setStage(entt::registry& r, entt::entity e, C::PlantStage s) {
    r.get<C::PlantGrowthComponent>(e).stage = static_cast<std::uint8_t>(s);
}

// The live spawn path opts a plant into BOTH the lifecycle and the cross-pollination ticks.
TEST(FoliageDrift, SpawnedPlantOptsIntoPollinationAndLifecycle) {
    entt::registry r;
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 1);
    const auto e = F::MakePlantFromSpecies(r, Luminumbra::Vec3(0, 0, 0), testCrop(), rng, 0);
    EXPECT_TRUE(r.all_of<C::PlantTag>(e));
    EXPECT_TRUE(r.all_of<C::CropLifecycleComponent>(e));
    EXPECT_TRUE(r.all_of<C::PollinationTag>(e))
        << "a spawned plant must participate in cross-pollination";
    EXPECT_TRUE(r.all_of<C::PollinationComponent>(e));
}

// Two adjacent GAME-spawned flowering plants cross-pollinate: each receives the other's pollen and
// its next-generation genome becomes a real cross (differs from its own genome).
TEST(FoliageDrift, AdjacentSpawnedPlantsCrossPollinate) {
    entt::registry r;
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 7);
    const auto a = F::MakePlantFromSpecies(r, Luminumbra::Vec3(0, 0, 0), testCrop(), rng, 0);
    const auto b = F::MakePlantFromSpecies(
        r, Luminumbra::Vec3(2, 0, 0), testCrop(), rng, 0); // within reach (6 m)
    setStage(r, a, C::PlantStage::Flowering);
    setStage(r, b, C::PlantStage::Flowering);
    const auto genomeA = r.get<C::PlantGenomeComponent>(a).genes;

    const F::PollinationStats s = F::RunPollinationOnTick(r, /*tick=*/1);
    EXPECT_EQ(s.crossed, 2) << "each flowering neighbour pollinates the other";
    const auto& pcA = r.get<C::PollinationComponent>(a);
    EXPECT_TRUE(pcA.pollinated);
    EXPECT_GT(pcA.crosses, 0u);
    EXPECT_NE(pcA.next_genome.genes, genomeA)
        << "the next-gen genome is a cross, not the self genome";
}

// The full loop on game-spawned plants: a flowering/fruiting field cross-pollinates, then
// senescence germinates a CHILD from the cross (not a self copy) — the field carries the blend
// forward.
TEST(FoliageDrift, SenescenceGerminatesCrossedChild) {
    entt::registry r;
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 11);
    const auto a =
        F::MakePlantFromSpecies(r, Luminumbra::Vec3(0, 0, 0), testCrop(false, 1), rng, 0);
    const auto b =
        F::MakePlantFromSpecies(r, Luminumbra::Vec3(2, 0, 0), testCrop(false, 1), rng, 0);
    setStage(
        r, a, C::PlantStage::Fruiting); // Fruiting plants donate pollen and are ripe to senesce
    setStage(r, b, C::PlantStage::Fruiting);
    const auto selfA = r.get<C::PlantGenomeComponent>(a).genes;
    const auto selfB = r.get<C::PlantGenomeComponent>(b).genes;

    F::RunPollinationOnTick(r, 1);   // records each parent's cross in next_genome
    F::RunCropLifecycleOnTick(r, 2); // ripe annuals senesce -> die + germinate the crossed child

    EXPECT_FALSE(r.valid(a)) << "the annual parents senesced";
    EXPECT_FALSE(r.valid(b));
    std::size_t total = 0, crossed = 0;
    for (auto e : r.view<C::PlantTag, const C::PlantGenomeComponent>()) {
        ++total;
        const auto& g = r.get<const C::PlantGenomeComponent>(e).genes;
        if (g != selfA && g != selfB)
            ++crossed;
        // The child must itself keep the opt-in so the field keeps drifting next generation.
        EXPECT_TRUE(r.all_of<C::PollinationTag>(e));
    }
    EXPECT_EQ(total, 2u) << "two annual parents germinate two children";
    EXPECT_EQ(crossed, total)
        << "every germinated child carries the pollination cross, not a self copy";
}

// Live-spawned plants FEED on soil (the monoculture-starves loop): a dense cell of game-spawned
// mature feeders draws its shared cell's nutrient far below an unplanted cell. The growth tick
// reads NutrientAt back into suitability, so a crowded monoculture self-limits until rotated /
// fertilised.
TEST(FoliageDrift, SpawnedMonocultureDepletesSoil) {
    entt::registry r;
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 5);
    constexpr float kCell = 4.0f, kOrigin = 0.0f;
    for (int i = 0; i < 8; ++i) { // 8 feeders packed into the single cell [0,kCell)
        const auto e =
            F::MakePlantFromSpecies(r, Luminumbra::Vec3(0.1f * i, 0, 0.1f * i), testCrop(), rng, 0);
        EXPECT_TRUE(r.all_of<C::SoilFeederComponent>(e)) << "a spawned plant must feed on soil";
        setStage(r, e, C::PlantStage::Fruiting); // fruiting plants feed hardest
    }
    F::SoilGrid soil(4, 4); // every cell starts at the rich baseline
    const std::int32_t before = F::NutrientAt(soil, 0.0f, 0.0f, kOrigin, kOrigin, kCell);
    for (int t = 0; t < 120; ++t)
        F::RunSoilNutrientOnTick(r, soil, kOrigin, kOrigin, kCell);
    const std::int32_t fed = F::NutrientAt(soil, 0.0f, 0.0f, kOrigin, kOrigin, kCell);
    const std::int32_t unplanted =
        F::NutrientAt(soil, 3.0f * kCell, 3.0f * kCell, kOrigin, kOrigin, kCell);
    EXPECT_LT(fed, before) << "a dense monoculture draws its shared cell's nutrient down";
    EXPECT_LT(fed, unplanted) << "only the planted cell starves; an unplanted cell stays rich";
}

// run == replay: the whole live loop (spawn -> pollinate -> germinate) is byte-deterministic.
TEST(FoliageDrift, DeterministicReplay) {
    auto run = [] {
        entt::registry r;
        DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, 3);
        for (int i = 0; i < 4; ++i) {
            const auto e = F::MakePlantFromSpecies(
                r, Luminumbra::Vec3(static_cast<float>(i * 2), 0, 0), testCrop(false, 1), rng, 0);
            setStage(r, e, C::PlantStage::Fruiting);
        }
        for (std::uint64_t t = 1; t <= 6; ++t) {
            F::RunPollinationOnTick(r, t);
            F::RunCropLifecycleOnTick(r, t);
        }
        std::uint64_t acc = r.view<C::PlantTag>().size();
        for (auto e : r.view<C::PlantTag, const C::PlantGrowthComponent>())
            acc = acc * 131u + r.get<const C::PlantGrowthComponent>(e).stage;
        return acc;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
