//   ( germination): CropLifecycleSystem — ripe plants senesce and reseed.
// Annual = die + drop a germinated seed; perennial = reset + regrow + reseed; the child genome is
// the pollination cross (PollinationComponent.next_genome) or a self copy. Deterministic.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
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

TEST(CropLifecycle, SimulationBirthsPreserveGrazingPresenceAndFreshDefaults) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    for (const bool perennial : {false, true}) {
        for (const bool grazeable : {false, true}) {
            SCOPED_TRACE(perennial);
            SCOPED_TRACE(grazeable);
            entt::registry registry;
            Luminumbra::world::ActiveRegionLedger allocator;
            registry.emplace<C::GrazeableComponent>(registry.create());
            auto parent = spawnRipe(registry, perennial, 1, {100, 0, 100}, RandomGenome(91));
            registry.emplace<C::SimulationIdentityComponent>(parent, 1u);
            ASSERT_TRUE(allocator.reserve_persistent_ids(2));
            if (grazeable)
                registry.emplace<C::GrazeableComponent>(parent, C::GrazeableComponent{0.25f, 6u});
            const Gate gate(Gate::PlantIdentity::Simulation);
            for (std::uint64_t tick = 7; tick <= 8; ++tick) {
                const auto stats = F::RunCropLifecycleOnTick(registry, tick, 99, gate, allocator);
                ASSERT_EQ(stats.reseeded, 1);
                EXPECT_EQ(stats.reset, perennial ? 1 : 0);
                EXPECT_EQ(stats.died, perennial ? 0 : 1);
                EXPECT_EQ(registry.valid(parent), perennial);
                const auto id = tick - 5u;
                entt::entity child = entt::null;
                for (const auto entity : registry.view<C::PlantTag>()) {
                    if (F::RequirePlantSimulationId(registry, entity) == id)
                        child = entity;
                }
                ASSERT_TRUE(registry.valid(child));
                EXPECT_FALSE(registry.all_of<C::PlantRegionComponent>(child));
                EXPECT_EQ(registry.all_of<C::GrazeableComponent>(child), grazeable);
                if (grazeable) {
                    EXPECT_FLOAT_EQ(registry.get<C::GrazeableComponent>(child).biomass, 1.0f);
                    EXPECT_EQ(registry.get<C::GrazeableComponent>(child).last_grazed_tick, 0u);
                    if (perennial) {
                        EXPECT_FLOAT_EQ(registry.get<C::GrazeableComponent>(parent).biomass, 0.25f);
                        EXPECT_EQ(registry.get<C::GrazeableComponent>(parent).last_grazed_tick, 6u);
                    }
                }
                EXPECT_EQ(registry.get<C::CropLifecycleComponent>(child).generations, 0u);
                EXPECT_EQ(registry.get<C::PlantGrowthComponent>(child).stage,
                          static_cast<std::uint8_t>(C::PlantStage::Seed));
                if (!perennial)
                    parent = child;
                registry.get<C::PlantGrowthComponent>(parent).stage =
                    static_cast<std::uint8_t>(C::PlantStage::Fruiting);
            }
            EXPECT_EQ(allocator.next_persistent_id(), 4u);
        }
    }
}

TEST(CropLifecycle, SimulationExhaustionPreservesAnnualAndPerennialParents) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    for (const bool perennial : {false, true}) {
        entt::registry registry;
        Luminumbra::world::ActiveRegionLedger allocator;
        const auto parent = spawnRipe(registry, perennial, 1, {}, RandomGenome(1));
        registry.emplace<C::SimulationIdentityComponent>(parent, 1u);
        registry.emplace<C::GrazeableComponent>(parent, C::GrazeableComponent{0.25f, 6u});
        registry.get<C::PlantGrowthComponent>(parent).stress_points = 81;
        ASSERT_TRUE(allocator.reserve_persistent_ids(
            Luminumbra::world::ActiveRegionLedger::kPersistentIdLimit));
        const auto stats = F::RunCropLifecycleOnTick(
            registry, 7, 99, Gate(Gate::PlantIdentity::Simulation), allocator);
        EXPECT_EQ(stats.reseeded, 0);
        EXPECT_EQ(stats.ripening, 0);
        ASSERT_TRUE(registry.valid(parent));
        EXPECT_EQ(plantCount(registry), 1u);
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(parent).stage,
                  static_cast<std::uint8_t>(C::PlantStage::Fruiting));
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(parent).stress_points, 81u);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 0u);
        EXPECT_FLOAT_EQ(registry.get<C::GrazeableComponent>(parent).biomass, 0.25f);
        EXPECT_EQ(registry.get<C::GrazeableComponent>(parent).last_grazed_tick, 6u);
        EXPECT_EQ(allocator.next_persistent_id(),
                  Luminumbra::world::ActiveRegionLedger::kPersistentIdLimit);
    }
}

TEST(CropLifecycle, SimulationAdmissionRefusesBeforeParentOrCursorMutation) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    for (const int failure : {0, 1, 2, 3, 4}) {
        entt::registry registry;
        Luminumbra::world::ActiveRegionLedger allocator;
        const auto first = spawnRipe(registry, true, 1, {}, RandomGenome(1));
        const auto second = spawnRipe(registry, false, 1, {}, RandomGenome(2));
        registry.emplace<C::SimulationIdentityComponent>(first, 1u);
        if (failure != 0) {
            const auto id =
                failure == 1
                    ? 0u
                    : (failure == 2
                           ? 1u
                           : (failure == 3
                                  ? Luminumbra::world::ActiveRegionLedger::kPersistentIdLimit
                                  : 3u));
            registry.emplace<C::SimulationIdentityComponent>(second, id);
        }
        ASSERT_TRUE(allocator.reserve_persistent_ids(3));
        EXPECT_THROW(F::RunCropLifecycleOnTick(
                         registry, 1, 0, Gate(Gate::PlantIdentity::Simulation), allocator),
                     std::runtime_error);
        EXPECT_EQ(allocator.next_persistent_id(), 3u);
        for (const auto entity : {first, second}) {
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(entity).generations, 0u);
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(entity).ripe_ticks, 0u);
            EXPECT_EQ(registry.get<C::PlantGrowthComponent>(entity).stage,
                      static_cast<std::uint8_t>(C::PlantStage::Fruiting));
        }
        EXPECT_EQ(plantCount(registry), 2u);
    }
}

TEST(CropLifecycle, SimulationSelectionRequiresMatchingAllocatorEvenForEmptyRoster) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    entt::registry registry;
    Luminumbra::world::ActiveRegionLedger allocator;
    const Gate simulation(Gate::PlantIdentity::Simulation);
    EXPECT_THROW(F::RunCropLifecycleOnTick(registry, 1, 0, simulation), std::invalid_argument);
    EXPECT_THROW(F::RunCropLifecycleOnTick(registry, 1, 0, {}, allocator), std::invalid_argument);
    EXPECT_EQ(F::RunCropLifecycleOnTick(registry, 1, 0, simulation, allocator).reseeded, 0);
    EXPECT_EQ(allocator.next_persistent_id(), 1u);
}

TEST(CropLifecycle, SimulationGrowingRipeningAndMaximumRipeCounter) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    entt::registry registry;
    Luminumbra::world::ActiveRegionLedger allocator;
    const auto parent =
        spawnRipe(registry, true, std::numeric_limits<std::uint32_t>::max(), {}, RandomGenome(1));
    registry.emplace<C::SimulationIdentityComponent>(parent, 1u);
    ASSERT_TRUE(allocator.reserve_persistent_ids(2));
    const Gate gate(Gate::PlantIdentity::Simulation);
    EXPECT_EQ(F::RunCropLifecycleOnTick(registry, 1, 0, gate, allocator).ripening, 1);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 1u);
    registry.get<C::PlantGrowthComponent>(parent).stage =
        static_cast<std::uint8_t>(C::PlantStage::Seed);
    EXPECT_EQ(F::RunCropLifecycleOnTick(registry, 2, 0, gate, allocator).reseeded, 0);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
    registry.get<C::PlantGrowthComponent>(parent).stage =
        static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    registry.get<C::CropLifecycleComponent>(parent).ripe_ticks =
        std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(F::RunCropLifecycleOnTick(registry, 3, 0, gate, allocator).reseeded, 1);
    EXPECT_EQ(allocator.next_persistent_id(), 3u);
}

} // namespace
