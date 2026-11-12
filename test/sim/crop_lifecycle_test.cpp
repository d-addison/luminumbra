//   ( germination): CropLifecycleSystem — ripe plants senesce and reseed.
// Annual = die + drop a germinated seed; perennial = reset + regrow + reseed; the child genome is
// the pollination cross (PollinationComponent.next_genome) or a self copy. Deterministic.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CropLifecycleComponents.h"
#include "luminumbra_common/components/GrazeableComponent.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/PollinationComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"

namespace {

namespace C = ::Luminumbra::Components;
namespace F = luminumbra::foliage;
using luminumbra::core::DeterministicRng;

C::PlantGenomeComponent RandomGenome(std::uint64_t seed) {
    DeterministicRng rng = DeterministicRng::seeded(F::kPlantSeedOffset, seed);
    return F::RandomGenome(rng);
}

// Spawn a plant already at the terminal Fruiting stage with a lifecycle component.
entt::entity spawnRipe(entt::registry& r,
                       bool perennial,
                       std::uint32_t lifespan,
                       const Luminumbra::Vec3& pos,
                       const C::PlantGenomeComponent& genome) {
    auto e = r.create();
    r.emplace<C::TransformComponent>(e).position = pos;
    r.emplace<C::PlantTag>(e);
    r.emplace<C::PlantGenomeComponent>(e, genome);
    auto& g = r.emplace<C::PlantGrowthComponent>(e);
    g.stage = static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    auto& lc = r.emplace<C::CropLifecycleComponent>(e);
    lc.perennial = perennial;
    lc.lifespan_ticks = lifespan;
    return e;
}

std::size_t plantCount(entt::registry& r) {
    return r.view<C::PlantTag>().size();
}

TEST(CropLifecycle, EmptyRosterNoOp) {
    entt::registry r;
    const auto s = F::RunCropLifecycleOnTick(r, 1);
    EXPECT_EQ(s.reseeded, 0);
    EXPECT_EQ(s.died, 0);
}

TEST(CropLifecycle, GrowingPlantDoesNotSenesce) {
    entt::registry r;
    auto e = spawnRipe(
        r, /*perennial=*/false, /*lifespan=*/3, Luminumbra::Vec3(0, 0, 0), RandomGenome(1));
    r.get<C::PlantGrowthComponent>(e).stage =
        static_cast<std::uint8_t>(C::PlantStage::Sprout); // not terminal
    for (std::uint64_t t = 1; t <= 10; ++t)
        F::RunCropLifecycleOnTick(r, t);
    EXPECT_EQ(r.get<C::CropLifecycleComponent>(e).ripe_ticks, 0u); // ripe counter stays 0
    EXPECT_EQ(plantCount(r), 1u);                                  // no birth/death
}

TEST(CropLifecycle, AnnualDiesAndReseeds) {
    entt::registry r;
    auto parent = spawnRipe(
        r, /*perennial=*/false, /*lifespan=*/3, Luminumbra::Vec3(0, 0, 0), RandomGenome(2));
    for (std::uint64_t t = 1; t <= 4; ++t)
        F::RunCropLifecycleOnTick(r, t);
    EXPECT_FALSE(r.valid(parent)) << "an annual dies at senescence";
    ASSERT_EQ(plantCount(r), 1u) << "exactly one germinated child remains";
    // The child is a fresh seed carrying a lifecycle component (the loop continues).
    for (auto e : r.view<C::PlantTag>()) {
        EXPECT_EQ(r.get<C::PlantGrowthComponent>(e).stage,
                  static_cast<std::uint8_t>(C::PlantStage::Seed));
        EXPECT_TRUE(r.all_of<C::CropLifecycleComponent>(e));
    }
}

TEST(CropLifecycle, PerennialResetsAndRegrowsAndReseeds) {
    entt::registry r;
    auto parent = spawnRipe(
        r, /*perennial=*/true, /*lifespan=*/1, Luminumbra::Vec3(0, 0, 0), RandomGenome(3));
    F::RunCropLifecycleOnTick(r, 1);
    ASSERT_TRUE(r.valid(parent)) << "a perennial survives senescence";
    EXPECT_EQ(r.get<C::PlantGrowthComponent>(parent).stage,
              static_cast<std::uint8_t>(C::PlantStage::Juvenile))
        << "reset to a vegetative stage";
    EXPECT_EQ(r.get<C::CropLifecycleComponent>(parent).generations, 1u);
    EXPECT_EQ(r.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
    EXPECT_EQ(plantCount(r), 2u) << "parent (regrowing) + one germinated child";
}

TEST(CropLifecycle, GrazeableAnnualOffspringStartFreshAcrossGenerations) {
    entt::registry r;
    auto parent = spawnRipe(
        r, /*perennial=*/false, /*lifespan=*/1, Luminumbra::Vec3(0, 0, 0), RandomGenome(5));
    r.emplace<C::GrazeableComponent>(parent, C::GrazeableComponent{0.0f, 6u});
    for (std::uint64_t tick = 7; tick <= 8; ++tick) {
        const auto stats = F::RunCropLifecycleOnTick(r, tick);
        EXPECT_EQ(stats.reseeded, 1);
        EXPECT_EQ(stats.died, 1);
        EXPECT_FALSE(r.valid(parent));
        ASSERT_EQ(plantCount(r), 1u);
        const auto child = *r.view<C::PlantTag>().begin();
        ASSERT_TRUE(r.all_of<C::GrazeableComponent>(child));
        const auto& grazing = r.get<C::GrazeableComponent>(child);
        EXPECT_FLOAT_EQ(grazing.biomass, 1.0f);
        EXPECT_EQ(grazing.last_grazed_tick, 0u);
        EXPECT_EQ(r.get<C::PlantGrowthComponent>(child).stage,
                  static_cast<std::uint8_t>(C::PlantStage::Seed));
        EXPECT_EQ(r.get<C::CropLifecycleComponent>(child).generations, 0u);
        parent = child;
        r.get<C::PlantGrowthComponent>(parent).stage =
            static_cast<std::uint8_t>(C::PlantStage::Fruiting);
        r.get<C::GrazeableComponent>(parent) = C::GrazeableComponent{0.25f, 7u};
    }
}

TEST(CropLifecycle, GrazeablePerennialOffspringStartFreshWithoutResettingParentGrazing) {
    entt::registry r;
    const auto parent = spawnRipe(
        r, /*perennial=*/true, /*lifespan=*/1, Luminumbra::Vec3(0, 0, 0), RandomGenome(6));
    r.emplace<C::GrazeableComponent>(parent, C::GrazeableComponent{0.25f, 6u});
    const auto stats = F::RunCropLifecycleOnTick(r, 7);
    EXPECT_EQ(stats.reseeded, 1);
    EXPECT_EQ(stats.reset, 1);
    ASSERT_TRUE(r.valid(parent));
    ASSERT_EQ(plantCount(r), 2u);
    EXPECT_FLOAT_EQ(r.get<C::GrazeableComponent>(parent).biomass, 0.25f);
    EXPECT_EQ(r.get<C::GrazeableComponent>(parent).last_grazed_tick, 6u);
    for (const auto child : r.view<C::PlantTag>()) {
        if (child != parent) {
            ASSERT_TRUE(r.all_of<C::GrazeableComponent>(child));
            EXPECT_FLOAT_EQ(r.get<C::GrazeableComponent>(child).biomass, 1.0f);
            EXPECT_EQ(r.get<C::GrazeableComponent>(child).last_grazed_tick, 0u);
        }
    }
}

TEST(CropLifecycle, NonGrazeableParentsProduceNonGrazeableOffspring) {
    for (const bool perennial : {false, true}) {
        SCOPED_TRACE(perennial);
        entt::registry r;
        r.emplace<C::GrazeableComponent>(r.create());
        const auto parent =
            spawnRipe(r, perennial, /*lifespan=*/1, Luminumbra::Vec3(0, 0, 0), RandomGenome(7));
        const auto stats = F::RunCropLifecycleOnTick(r, 7);
        EXPECT_EQ(stats.reseeded, 1);
        EXPECT_EQ(plantCount(r), perennial ? 2u : 1u);
        for (const auto plant : r.view<C::PlantTag>()) {
            EXPECT_FALSE(r.all_of<C::GrazeableComponent>(plant));
        }
        EXPECT_EQ(r.valid(parent), perennial);
    }
}

TEST(CropLifecycle, GrazingOptInDoesNotChangeExistingOffspringStateOrReplay) {
    const auto run = [](bool grazeable) {
        entt::registry r;
        for (std::uint64_t i = 0; i < 6; ++i) {
            const auto parent = spawnRipe(r,
                                          /*perennial=*/(i % 2u == 0),
                                          /*lifespan=*/2,
                                          Luminumbra::Vec3(static_cast<float>(i * 4u), 0, 0),
                                          RandomGenome(i));
            if (grazeable) {
                r.emplace<C::GrazeableComponent>(parent, C::GrazeableComponent{0.25f, 1u});
            }
            auto& pollination = r.emplace<C::PollinationComponent>(parent);
            pollination.pollinated = true;
            pollination.next_genome = RandomGenome(i + 100u);
            r.get<C::CropLifecycleComponent>(parent).species_id =
                static_cast<std::uint16_t>(i + 1u);
        }
        for (std::uint64_t tick = 1; tick <= 30; ++tick) {
            F::RunCropLifecycleOnTick(r, tick, 123u);
        }
        return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(r));
    };
    EXPECT_EQ(run(true), run(false));
    EXPECT_EQ(run(true), run(true));
}

TEST(CropLifecycle, PersistentOffspringRoundtripPreservesGrazingOptIn) {
    for (const bool grazeable : {false, true}) {
        SCOPED_TRACE(grazeable);
        entt::registry r;
        const auto parent = spawnRipe(
            r, /*perennial=*/false, /*lifespan=*/1, Luminumbra::Vec3(0, 0, 0), RandomGenome(8));
        if (grazeable) {
            r.emplace<C::GrazeableComponent>(parent, C::GrazeableComponent{0.25f, 6u});
        }
        F::RunCropLifecycleOnTick(r, 7);
        ASSERT_EQ(plantCount(r), 1u);
        const auto child = *r.view<C::PlantTag>().begin();
        const auto owner = Luminumbra::world::ActiveRegionLedger::region_at(
            r.get<C::TransformComponent>(child).position);
        r.emplace<C::PlantRegionComponent>(child,
                                           C::PlantRegionComponent{1u, owner.x, owner.z, 7u});
        const auto snapshot = F::BuildPlantEntitySnapshot(r, true);
        const auto bytes = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot);
        Luminumbra::Ecs::EntityRegistrySnapshot loaded;
        std::vector<std::string> errors;
        ASSERT_TRUE(Luminumbra::Ecs::LoadEntityRegistrySnapshotJson(bytes, loaded, errors));
        entt::registry restored;
        ASSERT_TRUE(F::ApplyPlantEntitySnapshot(restored, loaded));
        ASSERT_EQ(plantCount(restored), 1u);
        const auto restored_child = *restored.view<C::PlantTag>().begin();
        ASSERT_EQ(restored.all_of<C::GrazeableComponent>(restored_child), grazeable);
        if (grazeable) {
            EXPECT_FLOAT_EQ(restored.get<C::GrazeableComponent>(restored_child).biomass, 1.0f);
            EXPECT_EQ(restored.get<C::GrazeableComponent>(restored_child).last_grazed_tick, 0u);
        }
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                      F::BuildPlantEntitySnapshot(restored, true)),
                  bytes);
    }
}

TEST(CropLifecycle, GerminatesFromPollinationCross) {
    entt::registry r;
    auto parent = spawnRipe(
        r, /*perennial=*/false, /*lifespan=*/1, Luminumbra::Vec3(0, 0, 0), RandomGenome(4));
    C::PlantGenomeComponent cross;
    cross.genes.fill(0.42f); // a recognisably distinct cross genome
    auto& pc = r.emplace<C::PollinationComponent>(parent);
    pc.pollinated = true;
    pc.next_genome = cross;
    F::RunCropLifecycleOnTick(r, 1);
    ASSERT_FALSE(r.valid(parent));
    ASSERT_EQ(plantCount(r), 1u);
    for (auto e : r.view<C::PlantTag>()) {
        EXPECT_EQ(r.get<C::PlantGenomeComponent>(e).genes, cross.genes)
            << "the child germinates from the pollination cross, not the parent's self genome";
    }
}

TEST(CropLifecycle, DeterministicReplay) {
    auto run = [] {
        entt::registry r;
        for (int i = 0; i < 6; ++i)
            spawnRipe(r,
                      /*perennial=*/(i % 2 == 0),
                      /*lifespan=*/2,
                      Luminumbra::Vec3(static_cast<float>(i * 4), 0, 0),
                      RandomGenome(static_cast<std::uint64_t>(i)));
        for (std::uint64_t t = 1; t <= 30; ++t)
            F::RunCropLifecycleOnTick(r, t);
        // Digest: plant count + summed stage + generations (order-independent).
        std::uint64_t acc = plantCount(r);
        for (auto e : r.view<C::PlantTag, const C::PlantGrowthComponent>())
            acc = acc * 131u + r.get<const C::PlantGrowthComponent>(e).stage;
        return acc;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
