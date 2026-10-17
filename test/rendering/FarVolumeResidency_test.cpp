#include "core/JobSystem.h"
#include "luminumbra_client/rendering/FarVolumeBuildQueue.h"
#include "luminumbra_client/rendering/FarVolumeHandoff.h"
#include "luminumbra_client/rendering/FarVolumeResidency.h"
#include "luminumbra_client/rendering/FarVolumeUpload.h"
#include "luminumbra_client/rendering/FarWorldOwnership.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "world/Chunk.h"
#include "world/FarEditedBake.h"
#include "world/FarFrozenObservation.h"
#include "world/FarTierTable.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeRuntime.h"
#include "world/FarVolumeTierMemory.h"
#include "world/FarVolumeTile.h"
#include "world/FarWantedSet.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <latch>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

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
                 {{131ULL * 131, 20'000'000},
                  {131'072, 16'384},
                  {1'048'576, 3'000'000, 300'000, 1'000'000}},
                 4ULL * 1024 * 1024};
    return limits;
}
FarTierBudgets Budgets() {
    FarTierBudgets budgets;
    budgets.bytes.fill(64ULL * 1024 * 1024);
    return budgets;
}
FarVolumeWantedRequest Wanted(FarTileKey key) {
    FarVolumeTileKey canonical;
    EXPECT_TRUE(ToFarVolumeTileKey(key, canonical));
    return {{1, 42, 0, key, 1, 1},
            {canonical, {}},
            0,
            std::make_shared<const FarWorldConstantWaterObservation>(42, false, 0.0f),
            FarWorldWaterBound{true, 0, 0}};
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
            const auto local_x = double(p.x) - origin_x;
            const auto local_z = double(p.z) - origin_z;
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
               const FarVolumeAuthorityProducer& producer,
               std::size_t builds = 6) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        owner.AdvanceFrame(0, 0, {builds, 0, 0}, producer, {});
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
class Gpu final : public FarVolumeGpuAllocation {
public:
    explicit Gpu(std::uint64_t bytes)
        : count_(bytes) {}
    std::uint64_t bytes() const noexcept override {
        return count_;
    }

private:
    std::uint64_t count_;
};
FarVolumeUploadBackend Backend(std::size_t& calls) {
    return [&calls](const auto& mesh) {
        ++calls;
        return std::make_unique<Gpu>(mesh.vertices.size() * sizeof(VoxelVertex) +
                                     mesh.indices.size() * sizeof(std::uint32_t));
    };
}
} // namespace

TEST(FarVolumeResidency, CombinedWetUploadDebitsAndCurrentArrivalRestoresAfterWaterFailure) {
    JobSystem jobs;
    jobs.startup(2);
    FarVolumeTierMemory memory(Budgets());
    auto limits = Limits();
    limits.queues[0].result_slots = 4;
    FarVolumeResidency owner(jobs, memory, limits);
    std::array requests{Wanted({2, -1, -1}),
                        Wanted({1, -2, -2}),
                        Wanted({1, -1, -2}),
                        Wanted({1, -2, -1}),
                        Wanted({1, -1, -1})};
    for (auto& request : requests) {
        request.water = std::make_shared<const FarWorldConstantWaterObservation>(42, true, 0.0f);
        request.water_bound = FarWorldWaterBound{false, 0, 0};
    }
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    ASSERT_TRUE(WaitReady(owner, requests, Producer()));
    std::size_t calls = 0;
    const auto backend = Backend(calls);
    const auto before = Observation(owner, requests[0].identity.key);
    auto frame = owner.AdvanceFrame(0, 0, {0, 1, before.ready_upload_bytes}, Producer(), backend);
    EXPECT_EQ(frame.upload_bytes, before.ready_upload_bytes);
    EXPECT_EQ(frame.successful_upload_bytes, frame.upload_bytes);
    EXPECT_EQ(frame.upload_attempts, 1u);
    EXPECT_FALSE(owner.CoarseMayYield(requests[0].identity.key));
    owner.AdvanceFrame(0, 0, {0, 5, 32ULL * 1024 * 1024}, Producer(), backend);
    ASSERT_TRUE(owner.CoarseMayYield(requests[0].identity.key));
    EXPECT_EQ(owner.Snapshot()[0].water_wet_tiles, 4u);
    EXPECT_EQ(owner.Snapshot()[0].retained_water_indices, 4u * 98304);
    const auto collect = [](void* opaque, const FarVolumeDrawRecord& record) {
        auto& records = *static_cast<std::vector<FarVolumeDrawRecord>*>(opaque);
        records.push_back(record);
    };
    // A missing fine child invalidates the complete terrain/water certificate.
    ASSERT_TRUE(owner.ReplaceWanted(std::span(requests).first(4)));
    owner.AdvanceFrame(0, 0, {}, {}, {});
    std::vector<FarVolumeDrawRecord> draws;
    owner.VisitDraws(collect, &draws);
    ASSERT_FALSE(draws.empty());
    EXPECT_EQ(draws.front().identity.key, requests[0].identity.key);
    EXPECT_FALSE(draws.front().arrival.eligible);
    EXPECT_EQ(draws.front().water_indices, 98304u);
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    ASSERT_TRUE(WaitReady(owner, std::span(requests).last(1), Producer()));
    owner.AdvanceFrame(0, 0, {0, 1, 32ULL * 1024 * 1024}, Producer(), backend);
    ASSERT_TRUE(owner.CoarseMayYield(requests[0].identity.key));
    const auto failed_key = requests[1].identity.key;
    const auto* old = owner.Resource(failed_key);
    ASSERT_NE(old, nullptr);
    ++requests[1].identity.bake_generation;
    requests[1].water.reset();
    requests[1].water_bound.reset();
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    EXPECT_FALSE(owner.CoarseMayYield(requests[0].identity.key));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        owner.AdvanceFrame(0, 0, {5, 5, 32ULL * 1024 * 1024}, Producer(), backend);
        if (Observation(owner, failed_key).eligibility == FarVolumeEligibility::RequestRejected)
            break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(Observation(owner, failed_key).eligibility, FarVolumeEligibility::RequestRejected);
    EXPECT_EQ(owner.Resource(failed_key), old);
    EXPECT_FALSE(IsFarTileCovering(Observation(owner, failed_key).coverage));
    EXPECT_FALSE(owner.CoarseMayYield(requests[0].identity.key));
    draws.clear();
    owner.VisitDraws(collect, &draws);
    ASSERT_FALSE(draws.empty());
    EXPECT_EQ(draws.front().identity.key, requests[0].identity.key);
    EXPECT_FALSE(draws.front().arrival.eligible);
    EXPECT_EQ(draws.front().water_indices, 98304u);
    jobs.shutdown();
}

TEST(FarVolumeResidency, ConstructorRefusalRollsBackAllTierCharges) {
    JobSystem jobs;
    auto budgets = Budgets();
    budgets.bytes[2] = 1;
    FarVolumeTierMemory memory(budgets);
    EXPECT_THROW(FarVolumeResidency const owner(jobs, memory, Limits()), std::bad_alloc);
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
    ASSERT_TRUE(invalid.intent.extra_span);
    if (!invalid.intent.extra_span)
        return;
    invalid.intent.extra_span->max_y += 256;
    EXPECT_FALSE(owner.ReplaceWanted(std::array{invalid}));
    EXPECT_EQ(Observation(owner, request.identity.key).desired, request.identity);
    ++invalid.identity.span_generation;
    ASSERT_TRUE(owner.ReplaceWanted(std::array{invalid}));
    auto shrink = invalid;
    ++shrink.identity.span_generation;
    ASSERT_TRUE(shrink.intent.extra_span);
    if (!shrink.intent.extra_span)
        return;
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
    FarVolumeAuthorityProducer const producer = [&called](const auto& request, auto&) {
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
    FarVolumeAuthorityProducer const failed = [&calls](const auto&,
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
    requests[0].water = std::make_shared<const FarWorldConstantWaterObservation>(
        requests[0].identity.content_hash, false, 0.0f);
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    // Other Ready result is retained but cannot consume this build's dispatch.
    const std::array changed{requests[0]};
    ASSERT_TRUE(WaitReady(owner, changed, Producer(true)));
    FarVolumeUploadBackend failing = [](const auto&) -> std::unique_ptr<FarVolumeGpuAllocation> {
        throw std::runtime_error("partial backend failure");
    };
    std::size_t failed_calls = 0;
    FarVolumeUploadBackend const counted_failure = [&](const auto& mesh) {
        ++failed_calls;
        return failing(mesh);
    };
    const auto failure = owner.AdvanceFrame(0, 0, {0, 2, bytes}, {}, counted_failure);
    EXPECT_TRUE(failure.horizon_refused);
    EXPECT_EQ(failure.upload_bytes, bytes);
    EXPECT_EQ(failure.successful_upload_bytes, 0u);
    EXPECT_EQ(failed_calls, 1u);
    EXPECT_EQ(Observation(owner, requests[1].identity.key).eligibility,
              FarVolumeEligibility::Ready);
    EXPECT_EQ(owner.Resource(requests[0].identity.key), previous);
    const auto rejected = Observation(owner, requests[0].identity.key);
    EXPECT_EQ(rejected.eligibility, FarVolumeEligibility::RequestRejected);
    EXPECT_EQ(rejected.gpu_bytes, bytes);
    EXPECT_FALSE(rejected.coverage.current);
    const auto state = owner.Snapshot();
    EXPECT_EQ(state[4].wanted, 2u);
    EXPECT_EQ(state[4].request_rejected, 1u);
    EXPECT_EQ(state[4].ready, 1u);
    EXPECT_EQ(state[4].uploaded_pairs, 1u);
    EXPECT_EQ(state[4].covering, 0u);
    EXPECT_EQ(state[4].retained_gpu_bytes, bytes);
    EXPECT_EQ(state[4].memory.bytes_gpu, bytes);
    EXPECT_GT(state[4].memory.bytes_total, bytes);
    owner.CancelAndDrain();
    EXPECT_FALSE(owner.Observe(requests[0].identity.key));
    ASSERT_TRUE(memory.Pool(5));
    // Only fixed owner/queue controls survive explicit drain, until destruction.
    const auto stats = memory.Stats(5);
    ASSERT_TRUE(stats);
    if (!stats)
        return;
    EXPECT_EQ(stats->bytes_gpu, 0u);
}

TEST(FarVolumeResidency, FourCurrentEmptyChildrenPermitClippedArrivalEachFrame) {
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
    FarVolumeAuthorityProducer const producer = [](const auto& request, auto&) {
        return Authority(request, request.identity.key.tier == 5);
    };
    std::size_t backend_calls = 0;
    const auto backend = Backend(backend_calls);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        owner.AdvanceFrame(0, 0, {5, 5, 4ULL * 1024 * 1024}, producer, backend);
        if (owner.CoarseMayYield({5, 0, 0}) &&
            IsFarTileCovering(Observation(owner, requests[0].identity.key).coverage))
            break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(owner.CoarseMayYield({5, 0, 0}));
    ASSERT_TRUE(IsFarTileCovering(Observation(owner, requests[0].identity.key).coverage));
    const auto settled = owner.Snapshot();
    EXPECT_EQ(settled[3].empty_pairs, 4u);
    EXPECT_EQ(settled[3].covering, 4u);
    EXPECT_EQ(settled[4].uploaded_pairs, 1u);
    EXPECT_GT(settled[4].retained_gpu_bytes, 0u);
    EXPECT_EQ(backend_calls, 1u);
    std::vector<FarVolumeDrawRecord> records;
    const auto collect = [](void* context, const FarVolumeDrawRecord& record) {
        static_cast<std::vector<FarVolumeDrawRecord>*>(context)->push_back(record);
    };
    ASSERT_EQ(owner.VisitDraws(collect, &records), 1u);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_TRUE(records[0].arrival.eligible);
    EXPECT_EQ(records[0].arrival.finer_outer_radius_meters, 8320);
    for (std::size_t i = 0; i < records[0].arrival.children.size(); ++i) {
        const auto& span = records[0].arrival.children[i];
        EXPECT_LT(span.min_y, span.max_y);
        EXPECT_TRUE(FarWorldArrivalReplaces(
            records[0].arrival,
            8192,
            {10, static_cast<float>(span.min_y - records[0].origin_meters.y), 10},
            records[0].origin_meters.y,
            10,
            10));
    }
    // The coarse far corner is outside the finer clip, even with four Empty children.
    EXPECT_FALSE(FarWorldArrivalReplaces(records[0].arrival, 8192, {8191, 0, 8191}, 0, 8191, 8191));
    records.clear();
    EXPECT_FALSE(owner.CoarseMayYield({4, 0, 0}));
    ++requests[4].identity.span_generation;
    requests[4].intent.extra_span = FarVolumeCoverageSpan{-512, 256};
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    EXPECT_FALSE(owner.CoarseMayYield({5, 0, 0}));
    EXPECT_FALSE(Observation(owner, requests[4].identity.key).coverage.current);
    const auto stale = owner.Snapshot();
    EXPECT_EQ(stale[3].empty_pairs, 4u);
    EXPECT_EQ(stale[3].covering, 3u);
    ASSERT_EQ(owner.VisitDraws(collect, &records), 1u);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_FALSE(records[0].arrival.eligible);
    EXPECT_EQ(records[0].identity, requests[0].identity);
    EXPECT_EQ(records[0].resource, owner.Resource(requests[0].identity.key));
    EXPECT_EQ(records[0].horizontal_outer_clip_meters, 16384);
    EXPECT_EQ(records[0].view_ray_bias_meters, 32);
    EXPECT_EQ(records[0].origin_meters, (FarVolumePosition{0, records[0].request.span.min_y, 0}));
    EXPECT_THROW(owner.VisitDraws(
                     [](void* context, const auto&) {
                         auto& runtime = *static_cast<FarVolumeResidency*>(context);
                         runtime.CancelAndDrain();
                     },
                     &owner),
                 std::logic_error);
    EXPECT_EQ(owner.VisitDraws(collect, &records), 1u); // exception guard unwound
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
    FarVolumeAuthorityProducer const producer = [&calls](const auto& request, auto&) {
        ++calls;
        return Authority(request);
    };
    const auto first = owner.AdvanceFrame(0, 0, {2, 0, 0}, producer, {});
    EXPECT_EQ(first.dispatched, 1u);
    EXPECT_EQ(first.build_attempts, 1u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(Observation(owner, requests[1].identity.key).eligibility,
              FarVolumeEligibility::Wanted);
    for (int i = 0; i < 3; ++i)
        owner.AdvanceFrame(0, 0, {2, 0, 0}, producer, {});
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(Observation(owner, requests[1].identity.key).eligibility,
              FarVolumeEligibility::RequestRejected);
}

TEST(FarVolumeResidency, DeferredUploadReservesResultSlotBeforeBuildAdmission) {
    JobSystem jobs;
    jobs.startup(1);
    FarVolumeTierMemory memory(Budgets());
    auto limits = Limits();
    limits.queues[4].result_slots = 1;
    FarVolumeResidency owner(jobs, memory, limits);
    std::array requests{Wanted({5, 0, 0}), Wanted({5, 1, 0})};
    auto* pool = memory.Pool(5);
    requests[1].build_headroom_bytes = pool->limit() / 2;
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    const auto producer = Producer(true);
    ASSERT_EQ(owner.AdvanceFrame(0, 0, {1, 0, 0}, producer, {}).dispatched, 1u);
    ASSERT_TRUE(WaitReady(owner, std::span(requests).first(1), producer, 0));
    const auto ready = Observation(owner, requests[0].identity.key);
    ASSERT_GT(ready.ready_upload_bytes, 0u);
    const auto waiting = owner.AdvanceFrame(0, 0, {2, 0, 0}, producer, {});
    ASSERT_EQ(waiting.dispatched, 0u);
    EXPECT_EQ(waiting.build_attempts, 0u);
    EXPECT_EQ(waiting.rejected, 0u);
    ASSERT_LE(ready.ready_upload_bytes, pool->limit() - pool->used());
    FarVolumeMemoryLease pressure(*pool);
    ASSERT_TRUE(pressure.TryCharge(pool->limit() - pool->used() - ready.ready_upload_bytes));
    for (int frame = 0; frame < 3; ++frame) {
        const auto deferred = owner.AdvanceFrame(0, 0, {2, 0, 0}, producer, {});
        EXPECT_EQ(deferred.dispatched, 0u);
        EXPECT_EQ(deferred.build_attempts, 0u);
        EXPECT_EQ(deferred.rejected, 0u);
        EXPECT_EQ(deferred.evicted, 0u);
        EXPECT_EQ(Observation(owner, requests[0].identity.key).cpu_bytes, ready.cpu_bytes);
        EXPECT_EQ(Observation(owner, requests[1].identity.key).eligibility,
                  FarVolumeEligibility::Wanted);
    }
    std::size_t calls = 0;
    const auto backend = Backend(calls);
    EXPECT_EQ(owner.AdvanceFrame(0, 0, {0, 1, ready.ready_upload_bytes}, {}, backend).uploaded, 1u);
    pressure.Reset();
    EXPECT_EQ(owner.AdvanceFrame(0, 0, {1, 0, 0}, producer, {}).dispatched, 1u);
    ASSERT_TRUE(WaitReady(owner, std::span(requests).last(1), producer, 0));
    EXPECT_EQ(owner.AdvanceFrame(0, 0, {0, 1, 32ULL * 1024 * 1024}, {}, backend).uploaded, 1u);
    EXPECT_EQ(calls, 2u);
    for (const auto& request : requests)
        EXPECT_TRUE(IsFarTileCovering(Observation(owner, request.identity.key).coverage));
    owner.CancelAndDrain();
    const auto drained = owner.Snapshot()[4];
    EXPECT_EQ(drained.wanted, 0u);
    EXPECT_EQ(drained.building, 0u);
    EXPECT_EQ(drained.ready_cpu_bytes, 0u);
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
    FarVolumeAuthorityProducer const producer = [](const auto& request, auto&) {
        return Authority(request, request.identity.key.tier == 4);
    };
    ASSERT_TRUE(owner.ReplaceWanted(initial));
    ASSERT_TRUE(WaitReady(owner, initial, producer));
    std::size_t calls = 0;
    const auto backend = Backend(calls);
    ASSERT_EQ(owner.AdvanceFrame(0, 0, {0, 3, 8ULL * 1024 * 1024}, {}, backend).uploaded, 3u);
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
    wanted[2].water = std::make_shared<const FarWorldConstantWaterObservation>(
        wanted[2].identity.content_hash, false, 0.0f);
    ASSERT_TRUE(owner.ReplaceWanted(wanted));
    EXPECT_EQ(Observation(owner, far.identity.key).eligibility,
              FarVolumeEligibility::BudgetEvicted);
    owner.CancelAndDrain();

    // Without a covering parent, prospective pressure preserves both fine pairs.
    const std::array fine_only{near, far};
    ASSERT_TRUE(owner.ReplaceWanted(fine_only));
    ASSERT_TRUE(WaitReady(owner, fine_only, producer));
    ASSERT_EQ(owner.AdvanceFrame(0, 0, {0, 2, 8ULL * 1024 * 1024}, {}, backend).uploaded, 2u);
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

TEST(FarVolumeResidency, V2ArrivalMetadataRetainsOriginAndNegativeRadiusCorners) {
    for (const std::int64_t coordinate : {0, -1}) {
        JobSystem jobs;
        jobs.startup(2);
        FarVolumeTierMemory memory(Budgets());
        FarVolumeResidency owner(jobs, memory, Limits());
        auto requests = std::array{Wanted({2, coordinate, coordinate}),
                                   Wanted({1, coordinate * 2, coordinate * 2}),
                                   Wanted({1, coordinate * 2 + 1, coordinate * 2}),
                                   Wanted({1, coordinate * 2, coordinate * 2 + 1}),
                                   Wanted({1, coordinate * 2 + 1, coordinate * 2 + 1})};
        ASSERT_TRUE(owner.ReplaceWanted(requests));
        FarVolumeAuthorityProducer const producer = [](const auto& request, auto&) {
            return Authority(request, request.identity.key.tier == 2);
        };
        std::size_t calls = 0;
        const auto backend = Backend(calls);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            owner.AdvanceFrame(0, 0, {5, 5, 4ULL * 1024 * 1024}, producer, backend);
            if (owner.CoarseMayYield(requests[0].identity.key) &&
                IsFarTileCovering(Observation(owner, requests[0].identity.key).coverage))
                break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        ASSERT_TRUE(owner.CoarseMayYield(requests[0].identity.key));
        FarWorldArrival arrival;
        const auto inspect = [](void* context, const FarVolumeDrawRecord& record) {
            *static_cast<FarWorldArrival*>(context) = record.arrival;
        };
        ASSERT_EQ(owner.VisitDraws(inspect, &arrival), 1u);
        ASSERT_TRUE(arrival.eligible);
        EXPECT_EQ(arrival.finer_outer_radius_meters, 1040);
        const double corner = coordinate == 0 ? 1023 : -1023;
        EXPECT_FALSE(FarWorldArrivalReplaces(arrival, 1024, {1, 0, 1}, 0, corner, corner));
        // Partial manifest immediately disables arrival, including Empty coverage.
        ASSERT_TRUE(owner.ReplaceWanted(std::span(requests).first(4)));
        ASSERT_EQ(owner.VisitDraws(inspect, &arrival), 1u);
        EXPECT_FALSE(arrival.eligible);
        owner.AdvanceFrame(0, 0, {}, {}, {});
        ASSERT_EQ(owner.VisitDraws(inspect, &arrival), 1u);
        EXPECT_FALSE(arrival.eligible); // next-frame fallback restoration
    }
}

TEST(FarVolumeResidency, FailedAndBudgetEvictedChildrenRestoreArrivalFallbackNextFrame) {
    for (const bool evict : {false, true}) {
        JobSystem jobs;
        jobs.startup(2);
        FarVolumeTierMemory memory(Budgets());
        FarVolumeResidency owner(jobs, memory, Limits());
        auto requests = std::array{Wanted({2, 0, 0}),
                                   Wanted({1, 0, 0}),
                                   Wanted({1, 1, 0}),
                                   Wanted({1, 0, 1}),
                                   Wanted({1, 1, 1})};
        for (auto& request : requests) {
            request.water =
                std::make_shared<const FarWorldConstantWaterObservation>(42, true, 0.0f);
            request.water_bound = FarWorldWaterBound{false, 0, 0};
        }
        ASSERT_TRUE(owner.ReplaceWanted(requests));
        std::size_t calls = 0;
        const auto backend = Backend(calls);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            owner.AdvanceFrame(0, 0, {5, 5, 8ULL * 1024 * 1024}, Producer(true), backend);
            if (owner.CoarseMayYield({2, 0, 0}) &&
                IsFarTileCovering(Observation(owner, requests[0].identity.key).coverage))
                break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        ASSERT_TRUE(owner.CoarseMayYield({2, 0, 0}));
        if (evict) {
            auto pressure = Wanted({1, 2, 0});
            const auto* pool = memory.Pool(1);
            ASSERT_NE(pool, nullptr);
            pressure.build_headroom_bytes = pool->limit() - pool->used() + 1;
            const std::array pressured{
                requests[0], requests[1], requests[2], requests[3], requests[4], pressure};
            ASSERT_TRUE(owner.ReplaceWanted(pressured));
            EXPECT_GE(owner.AdvanceFrame(0, 0, {1, 0, 0}, Producer(true), {}).evicted, 1u);
        } else {
            const auto* retained = owner.Resource(requests[4].identity.key);
            ASSERT_NE(retained, nullptr);
            ++requests[4].identity.content_hash;
            requests[4].water = std::make_shared<const FarWorldConstantWaterObservation>(
                requests[4].identity.content_hash, true, 0.0f);
            ASSERT_TRUE(owner.ReplaceWanted(requests));
            ASSERT_TRUE(WaitReady(owner, std::span(requests).last(1), Producer(true)));
            FarVolumeUploadBackend const failure =
                [](const auto&) -> std::unique_ptr<FarVolumeGpuAllocation> {
                return {};
            };
            const auto combined_bytes =
                Observation(owner, requests[4].identity.key).ready_upload_bytes;
            const auto failed = owner.AdvanceFrame(0, 0, {0, 1, 8ULL * 1024 * 1024}, {}, failure);
            EXPECT_EQ(failed.uploaded, 0u);
            EXPECT_EQ(failed.upload_attempts, 1u);
            EXPECT_EQ(failed.upload_bytes, combined_bytes);
            EXPECT_EQ(failed.successful_upload_bytes, 0u);
            EXPECT_EQ(owner.Resource(requests[4].identity.key), retained);
        }
        owner.AdvanceFrame(0, 0, {}, {}, {});
        EXPECT_FALSE(owner.CoarseMayYield({2, 0, 0}));
        bool parent_drawn = false;
        const auto inspect = [](void* opaque, const FarVolumeDrawRecord& record) {
            if (record.identity.key.tier == 2) {
                *static_cast<bool*>(opaque) = true;
                EXPECT_FALSE(record.arrival.eligible);
                EXPECT_GT(record.terrain_indices, 0u);
                EXPECT_EQ(record.water_indices, 98304u);
            }
        };
        EXPECT_GE(owner.VisitDraws(inspect, &parent_drawn), 1u);
        EXPECT_TRUE(parent_drawn);
    }
}

namespace {
// Blocks an actual worker scratch allocation after its admission, while the
// queue retains immutable inputs. Owner-thread staging is never blocked.
class HandoffWorkerGate final : public std::pmr::memory_resource {
public:
    void Arm() {
        armed_ = true;
    }
    void RefuseNextAllocation(bool value) {
        refuse_ = value;
    }
    bool Entered() const {
        return entered_.try_wait();
    }
    void Release() {
        if (!release_.try_wait())
            release_.count_down();
    }

private:
    std::atomic<bool> armed_{false};
    std::atomic<bool> refuse_{false};
    std::latch entered_{1}, release_{1};
    const std::thread::id owner_ = std::this_thread::get_id();
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (std::this_thread::get_id() != owner_ && armed_.exchange(false)) {
            entered_.count_down();
            release_.wait();
        }
        if (std::this_thread::get_id() != owner_ && refuse_.exchange(false))
            throw std::bad_alloc();
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
class HandoffGateRelease {
public:
    explicit HandoffGateRelease(HandoffWorkerGate& gate)
        : gate_(gate) {}
    ~HandoffGateRelease() {
        gate_.Release();
    }

private:
    HandoffWorkerGate& gate_;
};
bool HandoffWait(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        if (predicate())
            return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
FarHandoffTileInput
HandoffInput(const std::shared_ptr<FarVolumeTierMemory>& memory,
             FarTileKey key,
             std::uint64_t generation,
             bool drawable,
             float min_y = -256,
             float max_y = 64,
             bool solid = false,
             bool wide = false,
             FarFrozenCaptureError* failure = nullptr,
             bool wet = false,
             FarFrozenUse use = FarFrozenUse::Committed,
             FarCaveMode caves = FarCaveMode::BandLimited,
             FarVolumeNumericProfile numeric = FarVolumeNumericProfile::RawFloat) {
    auto wanted = Wanted(key);
    wanted.identity.cave_policy = static_cast<std::uint8_t>(caves);
    wanted.identity.bake_generation = wanted.identity.span_generation = generation;
    wanted.intent.extra_span = FarVolumeCoverageSpan{min_y, max_y};
    FarFrozenCaptureSource source;
    source.pristine = Authority(wanted, drawable);
    source.pristine.numeric = numeric;
    if (solid)
        source.pristine.density = [](const auto&, float, std::uint32_t, FarCaveMode) {
            return FarVolumeSample{-3, 7};
        };
    if (wide)
        source.pristine.density = [](const auto& p, float, std::uint32_t, FarCaveMode) {
            return FarVolumeSample{float(p.y) + .25f, 7};
        };
    source.use = use;
    source.live = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{FarFrozenPresence::Present, {-3, 9}};
    };
    source.content_identity = 100 + generation;
    source.pristine.field_identity = source.content_identity;
    source.durable = {generation, FarEditedImageState::Committed};
    source.committed = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{FarFrozenPresence::Absent, {}};
    };
    source.water = [wet](std::int64_t, std::int64_t) {
        return FarFrozenWaterSample{wet, 0};
    };
    FarVolumeRequest request;
    EXPECT_TRUE(ResolveFarVolumeCoverage(wanted.intent,
                                         source.pristine,
                                         Limits().queues[key.tier - 1].compilation.authority,
                                         request));
    auto pool = std::shared_ptr<FarVolumeMemoryPool>(memory, memory->Pool(key.tier));
    std::shared_ptr<const FarFrozenObservation> image;
    const auto error = CaptureFarVolumeObservation(
        request, source, {1, 100'000, 20'000'000, 100'000, 32ULL * 1024 * 1024}, pool, image);
    if (failure)
        *failure = error;
    else
        EXPECT_EQ(error, FarFrozenCaptureError::None);
    if (!image)
        return {};
    wanted.identity.content_hash = image->content_identity();
    const auto input = MakeFarHandoffTileInput(wanted, image);
    EXPECT_TRUE(input);
    return input.value_or(FarHandoffTileInput{});
}
FarHandoffManifest HandoffManifest(std::uint64_t serial,
                                   std::span<const FarHandoffTileInput> inputs) {
    return {1, serial, serial, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, inputs};
}
FarHandoffTileObservation HandoffObserve(const FarVolumeHandoff& owner, FarTileKey key) {
    const auto value = owner.Observe(key);
    EXPECT_TRUE(value);
    return value.value_or(FarHandoffTileObservation{});
}
void CheckRejectedHandoffManifest(FarVolumeHandoff& owner,
                                  const FarHandoffManifest& manifest,
                                  std::uint64_t used,
                                  const FarHandoffTileInput& expected) {
    EXPECT_FALSE(owner.StageTarget(manifest));
    EXPECT_EQ(owner.State().target_serial, 1u);
    EXPECT_EQ(owner.State().serving_serial, 0u);
    EXPECT_EQ(owner.Memory()[4].bytes_total, used);
    const auto retained = HandoffObserve(owner, expected.wanted.identity.key);
    ASSERT_TRUE(retained.target.has_value());
    if (retained.target.has_value()) {
        EXPECT_EQ(retained.target->identity, expected.wanted.identity);
    }
}
bool HandoffSettle(FarVolumeHandoff& owner,
                   std::span<const FarHandoffTileInput> inputs,
                   const FarVolumeUploadBackend& backend) {
    return HandoffWait([&] {
        owner.AdvanceFrame(0, 0, {6, 6, 32ULL * 1024 * 1024}, backend);
        return std::all_of(inputs.begin(), inputs.end(), [&](const auto& input) {
            return IsFarTileCovering(
                HandoffObserve(owner, input.wanted.identity.key).target_coverage);
        });
    });
}
struct HandoffDrawSnapshot {
    FarBuildIdentity identity;
    FarVolumeRequest request;
    FarVolumePosition origin_meters;
    FarWorldArrival arrival;
    std::uintptr_t resource = 0;
};
std::vector<HandoffDrawSnapshot> HandoffDraws(FarVolumeHandoff& owner) {
    std::vector<HandoffDrawSnapshot> records;
    owner.VisitServingDraws(
        [](void* p, const FarVolumeDrawRecord& record) {
            // Keep a value fingerprint of the address; no borrowed pointer escapes.
            static_cast<std::vector<HandoffDrawSnapshot>*>(p)->push_back(
                {record.identity,
                 record.request,
                 record.origin_meters,
                 record.arrival,
                 reinterpret_cast<std::uintptr_t>(record.resource)});
        },
        &records);
    return records;
}
void CheckHandoffClip(const FarWorldArrival& arrival, std::int64_t origin_y) {
    const float edge = float(FarTileEdgeMeters(5));
    for (std::size_t i = 0; i < 4; ++i) {
        const auto span = arrival.children[i];
        const float x = (float(i % 2) + .5f) * edge / 2;
        const auto row = i / 2;
        const float z = (float(row) + .5f) * edge / 2;
        const float low = float(span.min_y - origin_y), high = float(span.max_y - origin_y);
        EXPECT_TRUE(FarWorldArrivalReplaces(arrival, edge, {x, low, z}, origin_y, 100, 100));
        EXPECT_TRUE(FarWorldArrivalReplaces(arrival, edge, {x, high, z}, origin_y, 100, 100));
        EXPECT_FALSE(FarWorldArrivalReplaces(arrival, edge, {x, high + 1, z}, origin_y, 100, 100));
        EXPECT_FALSE(FarWorldArrivalReplaces(arrival, edge, {x, low - 1, z}, origin_y, 100, 100));
        const auto radius = arrival.finer_outer_radius_meters;
        EXPECT_TRUE(FarWorldArrivalReplaces(arrival, edge, {x, low, z}, origin_y, radius, 0));
        EXPECT_FALSE(FarWorldArrivalReplaces(arrival, edge, {x, low, z}, origin_y, radius + 1, 0));
    }
    // Half-open horizontal boundaries select the positive-side child.
    const auto span = arrival.children[3];
    EXPECT_TRUE(FarWorldArrivalReplaces(
        arrival, edge, {edge / 2, float(span.max_y - origin_y), edge / 2}, origin_y, 0, 0));
    EXPECT_FALSE(FarWorldArrivalReplaces(
        arrival, edge, {edge / 2 - 1, float(span.max_y - origin_y), edge / 2}, origin_y, 0, 0));
}
} // namespace

class FarVolumeHandoffArrivalTest
    : public ::testing::TestWithParam<std::tuple<std::int64_t, bool, unsigned>> {};

TEST_P(FarVolumeHandoffArrivalTest, ReplacementArrivalMatrixUsesOnlyPublishedManifest) {
    const auto [coordinate, empty_parent, mask] = GetParam();
    JobSystem jobs;
    jobs.startup(2);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    FarVolumeHandoff owner(jobs, *memory, Limits(), &gate);
    HandoffGateRelease const release{gate};
    const FarTileKey parent{5, coordinate, coordinate};
    std::vector<FarHandoffTileInput> a{HandoffInput(memory, parent, 1, true)};
    for (std::int64_t z = 0; z < 2; ++z)
        for (std::int64_t x = 0; x < 2; ++x) {
            const auto i = x + 2 * z;
            a.push_back(HandoffInput(memory,
                                     {4, coordinate * 2 + x, coordinate * 2 + z},
                                     1,
                                     false,
                                     -256,
                                     64 + float(i) * 128));
        }
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    std::size_t calls = 0;
    const auto backend = Backend(calls);
    ASSERT_TRUE(HandoffSettle(owner, a, backend));
    EXPECT_TRUE(HandoffDraws(owner).empty());
    ASSERT_TRUE(owner.TryPromote());
    ASSERT_TRUE(owner.CoarseMayYield(parent, FarHandoffCoverageView::Serving));
    const auto before = HandoffDraws(owner);
    ASSERT_EQ(before.size(), 1u);
    ASSERT_TRUE(before[0].arrival.eligible);
    CheckHandoffClip(before[0].arrival, before[0].origin_meters.y);
    std::vector<FarHandoffTileInput> b{HandoffInput(memory, parent, 2, !empty_parent)};
    for (std::size_t i = 0; i < 4; ++i) {
        b.push_back(HandoffInput(
            memory, a[i + 1].wanted.identity.key, 2, false, -256, 192 + float(i) * 128));
        if (!(mask & (1u << i)))
            b.back().wanted.build_headroom_bytes = memory->Pool(4)->limit();
    }
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    gate.Arm();
    EXPECT_EQ(owner.AdvanceFrame(0, 0, {1, 0, 0}, backend).dispatched, 1u);
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    ASSERT_TRUE(HandoffWait([&] {
        owner.AdvanceFrame(0, 0, {4, 4, 32ULL * 1024 * 1024}, backend);
        for (std::size_t i = 0; i < 4; ++i) {
            const auto value = HandoffObserve(owner, b[i + 1].wanted.identity.key);
            if (mask & (1u << i)) {
                if (!IsFarTileCovering(value.target_coverage))
                    return false;
            } else if (value.target_eligibility != FarVolumeEligibility::RequestRejected)
                return false;
        }
        return true;
    }));
    EXPECT_FALSE(owner.TryPromote());
    EXPECT_EQ(owner.State().serving_serial, 1u);
    EXPECT_EQ(owner.CoarseMayYield(parent, FarHandoffCoverageView::Target), mask == 15);
    const auto during = HandoffDraws(owner);
    ASSERT_EQ(during.size(), 1u);
    EXPECT_EQ(during[0].identity, before[0].identity);
    EXPECT_EQ(during[0].resource, before[0].resource);
    EXPECT_EQ(during[0].request, before[0].request);
    EXPECT_TRUE(during[0].arrival.eligible);
    EXPECT_EQ(during[0].arrival.children, before[0].arrival.children);
    CheckHandoffClip(during[0].arrival, during[0].origin_meters.y);
    gate.Release();
    ASSERT_TRUE(HandoffWait([&] {
        owner.AdvanceFrame(0, 0, {0, 1, 32ULL * 1024 * 1024}, backend);
        return IsFarTileCovering(HandoffObserve(owner, parent).target_coverage);
    }));
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(owner.State().serving_serial, 2u);
    EXPECT_EQ(owner.CoarseMayYield(parent, FarHandoffCoverageView::Serving), mask == 15);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto value = HandoffObserve(owner, b[i + 1].wanted.identity.key);
        EXPECT_EQ(IsFarTileCovering(value.serving_coverage), bool(mask & (1u << i)));
        if (value.serving) {
            EXPECT_EQ(value.serving->identity, b[i + 1].wanted.identity);
        }
    }
    const auto after = HandoffDraws(owner);
    ASSERT_EQ(after.size(), empty_parent ? 0u : 1u);
    const auto parent_value = HandoffObserve(owner, parent);
    ASSERT_TRUE(parent_value.serving);
    if (!parent_value.serving)
        return;
    EXPECT_EQ(parent_value.serving->identity, b[0].wanted.identity);
    EXPECT_TRUE(IsFarTileCovering(parent_value.serving_coverage));
    if (empty_parent) {
        EXPECT_EQ(parent_value.serving_coverage.state, FarTileState::Empty);
        EXPECT_EQ(parent_value.serving_gpu_bytes, 0u);
    } else {
        EXPECT_EQ(after[0].identity, b[0].wanted.identity);
        EXPECT_EQ(after[0].arrival.eligible, mask == 15);
        if (mask == 15) {
            for (std::size_t i = 0; i < 4; ++i)
                EXPECT_EQ(after[0].arrival.children[i],
                          HandoffObserve(owner, b[i + 1].wanted.identity.key).serving_built.span);
            EXPECT_NE(after[0].arrival.children, before[0].arrival.children);
            CheckHandoffClip(after[0].arrival, after[0].origin_meters.y);
        }
    }
    if (mask == 15) {
        FarWorldArrival arrival{true, FarWantedRadiusMeters(4), {}};
        for (std::size_t i = 0; i < 4; ++i)
            arrival.children[i] =
                HandoffObserve(owner, b[i + 1].wanted.identity.key).serving_built.span;
        CheckHandoffClip(arrival, parent_value.serving_built.span.min_y);
    }
}

INSTANTIATE_TEST_SUITE_P(AllChildMasks,
                         FarVolumeHandoffArrivalTest,
                         ::testing::Combine(::testing::Values(std::int64_t{-1}, std::int64_t{1}),
                                            ::testing::Bool(),
                                            ::testing::Range(0u, 16u)));

TEST(FarVolumeHandoff, PreviewUseWithCommittedIdentityIsRefusedTransactionally) {
    // Refuse previews before initial staging, while staging and after publication.
    for (unsigned phase = 0; phase < 3; ++phase) {
        SCOPED_TRACE(phase);
        JobSystem jobs;
        jobs.startup(1);
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        FarVolumeHandoff owner(jobs, *memory, Limits());
        const FarTileKey key{5, 0, 0};
        const std::array committed{HandoffInput(memory, key, 1, false)};
        ASSERT_TRUE(committed[0].observation);
        EXPECT_EQ(committed[0].observation->use(), FarFrozenUse::Committed);
        std::size_t calls = 0;
        if (phase > 0) {
            ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, committed)));
        }
        if (phase == 2) {
            ASSERT_TRUE(HandoffSettle(owner, committed, Backend(calls)));
            ASSERT_TRUE(owner.TryPromote());
        }
        const std::array preview{HandoffInput(
            memory, key, 2, false, -256, 64, false, false, nullptr, false, FarFrozenUse::Preview)};
        ASSERT_TRUE(preview[0].observation);
        const auto& image = *preview[0].observation;
        ASSERT_EQ(image.use(), FarFrozenUse::Preview);
        ASSERT_EQ(image.durable_identity().state, FarEditedImageState::Committed);
        ASSERT_EQ(image.durable_identity().generation, 2u);
        const auto edited = image.Sample(key.tier, {0, 0, 0});
        ASSERT_TRUE(edited.has_value());
        if (edited.has_value()) {
            EXPECT_EQ(edited->density, -3);
            EXPECT_EQ(edited->material, 9);
        }
        const auto before = owner.Memory();
        EXPECT_FALSE(owner.StageTarget(HandoffManifest(2, preview)));
        EXPECT_EQ(owner.State().target_serial, phase > 0 ? 1u : 0u);
        EXPECT_EQ(owner.State().serving_serial, phase == 2 ? 1u : 0u);
        const auto after = owner.Memory();
        for (std::size_t i = 0; i < before.size(); ++i) {
            EXPECT_EQ(after[i].bytes_cpu, before[i].bytes_cpu);
            EXPECT_EQ(after[i].bytes_gpu, before[i].bytes_gpu);
        }
        const auto retained = owner.Observe(key);
        if (phase == 0) {
            EXPECT_FALSE(retained.has_value());
        } else {
            ASSERT_TRUE(retained.has_value());
            if (retained.has_value()) {
                ASSERT_TRUE(retained->target.has_value());
                if (retained->target.has_value()) {
                    EXPECT_EQ(retained->target->identity, committed[0].wanted.identity);
                }
            }
        }
        // The same durable generation captured for committed use is admissible.
        const std::array next{HandoffInput(memory, key, 2, false)};
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, next)));
        ASSERT_TRUE(HandoffSettle(owner, next, Backend(calls)));
        ASSERT_TRUE(owner.TryPromote());
        EXPECT_EQ(owner.State().serving_serial, 2u);
        EXPECT_EQ(calls, 0u);
    }
}

TEST(FarVolumeHandoff, NewPublicationCannotRegressCommittedGenerationWithinEpoch) {
    for (unsigned phase = 0; phase < 3; ++phase) {
        SCOPED_TRACE(phase);
        JobSystem jobs;
        jobs.startup(1);
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        FarVolumeHandoff owner(jobs, *memory, Limits());
        const FarTileKey key{5, -1, 0};
        const std::array current{HandoffInput(memory, key, 2, false)};
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, current)));
        std::size_t calls = 0;
        if (phase > 0) {
            ASSERT_TRUE(HandoffSettle(owner, current, Backend(calls)));
            ASSERT_TRUE(owner.TryPromote());
        }
        const std::array newer{HandoffInput(memory, key, 3, false)};
        if (phase == 2) {
            ASSERT_TRUE(owner.StageTarget(HandoffManifest(3, newer)));
        }
        // Also refuse a generation equal to serving but older than staged.
        for (std::uint64_t generation = 0; generation < (phase == 2 ? 3u : 2u); ++generation) {
            const std::array stale{HandoffInput(memory, key, generation, false)};
            const auto before = owner.Memory();
            const auto state = owner.State();
            auto manifest = HandoffManifest(10, stale);
            manifest.committed_generation = generation;
            EXPECT_FALSE(owner.StageTarget(manifest));
            EXPECT_EQ(owner.State().target_serial, state.target_serial);
            EXPECT_EQ(owner.State().serving_serial, state.serving_serial);
            EXPECT_EQ(owner.State().committed_generation, state.committed_generation);
            EXPECT_EQ(owner.State().serving_committed_generation,
                      state.serving_committed_generation);
            const auto retained = HandoffObserve(owner, key);
            ASSERT_TRUE(retained.target);
            if (retained.target) {
                EXPECT_EQ(retained.target->identity,
                          phase == 2 ? newer[0].wanted.identity : current[0].wanted.identity);
            }
            const auto after = owner.Memory();
            for (std::size_t i = 0; i < before.size(); ++i) {
                EXPECT_EQ(after[i].bytes_cpu, before[i].bytes_cpu);
                EXPECT_EQ(after[i].bytes_gpu, before[i].bytes_gpu);
            }
        }
        // A newer serial at the current generation remains admissible.
        auto same_generation = HandoffManifest(11, phase == 2 ? newer : current);
        same_generation.committed_generation = phase == 2 ? 3 : 2;
        ASSERT_TRUE(owner.StageTarget(same_generation));
        EXPECT_EQ(owner.State().target_serial, 11u);
        // Explicit drain permits a new epoch to start at the legacy base.
        owner.CancelAndDrain();
        auto base = std::array{HandoffInput(memory, key, 0, false)};
        base[0].wanted.identity.world_epoch = 2;
        auto next_world = HandoffManifest(1, base);
        next_world.world_epoch = 2;
        next_world.committed_generation = 0;
        ASSERT_TRUE(owner.StageTarget(next_world));
        EXPECT_EQ(owner.State().world_epoch, 2u);
        EXPECT_EQ(owner.State().committed_generation, 0u);
    }
}

TEST(FarVolumeHandoff, IndexBoundsAndAdmissionRefusalsReleaseAllControlCharges) {
    JobSystem jobs;
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    for (unsigned failure = 0; failure < 4; ++failure) {
        auto limits = Limits();
        if (failure == 0)
            limits.entry_slots[0] = 0;
        else if (failure == 1)
            limits.entry_slots[0] = limits.queues[0].descriptor_slots + 1;
        else if (failure == 2)
            limits.queues[0].result_slots = 0;
        else {
            limits.entry_slots[0] = std::numeric_limits<std::size_t>::max();
            limits.queues[0].descriptor_slots = limits.entry_slots[0];
        }
        if (failure < 3) {
            EXPECT_THROW(FarVolumeHandoff(jobs, *memory, limits), std::invalid_argument);
        } else {
            EXPECT_THROW(FarVolumeHandoff(jobs, *memory, limits), FarVolumeMemoryRefused);
        }
        for (std::size_t tier = 1; tier <= kFarTierCount; ++tier)
            EXPECT_EQ(memory->Pool(tier)->used(), 0u);
    }
    auto tiny = Budgets();
    tiny.bytes[0] = 1;
    auto refused = std::make_shared<FarVolumeTierMemory>(tiny);
    EXPECT_THROW(FarVolumeHandoff(jobs, *refused, Limits()), FarVolumeMemoryRefused);
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier)
        EXPECT_EQ(refused->Pool(tier)->used(), 0u);
}

TEST(FarVolumeHandoff, ObservationVisitsMatchIndexedQueriesAcrossRemovalAndDrain) {
    JobSystem jobs;
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    std::vector<FarHandoffTileInput> inputs;
    // Fill every retained slot, with negative keys and several hash collisions.
    for (std::int64_t coordinate = -3; coordinate < 3; ++coordinate)
        inputs.push_back(HandoffInput(memory, {5, coordinate, coordinate}, 1, false));
    const auto check = [](FarVolumeHandoff& handoff, std::size_t expected) {
        struct Context {
            FarVolumeHandoff& owner;
            std::size_t count = 0;
        } context{handoff};
        handoff.VisitObservations(
            [](void* opaque, const FarTileKey& key, const FarHandoffTileObservation& value) {
                auto& c = *static_cast<Context*>(opaque);
                const auto queried = c.owner.Observe(key);
                ASSERT_TRUE(queried);
                if (!queried)
                    return;
                ASSERT_TRUE(value.target);
                ASSERT_TRUE(queried->target);
                if (value.target && queried->target) {
                    EXPECT_EQ(queried->target->identity, value.target->identity);
                    EXPECT_EQ(queried->target->intent, value.target->intent);
                }
                EXPECT_EQ(queried->target_eligibility, value.target_eligibility);
                EXPECT_EQ(queried->target_source_payload_bytes, value.target_source_payload_bytes);
                EXPECT_FALSE(c.owner.CoarseMayYield(key, FarHandoffCoverageView::Target));
                ++c.count;
            },
            &context);
        EXPECT_EQ(context.count, expected);
    };
    check(owner, 0);
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, inputs)));
    check(owner, inputs.size());
    EXPECT_FALSE(owner.Observe({0, 0, 0}));
    EXPECT_FALSE(owner.Observe({6, 0, 0}));
    EXPECT_FALSE(owner.Observe({5, 500, -500}));
    EXPECT_EQ(owner.VisitServingDraws(nullptr, nullptr), 0u);
    owner.VisitObservations(nullptr, nullptr);
    auto subset = HandoffManifest(2, std::span(inputs).last(1));
    subset.committed_generation = 1;
    ASSERT_TRUE(owner.StageTarget(subset));
    check(owner, 1);
    EXPECT_FALSE(owner.Observe(inputs[0].wanted.identity.key));
    owner.CancelAndDrain();
    check(owner, 0);
    for (const auto& input : inputs)
        EXPECT_FALSE(owner.Observe(input.wanted.identity.key));
    // Rebind fills the same index with a different insertion order.
    std::reverse(inputs.begin(), inputs.end());
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, inputs)));
    check(owner, inputs.size());
}

TEST(FarVolumeHandoff, FailedUploadDebitsOnceAndRetainsServingCoverage) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    const FarTileKey key{5, 0, 0};
    const std::array a{HandoffInput(memory, key, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    const auto old = HandoffDraws(owner)[0];
    const auto gpu = owner.Memory()[4].bytes_gpu;
    const std::array b{HandoffInput(memory, key, 2, true)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    ASSERT_TRUE(HandoffWait([&] {
        owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
        return HandoffObserve(owner, key).target_eligibility == FarVolumeEligibility::Ready;
    }));
    const auto bytes = HandoffObserve(owner, key).ready_upload_bytes;
    ASSERT_GT(bytes, 0u);
    const auto used = memory->Pool(5)->used();
    const auto ready_cpu = HandoffObserve(owner, key).ready_cpu_bytes;
    std::size_t failed_calls = 0;
    const FarVolumeUploadBackend fail =
        [&](const auto&) -> std::unique_ptr<FarVolumeGpuAllocation> {
        ++failed_calls;
        EXPECT_EQ(owner.Memory()[4].bytes_gpu, gpu + bytes);
        EXPECT_EQ(memory->Pool(5)->used(), used + bytes);
        return {};
    };
    const auto first = owner.AdvanceFrame(0, 0, {1, 1, bytes}, fail);
    EXPECT_EQ(first.upload_attempts, 1u);
    EXPECT_EQ(first.upload_bytes, bytes);
    EXPECT_EQ(first.successful_upload_bytes, 0u);
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, gpu);
    EXPECT_EQ(memory->Pool(5)->used(), used - ready_cpu);
    EXPECT_EQ(HandoffObserve(owner, key).target_upload_failure,
              FarVolumeUploadError::BackendFailure);
    EXPECT_FALSE(owner.TryPromote());
    const auto second = owner.AdvanceFrame(0, 0, {1, 1, bytes}, fail);
    EXPECT_EQ(second.upload_attempts, 0u);
    EXPECT_EQ(second.upload_bytes, 0u);
    EXPECT_EQ(failed_calls, 1u);
    const auto draws = HandoffDraws(owner);
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].identity, old.identity);
    EXPECT_EQ(draws[0].resource, old.resource);
    EXPECT_EQ(draws[0].request, old.request);
    EXPECT_TRUE(IsFarTileCovering(HandoffObserve(owner, key).serving_coverage));
    EXPECT_FALSE(IsFarTileCovering(HandoffObserve(owner, key).target_coverage));
}

TEST(FarVolumeHandoff, HeldSupersededWorkerCannotIntegrateAndRetainsInputCharge) {
    JobSystem jobs;
    jobs.startup(2);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    FarVolumeHandoff owner(jobs, *memory, Limits(), &gate);
    HandoffGateRelease const release{gate};
    const FarTileKey key{5, 0, 0};
    const std::array a{HandoffInput(memory, key, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    auto b = std::array{HandoffInput(memory, key, 2, true)};
    std::weak_ptr<const FarFrozenObservation> const held = b[0].observation;
    const auto held_payload = b[0].observation->payload_bytes();
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    gate.Arm();
    owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    const std::array c{HandoffInput(memory, key, 3, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(3, c)));
    b = {};
    EXPECT_FALSE(held.expired());
    const auto value = HandoffObserve(owner, key);
    const auto held_used = memory->Pool(5)->used();
    EXPECT_GE(held_used,
              held_payload + value.serving_source_payload_bytes +
                  value.target_source_payload_bytes + value.serving_gpu_bytes);
    EXPECT_FALSE(owner.TryPromote());
    const auto calls_before = calls;
    gate.Release();
    ASSERT_TRUE(HandoffSettle(owner, c, Backend(calls)));
    EXPECT_TRUE(held.expired());
    EXPECT_LE(memory->Pool(5)->used(), held_used - held_payload);
    EXPECT_EQ(calls, calls_before); // C is Empty; B never enters the backend.
    ASSERT_TRUE(owner.TryPromote());
    const auto promoted = HandoffObserve(owner, key);
    ASSERT_TRUE(promoted.serving);
    if (!promoted.serving)
        return;
    EXPECT_EQ(promoted.serving->identity, c[0].wanted.identity);
    EXPECT_TRUE(HandoffDraws(owner).empty());
}

TEST(FarVolumeHandoff, EveryWantedV5PrerequisitePromotesAsOneCohort) {
    JobSystem jobs;
    jobs.startup(2);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    FarVolumeHandoff owner(jobs, *memory, Limits(), &gate);
    HandoffGateRelease const release{gate};
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true),
                       HandoffInput(memory, {5, 1, 0}, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    const std::array b{HandoffInput(memory, {5, 0, 0}, 2, false),
                       HandoffInput(memory, {5, 2, 0}, 2, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    gate.Arm();
    owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    ASSERT_TRUE(HandoffSettle(owner, std::span(b).last(1), Backend(calls)));
    EXPECT_FALSE(owner.TryPromote());
    EXPECT_EQ(owner.State().serving_serial, 1u);
    EXPECT_EQ(HandoffDraws(owner).size(), 2u);
    gate.Release();
    ASSERT_TRUE(HandoffSettle(owner, b, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(owner.State().serving_serial, 2u);
    EXPECT_FALSE(owner.Observe(a[1].wanted.identity.key));
    EXPECT_TRUE(HandoffDraws(owner).empty());
}

TEST(FarVolumeHandoff, DeferredUploadReservesResultSlotAndPreservesFallbackPromotion) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    auto limits = Limits();
    limits.queues[4].result_slots = 1;
    FarVolumeHandoff owner(jobs, *memory, limits);
    std::array inputs{HandoffInput(memory, {5, 0, 0}, 1, true),
                      HandoffInput(memory, {5, 1, 0}, 1, true)};
    auto* pool = memory->Pool(5);
    inputs[1].wanted.build_headroom_bytes = pool->limit() / 2;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, inputs)));
    ASSERT_EQ(owner.AdvanceFrame(0, 0, {1, 0, 0}, {}).dispatched, 1u);
    ASSERT_TRUE(HandoffWait([&] {
        owner.AdvanceFrame(0, 0, {}, {});
        return HandoffObserve(owner, inputs[0].wanted.identity.key).target_eligibility ==
               FarVolumeEligibility::Ready;
    }));
    const auto ready = HandoffObserve(owner, inputs[0].wanted.identity.key);
    ASSERT_GT(ready.ready_upload_bytes, 0u);
    const auto deferred_without_pressure = owner.AdvanceFrame(0, 0, {2, 0, 0}, {});
    ASSERT_EQ(deferred_without_pressure.dispatched, 0u);
    EXPECT_EQ(deferred_without_pressure.build_attempts, 0u);
    EXPECT_EQ(deferred_without_pressure.rejected, 0u);
    ASSERT_LE(ready.ready_upload_bytes, pool->limit() - pool->used());
    FarVolumeMemoryLease pressure(*pool);
    ASSERT_TRUE(pressure.TryCharge(pool->limit() - pool->used() - ready.ready_upload_bytes));
    for (int frame = 0; frame < 3; ++frame) {
        const auto deferred = owner.AdvanceFrame(0, 0, {2, 0, 0}, {});
        EXPECT_EQ(deferred.dispatched, 0u);
        EXPECT_EQ(deferred.build_attempts, 0u);
        EXPECT_EQ(deferred.rejected, 0u);
        EXPECT_FALSE(deferred.horizon_refused);
        EXPECT_EQ(HandoffObserve(owner, inputs[0].wanted.identity.key).ready_cpu_bytes,
                  ready.ready_cpu_bytes);
        const auto waiting = HandoffObserve(owner, inputs[1].wanted.identity.key);
        EXPECT_EQ(waiting.target_eligibility, FarVolumeEligibility::Wanted);
        EXPECT_FALSE(waiting.target_build_failure);
        EXPECT_FALSE(waiting.target_upload_failure);
        EXPECT_FALSE(owner.TryPromote());
    }
    std::size_t calls = 0;
    const auto backend = Backend(calls);
    EXPECT_EQ(owner.AdvanceFrame(0, 0, {0, 1, ready.ready_upload_bytes}, backend).uploaded, 1u);
    pressure.Reset();
    EXPECT_EQ(owner.AdvanceFrame(0, 0, {1, 0, 0}, {}).dispatched, 1u);
    ASSERT_TRUE(HandoffSettle(owner, inputs, backend));
    EXPECT_EQ(calls, 2u);
    EXPECT_TRUE(owner.TryPromote());
    EXPECT_EQ(owner.State().serving_serial, 1u);
    EXPECT_FALSE(owner.State().awaiting_initial_fallback);
}

TEST(FarVolumeHandoff, DiscardedDeferredUploadReleasesResultSlotForNextFallback) {
    for (const bool byte_refusal : {false, true}) {
        SCOPED_TRACE(byte_refusal);
        JobSystem jobs;
        jobs.startup(1);
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        auto limits = Limits();
        limits.queues[4].result_slots = 1;
        FarVolumeHandoff owner(jobs, *memory, limits);
        const std::array inputs{HandoffInput(memory, {5, 0, 0}, 1, true),
                                HandoffInput(memory, {5, 1, 0}, 1, false)};
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, inputs)));
        ASSERT_EQ(owner.AdvanceFrame(0, 0, {1, 0, 0}, {}).dispatched, 1u);
        ASSERT_TRUE(HandoffWait([&] {
            owner.AdvanceFrame(0, 0, {}, {});
            return HandoffObserve(owner, inputs[0].wanted.identity.key).target_eligibility ==
                   FarVolumeEligibility::Ready;
        }));
        const auto bytes = HandoffObserve(owner, inputs[0].wanted.identity.key).ready_upload_bytes;
        ASSERT_GT(bytes, 0u);
        const auto rejected =
            owner.AdvanceFrame(0, 0, {1, 1, byte_refusal ? bytes - 1 : bytes}, {});
        EXPECT_EQ(rejected.rejected, 1u);
        EXPECT_EQ(rejected.dispatched, 1u);
        const auto failed = HandoffObserve(owner, inputs[0].wanted.identity.key);
        EXPECT_EQ(failed.ready_cpu_bytes, 0u);
        EXPECT_EQ(failed.target_upload_failure,
                  byte_refusal ? FarVolumeUploadError::ByteLimit
                               : FarVolumeUploadError::BackendFailure);
        std::size_t calls = 0;
        ASSERT_TRUE(HandoffSettle(owner, std::span(inputs).last(1), Backend(calls)));
        EXPECT_EQ(calls, 0u);
        EXPECT_FALSE(owner.TryPromote());
        owner.CancelAndDrain();
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, std::span(inputs).last(1))));
        ASSERT_TRUE(HandoffSettle(owner, std::span(inputs).last(1), Backend(calls)));
        EXPECT_TRUE(owner.TryPromote());
    }
}

TEST(FarVolumeHandoff, ValidAirAndSolidEmptyCoverWithoutBackendAllocation) {
    for (const bool solid : {false, true}) {
        JobSystem jobs;
        jobs.startup(1);
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        FarVolumeHandoff owner(jobs, *memory, Limits());
        const std::array input{HandoffInput(memory, {5, 0, 0}, 0, false, -256, 64, solid)};
        auto manifest = HandoffManifest(1, input);
        manifest.committed_generation = 0; // Explicit legacy committed base.
        ASSERT_TRUE(owner.StageTarget(manifest));
        std::size_t calls = 0;
        ASSERT_TRUE(HandoffSettle(owner, input, Backend(calls)));
        ASSERT_TRUE(owner.TryPromote());
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(owner.Memory()[4].bytes_gpu, 0u);
        EXPECT_TRUE(HandoffDraws(owner).empty());
        EXPECT_EQ(HandoffObserve(owner, {5, 0, 0}).serving_coverage.state, FarTileState::Empty);
    }
}

TEST(FarVolumeHandoff, OverlapHeadroomRefusalRetainsOldGenerationWithoutRaisingLimit) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    auto b = std::array{HandoffInput(memory, {5, 0, 0}, 2, true)};
    const auto old_gpu = owner.Memory()[4].bytes_gpu;
    const auto current = memory->Pool(5)->used();
    // This prospective peak fits after retiring A, but cannot fit while A serves.
    b[0].wanted.build_headroom_bytes = memory->Pool(5)->limit() - current + old_gpu;
    ASSERT_GT(old_gpu, 0u);
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    const auto frame = owner.AdvanceFrame(0, 0, {1, 1, 32ULL * 1024 * 1024}, Backend(calls));
    EXPECT_TRUE(frame.horizon_refused);
    EXPECT_EQ(frame.dispatched, 0u);
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, old_gpu);
    EXPECT_EQ(memory->Pool(5)->limit(), Budgets().bytes[4]);
    EXPECT_TRUE(IsFarTileCovering(HandoffObserve(owner, {5, 0, 0}).serving_coverage));
    EXPECT_FALSE(owner.TryPromote());
}

TEST(FarVolumeHandoff, SuccessfulReplacementAccountsOldPlusNewGpuAtPeak) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    const auto old_bytes = owner.Memory()[4].bytes_gpu;
    const std::array b{HandoffInput(memory, {5, 0, 0}, 2, true)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    const FarVolumeUploadBackend backend = [&](const auto& mesh) {
        const auto bytes = mesh.vertices.size() * sizeof(VoxelVertex) +
                           mesh.indices.size() * sizeof(std::uint32_t);
        EXPECT_EQ(owner.Memory()[4].bytes_gpu, old_bytes + bytes);
        return std::make_unique<Gpu>(bytes);
    };
    ASSERT_TRUE(HandoffSettle(owner, b, backend));
    const auto value = HandoffObserve(owner, {5, 0, 0});
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, value.serving_gpu_bytes + value.staged_gpu_bytes);
    EXPECT_GE(owner.Memory()[4].peak_total, owner.Memory()[4].bytes_total);
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, value.staged_gpu_bytes);
    EXPECT_EQ(HandoffObserve(owner, {5, 0, 0}).staged_gpu_bytes, 0u);
}

TEST(FarVolumeHandoff, InvalidManifestAndDuplicatePublicationAreTransactional) {
    JobSystem jobs;
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, false)};
    const auto manifest = HandoffManifest(1, a);
    ASSERT_TRUE(owner.StageTarget(manifest));
    const auto used = memory->Pool(5)->used();
    ASSERT_TRUE(owner.StageTarget(manifest));
    auto bad_manifest = manifest;
    bad_manifest.world_epoch = 2;
    CheckRejectedHandoffManifest(owner, bad_manifest, used, a[0]);
    bad_manifest = manifest;
    bad_manifest.committed_generation = 9;
    CheckRejectedHandoffManifest(owner, bad_manifest, used, a[0]);
    bad_manifest = manifest;
    bad_manifest.publication_serial = 0;
    CheckRejectedHandoffManifest(owner, bad_manifest, used, a[0]);
    bad_manifest = manifest;
    bad_manifest.caves = FarCaveMode::BoxFiltered;
    CheckRejectedHandoffManifest(owner, bad_manifest, used, a[0]);
    auto bad = a;
    bad[0].wanted.identity.content_hash ^= 1;
    CheckRejectedHandoffManifest(owner, HandoffManifest(1, bad), used, a[0]);
    bad = a;
    bad[0].observation.reset();
    CheckRejectedHandoffManifest(owner, HandoffManifest(1, bad), used, a[0]);
    bad = a;
    bad[0].wanted.build_headroom_bytes = 1;
    CheckRejectedHandoffManifest(owner, HandoffManifest(1, bad), used, a[0]);
    bad = a;
    auto& extra_span = bad[0].wanted.intent.extra_span;
    ASSERT_TRUE(extra_span.has_value());
    if (!extra_span.has_value())
        return;
    extra_span->max_y -= 1;
    CheckRejectedHandoffManifest(owner, HandoffManifest(1, bad), used, a[0]);
    bad = a;
    bad[0].wanted.water = std::make_shared<const FarWorldConstantWaterObservation>(
        bad[0].wanted.identity.content_hash, false, 0);
    CheckRejectedHandoffManifest(owner, HandoffManifest(1, bad), used, a[0]);
    bad = a;
    auto& water_bound = bad[0].wanted.water_bound;
    ASSERT_TRUE(water_bound.has_value());
    if (!water_bound.has_value())
        return;
    water_bound->max_y = 1;
    CheckRejectedHandoffManifest(owner, HandoffManifest(1, bad), used, a[0]);
}

TEST(FarVolumeHandoff, DuplicateKeysWithFreshSerialAreTransactional) {
    JobSystem jobs;
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    const std::array duplicates{a[0], a[0]};
    auto manifest = HandoffManifest(2, duplicates);
    manifest.committed_generation = 1;
    const auto before = owner.Memory();
    EXPECT_FALSE(owner.StageTarget(manifest));
    EXPECT_EQ(owner.State().target_serial, 1u);
    EXPECT_EQ(owner.State().serving_serial, 0u);
    EXPECT_EQ(owner.State().committed_generation, 1u);
    const auto retained = HandoffObserve(owner, {5, 0, 0});
    ASSERT_TRUE(retained.target);
    if (!retained.target)
        return;
    EXPECT_EQ(retained.target->identity, a[0].wanted.identity);
    EXPECT_EQ(retained.target->intent, a[0].wanted.intent);
    EXPECT_FALSE(retained.serving);
    const auto after = owner.Memory();
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after[i].bytes_cpu, before[i].bytes_cpu);
        EXPECT_EQ(after[i].bytes_gpu, before[i].bytes_gpu);
        EXPECT_EQ(after[i].bytes_total, before[i].bytes_total);
    }
    manifest.tiles = a;
    ASSERT_TRUE(owner.StageTarget(manifest));
    EXPECT_EQ(owner.State().target_serial, 2u);
}

TEST(FarVolumeHandoff, DistinctValidKeysExceedPerTierManifestCapacityTransactionally) {
    JobSystem jobs;
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    auto limits = Limits();
    limits.entry_slots.fill(1);
    FarVolumeHandoff owner(jobs, *memory, limits);
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    const std::array excess{a[0], HandoffInput(memory, {5, 1, 0}, 1, false)};
    auto manifest = HandoffManifest(2, excess);
    manifest.committed_generation = 1;
    {
        FarVolumeHandoff probe(jobs, *memory, Limits());
        ASSERT_TRUE(probe.StageTarget(manifest));
    }
    const auto before = owner.Memory();
    bool accepted = false;
    EXPECT_NO_THROW(accepted = owner.StageTarget(manifest));
    EXPECT_FALSE(accepted);
    EXPECT_EQ(owner.State().target_serial, 1u);
    EXPECT_EQ(owner.State().serving_serial, 0u);
    EXPECT_EQ(owner.State().committed_generation, 1u);
    const auto retained = HandoffObserve(owner, {5, 0, 0});
    ASSERT_TRUE(retained.target);
    if (!retained.target)
        return;
    EXPECT_EQ(retained.target->identity, a[0].wanted.identity);
    EXPECT_EQ(retained.target->intent, a[0].wanted.intent);
    EXPECT_FALSE(retained.serving);
    EXPECT_FALSE(owner.Observe({5, 1, 0}));
    const auto after = owner.Memory();
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after[i].bytes_cpu, before[i].bytes_cpu);
        EXPECT_EQ(after[i].bytes_gpu, before[i].bytes_gpu);
        EXPECT_EQ(after[i].bytes_total, before[i].bytes_total);
    }
}

TEST(FarVolumeHandoff, ServingTargetUnionExhaustionIsTransactional) {
    for (const bool retire_old_serving : {false, true}) {
        SCOPED_TRACE(retire_old_serving);
        JobSystem jobs;
        jobs.startup(1);
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        auto limits = Limits();
        limits.entry_slots.fill(2);
        FarVolumeHandoff owner(jobs, *memory, limits);
        const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true)};
        const std::array b{HandoffInput(memory, {5, 1, 0}, 2, true)};
        const std::array c{HandoffInput(memory, {5, 2, 0}, 3, false),
                           HandoffInput(memory, {5, 3, 0}, 3, false)};
        {
            FarVolumeHandoff probe(jobs, *memory, limits);
            ASSERT_TRUE(probe.StageTarget(HandoffManifest(3, c)));
        }
        std::size_t calls = 0;
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
        ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
        ASSERT_TRUE(owner.TryPromote());
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
        if (retire_old_serving) {
            ASSERT_TRUE(HandoffSettle(owner, b, Backend(calls)));
            ASSERT_TRUE(owner.TryPromote());
            ASSERT_FALSE(owner.Observe({5, 0, 0}));
        } else {
            EXPECT_TRUE(IsFarTileCovering(HandoffObserve(owner, {5, 0, 0}).serving_coverage));
        }
        const auto before = owner.Memory();
        const auto draws = HandoffDraws(owner);
        const auto serving_serial = owner.State().serving_serial;
        bool accepted = false;
        EXPECT_NO_THROW(accepted = owner.StageTarget(HandoffManifest(3, c)));
        EXPECT_FALSE(accepted);
        EXPECT_EQ(owner.State().target_serial, 2u);
        EXPECT_EQ(owner.State().serving_serial, serving_serial);
        EXPECT_EQ(owner.State().committed_generation, 2u);
        EXPECT_FALSE(owner.Observe({5, 2, 0}));
        EXPECT_FALSE(owner.Observe({5, 3, 0}));
        const auto retained = HandoffObserve(owner, {5, 1, 0});
        ASSERT_TRUE(retained.target);
        if (!retained.target)
            return;
        EXPECT_EQ(retained.target->identity, b[0].wanted.identity);
        EXPECT_EQ(retained.target->intent, b[0].wanted.intent);
        const auto after_draws = HandoffDraws(owner);
        ASSERT_EQ(after_draws.size(), draws.size());
        for (std::size_t i = 0; i < draws.size(); ++i) {
            EXPECT_EQ(after_draws[i].identity, draws[i].identity);
            EXPECT_EQ(after_draws[i].resource, draws[i].resource);
            EXPECT_EQ(after_draws[i].request, draws[i].request);
            EXPECT_EQ(after_draws[i].arrival.children, draws[i].arrival.children);
        }
        const auto after = owner.Memory();
        for (std::size_t i = 0; i < before.size(); ++i) {
            EXPECT_EQ(after[i].bytes_cpu, before[i].bytes_cpu);
            EXPECT_EQ(after[i].bytes_gpu, before[i].bytes_gpu);
            EXPECT_EQ(after[i].bytes_total, before[i].bytes_total);
        }
    }
}

class FarVolumeHandoffTraversal
    : public ::testing::TestWithParam<
          std::tuple<std::uint8_t, bool, std::size_t, FarCaveMode, FarVolumeNumericProfile>> {};
TEST_P(FarVolumeHandoffTraversal, ReusesRetiredSlotsBeyondCapacityWithoutResettingPublication) {
    const auto [tier, publish, capacity, caves, numeric] = GetParam();
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    auto limits = Limits();
    limits.entry_slots.fill(capacity);
    FarVolumeHandoff owner(jobs, *memory, limits);
    std::size_t calls = 0;
    const auto input = [&](FarTileKey key) {
        return HandoffInput(memory,
                            key,
                            1,
                            false,
                            -256,
                            64,
                            false,
                            false,
                            nullptr,
                            false,
                            FarFrozenUse::Committed,
                            caves,
                            numeric);
    };
    const auto fallback = input({5, 0, 0});
    ASSERT_TRUE(fallback.observation);
    // Each tier traverses four times its descriptor capacity, including negative
    // keys and repeated reuse of hash buckets. Captures stay at one generation.
    for (std::uint64_t serial = 1; serial <= 4 * capacity; ++serial) {
        SCOPED_TRACE(serial);
        const FarTileKey key{tier, static_cast<std::int64_t>(serial) - 4, 0};
        const auto current = input(key);
        ASSERT_TRUE(current.observation);
        const std::array inputs{current, fallback};
        auto manifest = HandoffManifest(serial, std::span(inputs).first(tier == 5 ? 1 : 2));
        manifest.committed_generation = 1;
        manifest.caves = caves;
        manifest.numeric = numeric;
        const auto previous = owner.State();
        if (serial > 1) {
            // Refusals when reclamation is needed must not retire old entries.
            auto missing = inputs;
            missing[0].observation.reset();
            auto invalid = manifest;
            invalid.tiles = std::span(missing).first(manifest.tiles.size());
            EXPECT_FALSE(owner.StageTarget(invalid));
            invalid.tiles = {};
            EXPECT_FALSE(owner.StageTarget(invalid));
            invalid = manifest;
            invalid.publication_serial = serial - 2;
            EXPECT_FALSE(owner.StageTarget(invalid));
            invalid = manifest;
            invalid.committed_generation = 0;
            EXPECT_FALSE(owner.StageTarget(invalid));
            invalid = manifest;
            invalid.caves = caves == FarCaveMode::BandLimited ? FarCaveMode::BoxFiltered
                                                              : FarCaveMode::BandLimited;
            EXPECT_FALSE(owner.StageTarget(invalid));
            invalid = manifest;
            invalid.numeric = numeric == FarVolumeNumericProfile::RawFloat
                                  ? FarVolumeNumericProfile::LegacySigned16
                                  : FarVolumeNumericProfile::RawFloat;
            EXPECT_FALSE(owner.StageTarget(invalid));
            EXPECT_EQ(owner.State().target_serial, previous.target_serial);
            EXPECT_EQ(owner.State().serving_serial, previous.serving_serial);
            const auto retained = HandoffObserve(owner, {tier, key.tx - 1, 0});
            ASSERT_TRUE(retained.target);
            if (retained.target) {
                EXPECT_EQ(retained.target->identity.key.tx, key.tx - 1);
            }
        }
        ASSERT_TRUE(owner.StageTarget(manifest));
        ASSERT_TRUE(owner.StageTarget(manifest));
        EXPECT_EQ(owner.State().target_serial, serial);
        EXPECT_EQ(owner.State().committed_generation, 1u);
        const auto observed = HandoffObserve(owner, key);
        ASSERT_TRUE(observed.target);
        if (observed.target) {
            EXPECT_EQ(observed.target->identity, current.wanted.identity);
        }
        if (publish) {
            ASSERT_TRUE(HandoffSettle(owner, manifest.tiles, Backend(calls)));
            ASSERT_TRUE(owner.TryPromote());
            EXPECT_EQ(owner.State().serving_serial, serial);
            EXPECT_TRUE(IsFarTileCovering(HandoffObserve(owner, key).serving_coverage));
        }
        for (const auto& stats : owner.Memory())
            EXPECT_LE(stats.bytes_total, stats.limit);
    }
    EXPECT_EQ(calls, 0u);
    owner.CancelAndDrain();
    EXPECT_EQ(owner.State().target_serial, 0u);
    auto rebound = HandoffManifest(1, std::span(&fallback, 1));
    rebound.caves = caves;
    rebound.numeric = numeric;
    ASSERT_TRUE(owner.StageTarget(rebound));
}
INSTANTIATE_TEST_SUITE_P(
    AllTiers,
    FarVolumeHandoffTraversal,
    ::testing::Combine(::testing::Values(std::uint8_t{1},
                                         std::uint8_t{2},
                                         std::uint8_t{3},
                                         std::uint8_t{4},
                                         std::uint8_t{5}),
                       ::testing::Bool(),
                       ::testing::Values(std::size_t{2}, std::size_t{3}, std::size_t{6}),
                       ::testing::Values(FarCaveMode::BandLimited, FarCaveMode::BoxFiltered),
                       ::testing::Values(FarVolumeNumericProfile::RawFloat,
                                         FarVolumeNumericProfile::LegacySigned16)));

TEST(FarVolumeHandoff, ReclamationPreservesCollidingIncomingKeysAndRetainedSpanHistory) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    auto limits = Limits();
    limits.entry_slots.fill(2);
    FarVolumeHandoff owner(jobs, *memory, limits);
    // These keys share a bucket in the four-bucket table. Removing the first
    // must leave the second reachable, even before its incoming turn is staged.
    const std::array original{HandoffInput(memory, {5, 0, 0}, 1, false),
                              HandoffInput(memory, {5, 3, 0}, 1, false, -512, 512)};
    auto initial = HandoffManifest(1, original);
    ASSERT_TRUE(owner.StageTarget(initial));
    const std::array incoming{HandoffInput(memory, {5, 4, 0}, 1, false), original[1]};
    auto replacement = HandoffManifest(2, incoming);
    replacement.committed_generation = 1;
    ASSERT_TRUE(owner.StageTarget(replacement));
    ASSERT_TRUE(owner.StageTarget(replacement));
    EXPECT_FALSE(owner.Observe({5, 0, 0}));
    auto retained = HandoffObserve(owner, {5, 3, 0});
    ASSERT_TRUE(retained.target);
    if (retained.target) {
        EXPECT_EQ(retained.target->intent, original[1].wanted.intent);
    }
    // Removing a key does not discard its history while a descriptor is idle.
    replacement.publication_serial = 3;
    replacement.tiles = std::span(incoming).first(1);
    ASSERT_TRUE(owner.StageTarget(replacement));
    const std::array narrower{HandoffInput(memory, {5, 3, 0}, 2, false)};
    auto regression = HandoffManifest(4, narrower);
    regression.committed_generation = 2;
    const auto used = memory->Pool(5)->used();
    EXPECT_FALSE(owner.StageTarget(regression));
    EXPECT_EQ(owner.State().target_serial, 3u);
    EXPECT_EQ(memory->Pool(5)->used(), used);
    // Retained history also forbids changed metadata under an equal identity.
    auto changed = original;
    changed[1].wanted.build_headroom_bytes = 1;
    regression.tiles = std::span(changed).last(1);
    regression.committed_generation = 1;
    EXPECT_FALSE(owner.StageTarget(regression));
    EXPECT_EQ(owner.State().target_serial, 3u);
    regression.tiles = std::span(original).last(1);
    ASSERT_TRUE(owner.StageTarget(regression));
    retained = HandoffObserve(owner, {5, 3, 0});
    ASSERT_TRUE(retained.target);
    if (retained.target) {
        EXPECT_EQ(retained.target->intent, original[1].wanted.intent);
    }
}

TEST(FarVolumeHandoff, ReclaimedDescriptorCannotIntegrateCancelledWorkerFromPreviousKey) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    auto limits = Limits();
    limits.entry_slots.fill(1);
    FarVolumeHandoff owner(jobs, *memory, limits, &gate);
    HandoffGateRelease const release{gate};
    auto original = std::array{HandoffInput(memory, {5, -1, 0}, 1, true)};
    std::weak_ptr<const FarFrozenObservation> const held = original[0].observation;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, original)));
    gate.Arm();
    owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    const std::array replacement{HandoffInput(memory, {5, 1, 0}, 2, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, replacement)));
    original = {};
    EXPECT_FALSE(held.expired());
    EXPECT_FALSE(owner.Observe({5, -1, 0}));
    EXPECT_FALSE(owner.TryPromote());
    gate.Release();
    std::size_t calls = 0;
    ASSERT_TRUE(HandoffSettle(owner, replacement, Backend(calls)));
    EXPECT_TRUE(held.expired());
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, 0u);
    EXPECT_TRUE(HandoffDraws(owner).empty());
    EXPECT_FALSE(owner.Observe({5, -1, 0}));
    EXPECT_TRUE(IsFarTileCovering(HandoffObserve(owner, {5, 1, 0}).serving_coverage));
}

TEST(FarVolumeHandoff, SpanHistorySurvivesRangeRemovalAndRefusesRegression) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, false, -512, 512)};
    const std::array b{HandoffInput(memory, {5, 1, 0}, 2, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    std::size_t calls = 0;
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    EXPECT_TRUE(IsFarTileCovering(HandoffObserve(owner, {5, 0, 0}).serving_coverage));
    ASSERT_TRUE(HandoffSettle(owner, b, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_FALSE(owner.Observe({5, 0, 0}));
    const std::array c{HandoffInput(memory, {5, 0, 0}, 3, false)};
    const auto used = memory->Pool(5)->used();
    EXPECT_FALSE(owner.StageTarget(HandoffManifest(3, c)));
    EXPECT_EQ(owner.State().serving_serial, 2u);
    EXPECT_EQ(owner.State().target_serial, 2u);
    EXPECT_EQ(memory->Pool(5)->used(), used);
}

TEST(FarVolumeHandoff, ExpiredFrozenObserverRetainsControlChargeUntilItsOwnRelease) {
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    auto input = HandoffInput(memory, {5, 0, 0}, 1, false);
    ASSERT_TRUE(input.observation);
    if (!input.observation)
        return;
    const auto payload = input.observation->payload_bytes();
    const auto used = memory->Pool(5)->used();
    std::weak_ptr<const FarFrozenObservation> observer = input.observation;
    input = {};
    EXPECT_TRUE(observer.expired());
    EXPECT_GT(memory->Pool(5)->used(), 0u);
    EXPECT_LT(memory->Pool(5)->used(), used - payload);
    observer.reset();
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier)
        EXPECT_EQ(memory->Pool(tier)->used(), 0u);
}

TEST(FarVolumeHandoff, WorldSwapDrainsServingStagedAndHeldAuthorityExactlyOnce) {
    JobSystem jobs;
    jobs.startup(2);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    auto owner = std::make_unique<FarVolumeHandoff>(jobs, *memory, Limits(), &gate);
    HandoffGateRelease const release{gate};
    auto a = std::array{HandoffInput(memory, {5, 0, 0}, 1, true),
                        HandoffInput(memory, {5, 1, 0}, 1, true)};
    std::size_t creates = 0, destroys = 0;
    class Counted final : public FarVolumeGpuAllocation {
    public:
        Counted(std::uint64_t n, std::size_t& d)
            : count_(n)
            , destroys_(d) {}
        ~Counted() override {
            ++destroys_;
        }
        std::uint64_t bytes() const noexcept override {
            return count_;
        }

    private:
        std::uint64_t count_;
        std::size_t& destroys_;
    };
    const FarVolumeUploadBackend backend = [&](const auto& mesh) {
        ++creates;
        return std::make_unique<Counted>(mesh.vertices.size() * sizeof(VoxelVertex) +
                                             mesh.indices.size() * sizeof(std::uint32_t),
                                         destroys);
    };
    ASSERT_TRUE(owner->StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(*owner, a, backend));
    ASSERT_TRUE(owner->TryPromote());
    auto b = std::array{HandoffInput(memory, {5, 0, 0}, 2, true),
                        HandoffInput(memory, {5, 1, 0}, 2, true)};
    std::weak_ptr<const FarFrozenObservation> held = b[0].observation;
    ASSERT_TRUE(owner->StageTarget(HandoffManifest(2, b)));
    gate.Arm();
    owner->AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    ASSERT_TRUE(HandoffSettle(*owner, std::span(b).last(1), backend));
    EXPECT_GT(HandoffObserve(*owner, {5, 1, 0}).staged_gpu_bytes, 0u);
    EXPECT_GT(HandoffObserve(*owner, {5, 0, 0}).serving_gpu_bytes, 0u);
    a = {};
    b = {};
    EXPECT_FALSE(held.expired());
    // A helper releases the admitted scratch barrier so drain can join it.
    // Authority remains owned until every queue has joined.
    std::jthread const releaser([&] { gate.Release(); });
    owner->CancelAndDrain();
    EXPECT_TRUE(held.expired());
    EXPECT_EQ(destroys, creates);
    EXPECT_EQ(owner->State().target_serial, 0u);
    EXPECT_TRUE(HandoffDraws(*owner).empty());
    owner.reset();
    // The expired observer still owns the charged shared control allocation.
    // Release it only after checking that drain destroyed the image itself.
    held.reset();
    for (std::size_t i = 1; i <= kFarTierCount; ++i)
        EXPECT_EQ(memory->Pool(i)->used(), 0u);
}

TEST(FarVolumeHandoff, BuildAndConversionFailuresKeepServingAndLatchUntilNewPublication) {
    for (const bool conversion : {false, true}) {
        JobSystem jobs;
        jobs.startup(1);
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        HandoffWorkerGate upstream;
        auto limits = Limits();
        limits.queues[4].max_converted_bytes = 128ULL * 1024;
        FarVolumeHandoff owner(jobs, *memory, limits, &upstream);
        const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true)};
        std::size_t calls = 0;
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
        ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
        ASSERT_TRUE(owner.TryPromote());
        const auto before = HandoffDraws(owner);
        const std::array b{HandoffInput(memory, {5, 0, 0}, 2, true, -256, 64, false, conversion)};
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
        upstream.RefuseNextAllocation(!conversion);
        ASSERT_TRUE(HandoffWait([&] {
            owner.AdvanceFrame(0, 0, {1, 1, 32ULL * 1024 * 1024}, Backend(calls));
            return HandoffObserve(owner, {5, 0, 0}).target_eligibility ==
                   FarVolumeEligibility::RequestRejected;
        }));
        EXPECT_EQ(HandoffObserve(owner, {5, 0, 0}).target_build_failure,
                  conversion ? FarVolumeBuildStatus::ConvertRefused
                             : FarVolumeBuildStatus::CompileRefused);
        EXPECT_FALSE(owner.TryPromote());
        const auto after = HandoffDraws(owner);
        ASSERT_EQ(after.size(), 1u);
        EXPECT_EQ(after[0].identity, before[0].identity);
        EXPECT_EQ(after[0].request, before[0].request);
        EXPECT_EQ(after[0].resource, before[0].resource);
        EXPECT_EQ(
            owner.AdvanceFrame(0, 0, {1, 1, 32ULL * 1024 * 1024}, Backend(calls)).build_attempts,
            0u);
        EXPECT_TRUE(owner.StageTarget(HandoffManifest(2, b))); // Idempotence never retries.
        EXPECT_EQ(HandoffObserve(owner, {5, 0, 0}).target_eligibility,
                  FarVolumeEligibility::RequestRejected);
        if (!conversion) {
            auto retry = HandoffManifest(3, b);
            retry.committed_generation = 2;
            ASSERT_TRUE(owner.StageTarget(retry));
            ASSERT_TRUE(HandoffSettle(owner, b, Backend(calls)));
            ASSERT_TRUE(owner.TryPromote());
            EXPECT_EQ(owner.State().serving_serial, 3u);
        }
    }
}

TEST(FarVolumeHandoff, UnchargedOrForeignWaterAndForeignCapturePoolsAreRefused) {
    JobSystem jobs;
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    auto input = std::array{HandoffInput(memory, {5, 0, 0}, 1, false)};
    auto original = input;
    input[0].wanted.water = std::make_shared<const FarWorldFrozenWaterObservation>(
        input[0].observation, input[0].wanted.identity.key);
    EXPECT_FALSE(owner.StageTarget(HandoffManifest(1, input)));
    input = original;
    auto foreign_memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    const auto foreign = HandoffInput(foreign_memory, {5, 0, 0}, 1, false);
    input[0].wanted.water = foreign.wanted.water;
    EXPECT_FALSE(owner.StageTarget(HandoffManifest(1, input)));
    input[0] = foreign;
    EXPECT_FALSE(owner.StageTarget(HandoffManifest(1, input)));
    EXPECT_EQ(owner.State().target_serial, 0u);
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, original)));
}

TEST(FarVolumeHandoff, IdenticalImmutableWorkSurvivesPublicationSerialOnlySupersession) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    FarVolumeHandoff owner(jobs, *memory, Limits(), &gate);
    HandoffGateRelease const release{gate};
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    gate.Arm();
    owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    auto next = HandoffManifest(2, a);
    next.committed_generation = 1;
    ASSERT_TRUE(owner.StageTarget(next));
    EXPECT_EQ(HandoffObserve(owner, {5, 0, 0}).target_eligibility, FarVolumeEligibility::Building);
    gate.Release();
    std::size_t calls = 0;
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    EXPECT_EQ(calls, 1u);
    ASSERT_TRUE(owner.TryPromote());
    next.publication_serial = 3;
    const auto used = memory->Pool(5)->used();
    ASSERT_TRUE(owner.StageTarget(next));
    EXPECT_EQ(memory->Pool(5)->used(), used);
    EXPECT_EQ(owner.AdvanceFrame(0, 0, {1, 1, 32ULL * 1024 * 1024}, Backend(calls)).build_attempts,
              0u);
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, HandoffObserve(owner, {5, 0, 0}).serving_gpu_bytes);
}

TEST(FarVolumeHandoff, ChangedFinerAllocationRetiresAtFallbackThenArrivesWithoutResurrection) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    FarVolumeHandoff owner(jobs, *memory, Limits(), &gate);
    HandoffGateRelease const release{gate};
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true),
                       HandoffInput(memory, {4, 0, 0}, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_GT(owner.Memory()[3].bytes_gpu, 0u);
    const std::array b{HandoffInput(memory, {5, 0, 0}, 2, false),
                       HandoffInput(memory, {4, 0, 0}, 2, true)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] {
        owner.AdvanceFrame(0, 0, {0, 1, 32ULL * 1024 * 1024}, Backend(calls));
        return IsFarTileCovering(HandoffObserve(owner, {5, 0, 0}).target_coverage);
    }));
    gate.Arm();
    owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_FALSE(HandoffObserve(owner, {4, 0, 0}).serving);
    EXPECT_EQ(owner.Memory()[3].bytes_gpu, 0u);
    EXPECT_TRUE(HandoffDraws(owner).empty());
    gate.Release();
    ASSERT_TRUE(HandoffSettle(owner, std::span(b).last(1), Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(owner.State().last_promotion, FarHandoffPromotion::FinerArrival);
    EXPECT_EQ(owner.State().serving_serial, 2u);
    const auto draws = HandoffDraws(owner);
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].identity, b[1].wanted.identity);
    EXPECT_NE(draws[0].identity, a[1].wanted.identity);
}

TEST(FarVolumeHandoff, MeasuredSingleGenerationBudgetRefusesCaptureOverlapAndKeepsServing) {
    JobSystem jobs;
    jobs.startup(1);
    std::uint64_t single_peak = 0;
    {
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        FarVolumeHandoff owner(jobs, *memory, Limits());
        const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true)};
        std::size_t calls = 0;
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
        ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
        ASSERT_TRUE(owner.TryPromote());
        single_peak = memory->Pool(5)->peak();
    }
    auto budgets = Budgets();
    budgets.bytes[4] = single_peak;
    auto memory = std::make_shared<FarVolumeTierMemory>(budgets);
    FarVolumeHandoff owner(jobs, *memory, Limits());
    auto a = std::array{HandoffInput(memory, {5, 0, 0}, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    a = {}; // The serving manifest is the remaining authority owner.
    const auto before = HandoffDraws(owner);
    const auto used = memory->Pool(5)->used();
    FarFrozenCaptureError error = FarFrozenCaptureError::None;
    const std::array b{HandoffInput(memory, {5, 0, 0}, 2, true, -256, 64, false, false, &error)};
    EXPECT_EQ(error, FarFrozenCaptureError::ByteLimit);
    EXPECT_FALSE(b[0].observation);
    EXPECT_FALSE(owner.StageTarget(HandoffManifest(2, b)));
    EXPECT_EQ(memory->Pool(5)->limit(), single_peak);
    EXPECT_EQ(memory->Pool(5)->used(), used);
    EXPECT_EQ(owner.State().serving_serial, 1u);
    EXPECT_EQ(owner.State().target_serial, 1u);
    const auto after = HandoffDraws(owner);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].identity, before[0].identity);
    EXPECT_EQ(after[0].resource, before[0].resource);
    EXPECT_EQ(calls, 1u);
}

TEST(FarVolumeHandoff, SupersededStagedPairsReleaseWhileServingAndRunningInputsStay) {
    JobSystem jobs;
    jobs.startup(2);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    HandoffWorkerGate gate;
    FarVolumeHandoff owner(jobs, *memory, Limits(), &gate);
    HandoffGateRelease const release{gate};
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, true),
                       HandoffInput(memory, {5, 1, 0}, 1, true)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    const auto serving = HandoffDraws(owner);
    const auto gpu = owner.Memory()[4].bytes_gpu;
    auto b = std::array{HandoffInput(memory, {5, 0, 0}, 2, true),
                        HandoffInput(memory, {5, 1, 0}, 2, true)};
    std::weak_ptr<const FarFrozenObservation> const held = b[0].observation;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    gate.Arm();
    owner.AdvanceFrame(0, 0, {1, 0, 0}, {});
    ASSERT_TRUE(HandoffWait([&] { return gate.Entered(); }));
    ASSERT_TRUE(HandoffSettle(owner, std::span(b).last(1), Backend(calls)));
    EXPECT_GT(owner.Memory()[4].bytes_gpu, gpu);
    const std::array c{HandoffInput(memory, {5, 0, 0}, 3, false),
                       HandoffInput(memory, {5, 1, 0}, 3, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(3, c)));
    b = {};
    EXPECT_FALSE(held.expired());
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, gpu);
    EXPECT_FALSE(owner.TryPromote());
    const auto during = HandoffDraws(owner);
    ASSERT_EQ(during.size(), serving.size());
    for (std::size_t i = 0; i < during.size(); ++i) {
        EXPECT_EQ(during[i].identity, serving[i].identity);
        EXPECT_EQ(during[i].resource, serving[i].resource);
    }
    const auto before = calls;
    gate.Release();
    ASSERT_TRUE(HandoffSettle(owner, c, Backend(calls)));
    EXPECT_TRUE(held.expired());
    EXPECT_EQ(calls, before);
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, 0u);
    EXPECT_TRUE(HandoffDraws(owner).empty());
}

TEST(FarVolumeHandoff, EmptyTerrainWithWetWaterCoversOnlyAsCombinedUploadedPair) {
    JobSystem jobs;
    jobs.startup(1);
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    FarVolumeHandoff owner(jobs, *memory, Limits());
    const std::array a{HandoffInput(memory, {5, 0, 0}, 1, false)};
    std::size_t calls = 0;
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, a)));
    ASSERT_TRUE(HandoffSettle(owner, a, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    const std::array b{
        HandoffInput(memory, {5, 0, 0}, 2, false, -256, 64, false, false, nullptr, true)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(2, b)));
    ASSERT_TRUE(HandoffSettle(owner, b, Backend(calls)));
    EXPECT_EQ(calls, 1u);
    EXPECT_TRUE(HandoffDraws(owner).empty());
    const auto staged = HandoffObserve(owner, {5, 0, 0});
    EXPECT_EQ(staged.target_coverage.state, FarTileState::Uploaded);
    EXPECT_TRUE(staged.target_water_complete);
    EXPECT_FALSE(staged.target_water_known_dry);
    EXPECT_EQ(staged.target_terrain_indices, 0u);
    EXPECT_GT(staged.target_water_indices, 0u);
    ASSERT_TRUE(owner.TryPromote());
    const auto wet = HandoffObserve(owner, {5, 0, 0});
    EXPECT_EQ(wet.serving_water_indices, staged.target_water_indices);
    EXPECT_EQ(HandoffDraws(owner).size(), 1u);
    const std::array c{HandoffInput(memory, {5, 0, 0}, 3, false)};
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(3, c)));
    ASSERT_TRUE(HandoffSettle(owner, c, Backend(calls)));
    ASSERT_TRUE(owner.TryPromote());
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(owner.Memory()[4].bytes_gpu, 0u);
    EXPECT_TRUE(HandoffObserve(owner, {5, 0, 0}).serving_water_known_dry);
    EXPECT_TRUE(HandoffDraws(owner).empty());
}

namespace {
// Capacities below are stress inputs within the existing 512-entry renderer
// contract. Image/pool limits and the established capture helpers stay intact.
FarVolumeResidencyLimits AdmissionLimits(std::size_t count) {
    auto limits = Limits();
    limits.entry_slots.fill(count);
    for (auto& queue : limits.queues)
        queue.descriptor_slots = count;
    return limits;
}
void CheckAdmissionWork(const FarVolumeAdmissionWork& work, std::size_t count) {
    // A deterministic comparison envelope, not a time or production work limit.
    // Pairwise validation/retirement at 512 keys exceeds this envelope.
    const auto levels = std::bit_width(count) + 1;
    EXPECT_LE(work.key_comparisons, 12 * count * levels);
    EXPECT_LE(work.descriptor_visits, 12 * count);
}
class AdmissionAllocationObserver final : public std::pmr::memory_resource {
public:
    std::size_t Allocations() const noexcept {
        return allocations_;
    }

private:
    std::size_t allocations_ = 0;
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
std::vector<FarTileKey> HandoffTargetOrder(FarVolumeHandoff& owner) {
    std::vector<FarTileKey> keys;
    owner.VisitObservations(
        [](void* context, const FarTileKey& key, const FarHandoffTileObservation& observation) {
            if (observation.target)
                static_cast<std::vector<FarTileKey>*>(context)->push_back(key);
        },
        &keys);
    return keys;
}
std::vector<FarTileKey> AdmissionCollisionKeys() {
    const auto mix = [](std::uint64_t value) {
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    };
    std::vector<FarTileKey> keys;
    // Fixed candidate bound; all 16 keys collide in the former 16-bucket
    // eight-entry index. Coordinates include both signs and remain exact.
    for (std::int64_t candidate = 0; candidate < 2048 && keys.size() < 16; ++candidate) {
        const auto x = candidate % 32 - 16, z = candidate / 32 - 32;
        if (((mix(static_cast<std::uint64_t>(x)) ^
              mix(static_cast<std::uint64_t>(z) + 0x9e3779b97f4a7c15ULL)) %
             16) == 0)
            keys.push_back({5, x, z});
    }
    return keys;
}
} // namespace

TEST(FarVolumeResidency, AdmissionScalePreservesOrderIdentityAndRefusalWithoutScratchGrowth) {
    for (const std::size_t count : {32u, 128u, 512u}) {
        SCOPED_TRACE(count);
        JobSystem jobs;
        FarVolumeTierMemory memory(Budgets());
        AdmissionAllocationObserver allocator;
        FarVolumeResidency owner(jobs, memory, AdmissionLimits(count), &allocator);
        EXPECT_FALSE(owner.Observe({0, 0, 0}));
        EXPECT_FALSE(owner.Observe({6, 0, 0}));
        EXPECT_FALSE(owner.Observe({5, 0, 0}));
        std::vector<FarVolumeWantedRequest> requests;
        for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier)
            for (std::size_t i = 0; i < count; ++i)
                requests.push_back(Wanted({tier, static_cast<std::int64_t>(i) - 256, -1}));
        const auto allocations = allocator.Allocations();
        ASSERT_TRUE(owner.ReplaceWanted(requests));
        CheckAdmissionWork(owner.AdmissionWork(), requests.size());
        for (const auto& request : requests)
            EXPECT_EQ(Observation(owner, request.identity.key).desired, request.identity);
        std::reverse(requests.begin(), requests.end());
        ASSERT_TRUE(owner.ReplaceWanted(requests));
        CheckAdmissionWork(owner.AdmissionWork(), requests.size());
        const auto before = owner.Snapshot();
        auto bad = requests;
        bad.back() = bad.front();
        EXPECT_FALSE(owner.ReplaceWanted(bad));
        CheckAdmissionWork(owner.AdmissionWork(), requests.size());
        bad = requests;
        bad.back().identity.key.tier = 0;
        EXPECT_FALSE(owner.ReplaceWanted(bad));
        bad = requests;
        bad.back().build_headroom_bytes = 1;
        EXPECT_FALSE(owner.ReplaceWanted(bad));
        for (std::size_t i = 0; i < before.size(); ++i) {
            const auto after = owner.Snapshot()[i];
            EXPECT_EQ(after.wanted, before[i].wanted);
            EXPECT_EQ(after.memory.bytes_total, before[i].memory.bytes_total);
        }
        for (auto& request : requests) {
            request.identity.key.tz = 1;
            request.intent.key.z = 1;
            ++request.identity.bake_generation;
        }
        ASSERT_TRUE(owner.ReplaceWanted(requests));
        CheckAdmissionWork(owner.AdmissionWork(), requests.size());
        for (const auto& request : requests) {
            EXPECT_EQ(Observation(owner, request.identity.key).desired, request.identity);
            auto absent = request.identity.key;
            absent.tz = -1;
            EXPECT_FALSE(owner.Observe(absent));
        }
        EXPECT_EQ(allocator.Allocations(), allocations);
        ASSERT_TRUE(owner.ReplaceWanted({}));
        EXPECT_FALSE(owner.Observe(requests.front().identity.key));
        owner.CancelAndDrain();
        EXPECT_FALSE(owner.Observe(requests.back().identity.key));
        ASSERT_TRUE(owner.ReplaceWanted(requests));
        CheckAdmissionWork(owner.AdmissionWork(), requests.size());
        EXPECT_EQ(allocator.Allocations(), allocations);
    }
}

TEST(FarVolumeResidency, AdmissionIndexSizeRefusalsUnwindBeforeAnyTierStorageEscapes) {
    JobSystem jobs;
    for (unsigned failure = 0; failure < 4; ++failure) {
        auto limits = AdmissionLimits(1);
        if (failure == 0)
            limits.entry_slots[0] = 0;
        else if (failure == 1)
            limits.queues[0].descriptor_slots = 0;
        else if (failure == 2)
            limits.queues[0].result_slots = 0;
        else {
            limits.entry_slots[0] = std::numeric_limits<std::size_t>::max();
            limits.queues[0].descriptor_slots = limits.entry_slots[0];
        }
        FarVolumeTierMemory memory(Budgets());
        if (failure < 3) {
            EXPECT_THROW(FarVolumeResidency(jobs, memory, limits), std::invalid_argument);
        } else {
            EXPECT_THROW(FarVolumeResidency(jobs, memory, limits), FarVolumeMemoryRefused);
        }
        for (std::size_t tier = 1; tier <= kFarTierCount; ++tier)
            EXPECT_EQ(memory.Pool(tier)->used(), 0u);
    }
}

TEST(FarVolumeHandoff, AdmissionCollisionSupersessionKeepsSlotsAndIdempotenceWithoutScratchGrowth) {
    JobSystem jobs;
    auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
    auto limits = AdmissionLimits(8);
    AdmissionAllocationObserver allocator;
    FarVolumeHandoff owner(jobs, *memory, limits, &allocator);
    EXPECT_FALSE(owner.Observe({0, 0, 0}));
    EXPECT_FALSE(owner.Observe({6, 0, 0}));
    EXPECT_FALSE(owner.Observe({5, 0, 0}));
    const auto keys = AdmissionCollisionKeys();
    ASSERT_EQ(keys.size(), 16u);
    std::vector<FarHandoffTileInput> original, replacement;
    for (std::size_t i = 0; i < 8; ++i) {
        original.push_back(HandoffInput(memory, keys[i], 1, false));
        replacement.push_back(HandoffInput(memory, keys[i + 8], 1, false));
        ASSERT_TRUE(original.back().observation);
        ASSERT_TRUE(replacement.back().observation);
    }
    const auto allocations = allocator.Allocations();
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, original)));
    CheckAdmissionWork(owner.AdmissionWork(), std::size_t{5} * 8);
    const auto order = HandoffTargetOrder(owner);
    ASSERT_EQ(order.size(), original.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        EXPECT_EQ(order[i], original[i].wanted.identity.key);
    std::reverse(original.begin(), original.end());
    const auto before = owner.Memory();
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, original)));
    EXPECT_EQ(HandoffTargetOrder(owner), order);
    EXPECT_EQ(owner.State().target_serial, 1u);
    auto bad = replacement;
    bad.back().observation.reset();
    auto next = HandoffManifest(2, bad);
    next.committed_generation = 1;
    EXPECT_FALSE(owner.StageTarget(next));
    EXPECT_EQ(HandoffTargetOrder(owner), order);
    bad = replacement;
    bad.back() = bad.front();
    next.tiles = bad;
    EXPECT_FALSE(owner.StageTarget(next));
    EXPECT_EQ(HandoffTargetOrder(owner), order);
    for (std::size_t i = 0; i < before.size(); ++i)
        EXPECT_EQ(owner.Memory()[i].bytes_total, before[i].bytes_total);
    std::rotate(replacement.begin(), replacement.begin() + 3, replacement.end());
    next.tiles = replacement;
    ASSERT_TRUE(owner.StageTarget(next));
    CheckAdmissionWork(owner.AdmissionWork(), std::size_t{5} * 8);
    EXPECT_EQ(owner.State().target_serial, 2u);
    const auto replaced_order = HandoffTargetOrder(owner);
    ASSERT_EQ(replaced_order.size(), replacement.size());
    for (std::size_t i = 0; i < replaced_order.size(); ++i)
        EXPECT_EQ(replaced_order[i], replacement[i].wanted.identity.key);
    for (const auto& input : original)
        EXPECT_FALSE(owner.Observe(input.wanted.identity.key));
    std::reverse(replacement.begin(), replacement.end());
    next.tiles = replacement;
    ASSERT_TRUE(owner.StageTarget(next));
    EXPECT_EQ(HandoffTargetOrder(owner), replaced_order);
    next.publication_serial = 1;
    EXPECT_FALSE(owner.StageTarget(next));
    EXPECT_EQ(HandoffTargetOrder(owner), replaced_order);
    next.publication_serial = 3;
    next.tiles = {};
    EXPECT_FALSE(owner.StageTarget(next));
    EXPECT_EQ(owner.State().target_serial, 2u);
    EXPECT_EQ(allocator.Allocations(), allocations);
    owner.CancelAndDrain();
    EXPECT_TRUE(HandoffTargetOrder(owner).empty());
    EXPECT_FALSE(owner.Observe(keys.front()));
    ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, replacement)));
    EXPECT_EQ(allocator.Allocations(), allocations);
}

TEST(FarVolumeHandoff, AdmissionScaleLateDuplicatesAndInvalidSupportLeavePublishedTargetUntouched) {
    for (const std::size_t count : {8u, 64u, 512u}) {
        SCOPED_TRACE(count);
        JobSystem jobs;
        auto memory = std::make_shared<FarVolumeTierMemory>(Budgets());
        FarVolumeHandoff owner(jobs, *memory, AdmissionLimits(count));
        const std::array original{HandoffInput(memory, {5, 0, 0}, 1, false)};
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, original)));
        std::vector<FarHandoffTileInput> inputs(count, original[0]);
        // Only one captured image is retained. Incompatible support is refused;
        // the duplicate pass must still be bounded before authority admission.
        for (std::size_t i = 0; i < count; ++i) {
            const auto x = static_cast<std::int64_t>((i * 17) % count) - 256;
            inputs[i].wanted.identity.key.tx = x;
            inputs[i].wanted.intent.key.x = x;
        }
        const auto used = owner.Memory()[4].bytes_total;
        auto manifest = HandoffManifest(2, inputs);
        manifest.committed_generation = 1;
        EXPECT_FALSE(owner.StageTarget(manifest));
        CheckAdmissionWork(owner.AdmissionWork(), 5 * count);
        inputs.back() = inputs.front();
        EXPECT_FALSE(owner.StageTarget(manifest));
        EXPECT_GE(owner.AdmissionWork().key_comparisons, count - 1);
        EXPECT_LE(owner.AdmissionWork().key_comparisons, 12 * count * (std::bit_width(count) + 1));
        CheckAdmissionWork(owner.AdmissionWork(), 5 * count);
        EXPECT_EQ(owner.State().target_serial, 1u);
        EXPECT_EQ(owner.Memory()[4].bytes_total, used);
        const auto retained = HandoffObserve(owner, {5, 0, 0});
        ASSERT_TRUE(retained.target);
        if (retained.target) {
            EXPECT_EQ(retained.target->identity, original[0].wanted.identity);
        }
        inputs.push_back(original[0]);
        manifest.tiles = inputs;
        EXPECT_FALSE(owner.StageTarget(manifest));
        EXPECT_EQ(owner.State().target_serial, 1u);
        EXPECT_EQ(owner.Memory()[4].bytes_total, used);
        ASSERT_TRUE(owner.StageTarget(HandoffManifest(1, original)));
    }
}
