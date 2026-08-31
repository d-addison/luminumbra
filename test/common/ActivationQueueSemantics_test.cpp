// the activation queue's tick-keyed
// publication semantics, exercised DIRECTLY (no per-tick barrier). A batch
// dispatched at tick D with the fixed pipeline latency K publishes at
// activate_due(D+K) — not one tick earlier — even when its jobs finished long
// before (verified by raw-quiescing the workers first). This is the proving
// pin for the barrier swap: availability is a pure function of (dispatch
// schedule, tick), never job-completion timing.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::ChunkState;
using Luminumbra::JobSystem;
using Luminumbra::Vec3;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::world::GameSession;

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

// Must mirror kActivationPipelineLatencyTicks in SHIELD_WorldSystem.cpp.
constexpr std::int64_t kExpectedPipelineLatencyTicks = 8;

class HeadlessRoot {
public:
    HeadlessRoot() {
        root_ = fs::temp_directory_path() / "luminumbra_activation_queue_test";
        fs::remove_all(root_);
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

std::vector<Luminumbra::ChunkID> LoadingChunkIds(SHIELD_WorldSystem* world) {
    std::vector<Luminumbra::ChunkID> ids;
    for (const auto& chunk : world->snapshot_streamed_chunks()) {
        if (chunk && chunk->get_state() == ChunkState::Loading) {
            ids.push_back(chunk->get_id());
        }
    }
    return ids;
}

TEST(ActivationQueueSemantics, BatchPublishesAtDispatchTickPlusKNotEarlier) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        ASSERT_TRUE(session.CreateWorld("ActivationQueue", "12345", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        ASSERT_NE(world, nullptr);
        auto* physics = session.GetPhysicsSystem();
        auto& registry = session.GetRegistry();

        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);

        // Drive ticks WITHOUT the barrier until a generation batch dispatches
        // (the activation pass runs on its own interval). Loading chunks in
        // the snapshot are the observable: they appear at dispatch and stay
        // Loading until the queue publishes.
        std::int64_t dispatch_tick = -1;
        for (std::int64_t tick = 0; tick < 32 && dispatch_tick < 0; ++tick) {
            world->begin_tick(tick);
            world->update(registry, {anchor}, physics);
            if (!LoadingChunkIds(world).empty()) {
                dispatch_tick = tick;
            }
        }
        ASSERT_GE(dispatch_tick, 0) << "no generation batch dispatched in 32 ticks";

        // Let the WORKERS finish completely — without publishing (raw waits).
        world->quiesce_streaming_jobs_for_save();
        const auto loading_before = LoadingChunkIds(world);
        ASSERT_FALSE(loading_before.empty())
            << "no Loading chunks staged — generation published without activate_due?";

        // One tick before due: nothing may publish, even though every job is done.
        world->activate_due(dispatch_tick + kExpectedPipelineLatencyTicks - 1);
        EXPECT_EQ(LoadingChunkIds(world).size(), loading_before.size())
            << "a batch published BEFORE its due tick — availability leaked "
               "job-completion timing (violation)";

        // At the due tick: the batch publishes (Loading -> Idle flips).
        world->activate_due(dispatch_tick + kExpectedPipelineLatencyTicks);
        const auto loading_after = LoadingChunkIds(world);
        EXPECT_LT(loading_after.size(), loading_before.size())
            << "the due batch did not publish at dispatch+K";
    }
    jobs.shutdown();
}

} // namespace
