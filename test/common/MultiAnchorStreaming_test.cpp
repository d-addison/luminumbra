// multi-anchor chunk streaming. Proves SHIELD_WorldSystem streams the
// near surface around EVERY anchor in the set (union), not just one — the foundation
// for multi-player / multi-camera streaming. The single-anchor path is covered
// byte-identically by HeadlessServerTick + PlayerView; this exercises the 2-anchor path
// the forwarding overload does not reach.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::JobSystem;
using Luminumbra::Vec3;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::world::GameSession;

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

// Temp root containing ONLY the world preset (headless — no res/data assets).
// The root is unique per fixture instance so concurrent common_tests processes
// never share (or clobber) a directory; each TEST builds ONE instance and
// reuses it, so within-run path stability still holds. The destructor removes
// the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ =
            fs::temp_directory_path() / ("luminumbra_multi_anchor_test_" + std::to_string(stamp));
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets" /
                          "default.json",
                      root_ / "worlds" / "atlas" / "presets" / "default.json");
    }
    ~HeadlessRoot() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }

private:
    fs::path root_;
};

// Tick the world `n` times around `anchors`, quiescing async streaming each tick so the
// near surface around every anchor reaches residency deterministically.
void stream_around(SHIELD_WorldSystem* world,
                   entt::registry& registry,
                   Luminumbra::Systems::PhysicsSystem* physics,
                   const std::vector<Vec3>& anchors,
                   int n) {
    for (int i = 0; i < n; ++i) {
        world->update(registry, anchors, physics);
        world->wait_for_streaming_jobs();
    }
}

// Surface anchor at a horizontal offset from spawn (Y placed above the local terrain).
Vec3 surface_anchor(SHIELD_WorldSystem* world, const Vec3& spawn, float dx, float dz) {
    const float x = spawn.x + dx;
    const float z = spawn.z + dz;
    return Vec3(x, world->GetTerrainHeightAt(x, z) + 2.0f, z);
}

constexpr int kTicks = 96;         // generous: generation is budget-limited per activation
constexpr int kCoverageRadius = 3; // near-surface disc to probe
// Streaming cap (the WaterDeterminism pattern): each anchor's wanted disc is a
// 6-chunk (96 m) full-detail disc instead of the ~1,800-column RENDER_DISTANCE
// fill that dominated every wait_for_streaming_jobs tick. The cap does not
// weaken either gate: the positive probes read a kCoverageRadius=3 disc, well
// inside the 6-chunk cap, and the negative gate ("far region NOT resident")
// only gets STRONGER under a smaller wanted set. Both tests run under the SAME
// cap, so the A/B contrast (union streams B's disc; single anchor does not)
// still isolates exactly the multi-anchor union semantics.
constexpr int kStreamRadiusCap = 6; // chunks (96 m)
// Far enough that anchor B's disc never overlaps A's wanted/eviction disc under
// EITHER the production radius (RENDER_DISTANCE is a few hundred metres) or the
// capped 96 m disc; 3 km is comfortably beyond both.
constexpr float kFarOffsetM = 3000.0f;

TEST(MultiAnchorStreaming, BothAnchorsStreamTheirNearSurface) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        ASSERT_TRUE(session.CreateWorld("MultiAnchor", "12345", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        ASSERT_NE(world, nullptr);
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();

        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor_a = surface_anchor(world, spawn, 0.0f, 0.0f);
        const Vec3 anchor_b = surface_anchor(world, spawn, kFarOffsetM, kFarOffsetM);

        stream_around(world, session.GetRegistry(), physics, {anchor_a, anchor_b}, kTicks);

        const auto cov_a = world->get_camera_local_coverage_stats(anchor_a, kCoverageRadius);
        const auto cov_b = world->get_camera_local_coverage_stats(anchor_b, kCoverageRadius);
        EXPECT_GT(cov_a.present_surface_chunks, 0u) << "anchor A near surface did not stream";
        EXPECT_GT(cov_b.present_surface_chunks, 0u)
            << "anchor B near surface did not stream — multi-anchor union failed to cover the 2nd "
               "anchor";
    }
    jobs.shutdown();
}

TEST(MultiAnchorStreaming, SingleAnchorDoesNotStreamTheFarAnchorRegion) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        ASSERT_TRUE(session.CreateWorld("SingleAnchor", "12345", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        ASSERT_NE(world, nullptr);
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();

        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor_a = surface_anchor(world, spawn, 0.0f, 0.0f);
        const Vec3 anchor_b = surface_anchor(world, spawn, kFarOffsetM, kFarOffsetM);

        // Stream around A ONLY. The far B region must stay unstreamed — this is the
        // contrast that proves the 2-anchor test above is actually the union, not just
        // an artifact of a huge radius.
        stream_around(world, session.GetRegistry(), physics, {anchor_a}, kTicks);

        const auto cov_a = world->get_camera_local_coverage_stats(anchor_a, kCoverageRadius);
        const auto cov_b = world->get_camera_local_coverage_stats(anchor_b, kCoverageRadius);
        EXPECT_GT(cov_a.present_surface_chunks, 0u) << "anchor A near surface did not stream";
        EXPECT_EQ(cov_b.present_surface_chunks, 0u)
            << "far region streamed with a single anchor — the 2-anchor test would be vacuous";
    }
    jobs.shutdown();
}

} // namespace
