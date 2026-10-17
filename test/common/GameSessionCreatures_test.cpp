#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/DecayComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/components/MortalComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace W = Luminumbra::world;
namespace A = luminumbra::ai;
namespace fs = std::filesystem;

class CreatureWorld {
public:
    fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("creature_session_");
    Luminumbra::JobSystem jobs;
    W::GameSession session;
    explicit CreatureWorld(std::uint16_t promotion = 512,
                           std::uint32_t travel = 0,
                           std::uint32_t lifespan = 0,
                           std::uint32_t reproduction = 0,
                           bool transient = true) {
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        fs::create_directories(root / "data/common/sim");
        nlohmann::json policy{{"schema", "luminumbra.distant_policy.v1"},
                              {"creature_species",
                               {{"fixture_creature",
                                 {{"coarse_need_rate_ppm", 0u},
                                  {"coarse_travel_m_per_cadence", travel},
                                  {"promote_distance_m", promotion},
                                  {"lifespan_cadences", lifespan},
                                  {"reproduce_cadences", reproduction}}}}},
                              {"plant_species", nlohmann::json::object()}};
        std::ofstream(root / "data/common/sim/distant_policy.json") << policy.dump();
        jobs.startup(1);
        session.SetRootPath(root.generic_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        const bool created =
            transient ? session.CreateTransientWorld("Creature continuation", "1337", "default")
                      : session.CreateWorld("Creature continuation", "1337", "default");
        if (!created)
            throw std::runtime_error(session.GetWorldOpenError());
        session.SetReplicatedSimulationAnchors({{8.0f, 0.0f, 8.0f}});
    }
    ~CreatureWorld() {
        session.GetRegistry().clear();
        std::error_code error;
        fs::remove_all(root, error);
    }
    entt::entity Add(Luminumbra::Vec3 position = {8.0f, 0.0f, 8.0f}, bool physics = false) {
        session.NotifyGroundObjectEdit(position);
        auto& registry = session.GetRegistry();
        const auto e = registry.create();
        registry.emplace<C::TransformComponent>(e).position = position;
        registry.emplace<C::CreatureComponent>(e).species_id =
            C::CreatureSpeciesId16("fixture_creature");
        if (!session.AdmitCreature(e, physics))
            throw std::runtime_error("Creature admission refused");
        registry.get<C::CreatureComponent>(e).move_speed = 0.0f;
        return e;
    }
    void Tick() {
        if (session.TickSimulation(session.GetSimulationClock().fixed_dt()) != 1 ||
            session.GetRegionSchedulerFault())
            throw std::runtime_error("Creature tick fault");
    }
    C::CreatureCoarseComponent& Coarse(entt::entity e) {
        return session.GetRegistry().get<C::CreatureCoarseComponent>(e);
    }
    void State(W::RegionState state, std::uint8_t shift = 0) {
        auto& ledger = const_cast<W::ActiveRegionLedger&>(session.GetActiveRegionLedger());
        auto& record =
            const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(ledger.records()).at({0, 0});
        record.state = state;
        record.wake_pending = false;
        record.cadence_shift = shift;
        record.cadence_phase = W::ActiveRegionLedger::phase({0, 0}, shift);
        record.under_hold = 0;
    }
    std::string Bytes() {
        return A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(session.GetRegistry()));
    }
};

void ExpectRecoverySaveAndContinuation(CreatureWorld& world,
                                       const std::vector<Luminumbra::Vec3>& anchors) {
    world.session.GetSimulationEventBus().subscribe([](const auto& event) {
        if (event.topic == "creature.domain.failure")
            throw std::runtime_error("Consumer failure after movement");
    });
    world.session.GetSimulationEventBus().publish(
        world.session.GetWorldClock().tick() + 1, "creature.domain.failure", "");
    EXPECT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    EXPECT_NO_THROW(world.Bytes());
    ASSERT_TRUE(world.session.SaveWorld());
    W::GameSession restored;
    restored.SetRootPath(world.root.generic_string());
    restored.SetJobSystem(&world.jobs);
    restored.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(restored.LoadWorld(world.session.GetMetadata().worldId))
        << restored.GetWorldOpenError();
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored.GetRegistry())),
              world.Bytes());
    restored.SetReplicatedSimulationAnchors(anchors);
    EXPECT_EQ(restored.TickSimulation(restored.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(restored.GetRegionSchedulerFault());
    auto& r = restored.GetRegistry();
    for (auto e : r.view<C::CreatureRegionComponent, C::TransformComponent>()) {
        const auto position = r.get<C::TransformComponent>(e).position;
        const auto& owner = r.get<C::CreatureRegionComponent>(e);
        EXPECT_TRUE(W::ActiveRegionLedger::valid_anchor(position));
        EXPECT_GE(position.y, -1048576.0f);
        EXPECT_LT(position.y, 1048576.0f);
        EXPECT_EQ(W::ActiveRegionLedger::region_at(position),
                  (W::RegionKey{owner.owner_x, owner.owner_z}));
        if (const auto* runtime = r.try_get<C::CreaturePhysicsComponent>(e))
            EXPECT_EQ(restored.GetPhysicsSystem()->get_avatar_position(runtime->avatar_index),
                      position);
    }
    EXPECT_NO_THROW(A::BuildCreatureSnapshot(r));
    EXPECT_TRUE(restored.SaveWorld());
    r.clear();
}

entt::entity RestoreCreatureRegistry(CreatureWorld& world, entt::entity creature) {
    auto& r = world.session.GetRegistry();
    const auto id = r.get<C::CreatureRegionComponent>(creature).persistent_id;
    const auto snapshot = A::BuildCreatureSnapshot(r);
    r.clear();
    A::ApplyCreatureSnapshot(r, snapshot);
    for (auto e : r.view<C::CreatureRegionComponent>())
        if (r.get<C::CreatureRegionComponent>(e).persistent_id == id)
            return e;
    throw std::runtime_error("Restored creature missing");
}

void CheckFullCornerMovement(bool physics,
                             const Luminumbra::Vec3& start,
                             const Luminumbra::Vec3& endpoint,
                             std::optional<W::RegionKey> sealed,
                             bool frozen,
                             bool blocked) {
    SCOPED_TRACE(physics);
    SCOPED_TRACE(frozen);
    SCOPED_TRACE(::testing::Message() << "sealed owner: " << (sealed ? sealed->x : -1) << ","
                                      << (sealed ? sealed->z : -1));
    CreatureWorld world(1024);
    // Within promotion range, beyond the visitation disc of the corner owners.
    world.session.SetReplicatedSimulationAnchors({{-39.0f, 100.0f, -39.0f}});
    const auto e = world.Add(start, physics);
    const auto delta = endpoint - start;
    // A power-of-two scale preserves the quoted heading's float components.
    const auto predator = world.Add(start - 1024.0f * delta);
    auto& r = world.session.GetRegistry();
    r.get<C::CreatureComponent>(predator).is_predator = true;
    const auto dt = static_cast<float>(world.session.GetSimulationClock().fixed_dt());
    r.get<C::CreatureComponent>(e).move_speed =
        std::sqrt(delta.x * delta.x + delta.z * delta.z) / (1.5f * dt);
    A::EcologyTuning tuning;
    tuning.catch_radius = 0.0f;
    world.session.SetEcologyTuning(tuning);
    world.session.PinActiveRegion({0, 0}, true);
    for (const W::RegionKey key : {W::RegionKey{1, 0}, {0, 1}, {1, 1}}) {
        if (sealed && key == *sealed) {
            if (frozen) {
                world.session.NotifyGroundObjectEdit(
                    {float(key.x) * 512.0f + 8.0f, 100.0f, float(key.z) * 512.0f + 8.0f});
                auto& record = const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(
                                   world.session.GetActiveRegionLedger().records())
                                   .at(key);
                record.state = W::RegionState::Frozen;
                record.wake_pending = false;
                record.under_hold = 0;
                record.cadence_shift = 2;
                record.cadence_phase = W::ActiveRegionLedger::phase(key, 2);
            }
        } else {
            world.session.PinActiveRegion(key, true);
        }
    }
    world.Tick();
    ASSERT_EQ(world.Coarse(e).tier, C::CreatureTier::Full);
    const auto& creature = r.get<C::CreatureComponent>(e);
    // Ensure this really proposes the requested segment in both movement lanes.
    ASSERT_EQ(start.x + creature.wish_x * dt, endpoint.x);
    ASSERT_EQ(start.z + creature.wish_z * dt, endpoint.z);
    const auto position = r.get<C::TransformComponent>(e).position;
    const auto& owner = r.get<C::CreatureRegionComponent>(e);
    if (blocked) {
        EXPECT_GE(position.x, start.x - .001f);
        EXPECT_GE(position.z, start.z - .001f);
        EXPECT_LT(position.x, 512.0f);
        EXPECT_LT(position.z, 512.0f);
        EXPECT_EQ(owner.owner_x, 0);
        EXPECT_EQ(owner.owner_z, 0);
    } else {
        EXPECT_EQ(position.x, endpoint.x);
        EXPECT_EQ(position.z, endpoint.z);
        EXPECT_EQ(owner.owner_x, 1);
        EXPECT_EQ(owner.owner_z, 1);
    }
    if (sealed) {
        const auto& records = world.session.GetActiveRegionLedger().records();
        if (frozen) {
            ASSERT_TRUE(records.contains(*sealed));
            EXPECT_EQ(records.at(*sealed).state, W::RegionState::Frozen);
        } else {
            EXPECT_FALSE(records.contains(*sealed));
        }
    }
    EXPECT_EQ(W::ActiveRegionLedger::region_at(position),
              (W::RegionKey{owner.owner_x, owner.owner_z}));
    EXPECT_TRUE(W::ActiveRegionLedger::valid_anchor(position));
    if (physics) {
        ASSERT_TRUE(r.all_of<C::CreaturePhysicsComponent>(e));
        EXPECT_EQ(world.session.GetPhysicsSystem()->get_avatar_position(
                      r.get<C::CreaturePhysicsComponent>(e).avatar_index),
                  position);
    }
    EXPECT_NO_THROW(world.Bytes());
}

TEST(GameSessionCreatures, PromotionUsesInclusiveThresholdAndValidatedSimulationAnchors) {
    CreatureWorld world;
    const auto e = world.Add();
    world.Tick();
    world.Coarse(e).tier = C::CreatureTier::Coarse;
    world.session.SetReplicatedSimulationAnchors(
        {{520.0f, 1000.0f, 8.0f}, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}});
    world.Tick();
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Full);
    world.Coarse(e).tier = C::CreatureTier::Coarse;
    world.session.SetReplicatedSimulationAnchors({{520.001f, 0.0f, 8.0f}});
    world.Tick();
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Coarse);
}

TEST(GameSessionCreatures, DemotionUsesHysteresisAndRegionStatePrecedence) {
    CreatureWorld world(1024);
    const auto e = world.Add();
    world.Tick();
    world.session.SetReplicatedSimulationAnchors({{1096.0f, 0.0f, 8.0f}});
    world.Tick();
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Full);
    world.session.SetReplicatedSimulationAnchors({{1096.001f, 0.0f, 8.0f}});
    world.Tick();
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Coarse);
    world.Coarse(e).tier = C::CreatureTier::Full;
    world.State(W::RegionState::Reduced, 2);
    // An anchor in the neighboring owner remains within promotion range.
    world.session.SetReplicatedSimulationAnchors({{1025.0f, 0.0f, 8.0f}});
    world.Tick();
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Coarse);
}

TEST(GameSessionCreatures, ZeroPromotionDistanceDisablesPromotion) {
    CreatureWorld world(0);
    const auto e = world.Add();
    world.Tick();
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Coarse);
    EXPECT_EQ(world.Coarse(e).age_cadences, 1u);
    world.Tick();
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Coarse);
    EXPECT_EQ(world.Coarse(e).age_cadences, 2u);
}

TEST(GameSessionCreatures, FrozenCleanupDoesNotChangePersistedTierOrContinuation) {
    CreatureWorld world;
    const auto e = world.Add({8.0f, 100.0f, 8.0f}, true);
    world.Tick();
    auto& r = world.session.GetRegistry();
    ASSERT_TRUE(r.all_of<C::CreaturePhysicsComponent>(e));
    const auto handle = r.get<C::CreaturePhysicsComponent>(e).avatar_index;
    world.State(W::RegionState::Frozen);
    world.session.SetReplicatedSimulationAnchors({});
    const auto before = world.Bytes();
    world.Tick();
    EXPECT_EQ(world.Bytes(), before);
    EXPECT_FALSE(r.all_of<C::CreaturePhysicsComponent>(e));
    EXPECT_FALSE(world.session.GetPhysicsSystem()->valid_avatar_handle(handle));
    EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Full);
}

TEST(GameSessionCreatures, FullAndCoarseBridgesRoundValidateAndPreserveOtherBits) {
    CreatureWorld world(0);
    const auto e = world.Add({-0.0625f, -0.0625f, 8.0f});
    auto& r = world.session.GetRegistry();
    auto& cr = r.get<C::CreatureComponent>(e);
    cr.hunger = 0.0625f;
    cr.energy = -0.0f;
    r.emplace<C::NeedsComponent>(e).needs = {{"rest", .0625f, .001f}};
    world.Tick();
    EXPECT_EQ(world.Coarse(e).x_mm, -63);
    EXPECT_EQ(world.Coarse(e).y_mm, -63);
    EXPECT_EQ(world.Coarse(e).hunger_ppm, 62500u);
    ASSERT_EQ(world.Coarse(e).needs.size(), 1u);
    EXPECT_EQ(world.Coarse(e).needs.front().name, "rest");
    EXPECT_EQ(world.Coarse(e).needs.front().pressure_ppm, 62500u);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(cr.energy), 0x80000000u);
    EXPECT_EQ(r.get<C::NeedsComponent>(e).needs.front().growth_per_tick, .001f);
    const auto invalid = r.create();
    r.emplace<C::CreatureComponent>(invalid).species_id =
        C::CreatureSpeciesId16("fixture_creature");
    r.emplace<C::TransformComponent>(invalid).position.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(world.session.AdmitCreature(invalid));
}

TEST(GameSessionCreatures, FullAndCoarseCountersSurviveRepeatedTierChanges) {
    CreatureWorld world;
    const auto e = world.Add();
    auto& g = world.session.GetRegistry().emplace<C::CreatureGenomeComponent>(e);
    g.age_ticks = 17;
    for (std::uint64_t pass = 1; pass <= 3; ++pass) {
        world.session.SetReplicatedSimulationAnchors({});
        world.Tick();
        EXPECT_EQ(g.age_ticks, 16u + pass);
        EXPECT_EQ(world.Coarse(e).age_cadences, pass);
        world.session.SetReplicatedSimulationAnchors({{8.0f, 0.0f, 8.0f}});
        world.Tick();
        EXPECT_EQ(g.age_ticks, 17u + pass);
        EXPECT_EQ(world.Coarse(e).age_cadences, pass);
    }
}

TEST(GameSessionCreatures, CoarseKernelRunsOnceAndTransfersPublishAfterThePass) {
    CreatureWorld world(0, 10);
    const auto e = world.Add({511.0f, 0.0f, 8.0f});
    world.session.NotifyGroundObjectEdit({520.0f, 0.0f, 8.0f});
    auto& coarse = world.Coarse(e);
    coarse.travel_pending = true;
    coarse.destination_x = 1;
    world.Tick();
    EXPECT_EQ(coarse.age_cadences, 1u);
    EXPECT_EQ(coarse.x_mm, 521000);
    const auto& owner = world.session.GetRegistry().get<C::CreatureRegionComponent>(e);
    EXPECT_EQ(owner.owner_x, 1);
    EXPECT_EQ(owner.region_stamp, 1u);
    const auto& work = world.session.GetActiveRegionLedger().completed_work();
    ASSERT_TRUE(work.contains({0, 0}));
    EXPECT_EQ(work.at({0, 0}), 2u);
    EXPECT_FALSE(work.contains({1, 0}));
}

TEST(GameSessionCreatures, CoarseBirthsAreAdmittedInMotherOrderAfterThePass) {
    CreatureWorld world(0, 0, 0, 1);
    std::array<entt::entity, 4> parents;
    for (std::size_t i = 0; i < parents.size(); ++i) {
        parents[i] = world.Add({8.0f + float(i / 2) * 20.0f, 0.0f, 8.0f});
        auto& genome = world.session.GetRegistry().emplace<C::CreatureGenomeComponent>(parents[i]);
        genome.female = (i % 2) == 0;
        genome.age_ticks = 777;
        auto& coarse = world.Coarse(parents[i]);
        coarse.age_cadences = 90;
        coarse.reproduce_cadences = 1;
        coarse.courting_cadences = 2;
    }
    world.Tick();
    auto& r = world.session.GetRegistry();
    EXPECT_EQ(r.view<C::CreatureComponent>().size(), 6u);
    for (auto e : r.view<C::CreatureRegionComponent>()) {
        const auto& owner = r.get<C::CreatureRegionComponent>(e);
        if (owner.persistent_id <= 4)
            continue;
        EXPECT_EQ(owner.region_stamp, 1u);
        EXPECT_EQ(world.Coarse(e).age_cadences, 0u);
        EXPECT_EQ(r.get<C::CreatureGenomeComponent>(e).age_ticks, 0u);
        EXPECT_EQ(world.Coarse(e).x_mm, owner.persistent_id == 5 ? 8000 : 28000);
    }
}

TEST(GameSessionCreatures, CoarseDeathAndDecayHaveOneOwner) {
    CreatureWorld world(0, 0, 1);
    const auto e = world.Add();
    auto& r = world.session.GetRegistry();
    auto& mortal = r.emplace<C::MortalComponent>(e);
    mortal.age_ticks = 22;
    auto& decay = r.emplace<C::DecayComponent>(e);
    world.Tick();
    EXPECT_TRUE(r.get<C::CreatureComponent>(e).eaten);
    EXPECT_EQ(mortal.dead, 1u);
    EXPECT_EQ(mortal.age_ticks, 22u);
    EXPECT_EQ(decay.decay_ticks, 0u);
    world.Tick();
    EXPECT_EQ(decay.decay_ticks, 1u);
    EXPECT_EQ(world.Coarse(e).age_cadences, 1u);
}

TEST(GameSessionCreatures, ControllerFalseRestoresKinematicAndTrueRestoresPhysics) {
    CreatureWorld world;
    const auto kinematic = world.Add(), controlled = world.Add({20.0f, 100.0f, 8.0f}, true);
    auto& r = world.session.GetRegistry();
    r.get<C::CreatureLocomotionStateComponent>(controlled).velocity_y = -7.0f;
    const auto saved = A::BuildCreatureSnapshot(r);
    r.clear();
    A::ApplyCreatureSnapshot(r, saved);
    world.Tick();
    for (auto e : r.view<C::CreatureControllerComponent>()) {
        const bool physics = r.get<C::CreatureControllerComponent>(e).physics_controlled;
        EXPECT_EQ(r.all_of<C::CreaturePhysicsComponent>(e), physics);
        if (physics)
            EXPECT_LT(r.get<C::CreatureLocomotionStateComponent>(e).velocity_y, -7.0f);
    }
    EXPECT_NE(kinematic, controlled);
}

TEST(GameSessionCreatures, FullMovementCannotCrossAbsentOrFrozenOwners) {
    for (bool frozen : {false, true}) {
        CreatureWorld world;
        const auto e = world.Add({511.0f, 0.0f, 8.0f});
        const auto predator = world.Add({510.0f, 0.0f, 8.0f});
        auto& r = world.session.GetRegistry();
        r.get<C::CreatureComponent>(predator).is_predator = true;
        A::EcologyTuning tuning;
        tuning.catch_radius = 0.0f;
        world.session.SetEcologyTuning(tuning);
        // Within the Full hysteresis band, beyond the scheduling disc of owner (1,0).
        world.session.SetReplicatedSimulationAnchors({{-39.0f, 0.0f, 8.0f}});
        world.Tick();
        world.session.NotifyGroundObjectEdit({1540.0f, 0.0f, 8.0f});
        if (frozen) {
            world.session.NotifyGroundObjectEdit({520.0f, 0.0f, 8.0f});
            auto& records = const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(
                world.session.GetActiveRegionLedger().records());
            records.at({1, 0}).state = W::RegionState::Frozen;
            records.at({1, 0}).wake_pending = false;
        }
        r.get<C::CreatureComponent>(e).move_speed = 90000.0f;
        world.Tick();
        const auto position = r.get<C::TransformComponent>(e).position;
        const auto& owner = r.get<C::CreatureRegionComponent>(e);
        EXPECT_EQ(owner.owner_x, 0);
        EXPECT_GE(position.x, 511.0f);
        EXPECT_LT(position.x, 512.0f);
        EXPECT_GT(r.get<C::CreatureComponent>(e).wish_x, 1000.0f);
        EXPECT_EQ(W::ActiveRegionLedger::region_at(position),
                  (W::RegionKey{owner.owner_x, owner.owner_z}));
        if (!frozen)
            EXPECT_FALSE(world.session.GetActiveRegionLedger().records().contains({1, 0}));
    }
}

TEST(GameSessionCreatures, FullNearCornerMovementUsesFloatCrossingOrder) {
    const Luminumbra::Vec3 start{511.99951171875f, 100.0f, 511.9999084472656f};
    const Luminumbra::Vec3 endpoint{512.001708984375f, 100.0f, 512.00103759765625f};
    for (bool physics : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(
            CheckFullCornerMovement(physics, start, endpoint, std::nullopt, false, false));
        for (bool frozen : {false, true}) {
            // The true segment visits (0,1), whereas rounded endpoints visit (1,0).
            ASSERT_NO_FATAL_FAILURE(CheckFullCornerMovement(
                physics, start, endpoint, W::RegionKey{0, 1}, frozen, true));
            ASSERT_NO_FATAL_FAILURE(CheckFullCornerMovement(
                physics, start, endpoint, W::RegionKey{1, 0}, frozen, false));
        }
    }
}

TEST(GameSessionCreatures, FullExactCornerMovementChecksBothAdjacentOwners) {
    const Luminumbra::Vec3 start{511.99951171875f, 100.0f, 511.99951171875f};
    const Luminumbra::Vec3 endpoint{512.001708984375f, 100.0f, 512.001708984375f};
    for (bool physics : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(
            CheckFullCornerMovement(physics, start, endpoint, std::nullopt, false, false));
        for (bool frozen : {false, true})
            for (const W::RegionKey sealed : {W::RegionKey{1, 0}, {0, 1}, {1, 1}})
                ASSERT_NO_FATAL_FAILURE(
                    CheckFullCornerMovement(physics, start, endpoint, sealed, frozen, true));
    }
}

TEST(GameSessionCreatures, FullMovementAtWorldEdgeIsRefusedBeforePublicationAndRecoverySave) {
    for (bool physics : {false, true}) {
        SCOPED_TRACE(physics);
        CreatureWorld world(512, 0, 0, 0, false);
        const Luminumbra::Vec3 edge{16777215.0f, 100.0f, 8.0f};
        world.session.SetReplicatedSimulationAnchors({edge});
        const auto e = world.Add(edge, physics);
        const auto predator = world.Add({16777214.0f, 100.0f, 8.0f});
        auto& r = world.session.GetRegistry();
        r.get<C::CreatureComponent>(predator).is_predator = true;
        r.get<C::CreatureComponent>(e).move_speed = 60.0f;
        A::EcologyTuning tuning;
        tuning.catch_radius = 0.0f;
        world.session.SetEcologyTuning(tuning);
        world.Tick();
        EXPECT_GT(r.get<C::CreatureComponent>(e).wish_x, 0.0f);
        EXPECT_EQ(r.get<C::TransformComponent>(e).position, edge);
        EXPECT_TRUE(W::ActiveRegionLedger::valid_anchor(r.get<C::TransformComponent>(e).position));
        EXPECT_EQ(r.get<C::CreatureRegionComponent>(e).owner_x, 32767);
        if (physics) {
            ASSERT_TRUE(r.all_of<C::CreaturePhysicsComponent>(e));
            EXPECT_EQ(world.session.GetPhysicsSystem()->get_avatar_position(
                          r.get<C::CreaturePhysicsComponent>(e).avatar_index),
                      edge);
        }
        EXPECT_NO_THROW(world.Bytes());
        // A later consumer failure must still leave a valid recovery snapshot.
        world.session.GetSimulationEventBus().subscribe([](const auto& event) {
            if (event.topic == "creature.edge.failure")
                throw std::runtime_error("Consumer failure after edge movement");
        });
        world.session.GetSimulationEventBus().publish(2, "creature.edge.failure", "");
        EXPECT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_TRUE(world.session.GetRegionSchedulerFault());
        EXPECT_NO_THROW(world.Bytes());
        ASSERT_TRUE(world.session.SaveWorld());
        W::GameSession restored;
        restored.SetRootPath(world.root.generic_string());
        restored.SetJobSystem(&world.jobs);
        restored.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(restored.LoadWorld(world.session.GetMetadata().worldId))
            << restored.GetWorldOpenError();
        EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored.GetRegistry())),
                  world.Bytes());
    }
}

TEST(GameSessionCreatures, RestoredRoundedBoundaryStationaryCreatureIsAMovementNoOp) {
    CreatureWorld world(512, 0, 0, 0, false);
    const Luminumbra::Vec3 boundary{std::nextafter(512.0f, 0.0f), 100.0f, 8.0f};
    const std::vector<Luminumbra::Vec3> anchors{{1025.0f, 100.0f, 8.0f}};
    world.session.SetReplicatedSimulationAnchors(anchors);
    const auto admitted = world.Add(boundary);
    // Full snapshots permit this old rounded projection independently of owner 0.
    world.Coarse(admitted).x_mm = 512000;
    const auto e = RestoreCreatureRegistry(world, admitted);
    auto& r = world.session.GetRegistry();
    for (int tick = 0; tick < 3; ++tick) {
        world.Tick();
        EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Full);
        const auto position = r.get<C::TransformComponent>(e).position;
        EXPECT_EQ(std::bit_cast<std::uint32_t>(position.x),
                  std::bit_cast<std::uint32_t>(boundary.x));
        EXPECT_EQ(position, boundary);
        EXPECT_EQ(r.get<C::CreatureRegionComponent>(e).owner_x, 0);
        EXPECT_FALSE(world.session.GetActiveRegionLedger().records().contains({-1, 0}));
        // Unchanged Full state must not be repeatedly reprojected.
        EXPECT_EQ(world.Coarse(e).x_mm, 512000);
        EXPECT_NO_THROW(world.Bytes());
    }
    ExpectRecoverySaveAndContinuation(world, anchors);
}

TEST(GameSessionCreatures, RestoredRoundedBoundaryMovementSkipsZeroDeltaAndAbsentNegativeOwner) {
    for (bool physics : {false, true}) {
        for (float speed : {3.0f, 90000.0f}) {
            SCOPED_TRACE(physics);
            SCOPED_TRACE(speed);
            CreatureWorld world(512, 0, 0, 0, false);
            const Luminumbra::Vec3 boundary{std::nextafter(512.0f, 0.0f), 100.0f, 8.0f};
            const std::vector<Luminumbra::Vec3> anchors{{1025.0f, 100.0f, 8.0f}};
            world.session.SetReplicatedSimulationAnchors(anchors);
            const auto admitted = world.Add(boundary, physics);
            const auto predator = world.Add({boundary.x, 100.0f, 7.0f});
            auto& r = world.session.GetRegistry();
            r.get<C::CreatureComponent>(predator).is_predator = true;
            r.get<C::CreatureComponent>(admitted).move_speed = speed;
            world.Coarse(admitted).x_mm = 512000;
            A::EcologyTuning tuning;
            tuning.catch_radius = 0.0f;
            world.session.SetEcologyTuning(tuning);
            const auto e = RestoreCreatureRegistry(world, admitted);
            world.Tick();
            const auto position = r.get<C::TransformComponent>(e).position;
            const auto& owner = r.get<C::CreatureRegionComponent>(e);
            EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Full);
            EXPECT_FLOAT_EQ(r.get<C::CreatureComponent>(e).wish_x, 0.0f);
            EXPECT_GT(r.get<C::CreatureComponent>(e).wish_z, 0.0f);
            EXPECT_GT(position.z, boundary.z);
            EXPECT_LT(position.z, 512.0f);
            EXPECT_EQ(owner.owner_x, 0);
            EXPECT_TRUE(W::ActiveRegionLedger::valid_anchor(position));
            EXPECT_EQ(W::ActiveRegionLedger::region_at(position),
                      (W::RegionKey{owner.owner_x, owner.owner_z}));
            EXPECT_FALSE(world.session.GetActiveRegionLedger().records().contains({-1, 0}));
            EXPECT_FALSE(world.session.GetActiveRegionLedger().records().contains({0, 1}));
            EXPECT_NO_THROW(world.Bytes());
            ExpectRecoverySaveAndContinuation(world, anchors);
        }
    }
}

TEST(GameSessionCreatures, RoundedBoundaryDemotionPreservesOwnerAndControllerContinuation) {
    for (bool physics : {false, true}) {
        SCOPED_TRACE(physics);
        CreatureWorld world(512, 0, 0, 0, false);
        const auto admitted = world.Add({std::nextafter(512.0f, 0.0f), 100.0f, 8.0f}, physics);
        world.Coarse(admitted).x_mm = 512000;
        if (physics)
            world.session.GetRegistry()
                .get<C::CreatureLocomotionStateComponent>(admitted)
                .velocity_y = -7.0f;
        const auto e = RestoreCreatureRegistry(world, admitted);
        world.session.SetReplicatedSimulationAnchors({{1025.0f, 100.0f, 8.0f}});
        world.Tick();
        auto& r = world.session.GetRegistry();
        world.session.SetReplicatedSimulationAnchors({});
        const auto continuation =
            physics ? r.get<C::CreatureLocomotionStateComponent>(e).velocity_y : 0.0f;
        world.Tick();
        EXPECT_EQ(world.Coarse(e).tier, C::CreatureTier::Coarse);
        EXPECT_EQ(world.Coarse(e).x_mm, 511999);
        EXPECT_EQ(r.get<C::CreatureRegionComponent>(e).owner_x, 0);
        EXPECT_FALSE(r.all_of<C::CreaturePhysicsComponent>(e));
        if (physics)
            EXPECT_EQ(r.get<C::CreatureLocomotionStateComponent>(e).velocity_y, continuation);
        EXPECT_NO_THROW(world.Bytes());
        ExpectRecoverySaveAndContinuation(world, {});
    }
}

TEST(GameSessionCreatures, ControllerVerticalLimitsAreRefusedBeforePublicationAndRecoverySave) {
    const float upper = std::nextafter(1048576.0f, 0.0f);
    for (float height : {-1048576.0f, upper}) {
        SCOPED_TRACE(height);
        CreatureWorld world(512, 0, 0, 0, false);
        const Luminumbra::Vec3 boundary{8.0f, height, 8.0f};
        const auto admitted = world.Add(boundary, true);
        auto& r = world.session.GetRegistry();
        auto& state = r.get<C::CreatureLocomotionStateComponent>(admitted);
        state.velocity_y = height < 0.0f ? -7.0f : 7.0f;
        state.grounded = false;
        const auto e = RestoreCreatureRegistry(world, admitted);
        for (int tick = 0; tick < 3; ++tick) {
            world.Tick();
            const auto position = r.get<C::TransformComponent>(e).position;
            EXPECT_EQ(position, boundary);
            ASSERT_TRUE(r.all_of<C::CreaturePhysicsComponent>(e));
            const auto handle = r.get<C::CreaturePhysicsComponent>(e).avatar_index;
            EXPECT_EQ(world.session.GetPhysicsSystem()->get_avatar_position(handle), boundary);
            EXPECT_EQ(r.get<C::CreatureLocomotionStateComponent>(e).velocity_y,
                      world.session.GetPhysicsSystem()->get_avatar_velocity(handle).y);
            EXPECT_GE(position.y, -1048576.0f);
            EXPECT_LT(position.y, 1048576.0f);
            EXPECT_NO_THROW(world.Bytes());
        }
        ExpectRecoverySaveAndContinuation(world, {{8.0f, 0.0f, 8.0f}});
    }
}

TEST(GameSessionCreatures, TickBatchingDoesNotChangeTierOrCreatureResults) {
    CreatureWorld single(0), batched(0);
    single.Add();
    batched.Add();
    for (int i = 0; i < 4; ++i)
        single.Tick();
    for (int i = 0; i < 2; ++i)
        ASSERT_EQ(
            batched.session.TickSimulation(2.0 * batched.session.GetSimulationClock().fixed_dt()),
            2u);
    EXPECT_EQ(single.Bytes(), batched.Bytes());
    EXPECT_EQ(single.session.GetActiveRegionLedger().canonical_bytes(),
              batched.session.GetActiveRegionLedger().canonical_bytes());
}

TEST(GameSessionCreatures, CreatureWorkIsPublishedOnceAfterSuccessfulDrain) {
    CreatureWorld world(0);
    world.Add();
    world.Tick();
    const auto& work = world.session.GetActiveRegionLedger().completed_work();
    ASSERT_TRUE(work.contains({0, 0}));
    EXPECT_EQ(work.at({0, 0}), 2u);
    // Fault the next drain after consumers have staged their work.
    world.session.GetSimulationEventBus().subscribe([](const auto& event) {
        if (event.topic != "creature.failure")
            return;
        throw std::runtime_error("Consumer failure");
    });
    world.session.GetSimulationEventBus().publish(2, "creature.failure", "");
    EXPECT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    EXPECT_TRUE(world.session.GetActiveRegionLedger().completed_work().empty());
    EXPECT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 0u);
}
} // namespace
