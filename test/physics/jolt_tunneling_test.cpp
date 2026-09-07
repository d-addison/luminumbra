// Jolt tunneling / penetration test ( of the second-class adversarial pass).
//
// Jolt's discrete solver can let a fast body pass THROUGH thin geometry in a single
// step if the body moves more than its own thickness per step. The projectile path
// (PhysicsSystem::create_dynamic_sphere) already opts into JPH::EMotionQuality::
// LinearCast (continuous collision) precisely to prevent this -- this test PINS that
// guarantee: a fast sphere fired straight down at a thin static terrain floor must
// COLLIDE with it and come to rest ABOVE the floor, never tunnel below it.
//
// The static "thin geometry" here is a flat HeightFieldShape chunk floor at y=8 built
// through the real PhysicsSystem::add_chunk_collision path (the same collider the live
// world uses), so the test exercises the production collide-and-slide, not a bespoke
// fixture. If Jolt's step ever tunnels (e.g. LinearCast is dropped, or the step grows
// past the floor thickness), the sphere ends up at y<0 and the assertion pins the
// failure with the penetration depth -- the mitigation (sub-stepping / linear cast)
// is then a separate decision, not a silent change.
//
// Registered as its own gtest exe (jolt_tunneling_test); links luminumbra_common,
// which carries Jolt.
#include "gtest/gtest.h"

#include "luminumbra/core/Types.h"
#include "systems/PhysicsSystem.h"
#include "world/Chunk.h"

#include <glm/glm.hpp>

#include <vector>

namespace {

using Luminumbra::Chunk;
using Luminumbra::IVec3;
using Luminumbra::Systems::PhysicsSystem;

// Builds a chunk with a FLAT heightmap floor at world height `floor_y`. The
// HeightFieldShape resolution is CHUNK_SIZE_X+1 per side (the +1 padding the live
// add_chunk_collision expects); add_chunk_collision gates on the chunk-center sample
// lying inside the chunk's vertical band [cc.y*CHUNK_SIZE_Y, (cc.y+1)*CHUNK_SIZE_Y),
// so floor_y must sit in [0,16) for a chunk at cc.y=0.
std::shared_ptr<Chunk> MakeFlatFloorChunk(const IVec3& coords, float floor_y) {
    auto chunk = std::make_shared<Chunk>(coords);
    const int side = Luminumbra::CHUNK_SIZE_X + 1;
    chunk->heightmap_data.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side),
                                 floor_y);
    return chunk;
}

// Steps the Jolt world a fixed number of 1/60 s sub-frames so the sphere travels its
// full descent under continuous collision.
void StepWorld(PhysicsSystem& physics, int frames) {
    for (int i = 0; i < frames; ++i) {
        physics.update(1.0f / 60.0f);
    }
}

} // namespace

TEST(JoltTunneling, FastSphereDoesNotTunnelThroughThinFloor) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    // A flat collision floor at y = 8 inside the chunk at (0,0,0). Its heightfield
    // origin is the chunk corner (0,0,0) spanning world x,z in [0, CHUNK_SIZE_X].
    constexpr float kFloorY = 8.0f;
    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), kFloorY);
    physics.add_chunk_collision(*floor);

    // Fire a small sphere straight DOWN at the floor at high speed from well above it,
    // centered over the heightfield (x=z=8). At 80 m/s and a 1/60 s step that is ~1.33
    // m/step -- far more than the sphere's 0.25 m diameter, so a DISCRETE solver would
    // tunnel. LinearCast must catch it.
    const glm::vec3 spawn(8.0f, 25.0f, 8.0f);
    const glm::vec3 velocity(0.0f, -80.0f, 0.0f);
    constexpr float kRadius = 0.25f;
    const JPH::BodyID ball = physics.create_dynamic_sphere(spawn, velocity, kRadius);
    ASSERT_FALSE(ball.IsInvalid());

    // 240 frames (4 s) is ample for a 17 m descent + settle.
    StepWorld(physics, 240);

    const glm::vec3 rest = physics.get_body_position(ball);
    const float penetration = (kFloorY - kRadius) - rest.y; // >0 means it sank into/below the floor

    // The sphere must rest ON the floor (its center at >= floor_y - radius, within a
    // tolerance for Jolt's penetration-recovery slop), and CRUCIALLY must not be below
    // the floor plane -- that would be a tunnel-through.
    EXPECT_GT(rest.y, 0.0f) << "the sphere tunneled to or below the world floor (y=" << rest.y
                            << ") -- it passed through the thin geometry";
    EXPECT_GE(rest.y, kFloorY - kRadius - 0.5f)
        << "the sphere penetrated the floor by " << penetration << " m (rest y=" << rest.y
        << ", floor y=" << kFloorY << ") -- discrete-step tunneling through thin geometry";

    physics.destroy_body(ball);
    physics.shutdown();
}

TEST(JoltTunneling, SlowSphereAlsoRestsOnTheFloor) {
    // Control: a SLOW sphere (no tunneling risk) must rest on the same floor. If this
    // fails, the floor collider itself is wrong and the fast-sphere result above would
    // be a false pass (the ball falls forever in empty space).
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    constexpr float kFloorY = 8.0f;
    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), kFloorY);
    physics.add_chunk_collision(*floor);

    constexpr float kRadius = 0.25f;
    const JPH::BodyID ball = physics.create_dynamic_sphere(
        glm::vec3(8.0f, 12.0f, 8.0f), glm::vec3(0.0f, -2.0f, 0.0f), kRadius);
    ASSERT_FALSE(ball.IsInvalid());
    StepWorld(physics, 300);

    const glm::vec3 rest = physics.get_body_position(ball);
    EXPECT_GE(rest.y, kFloorY - kRadius - 0.5f)
        << "slow sphere did not rest on the floor (rest y=" << rest.y
        << ") -- the collider is not present";
    EXPECT_LE(rest.y, kFloorY + 5.0f)
        << "slow sphere came to rest implausibly high (rest y=" << rest.y << ")";

    physics.destroy_body(ball);
    physics.shutdown();
}

TEST(JoltRuntimeLifetime, OverlappingWorldsShareTheProcessFactory) {
    PhysicsSystem first;
    PhysicsSystem second;

    first.startup();
    second.startup();
    ASSERT_TRUE(first.is_started());
    ASSERT_TRUE(second.is_started());

    first.shutdown();
    EXPECT_TRUE(second.is_started());
    EXPECT_NO_THROW(second.update(1.0f / 60.0f));

    second.shutdown();
}

TEST(LocalPlayerCollision, StanceChangesPreserveFeetAndRefuseLowCeilings) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());
    constexpr float kFloorY = 14.0f;
    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), kFloorY);
    physics.add_chunk_collision(*floor);
    physics.create_player_controller({8.0f, kFloorY + 0.24f, 8.0f});
    for (int frame = 0; frame < 120; ++frame) {
        physics.update_player(glm::vec3(0.0f), false, 0.0f, 0.05f);
        physics.update(0.05f);
    }
    ASSERT_TRUE(physics.is_player_grounded());
    const auto feet = physics.get_player_position();
    ASSERT_NEAR(feet.y, kFloorY, 0.05f);
    for (int transition = 0; transition < 5; ++transition) {
        ASSERT_TRUE(physics.set_player_crouched(true));
        EXPECT_EQ(physics.get_player_position(), feet);
        EXPECT_TRUE(physics.player_has_space_to_stand());
        ASSERT_TRUE(physics.set_player_crouched(false));
        EXPECT_EQ(physics.get_player_position(), feet);
    }

    ASSERT_TRUE(physics.set_player_crouched(true));
    // A 2.1 m clearance fits the existing 1.7 m crouched capsule but blocks the
    // 2.6 m standing capsule. A separate vertical chunk keeps both colliders live.
    auto ceiling = MakeFlatFloorChunk(IVec3(0, 1, 0), kFloorY + 2.1f);
    physics.add_chunk_collision(*ceiling);
    EXPECT_FALSE(physics.player_has_space_to_stand());
    EXPECT_FALSE(physics.set_player_crouched(false));
    EXPECT_EQ(physics.get_player_position(), feet);
    EXPECT_TRUE(physics.set_player_crouched(true));
    physics.remove_chunk_collision(ceiling->get_id());
    EXPECT_TRUE(physics.player_has_space_to_stand());
    EXPECT_TRUE(physics.set_player_crouched(false));
    EXPECT_EQ(physics.get_player_position(), feet);
    physics.shutdown();
}

TEST(LocalPlayerCollision, LowerSphereSupportsWalkingOnFortyFiveDegreeSlope) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());
    auto ramp = MakeFlatFloorChunk(IVec3(0, 0, 0), 0.0f);
    constexpr int kSide = Luminumbra::CHUNK_SIZE_X + 1;
    for (int z = 0; z < kSide; ++z) {
        for (int x = 0; x < kSide; ++x) {
            ramp->heightmap_data[static_cast<std::size_t>(z * kSide + x)] = static_cast<float>(x);
        }
    }
    physics.add_chunk_collision(*ramp);
    physics.create_player_controller({8.0f, 8.24f, 8.0f});
    for (int frame = 0; frame < 60; ++frame) {
        physics.update_player(glm::vec3(0.0f), false, 0.0f, 1.0f / 60.0f);
        physics.update(1.0f / 60.0f);
    }
    ASSERT_TRUE(physics.is_player_grounded());
    const auto before = physics.get_player_position();
    for (int frame = 0; frame < 30; ++frame) {
        physics.update_player({3.0f, 0.0f, 0.0f}, false, 0.0f, 1.0f / 60.0f);
        physics.update(1.0f / 60.0f);
        EXPECT_TRUE(physics.is_player_grounded());
    }
    const auto after = physics.get_player_position();
    EXPECT_GT(after.x, before.x);
    EXPECT_GT(after.y, before.y);
    EXPECT_NEAR(after.y - after.x, 0.0f, 0.3f);
    physics.shutdown();
}
