#include <gtest/gtest.h>

#include "ai/ForagingSystem.h"
#include "ai/ScavengingSystem.h"
#include "ai/ThirstSystem.h"
#include "components/MortalComponents.h"
#include "components/SimulationIdentityComponent.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace {
namespace A = luminumbra::ai;
namespace C = Luminumbra::Components;
namespace W = Luminumbra::world;

constexpr entt::entity kNullEntity = entt::null;
// Exercise all eight ID bytes, including values above the runtime handle width.
constexpr std::uint64_t kLowId = 0x0102030405060708ull;
constexpr std::uint64_t kHighId = 0x0201030405060708ull;

entt::entity Identified(entt::registry& registry, std::uint64_t id, float x = 100) {
    const auto entity = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(entity, id);
    registry.emplace<C::TransformComponent>(entity).position = {x, 0, 100};
    registry.emplace<C::CreatureComponent>(entity);
    return entity;
}

void ChurnHandles(entt::registry& registry) {
    std::array<entt::entity, 20> scenery{};
    for (auto& entity : scenery)
        entity = registry.create();
    // Retain unrelated mesh residents and holes, and recycle a handle repeatedly.
    for (std::size_t i = 0; i < scenery.size(); i += 2)
        registry.destroy(scenery[i]);
    for (int i = 0; i < 5; ++i)
        registry.destroy(registry.create());
}

const A::CreatureTickAdmission*
CaptureIfEnabled(entt::registry& registry, A::CreatureTickAdmission& admission, bool enabled) {
    if (!enabled)
        return nullptr;
    const std::array<W::RegionKey, 1> due{{{0, 0}}};
    admission.Capture(registry, W::RegionConsumerGate(registry, due));
    return &admission;
}

TEST(ResourceOrder, WaterTiesAndSharedWaterIgnoreHandlesAndAdmissionMode) {
    for (bool regional : {false, true}) {
        for (bool overlapping : {false, true}) {
            std::array<int, 4> order{0, 1, 2, 3};
            do {
                entt::registry registry;
                ChurnHandles(registry);
                std::array<entt::entity, 4> entities{
                    kNullEntity, kNullEntity, kNullEntity, kNullEntity};
                for (auto role : order) {
                    const auto index = static_cast<std::size_t>(role);
                    const auto id = role == 0 ? kHighId : kLowId + index;
                    const auto entity = Identified(registry, id);
                    entities[index] = entity;
                    if (role < 2) {
                        registry.get<C::TransformComponent>(entity).position.x =
                            role == 0 ? 90.0f : 110.0f;
                        registry.emplace<C::WaterHoleComponent>(entity).radius =
                            role == 1 && overlapping ? 12.0f : 1.0f;
                    } else {
                        registry.emplace<C::ThirstComponent>(entity).thirst = 0.8f;
                    }
                }
                A::CreatureTickAdmission admission;
                const auto* gate = CaptureIfEnabled(registry, admission, regional);
                const auto stats = A::RunThirstOnTickV2(registry, 0.0f, {}, gate);
                EXPECT_EQ(stats.participants, 2);
                EXPECT_EQ(stats.identity_order_visits, 64u);
                EXPECT_EQ(stats.seeking, overlapping ? 0 : 2);
                EXPECT_EQ(stats.drinking, overlapping ? 2 : 0);
                for (std::size_t i = 2; i < entities.size(); ++i) {
                    ASSERT_NE(entities[i], kNullEntity);
                    const auto& thirst = registry.get<C::ThirstComponent>(entities[i]);
                    EXPECT_FLOAT_EQ(thirst.wish_x, overlapping ? 0.0f : 0.8f);
                    EXPECT_FLOAT_EQ(thirst.wish_z, 0);
                    EXPECT_EQ(thirst.drinking, overlapping ? 1 : 0);
                }
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
}

TEST(ResourceOrder, CarcassTiesIgnoreStorageAndRegionalPositionTieRules) {
    for (bool regional : {false, true}) {
        for (bool in_range : {false, true}) {
            std::array<int, 4> order{0, 1, 2, 3};
            do {
                entt::registry registry;
                ChurnHandles(registry);
                std::array<entt::entity, 4> entities{
                    kNullEntity, kNullEntity, kNullEntity, kNullEntity};
                for (auto role : order) {
                    const auto index = static_cast<std::size_t>(role);
                    const auto entity = Identified(registry, role == 0 ? kHighId : kLowId + index);
                    entities[index] = entity;
                    if (role < 2) {
                        const float distance = in_range ? 1.0f : 10.0f;
                        registry.get<C::TransformComponent>(entity).position.x =
                            100.0f + (role == 0 ? -distance : distance);
                        if (role == 0)
                            registry.get<C::CreatureComponent>(entity).eaten = true;
                        else
                            registry.emplace<C::MortalComponent>(entity).dead = 1;
                    } else {
                        registry.emplace<C::ScavengerComponent>(entity);
                        registry.get<C::CreatureComponent>(entity).hunger = 0.9f;
                    }
                }
                A::CreatureTickAdmission admission;
                const auto* gate = CaptureIfEnabled(registry, admission, regional);
                const auto stats = A::RunScavengingOnTickV2(registry, 0, {}, gate);
                EXPECT_EQ(stats.scavengers, 2);
                EXPECT_EQ(stats.identity_order_visits, 32u);
                EXPECT_EQ(stats.carcasses, 2);
                EXPECT_EQ(stats.seeking, 2);
                EXPECT_EQ(stats.feeding, in_range ? 2 : 0);
                for (std::size_t i = 2; i < entities.size(); ++i) {
                    const auto& scavenger = registry.get<C::ScavengerComponent>(entities[i]);
                    EXPECT_FLOAT_EQ(scavenger.wish_x, in_range ? 0.0f : 1.0f);
                    EXPECT_FLOAT_EQ(scavenger.wish_z, 0);
                    EXPECT_EQ(scavenger.feeding, in_range ? 1 : 0);
                    EXPECT_FLOAT_EQ(registry.get<C::CreatureComponent>(entities[i]).hunger,
                                    in_range ? 0.9f - A::kScavengeFeedRate : 0.9f);
                }
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
}

TEST(ResourceOrder, FoodDistanceTiesUseSourceIdentity) {
    for (bool regional : {false, true}) {
        for (bool reverse : {false, true}) {
            entt::registry registry;
            ChurnHandles(registry);
            const auto ant = Identified(registry, 0x7fffffffffffffffull);
            registry.emplace<C::ForagerComponent>(ant, 4, 4, 4, 4, false, 0u);
            for (int i = 0; i < 2; ++i) {
                const bool low = (i == 0) == reverse;
                const auto food = Identified(registry, low ? kLowId : kHighId);
                registry.emplace<C::FoodSourceComponent>(food, low ? 5 : 3, 4, 1);
            }
            A::ScentField field(9, 9, 4);
            A::CreatureTickAdmission admission;
            const auto* gate = CaptureIfEnabled(registry, admission, regional);
            const auto stats = A::RunForagingOnTickV2(registry, field, {1, 0, 1}, gate);
            EXPECT_EQ(stats.foragers, 1u);
            EXPECT_EQ(stats.identity_order_visits, 32u);
            EXPECT_EQ(stats.pickups, 1u);
            const auto& state = registry.get<C::ForagerComponent>(ant);
            EXPECT_EQ(state.cell_x, 5);
            EXPECT_EQ(state.cell_z, 4);
            EXPECT_TRUE(state.carrying_food);
        }
    }
}

TEST(ResourceOrder, ContestedFoodOrdersBothForagersAndColocatedSourcesByIdentity) {
    for (bool regional : {false, true}) {
        std::array<int, 4> order{0, 1, 2, 3};
        do {
            entt::registry registry;
            ChurnHandles(registry);
            std::array<entt::entity, 4> entities{
                kNullEntity, kNullEntity, kNullEntity, kNullEntity};
            for (auto role : order) {
                const auto index = static_cast<std::size_t>(role);
                const auto entity =
                    Identified(registry, role % 2 == 0 ? kHighId + index : kLowId + index);
                entities[index] = entity;
                if (role < 2)
                    registry.emplace<C::ForagerComponent>(entity, 4, 4, 4, 4, false, 0u);
                else
                    registry.emplace<C::FoodSourceComponent>(entity, 5, 4, 1);
            }
            A::ScentField field(9, 9, 4);
            A::CreatureTickAdmission admission;
            const auto* gate = CaptureIfEnabled(registry, admission, regional);
            const auto stats = A::RunForagingOnTickV2(registry, field, {1, 0, 1}, gate);
            // Both reach the same cell, but only the first forager gets the first source's
            // single unit. Preserve the existing first-source arrival/depletion semantics.
            EXPECT_EQ(stats.foragers, 2u);
            EXPECT_EQ(stats.identity_order_visits, 64u);
            EXPECT_EQ(stats.pickups, 1u);
            EXPECT_TRUE(registry.get<C::ForagerComponent>(entities[1]).carrying_food);
            EXPECT_FALSE(registry.get<C::ForagerComponent>(entities[0]).carrying_food);
            EXPECT_EQ(registry.get<C::FoodSourceComponent>(entities[3]).amount, 0);
            EXPECT_EQ(registry.get<C::FoodSourceComponent>(entities[2]).amount, 1);
            EXPECT_DOUBLE_EQ(field.Sample(A::kHomeTrailChannel, 4, 4), 2);
        } while (std::next_permutation(order.begin(), order.end()));
    }
}

TEST(ResourceOrder, IdentityModesWithEmptyRostersRemainNoOps) {
    entt::registry registry;
    ChurnHandles(registry);
    A::ScentField field(9, 9, 4);
    const auto thirst = A::RunThirstOnTickV2(registry, 1.0f);
    const auto scavenging = A::RunScavengingOnTickV2(registry, 0);
    const auto foraging = A::RunForagingOnTickV2(registry, field);
    EXPECT_EQ(thirst.participants, 0);
    EXPECT_EQ(thirst.identity_order_visits, 0u);
    EXPECT_EQ(scavenging.identity_order_visits, 0u);
    EXPECT_EQ(foraging.identity_order_visits, 0u);
    EXPECT_EQ(scavenging.scavengers, 0);
    EXPECT_EQ(scavenging.carcasses, 0);
    EXPECT_EQ(foraging.foragers, 0u);
    EXPECT_EQ(foraging.pickups, 0u);
    EXPECT_EQ(foraging.deliveries, 0u);
    EXPECT_DOUBLE_EQ(field.Sample(A::kHomeTrailChannel, 4, 4), 0);
}

TEST(ResourceOrder, IdentityTiesNeverOverrideNearerResources) {
    entt::registry registry;
    const auto actor = Identified(registry, 1);
    registry.emplace<C::ThirstComponent>(actor).thirst = 0.8f;
    registry.emplace<C::ScavengerComponent>(actor);
    registry.get<C::CreatureComponent>(actor).hunger = 0.9f;
    registry.emplace<C::ForagerComponent>(actor, 4, 4, 4, 4, false, 0u);
    for (bool near : {false, true}) {
        const auto source = Identified(registry, near ? kHighId : kLowId, near ? 98.0f : 110.0f);
        registry.get<C::CreatureComponent>(source).eaten = true;
        registry.emplace<C::WaterHoleComponent>(source).radius = 0;
        registry.emplace<C::FoodSourceComponent>(source, near ? 3 : 7, 4, 1);
    }
    A::RunThirstOnTickV2(registry, 0.0f);
    A::RunScavengingOnTickV2(registry, 0);
    A::ScentField field(9, 9, 4);
    const auto foraging = A::RunForagingOnTickV2(registry, field, {1, 0, 1});
    EXPECT_LT(registry.get<C::ThirstComponent>(actor).wish_x, 0);
    EXPECT_FLOAT_EQ(registry.get<C::ScavengerComponent>(actor).wish_x, -1);
    EXPECT_EQ(registry.get<C::ForagerComponent>(actor).cell_x, 3);
    EXPECT_EQ(foraging.pickups, 1u);
}

TEST(ResourceOrder, DeniedSourcesAndParticipantsKeepAdmissionSemantics) {
    for (bool deny_actor : {false, true}) {
        entt::registry registry;
        const auto actor = Identified(registry, kHighId, deny_actor ? 600.0f : 100.0f);
        auto& thirst = registry.emplace<C::ThirstComponent>(actor);
        thirst.thirst = 0.8f;
        thirst.wish_x = 0.75f;
        thirst.drinking = 1;
        auto& scavenger = registry.emplace<C::ScavengerComponent>(actor);
        scavenger.wish_x = 0.75f;
        scavenger.feeding = 1;
        registry.get<C::CreatureComponent>(actor).hunger = 0.9f;
        registry.emplace<C::ForagerComponent>(actor, 4, 4, 4, 4, false, 0u);
        // One denied source carries all three resource roles and only one identity.
        const auto source = Identified(registry, kLowId, 600);
        registry.get<C::CreatureComponent>(source).eaten = true;
        registry.emplace<C::WaterHoleComponent>(source).radius = 1000;
        registry.emplace<C::FoodSourceComponent>(source, 5, 4, 1);
        A::CreatureTickAdmission admission;
        const auto* gate = CaptureIfEnabled(registry, admission, true);
        A::ScentField field(9, 9, 4);
        const auto th = A::RunThirstOnTickV2(registry, 0.0f, {}, gate);
        const auto sc = A::RunScavengingOnTickV2(registry, 0, {}, gate);
        const auto fo = A::RunForagingOnTickV2(registry, field, {1, 0, 1}, gate);
        EXPECT_EQ(th.participants, deny_actor ? 0 : 1);
        EXPECT_EQ(th.drinking, 0);
        EXPECT_EQ(sc.scavengers, deny_actor ? 0 : 1);
        EXPECT_EQ(sc.carcasses, 0);
        EXPECT_EQ(sc.feeding, 0);
        EXPECT_EQ(fo.foragers, deny_actor ? 0u : 1u);
        EXPECT_EQ(fo.pickups, 0u);
        EXPECT_FLOAT_EQ(thirst.wish_x, deny_actor ? 0.75f : 0.0f);
        EXPECT_FLOAT_EQ(scavenger.wish_x, deny_actor ? 0.75f : 0.0f);
        EXPECT_EQ(thirst.drinking, deny_actor ? 1 : 0);
        EXPECT_EQ(scavenger.feeding, deny_actor ? 1 : 0);
        EXPECT_EQ(registry.get<C::FoodSourceComponent>(source).amount, 1);
        EXPECT_DOUBLE_EQ(field.Sample(A::kHomeTrailChannel, 4, 4), deny_actor ? 0 : 1);
    }
}

TEST(ResourceOrder, EveryIdentityByteParticipatesInResourceOrdering) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        for (bool reverse : {false, true}) {
            entt::registry registry;
            const auto actor = Identified(registry, 0x7fffffffffffffffull);
            registry.emplace<C::ThirstComponent>(actor).thirst = 0.8f;
            registry.emplace<C::ForagerComponent>(actor, 4, 4, 4, 4, false, 0u);
            // For nonzero shifts the lower byte deliberately disagrees with the decisive
            // byte. Omitting any byte pass must therefore select the wrong source.
            const auto low_id = shift == 0 ? 1ull : (1ull << shift) + 255ull;
            const auto high_id = shift == 0 ? 2ull : (2ull << shift) + 1ull;
            for (int i = 0; i < 2; ++i) {
                const bool low = (i == 0) == reverse;
                const auto source = Identified(registry, low ? low_id : high_id, low ? 110 : 90);
                registry.emplace<C::WaterHoleComponent>(source).radius = 1;
                registry.emplace<C::FoodSourceComponent>(source, low ? 5 : 3, 4, 1);
            }
            A::RunThirstOnTickV2(registry, 0.0f);
            EXPECT_GT(registry.get<C::ThirstComponent>(actor).wish_x, 0);
            A::ScentField field(9, 9, 4);
            A::RunForagingOnTickV2(registry, field, {1, 0, 1});
            EXPECT_EQ(registry.get<C::ForagerComponent>(actor).cell_x, 5);
        }
    }
}

TEST(ResourceOrder, LegacyWrappersKeepBothAdmissionModeTieRules) {
    for (bool regional : {false, true}) {
        entt::registry registry;
        const auto actor = Identified(registry, 1);
        registry.emplace<C::ThirstComponent>(actor).thirst = 0.8f;
        registry.emplace<C::ScavengerComponent>(actor);
        registry.get<C::CreatureComponent>(actor).hunger = 0.9f;
        registry.emplace<C::ForagerComponent>(actor, 4, 4, 4, 4, false, 0u);
        for (bool first : {true, false}) {
            const auto source = Identified(registry, first ? kHighId : kLowId, first ? 90 : 110);
            registry.get<C::CreatureComponent>(source).eaten = true;
            registry.emplace<C::WaterHoleComponent>(source).radius = 1;
            registry.emplace<C::FoodSourceComponent>(source, first ? 3 : 5, 4, 1);
        }
        A::CreatureTickAdmission admission;
        const auto* gate = CaptureIfEnabled(registry, admission, regional);
        const auto th = A::RunThirstOnTick(registry, 0.0f, {}, gate);
        const auto sc = A::RunScavengingOnTick(registry, 0, {}, gate);
        A::ScentField field(9, 9, 4);
        const auto fo = A::RunForagingOnTick(registry, field, {1, 0, 1}, gate);
        EXPECT_EQ(th.identity_order_visits, 0u);
        EXPECT_EQ(sc.identity_order_visits, 0u);
        EXPECT_EQ(fo.identity_order_visits, 0u);
        EXPECT_LT(registry.get<C::ThirstComponent>(actor).wish_x, 0);
        EXPECT_FLOAT_EQ(registry.get<C::ScavengerComponent>(actor).wish_x, regional ? -1 : 1);
        EXPECT_EQ(registry.get<C::ForagerComponent>(actor).cell_x, 3);
    }
}

} // namespace
