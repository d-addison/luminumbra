// real audio occlusion via physics raycasts.
//
// AudioSpatialCluster::CalculateOcclusion used to depend solely on a mockable
// raycast callback (and returned 0 when none was set) — no world geometry ever
// muffled a sound.  adds a physics-system path: when a PhysicsSystem is
// set, the occlusion query casts a ray through the real Jolt world from the
// source to the listener; solid geometry in the path raises the 0..1 occlusion
// scalar. This test PINS that behavior against the proving_signal:
//
//   * clear line of sight  -> occlusion ~0
//   * a solid body BETWEEN source and listener -> occlusion RISES
//
// It holds the source/listener FIXED and toggles only the occluder (one
// variable), so the contrast can only come from the occlusion logic. The
// occluder is a flat HeightFieldShape floor built through the production
// PhysicsSystem::add_chunk_collision path (the same collider the live world
// uses and the same one jolt_tunneling_test exercises) — no bespoke fixture.
//
// It also pins the fallbacks the distance-only path must preserve byte-for-byte:
// no physics + no callback -> 0; the mockable callback is used only when no
// physics system is present; and the physics path takes precedence over a
// callback when both are set.
//
// Registered as its own gtest exe (audio_occlusion_test). It compiles
// AudioSpatialCluster.cpp directly (decoupled from miniaudio) and links
// luminumbra_common, which carries Jolt + the PhysicsSystem audio raycast.
#include "gtest/gtest.h"

#include "audio/AudioSpatialCluster.h"
#include "luminumbra/core/Types.h"
#include "systems/PhysicsSystem.h"
#include "world/Chunk.h"

#include <glm/glm.hpp>

#include <memory>

namespace {

using Luminumbra::Chunk;
using Luminumbra::IVec3;
using Luminumbra::Client::AudioSpatialCluster;
using Luminumbra::Systems::PhysicsSystem;

// Builds a chunk with a FLAT heightmap floor at world height `floor_y`, matching
// the layout PhysicsSystem::add_chunk_collision expects: a HeightFieldShape of
// (CHUNK_SIZE_X + 1) samples per side spanning world x,z in [0, CHUNK_SIZE_X].
// add_chunk_collision gates on the chunk-center sample lying in the chunk's
// vertical band [cc.y*CHUNK_SIZE_Y, (cc.y+1)*CHUNK_SIZE_Y), so floor_y must sit
// in [0,16) for a chunk at cc.y=0. (Same helper shape as jolt_tunneling_test.)
std::shared_ptr<Chunk> MakeFlatFloorChunk(const IVec3& coords, float floor_y) {
    auto chunk = std::make_shared<Chunk>(coords);
    const int side = Luminumbra::CHUNK_SIZE_X + 1;
    chunk->heightmap_data.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side),
                                 floor_y);
    return chunk;
}

// A source directly below the floor and a listener directly above it, both over
// the heightfield center (x=z=8). The straight source->listener segment crosses
// the y=8 floor plane, so a blocking floor occludes it; with no floor the same
// segment is a clear line of sight.
const glm::vec3 kSourceBelow(8.0f, 2.0f, 8.0f);
const glm::vec3 kListenerAbove(8.0f, 14.0f, 8.0f);
constexpr float kFloorY = 8.0f;

} // namespace

// Core proving_signal: a solid body placed between source and listener raises the
// occlusion, while a clear line of sight reads ~0. Only the occluder changes.
TEST(AudioOcclusion, PhysicsOccluderBetweenSourceAndListenerRaisesOcclusion) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    AudioSpatialCluster cluster;
    cluster.SetPhysicsSystem(&physics);

    // Clear line of sight (no bodies in the world yet): CastRay finds no hit, so
    // PhysicsSystem::calculate_audio_occlusion returns exactly 0.
    const float clear = cluster.QueryOcclusion(kSourceBelow, kListenerAbove);
    EXPECT_NEAR(clear, 0.0f, 1e-4f) << "clear line of sight must not occlude (got " << clear << ")";

    // Drop a solid floor squarely between the source and the listener.
    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), kFloorY);
    physics.add_chunk_collision(*floor);

    const float blocked = cluster.QueryOcclusion(kSourceBelow, kListenerAbove);
    EXPECT_GT(blocked, 0.05f)
        << "a solid body between source and listener must occlude the sound (got " << blocked
        << ")";
    EXPECT_GT(blocked, clear) << "occlusion must RISE when geometry blocks the path (clear="
                              << clear << ", blocked=" << blocked << ")";

    physics.shutdown();
}

// Fallback: with neither a physics system nor a raycast callback, occlusion is 0
// (the byte-identical distance-only path — nothing to raycast against).
TEST(AudioOcclusion, ReturnsZeroWithoutPhysicsOrCallback) {
    AudioSpatialCluster cluster;
    EXPECT_EQ(cluster.QueryOcclusion(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f)), 0.0f);
}

// Fallback: when no physics system is set, the mockable raycast-callback seam is
// used unchanged (occlusion = hit_distance / total_distance). A hit at 3 m along
// a 10 m source->listener span yields 0.3 — the pre- behavior, preserved.
TEST(AudioOcclusion, MockRaycastCallbackUsedWhenNoPhysics) {
    AudioSpatialCluster cluster;
    cluster.SetPhysicsRaycastCallback([](const glm::vec3&, const glm::vec3&, float& hit_distance) {
        hit_distance = 3.0f;
        return true;
    });

    const float occ = cluster.QueryOcclusion(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    EXPECT_NEAR(occ, 0.3f, 1e-4f)
        << "the raycast-callback fallback must stay byte-identical (hit/total = 0.3), got " << occ;
}

// Precedence: when BOTH a physics system and a callback are set, the physics path
// wins. Here the world is empty (clear line of sight -> physics returns 0) while
// the callback would report a strong hit (~0.42) if it were consulted; the result
// must be ~0, proving the callback is ignored once a physics system is present.
TEST(AudioOcclusion, PhysicsTakesPrecedenceOverCallback) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    AudioSpatialCluster cluster;
    cluster.SetPhysicsSystem(&physics);
    cluster.SetPhysicsRaycastCallback([](const glm::vec3&, const glm::vec3&, float& hit_distance) {
        hit_distance = 5.0f; // would be ~0.42 over the 12 m span if consulted
        return true;
    });

    const float occ = cluster.QueryOcclusion(kSourceBelow, kListenerAbove);
    EXPECT_NEAR(occ, 0.0f, 1e-4f)
        << "physics raycast (clear LOS -> 0) must override the callback, got " << occ;

    physics.shutdown();
}
