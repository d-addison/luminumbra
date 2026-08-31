#include "luminumbra_common/core/JobSystem.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class RunningJobSystem {
public:
    RunningJobSystem() {
        job_system.startup();
    }

    ~RunningJobSystem() {
        job_system.shutdown();
    }

    RunningJobSystem(const RunningJobSystem&) = delete;
    RunningJobSystem& operator=(const RunningJobSystem&) = delete;

    Luminumbra::JobSystem job_system;
};

TEST(WorldStreamingStateStressTest, ChunkStateBurstsCompleteEveryRequestedTransition) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr std::size_t kChunkCount = 128;
    constexpr std::size_t kStreamingPasses = 32;
    constexpr int kExpectedTransitions = static_cast<int>(kChunkCount * kStreamingPasses);
    constexpr auto kTimeout = 10s;

    RunningJobSystem system;
    std::array<std::atomic<int>, kChunkCount> requested_counts;
    std::array<std::atomic<int>, kChunkCount> loaded_counts;
    for (std::size_t i = 0; i < kChunkCount; ++i) {
        requested_counts[i].store(0, std::memory_order_relaxed);
        loaded_counts[i].store(0, std::memory_order_relaxed);
    }

    std::atomic<int> completed{0};
    std::mutex mutex;
    std::condition_variable completed_cv;
    std::vector<Luminumbra::JobHandle> handles;
    handles.reserve(kStreamingPasses);

    for (std::size_t pass = 0; pass < kStreamingPasses; ++pass) {
        std::vector<Luminumbra::Job> jobs;
        jobs.reserve(kChunkCount);

        for (std::size_t chunk = 0; chunk < kChunkCount; ++chunk) {
            jobs.emplace_back(
                [&requested_counts, &loaded_counts, &completed, &completed_cv, chunk]() {
                    requested_counts[chunk].fetch_add(1, std::memory_order_relaxed);
                    loaded_counts[chunk].fetch_add(1, std::memory_order_relaxed);

                    const int done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done == kExpectedTransitions) {
                        completed_cv.notify_one();
                    }
                });
        }

        handles.push_back(system.job_system.dispatch_batch(jobs));
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(completed_cv.wait_for(lock, kTimeout, [&completed]() {
            return completed.load(std::memory_order_relaxed) == kExpectedTransitions;
        }));
    }

    for (const Luminumbra::JobHandle& handle : handles) {
        system.job_system.wait(handle);
    }

    for (std::size_t chunk = 0; chunk < kChunkCount; ++chunk) {
        EXPECT_EQ(requested_counts[chunk].load(std::memory_order_relaxed),
                  static_cast<int>(kStreamingPasses));
        EXPECT_EQ(loaded_counts[chunk].load(std::memory_order_relaxed),
                  static_cast<int>(kStreamingPasses));
    }
}

TEST(WorldStreamingStateStressTest, RepeatedStreamingWavesDoNotDropChunkRequests) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr std::size_t kChunkCount = 64;
    constexpr std::size_t kWaveCount = 96;
    constexpr int kExpectedRequests = static_cast<int>(kChunkCount * kWaveCount);
    constexpr auto kTimeout = 10s;

    RunningJobSystem system;
    std::array<std::atomic<int>, kChunkCount> request_counts;
    for (auto& count : request_counts) {
        count.store(0, std::memory_order_relaxed);
    }

    std::atomic<int> completed{0};
    std::mutex mutex;
    std::condition_variable completed_cv;

    for (std::size_t wave = 0; wave < kWaveCount; ++wave) {
        for (std::size_t chunk = 0; chunk < kChunkCount; ++chunk) {
            system.job_system.dispatch([&request_counts, &completed, &completed_cv, chunk]() {
                request_counts[chunk].fetch_add(1, std::memory_order_relaxed);

                const int done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == kExpectedRequests) {
                    completed_cv.notify_one();
                }
            });
        }
    }

    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(completed_cv.wait_for(lock, kTimeout, [&completed]() {
        return completed.load(std::memory_order_relaxed) == kExpectedRequests;
    }));

    for (const auto& count : request_counts) {
        EXPECT_EQ(count.load(std::memory_order_relaxed), static_cast<int>(kWaveCount));
    }
}

} // namespace
