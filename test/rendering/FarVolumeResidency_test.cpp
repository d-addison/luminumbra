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
    return {{1, 42, 0, key, 1, 1},
            {canonical, {}},
            0,
            std::make_shared<const FarWorldConstantWaterObservation>(42, false, 0),
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
        request.water = std::make_shared<const FarWorldConstantWaterObservation>(42, true, 0);
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
    owner.AdvanceFrame(0, 0, {0, 5, 32 * 1024 * 1024}, Producer(), backend);
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
    owner.AdvanceFrame(0, 0, {0, 1, 32 * 1024 * 1024}, Producer(), backend);
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
        owner.AdvanceFrame(0, 0, {5, 5, 32 * 1024 * 1024}, Producer(), backend);
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
    requests[0].water = std::make_shared<const FarWorldConstantWaterObservation>(
        requests[0].identity.content_hash, false, 0);
    ASSERT_TRUE(owner.ReplaceWanted(requests));
    // Other Ready result is retained but cannot consume this build's dispatch.
    const std::array changed{requests[0]};
    ASSERT_TRUE(WaitReady(owner, changed, Producer(true)));
    FarVolumeUploadBackend failing = [](const auto&) -> std::unique_ptr<FarVolumeGpuAllocation> {
        throw std::runtime_error("partial backend failure");
    };
    std::size_t failed_calls = 0;
    FarVolumeUploadBackend counted_failure = [&](const auto& mesh) {
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
    FarVolumeAuthorityProducer producer = [](const auto& request, auto&) {
        return Authority(request, request.identity.key.tier == 5);
    };
    std::size_t backend_calls = 0;
    const auto backend = Backend(backend_calls);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        owner.AdvanceFrame(0, 0, {5, 5, 4 * 1024 * 1024}, producer, backend);
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
    wanted[2].water = std::make_shared<const FarWorldConstantWaterObservation>(
        wanted[2].identity.content_hash, false, 0);
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
        FarVolumeAuthorityProducer producer = [](const auto& request, auto&) {
            return Authority(request, request.identity.key.tier == 2);
        };
        std::size_t calls = 0;
        const auto backend = Backend(calls);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            owner.AdvanceFrame(0, 0, {5, 5, 4 * 1024 * 1024}, producer, backend);
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
            request.water = std::make_shared<const FarWorldConstantWaterObservation>(42, true, 0);
            request.water_bound = FarWorldWaterBound{false, 0, 0};
        }
        ASSERT_TRUE(owner.ReplaceWanted(requests));
        std::size_t calls = 0;
        const auto backend = Backend(calls);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            owner.AdvanceFrame(0, 0, {5, 5, 8 * 1024 * 1024}, Producer(true), backend);
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
                requests[4].identity.content_hash, true, 0);
            ASSERT_TRUE(owner.ReplaceWanted(requests));
            ASSERT_TRUE(WaitReady(owner, std::span(requests).last(1), Producer(true)));
            FarVolumeUploadBackend failure =
                [](const auto&) -> std::unique_ptr<FarVolumeGpuAllocation> {
                return {};
            };
            const auto combined_bytes =
                Observation(owner, requests[4].identity.key).ready_upload_bytes;
            const auto failed = owner.AdvanceFrame(0, 0, {0, 1, 8 * 1024 * 1024}, {}, failure);
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
