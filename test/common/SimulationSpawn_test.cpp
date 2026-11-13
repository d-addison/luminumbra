#include <gtest/gtest.h>

#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
using Session = Luminumbra::world::GameSession;
using Source = Session::SpawnSource;
using Ledger = Luminumbra::world::ActiveRegionLedger;
using Identity = Luminumbra::Components::SimulationIdentityComponent;
namespace F = luminumbra::foliage;

std::uint64_t Id(const entt::registry& registry, entt::entity entity) {
    return registry.get<Identity>(entity).id;
}

TEST(SimulationSpawn, AllSourcesReserveFromOneCursorInBoundaryOrder) {
    entt::registry registry;
    Ledger ledger;
    registry.ctx().emplace<Session::SimulationSpawns>(ledger);
    constexpr auto count = static_cast<std::size_t>(Source::Count);
    std::array<entt::entity, count> entities{};
    // Producer completion order is the reverse of the defined boundary order.
    for (std::size_t index = count; index != 0; --index) {
        entities[index - 1] =
            Session::CreateSimulationParticipant(registry, static_cast<Source>(index - 1));
    }
    const auto second_wildlife = Session::CreateSimulationParticipant(registry, Source::Wildlife);
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    for (const auto entity : entities) {
        EXPECT_EQ(Id(registry, entity), 0u);
    }
    Session::PublishSimulationSpawns(registry);
    EXPECT_EQ(Id(registry, entities[0]), 1u);
    EXPECT_EQ(Id(registry, entities[1]), 2u);
    EXPECT_EQ(Id(registry, second_wildlife), 3u);
    for (std::size_t index = 2; index < count; ++index) {
        EXPECT_EQ(Id(registry, entities[index]), index + 2u);
    }
    std::uint64_t ground_id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(ground_id));
    EXPECT_EQ(ground_id, count + 2u);
    const auto plant = F::PlantSeed(registry, {}, {}, 1u, 1u, ledger);
    ASSERT_TRUE(plant != entt::null);
    EXPECT_EQ(Id(registry, plant), ground_id + 1u);
    const auto cursor = ledger.next_persistent_id();
    registry.clear();
    Session::PublishSimulationSpawns(registry);
    EXPECT_EQ(ledger.next_persistent_id(), cursor);
    const auto next = Session::CreateSimulationParticipant(registry, Source::Boot);
    Session::PublishSimulationSpawns(registry);
    EXPECT_EQ(Id(registry, next), cursor);
}

TEST(SimulationSpawn, TickPublishesBeforeParticipationAndZeroTickDoesNotPublish) {
    Session session;
    session.PrepareSimulationCreation();
    auto& registry = session.GetRegistry();
    const auto entity = Session::CreateSimulationParticipant(registry, Source::Boot);
    EXPECT_THROW(session.PrepareSimulationCreation(), std::logic_error);
    EXPECT_FALSE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(0.0), 0u);
    EXPECT_FALSE(session.IsSimulationTickBoundary());
    EXPECT_EQ(Id(registry, entity), 0u);
    EXPECT_EQ(session.TickSimulation(1.0 / 30.0), 1u);
    EXPECT_EQ(Id(registry, entity), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), 2u);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
}

TEST(SimulationSpawn, ExhaustionRefusesTheWholeBatchAndAllowsRetryAfterCandidateRemoval) {
    entt::registry registry;
    Ledger ledger;
    ASSERT_TRUE(ledger.reserve_persistent_ids(Ledger::kPersistentIdLimit - 1));
    registry.ctx().emplace<Session::SimulationSpawns>(ledger);
    const auto first = Session::CreateSimulationParticipant(registry, Source::Plants);
    const auto second = Session::CreateSimulationParticipant(registry, Source::Creatures);
    const auto before = ledger.encode();
    EXPECT_THROW(Session::PublishSimulationSpawns(registry), std::length_error);
    EXPECT_EQ(ledger.encode(), before);
    EXPECT_EQ(Id(registry, first), 0u);
    EXPECT_EQ(Id(registry, second), 0u);
    registry.destroy(second);
    Session::PublishSimulationSpawns(registry);
    EXPECT_EQ(Id(registry, first), Ledger::kPersistentIdLimit - 1);
    const auto refused = Session::CreateSimulationParticipant(registry, Source::Scenario);
    EXPECT_THROW(Session::PublishSimulationSpawns(registry), std::length_error);
    EXPECT_EQ(Id(registry, refused), 0u);
    EXPECT_EQ(ledger.next_persistent_id(), Ledger::kPersistentIdLimit);
}

TEST(SimulationSpawn, InvalidChangedAndRepeatedRequestsRefuseWithoutPublishing) {
    entt::registry registry;
    Ledger ledger;
    registry.ctx().emplace<Session::SimulationSpawns>(ledger);
    const auto entity = Session::CreateSimulationParticipant(registry, Source::Boot);
    EXPECT_THROW(Session::QueueSimulationParticipant(registry, entity, Source::Boot),
                 std::invalid_argument);
    EXPECT_THROW((void)Session::CreateSimulationParticipant(registry, Source::Count),
                 std::invalid_argument);
    EXPECT_THROW(Session::QueueSimulationParticipant(registry, entt::null, Source::Boot),
                 std::invalid_argument);
    registry.get<Identity>(entity).id = 7;
    EXPECT_THROW(Session::PublishSimulationSpawns(registry), std::runtime_error);
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    registry.remove<Identity>(entity);
    EXPECT_THROW(Session::QueueSimulationParticipant(registry, entity, Source::Boot),
                 std::invalid_argument);
    EXPECT_THROW(Session::PublishSimulationSpawns(registry), std::runtime_error);
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    registry.emplace<Identity>(entity, 0u);
    Session::PublishSimulationSpawns(registry);
    EXPECT_EQ(Id(registry, entity), 1u);
    EXPECT_THROW(Session::QueueSimulationParticipant(registry, entity, Source::Boot),
                 std::invalid_argument);
    const auto retired = Session::CreateSimulationParticipant(registry, Source::Scenario);
    registry.destroy(retired);
    Session::PublishSimulationSpawns(registry);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
}

TEST(SimulationSpawn, AvatarOrderSurvivesHandlePermutationAndRejectsInvalidIdentity) {
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        Ledger ledger;
        ASSERT_TRUE(ledger.reserve_persistent_ids(4));
        registry.ctx().emplace<Session::SimulationSpawns>(ledger);
        std::vector<entt::entity> avatars;
        EXPECT_NO_THROW(Session::OrderSimulationAvatars(registry, avatars));
        for (const auto id : reverse ? std::array{3u, 2u, 1u} : std::array{1u, 2u, 3u}) {
            const auto entity = registry.create();
            registry.emplace<Identity>(entity, id);
            avatars.push_back(entity);
        }
        Session::OrderSimulationAvatars(registry, avatars);
        for (std::size_t index = 0; index < avatars.size(); ++index) {
            EXPECT_EQ(Id(registry, avatars[index]), index + 1u);
        }
        registry.get<Identity>(avatars[1]).id = 1;
        EXPECT_THROW(Session::OrderSimulationAvatars(registry, avatars), std::runtime_error);
        registry.get<Identity>(avatars[1]).id = 4;
        EXPECT_THROW(Session::OrderSimulationAvatars(registry, avatars), std::runtime_error);
        registry.get<Identity>(avatars[1]).id = 0;
        EXPECT_THROW(Session::OrderSimulationAvatars(registry, avatars), std::runtime_error);
        registry.remove<Identity>(avatars[1]);
        EXPECT_THROW(Session::OrderSimulationAvatars(registry, avatars), std::runtime_error);
    }
}

TEST(SimulationSpawn, MeshChurnAndRetiredCandidatesDoNotConsumeIdsOrBoundaryVisits) {
    for (const int meshes : {0, 1000}) {
        entt::registry registry;
        Ledger ledger;
        auto& spawns = registry.ctx().emplace<Session::SimulationSpawns>(ledger);
        const auto retired = Session::CreateSimulationParticipant(registry, Source::Boot);
        registry.destroy(retired);
        // Recycled slots and unrelated residents must never be discovered as participants.
        for (int index = 0; index < meshes; ++index) {
            registry.emplace<Luminumbra::Components::StaticMeshComponent>(registry.create());
        }
        const auto creature = Session::CreateSimulationParticipant(registry, Source::Wildlife);
        const auto plant = Session::QueueSimulationParticipant(
            registry, F::PlantSeed(registry, {}, {}, 1u, 0u), Source::Plants);
        Session::PublishSimulationSpawns(registry);
        EXPECT_EQ(Id(registry, creature), 1u);
        EXPECT_EQ(Id(registry, plant), 2u);
        EXPECT_EQ(spawns.publication_visits(), 6u);
        std::vector<entt::entity> avatars{plant, creature};
        Session::OrderSimulationAvatars(registry, avatars);
        const auto visits = spawns.avatar_visits();
        EXPECT_EQ(avatars.front(), creature);
        EXPECT_LE(visits, 20u * avatars.size());
        Session::PublishSimulationSpawns(registry);
        EXPECT_EQ(spawns.publication_visits(), 6u);
        EXPECT_EQ(ledger.next_persistent_id(), 3u);
    }
}

TEST(SimulationSpawn, WrappedRenderHandleCannotReviveARetiredRequest) {
    entt::registry registry;
    Ledger ledger;
    registry.ctx().emplace<Session::SimulationSpawns>(ledger);
    const auto retired = Session::CreateSimulationParticipant(registry, Source::Boot);
    registry.destroy(retired);
    // EnTT skips the tombstone version; the first destruction already advanced once.
    constexpr auto cycles = entt::entt_traits<entt::entity>::version_mask - 1u;
    for (std::uint32_t index = 0; index < cycles; ++index) {
        const auto mesh = registry.create();
        registry.emplace<Luminumbra::Components::StaticMeshComponent>(mesh);
        registry.destroy(mesh);
    }
    const auto creature = Session::CreateSimulationParticipant(registry, Source::Wildlife);
    ASSERT_EQ(creature, retired);
    Session::PublishSimulationSpawns(registry);
    EXPECT_EQ(Id(registry, creature), 1u);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
}

TEST(SimulationSpawn, UnselectedFactoriesPreserveLegacyPlantBytesCursorAndAvatarOrder) {
    entt::registry legacy;
    entt::registry shipping;
    Ledger ledger;
    const auto before = ledger.encode();
    const auto expected_creature = legacy.create();
    const auto creature = Session::CreateSimulationParticipant(shipping, Source::Wildlife);
    EXPECT_EQ(creature, expected_creature);
    const auto expected_plant = F::PlantSeed(legacy, {}, {}, 1u, 0u);
    const auto plant = Session::QueueSimulationParticipant(
        shipping, F::PlantSeed(shipping, {}, {}, 1u, 0u), Source::Plants);
    EXPECT_EQ(plant, expected_plant);
    Session::PublishSimulationSpawns(shipping);
    EXPECT_TRUE(shipping.view<const Identity>().empty());
    EXPECT_EQ(ledger.encode(), before);
    EXPECT_EQ(
        Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(shipping)),
        Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(legacy)));
    std::vector<entt::entity> avatars{plant, creature};
    Session::OrderSimulationAvatars(shipping, avatars);
    EXPECT_EQ(avatars.front(), creature);
}
} // namespace
