#include "luminumbra_client/rendering/FarVolumeResidency.h"

#include <chrono>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;

FarVolumeResidencyLimits Limits() {
    FarVolumeResidencyLimits limits;
    limits.entry_slots.fill(6);
    for (auto& queue : limits.queues)
        queue = {8,
                 2,
                 {{131 * 131, 20'000'000},
                  {131'072, 16'384},
                  {1'048'576, 3'000'000, 300'000, 1'000'000}},
                 4 * 1024 * 1024};
    return limits;
}
FarTierBudgets Budgets() {
    FarTierBudgets budgets;
    budgets.bytes.fill(64 * 1024 * 1024);
    return budgets;
}
FarVolumeWantedRequest Wanted(FarTileKey key) {
    FarVolumeTileKey canonical;
    EXPECT_TRUE(ToFarVolumeTileKey(key, canonical));
    return {{1, 42, 0, key, 1, 1}, {canonical, {}}, 0};
}
FarVolumeAuthority Authority(const FarVolumeWantedRequest& request, bool mixed = false) {
    const auto edge = FarTileEdgeMeters(request.identity.key.tier);
    const auto origin_x = double(request.identity.key.tx) * double(edge);
    const auto origin_z = double(request.identity.key.tz) * double(edge);
    return {
        request.identity.content_hash,
        static_cast<FarCaveMode>(request.identity.cave_policy),
        FarVolumeNumericProfile::RawFloat,
        [](std::int64_t, std::int64_t) { return 0.0f; },
        [mixed, origin_x, origin_z](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
            const auto local_x = p.x - origin_x;
            const auto local_z = p.z - origin_z;
            const bool column =
                mixed && local_x >= 0 && local_x <= 256 && local_z >= 0 && local_z <= 256;
            return FarVolumeSample{column ? float(p.y) + .25f : 3.0f, 7};
        }};
}
FarVolumeAuthorityProducer Producer(bool mixed = false) {
    return [mixed](const auto& request, auto&) {
        return Authority(request, mixed);
    };
}
FarVolumeResidentObservation Observation(const FarVolumeResidency& owner, FarTileKey key) {
    const auto value = owner.Observe(key);
    if (!value) {
        ADD_FAILURE() << "Missing resident observation";
        return {};
    }
    return *value;
}
bool WaitReady(FarVolumeResidency& owner,
               std::span<const FarVolumeWantedRequest> requests,
               const FarVolumeAuthorityProducer& producer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        owner.AdvanceFrame(0, 0, {6, 0, 0}, producer, {});
        bool ready = true;
        for (const auto& request : requests) {
            const auto state = owner.Observe(request.identity.key);
            ready = ready && state && state->eligibility == FarVolumeEligibility::Ready;
        }
        if (ready)
            return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
struct Gpu final : FarVolumeGpuAllocation {
    std::uint64_t count;
    explicit Gpu(std::uint64_t bytes)
        : count(bytes) {}
    std::uint64_t bytes() const noexcept override {
        return count;
    }
};
FarVolumeUploadBackend Backend(std::size_t& calls) {
    return [&calls](const auto& mesh) {
        ++calls;
        return std::make_unique<Gpu>(mesh.vertices.size() * sizeof(VoxelVertex) +
                                     mesh.indices.size() * sizeof(std::uint32_t));
    };
}
} // namespace

TEST(FarVolumeResidency, ConstructorRefusalRollsBackAllTierCharges) {
    JobSystem jobs;
    auto budgets = Budgets();
    budgets.bytes[2] = 1;
    FarVolumeTierMemory memory(budgets);
    EXPECT_THROW(FarVolumeResidency owner(jobs, memory, Limits()), std::bad_alloc);
    for (std::size_t i = 1; i <= kFarTierCount; ++i) {
        ASSERT_TRUE(memory.Pool(i));
        EXPECT_EQ(memory.Pool(i)->used(), 0u);
    }
}

TEST(FarVolumeResidency, ManifestValidationIsTransactionalAndSpanMonotone) {
    JobSystem jobs;
    FarVolumeTierMemory memory(Budgets());
    FarVolumeResidency owner(jobs, memory, Limits());
    auto request = Wanted({5, -1, 0});
    request.intent.extra_span = FarVolumeCoverageSpan{-512, 512};
    const std::array first{request};
    ASSERT_TRUE(owner.ReplaceWanted(first));
    auto invalid = request;
    invalid.intent.extra_span->max_y += 256;
    EXPECT_FALSE(owner.ReplaceWanted(std::array{invalid}));
    EXPECT_EQ(Observation(owner, request.identity.key).desired, request.identity);
    ++invalid.identity.span_generation;
    ASSERT_TRUE(owner.ReplaceWanted(std::array{invalid}));
    auto shrink = invalid;
    ++shrink.identity.span_generation;
    shrink.intent.extra_span->min_y = 0;
    EXPECT_FALSE(owner.ReplaceWanted(std::array{shrink}));
    EXPECT_FALSE(owner.ReplaceWanted(std::array{invalid, invalid}));
    auto bad_key = invalid;
    bad_key.identity.key.tier = 0;
    EXPECT_FALSE(owner.ReplaceWanted(std::array{bad_key}));
    EXPECT_EQ(Observation(owner, request.identity.key).desired, invalid.identity);
    ++shrink.identity.world_epoch;
    EXPECT_TRUE(owner.ReplaceWanted(std::array{shrink}));
}

TEST(FarVolumeResidency, OneGlobalDispatchBudgetAndNoFrameRetryAfterFailure) {
    JobSystem stopped;
    FarVolumeTierMemory memory(Budgets());
    FarVolumeResidency owner(stopped, memory, Limits());
    const std::array requests{Wanted({1, 0, 0}), Wanted({4, 0, 0}), Wanted({5, 0, 0})};
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    std::vector<FarTileKey> called;
    FarVolumeAuthorityProducer producer = [&called](const auto& request, auto&) {
        called.push_back(request.identity.key);
        return Authority(request);
    };
    for (int i = 0; i < 5; ++i)
        EXPECT_LE(owner.AdvanceFrame(0, 0, {1, 0, 0}, producer, {}).dispatched, 1u);
    ASSERT_EQ(called.size(), 3u);
    EXPECT_EQ(called[0].tier, 5);
    EXPECT_EQ(called[1].tier, 4);
    EXPECT_EQ(called[2].tier, 1);
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    owner.AdvanceFrame(0, 0, {3, 0, 0}, producer, {});
    EXPECT_EQ(called.size(), 3u);
    for (const auto& request : requests)
        EXPECT_EQ(Observation(owner, request.identity.key).eligibility,
                  FarVolumeEligibility::RequestRejected);
}

TEST(FarVolumeResidency, ProducerRefusalLatchesUntilMeaningfulRequestOrWantedChange) {
    JobSystem jobs;
    FarVolumeTierMemory memory(Budgets());
    FarVolumeResidency owner(jobs, memory, Limits());
    auto request = Wanted({5, 0, 0});
    ASSERT_TRUE(owner.ReplaceWanted(std::array{request}));
    std::size_t calls = 0;
    FarVolumeAuthorityProducer failed = [&calls](const auto&,
                                                 auto&) -> std::optional<FarVolumeAuthority> {
        ++calls;
        return {};
    };
    EXPECT_TRUE(owner.AdvanceFrame(0, 0, {1, 0, 0}, failed, {}).horizon_refused);
    for (int i = 0; i < 5; ++i)
        owner.AdvanceFrame(0, 0, {1, 0, 0}, failed, {});
    EXPECT_EQ(calls, 1u);
    ++request.identity.span_generation;
    request.intent.extra_span = FarVolumeCoverageSpan{-512, 256};
    ASSERT_TRUE(owner.ReplaceWanted(std::array{request}));
    owner.AdvanceFrame(0, 0, {1, 0, 0}, failed, {});
    EXPECT_EQ(calls, 2u);
    ASSERT_TRUE(owner.ReplaceWanted(std::array{request, Wanted({5, 1, 0})}));
    owner.AdvanceFrame(0, 0, {1, 0, 0}, failed, {});
    EXPECT_EQ(calls, 3u);
    owner.AdvanceFrame(0, 0, {1, 0, 0}, failed, {});
    EXPECT_EQ(calls, 4u); // failed producer still consumes a frame attempt
}

TEST(FarVolumeResidency, ActualUploadBytesCapDefersReadyAndPreservesOldGpuOnFailure) {
    JobSystem jobs;
    jobs.startup(2);
    FarVolumeTierMemory memory(Budgets());
    FarVolumeResidency owner(jobs, memory, Limits());
    auto requests = std::array{Wanted({5, 0, 0}), Wanted({5, 1, 0})};
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    ASSERT_TRUE(WaitReady(owner, requests, Producer(true)));
    const auto bytes = Observation(owner, requests[0].identity.key).ready_upload_bytes;
    ASSERT_GT(bytes, 0u);
    EXPECT_EQ(Observation(owner, requests[1].identity.key).ready_upload_bytes, bytes);
    std::size_t calls = 0;
    const auto backend = Backend(calls);
    const auto uploaded = owner.AdvanceFrame(0, 0, {0, 2, bytes}, {}, backend);
    EXPECT_EQ(uploaded.uploaded, 1u);
    EXPECT_EQ(uploaded.upload_bytes, bytes);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(Observation(owner, requests[1].identity.key).eligibility,
              FarVolumeEligibility::Ready);
    ASSERT_TRUE(owner.Resource(requests[0].identity.key));
    const auto* previous = owner.Resource(requests[0].identity.key);
    ++requests[0].identity.content_hash;
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    // Other Ready result is retained but cannot consume this build's dispatch.
    const std::array changed{requests[0]};
    ASSERT_TRUE(WaitReady(owner, changed, Producer(true)));
    FarVolumeUploadBackend failing = [](const auto&) -> std::unique_ptr<FarVolumeGpuAllocation> {
        throw std::runtime_error("partial backend failure");
    };
    EXPECT_TRUE(owner.AdvanceFrame(0, 0, {0, 1, bytes}, {}, failing).horizon_refused);
    EXPECT_EQ(owner.Resource(requests[0].identity.key), previous);
    const auto rejected = Observation(owner, requests[0].identity.key);
    EXPECT_EQ(rejected.eligibility, FarVolumeEligibility::RequestRejected);
    EXPECT_EQ(rejected.gpu_bytes, bytes);
    EXPECT_FALSE(rejected.coverage.current);
    owner.CancelAndDrain();
    EXPECT_FALSE(owner.Observe(requests[0].identity.key));
    ASSERT_TRUE(memory.Pool(5));
    // Only fixed owner/queue controls survive explicit drain, until destruction.
    const auto stats = memory.Stats(5);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->bytes_gpu, 0u);
}

TEST(FarVolumeResidency, FourCurrentEmptyChildrenPermitYieldEachFrame) {
    JobSystem jobs;
    jobs.startup(2);
    FarVolumeTierMemory memory(Budgets());
    FarVolumeResidency owner(jobs, memory, Limits());
    auto requests = std::array{Wanted({5, 0, 0}),
                               Wanted({4, 0, 0}),
                               Wanted({4, 1, 0}),
                               Wanted({4, 0, 1}),
                               Wanted({4, 1, 1})};
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    EXPECT_FALSE(owner.CoarseMayYield({5, 0, 0}));
    // Per-tier held-result slots are bounded at two, so upload while building.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        owner.AdvanceFrame(0, 0, {5, 5, 0}, Producer(), {});
        if (owner.CoarseMayYield({5, 0, 0}))
            break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(owner.CoarseMayYield({5, 0, 0}));
    EXPECT_FALSE(owner.CoarseMayYield({4, 0, 0}));
    ++requests[4].identity.span_generation;
    requests[4].intent.extra_span = FarVolumeCoverageSpan{-512, 256};
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    EXPECT_FALSE(owner.CoarseMayYield({5, 0, 0}));
    EXPECT_FALSE(Observation(owner, requests[4].identity.key).coverage.current);
    ASSERT_TRUE(owner.ReplaceWanted(std::span(requests).first(4)));
    EXPECT_FALSE(owner.CoarseMayYield({5, 0, 0}));
}

TEST(FarVolumeResidency, PendingResultSlotDoesNotReproduceAuthorityEachFrame) {
    JobSystem stopped;
    FarVolumeTierMemory memory(Budgets());
    auto limits = Limits();
    limits.queues[4].result_slots = 1;
    FarVolumeResidency owner(stopped, memory, limits);
    const std::array requests{Wanted({5, 0, 0}), Wanted({5, 1, 0})};
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    std::size_t calls = 0;
    FarVolumeAuthorityProducer producer = [&calls](const auto& request, auto&) {
        ++calls;
        return Authority(request);
    };
    const auto first = owner.AdvanceFrame(0, 0, {2, 0, 0}, producer, {});
    EXPECT_EQ(first.dispatched, 1u);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(Observation(owner, requests[1].identity.key).eligibility,
              FarVolumeEligibility::Pending);
    for (int i = 0; i < 3; ++i)
        owner.AdvanceFrame(0, 0, {2, 0, 0}, producer, {});
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(Observation(owner, requests[1].identity.key).eligibility,
              FarVolumeEligibility::RequestRejected);
}

TEST(FarVolumeResidency, ProspectiveEvictionRequiresCurrentUnderlayAndKeepsKeyLatch) {
    JobSystem jobs;
    jobs.startup(2);
    FarVolumeTierMemory memory(Budgets());
    FarVolumeResidency owner(jobs, memory, Limits());
    const auto parent = Wanted({5, 0, 0});
    const auto near = Wanted({4, 0, 0});
    auto far = Wanted({4, 1, 0});
    const std::array initial{parent, near, far};
    FarVolumeAuthorityProducer producer = [](const auto& request, auto&) {
        return Authority(request, request.identity.key.tier == 4);
    };
    ASSERT_TRUE(owner.ReplaceWanted(initial));
    ASSERT_TRUE(WaitReady(owner, initial, producer));
    std::size_t calls = 0;
    const auto backend = Backend(calls);
    ASSERT_EQ(owner.AdvanceFrame(0, 0, {0, 3, 8 * 1024 * 1024}, {}, backend).uploaded, 3u);
    ASSERT_TRUE(owner.Resource(near.identity.key));
    ASSERT_TRUE(owner.Resource(far.identity.key));
    const auto* near_gpu = owner.Resource(near.identity.key);
    auto* pool = memory.Pool(4);
    ASSERT_TRUE(pool);
    auto added = Wanted({4, 0, 1});
    added.build_headroom_bytes = pool->limit() - pool->used() + 1;
    auto wanted = std::array{parent, near, far, added};
    ASSERT_TRUE(owner.ReplaceWanted(wanted));
    const auto pressure = owner.AdvanceFrame(0, 0, {1, 0, 0}, producer, {});
    EXPECT_EQ(pressure.evicted, 1u);
    EXPECT_FALSE(owner.Resource(far.identity.key));
    EXPECT_EQ(owner.Resource(near.identity.key), near_gpu);
    EXPECT_EQ(Observation(owner, far.identity.key).eligibility,
              FarVolumeEligibility::BudgetEvicted);
    // A new real content identity does not defeat the accepted key-scoped latch.
    ++wanted[2].identity.content_hash;
    ASSERT_TRUE(owner.ReplaceWanted(wanted));
    EXPECT_EQ(Observation(owner, far.identity.key).eligibility,
              FarVolumeEligibility::BudgetEvicted);
    owner.CancelAndDrain();

    // Without a covering parent, prospective pressure preserves both fine pairs.
    const std::array fine_only{near, far};
    ASSERT_TRUE(owner.ReplaceWanted(fine_only));
    ASSERT_TRUE(WaitReady(owner, fine_only, producer));
    ASSERT_EQ(owner.AdvanceFrame(0, 0, {0, 2, 8 * 1024 * 1024}, {}, backend).uploaded, 2u);
    const auto* saved_near = owner.Resource(near.identity.key);
    const auto* saved_far = owner.Resource(far.identity.key);
    added.build_headroom_bytes = pool->limit() - pool->used() + 1;
    ASSERT_TRUE(owner.ReplaceWanted(std::array{near, far, added}));
    const auto refused = owner.AdvanceFrame(0, 0, {1, 0, 0}, producer, {});
    EXPECT_EQ(refused.evicted, 0u);
    EXPECT_EQ(refused.rejected, 1u);
    EXPECT_EQ(owner.Resource(near.identity.key), saved_near);
    EXPECT_EQ(owner.Resource(far.identity.key), saved_far);
}
