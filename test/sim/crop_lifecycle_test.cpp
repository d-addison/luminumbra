//   ( germination): CropLifecycleSystem — ripe plants senesce and reseed.
// Annual = die + drop a germinated seed; perennial = reset + regrow + reseed; the child genome is
// the pollination cross (PollinationComponent.next_genome) or a self copy. Deterministic.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
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

void ExpectInvalidOwnedDestinationUnchanged(const Luminumbra::Vec3& position,
                                            std::uint64_t outward_tick,
                                            bool simulation,
                                            bool regional,
                                            bool perennial) {
    using Ledger = Luminumbra::world::ActiveRegionLedger;
    using Gate = Luminumbra::world::RegionConsumerGate;
    SCOPED_TRACE(simulation);
    SCOPED_TRACE(regional);
    SCOPED_TRACE(perennial);
    SCOPED_TRACE(position.x);
    SCOPED_TRACE(position.z);
    entt::registry registry;
    Ledger ledger;
    (void)registry.create(); // Keep the parent's runtime handle equal to ID 1.
    const auto parent = spawnRipe(registry, perennial, 1, position, RandomGenome(1));
    ASSERT_EQ(entt::to_integral(parent), 1u);
    registry.emplace<C::SimulationIdentityComponent>(parent, 1u);
    const auto owner = Ledger::valid_anchor(position) ? Ledger::region_at(position)
                                                      : Luminumbra::world::RegionKey{0, 0};
    registry.emplace<C::PlantRegionComponent>(parent,
                                              C::PlantRegionComponent{1u, owner.x, owner.z, 0u});
    registry.emplace<C::GrazeableComponent>(parent, C::GrazeableComponent{0.25f, 6u});
    registry.get<C::PlantGrowthComponent>(parent).stress_points = 81;
    ASSERT_TRUE(ledger.reserve_persistent_ids(2));
    const auto before_ledger = ledger.encode();
    const auto before_entities = registry.storage<entt::entity>().free_list();
    const auto before_plants = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        F::BuildPlantEntitySnapshot(registry, true));
    const auto identity =
        simulation ? Gate::PlantIdentity::Simulation : Gate::PlantIdentity::Legacy;
    const std::array due{owner};
    std::vector<std::uint64_t> owners;
    const auto gate = regional ? Gate(registry, due, owners, identity) : Gate(identity);
    std::uint32_t allocations = 0;
    const auto allocate = [&ledger, &allocations](std::uint64_t& id) {
        ++allocations;
        return ledger.allocate_persistent_id(id);
    };
    F::CropLifecycleStats stats;
    ASSERT_NO_THROW(
        stats = simulation ? F::RunCropLifecycleOnTick(registry, outward_tick, 99, gate, ledger)
                           : F::RunCropLifecycleOnTick(registry, outward_tick, 99, gate, allocate));
    EXPECT_EQ(stats.reseeded, 0);
    EXPECT_EQ(stats.ripening, 0);
    EXPECT_EQ(stats.reset, 0);
    EXPECT_EQ(stats.died, 0);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
    EXPECT_EQ(ledger.encode(), before_ledger);
    EXPECT_EQ(registry.storage<entt::entity>().free_list(), before_entities);
    ASSERT_TRUE(registry.valid(parent));
    EXPECT_EQ(plantCount(registry), 1u);
    EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                  F::BuildPlantEntitySnapshot(registry, true)),
              before_plants);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, parent), 1u);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 0u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(parent).stage,
              static_cast<std::uint8_t>(C::PlantStage::Fruiting));
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(parent).stress_points, 81u);
    EXPECT_FLOAT_EQ(registry.get<C::GrazeableComponent>(parent).biomass, 0.25f);
    EXPECT_EQ(registry.get<C::GrazeableComponent>(parent).last_grazed_tick, 6u);
}

TEST(CropLifecycle, InvalidOwnedDestinationsLeaveParentsEntitiesAndLedgerUnchanged) {
    using Ledger = Luminumbra::world::ActiveRegionLedger;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const std::array positions{Luminumbra::Vec3(16777215.0f, 0, 100),
                               Luminumbra::Vec3(-16777216.0f, 0, 100),
                               Luminumbra::Vec3(100, 0, 16777215.0f),
                               Luminumbra::Vec3(100, 0, -16777216.0f),
                               Luminumbra::Vec3(nan, 0, 100),
                               Luminumbra::Vec3(100, inf, 100),
                               Luminumbra::Vec3(100, 0, inf)};
    for (const auto& position : positions) {
        // Find outward dispersal using the same full ID in both identity lanes.
        // At most 100 probes, independent of the world and registry sizes.
        std::uint64_t outward_tick = 0;
        for (std::uint64_t tick = 1; tick <= 100; ++tick) {
            auto rng = DeterministicRng::seeded(F::kGerminationSeedOffset ^ 99u, 1u, tick);
            const float angle = rng.next_unit() * 6.2831853f;
            const float radius = 1.0f + rng.next_unit() * 2.0f;
            const Luminumbra::Vec3 destination(
                position.x + radius * Luminumbra::DeterministicMath::Cos(angle),
                position.y,
                position.z + radius * Luminumbra::DeterministicMath::Sin(angle));
            if (!Ledger::valid_anchor(destination)) {
                outward_tick = tick;
                break;
            }
        }
        ASSERT_NE(outward_tick, 0u);
        for (const bool simulation : {true, false}) {
            for (const bool regional : {false, true}) {
                for (const bool perennial : {false, true}) {
                    ExpectInvalidOwnedDestinationUnchanged(
                        position, outward_tick, simulation, regional, perennial);
                }
            }
        }
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
        // The invalid participant joins only the last virtual tick. Admission
        // must refuse the whole window before the first parent can reproduce.
        const std::array due{Luminumbra::world::RegionKey{0, 0},
                             Luminumbra::world::RegionKey{1, 0}};
        registry.emplace<C::PlantRegionComponent>(first, C::PlantRegionComponent{1u, 0, 0, 0u});
        registry.emplace<C::PlantRegionComponent>(second, C::PlantRegionComponent{2u, 1, 0, 0u});
        const std::map<Luminumbra::world::RegionKey, std::uint32_t> windows{{due[0], 2},
                                                                            {due[1], 1}};
        std::vector<std::uint64_t> scratch;
        const Gate gate(registry,
                        due,
                        scratch,
                        Gate::PlantIdentity::Simulation,
                        Luminumbra::world::RegionElapsedTicks(windows));
        EXPECT_THROW(F::RunCropLifecycleRegional(registry, 2, gate, 0, allocator),
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
    EXPECT_THROW(F::RunCropLifecycleRegional(registry, 1, simulation), std::invalid_argument);
    EXPECT_THROW(F::RunCropLifecycleRegional(registry, 1, {}, 0, allocator), std::invalid_argument);
    EXPECT_EQ(F::RunCropLifecycleRegional(registry, 1, simulation, 0, allocator).reseeded, 0);
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

TEST(CropLifecycle, RegionalElapsedPreservesBirthIdentityAndEventMultiplicity) {
    namespace W = Luminumbra::world;
    using Gate = W::RegionConsumerGate;
    for (const bool simulation : {false, true}) {
        SCOPED_TRACE(simulation);
        entt::registry regional, reference;
        W::ActiveRegionLedger regional_ledger, reference_ledger;
        const std::array<std::uint64_t, 6> ids{3, 1, 2, 4, 5, 6};
        const std::array owners{W::RegionKey{0, 0}, W::RegionKey{-1, 0}, W::RegionKey{1, 0}};
        for (auto* registry : {&regional, &reference}) {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto owner = owners[index == 3 ? 1 : (index == 4 ? 2 : 0)];
                const auto parent = spawnRipe(*registry,
                                              index % 2 == 0,
                                              index % 2 == 0 ? 2u : 1u,
                                              {static_cast<float>(owner.x * 512 + 100), 0, 100},
                                              RandomGenome(index + 1u));
                registry->emplace<C::PlantRegionComponent>(
                    parent, C::PlantRegionComponent{ids[index], owner.x, owner.z, 0u});
                registry->emplace<C::SimulationIdentityComponent>(parent, ids[index]);
                registry->get<C::PlantGrowthComponent>(parent).stress_points = 81;
                if (index == 5)
                    registry->get<C::PlantGrowthComponent>(parent).planted_tick = 100;
                registry->emplace<C::PollinationComponent>(parent).pollinated = true;
                registry->get<C::PollinationComponent>(parent).next_genome = RandomGenome(999);
            }
        }
        ASSERT_TRUE(regional_ledger.reserve_persistent_ids(7));
        ASSERT_TRUE(reference_ledger.reserve_persistent_ids(7));
        const auto identity =
            simulation ? Gate::PlantIdentity::Simulation : Gate::PlantIdentity::Legacy;
        const std::array due{owners[0], owners[2]};
        const std::map<W::RegionKey, std::uint32_t> windows{
            {owners[0], 5}, {owners[1], 5}, {owners[2], 0}};
        std::vector<std::uint64_t> scratch;
        const Gate gate(regional, due, scratch, identity, W::RegionElapsedTicks(windows));
        const auto allocate = [&](std::uint64_t& id) {
            return regional_ledger.allocate_persistent_id(id);
        };
        const auto result =
            simulation ? F::RunCropLifecycleRegional(regional, 100, gate, 99, regional_ledger)
                       : F::RunCropLifecycleRegional(regional, 100, gate, 99, allocate);
        F::CropLifecycleStats expected;
        for (std::uint64_t tick = 96; tick <= 100; ++tick) {
            // The reference advances one ordinary tick. Exclude the explicitly
            // future-born parent until its real birth, without changing its owner.
            entt::entity future_parent = entt::null;
            if (tick < 100) {
                for (const auto entity : reference.view<C::PlantRegionComponent>()) {
                    if (reference.get<C::PlantRegionComponent>(entity).persistent_id == 6u)
                        future_parent = entity;
                }
                ASSERT_TRUE(reference.valid(future_parent));
                reference.remove<C::CropLifecycleComponent>(future_parent);
            }
            std::vector<std::uint64_t> step_scratch;
            const std::array active{owners[0]};
            const Gate step_gate(reference, active, step_scratch, identity);
            const auto step_allocate = [&](std::uint64_t& id) {
                return reference_ledger.allocate_persistent_id(id);
            };
            const auto step =
                simulation
                    ? F::RunCropLifecycleOnTick(reference, tick, 99, step_gate, reference_ledger)
                    : F::RunCropLifecycleOnTick(reference, tick, 99, step_gate, step_allocate);
            if (tick < 100) {
                auto& lifecycle = reference.emplace<C::CropLifecycleComponent>(future_parent);
                lifecycle.lifespan_ticks = 1;
            }
            expected.ripening += step.ripening;
            expected.reseeded += step.reseeded;
            expected.reset += step.reset;
            expected.died += step.died;
        }
        EXPECT_EQ(result.ripening, expected.ripening);
        EXPECT_EQ(result.reseeded, expected.reseeded);
        EXPECT_EQ(result.reset, expected.reset);
        EXPECT_EQ(result.died, expected.died);
        EXPECT_EQ(result.ripening, 6);
        EXPECT_EQ(result.reseeded, 4);
        EXPECT_EQ(result.reset, 2);
        EXPECT_EQ(result.died, 2);
        // Owner lookups depend on the initial roster and four births, not elapsed scans.
        EXPECT_EQ(gate.owner_visits(), 21u);
        EXPECT_EQ(regional_ledger.next_persistent_id(), 11u);
        EXPECT_EQ(regional_ledger.encode(), reference_ledger.encode());
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                      F::BuildPlantEntitySnapshot(regional, true)),
                  Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                      F::BuildPlantEntitySnapshot(reference, true)));
        for (const auto entity : regional.view<C::PlantRegionComponent>()) {
            const auto id = regional.get<C::PlantRegionComponent>(entity).persistent_id;
            if (id < 7)
                continue;
            const auto birth_tick = id == 7 ? 96u : (id == 10 ? 100u : 97u);
            const auto& growth = regional.get<C::PlantGrowthComponent>(entity);
            EXPECT_EQ(growth.planted_tick, birth_tick);
            EXPECT_EQ(growth.last_tick, birth_tick);
            EXPECT_EQ(regional.get<C::PlantRegionComponent>(entity).region_stamp, birth_tick);
            EXPECT_EQ(regional.get<C::PlantGenomeComponent>(entity).genes, RandomGenome(999).genes);
            EXPECT_EQ(regional.get<C::CropLifecycleComponent>(entity).generations, 0u);
            EXPECT_EQ(regional.get<C::CropLifecycleComponent>(entity).ripe_ticks, 0u);
            if (simulation) {
                EXPECT_EQ(F::RequirePlantSimulationId(regional, entity), id);
            }
        }
    }
}

TEST(CropLifecycle, RegionalDestinationMustCoverBirthTickAndRefusalsPreserveParent) {
    namespace W = Luminumbra::world;
    using Gate = W::RegionConsumerGate;
    for (const bool simulation : {false, true}) {
        for (const int target_window : {-1, 0, 1}) {
            SCOPED_TRACE(simulation);
            SCOPED_TRACE(target_window);
            entt::registry registry;
            W::ActiveRegionLedger ledger;
            const auto parent = spawnRipe(registry, true, 1, {0, 0, 100}, RandomGenome(1));
            registry.emplace<C::PlantRegionComponent>(parent,
                                                      C::PlantRegionComponent{1u, 0, 0, 0u});
            registry.emplace<C::SimulationIdentityComponent>(parent, 1u);
            registry.get<C::PlantGrowthComponent>(parent).stress_points = 81;
            ASSERT_TRUE(ledger.reserve_persistent_ids(2));
            std::uint64_t due_tick = 0;
            for (std::uint64_t tick = 1; tick <= 100; ++tick) {
                auto rng = DeterministicRng::seeded(F::kGerminationSeedOffset ^ 99u, 1u, tick);
                const auto angle = rng.next_unit() * 6.2831853f;
                if (Luminumbra::DeterministicMath::Cos(angle) < 0) {
                    due_tick = tick + 1;
                    break;
                }
            }
            ASSERT_NE(due_tick, 0u);
            std::vector<W::RegionKey> due{{0, 0}};
            if (target_window >= 0)
                due.push_back({-1, 0});
            const std::map<W::RegionKey, std::uint32_t> windows{
                {{0, 0}, 2}, {{-1, 0}, target_window > 0 ? 1u : 0u}};
            std::vector<std::uint64_t> scratch;
            const Gate gate(registry,
                            due,
                            scratch,
                            simulation ? Gate::PlantIdentity::Simulation
                                       : Gate::PlantIdentity::Legacy,
                            W::RegionElapsedTicks(windows));
            // First replay only the outward event tick. The target is frozen or
            // has no overlapping history, so no identity may be consumed.
            const std::map<W::RegionKey, std::uint32_t> first_windows{{{0, 0}, 1}, {{-1, 0}, 0}};
            std::vector<std::uint64_t> first_scratch;
            const Gate first_gate(registry,
                                  due,
                                  first_scratch,
                                  simulation ? Gate::PlantIdentity::Simulation
                                             : Gate::PlantIdentity::Legacy,
                                  W::RegionElapsedTicks(first_windows));
            const auto allocate = [&](std::uint64_t& id) {
                return ledger.allocate_persistent_id(id);
            };
            const auto refused =
                simulation
                    ? F::RunCropLifecycleRegional(registry, due_tick - 1, first_gate, 99, ledger)
                    : F::RunCropLifecycleRegional(registry, due_tick - 1, first_gate, 99, allocate);
            EXPECT_EQ(refused.reseeded, 0);
            EXPECT_EQ(ledger.next_persistent_id(), 2u);
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 0u);
            EXPECT_EQ(registry.get<C::PlantGrowthComponent>(parent).stress_points, 81u);
            if (target_window == 1) {
                const auto result =
                    simulation
                        ? F::RunCropLifecycleRegional(registry, due_tick, gate, 99, ledger)
                        : F::RunCropLifecycleRegional(registry, due_tick, gate, 99, allocate);
                EXPECT_EQ(result.reseeded, 1);
                EXPECT_EQ(ledger.next_persistent_id(), 3u);
                for (const auto entity : registry.view<C::PlantRegionComponent>()) {
                    if (entity != parent) {
                        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(entity).planted_tick,
                                  due_tick);
                    }
                }
            }
        }
    }
}

TEST(CropLifecycle, RegionalWindowBoundsExhaustionAndMaximumTick) {
    namespace W = Luminumbra::world;
    using Gate = W::RegionConsumerGate;
    for (const std::uint32_t elapsed : {0u, 1u, 36001u, 65536u, 65537u}) {
        SCOPED_TRACE(elapsed);
        entt::registry registry;
        const auto parent = spawnRipe(registry,
                                      true,
                                      std::numeric_limits<std::uint32_t>::max(),
                                      {100, 0, 100},
                                      RandomGenome(1));
        const std::array due{W::RegionKey{0, 0}};
        const std::map<W::RegionKey, std::uint32_t> windows{{due[0], elapsed}};
        const Gate gate(registry, due, W::RegionElapsedTicks(windows));
        if (elapsed > 65536) {
            EXPECT_THROW(F::RunCropLifecycleRegional(registry, 65536, gate), std::invalid_argument);
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
        } else {
            EXPECT_EQ(F::RunCropLifecycleRegional(registry, 65536, gate).ripening,
                      static_cast<int>(elapsed));
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, elapsed);
        }
        const auto before = registry.get<C::CropLifecycleComponent>(parent).ripe_ticks;
        F::RunCropLifecycleOnTick(registry, 65537, 0, gate);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, before + 1u);
    }
    for (const bool simulation : {false, true}) {
        entt::registry registry;
        W::ActiveRegionLedger ledger;
        const auto parent = spawnRipe(registry, true, 1, {100, 0, 100}, RandomGenome(1));
        registry.emplace<C::PlantRegionComponent>(parent, C::PlantRegionComponent{1u, 0, 0, 0u});
        registry.emplace<C::SimulationIdentityComponent>(parent, 1u);
        ASSERT_TRUE(ledger.reserve_persistent_ids(W::ActiveRegionLedger::kPersistentIdLimit));
        const std::array due{W::RegionKey{0, 0}};
        std::map<W::RegionKey, std::uint32_t> windows{{due[0], 3}};
        std::vector<std::uint64_t> scratch;
        const Gate gate(registry,
                        due,
                        scratch,
                        simulation ? Gate::PlantIdentity::Simulation : Gate::PlantIdentity::Legacy,
                        W::RegionElapsedTicks(windows));
        const auto allocate = [&](std::uint64_t& id) {
            return ledger.allocate_persistent_id(id);
        };
        EXPECT_THROW((simulation ? F::RunCropLifecycleRegional(registry, 1, gate, 0, ledger)
                                 : F::RunCropLifecycleRegional(registry, 1, gate, 0, allocate)),
                     std::invalid_argument);
        const auto result = simulation
                                ? F::RunCropLifecycleRegional(registry, 100, gate, 0, ledger)
                                : F::RunCropLifecycleRegional(registry, 100, gate, 0, allocate);
        EXPECT_EQ(result.reseeded, 0);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 0u);
        EXPECT_EQ(ledger.next_persistent_id(), W::ActiveRegionLedger::kPersistentIdLimit);
        // Blocked farming refuses before invoking even a legacy ID allocator.
        int allocations = 0;
        const auto blocked_allocate = [&](std::uint64_t&) {
            ++allocations;
            return true;
        };
        registry.ctx().emplace<F::FarmingReplayGate>().blocked = true;
        if (!simulation) {
            EXPECT_EQ(
                F::RunCropLifecycleRegional(registry, 100, gate, 0, blocked_allocate).reseeded, 0);
        }
        EXPECT_EQ(allocations, 0);
    }
    entt::registry registry;
    const auto parent = spawnRipe(
        registry, true, std::numeric_limits<std::uint32_t>::max(), {100, 0, 100}, RandomGenome(1));
    registry.get<C::CropLifecycleComponent>(parent).ripe_ticks =
        std::numeric_limits<std::uint32_t>::max();
    const std::array due{W::RegionKey{0, 0}};
    const std::map<W::RegionKey, std::uint32_t> windows{{due[0], 3}};
    const Gate gate(registry, due, W::RegionElapsedTicks(windows));
    EXPECT_EQ(F::RunCropLifecycleRegional(registry, std::numeric_limits<std::uint64_t>::max(), gate)
                  .reseeded,
              1);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 1u);
    for (const auto entity : registry.view<C::PlantTag>()) {
        if (entity != parent) {
            EXPECT_EQ(registry.get<C::PlantGrowthComponent>(entity).planted_tick,
                      std::numeric_limits<std::uint64_t>::max() - 2u);
        }
    }
}

} // namespace
