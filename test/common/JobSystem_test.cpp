#include "luminumbra_common/core/JobSystem.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <stdexcept>
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

// Single-worker fixture whose worker is parked on a gate job until
// release is called, so every dispatch made while the gate is held lands
// in the queues before the worker pops anything. This makes lane-selection
// order fully deterministic without timing assumptions.
class GatedSingleWorker {
public:
    GatedSingleWorker() {
        job_system.startup(1);
        job_system.dispatch([this]() {
            gate_entered.store(true, std::memory_order_release);
            while (!release_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        while (!gate_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    ~GatedSingleWorker() {
        release();
        job_system.shutdown();
    }

    void release() {
        release_gate.store(true, std::memory_order_release);
    }

    Luminumbra::JobSystem job_system;

private:
    std::atomic<bool> gate_entered{false};
    std::atomic<bool> release_gate{false};
};

constexpr int kHighLaneMarker = 1;
constexpr int kNormalLaneMarker = 0;

std::vector<Luminumbra::Job> MakeLaneMarkerJobs(int count,
                                                int marker,
                                                std::mutex& order_mutex,
                                                std::vector<int>& completion_order) {
    std::vector<Luminumbra::Job> jobs;
    jobs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        jobs.emplace_back([marker, &order_mutex, &completion_order]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            completion_order.push_back(marker);
        });
    }
    return jobs;
}

TEST(JobSystemPriorityTest, HighJobsCompleteBeforeQueuedNormalBacklog) {
    constexpr int kNormalJobs = 32;
    constexpr int kHighJobs = 5;

    GatedSingleWorker gated;
    std::mutex order_mutex;
    std::vector<int> completion_order;

    // Normal backlog is queued FIRST; the High jobs arrive behind it and must
    // still be served ahead of it (modulo the starvation guard).
    const Luminumbra::JobHandle normal_handle = gated.job_system.dispatch_batch(
        MakeLaneMarkerJobs(kNormalJobs, kNormalLaneMarker, order_mutex, completion_order));
    const Luminumbra::JobHandle high_handle = gated.job_system.dispatch_batch(
        MakeLaneMarkerJobs(kHighJobs, kHighLaneMarker, order_mutex, completion_order),
        Luminumbra::JobPriority::High);

    gated.release();
    gated.job_system.wait(high_handle);
    gated.job_system.wait(normal_handle);

    std::vector<int> order;
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        order = completion_order;
    }
    ASSERT_EQ(order.size(), static_cast<std::size_t>(kNormalJobs + kHighJobs));

    // The very first job served after the gate must come from the High lane.
    EXPECT_EQ(order.front(), kHighLaneMarker);

    // At most one Normal job per kNormalServiceInterval High jobs may be
    // interleaved before the last High job completes.
    std::size_t last_high_index = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == kHighLaneMarker) {
            last_high_index = i;
        }
    }
    std::size_t normals_before_last_high = 0;
    for (std::size_t i = 0; i < last_high_index; ++i) {
        if (order[i] == kNormalLaneMarker) {
            ++normals_before_last_high;
        }
    }
    EXPECT_LE(normals_before_last_high,
              static_cast<std::size_t>(kHighJobs) / Luminumbra::JobSystem::kNormalServiceInterval);
}

TEST(JobSystemPriorityTest, NormalJobsAreNotStarvedByHighBacklog) {
    constexpr int kHighJobs = 20;
    constexpr int kNormalJobs = 3;

    GatedSingleWorker gated;
    std::mutex order_mutex;
    std::vector<int> completion_order;

    const Luminumbra::JobHandle high_handle = gated.job_system.dispatch_batch(
        MakeLaneMarkerJobs(kHighJobs, kHighLaneMarker, order_mutex, completion_order),
        Luminumbra::JobPriority::High);
    const Luminumbra::JobHandle normal_handle = gated.job_system.dispatch_batch(
        MakeLaneMarkerJobs(kNormalJobs, kNormalLaneMarker, order_mutex, completion_order));

    gated.release();
    gated.job_system.wait(high_handle);
    gated.job_system.wait(normal_handle);

    std::vector<int> order;
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        order = completion_order;
    }
    ASSERT_EQ(order.size(), static_cast<std::size_t>(kHighJobs + kNormalJobs));

    // Every Normal job must be served after at most kNormalServiceInterval
    // High jobs each, even though the High backlog outlasts the Normal lane.
    const std::size_t interval = Luminumbra::JobSystem::kNormalServiceInterval;
    std::size_t normals_seen = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == kNormalLaneMarker) {
            ++normals_seen;
            EXPECT_LE(i + 1, normals_seen * (interval + 1))
                << "normal job " << normals_seen << " was starved until position " << i;
        }
    }
    EXPECT_EQ(normals_seen, static_cast<std::size_t>(kNormalJobs));

    // The High backlog must still outlast the last Normal job: the tail of
    // the completion order is pure High-lane work.
    EXPECT_EQ(order.back(), kHighLaneMarker);
}

TEST(JobSystemPriorityTest, RuntimeStatsReportPerLaneQueueDepths) {
    GatedSingleWorker gated;
    std::mutex order_mutex;
    std::vector<int> completion_order;

    const Luminumbra::JobHandle high_handle = gated.job_system.dispatch_batch(
        MakeLaneMarkerJobs(2, kHighLaneMarker, order_mutex, completion_order),
        Luminumbra::JobPriority::High);
    const Luminumbra::JobHandle normal_handle = gated.job_system.dispatch_batch(
        MakeLaneMarkerJobs(3, kNormalLaneMarker, order_mutex, completion_order));

    const auto queued_stats = gated.job_system.get_runtime_stats();
    EXPECT_EQ(queued_stats.worker_count, 1u);
    EXPECT_EQ(queued_stats.high_priority_queue_depth, 2u);
    EXPECT_EQ(queued_stats.normal_priority_queue_depth, 3u);
    // queue_depth stays the lane total for backward compatibility.
    EXPECT_EQ(queued_stats.queue_depth, 5u);
    EXPECT_TRUE(queued_stats.accepting_jobs);
    EXPECT_FALSE(queued_stats.stop_requested);

    gated.release();
    gated.job_system.wait(high_handle);
    gated.job_system.wait(normal_handle);

    const auto drained_stats = gated.job_system.get_runtime_stats();
    EXPECT_EQ(drained_stats.high_priority_queue_depth, 0u);
    EXPECT_EQ(drained_stats.normal_priority_queue_depth, 0u);
    EXPECT_EQ(drained_stats.queue_depth, 0u);
}

TEST(JobSystemStressTest, BatchCountersCanBeReusedAcrossSequentialBatches) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr int kBatchIterations = 512;
    constexpr int kJobsPerBatch = 8;

    RunningJobSystem system;

    for (int batch = 0; batch < kBatchIterations; ++batch) {
        std::atomic<int> completed{0};
        std::vector<Luminumbra::Job> jobs;
        jobs.reserve(kJobsPerBatch);

        for (int i = 0; i < kJobsPerBatch; ++i) {
            jobs.emplace_back(
                [&completed]() { completed.fetch_add(1, std::memory_order_relaxed); });
        }

        const Luminumbra::JobHandle handle = system.job_system.dispatch_batch(jobs);
        system.job_system.wait(handle);

        ASSERT_EQ(completed.load(std::memory_order_acquire), kJobsPerBatch) << "batch " << batch;
    }
}

// exercise the pooled-slot ring hard -- many
// dispatch/complete cycles that force the lane ring well past its initial
// capacity (256/lane) and back to empty repeatedly, asserting every job runs
// exactly once and the completion barrier drains each time. A leak or
// use-after-free in the slot recycling would surface as a wrong count or a
// hang.
TEST(JobSystemPoolTest, RingGrowsAndRecyclesAcrossManyCycles) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr int kCycles = 200;
    constexpr int kJobsPerCycle = 1000; // > initial 256/lane capacity -> growth

    RunningJobSystem system;
    std::atomic<long long> grand_total{0};

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        std::atomic<int> ran{0};
        std::vector<Luminumbra::Job> jobs;
        jobs.reserve(kJobsPerCycle);
        for (int i = 0; i < kJobsPerCycle; ++i) {
            jobs.emplace_back([&ran, &grand_total]() {
                ran.fetch_add(1, std::memory_order_relaxed);
                grand_total.fetch_add(1, std::memory_order_relaxed);
            });
        }
        const Luminumbra::JobHandle handle = system.job_system.dispatch_batch(jobs);
        system.job_system.wait(handle);
        ASSERT_EQ(ran.load(std::memory_order_acquire), kJobsPerCycle) << "cycle " << cycle;
    }

    EXPECT_EQ(grand_total.load(std::memory_order_acquire),
              static_cast<long long>(kCycles) * kJobsPerCycle);
}

// nested dispatch -- outer jobs each dispatch an
// inner batch (collecting handles) WITHOUT blocking inside the worker. The main
// thread then drains the outer barrier and every inner handle. This stresses
// the ring with interleaved outer+inner slots and recycling, but avoids the
// fixed-pool deadlock that blocking on inner work from inside a worker would
// cause (a worker parked in wait cannot service the inner jobs it waits on --
// a pre-existing thread-pool hazard, unrelated to the pooling change). The
// engine itself never blocks inside a job; nested *dispatch* is the realistic
// pattern.
TEST(JobSystemPoolTest, NestedDispatchCompletesFully) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr int kOuter = 16;
    constexpr int kInner = 32;

    RunningJobSystem system;
    std::atomic<int> inner_ran{0};
    std::mutex handles_mutex;
    std::vector<Luminumbra::JobHandle> inner_handles;
    inner_handles.reserve(kOuter);

    std::vector<Luminumbra::Job> outer_jobs;
    outer_jobs.reserve(kOuter);
    for (int o = 0; o < kOuter; ++o) {
        outer_jobs.emplace_back([&system, &inner_ran, &handles_mutex, &inner_handles]() {
            std::vector<Luminumbra::Job> inner_jobs;
            inner_jobs.reserve(kInner);
            for (int i = 0; i < kInner; ++i) {
                inner_jobs.emplace_back(
                    [&inner_ran]() { inner_ran.fetch_add(1, std::memory_order_relaxed); });
            }
            Luminumbra::JobHandle inner_handle = system.job_system.dispatch_batch(inner_jobs);
            std::lock_guard<std::mutex> lock(handles_mutex);
            inner_handles.push_back(std::move(inner_handle));
        });
    }

    const Luminumbra::JobHandle outer_handle = system.job_system.dispatch_batch(outer_jobs);
    system.job_system.wait(outer_handle); // all inner batches now dispatched

    std::vector<Luminumbra::JobHandle> to_wait;
    {
        std::lock_guard<std::mutex> lock(handles_mutex);
        to_wait = inner_handles;
    }
    for (const Luminumbra::JobHandle& handle : to_wait) {
        system.job_system.wait(handle);
    }

    EXPECT_EQ(inner_ran.load(std::memory_order_acquire), kOuter * kInner);
}

// an empty batch returns a default (null) handle
// and waiting on it is a no-op -- no allocation, no hang.
TEST(JobSystemPoolTest, EmptyBatchReturnsNullHandleAndWaitIsNoop) {
    RunningJobSystem system;
    const std::vector<Luminumbra::Job> none;
    const Luminumbra::JobHandle handle = system.job_system.dispatch_batch(none);
    EXPECT_FALSE(static_cast<bool>(handle.completion));
    EXPECT_FALSE(static_cast<bool>(handle.counter));
    // Must return immediately.
    system.job_system.wait(handle);
}

// jobs that throw must still complete their batch
// slot (run_slot's Finisher runs during unwind) so the barrier drains and the
// worker survives to serve later work.
TEST(JobSystemPoolTest, ThrowingJobsStillDrainTheBarrier) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr int kJobs = 128;
    RunningJobSystem system;
    std::atomic<int> survived{0};

    std::vector<Luminumbra::Job> jobs;
    jobs.reserve(kJobs);
    for (int i = 0; i < kJobs; ++i) {
        jobs.emplace_back([i, &survived]() {
            survived.fetch_add(1, std::memory_order_relaxed);
            if (i % 3 == 0) {
                throw std::runtime_error("intentional job failure");
            }
        });
    }

    const Luminumbra::JobHandle handle = system.job_system.dispatch_batch(jobs);
    // If a throwing job leaked its completion decrement, this would hang.
    system.job_system.wait(handle);
    EXPECT_EQ(survived.load(std::memory_order_acquire), kJobs);

    // The pool/workers are still healthy: a subsequent batch completes.
    std::atomic<int> after{0};
    std::vector<Luminumbra::Job> more;
    more.reserve(8);
    for (int i = 0; i < 8; ++i) {
        more.emplace_back([&after]() { after.fetch_add(1, std::memory_order_relaxed); });
    }
    system.job_system.wait(system.job_system.dispatch_batch(more));
    EXPECT_EQ(after.load(std::memory_order_acquire), 8);
}

// after shutdown, dispatch_batch must reject the
// batch, drive the counter to 0, and return a handle whose wait does not
// hang (the lost-wakeup-safe reject path). dispatch of a single job after
// shutdown must be a no-op.
TEST(JobSystemPoolTest, DispatchAfterShutdownRejectsWithoutHang) {
    Luminumbra::JobSystem job_system;
    job_system.startup();
    job_system.shutdown();

    std::atomic<int> ran{0};
    std::vector<Luminumbra::Job> jobs;
    jobs.reserve(4);
    for (int i = 0; i < 4; ++i) {
        jobs.emplace_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    const Luminumbra::JobHandle handle = job_system.dispatch_batch(jobs);
    // Reject path must leave a waitable handle that does not block.
    job_system.wait(handle);
    EXPECT_EQ(ran.load(std::memory_order_acquire), 0);

    // Single dispatch after shutdown is a silent no-op.
    job_system.dispatch([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(ran.load(std::memory_order_acquire), 0);
}

// PERF micro-benchmark, DISABLED by default (run
// with --gtest_also_run_disabled_tests). Measures empty-job dispatch_batch +
// wait throughput so the pooled-slot win can be reported without update the baseline
// any baseline. Reports to stdout; asserts nothing timing-dependent.
TEST(JobSystemPoolTest, DISABLED_DispatchThroughputBenchmark) {
    if (std::thread::hardware_concurrency() == 0) {
        GTEST_SKIP() << "JobSystem cannot start workers when hardware_concurrency is zero.";
    }

    constexpr int kTotal = 100000;
    constexpr int kBatch = 1000;
    constexpr int kBatches = kTotal / kBatch;

    RunningJobSystem system;
    std::atomic<long long> ran{0};

    const auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < kBatches; ++b) {
        std::vector<Luminumbra::Job> jobs;
        jobs.reserve(kBatch);
        for (int i = 0; i < kBatch; ++i) {
            jobs.emplace_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
        }
        system.job_system.wait(system.job_system.dispatch_batch(jobs));
    }
    const auto end = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const double per_job_ns = (seconds * 1e9) / static_cast<double>(kTotal);
    std::printf("[PERF] dispatch_batch+wait: %d jobs in %.4f s = %.1f ns/job (%.0f jobs/s)\n",
                kTotal,
                seconds,
                per_job_ns,
                static_cast<double>(kTotal) / seconds);
    EXPECT_EQ(ran.load(std::memory_order_acquire), static_cast<long long>(kTotal));
}

} // namespace
