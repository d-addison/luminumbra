#include "gtest/gtest.h"

#include "core/EngineContracts.h"
#include "core/JobSystem.h"
#include "systems/PhysicsSystem.h"
#include "world/GameSession.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

std::string SourceRoot() {
    return std::filesystem::weakly_canonical(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT)).string();
}

} // namespace

TEST(EngineContractsTest, LifecycleGraphAcceptsRuntimeOrderAndRejectsIllegalOrder) {
    const Luminumbra::Contracts::LifecycleGraph graph(
        Luminumbra::Contracts::KnownRuntimeSubsystemContracts());

    const std::vector<std::string> startup_order = {
        "job_system",
        "physics_system",
        "world_system",
        "water_system",
        "instinct_system",
        "renderer",
        "audio",
        "ui",
        "gameplay",
    };
    EXPECT_TRUE(graph.validate_startup_order(startup_order).ok);

    const std::vector<std::string> shutdown_order = {
        "gameplay",
        "ui",
        "audio",
        "renderer",
        "instinct_system",
        "water_system",
        "world_system",
        "physics_system",
        "job_system",
    };
    EXPECT_TRUE(graph.validate_shutdown_order(shutdown_order).ok);

    auto bad_startup = startup_order;
    std::swap(bad_startup[0], bad_startup[2]);
    const auto startup_result = graph.validate_startup_order(bad_startup);
    EXPECT_FALSE(startup_result.ok);
    ASSERT_FALSE(startup_result.errors.empty());

    auto bad_shutdown = shutdown_order;
    std::swap(bad_shutdown[0], bad_shutdown[7]);
    const auto shutdown_result = graph.validate_shutdown_order(bad_shutdown);
    EXPECT_FALSE(shutdown_result.ok);
    ASSERT_FALSE(shutdown_result.errors.empty());
}

TEST(EngineContractsTest, LifecycleTrackerRejectsIllegalTransitions) {
    Luminumbra::Contracts::LifecycleTracker tracker("test_system");

    EXPECT_FALSE(tracker.mark_running());
    EXPECT_EQ(tracker.state(), Luminumbra::Contracts::LifecycleState::Uninitialized);
    EXPECT_FALSE(tracker.failure_reason().empty());

    EXPECT_TRUE(tracker.mark_starting());
    EXPECT_TRUE(tracker.mark_running());
    EXPECT_TRUE(tracker.ready());
    EXPECT_TRUE(tracker.mark_stopping());
    EXPECT_TRUE(tracker.mark_stopped());
}

TEST(EngineContractsTest, OwnedJobTrackerDrainsOutstandingBatches) {
    Luminumbra::JobSystem job_system;
    job_system.startup();

    std::atomic<bool> release_jobs{false};
    std::atomic<int> completed{0};
    std::vector<Luminumbra::Job> jobs;
    for (int i = 0; i < 8; ++i) {
        jobs.emplace_back([&]() {
            while (!release_jobs.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            completed.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    Luminumbra::Contracts::OwnedJobTracker tracker("contract_test");
    tracker.track(job_system.dispatch_batch(jobs));
    EXPECT_GT(tracker.outstanding_count(), 0u);

    release_jobs.store(true, std::memory_order_release);
    tracker.drain(job_system);
    EXPECT_EQ(tracker.outstanding_count(), 0u);
    EXPECT_EQ(completed.load(std::memory_order_acquire), 8);

    job_system.shutdown();
    EXPECT_EQ(job_system.get_runtime_stats().worker_count, 0u);
}

TEST(EngineContractsTest, JobSystemShutdownCompletesAcceptedBatches) {
    Luminumbra::JobSystem job_system;
    job_system.startup();

    std::atomic<int> completed{0};
    std::vector<Luminumbra::Job> jobs;
    for (int i = 0; i < 32; ++i) {
        jobs.emplace_back([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    const Luminumbra::JobHandle handle = job_system.dispatch_batch(jobs);
    job_system.shutdown();

    EXPECT_TRUE(Luminumbra::Contracts::IsJobComplete(handle));
    EXPECT_EQ(completed.load(std::memory_order_acquire), 32);
}

TEST(EngineContractsTest, ResourceHandlesRejectDestroyedAndStaleGenerations) {
    Luminumbra::Contracts::ResourceGenerationTable table;
    const auto first = table.create();

    EXPECT_TRUE(table.is_valid(first));
    EXPECT_EQ(table.active_count(), 1u);
    EXPECT_TRUE(table.destroy(first));
    EXPECT_FALSE(table.is_valid(first));
    EXPECT_EQ(table.active_count(), 0u);

    const auto second = table.create();
    EXPECT_TRUE(table.is_valid(second));
    EXPECT_FALSE(table.is_valid(first));
    EXPECT_EQ(second.slot, first.slot);
    EXPECT_NE(second.generation, first.generation);
}

TEST(EngineContractsTest, WorldConfigValidationRejectsMissingOrUnsafePresets) {
    const auto valid = Luminumbra::world::GameSession::ValidateWorldConfig(SourceRoot(), "default");
    ASSERT_TRUE(valid.ok) << (valid.errors.empty() ? "" : valid.errors.front());
    EXPECT_TRUE(std::filesystem::exists(valid.preset_path));

    const auto missing = Luminumbra::world::GameSession::ValidateWorldConfig(SourceRoot(), "missing_contract_test_preset");
    EXPECT_FALSE(missing.ok);
    ASSERT_FALSE(missing.errors.empty());

    const auto unsafe = Luminumbra::world::GameSession::ValidateWorldConfig(SourceRoot(), "../default");
    EXPECT_FALSE(unsafe.ok);
    ASSERT_FALSE(unsafe.errors.empty());
}

TEST(EngineContractsTest, PhysicsLifecycleIsIdempotent) {
    Luminumbra::Systems::PhysicsSystem physics;

    EXPECT_FALSE(physics.is_started());
    physics.startup();
    EXPECT_TRUE(physics.is_started());
    physics.startup();
    EXPECT_TRUE(physics.is_started());
    physics.shutdown();
    EXPECT_FALSE(physics.is_started());
    physics.shutdown();
    EXPECT_FALSE(physics.is_started());
}
