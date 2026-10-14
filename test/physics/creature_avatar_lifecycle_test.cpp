#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <gtest/gtest.h>
#include <vector>

namespace {
using Luminumbra::Systems::PhysicsSystem;
constexpr float kDt = 1.0f / 30.0f;

TEST(CreatureAvatarLifecycle, RemovalPreservesOtherHandles) {
    PhysicsSystem physics;
    physics.startup();
    const auto first =
        physics.create_avatar_handle({0.0f, 100.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, false);
    const auto other =
        physics.create_avatar_handle({10.0f, 100.0f, 0.0f}, {0.0f, -3.0f, 0.0f}, false);
    const auto before = physics.get_avatar_position(other);
    EXPECT_TRUE(physics.remove_avatar_handle(first));
    EXPECT_TRUE(physics.valid_avatar_handle(other));
    EXPECT_EQ(physics.get_avatar_position(other), before);
    EXPECT_EQ(physics.get_avatar_velocity(other).y, -3.0f);
    EXPECT_FALSE(physics.remove_avatar_handle(first));
}

TEST(CreatureAvatarLifecycle, ReusedStorageRejectsStaleHandles) {
    PhysicsSystem physics;
    physics.startup();
    const auto stale = physics.create_avatar_handle({0.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    ASSERT_TRUE(physics.remove_avatar_handle(stale));
    const auto fresh = physics.create_avatar_handle({10.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    EXPECT_NE(stale, fresh);
    EXPECT_FALSE(physics.valid_avatar_handle(stale));
    const auto before = physics.get_avatar_position(fresh);
    EXPECT_FALSE(physics.update_selected_avatars(std::array{fresh, stale}, kDt));
    EXPECT_EQ(physics.get_avatar_position(fresh), before);
    EXPECT_FALSE(physics.set_avatar_position(stale, {99.0f, 99.0f, 99.0f}));
    physics.set_avatar_wish_velocity(stale, {100.0f, 100.0f});
    ASSERT_TRUE(physics.update_selected_avatars(std::array{fresh}, kDt));
    EXPECT_EQ(physics.get_avatar_position(fresh).x, before.x);
    physics.clear_avatar_characters();
    EXPECT_FALSE(physics.valid_avatar_handle(fresh));
    EXPECT_NE(physics.create_avatar_handle({0.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false), fresh);
}

TEST(CreatureAvatarLifecycle, SelectedStepDoesNotAdvanceUnselectedAvatars) {
    PhysicsSystem physics;
    physics.startup();
    const auto first = physics.create_avatar_handle({0.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    const auto other = physics.create_avatar_handle({10.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    const auto legacy = physics.create_avatar_character({20.0f, 100.0f, 0.0f});
    const auto held = physics.get_avatar_position(other),
               player = physics.get_avatar_position(legacy);
    ASSERT_TRUE(physics.update_selected_avatars(std::array{first}, kDt));
    EXPECT_LT(physics.get_avatar_position(first).y, 100.0f);
    EXPECT_EQ(physics.get_avatar_position(other), held);
    EXPECT_EQ(physics.get_avatar_position(legacy), player);
}

TEST(CreatureAvatarLifecycle, MixedPlayerAndCreatureSelectionStepsEachExactlyOnce) {
    PhysicsSystem physics, reference;
    physics.startup();
    reference.startup();
    const auto player = physics.create_avatar_handle({0.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    const auto creature =
        physics.create_avatar_handle({10.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    const auto expected_player =
        reference.create_avatar_handle({0.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    const auto expected_creature =
        reference.create_avatar_handle({10.0f, 100.0f, 0.0f}, glm::vec3(0.0f), false);
    ASSERT_TRUE(
        physics.update_selected_avatars(std::array{player, creature, player, creature}, kDt));
    ASSERT_TRUE(reference.update_selected_avatars(std::array{expected_player}, kDt));
    ASSERT_TRUE(reference.update_selected_avatars(std::array{expected_creature}, kDt));
    EXPECT_EQ(physics.get_avatar_position(player), reference.get_avatar_position(expected_player));
    EXPECT_EQ(physics.get_avatar_position(creature),
              reference.get_avatar_position(expected_creature));
}

TEST(CreatureAvatarLifecycle, RestoredVerticalVelocityAndGroundedStateContinue) {
    PhysicsSystem physics;
    physics.startup();
    const auto falling =
        physics.create_avatar_handle({0.0f, 100.0f, 0.0f}, {2.0f, -7.0f, 3.0f}, false);
    const auto grounded =
        physics.create_avatar_handle({10.0f, 100.0f, 0.0f}, {2.0f, -7.0f, 3.0f}, true);
    EXPECT_EQ(physics.get_avatar_velocity(falling), (glm::vec3{2.0f, -7.0f, 3.0f}));
    EXPECT_FALSE(physics.is_avatar_grounded(falling));
    EXPECT_TRUE(physics.is_avatar_grounded(grounded));
    ASSERT_TRUE(physics.update_selected_avatars(std::array{falling, grounded}, kDt));
    EXPECT_NEAR(physics.get_avatar_velocity(falling).y, -7.0f - 9.81f * kDt, .0001f);
    EXPECT_FLOAT_EQ(physics.get_avatar_velocity(grounded).y, -1.0f);
    const auto position = physics.get_avatar_position(falling);
    const auto velocity = physics.get_avatar_velocity(falling);
    const bool on_ground = physics.is_avatar_grounded(falling);
    ASSERT_TRUE(physics.remove_avatar_handle(falling));
    const auto restored = physics.create_avatar_handle(position, velocity, on_ground);
    ASSERT_TRUE(physics.update_selected_avatars(std::array{restored}, kDt));
    EXPECT_NEAR(physics.get_avatar_velocity(restored).y, velocity.y - 9.81f * kDt, .0001f);
}

TEST(CreatureAvatarLifecycle, FrozenAndCoarseCreaturesHaveNoSteppedController) {
    namespace C = Luminumbra::Components;
    namespace W = Luminumbra::world;
    namespace fs = std::filesystem;
    const auto root = Luminumbra::TestSupport::CreateUniqueTempDirectory("creature_controllers_");
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(root, error);
        }
    } cleanup{root};
    fs::create_directories(root / "worlds/atlas/presets");
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                  root / "worlds/atlas/presets/default.json");
    Luminumbra::TestSupport::WriteSimulationContentFixture(root);
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    W::GameSession session;
    session.SetRootPath(root.generic_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetRegionSchedulerConfig({0, 1, 2});
    ASSERT_TRUE(session.CreateTransientWorld("Controller reconciliation", "1337", "default"))
        << session.GetWorldOpenError();
    const std::array<Luminumbra::Vec3, 3> positions{
        {{8.0f, 100.0f, 8.0f}, {5200.0f, 100.0f, 8.0f}, {10300.0f, 100.0f, 8.0f}}};
    session.SetReplicatedSimulationAnchors(
        std::vector<Luminumbra::Vec3>(positions.begin(), positions.end()));
    auto& r = session.GetRegistry();
    std::array<entt::entity, 3> creatures;
    for (std::size_t i = 0; i < creatures.size(); ++i) {
        session.NotifyGroundObjectEdit(positions[i]);
        creatures[i] = r.create();
        r.emplace<C::TransformComponent>(creatures[i]).position = positions[i];
        r.emplace<C::CreatureComponent>(creatures[i]).species_id =
            C::CreatureSpeciesId16("fixture_creature");
        ASSERT_TRUE(session.AdmitCreature(creatures[i], true));
        r.get<C::CreatureComponent>(creatures[i]).move_speed = 0.0f;
    }
    auto tick = [&] {
        EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        EXPECT_FALSE(session.GetRegionSchedulerFault());
    };
    tick();
    std::array<std::size_t, 3> handles;
    for (std::size_t i = 0; i < creatures.size(); ++i) {
        ASSERT_EQ(r.get<C::CreatureCoarseComponent>(creatures[i]).tier, C::CreatureTier::Full);
        ASSERT_TRUE(r.all_of<C::CreaturePhysicsComponent>(creatures[i]));
        handles[i] = r.get<C::CreaturePhysicsComponent>(creatures[i]).avatar_index;
        ASSERT_TRUE(session.GetPhysicsSystem()->valid_avatar_handle(handles[i]));
    }
    const auto held = r.get<C::TransformComponent>(creatures[2]).position;
    const auto continuation = r.get<C::CreatureLocomotionStateComponent>(creatures[2]);
    auto& records = const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(
        session.GetActiveRegionLedger().records());
    const auto freezing_owner = W::ActiveRegionLedger::region_at(positions[2]);
    // Seed the scheduler's Reduced precondition while the prior Full controller
    // is still live. The next session tick must freeze this owner before cleanup.
    auto& freezing = records.at(freezing_owner);
    freezing.state = W::RegionState::Reduced;
    freezing.wake_pending = false;
    freezing.cadence_shift = 2;
    freezing.cadence_phase = W::ActiveRegionLedger::phase(freezing_owner, 2);
    for (const auto& [key, record] : records) {
        (void)record;
        if (key != freezing_owner)
            session.PinActiveRegion(key, true);
    }
    session.RecordActiveRegionWork({freezing_owner, 1, 0, 0, 0});
    session.SetReplicatedSimulationAnchors({positions[0]});
    tick();
    EXPECT_EQ(records.at(freezing_owner).state, W::RegionState::Frozen);
    const W::RegionTransition frozen_transition{
        freezing_owner, W::RegionState::Reduced, W::RegionState::Frozen};
    EXPECT_NE(std::find(session.GetRegionSchedule().transitions.begin(),
                        session.GetRegionSchedule().transitions.end(),
                        frozen_transition),
              session.GetRegionSchedule().transitions.end());
    EXPECT_EQ(r.get<C::CreatureCoarseComponent>(creatures[1]).tier, C::CreatureTier::Coarse);
    EXPECT_EQ(r.get<C::CreatureCoarseComponent>(creatures[2]).tier, C::CreatureTier::Full);
    EXPECT_EQ(session.GetCreatureActivity().controllers_removed, 2u);
    EXPECT_EQ(session.GetCreatureActivity().demotions, 1u);
    for (std::size_t i : {1u, 2u}) {
        EXPECT_FALSE(r.all_of<C::CreaturePhysicsComponent>(creatures[i]));
        EXPECT_FALSE(session.GetPhysicsSystem()->valid_avatar_handle(handles[i]));
    }
    tick();
    for (std::size_t i : {1u, 2u}) {
        EXPECT_FALSE(r.all_of<C::CreaturePhysicsComponent>(creatures[i]));
        EXPECT_FALSE(session.GetPhysicsSystem()->valid_avatar_handle(handles[i]));
    }
    EXPECT_TRUE(session.GetPhysicsSystem()->valid_avatar_handle(handles[0]));
    EXPECT_EQ(r.get<C::CreaturePhysicsComponent>(creatures[0]).avatar_index, handles[0]);
    EXPECT_LT(r.get<C::TransformComponent>(creatures[0]).position.y, positions[0].y);
    EXPECT_EQ(r.get<C::TransformComponent>(creatures[2]).position, held);
    const auto& after = r.get<C::CreatureLocomotionStateComponent>(creatures[2]);
    EXPECT_EQ(after.velocity_x, continuation.velocity_x);
    EXPECT_EQ(after.velocity_y, continuation.velocity_y);
    EXPECT_EQ(after.velocity_z, continuation.velocity_z);
    EXPECT_EQ(after.grounded, continuation.grounded);
    r.clear();
}

TEST(CreatureAvatarLifecycle, FlagOffAvatarBehaviorMatchesLegacyIndexOracle) {
    PhysicsSystem physics;
    physics.startup();
    const auto first = physics.create_avatar_character({0.0f, 100.0f, 0.0f});
    const auto second = physics.create_avatar_character({10.0f, 100.0f, 0.0f});
    ASSERT_EQ(first, 0u);
    ASSERT_EQ(second, 1u);
    physics.set_avatar_wish_velocity(first, {2.0f, 3.0f});
    physics.set_avatar_wish_velocity(second, {-1.0f, 4.0f});
    float vertical_velocity = 0.0f, height = 100.0f;
    for (int tick = 1; tick <= 4; ++tick) {
        vertical_velocity -= 9.81f * kDt;
        height += vertical_velocity * kDt;
        physics.update_avatars(kDt);
        EXPECT_NEAR(physics.get_avatar_position(first).y, height, .0001f);
        EXPECT_NEAR(physics.get_avatar_position(second).y, height, .0001f);
        EXPECT_NEAR(physics.get_avatar_position(first).x, float(tick) * 2.0f * kDt, .0001f);
        EXPECT_NEAR(physics.get_avatar_position(second).x, 10.0f - float(tick) * kDt, .0001f);
        EXPECT_FLOAT_EQ(physics.get_avatar_velocity(first).y, vertical_velocity);
        EXPECT_FLOAT_EQ(physics.get_avatar_velocity(second).y, vertical_velocity);
    }
    physics.clear_avatar_characters();
    EXPECT_EQ(physics.create_avatar_character({0.0f, 0.0f, 0.0f}), 0u);
}
} // namespace
