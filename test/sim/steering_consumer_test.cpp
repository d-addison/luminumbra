//  INTEGRATION layer: the steering consumer that blends the  bias systems' outputs
// (pack flank / migration / territory) into the creature wish. These tests guard the
// PRODUCER->CONSUMER contract -- e.g. that the pack flank field is read as a unit STEER
// DIRECTION, not a world POINT (the bug that made packs steer toward the origin).
#include <gtest/gtest.h>

#include <vector>

#include <entt/entt.hpp>

#include "ai/SteeringConsumer.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/MigratoryComponents.h"
#include "components/PackHunterComponents.h"
#include "components/TerritoryComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::RunSteeringConsumerOnTick;

entt::entity spawn(entt::registry& r, float x, float z, bool predator, float speed = 4.0f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = predator;
    cr.move_speed = speed;
    return e;
}

// A pack predator's wish becomes its flank-steer DIRECTION * speed*1.5 -- NOT coord-minus-position.
// (Spawn the predator FAR from the origin so the old point-misread would have produced a wildly
// different, origin-ward wish.)
TEST(SteeringConsumer, PackPredatorSteersAlongCoordDirection) {
    entt::registry r;
    const entt::entity p = spawn(r, /*x*/ 500.0f, /*z*/ -300.0f, /*predator*/ true, /*speed*/ 4.0f);
    auto& pk = r.emplace<Comp::PackHunterComponent>(p);
    pk.in_pack = 1;
    pk.coord_x = 0.6f; // a unit-ish steer direction (already normalized by the pack system)
    pk.coord_z = 0.8f;
    const auto stats = RunSteeringConsumerOnTick(r);
    EXPECT_EQ(stats.packed, 1);
    const auto& cr = r.get<Comp::CreatureComponent>(p);
    EXPECT_FLOAT_EQ(cr.wish_x, 0.6f * 4.0f * 1.5f);
    EXPECT_FLOAT_EQ(cr.wish_z, 0.8f * 4.0f * 1.5f);
}

// in_pack==0 (lone predator) leaves the wish to the brain (untouched here).
TEST(SteeringConsumer, LonePredatorNotSteered) {
    entt::registry r;
    const entt::entity p = spawn(r, 10.0f, 10.0f, true);
    auto& pk = r.emplace<Comp::PackHunterComponent>(p);
    pk.in_pack = 0;
    pk.coord_x = 1.0f;
    r.get<Comp::CreatureComponent>(p).wish_x = 7.0f; // brain's value
    RunSteeringConsumerOnTick(r);
    EXPECT_FLOAT_EQ(r.get<Comp::CreatureComponent>(p).wish_x, 7.0f); // unchanged
}

// Pack steering applies only to predators.
TEST(SteeringConsumer, PreyNotPackSteered) {
    entt::registry r;
    const entt::entity prey = spawn(r, 0.0f, 0.0f, /*predator*/ false);
    auto& pk = r.emplace<Comp::PackHunterComponent>(prey);
    pk.in_pack = 1;
    pk.coord_x = 1.0f;
    const auto stats = RunSteeringConsumerOnTick(r);
    EXPECT_EQ(stats.packed, 0);
}

// Migration + territory ADD their bias to the wish.
TEST(SteeringConsumer, MigrationAndTerritoryAddBias) {
    entt::registry r;
    const entt::entity e = spawn(r, 0.0f, 0.0f, false);
    r.get<Comp::CreatureComponent>(e).wish_x = 1.0f;
    r.get<Comp::CreatureComponent>(e).wish_z = 2.0f;
    auto& mig = r.emplace<Comp::MigratoryComponent>(e);
    mig.wish_x = 0.5f;
    mig.wish_z = -0.5f;
    auto& tb = r.emplace<Comp::TerritoryBiasComponent>(e);
    tb.wish_x = 0.25f;
    tb.wish_z = 0.25f;
    const auto stats = RunSteeringConsumerOnTick(r);
    EXPECT_EQ(stats.migrated, 1);
    EXPECT_EQ(stats.homed, 1);
    const auto& cr = r.get<Comp::CreatureComponent>(e);
    EXPECT_FLOAT_EQ(cr.wish_x, 1.0f + 0.5f + 0.25f);
    EXPECT_FLOAT_EQ(cr.wish_z, 2.0f - 0.5f + 0.25f);
}

// A carcass (eaten) is inert -- its wish is never touched.
TEST(SteeringConsumer, CarcassUntouched) {
    entt::registry r;
    const entt::entity p = spawn(r, 100.0f, 0.0f, true);
    r.get<Comp::CreatureComponent>(p).eaten = true;
    r.get<Comp::CreatureComponent>(p).wish_x = 3.0f;
    auto& pk = r.emplace<Comp::PackHunterComponent>(p);
    pk.in_pack = 1;
    pk.coord_x = 1.0f;
    RunSteeringConsumerOnTick(r);
    EXPECT_FLOAT_EQ(r.get<Comp::CreatureComponent>(p).wish_x, 3.0f);
}

// run == replay: identical setup -> identical wishes.
TEST(SteeringConsumer, Deterministic) {
    auto run = [] {
        entt::registry r;
        auto a = spawn(r, 5.0f, 5.0f, true);
        auto& pk = r.emplace<Comp::PackHunterComponent>(a);
        pk.in_pack = 1;
        pk.coord_x = 0.3f;
        pk.coord_z = -0.9f;
        auto b = spawn(r, -2.0f, 1.0f, false);
        r.emplace<Comp::MigratoryComponent>(b).wish_x = 0.4f;
        RunSteeringConsumerOnTick(r);
        return std::vector<float>{r.get<Comp::CreatureComponent>(a).wish_x,
                                  r.get<Comp::CreatureComponent>(a).wish_z,
                                  r.get<Comp::CreatureComponent>(b).wish_x};
    };
    EXPECT_EQ(run(), run());
}

// No creatures -> no-op.
TEST(SteeringConsumer, EmptyRosterNoOp) {
    entt::registry r;
    const auto stats = RunSteeringConsumerOnTick(r);
    EXPECT_EQ(stats.packed, 0);
    EXPECT_EQ(stats.migrated, 0);
    EXPECT_EQ(stats.homed, 0);
}

} // namespace
