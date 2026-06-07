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

TEST(JobSystemStressTest, DispatchRunsThousandsOfIndependentJobs) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr int kJobCount = 4096;
    constexpr auto kTimeout = 10s;

    RunningJobSystem system;
    std::atomic<int> completed{0};
    std::mutex mutex;
    std::condition_variable completed_cv;

    for (int i = 0; i < kJobCount; ++i) {
        system.job_system.dispatch([&completed, &completed_cv]() {
            const int done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done == kJobCount) {
                completed_cv.notify_one();
            }
        });
    }

    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(completed_cv.wait_for(lock, kTimeout, [&completed]() {
        return completed.load(std::memory_order_relaxed) == kJobCount;
    }));
}

TEST(JobSystemStressTest, BatchDispatchCompletesManyIndependentBatches) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr std::size_t kBatchCount = 64;
    constexpr std::size_t kJobsPerBatch = 64;
    constexpr int kTotalJobs = static_cast<int>(kBatchCount * kJobsPerBatch);
    constexpr auto kTimeout = 10s;

    RunningJobSystem system;
    std::array<std::atomic<int>, kBatchCount> batch_counts;
    for (auto& count : batch_counts) {
        count.store(0, std::memory_order_relaxed);
    }

    std::atomic<int> completed{0};
    std::mutex mutex;
    std::condition_variable completed_cv;
    std::vector<Luminumbra::JobHandle> handles;
    handles.reserve(kBatchCount);

    for (std::size_t batch_index = 0; batch_index < kBatchCount; ++batch_index) {
        std::vector<Luminumbra::Job> jobs;
        jobs.reserve(kJobsPerBatch);

        for (std::size_t job_index = 0; job_index < kJobsPerBatch; ++job_index) {
            jobs.emplace_back([&batch_counts, &completed, &completed_cv, batch_index]() {
                batch_counts[batch_index].fetch_add(1, std::memory_order_relaxed);
                const int done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == kTotalJobs) {
                    completed_cv.notify_one();
                }
            });
        }

        handles.push_back(system.job_system.dispatch_batch(jobs));
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(completed_cv.wait_for(lock, kTimeout, [&completed]() {
            return completed.load(std::memory_order_relaxed) == kTotalJobs;
        }));
    }

    for (const Luminumbra::JobHandle& handle : handles) {
        system.job_system.wait(handle);
    }

    for (const auto& count : batch_counts) {
        EXPECT_EQ(count.load(std::memory_order_relaxed), static_cast<int>(kJobsPerBatch));
    }
}

TEST(JobSystemStressTest, BatchCountersCanBeReusedAcrossSequentialBatches) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr int kBatchIterations = 512;
    constexpr int kJobsPerBatch = 8;
    constexpr auto kTimeout = 5s;

    RunningJobSystem system;

    for (int batch = 0; batch < kBatchIterations; ++batch) {
        std::atomic<int> completed{0};
        std::mutex mutex;
        std::condition_variable completed_cv;
        std::vector<Luminumbra::Job> jobs;
        jobs.reserve(kJobsPerBatch);

        for (int i = 0; i < kJobsPerBatch; ++i) {
            jobs.emplace_back([&completed, &completed_cv]() {
                const int done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done == kJobsPerBatch) {
                    completed_cv.notify_one();
                }
            });
        }

        const Luminumbra::JobHandle handle = system.job_system.dispatch_batch(jobs);

        {
            std::unique_lock<std::mutex> lock(mutex);
            ASSERT_TRUE(completed_cv.wait_for(lock, kTimeout, [&completed]() {
                return completed.load(std::memory_order_relaxed) == kJobsPerBatch;
            }));
        }

        system.job_system.wait(handle);
    }
}

} // namespace
