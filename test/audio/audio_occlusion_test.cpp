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

#include <algorithm>
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

class DeferredAudioOcclusion : public ::testing::Test {
protected:
    void SetUp() override {
        physics.startup();
        ASSERT_TRUE(physics.is_started());
        auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), kFloorY);
        physics.add_chunk_collision(*floor);
        cluster.SetPhysicsSystem(&physics);
        cluster.SetClusteringEnabled(false);
        cluster.SetPhysicsRaycastCallback(
            [](const glm::vec3&, const glm::vec3&, float&) { return false; });
        physics.GetBatchedQueries().SetMaxQueriesPerFrame(1);
    }

    void QueueDeferredSource(const glm::vec3& position = kSourceBelow) {
        auto& queries = physics.GetBatchedQueries();
        // Two higher-priority queries hold the audio result beyond one processing budget.
        for (int index = 0; index < 2; ++index) {
            queries.QueueRaycast(kSourceBelow, kListenerAbove, {}, 2.0f);
        }
        cluster.AddAudioSource(kHandle, position, 1.0f, 1.0f, 100.0f);
        cluster.Update(kListenerAbove, 1.0f / 30.0f);
        ASSERT_EQ(queries.GetQueuedQueryCount(), 3u);
        queries.ProcessBatch(&physics);
        EXPECT_EQ(queries.GetProcessedQueryCount(), 1u);
        EXPECT_EQ(queries.GetQueuedQueryCount(), 2u);
    }

    void DeliverDeferredResult() {
        auto& queries = physics.GetBatchedQueries();
        queries.ProcessBatch(&physics);
        EXPECT_EQ(queries.GetProcessedQueryCount(), 1u);
        EXPECT_EQ(queries.GetQueuedQueryCount(), 1u);
        queries.ProcessBatch(&physics);
        EXPECT_EQ(queries.GetProcessedQueryCount(), 1u);
        EXPECT_EQ(queries.GetQueuedQueryCount(), 0u);
    }

    static constexpr Luminumbra::u32 kHandle = 17;
    PhysicsSystem physics;
    AudioSpatialCluster cluster;
};

} // namespace

TEST_F(DeferredAudioOcclusion, ReplacementWhileDeferredDoesNotReceiveOldResult) {
    QueueDeferredSource();
    cluster.AddAudioSource(kHandle, glm::vec3(14.0f, 2.0f, 8.0f), 1.0f, 1.0f, 100.0f);
    ASSERT_EQ(cluster.GetSourceAttenuation(kHandle), 1.0f);

    DeliverDeferredResult();
    EXPECT_EQ(cluster.GetSourceAttenuation(kHandle), 1.0f);
    EXPECT_EQ(cluster.GetActiveSourceCount(), 1u);
}

TEST_F(DeferredAudioOcclusion, RemovalRejectsDeferredResultBeforeRetirement) {
    QueueDeferredSource();
    const auto attenuation = cluster.GetSourceAttenuation(kHandle);
    ASSERT_TRUE(attenuation.has_value());
    cluster.UpdateSourcePosition(kHandle, glm::vec3(14.0f, 2.0f, 8.0f));
    cluster.RemoveAudioSource(kHandle);

    DeliverDeferredResult();
    EXPECT_EQ(cluster.GetSourceAttenuation(kHandle), attenuation);
    EXPECT_EQ(cluster.GetActiveSourceCount(), 1u);
    cluster.Update(kListenerAbove, 1.0f / 30.0f);
    EXPECT_FALSE(cluster.GetSourceAttenuation(kHandle).has_value());
    EXPECT_EQ(cluster.GetActiveSourceCount(), 0u);
}

TEST_F(DeferredAudioOcclusion, RetiredSourceResultDoesNotReachReusedHandle) {
    QueueDeferredSource();
    cluster.RemoveAudioSource(kHandle);
    cluster.Update(kListenerAbove, 1.0f / 30.0f);
    ASSERT_FALSE(cluster.GetSourceAttenuation(kHandle).has_value());
    ASSERT_EQ(cluster.GetActiveSourceCount(), 0u);
    cluster.AddAudioSource(kHandle, glm::vec3(14.0f, 2.0f, 8.0f), 1.0f, 1.0f, 100.0f);

    DeliverDeferredResult();
    EXPECT_EQ(cluster.GetSourceAttenuation(kHandle), 1.0f);
}

TEST_F(DeferredAudioOcclusion, RemovedSourceCanRetireBeforeDeferredResult) {
    QueueDeferredSource();
    cluster.RemoveAudioSource(kHandle);
    cluster.Update(kListenerAbove, 1.0f / 30.0f);
    ASSERT_EQ(cluster.GetActiveSourceCount(), 0u);

    DeliverDeferredResult();
    EXPECT_FALSE(cluster.GetSourceAttenuation(kHandle).has_value());
    EXPECT_EQ(cluster.GetActiveSourceCount(), 0u);
}

TEST_F(DeferredAudioOcclusion, LiveSourceStillReceivesDeferredResultAfterMoving) {
    QueueDeferredSource();
    const float attenuation = cluster.GetSourceAttenuation(kHandle).value_or(-1.0f);
    ASSERT_GE(attenuation, 0.0f);
    const auto hit = physics.audio_raycast(kSourceBelow, kListenerAbove);
    ASSERT_TRUE(hit.hit);
    const glm::vec3 moved_position(14.0f, 2.0f, 8.0f);
    cluster.UpdateSourcePosition(kHandle, moved_position);
    const float path_factor = (glm::distance(moved_position, hit.hit_point) +
                               glm::distance(hit.hit_point, kListenerAbove)) /
                              glm::distance(moved_position, kListenerAbove);
    const float occlusion =
        std::clamp(hit.material_absorption * std::min(path_factor - 1.0f, 1.0f), 0.0f, 0.95f);
    ASSERT_GT(occlusion, 0.0f);

    DeliverDeferredResult();
    const float result = cluster.GetSourceAttenuation(kHandle).value_or(-1.0f);
    EXPECT_NEAR(result, attenuation * (1.0f - occlusion), 1e-6f);
    EXPECT_LT(result, attenuation);
}

TEST_F(DeferredAudioOcclusion, SourceMovedToListenerPreservesAttenuation) {
    QueueDeferredSource();
    const auto attenuation = cluster.GetSourceAttenuation(kHandle);
    ASSERT_TRUE(attenuation.has_value());
    cluster.UpdateSourcePosition(kHandle, kListenerAbove);

    DeliverDeferredResult();
    EXPECT_EQ(cluster.GetSourceAttenuation(kHandle), attenuation);
}

TEST_F(DeferredAudioOcclusion, DeferredMissPreservesAttenuation) {
    const glm::vec3 outside_floor(40.0f, 2.0f, 8.0f);
    ASSERT_FALSE(physics.audio_raycast(outside_floor, kListenerAbove).hit);
    QueueDeferredSource(outside_floor);
    const auto attenuation = cluster.GetSourceAttenuation(kHandle);
    ASSERT_TRUE(attenuation.has_value());

    DeliverDeferredResult();
    EXPECT_EQ(cluster.GetSourceAttenuation(kHandle), attenuation);
}

TEST_F(DeferredAudioOcclusion, ClusterCanBeDestroyedBeforeDeferredResult) {
    {
        auto temporary_cluster = std::make_unique<AudioSpatialCluster>();
        temporary_cluster->SetPhysicsSystem(&physics);
        temporary_cluster->SetPhysicsRaycastCallback(
            [](const glm::vec3&, const glm::vec3&, float&) { return false; });
        temporary_cluster->AddAudioSource(kHandle, kSourceBelow, 1.0f, 1.0f, 100.0f);
        temporary_cluster->Update(kListenerAbove, 1.0f / 30.0f);
        ASSERT_EQ(physics.GetBatchedQueries().GetQueuedQueryCount(), 1u);
    }

    physics.GetBatchedQueries().ProcessBatch(&physics);
    EXPECT_EQ(physics.GetBatchedQueries().GetProcessedQueryCount(), 1u);
    EXPECT_EQ(physics.GetBatchedQueries().GetQueuedQueryCount(), 0u);
}

TEST_F(DeferredAudioOcclusion, UnownedSourcesAreNotQueued) {
    Luminumbra::Client::AudioSource unowned{.position = kSourceBelow,
                                            .volume = 1.0f,
                                            .min_distance = 1.0f,
                                            .max_distance = 100.0f,
                                            .is_active = true,
                                            .event_handle = kHandle,
                                            .last_calculated_attenuation = 1.0f,
                                            .cluster_id = -1};
    cluster.CalculateBatchedOcclusion({&unowned}, kListenerAbove);
    EXPECT_EQ(physics.GetBatchedQueries().GetQueuedQueryCount(), 0u);
    cluster.AddAudioSource(kHandle, kSourceBelow, 1.0f, 1.0f, 100.0f);
    cluster.CalculateBatchedOcclusion({&unowned}, kListenerAbove);
    EXPECT_EQ(physics.GetBatchedQueries().GetQueuedQueryCount(), 0u);
    cluster.CalculateBatchedOcclusion({}, kListenerAbove);
    EXPECT_EQ(physics.GetBatchedQueries().GetQueuedQueryCount(), 0u);
    EXPECT_EQ(unowned.last_calculated_attenuation, 1.0f);
    EXPECT_FALSE(cluster.GetSourceAttenuation(kHandle + 1).has_value());
}

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
