#include "luminumbra_client/rendering/FarVolumeUpload.h"

#include <chrono>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;

FarVolumeTierMemoryStats Observe(const FarVolumeTierMemory& memory, std::size_t tier) {
    const auto stats = memory.Stats(tier);
    if (!stats) {
        ADD_FAILURE() << "No memory observation for tier " << tier;
        return {};
    }
    return *stats;
}

FarVolumeQueueLimits UploadLimits() {
    return {
        2,
        1,
        {{131 * 131, 20'000'000}, {131'072, 16'384}, {1'048'576, 3'000'000, 300'000, 1'000'000}},
        4 * 1024 * 1024};
}
FarVolumeQueueRequest UploadRequest(bool plane = true) {
    FarVolumeQueueRequest request;
    request.identity = {1, 42, 0, {1, 0, 0}, 7, 3};
    request.intent = {{1, 0, 0}, {}};
    request.authority = {42,
                         FarCaveMode::BandLimited,
                         FarVolumeNumericProfile::RawFloat,
                         [](std::int64_t, std::int64_t) { return 0.0f; },
                         [plane](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
                             return FarVolumeSample{plane ? float(p.y) + .25f : 1.0f, 7};
                         }};
    request.water = std::make_shared<const FarWorldConstantWaterObservation>(
        request.identity.content_hash, false, 0);
    request.water_bound = FarWorldWaterBound{true, 0, 0};
    return request;
}
std::optional<FarVolumeQueueCompletion> Build(FarVolumeBuildQueue& queue,
                                              const FarVolumeQueueRequest& request) {
    EXPECT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        queue.Pump(1);
        auto result = queue.TakeCompleted(request.identity, request.intent);
        if (result)
            return result;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    ADD_FAILURE() << "Upload fixture queue result timeout";
    return std::nullopt;
}
std::uint64_t Bytes(const FarVolumeRenderMesh& mesh) {
    return mesh.vertices.size() * sizeof(VoxelVertex) + mesh.indices.size() * sizeof(std::uint32_t);
}
struct FakeGpu final : FarVolumeGpuAllocation {
    std::uint64_t allocated;
    std::function<void()> deleted;
    FakeGpu(std::uint64_t bytes, std::function<void()> cleanup = {})
        : allocated(bytes)
        , deleted(std::move(cleanup)) {}
    ~FakeGpu() override {
        if (deleted)
            deleted();
    }
    std::uint64_t bytes() const noexcept override {
        return allocated;
    }
};
} // namespace

TEST(FarVolumeUpload, CombinedWetAllocationByteDebitAndWaterFailurePreserveOldPair) {
    JobSystem jobs;
    jobs.startup(2);
    FarTierBudgets budgets;
    budgets.bytes.fill(64 * 1024 * 1024);
    FarVolumeTierMemory memory(budgets);
    auto limits = UploadLimits();
    limits.max_converted_bytes = 16 * 1024 * 1024;
    FarVolumeBuildQueue queue(jobs, *memory.Pool(1), limits);
    auto request = UploadRequest();
    request.water = std::make_shared<const FarWorldConstantWaterObservation>(42, true, 0);
    request.water_bound = FarWorldWaterBound{false, 0, 0};
    auto result = Build(queue, request);
    ASSERT_TRUE(result);
    if (result) {
        const auto bytes = Bytes(result->mesh());
        EXPECT_GT(result->mesh().terrain_indices, 0u);
        EXPECT_EQ(result->mesh().indices.size(),
                  result->mesh().terrain_indices + result->mesh().water_indices);
        EXPECT_GT(bytes, 65536u * sizeof(VoxelVertex) + 98304u * sizeof(std::uint32_t));
        FarVolumeUploadedTile output;
        std::size_t calls = 0;
        const auto backend = [&calls](const FarVolumeRenderMesh& mesh) {
            ++calls;
            return std::make_unique<FakeGpu>(Bytes(mesh));
        };
        FarVolumeUploadError error;
        EXPECT_FALSE(UploadFarVolume(
            *result, request.identity, request.intent, memory, bytes - 1, backend, output, &error));
        EXPECT_EQ(error, FarVolumeUploadError::ByteLimit);
        EXPECT_EQ(calls, 0u);
        ASSERT_TRUE(UploadFarVolume(
            *result, request.identity, request.intent, memory, bytes, backend, output));
        EXPECT_EQ(output.gpu_bytes(), bytes);
        EXPECT_EQ(output.water_indices(), 98304u);
        EXPECT_EQ(output.terrain_indices(), result->mesh().terrain_indices);
        EXPECT_TRUE(output.water_complete());
        const auto* old = output.resource();
        const auto old_identity = output.identity();
        ++request.identity.bake_generation;
        request.water.reset();
        request.water_bound.reset();
        result.reset();
        result = Build(queue, request);
        ASSERT_TRUE(result);
        if (result) {
            EXPECT_EQ(result->status(), FarVolumeBuildStatus::WaterRefused);
            EXPECT_FALSE(UploadFarVolume(
                *result, request.identity, request.intent, memory, bytes, backend, output, &error));
            EXPECT_EQ(error, FarVolumeUploadError::NotReady);
            EXPECT_EQ(output.resource(), old);
            EXPECT_EQ(output.identity(), old_identity);
            EXPECT_EQ(output.gpu_bytes(), bytes);
            EXPECT_FALSE(IsFarTileCovering(output.Status(request.identity, request.intent)));
            EXPECT_EQ(calls, 1u);
        }
    }
    jobs.shutdown();
}

TEST(FarVolumeUpload, CurrencyByteAndSharedPoolRefusalsPrecedeBackend) {
    JobSystem jobs;
    jobs.startup(1);
    FarTierBudgets budgets;
    budgets.bytes.fill(64 * 1024 * 1024);
    FarVolumeTierMemory memory(budgets), other(budgets);
    FarVolumeBuildQueue queue(jobs, *memory.Pool(1), UploadLimits());
    const auto request = UploadRequest();
    auto completion = Build(queue, request);
    ASSERT_TRUE(completion);
    if (!completion)
        return;
    ASSERT_EQ(completion->status(), FarVolumeBuildStatus::Ready);
    std::size_t calls = 0;
    FarVolumeUploadBackend backend = [&calls](const FarVolumeRenderMesh& mesh) {
        ++calls;
        return std::make_unique<FakeGpu>(Bytes(mesh));
    };
    FarVolumeUploadedTile output;
    FarVolumeUploadError error;
    for (int axis = 0; axis < 6; ++axis) {
        auto stale = request.identity;
        if (axis == 0)
            ++stale.world_epoch;
        if (axis == 1)
            ++stale.content_hash;
        if (axis == 2)
            ++stale.cave_policy;
        if (axis == 3)
            ++stale.key.tx;
        if (axis == 4)
            ++stale.bake_generation;
        if (axis == 5)
            ++stale.span_generation;
        EXPECT_FALSE(UploadFarVolume(
            *completion, stale, request.intent, memory, 4 * 1024 * 1024, backend, output, &error));
        EXPECT_EQ(error, FarVolumeUploadError::Stale);
    }
    EXPECT_FALSE(UploadFarVolume(*completion,
                                 request.identity,
                                 request.intent,
                                 other,
                                 4 * 1024 * 1024,
                                 backend,
                                 output,
                                 &error));
    EXPECT_EQ(error, FarVolumeUploadError::InvalidBinding);
    EXPECT_FALSE(UploadFarVolume(*completion,
                                 request.identity,
                                 request.intent,
                                 memory,
                                 Bytes(completion->mesh()) - 1,
                                 backend,
                                 output,
                                 &error));
    EXPECT_EQ(error, FarVolumeUploadError::ByteLimit);
    memory.Pool(1)->RefuseAttemptsFrom(memory.Pool(1)->attempts());
    EXPECT_FALSE(UploadFarVolume(*completion,
                                 request.identity,
                                 request.intent,
                                 memory,
                                 4 * 1024 * 1024,
                                 backend,
                                 output,
                                 &error));
    EXPECT_EQ(error, FarVolumeUploadError::AdmissionRefused);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(Observe(memory, 1).bytes_gpu, 0u);
    EXPECT_EQ(output.resource(), nullptr);
}

TEST(FarVolumeUpload, ReplacementAndBackendFailureKeepOldChargeUntilGpuDeletion) {
    JobSystem jobs;
    jobs.startup(1);
    FarTierBudgets budgets;
    budgets.bytes.fill(64 * 1024 * 1024);
    FarVolumeTierMemory memory(budgets);
    FarVolumeBuildQueue queue(jobs, *memory.Pool(1), UploadLimits());
    const auto request = UploadRequest();
    auto completion = Build(queue, request);
    ASSERT_TRUE(completion);
    if (!completion)
        return;
    ASSERT_EQ(completion->status(), FarVolumeBuildStatus::Ready);
    const auto bytes = Bytes(completion->mesh());
    const auto cpu = Observe(memory, 1).bytes_cpu;
    std::size_t deletions = 0;
    FarVolumeUploadBackend backend = [&](const FarVolumeRenderMesh&) {
        return std::make_unique<FakeGpu>(bytes, [&] {
            ++deletions;
            EXPECT_GE(Observe(memory, 1).bytes_gpu, bytes);
        });
    };
    FarVolumeUploadedTile output;
    ASSERT_TRUE(UploadFarVolume(
        *completion, request.identity, request.intent, memory, bytes, backend, output));
    EXPECT_TRUE(IsFarTileCovering(output.Status(request.identity, request.intent)));
    EXPECT_EQ(Observe(memory, 1).bytes_total, cpu + bytes);
    const auto* old_resource = output.resource();
    FarVolumeUploadBackend throwing =
        [](const FarVolumeRenderMesh&) -> std::unique_ptr<FarVolumeGpuAllocation> {
        throw std::runtime_error("fixture backend refusal");
    };
    EXPECT_FALSE(UploadFarVolume(
        *completion, request.identity, request.intent, memory, bytes, throwing, output));
    EXPECT_EQ(output.resource(), old_resource);
    EXPECT_EQ(Observe(memory, 1).bytes_total, cpu + bytes);
    EXPECT_EQ(deletions, 0u);
    auto current = request.identity;
    auto intent = request.intent;
    FarVolumeUploadBackend changing_identity = [&](const FarVolumeRenderMesh&) {
        ++current.world_epoch;
        return std::make_unique<FakeGpu>(bytes);
    };
    FarVolumeUploadError error;
    EXPECT_FALSE(UploadFarVolume(
        *completion, current, intent, memory, bytes, changing_identity, output, &error));
    EXPECT_EQ(error, FarVolumeUploadError::Stale);
    EXPECT_EQ(output.resource(), old_resource);
    EXPECT_EQ(Observe(memory, 1).bytes_total, cpu + bytes);
    current = request.identity;
    FarVolumeUploadBackend growing_span = [&](const FarVolumeRenderMesh&) {
        intent.extra_span = FarVolumeCoverageSpan{-1024, 32};
        return std::make_unique<FakeGpu>(bytes);
    };
    EXPECT_FALSE(
        UploadFarVolume(*completion, current, intent, memory, bytes, growing_span, output, &error));
    EXPECT_EQ(error, FarVolumeUploadError::Stale);
    EXPECT_EQ(output.resource(), old_resource);
    EXPECT_EQ(Observe(memory, 1).bytes_total, cpu + bytes);
    FarVolumeUploadBackend null_backend = [](const FarVolumeRenderMesh&) {
        return std::unique_ptr<FarVolumeGpuAllocation>{};
    };
    EXPECT_FALSE(UploadFarVolume(*completion,
                                 request.identity,
                                 request.intent,
                                 memory,
                                 bytes,
                                 null_backend,
                                 output,
                                 &error));
    EXPECT_EQ(error, FarVolumeUploadError::BackendFailure);
    std::size_t partial_deletions = 0;
    FarVolumeUploadBackend wrong_size = [&](const FarVolumeRenderMesh&) {
        return std::make_unique<FakeGpu>(bytes + 1, [&] { ++partial_deletions; });
    };
    EXPECT_FALSE(UploadFarVolume(
        *completion, request.identity, request.intent, memory, bytes, wrong_size, output, &error));
    EXPECT_EQ(error, FarVolumeUploadError::BackendFailure);
    EXPECT_EQ(partial_deletions, 1u);
    FarVolumeUploadBackend partial_throw =
        [&](const FarVolumeRenderMesh&) -> std::unique_ptr<FarVolumeGpuAllocation> {
        auto partial = std::make_unique<FakeGpu>(bytes, [&] { ++partial_deletions; });
        throw std::runtime_error("fixture partial upload refusal");
    };
    EXPECT_FALSE(UploadFarVolume(*completion,
                                 request.identity,
                                 request.intent,
                                 memory,
                                 bytes,
                                 partial_throw,
                                 output,
                                 &error));
    EXPECT_EQ(partial_deletions, 2u);
    EXPECT_EQ(output.resource(), old_resource);
    EXPECT_EQ(Observe(memory, 1).bytes_total, cpu + bytes);
    ASSERT_TRUE(UploadFarVolume(
        *completion, request.identity, request.intent, memory, bytes, backend, output));
    EXPECT_EQ(deletions, 1u);
    EXPECT_GE(Observe(memory, 1).peak_total, cpu + bytes * 2);
    const FarVolumeUploadedTile& moved_from_output = output;
    FarVolumeUploadedTile moved(std::move(output));
    EXPECT_FALSE(IsFarTileCovering(moved_from_output.Status(request.identity, request.intent)));
    EXPECT_TRUE(IsFarTileCovering(moved.Status(request.identity, request.intent)));
    moved = FarVolumeUploadedTile();
    EXPECT_EQ(deletions, 2u);
    EXPECT_EQ(Observe(memory, 1).bytes_gpu, 0u);
    EXPECT_EQ(Observe(memory, 1).bytes_total, cpu);
}

TEST(FarVolumeUpload, CurrentEmptyCoversWithoutGpuAndSpanGrowthMakesItIneligible) {
    JobSystem jobs;
    jobs.startup(1);
    FarTierBudgets budgets;
    budgets.bytes.fill(64 * 1024 * 1024);
    FarVolumeTierMemory memory(budgets);
    FarVolumeBuildQueue queue(jobs, *memory.Pool(1), UploadLimits());
    const auto request = UploadRequest(false);
    auto completion = Build(queue, request);
    ASSERT_TRUE(completion);
    if (!completion)
        return;
    ASSERT_EQ(completion->status(), FarVolumeBuildStatus::ReadyEmpty);
    FarVolumeUploadedTile output;
    ASSERT_TRUE(
        UploadFarVolume(*completion, request.identity, request.intent, memory, 0, {}, output));
    EXPECT_EQ(output.resource(), nullptr);
    EXPECT_EQ(output.gpu_bytes(), 0u);
    EXPECT_TRUE(IsFarTileCovering(output.Status(request.identity, request.intent)));
    auto extended = request.intent;
    extended.extra_span = FarVolumeCoverageSpan{-1024, 32};
    EXPECT_FALSE(IsFarTileCovering(output.Status(request.identity, extended)));
    extended.extra_span = FarVolumeCoverageSpan{std::numeric_limits<float>::quiet_NaN(), 32};
    EXPECT_FALSE(IsFarTileCovering(output.Status(request.identity, extended)));
    auto changed = request.identity;
    ++changed.world_epoch;
    EXPECT_FALSE(IsFarTileCovering(output.Status(changed, request.intent)));
}
