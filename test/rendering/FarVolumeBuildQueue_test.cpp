#include "core/JobSystem.h"
#include "luminumbra_client/rendering/FarVolumeBuildQueue.h"
#include "luminumbra_client/rendering/FarVolumeRenderMesh.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeTile.h"
#include "world/FarWantedSet.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;

FarVolumeQueueLimits QueueLimits(std::size_t descriptors = 2, std::size_t results = 1) {
    // Analytic fixture bounds only; no production cap/performance claim.
    return {
        descriptors,
        results,
        {{131ULL * 131, 20'000'000}, {131'072, 16'384}, {1'048'576, 3'000'000, 300'000, 1'000'000}},
        4ULL * 1024ULL * 1024};
}
FarVolumeQueueRequest AirRequest(std::int64_t x = 0) {
    FarVolumeQueueRequest request;
    request.identity = {1, 42, 0, {1, x, 0}, 7, 3};
    request.intent = {{1, x, 0}, {}};
    request.authority = {42,
                         FarCaveMode::BandLimited,
                         FarVolumeNumericProfile::RawFloat,
                         [](std::int64_t, std::int64_t) { return 0.0f; },
                         [](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
                             return FarVolumeSample{1, 7};
                         }};
    request.water = std::make_shared<const FarWorldConstantWaterObservation>(
        request.identity.content_hash, false, 0.0f);
    request.water_bound = FarWorldWaterBound{true, 0, 0};
    return request;
}
FarVolumeQueueRequest PlaneRequest(std::int64_t x = 0) {
    auto request = AirRequest(x);
    request.identity.content_hash = request.authority.field_identity = 43;
    request.authority.density = [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{static_cast<float>(p.y) + .25f,
                               static_cast<std::uint8_t>(p.x < 512 ? 7 : 8)};
    };
    request.water = std::make_shared<const FarWorldConstantWaterObservation>(
        request.identity.content_hash, false, 0.0f);
    request.water_bound = FarWorldWaterBound{true, 0, 0};
    return request;
}

class Barrier {
public:
    void Enter() {
        if (!first.exchange(false))
            return;
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [this] { return released; });
    }
    bool WaitEntered() {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(10), [this] { return entered; });
    }
    void Release() {
        std::lock_guard const lock(mutex);
        released = true;
        condition.notify_all();
    }

private:
    std::atomic<bool> first{true};
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
};
class ReleaseOnExit {
public:
    explicit ReleaseOnExit(std::shared_ptr<Barrier> value)
        : barrier(std::move(value)) {}
    ~ReleaseOnExit() {
        barrier->Release();
    }

private:
    std::shared_ptr<Barrier> barrier;
};

std::optional<FarVolumeQueueCompletion> WaitResult(FarVolumeBuildQueue& queue,
                                                   const FarVolumeQueueRequest& request) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        queue.Pump(1);
        auto result = queue.TakeCompleted(request.identity, request.intent);
        if (result)
            return result;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    ADD_FAILURE() << "Canonical queue result timeout; pending=" << queue.Stats().pending
                  << " running=" << queue.Stats().running << " ready=" << queue.Stats().ready;
    return std::nullopt;
}
class AllocationObserver final : public std::pmr::memory_resource {
public:
    std::size_t Allocations() const {
        return allocations;
    }
    std::size_t Deallocations() const {
        return deallocations;
    }

private:
    std::size_t allocations = 0, deallocations = 0;
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
} // namespace

TEST(FarVolumeBuildQueue, EmptyTerrainWetWaterPublishesOneCompleteMixedResult) {
    JobSystem jobs;
    jobs.startup(2);
    FarVolumeMemoryPool pool(16ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto request = AirRequest(-1);
    request.water = std::make_shared<const FarWorldConstantWaterObservation>(42, true, 0.0f);
    request.water_bound = FarWorldWaterBound{false, 0, 0};
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    const auto result = WaitResult(queue, request);
    ASSERT_TRUE(result);
    if (result) {
        EXPECT_EQ(result->status(), FarVolumeBuildStatus::Ready);
        EXPECT_TRUE(result->mesh().water_complete);
        EXPECT_FALSE(result->mesh().water_known_dry);
        EXPECT_EQ(result->mesh().terrain_content, FarVolumeContent::Air);
        EXPECT_EQ(result->mesh().content, FarVolumeContent::Mixed);
        EXPECT_EQ(result->mesh().terrain_indices, 0u);
        EXPECT_EQ(result->mesh().water_indices, 98304u);
        EXPECT_EQ(result->retained_bytes(), FarVolumeRenderRetainedBytes(result->mesh()));
    }
    jobs.shutdown();
}

TEST(FarVolumeBuildQueue, UnavailableWaterIsTypedRefusalAndNeverReadyEmpty) {
    JobSystem jobs;
    jobs.startup(2);
    FarVolumeMemoryPool pool(16ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto request = AirRequest();
    request.water.reset();
    request.water_bound.reset();
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    const auto result = WaitResult(queue, request);
    ASSERT_TRUE(result);
    if (result) {
        EXPECT_EQ(result->status(), FarVolumeBuildStatus::WaterRefused);
        EXPECT_EQ(result->water_failure(), FarWorldWaterError::Unavailable);
        EXPECT_FALSE(result->mesh().water_complete);
    }
    jobs.shutdown();
}

TEST(FarVolumeBuildQueue, AvailableWaterWithUnavailableSamplesCannotPublishTerrainAlone) {
    class MissingSample final : public FarWorldWaterObservation {
    public:
        std::uint64_t content_identity() const noexcept override {
            return 43;
        }
        FarWorldWaterCapability WaterCapability() const noexcept override {
            return FarWorldWaterCapability::Wet;
        }
    };
    JobSystem jobs;
    jobs.startup(2);
    FarVolumeMemoryPool pool(16ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto request = PlaneRequest();
    request.water = std::make_shared<const MissingSample>();
    request.water_bound = FarWorldWaterBound{false, 0, 0};
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    const auto result = WaitResult(queue, request);
    ASSERT_TRUE(result);
    if (result) {
        EXPECT_EQ(result->status(), FarVolumeBuildStatus::WaterRefused);
        EXPECT_EQ(result->water_failure(), FarWorldWaterError::Unavailable);
        EXPECT_GT(result->mesh().terrain_indices, 0u);
        EXPECT_FALSE(result->mesh().water_complete);
    }
    jobs.shutdown();
}

TEST(FarVolumeBuildQueue, ControlAdmissionPrecedesAllocationAndUnwinds) {
    JobSystem jobs;
    for (std::uint64_t failure = 0; failure < 2; ++failure) {
        FarVolumeMemoryPool pool(1024ULL * 1024);
        pool.RefuseAttemptsFrom(failure);
        AllocationObserver allocator;
        EXPECT_THROW(FarVolumeBuildQueue(jobs, pool, QueueLimits(), &allocator),
                     FarVolumeMemoryRefused);
        EXPECT_EQ(pool.used(), 0u);
        EXPECT_EQ(allocator.Allocations(), failure);
        EXPECT_EQ(allocator.Deallocations(), failure);
    }
}

TEST(FarVolumeBuildQueue, EqualIdentityFreezesIntentAndFullDescriptorsPreserveOldRequest) {
    JobSystem jobs;
    FarVolumeMemoryPool pool(1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits(1));
    auto request = AirRequest();
    EXPECT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    EXPECT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Duplicate);
    auto altered = request;
    altered.intent.extra_span = FarVolumeCoverageSpan{-320, 32};
    EXPECT_EQ(queue.Submit(altered), FarVolumeSubmitStatus::Invalid);
    altered = request;
    altered.authority.numeric = FarVolumeNumericProfile::LegacySigned16;
    EXPECT_EQ(queue.Submit(altered), FarVolumeSubmitStatus::Invalid);
    altered = request;
    altered.authority.field_identity = 99;
    EXPECT_EQ(queue.Submit(altered), FarVolumeSubmitStatus::Invalid);
    EXPECT_EQ(queue.Submit(AirRequest(1)), FarVolumeSubmitStatus::Full);
    EXPECT_EQ(queue.Stats().descriptors, 1u);
    EXPECT_EQ(queue.Stats().pending, 1u);
    queue.Forget(request.identity.key);
    EXPECT_EQ(queue.Submit(AirRequest(1)), FarVolumeSubmitStatus::Queued);
}

TEST(FarVolumeBuildQueue, SupersededWorkReapsBeforeLatestSpanDispatch) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto barrier = std::make_shared<Barrier>();
    ReleaseOnExit const release{barrier};
    auto first = AirRequest();
    first.authority.height = [barrier](std::int64_t, std::int64_t) {
        barrier->Enter();
        return 0.0f;
    };
    ASSERT_EQ(queue.Submit(first), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Pump(1), 1u);
    ASSERT_TRUE(barrier->WaitEntered());
    auto latest = AirRequest();
    ++latest.identity.span_generation;
    latest.intent.extra_span = FarVolumeCoverageSpan{-320, 32};
    ASSERT_EQ(queue.Submit(latest), FarVolumeSubmitStatus::Coalesced);
    EXPECT_EQ(queue.Pump(1), 0u);
    EXPECT_EQ(queue.Stats().running, 1u);
    EXPECT_EQ(queue.Stats().pending, 1u);
    barrier->Release();
    auto result = WaitResult(queue, latest);
    ASSERT_TRUE(result);
    if (!result)
        return;
    EXPECT_EQ(result->identity(), latest.identity);
    EXPECT_EQ(result->status(), FarVolumeBuildStatus::ReadyEmpty);
    EXPECT_LE(result->mesh().request.span.min_y, -320);
    EXPECT_GE(result->mesh().request.span.max_y, 32);
    EXPECT_EQ(queue.Stats().dispatched, 2u);
    EXPECT_EQ(queue.Stats().stale, 1u);
    EXPECT_FALSE(queue.TakeCompleted(first.identity, first.intent));
}

TEST(FarVolumeBuildQueue, ForgottenRunningKeyCanReturnWithSameIdentityAfterDrain) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto barrier = std::make_shared<Barrier>();
    ReleaseOnExit const release{barrier};
    auto request = AirRequest();
    request.authority.height = [barrier](std::int64_t, std::int64_t) {
        barrier->Enter();
        return 0.0f;
    };
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Pump(1), 1u);
    ASSERT_TRUE(barrier->WaitEntered());
    queue.Forget(request.identity.key);
    // Leaving/re-entering wanted before the cancelled worker has returned.
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Coalesced);
    EXPECT_EQ(queue.Pump(1), 0u);
    EXPECT_EQ(queue.Stats().pending, 1u);
    auto wrong_intent = request;
    wrong_intent.intent.extra_span = FarVolumeCoverageSpan{-320, 32};
    EXPECT_EQ(queue.Submit(wrong_intent), FarVolumeSubmitStatus::Invalid);
    barrier->Release();
    auto result = WaitResult(queue, request);
    ASSERT_TRUE(result);
    if (!result)
        return;
    EXPECT_EQ(result->status(), FarVolumeBuildStatus::ReadyEmpty);
    EXPECT_EQ(result->identity(), request.identity);
    EXPECT_EQ(queue.Stats().dispatched, 2u);
    EXPECT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Duplicate);
    EXPECT_EQ(queue.Pump(1), 0u);
}

TEST(FarVolumeBuildQueue, EveryCurrencyFieldPreventsWrongCompletion) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024ULL * 1024);
    for (int axis = 0; axis < 6; ++axis) {
        FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
        const auto request = AirRequest();
        ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
        ASSERT_EQ(queue.Pump(1), 1u);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (queue.Stats().ready == 0 && std::chrono::steady_clock::now() < deadline) {
            queue.Pump(0);
            std::this_thread::yield();
        }
        ASSERT_EQ(queue.Stats().ready, 1u);
        auto expected = request.identity;
        if (axis == 0)
            ++expected.world_epoch;
        if (axis == 1)
            ++expected.content_hash;
        if (axis == 2)
            ++expected.cave_policy;
        if (axis == 3)
            ++expected.key.tx;
        if (axis == 4)
            ++expected.bake_generation;
        if (axis == 5)
            ++expected.span_generation;
        EXPECT_FALSE(queue.TakeCompleted(expected, request.intent));
        if (axis == 3) {
            // A query for another key does not consume this key's completion.
            auto correct = queue.TakeCompleted(request.identity, request.intent);
            ASSERT_TRUE(correct);
            if (!correct)
                return;
            EXPECT_EQ(correct->identity(), request.identity);
        } else
            EXPECT_EQ(queue.Stats().ready, 0u);
    }
}

TEST(FarVolumeBuildQueue, StoppedJobSystemReportsRejectedWithoutPublication) {
    JobSystem jobs;
    FarVolumeMemoryPool pool(1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    const auto request = AirRequest();
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Pump(1), 1u);
    auto result = queue.TakeCompleted(request.identity, request.intent);
    ASSERT_TRUE(result);
    if (!result)
        return;
    EXPECT_EQ(result->status(), FarVolumeBuildStatus::DispatchRejected);
    EXPECT_EQ(queue.Stats().rejected, 1u);
    EXPECT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Duplicate);
    EXPECT_EQ(queue.Pump(1), 0u);
}

TEST(FarVolumeBuildQueue, RetainedReservationsShareCapacityWithRunningAndQueuedResults) {
    JobSystem stopped;
    FarVolumeMemoryPool pool(1024ULL * 1024);
    FarVolumeBuildQueue queue(stopped, pool, QueueLimits(3, 2));
    const auto first = AirRequest();
    const auto second = AirRequest(1);
    const auto third = AirRequest(2);
    EXPECT_EQ(queue.ResultCapacity(), 2u);
    EXPECT_EQ(queue.ResultCapacity(2), 0u);
    EXPECT_EQ(queue.ResultCapacity(std::numeric_limits<std::size_t>::max()), 0u);
    for (const auto* request : {&first, &second, &third})
        ASSERT_EQ(queue.Submit(*request), FarVolumeSubmitStatus::Queued);
    EXPECT_EQ(queue.Pump(3, {}, 1), 1u);
    EXPECT_EQ(queue.ResultCapacity(1), 0u);
    EXPECT_EQ(queue.Pump(3, {}, 1), 0u);
    EXPECT_EQ(queue.Stats().ready, 1u);
    auto retained = queue.TakeCompleted(first.identity, first.intent);
    ASSERT_TRUE(retained);
    if (!retained)
        return;
    EXPECT_EQ(retained->status(), FarVolumeBuildStatus::DispatchRejected);
    EXPECT_EQ(queue.Pump(3, {}, 1), 1u);
    EXPECT_EQ(queue.ResultCapacity(1), 0u);
    queue.Pump(0);
    auto another = queue.TakeCompleted(second.identity, second.intent);
    ASSERT_TRUE(another);
    EXPECT_EQ(queue.Pump(3, {}, 2), 0u);
    EXPECT_EQ(queue.Pump(3, {}, std::numeric_limits<std::size_t>::max()), 0u);
    retained.reset();
    EXPECT_EQ(queue.Pump(3, {}, 1), 1u);
    EXPECT_EQ(queue.ResultCapacity(1), 0u);
    queue.CancelAndDrain();
    EXPECT_EQ(queue.ResultCapacity(1), 1u);
    another.reset();
    EXPECT_EQ(queue.ResultCapacity(), 2u);
    EXPECT_EQ(pool.used(), queue.Stats().control_bytes);
}

TEST(FarVolumeBuildQueue, FailedDispatchCopyRetainsReservationUntilDiscarded) {
    class ThrowingHeight {
    public:
        explicit ThrowingHeight(std::shared_ptr<std::atomic<bool>> value)
            : refuse(std::move(value)) {}
        ThrowingHeight(const ThrowingHeight& other)
            : refuse(other.refuse) {
            if (refuse->load())
                throw std::bad_alloc();
        }
        std::optional<float> operator()(std::int64_t, std::int64_t) const {
            return 0.0f;
        }

    private:
        std::shared_ptr<std::atomic<bool>> refuse;
    };
    JobSystem stopped;
    FarVolumeMemoryPool pool(1024ULL * 1024);
    FarVolumeBuildQueue queue(stopped, pool, QueueLimits());
    auto request = AirRequest();
    const auto refuse = std::make_shared<std::atomic<bool>>(false);
    request.authority.height = ThrowingHeight(refuse);
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    refuse->store(true);
    EXPECT_EQ(queue.Pump(1), 0u);
    EXPECT_EQ(queue.ResultCapacity(), 0u);
    EXPECT_EQ(queue.Stats().ready, 1u);
    auto failed = queue.TakeCompleted(request.identity, request.intent);
    ASSERT_TRUE(failed);
    if (!failed)
        return;
    EXPECT_EQ(failed->status(), FarVolumeBuildStatus::Failed);
    EXPECT_EQ(queue.ResultCapacity(1), 0u);
    failed.reset();
    EXPECT_EQ(queue.ResultCapacity(), 1u);
    queue.Forget(request.identity.key);
    EXPECT_EQ(pool.used(), queue.Stats().control_bytes);
}

TEST(FarVolumeBuildQueue, CompilationAdmissionRefusalNeedsExplicitRetry) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    const auto request = AirRequest();
    pool.RefuseAttemptsFrom(pool.attempts());
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    auto refused = WaitResult(queue, request);
    ASSERT_TRUE(refused);
    if (!refused)
        return;
    EXPECT_EQ(refused->status(), FarVolumeBuildStatus::CompileRefused);
    EXPECT_EQ(refused->compilation_failure().code, FarVolumeFacadeError::AllocationFailure);
    EXPECT_EQ(refused->compilation_failure().work, FarVolumeAuthorityWork{});
    EXPECT_EQ(pool.used(), queue.Stats().control_bytes);
    EXPECT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Duplicate);
    EXPECT_EQ(queue.Pump(1), 0u);
    pool.RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(queue.Retry(request.identity.key));
    auto accepted = WaitResult(queue, request);
    ASSERT_TRUE(accepted);
    if (!accepted)
        return;
    EXPECT_EQ(accepted->status(), FarVolumeBuildStatus::ReadyEmpty);
}

TEST(FarVolumeBuildQueue, DrainDropsEveryAuthorityOwnerBeforeReturning) {
    class Owner {
    public:
        explicit Owner(std::thread::id& value)
            : destroyed_on(&value) {}
        ~Owner() {
            *destroyed_on = std::this_thread::get_id();
        }

    private:
        std::thread::id* destroyed_on;
    };
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto barrier = std::make_shared<Barrier>();
    ReleaseOnExit const release{barrier};
    std::thread::id destroyed;
    auto owner = std::make_shared<Owner>(destroyed);
    std::weak_ptr<Owner> const weak = owner;
    auto request = AirRequest();
    request.authority.height = [owner, barrier](std::int64_t, std::int64_t) {
        barrier->Enter();
        return 0.0f;
    };
    request.authority.density =
        [owner](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
            return FarVolumeSample{1, 7};
        };
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Pump(1), 1u);
    ASSERT_TRUE(barrier->WaitEntered());
    request.authority = {};
    owner.reset();
    EXPECT_FALSE(weak.expired());
    barrier->Release();
    queue.CancelAndDrain();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(destroyed, std::this_thread::get_id());
    EXPECT_EQ(queue.Stats().descriptors, 0u);
    EXPECT_EQ(queue.Stats().running, 0u);
    EXPECT_EQ(pool.used(), queue.Stats().control_bytes);
}

TEST(FarVolumeBuildQueue, TakenGeometryChargeOutlivesQueueAndMoveReplacement) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024ULL * 1024);
    std::optional<FarVolumeQueueCompletion> retained;
    {
        FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
        const auto first = PlaneRequest();
        ASSERT_EQ(queue.Submit(first), FarVolumeSubmitStatus::Queued);
        retained = WaitResult(queue, first);
        ASSERT_TRUE(retained);
        if (!retained)
            return;
        ASSERT_EQ(retained->status(), FarVolumeBuildStatus::Ready);
        EXPECT_GT(retained->retained_bytes(), 0u);
        EXPECT_EQ(pool.used(), queue.Stats().control_bytes + retained->retained_bytes());
        const auto second = PlaneRequest(1);
        ASSERT_EQ(queue.Submit(second), FarVolumeSubmitStatus::Queued);
        auto replacement = WaitResult(queue, second);
        ASSERT_TRUE(replacement);
        if (!replacement)
            return;
        EXPECT_EQ(pool.used(),
                  queue.Stats().control_bytes + retained->retained_bytes() +
                      replacement->retained_bytes());
        *retained = std::move(*replacement);
        EXPECT_EQ(replacement->retained_bytes(), 0u);
        EXPECT_EQ(retained->identity(), second.identity);
        EXPECT_EQ(retained->mesh().origin_meters.x, 512);
        EXPECT_EQ(pool.used(), queue.Stats().control_bytes + retained->retained_bytes());
    }
    ASSERT_TRUE(retained);
    if (!retained)
        return;
    EXPECT_EQ(pool.used(), retained->retained_bytes());
    retained.reset();
    EXPECT_EQ(pool.used(), 0u);
}

TEST(FarVolumeBuildQueue, ExplicitSelectionControlsDispatchRatherThanInsertionOrder) {
    JobSystem stopped_jobs;
    FarVolumeMemoryPool pool(1024ULL * 1024);
    FarVolumeBuildQueue queue(stopped_jobs, pool, QueueLimits());
    const auto first = AirRequest();
    auto second = AirRequest(1);
    ASSERT_EQ(queue.Submit(first), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Submit(second), FarVolumeSubmitStatus::Queued);
    const std::array selected{second.identity.key, second.identity.key};
    EXPECT_EQ(queue.Pump(2, selected), 1u); // repeated key starts only once
    EXPECT_FALSE(queue.TakeCompleted(first.identity, first.intent));
    auto result = queue.TakeCompleted(second.identity, second.intent);
    ASSERT_TRUE(result);
    if (!result)
        return;
    EXPECT_EQ(result->identity(), second.identity);
    EXPECT_EQ(result->status(), FarVolumeBuildStatus::DispatchRejected);
    EXPECT_EQ(queue.Stats().pending, 1u);
    const std::array unknown{FarTileKey{1, 99, 99}};
    EXPECT_EQ(queue.Pump(1, unknown), 0u);
    EXPECT_EQ(queue.Stats().pending, 1u);
    EXPECT_EQ(queue.Pump(1), 1u);
}

TEST(FarVolumeBuildQueue, DispatchUsesNormalLaneBehindBlockedHighWorker) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024ULL * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto barrier = std::make_shared<Barrier>();
    ReleaseOnExit const release{barrier};
    jobs.dispatch([barrier] { barrier->Enter(); }, JobPriority::High);
    ASSERT_TRUE(barrier->WaitEntered());
    const auto request = AirRequest();
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Pump(1), 1u);
    const auto stats = jobs.get_runtime_stats();
    EXPECT_EQ(stats.normal_priority_queue_depth, 1u);
    EXPECT_EQ(stats.high_priority_queue_depth, 0u);
    barrier->Release();
    auto result = WaitResult(queue, request);
    ASSERT_TRUE(result);
    if (!result)
        return;
    EXPECT_EQ(result->status(), FarVolumeBuildStatus::ReadyEmpty);
}

TEST(FarVolumeBuildQueue, BatchForgetVisitsDescriptorsOnceAndPreservesUnselectedPendingWork) {
    JobSystem jobs;
    FarVolumeMemoryPool pool(64ULL * 1024 * 1024);
    AllocationObserver allocator;
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits(), &allocator);
    const auto allocations = allocator.Allocations();
    std::size_t calls = 0;
    EXPECT_EQ(queue.ForgetMatching([&](const FarTileKey&) noexcept {
        ++calls;
        return true;
    }),
              2u);
    EXPECT_EQ(calls, 0u);
    const auto first = AirRequest(-1), second = AirRequest(1);
    ASSERT_EQ(queue.Submit(first), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Submit(second), FarVolumeSubmitStatus::Queued);
    EXPECT_EQ(queue.ForgetMatching([](const FarTileKey&) noexcept { return false; }), 2u);
    EXPECT_EQ(queue.Stats().pending, 2u);
    EXPECT_EQ(queue.ForgetMatching([](const FarTileKey& key) noexcept { return key.tx < 0; }), 2u);
    EXPECT_EQ(queue.Stats().descriptors, 1u);
    EXPECT_EQ(queue.Stats().pending, 1u);
    EXPECT_EQ(queue.Submit(second), FarVolumeSubmitStatus::Duplicate);
    EXPECT_EQ(queue.Submit(first), FarVolumeSubmitStatus::Queued);
    EXPECT_EQ(allocator.Allocations(), allocations);
    EXPECT_EQ(queue.ForgetMatching([](const FarTileKey&) noexcept { return true; }), 2u);
    EXPECT_EQ(queue.Stats().descriptors, 0u);
    EXPECT_EQ(queue.Stats().pending, 0u);
    EXPECT_EQ(allocator.Allocations(), allocations);
}

TEST(FarVolumeBuildQueue, BatchForgetRunningWorkRetainsInputsUntilDrainAndStartsOnlySurvivor) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024 * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    auto barrier = std::make_shared<Barrier>();
    ReleaseOnExit const release(barrier);
    auto first = AirRequest(-1);
    first.authority.height = [barrier](std::int64_t, std::int64_t) {
        barrier->Enter();
        return 0.0f;
    };
    const auto second = AirRequest(1);
    ASSERT_EQ(queue.Submit(first), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Submit(second), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Pump(1), 1u);
    ASSERT_TRUE(barrier->WaitEntered());
    const auto retained = pool.used();
    EXPECT_EQ(queue.ForgetMatching([](const FarTileKey& key) noexcept { return key.tx < 0; }), 2u);
    EXPECT_EQ(pool.used(), retained);
    EXPECT_EQ(queue.Stats().running, 1u);
    EXPECT_FALSE(queue.TakeCompleted(first.identity, first.intent));
    barrier->Release();
    const auto result = WaitResult(queue, second);
    ASSERT_TRUE(result);
    if (result) {
        EXPECT_EQ(result->identity(), second.identity);
    }
    EXPECT_EQ(queue.Stats().descriptors, 1u);
    EXPECT_FALSE(queue.TakeCompleted(first.identity, first.intent));
}

TEST(FarVolumeBuildQueue, BatchForgetCannotReleaseTakenResultOrItsReservation) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024 * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    const auto request = PlaneRequest();
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    auto completion = WaitResult(queue, request);
    ASSERT_TRUE(completion);
    if (!completion)
        return;
    const auto retained = completion->retained_bytes();
    EXPECT_GT(retained, 0u);
    EXPECT_EQ(queue.ForgetMatching([](const FarTileKey&) noexcept { return true; }), 2u);
    EXPECT_EQ(queue.Stats().descriptors, 0u);
    EXPECT_GE(pool.used(), retained);
    EXPECT_EQ(completion->identity(), request.identity);
    EXPECT_EQ(queue.ResultCapacity(1), 0u);
    EXPECT_EQ(queue.Submit(AirRequest(1)), FarVolumeSubmitStatus::Queued);
    EXPECT_EQ(queue.Pump(1, {}, 1), 0u);
    completion.reset();
    EXPECT_EQ(queue.ResultCapacity(), 1u);
}

TEST(FarVolumeBuildQueue, BatchForgetReleasesReadyUntakenResultAfterWorkerCompletion) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeMemoryPool pool(64ULL * 1024 * 1024);
    FarVolumeBuildQueue queue(jobs, pool, QueueLimits());
    const auto request = PlaneRequest();
    ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    ASSERT_EQ(queue.Pump(1), 1u);
    // Join provides a deterministic completion boundary without polling time.
    jobs.shutdown();
    queue.Pump(0);
    ASSERT_EQ(queue.Stats().ready, 1u);
    const auto retained = pool.used();
    EXPECT_EQ(queue.ForgetMatching([](const FarTileKey&) noexcept { return true; }), 2u);
    EXPECT_EQ(queue.Stats().ready, 0u);
    EXPECT_EQ(queue.Stats().descriptors, 0u);
    EXPECT_EQ(queue.ResultCapacity(), 1u);
    EXPECT_LT(pool.used(), retained);
    EXPECT_FALSE(queue.TakeCompleted(request.identity, request.intent));
}
