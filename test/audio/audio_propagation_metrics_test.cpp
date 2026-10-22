#include "audio/AudioPropagationSystem.h"

#include <glm/ext/vector_float3.hpp>
#include <gtest/gtest.h>

#include <barrier>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Luminumbra::Client::AudioPropagationResult;
using Luminumbra::Client::AudioPropagationSystem;

static_assert(std::is_same_v<decltype(std::declval<const AudioPropagationSystem&>().GetMetrics()),
                             AudioPropagationSystem::PerformanceMetrics>);

void ExpectCoherent(const AudioPropagationSystem::PerformanceMetrics& metrics) {
    EXPECT_EQ(metrics.completed_calculations, metrics.cache_misses);
    EXPECT_GE(metrics.active_calculations, 0);
    EXPECT_GE(metrics.last_calculation_time_ms, 0.0f);
    EXPECT_GE(metrics.avg_calculation_time_ms, 0.0f);
    if (metrics.completed_calculations == 0) {
        EXPECT_FLOAT_EQ(metrics.last_calculation_time_ms, 0.0f);
        EXPECT_FLOAT_EQ(metrics.avg_calculation_time_ms, 0.0f);
    }
}

// The final callback capture is destroyed after the worker decrements its
// active gauge. This observes completion through public callback ownership,
// without sleeps, polling, or access to the worker's private state.
class CallbackCompletion {
public:
    std::future<void> GetFuture() {
        return m_finished.get_future();
    }
    ~CallbackCompletion() {
        m_finished.set_value();
    }

private:
    std::promise<void> m_finished;
};

} // namespace

TEST(AudioPropagationMetrics, SnapshotsOwnAllFieldsAndResetClearsStatistics) {
    AudioPropagationSystem system(nullptr);
    const glm::vec3 source(0.0f);
    const glm::vec3 listener(2.0f, 0.0f, 0.0f);
    system.CalculatePropagation(source, listener);
    const auto& snapshot = system.GetMetrics();
    EXPECT_EQ(snapshot.completed_calculations, 1);
    EXPECT_EQ(snapshot.cache_misses, 1);
    EXPECT_EQ(snapshot.cache_hits, 0);
    EXPECT_FLOAT_EQ(snapshot.avg_calculation_time_ms, snapshot.last_calculation_time_ms * 0.1f);

    system.CalculatePropagation(glm::vec3(4.0f, 0.0f, 0.0f), listener);
    EXPECT_EQ(system.GetMetrics().completed_calculations, 2);
    EXPECT_EQ(snapshot.cache_hits, 0);
    system.ResetMetrics();
    const auto reset = system.GetMetrics();
    ExpectCoherent(reset);
    EXPECT_EQ(reset.completed_calculations, 0);
    EXPECT_EQ(reset.cache_hits, 0);
    EXPECT_EQ(reset.active_calculations, 0);
    EXPECT_EQ(snapshot.completed_calculations, 1);
    EXPECT_EQ(snapshot.cache_misses, 1);
}

TEST(AudioPropagationMetrics, ConcurrentUpdatesAndReadsKeepExactTotalsAndCoherentSnapshots) {
    AudioPropagationSystem system(nullptr);
    constexpr int writer_count = 4;
    constexpr int rounds = 64;
    std::barrier phase(writer_count + 1);
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    // Fixed 256 distinct cache keys bound both fixture work and cache storage.
    for (int writer = 0; writer < writer_count; ++writer) {
        writers.emplace_back([&, writer] {
            for (int round = 0; round < rounds; ++round) {
                phase.arrive_and_wait();
                const auto coordinate = static_cast<float>((round * writer_count + writer) * 4);
                system.CalculatePropagation(glm::vec3(coordinate, 0.0f, 0.0f),
                                            glm::vec3(coordinate + 2.0f, 0.0f, 0.0f));
                phase.arrive_and_wait();
            }
        });
    }
    for (int round = 0; round < rounds; ++round) {
        phase.arrive_and_wait();
        const auto during = system.GetMetrics();
        ExpectCoherent(during);
        phase.arrive_and_wait();
        const auto after = system.GetMetrics();
        ExpectCoherent(after);
        EXPECT_EQ(after.completed_calculations, (round + 1) * writer_count);
        EXPECT_EQ(after.cache_hits, 0);
        EXPECT_EQ(after.active_calculations, 0);
    }
    for (auto& writer : writers) {
        writer.join();
    }
}

TEST(AudioPropagationMetrics, ResetPreservesInFlightCallbackAndCountsLaterUpdates) {
    AudioPropagationSystem system(nullptr);
    std::promise<void> entered;
    std::promise<void> release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    auto completion = std::make_shared<CallbackCompletion>();
    auto finished = completion->GetFuture();
    const glm::vec3 source(0.0f);
    const glm::vec3 listener(2.0f, 0.0f, 0.0f);
    system.CalculatePropagationAsync(
        source, listener, [&, receipt = std::move(completion)](const AudioPropagationResult&) {
            EXPECT_NE(receipt, nullptr);
            // Reenter both public metrics operations from a worker callback.
            EXPECT_EQ(system.GetMetrics().active_calculations, 1);
            system.ResetMetrics();
            entered.set_value();
            release_future.wait();
            system.CalculatePropagation(glm::vec3(4.0f, 0.0f, 0.0f), listener);
        });
    entered_future.wait();
    const auto before = system.GetMetrics();
    EXPECT_EQ(before.active_calculations, 1);
    system.ResetMetrics();
    const auto reset = system.GetMetrics();
    ExpectCoherent(reset);
    EXPECT_EQ(reset.active_calculations, 1);
    EXPECT_EQ(reset.cache_hits, 0);
    EXPECT_EQ(reset.completed_calculations, 0);
    release.set_value();
    finished.wait();
    const auto after = system.GetMetrics();
    ExpectCoherent(after);
    EXPECT_EQ(after.active_calculations, 0);
    EXPECT_EQ(after.cache_hits, 0);
    EXPECT_EQ(after.cache_misses, 1);
    EXPECT_FLOAT_EQ(after.avg_calculation_time_ms, after.last_calculation_time_ms * 0.1f);
    EXPECT_EQ(before.active_calculations, 1);
    EXPECT_EQ(reset.active_calculations, 1);
}
