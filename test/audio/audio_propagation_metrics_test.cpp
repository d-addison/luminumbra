#include "audio/AudioPropagationSystem.h"

#include <glm/ext/vector_float3.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// The regression target alone supplies these hooks to the implementation.
struct PropagationTestState {
    std::atomic<int64_t> milliseconds{0};
    unsigned int worker_count = 3;
    size_t fail_after_workers = 0;
    bool fail_before_logging = false;
    std::atomic<bool> fail_cache_insertion{false};
    std::exception_ptr failure;
    std::atomic<size_t> started{0};
    std::atomic<size_t> exited{0};
    std::atomic<size_t> joined{0};
    std::atomic<bool> pause_publication{false};
    std::promise<void> publication_entered;
    std::shared_future<void> publication_release;
    std::barrier<>* publication_barrier = nullptr;
};

std::atomic<PropagationTestState*>& TestStateSlot() {
    static std::atomic<PropagationTestState*> state{nullptr};
    return state;
}

class ScopedTestState {
public:
    explicit ScopedTestState(PropagationTestState& state) {
        TestStateSlot().store(&state);
    }
    ~ScopedTestState() {
        TestStateSlot().store(nullptr);
    }
    ScopedTestState(const ScopedTestState&) = delete;
    ScopedTestState& operator=(const ScopedTestState&) = delete;
};

} // namespace

namespace Luminumbra::Client {

unsigned int AudioPropagationTestWorkerCount() {
    const auto* state = TestStateSlot().load();
    return state != nullptr ? state->worker_count : 3U;
}

std::chrono::steady_clock::time_point AudioPropagationTestNow() {
    const auto* state = TestStateSlot().load();
    return state != nullptr ? std::chrono::steady_clock::time_point{} +
                                  std::chrono::milliseconds(state->milliseconds.load())
                            : std::chrono::steady_clock::now();
}

void AudioPropagationTestEvent(std::string_view event, size_t worker_count) {
    auto* state = TestStateSlot().load();
    if (state == nullptr) {
        return;
    }
    if (event == "worker started") {
        state->started.store(worker_count);
        if (worker_count == state->fail_after_workers) {
            std::rethrow_exception(state->failure);
        }
    } else if (event == "before logging" && state->fail_before_logging) {
        std::rethrow_exception(state->failure);
    } else if (event == "worker exited") {
        ++state->exited;
    } else if (event == "worker joined") {
        ++state->joined;
    } else if (event == "before cache insertion" && state->fail_cache_insertion.exchange(false)) {
        std::rethrow_exception(state->failure);
    } else if (event == "before cache publication") {
        if (state->publication_barrier != nullptr) {
            state->publication_barrier->arrive_and_wait();
        }
        if (state->pause_publication.exchange(false)) {
            state->publication_entered.set_value();
            state->publication_release.wait();
        }
    }
}

} // namespace Luminumbra::Client

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

AudioPropagationResult CalculatePair(AudioPropagationSystem& system, size_t pair) {
    const float coordinate = static_cast<float>(pair * 4);
    return system.CalculatePropagation(glm::vec3(coordinate, 0.0f, 0.0f),
                                       glm::vec3(coordinate + 2.0f, 0.0f, 0.0f));
}

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

TEST(AudioPropagationCache, CapacityIsBoundedAndEvictionUsesInsertionOrder) {
    PropagationTestState state;
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    const size_t capacity = system.GetCacheDiagnostics().max_entries;
    ASSERT_EQ(capacity, 1000U);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 0U);
    for (size_t pair = 0; pair < capacity; ++pair) {
        CalculatePair(system, pair);
    }
    system.ResetMetrics();
    CalculatePair(system, 0); // A hit does not refresh insertion order.
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
    CalculatePair(system, capacity);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, capacity);
    CalculatePair(system, 1);
    EXPECT_EQ(system.GetMetrics().cache_hits, 2);
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_misses, 2);
    // Sixteen further distinct pairs exercise repeated eviction at the same bound.
    for (size_t pair = capacity + 1; pair <= capacity + 16; ++pair) {
        CalculatePair(system, pair);
        EXPECT_EQ(system.GetCacheDiagnostics().entries, capacity);
    }
}

TEST(AudioPropagationCache, ExpiryRetiresUnrelatedEntriesAndAllowsReinsertion) {
    PropagationTestState state;
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    CalculatePair(system, 0);
    state.milliseconds.store(999);
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
    CalculatePair(system, 1);
    state.milliseconds.store(1000);
    CalculatePair(system, 2);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 2U);
    system.ResetMetrics();
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_misses, 1);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 3U);
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
    state.milliseconds.store(2000);
    CalculatePair(system, 3);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 1U);
}

TEST(AudioPropagationCache, ConcurrentDistinctPairsStayBounded) {
    PropagationTestState state;
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    constexpr size_t writer_count = 4;
    const size_t capacity = system.GetCacheDiagnostics().max_entries;
    const size_t rounds = capacity / writer_count + 8;
    std::barrier start(static_cast<std::ptrdiff_t>(writer_count));
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    // Four writers publish 1032 distinct pairs, all within one cache lifetime.
    for (size_t writer = 0; writer < writer_count; ++writer) {
        writers.emplace_back([&, writer] {
            start.arrive_and_wait();
            for (size_t round = 0; round < rounds; ++round) {
                CalculatePair(system, round * writer_count + writer);
                EXPECT_LE(system.GetCacheDiagnostics().entries, capacity);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    EXPECT_EQ(system.GetCacheDiagnostics().entries, capacity);
    EXPECT_EQ(system.GetMetrics().cache_misses, static_cast<int>(rounds * writer_count));
    EXPECT_EQ(system.GetMetrics().cache_hits, 0);
}

TEST(AudioPropagationCache, ConcurrentDuplicatePublicationsKeepOneEntry) {
    constexpr int writer_count = 4;
    std::barrier publication(writer_count);
    PropagationTestState state;
    state.publication_barrier = &publication;
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    for (int writer = 0; writer < writer_count; ++writer) {
        writers.emplace_back([&] { CalculatePair(system, 0); });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 1U);
    EXPECT_EQ(system.GetMetrics().cache_misses, writer_count);
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
}

TEST(AudioPropagationCache, DistanceReflectionsAndSettingsBindReuse) {
    PropagationTestState state;
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    const glm::vec3 source(0.0f);
    const glm::vec3 listener(2.0f, 0.0f, 0.0f);
    system.CalculatePropagation(source, listener, 100.0f, 5);
    system.CalculatePropagation(source, listener, 100.0f, 5);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
    system.CalculatePropagation(source, listener, 1.0f, 5);
    system.CalculatePropagation(source, listener, 1.0f, 5);
    EXPECT_EQ(system.GetMetrics().cache_misses, 2);
    EXPECT_EQ(system.GetMetrics().cache_hits, 2);
    system.CalculatePropagation(source, listener, 100.0f, 0);
    system.CalculatePropagation(source, listener, 100.0f, 0);
    EXPECT_EQ(system.GetMetrics().cache_misses, 3);
    EXPECT_EQ(system.GetMetrics().cache_hits, 3);
    auto settings = system.GetSettings();
    const auto original_settings = system.GetSettings();
    settings.speed_of_sound = 200.0f;
    system.SetSettings(settings);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 0U);
    const auto changed = system.CalculatePropagation(source, listener, 100.0f, 5);
    EXPECT_FLOAT_EQ(changed.direct_path.delay_ms, 10.0f);
    EXPECT_FLOAT_EQ(original_settings.speed_of_sound, 343.0f);
    system.CalculatePropagation(source, listener, 100.0f, 5);
    EXPECT_EQ(system.GetMetrics().cache_misses, 4);
    EXPECT_EQ(system.GetMetrics().cache_hits, 4);
    // Even identical settings establish a new generation.
    system.SetSettings(settings);
    system.CalculatePropagation(source, listener, 100.0f, 5);
    EXPECT_EQ(system.GetMetrics().cache_misses, 5);
}

TEST(AudioPropagationCache, ExactPositionsAndLargeCoordinatesHaveDistinctIdentity) {
    PropagationTestState state;
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    const glm::vec3 listener(2.0f, 0.0f, 0.0f);
    system.CalculatePropagation(glm::vec3(0.0f), listener);
    const auto nearby = system.CalculatePropagation(glm::vec3(0.1f, 0.0f, 0.0f), listener);
    EXPECT_FLOAT_EQ(nearby.direct_path.total_distance, 1.9f);
    system.CalculatePropagation(glm::vec3(1.0e10f, 0.0f, 0.0f), listener);
    system.CalculatePropagation(glm::vec3(-1.0e10f, 0.0f, 0.0f), listener);
    EXPECT_EQ(system.GetMetrics().cache_misses, 4);
    system.CalculatePropagation(glm::vec3(-1.0e10f, 0.0f, 0.0f), listener);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
}

TEST(AudioPropagationCache, OlderAsyncGenerationCannotRepopulateCurrentCache) {
    PropagationTestState state;
    std::promise<void> release;
    state.publication_release = release.get_future().share();
    state.pause_publication.store(true);
    auto entered = state.publication_entered.get_future();
    ScopedTestState hooks(state);
    std::promise<AudioPropagationResult> old_result;
    std::promise<AudioPropagationResult> new_result;
    auto old_future = old_result.get_future();
    auto new_future = new_result.get_future();
    AudioPropagationSystem system(nullptr);
    const glm::vec3 source(0.0f);
    const glm::vec3 listener(2.0f, 0.0f, 0.0f);
    system.CalculatePropagationAsync(source, listener, [&](const AudioPropagationResult& result) {
        old_result.set_value(result);
    });
    entered.wait();
    auto settings = system.GetSettings();
    settings.speed_of_sound = 200.0f;
    system.SetSettings(settings);
    // Publish the newer generation before allowing the old worker to finish.
    const auto current = system.CalculatePropagation(source, listener);
    EXPECT_FLOAT_EQ(current.direct_path.delay_ms, 10.0f);
    release.set_value();
    EXPECT_FLOAT_EQ(old_future.get().direct_path.delay_ms, 2000.0f / 343.0f);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 1U);
    system.CalculatePropagationAsync(source, listener, [&](const AudioPropagationResult& result) {
        new_result.set_value(result);
    });
    EXPECT_FLOAT_EQ(new_future.get().direct_path.delay_ms, 10.0f);
    EXPECT_EQ(system.GetMetrics().cache_misses, 2);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
}

TEST(AudioPropagationStartup, FailureAfterSecondWorkerJoinsEveryStartedWorker) {
    PropagationTestState state;
    state.fail_after_workers = 2;
    state.failure = std::make_exception_ptr(std::runtime_error("startup failure after worker two"));
    ScopedTestState hooks(state);
    std::exception_ptr caught;
    try {
        AudioPropagationSystem system(nullptr);
    } catch (...) {
        caught = std::current_exception();
    }
    EXPECT_EQ(caught, state.failure);
    EXPECT_EQ(state.started.load(), 2U);
    EXPECT_EQ(state.exited.load(), 2U);
    EXPECT_EQ(state.joined.load(), 2U);
}

TEST(AudioPropagationStartup, LoggingBoundaryFailureJoinsEveryStartedWorker) {
    PropagationTestState state;
    state.fail_before_logging = true;
    state.failure = std::make_exception_ptr(std::runtime_error("startup logging failure"));
    ScopedTestState hooks(state);
    std::exception_ptr caught;
    try {
        AudioPropagationSystem system(nullptr);
    } catch (...) {
        caught = std::current_exception();
    }
    EXPECT_EQ(caught, state.failure);
    EXPECT_EQ(state.started.load(), state.worker_count);
    EXPECT_EQ(state.exited.load(), state.worker_count);
    EXPECT_EQ(state.joined.load(), state.worker_count);
}

TEST(AudioPropagationStartup, NormalStartupAsyncWorkAndShutdown) {
    // Include both a single worker and the same three-worker setup used for failures.
    for (const unsigned int count : {1U, 3U}) {
        PropagationTestState state;
        state.worker_count = count;
        ScopedTestState hooks(state);
        {
            std::promise<AudioPropagationResult> completed;
            auto result = completed.get_future();
            AudioPropagationSystem system(nullptr);
            system.CalculatePropagationAsync(
                glm::vec3(0.0f),
                glm::vec3(2.0f, 0.0f, 0.0f),
                [&](const AudioPropagationResult& value) { completed.set_value(value); });
            EXPECT_FLOAT_EQ(result.get().direct_path.total_distance, 2.0f);
        }
        EXPECT_EQ(state.started.load(), count);
        EXPECT_EQ(state.exited.load(), count);
    }
}

TEST(AudioPropagationCache, FailedInsertionRollsBackOrderAndAllowsRetry) {
    PropagationTestState state;
    state.failure = std::make_exception_ptr(std::bad_alloc{});
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    state.fail_cache_insertion.store(true);
    EXPECT_THROW(CalculatePair(system, 0), std::bad_alloc);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 0U);
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 1U);
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
    // Expiring the retry also checks that the rolled-back order node is absent.
    state.milliseconds.store(1000);
    CalculatePair(system, 1);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, 1U);
}

TEST(AudioPropagationCache, FailedInsertionAtCapacityPreservesRetainedEntries) {
    PropagationTestState state;
    state.failure = std::make_exception_ptr(std::bad_alloc{});
    ScopedTestState hooks(state);
    AudioPropagationSystem system(nullptr);
    const size_t capacity = system.GetCacheDiagnostics().max_entries;
    for (size_t pair = 0; pair < capacity; ++pair) {
        CalculatePair(system, pair);
    }
    state.fail_cache_insertion.store(true);
    EXPECT_THROW(CalculatePair(system, capacity), std::bad_alloc);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, capacity);
    system.ResetMetrics();
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_hits, 1);
    CalculatePair(system, capacity);
    EXPECT_EQ(system.GetCacheDiagnostics().entries, capacity);
    CalculatePair(system, 0);
    EXPECT_EQ(system.GetMetrics().cache_misses, 2);
}
