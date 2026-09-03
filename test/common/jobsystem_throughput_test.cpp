#include "luminumbra_common/core/JobSystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <vector>

namespace {

using namespace std::chrono_literals;

TEST(JobSystemPoolTest, DispatchThroughputBenchmark) {
    constexpr int kTotalJobs = 100000;
    constexpr int kJobsPerBatch = 1000;
    constexpr int kBatchCount = kTotalJobs / kJobsPerBatch;
    constexpr auto kMaximumDuration = 10s;

    Luminumbra::JobSystem job_system;
    job_system.startup(1);
    std::atomic<int> completed{0};

    const auto start = std::chrono::steady_clock::now();
    for (int batch = 0; batch < kBatchCount; ++batch) {
        std::vector<Luminumbra::Job> jobs;
        jobs.reserve(kJobsPerBatch);
        for (int job = 0; job < kJobsPerBatch; ++job) {
            jobs.emplace_back(
                [&completed]() { completed.fetch_add(1, std::memory_order_relaxed); });
        }
        job_system.wait(job_system.dispatch_batch(jobs));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    job_system.shutdown();

    EXPECT_EQ(completed.load(std::memory_order_acquire), kTotalJobs);
    EXPECT_LT(elapsed, kMaximumDuration)
        << "dispatch_batch throughput fell below "
        << (kTotalJobs / std::chrono::duration<double>(kMaximumDuration).count()) << " jobs/second";
}

} // namespace
