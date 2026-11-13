#include <gtest/gtest.h>

#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FoliageRegistryStage.h"
#include "luminumbra_common/systems/PollinationSystem.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::RegionConsumerGate;
using Luminumbra::world::RegionKey;

entt::entity Parent(entt::registry& registry, std::uint64_t id) {
    const auto entity = registry.create();
    registry.emplace<C::PlantTag>(entity);
    registry.emplace<C::SimulationIdentityComponent>(entity, id);
    registry.emplace<C::TransformComponent>(entity).position =
        Luminumbra::Vec3(99.0f + static_cast<float>(id - 1u), 0, 100);
    registry.emplace<C::PlantGenomeComponent>(entity).genes.fill(static_cast<float>(id) * 0.2f);
    registry.emplace<C::PlantGrowthComponent>(entity).stage =
        static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    registry.emplace<C::CropLifecycleComponent>(
        entity,
        C::CropLifecycleComponent{.perennial = id == 2,
                                  .lifespan_ticks = 1,
                                  .species_id = static_cast<std::uint16_t>(id)});
    registry.emplace<C::PollinationTag>(entity);
    registry.emplace<C::PollinationComponent>(entity);
    if (id != 2) {
        registry.emplace<C::PlantRegionComponent>(entity,
                                                  C::PlantRegionComponent{.persistent_id = id});
        registry.emplace<C::GrazeableComponent>(entity, C::GrazeableComponent{0.25f, 6u});
    }
    return entity;
}

// An explicit observation of reproduction state; no shipping codec/hash is selected here.
struct PlantRecord {
    std::uint64_t id;
    std::array<float, C::kPlantGeneCount> genome;
    std::array<float, 3> position;
    std::uint8_t stage;
    std::uint32_t growth;
    std::uint32_t stress;
    std::uint64_t planted;
    std::uint64_t last_tick;
    bool perennial;
    std::uint32_t ripe_ticks;
    std::uint32_t lifespan;
    std::uint16_t generations;
    std::uint16_t species;
    bool pollinated;
    std::array<float, C::kPlantGeneCount> next_genome;
    std::uint64_t pollen_tick;
    std::uint32_t crosses;
    bool grazeable;
    float biomass;
    std::uint32_t grazed_tick;
    bool owned;
    std::uint64_t region_id;
    std::int32_t owner_x;
    std::int32_t owner_z;
    std::uint64_t stamp;
    bool operator==(const PlantRecord&) const = default;
};

std::vector<PlantRecord> Observe(const entt::registry& registry) {
    std::vector<entt::entity> plants;
    std::vector<entt::entity> scratch;
    for (const auto entity : registry.view<const C::PlantTag>())
        plants.push_back(entity);
    F::OrderPlantsBySimulationIdentity(registry, plants, scratch);
    std::vector<PlantRecord> records;
    for (const auto entity : plants) {
        const auto& growth = registry.get<C::PlantGrowthComponent>(entity);
        const auto& lifecycle = registry.get<C::CropLifecycleComponent>(entity);
        const auto& pollen = registry.get<C::PollinationComponent>(entity);
        const auto& position = registry.get<C::TransformComponent>(entity).position;
        const auto* grazing = registry.try_get<C::GrazeableComponent>(entity);
        const auto* region = registry.try_get<C::PlantRegionComponent>(entity);
        records.push_back({F::RequirePlantSimulationId(registry, entity),
                           registry.get<C::PlantGenomeComponent>(entity).genes,
                           {position.x, position.y, position.z},
                           growth.stage,
                           growth.growth_points,
                           growth.stress_points,
                           growth.planted_tick,
                           growth.last_tick,
                           lifecycle.perennial,
                           lifecycle.ripe_ticks,
                           lifecycle.lifespan_ticks,
                           lifecycle.generations,
                           lifecycle.species_id,
                           pollen.pollinated,
                           pollen.next_genome.genes,
                           pollen.last_pollen_tick,
                           pollen.crosses,
                           grazing != nullptr,
                           grazing ? grazing->biomass : 0.0f,
                           grazing ? grazing->last_grazed_tick : 0u,
                           region != nullptr,
                           region ? region->persistent_id : 0u,
                           region ? region->owner_x : 0,
                           region ? region->owner_z : 0,
                           region ? region->region_stamp : 0u});
    }
    return records;
}

std::vector<std::vector<PlantRecord>> Continue(bool regional, bool churn) {
    entt::registry registry;
    ActiveRegionLedger ledger;
    if (churn) {
        // Unrelated mesh residents, holes, reversed insertion and a recycled generation.
        for (int index = 0; index < 64; ++index) {
            const auto mesh = registry.create();
            registry.emplace<C::TransformComponent>(mesh);
            registry.emplace<C::PlantGenomeComponent>(mesh);
        }
        const auto hole = registry.create();
        registry.destroy(hole);
        for (const auto id : {3u, 2u, 1u})
            (void)Parent(registry, id);
    } else {
        for (const auto id : {1u, 2u, 3u})
            (void)Parent(registry, id);
    }
    EXPECT_TRUE(ledger.reserve_persistent_ids(4));
    std::uint64_t ground_id = 0;
    EXPECT_TRUE(ledger.allocate_persistent_id(ground_id));
    EXPECT_EQ(ground_id, 4u);
    F::FoliageRegistryStage stage;
    std::vector<std::vector<PlantRecord>> observations;
    for (std::uint64_t tick = 7; tick <= 9; ++tick) {
        stage.Begin(registry, ledger);
        auto& candidate = stage.registry();
        for (const auto entity : candidate.view<C::PlantTag>())
            candidate.get<C::PlantGrowthComponent>(entity).stage =
                static_cast<std::uint8_t>(C::PlantStage::Fruiting);
        const std::array due{RegionKey{0, 0}};
        std::vector<std::uint64_t> owners;
        const auto gate =
            regional ? RegionConsumerGate(
                           candidate, due, owners, RegionConsumerGate::PlantIdentity::Simulation)
                     : RegionConsumerGate(RegionConsumerGate::PlantIdentity::Simulation);
        const auto pollen = F::RunPollinationOnTick(candidate, tick, {}, 99, 0.05f, gate);
        EXPECT_GT(pollen.crossed, 0);
        const auto stats =
            F::RunCropLifecycleOnTick(candidate, tick, 99, gate, stage.identity_allocator());
        EXPECT_EQ(stats.reseeded, pollen.considered);
        if (tick == 7) {
            // Three simultaneous children: low parent identity gets the first shared ID.
            for (const auto child : candidate.view<C::PlantTag, C::SimulationIdentityComponent>()) {
                const auto child_id = F::RequirePlantSimulationId(candidate, child);
                if (child_id < 5)
                    continue;
                const auto parent_id = child_id - 4u;
                EXPECT_EQ(candidate.get<C::CropLifecycleComponent>(child).species_id, parent_id);
                auto rng = luminumbra::core::DeterministicRng::seeded(
                    F::kGerminationSeedOffset ^ 99u, parent_id, tick);
                const float angle = rng.next_unit() * 6.2831853f;
                const float radius = 1.0f + rng.next_unit() * 2.0f;
                const auto& pos = candidate.get<C::TransformComponent>(child).position;
                EXPECT_EQ(pos.x,
                          99.0f + static_cast<float>(parent_id - 1u) +
                              radius * Luminumbra::DeterministicMath::Cos(angle));
                EXPECT_EQ(pos.z, 100.0f + radius * Luminumbra::DeterministicMath::Sin(angle));
                EXPECT_EQ(candidate.all_of<C::PlantRegionComponent>(child), parent_id != 2);
                if (parent_id != 2) {
                    const auto& region = candidate.get<C::PlantRegionComponent>(child);
                    EXPECT_EQ(region.persistent_id, child_id);
                    EXPECT_EQ(region.region_stamp, tick);
                }
            }
            EXPECT_EQ(stage.next_id(), 8u);
        }
        stage.PreparePublication(registry);
        EXPECT_TRUE(ledger.reserve_persistent_ids(stage.next_id()));
        stage.Commit(registry);
        observations.push_back(Observe(registry));
        EXPECT_EQ(ledger.next_persistent_id(), stage.next_id());
    }
    return observations;
}

TEST(PlantReproductionContinuation, HandleHistoryAndRegionalSchedulingPreserveThreeGenerations) {
    const auto expected = Continue(false, false);
    EXPECT_EQ(Continue(false, true), expected);
    EXPECT_EQ(Continue(true, false), expected);
    EXPECT_EQ(Continue(true, true), expected);
}

TEST(PlantReproductionContinuation, LastAvailableIdentityGoesToTheLowestParent) {
    entt::registry registry;
    ActiveRegionLedger ledger;
    const auto high = Parent(registry, 3);
    const auto low = Parent(registry, 1);
    ASSERT_TRUE(ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit - 1));
    const RegionConsumerGate gate(RegionConsumerGate::PlantIdentity::Simulation);
    const auto stats = F::RunCropLifecycleOnTick(registry, 7, 99, gate, ledger);
    EXPECT_EQ(stats.reseeded, 1);
    EXPECT_EQ(stats.died, 1);
    EXPECT_FALSE(registry.valid(low));
    ASSERT_TRUE(registry.valid(high));
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(high).ripe_ticks, 0u);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(high).generations, 0u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(high).stage,
              static_cast<std::uint8_t>(C::PlantStage::Fruiting));
    EXPECT_EQ(ledger.next_persistent_id(), ActiveRegionLedger::kPersistentIdLimit);
    for (const auto child : registry.view<C::PlantTag>()) {
        if (child != high) {
            EXPECT_EQ(F::RequirePlantSimulationId(registry, child),
                      ActiveRegionLedger::kPersistentIdLimit - 1);
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(child).species_id, 1u);
        }
    }
}

TEST(PlantReproductionContinuation, UndueParentsAndBlockedDestinationsConsumeNoIdentity) {
    entt::registry registry;
    ActiveRegionLedger ledger;
    const auto parent = Parent(registry, 1);
    ASSERT_TRUE(ledger.reserve_persistent_ids(2));
    std::vector<std::uint64_t> owners;
    const RegionConsumerGate none_due(
        registry, {}, owners, RegionConsumerGate::PlantIdentity::Simulation);
    EXPECT_EQ(F::RunCropLifecycleOnTick(registry, 7, 99, none_due, ledger).reseeded, 0);
    registry.get<C::TransformComponent>(parent).position.x = 511.99f;
    const std::array due{RegionKey{0, 0}};
    // The deterministic search is bounded independently of world size.
    std::uint64_t outward_tick = 0;
    for (std::uint64_t tick = 1; tick <= 100; ++tick) {
        auto rng =
            luminumbra::core::DeterministicRng::seeded(F::kGerminationSeedOffset ^ 99u, 1u, tick);
        const float angle = rng.next_unit() * 6.2831853f;
        const float radius = 1.0f + rng.next_unit() * 2.0f;
        if (radius * Luminumbra::DeterministicMath::Cos(angle) > 0.1f) {
            outward_tick = tick;
            break;
        }
    }
    ASSERT_NE(outward_tick, 0u);
    const RegionConsumerGate blocked(
        registry, due, owners, RegionConsumerGate::PlantIdentity::Simulation);
    EXPECT_EQ(F::RunCropLifecycleOnTick(registry, outward_tick, 99, blocked, ledger).reseeded, 0);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 0u);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 0u);
    EXPECT_EQ(registry.view<C::PlantTag>().size(), 1u);
    const std::array both_due{RegionKey{0, 0}, RegionKey{1, 0}};
    const RegionConsumerGate admitted(
        registry, both_due, owners, RegionConsumerGate::PlantIdentity::Simulation);
    ASSERT_EQ(F::RunCropLifecycleOnTick(registry, outward_tick, 99, admitted, ledger).reseeded, 1);
    EXPECT_FALSE(registry.valid(parent));
    ASSERT_EQ(registry.view<C::PlantTag>().size(), 1u);
    const auto child = *registry.view<C::PlantTag>().begin();
    const auto& region = registry.get<C::PlantRegionComponent>(child);
    EXPECT_EQ(region.persistent_id, 2u);
    EXPECT_EQ(region.owner_x, 1);
    EXPECT_EQ(region.owner_z, 0);
    EXPECT_EQ(region.region_stamp, outward_tick);
    EXPECT_EQ(ledger.next_persistent_id(), 3u);
}

TEST(PlantReproductionContinuation, LifecycleVisitCountDependsOnlyOnParticipatingParents) {
    for (const int residents : {0, 4096}) {
        entt::registry registry;
        ActiveRegionLedger ledger;
        for (int index = 0; index < residents; ++index) {
            const auto mesh = registry.create();
            registry.emplace<C::TransformComponent>(mesh);
            registry.emplace<C::PlantGenomeComponent>(mesh);
        }
        (void)Parent(registry, 1);
        ASSERT_TRUE(ledger.reserve_persistent_ids(2));
        const std::array due{RegionKey{0, 0}};
        std::vector<std::uint64_t> owners;
        const RegionConsumerGate gate(
            registry, due, owners, RegionConsumerGate::PlantIdentity::Simulation);
        EXPECT_EQ(F::RunCropLifecycleOnTick(registry, 7, 99, gate, ledger).reseeded, 1);
        // One owner to build membership, one parent query, one destination query.
        EXPECT_EQ(gate.owner_visits(), 3u);
        EXPECT_EQ(ledger.next_persistent_id(), 3u);
    }
}

TEST(PlantReproductionContinuation, LegacyCallsIgnoreDormantSimulationIdentities) {
    entt::registry registry;
    const auto parent = Parent(registry, 1);
    const auto donor = Parent(registry, 3);
    // Common identity order is deliberately opposite to handle order.
    registry.get<C::SimulationIdentityComponent>(parent).id = 90;
    registry.get<C::SimulationIdentityComponent>(donor).id = 80;
    const auto expected = F::PollinateCross(registry.get<C::PlantGenomeComponent>(parent),
                                            registry.get<C::PlantGenomeComponent>(donor),
                                            entt::to_integral(parent),
                                            entt::to_integral(donor),
                                            7);
    ASSERT_EQ(F::RunPollinationOnTick(registry, 7).crossed, 2);
    EXPECT_EQ(registry.get<C::PollinationComponent>(parent).next_genome.genes, expected.genes);
    ASSERT_EQ(F::RunCropLifecycleOnTick(registry, 7).reseeded, 2);
    for (const auto child : registry.view<C::PlantTag>())
        EXPECT_FALSE(registry.all_of<C::SimulationIdentityComponent>(child));
}

} // namespace
