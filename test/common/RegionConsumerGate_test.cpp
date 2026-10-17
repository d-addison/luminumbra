#include "luminumbra_common/ai/LifespanSystem.h"
#include "luminumbra_common/ai/WildlifeFoliageSystem.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FireSpreadSystem.h"
#include "luminumbra_common/systems/PlantDiseaseSystem.h"
#include "luminumbra_common/systems/PollinationSystem.h"
#include "luminumbra_common/systems/RegionConsumerGate.h"
#include <gtest/gtest.h>

namespace {
namespace C = Luminumbra::Components;
TEST(RegionConsumerGate, SharedIdentityUsesCreatureAndPlantPersistentIdsOnlyWhenEnabled) {
    entt::registry a, b;
    const auto creature_a = a.create(), plant_a = a.create();
    const auto plant_b = b.create(), creature_b = b.create();
    a.emplace<C::CreatureRegionComponent>(creature_a).persistent_id = 42;
    b.emplace<C::CreatureRegionComponent>(creature_b).persistent_id = 42;
    a.emplace<C::PlantRegionComponent>(plant_a).persistent_id = 43;
    b.emplace<C::PlantRegionComponent>(plant_b).persistent_id = 43;
    using Luminumbra::world::RegionEntityIdentity;
    EXPECT_EQ(RegionEntityIdentity(a, creature_a, true), 42u);
    EXPECT_EQ(RegionEntityIdentity(b, creature_b, true), 42u);
    EXPECT_EQ(RegionEntityIdentity(a, plant_a, true), 43u);
    EXPECT_EQ(RegionEntityIdentity(b, plant_b, true), 43u);
    EXPECT_EQ(RegionEntityIdentity(a, creature_a, false), entt::to_integral(creature_a));
    EXPECT_EQ(RegionEntityIdentity(b, creature_b, false), entt::to_integral(creature_b));
    EXPECT_NE(RegionEntityIdentity(a, creature_a, false),
              RegionEntityIdentity(b, creature_b, false));
    const auto unowned = a.create();
    EXPECT_EQ(RegionEntityIdentity(a, unowned, true), entt::to_integral(unowned));
}

TEST(RegionConsumerGate, BlockedOwnerRetainsEveryLivingWorldCounter) {
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::PlantTag>(entity);
    registry.emplace<C::TransformComponent>(entity);
    registry.emplace<C::PlantGenomeComponent>(entity);
    auto& growth = registry.emplace<C::PlantGrowthComponent>(entity);
    growth.stage = static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    auto& crop = registry.emplace<C::CropLifecycleComponent>(entity);
    crop.perennial = true;
    crop.lifespan_ticks = 1;
    registry.emplace<C::PollinationTag>(entity);
    registry.emplace<C::PollinationComponent>(entity);
    auto& health = registry.emplace<C::PlantHealthComponent>(entity);
    health.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
    health.infection = 800;
    auto& fire = registry.emplace<C::CombustibleComponent>(entity);
    fire.burn_state = static_cast<std::uint8_t>(C::BurnState::Burning);
    fire.burn_ticks_remaining = 20;
    auto& biomass = registry.emplace<C::GrazeableComponent>(entity);
    biomass.biomass = 0.8f;
    registry.emplace<C::MortalComponent>(entity);
    const Luminumbra::world::RegionEntityGate blocked = [](entt::entity) {
        return false;
    };
    luminumbra::foliage::RunPollinationOnTick(registry, 100, {}, 0, 0.05f, blocked);
    luminumbra::foliage::RunPlantDiseaseOnTick(registry, 100, 0, blocked);
    luminumbra::foliage::RunCropLifecycleOnTick(registry, 100, 0, blocked);
    luminumbra::sim::RunFireSpreadOnTick(registry, 100, {}, {}, blocked);
    luminumbra::ai::RunWildlifeFoliageOnTick(registry, 100, {}, blocked);
    luminumbra::ai::RunLifespanOnTick(registry, 100, blocked);
    EXPECT_EQ(crop.ripe_ticks, 0u);
    EXPECT_EQ(crop.generations, 0u);
    EXPECT_EQ(health.infected_ticks, 0u);
    EXPECT_EQ(health.infection, 800u);
    EXPECT_EQ(fire.burn_ticks_remaining, 20u);
    EXPECT_EQ(biomass.biomass, 0.8f);
    EXPECT_EQ(registry.get<C::MortalComponent>(entity).age_ticks, 0u);
    EXPECT_EQ(registry.get<C::PollinationComponent>(entity).crosses, 0u);
    EXPECT_EQ(registry.view<C::PlantTag>().size(), 1u);

    // Positive controls use the existing legacy signatures with empty admission.
    luminumbra::foliage::RunPlantDiseaseOnTick(registry, 101);
    luminumbra::sim::RunFireSpreadOnTick(registry, 101);
    luminumbra::ai::RunWildlifeFoliageOnTick(registry, 101);
    luminumbra::ai::RunLifespanOnTick(registry, 101);
    EXPECT_GT(health.infected_ticks, 0u);
    EXPECT_EQ(fire.burn_ticks_remaining, 19u);
    EXPECT_GT(biomass.biomass, 0.8f);
    EXPECT_EQ(registry.get<C::MortalComponent>(entity).age_ticks, 1u);
    luminumbra::foliage::RunCropLifecycleOnTick(registry, 101);
    EXPECT_EQ(registry.get<C::CropLifecycleComponent>(entity).generations, 1u);
    EXPECT_EQ(registry.view<C::PlantTag>().size(), 2u);
}

TEST(RegionConsumerGate, BlockedEntitiesCannotTransmitDiseaseOrPollenToDueOwner) {
    entt::registry registry;
    const auto blocked = registry.create(), due = registry.create();
    for (const auto entity : {blocked, due}) {
        registry.emplace<C::PlantTag>(entity);
        registry.emplace<C::TransformComponent>(entity);
        registry.emplace<C::PlantGenomeComponent>(entity);
        registry.emplace<C::PlantGrowthComponent>(entity).stage =
            static_cast<std::uint8_t>(C::PlantStage::Flowering);
        registry.emplace<C::PollinationTag>(entity);
        registry.emplace<C::PollinationComponent>(entity);
        registry.emplace<C::PlantHealthComponent>(entity);
    }
    // Disease skips coincident positions as self exposure; use a nearby distinct recipient.
    registry.get<C::TransformComponent>(due).position.x = 1.0f;
    auto& sick = registry.get<C::PlantHealthComponent>(blocked);
    sick.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
    sick.infection = 1000;
    const Luminumbra::world::RegionEntityGate gate = [due](entt::entity e) {
        return e == due;
    };
    luminumbra::foliage::RunPlantDiseaseOnTick(registry, 1, 0, gate);
    luminumbra::foliage::RunPollinationOnTick(registry, 1, {}, 0, 0.05f, gate);
    EXPECT_EQ(registry.get<C::PlantHealthComponent>(due).infection, 0u);
    EXPECT_EQ(registry.get<C::PollinationComponent>(due).crosses, 0u);
    EXPECT_EQ(sick.infected_ticks, 0u);
    luminumbra::foliage::RunPlantDiseaseOnTick(registry, 2);
    luminumbra::foliage::RunPollinationOnTick(registry, 2);
    EXPECT_GT(registry.get<C::PlantHealthComponent>(due).infection, 0u);
    EXPECT_GT(registry.get<C::PollinationComponent>(due).crosses, 0u);
}
} // namespace
