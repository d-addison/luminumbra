#include "luminumbra_common/world/FarVolumeMemory.h"
#include "luminumbra_common/world/FarVolumeMesher.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::World;

constexpr FarVolumeBuildLimits memBuildLimits{1048576, 1048576};
constexpr FarVolumeMeshLimits memMeshLimits{2000000, 4000000, 1000000, 4000000};
constexpr std::uint64_t memIdentity = 731;
constexpr std::uint64_t memMax = std::numeric_limits<std::uint64_t>::max();

FarVolumeSample memPlane(const FarVolumePosition& p) {
    return {static_cast<float>(p.y) + 2.0f, p.y < -2 ? std::uint8_t{255} : std::uint8_t{19}};
}

// One xz column of plane density inside air. A tile over this field keeps only the
// four bricks that touch the column, so mesh refusal sweeps stay small.
FarVolumeSample memSparse(const FarVolumePosition& p) {
    if (p.x >= 0 && p.x <= 16 && p.z >= 0 && p.z <= 16)
        return {static_cast<float>(p.y) + 2.0f, std::uint8_t{19}};
    return {3.0f, std::uint8_t{7}};
}

// Upstream spy: counts requests that reach the system allocator and tracks what is
// outstanding through this resource.
class MemSpy final : public std::pmr::memory_resource {
public:
    std::uint64_t allocations = 0;
    std::uint64_t outstanding = 0;
    std::uint64_t peak = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        outstanding += bytes;
        if (outstanding > peak)
            peak = outstanding;
        return pointer;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        outstanding -= bytes;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

FarVolumeTile memBuild(const FarVolumeRequest& request, const FarVolumeSampler& sampler) {
    FarVolumeTile tile;
    EXPECT_TRUE(BuildFarVolumeTile(request, memBuildLimits, sampler, tile));
    return tile;
}

bool memMeshInto(const FarVolumeTile& tile,
                 const FarVolumeSampler& sampler,
                 const FarVolumeMemory& memory,
                 FarVolumeMesh& output,
                 FarVolumeMeshError& error) {
    return MeshFarVolumeTileCharged(
        tile, memIdentity, sampler, memMeshLimits, memory, output, &error);
}
} // namespace

TEST(FarVolumeMemory, PoolAdmitsUntilLimitAndNeverOverflows) {
    FarVolumeMemoryPool pool(100);
    EXPECT_TRUE(pool.TryReserve(60));
    EXPECT_FALSE(pool.TryReserve(41));
    EXPECT_EQ(pool.used(), 60u);
    EXPECT_EQ(pool.peak(), 60u);
    EXPECT_FALSE(pool.TryReserve(memMax));
    pool.Release(1000);
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(pool.peak(), 60u);
    EXPECT_EQ(pool.refusals(), 2u);
    EXPECT_EQ(pool.attempts(), 3u);
    EXPECT_TRUE(pool.TryReserve(0));
    EXPECT_EQ(pool.attempts(), 3u);
    EXPECT_EQ(pool.refusals(), 2u);
    EXPECT_EQ(pool.used(), 0u);
}

TEST(FarVolumeMemory, LeaseMoveAssignAdoptsThenReleasesOld) {
    FarVolumeMemoryPool pool(1000);
    FarVolumeMemoryLease a(pool), b(pool);
    ASSERT_TRUE(a.TryCharge(100));
    ASSERT_TRUE(b.TryCharge(50));
    EXPECT_EQ(pool.used(), 150u);
    const FarVolumeMemoryLease& moved_from_a = a; // Observed through a view after the move.
    b = std::move(a);
    EXPECT_EQ(b.bytes(), 100u);
    EXPECT_EQ(moved_from_a.bytes(), 0u);
    EXPECT_EQ(pool.used(), 100u);

    FarVolumeMemoryLease c(pool);
    ASSERT_TRUE(c.TryCharge(30));
    const FarVolumeMemoryLease& absorbed_c = c;
    EXPECT_TRUE(b.Absorb(std::move(c)));
    EXPECT_EQ(b.bytes(), 130u);
    EXPECT_EQ(absorbed_c.bytes(), 0u);
    EXPECT_EQ(pool.used(), 130u);

    FarVolumeMemoryPool other(1000);
    FarVolumeMemoryLease d(other);
    ASSERT_TRUE(d.TryCharge(10));
    const FarVolumeMemoryLease& refused_d = d;
    EXPECT_FALSE(b.Absorb(std::move(d)));
    EXPECT_EQ(refused_d.bytes(), 10u);
    EXPECT_EQ(b.bytes(), 130u);

    {
        FarVolumeMemoryLease e(pool);
        ASSERT_TRUE(e.TryCharge(20));
        EXPECT_EQ(pool.used(), 150u);
    }
    EXPECT_EQ(pool.used(), 130u);
}

TEST(FarVolumeMemory, ChargedResourceRefusalNeverCallsUpstream) {
    FarVolumeMemoryPool pool(64);
    FarVolumeMemoryLease lease(pool);
    MemSpy spy;
    FarVolumeChargedResource resource(lease, &spy);
    void* first = resource.allocate(32, alignof(std::max_align_t));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(lease.bytes(), 32u);

    bool refused = false;
    try {
        (void)resource.allocate(64, alignof(std::max_align_t));
    } catch (const std::bad_alloc&) {
        refused = true;
    }
    EXPECT_TRUE(refused);
    EXPECT_EQ(spy.allocations, 1u);
    EXPECT_EQ(pool.used(), 32u);

    resource.deallocate(first, 32, alignof(std::max_align_t));
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(spy.outstanding, 0u);
}

TEST(FarVolumeMemory, ReserveChargedRefusalLeavesVectorAndLeaseUnchanged) {
    FarVolumeMemoryPool pool(1 << 20);
    FarVolumeMemoryLease lease(pool);
    std::vector<std::uint64_t> values;

    pool.RefuseAttemptsFrom(0);
    EXPECT_FALSE(FarVolumeReserveCharged(values, 1, 1000, &lease));
    EXPECT_EQ(values.capacity(), 0u);
    EXPECT_EQ(lease.bytes(), 0u);
    EXPECT_EQ(pool.used(), 0u);

    pool.RefuseAttemptsFrom(memMax);
    ASSERT_TRUE(FarVolumeReserveCharged(values, 1, 1000, &lease));
    EXPECT_GE(values.capacity(), 1u);
    EXPECT_EQ(lease.bytes(), values.capacity() * sizeof(std::uint64_t));
    EXPECT_EQ(pool.used(), lease.bytes());
}

TEST(FarVolumeMemory, TileBoundedEqualsUnboundedAndChargesExactCapacity) {
    const FarVolumeRequest request{{1, 0, 0}, {-16, 0}, memIdentity};
    FarVolumeTile unbounded;
    FarVolumeBuildError error = FarVolumeBuildError::AllocationFailure;
    ASSERT_TRUE(BuildFarVolumeTile(request, memBuildLimits, memPlane, unbounded, &error));

    FarVolumeMemoryPool pool(1ULL << 30);
    FarVolumeMemoryLease retained(pool);
    const FarVolumeMemory memory{&pool, &retained, nullptr};
    FarVolumeTile bounded;
    ASSERT_TRUE(
        BuildFarVolumeTileCharged(request, memBuildLimits, memory, memPlane, bounded, &error));
    EXPECT_EQ(bounded, unbounded);
    EXPECT_EQ(retained.bytes(), FarVolumeTileRetainedBytes(bounded));
    EXPECT_EQ(pool.used(), retained.bytes());
}

TEST(FarVolumeMemory, TileRefusalSweepPreservesPreviousOutput) {
    const FarVolumeRequest request{{1, 0, 0}, {-16, 0}, memIdentity};
    FarVolumeBuildError error = FarVolumeBuildError::AllocationFailure;

    std::uint64_t total = 0;
    {
        FarVolumeMemoryPool probe(1ULL << 30);
        FarVolumeMemoryLease lease(probe);
        const FarVolumeMemory memory{&probe, &lease, nullptr};
        FarVolumeTile tile;
        ASSERT_TRUE(
            BuildFarVolumeTileCharged(request, memBuildLimits, memory, memPlane, tile, &error));
        total = probe.attempts();
    }
    ASSERT_GT(total, 0u);

    for (std::uint64_t k = 0; k < total; ++k) {
        FarVolumeMemoryPool pool(1ULL << 30);
        FarVolumeMemoryLease retained(pool);
        const FarVolumeMemory memory{&pool, &retained, nullptr};
        FarVolumeTile previous;
        ASSERT_TRUE(
            BuildFarVolumeTileCharged(request, memBuildLimits, memory, memPlane, previous, &error));
        const FarVolumeTile expected = previous;
        const auto baseline = pool.used();
        const auto retained_before = retained.bytes();

        pool.RefuseAttemptsFrom(pool.attempts() + k);
        EXPECT_FALSE(
            BuildFarVolumeTileCharged(request, memBuildLimits, memory, memPlane, previous, &error))
            << "attempt offset " << k;
        EXPECT_EQ(error, FarVolumeBuildError::AllocationFailure) << "attempt offset " << k;
        EXPECT_EQ(previous, expected) << "attempt offset " << k;
        EXPECT_EQ(retained.bytes(), retained_before) << "attempt offset " << k;
        EXPECT_EQ(pool.used(), baseline) << "attempt offset " << k;
        if (::testing::Test::HasFailure())
            return;
    }
}

TEST(FarVolumeMemory, TileInvalidMemoryBindingsFailBeforeFirstSample) {
    const FarVolumeRequest request{{1, 0, 0}, {-16, 0}, memIdentity};
    FarVolumeBuildError error = FarVolumeBuildError::None;
    FarVolumeTile output = memBuild(request, memPlane);
    const FarVolumeTile expected = output;

    int calls = 0;
    const FarVolumeSampler counting =
        [&calls](const FarVolumePosition& p) -> std::optional<FarVolumeSample> {
        ++calls;
        return memPlane(p);
    };

    FarVolumeMemoryPool pool(1ULL << 30);
    const FarVolumeMemory unbound_retained{&pool, nullptr, nullptr};
    EXPECT_FALSE(BuildFarVolumeTileCharged(
        request, memBuildLimits, unbound_retained, counting, output, &error));
    EXPECT_EQ(error, FarVolumeBuildError::InvalidMemory);
    EXPECT_EQ(output, expected);

    FarVolumeMemoryPool other(1ULL << 30);
    FarVolumeMemoryLease foreign(other);
    const FarVolumeMemory foreign_retained{&pool, &foreign, nullptr};
    EXPECT_FALSE(BuildFarVolumeTileCharged(
        request, memBuildLimits, foreign_retained, counting, output, &error));
    EXPECT_EQ(error, FarVolumeBuildError::InvalidMemory);
    EXPECT_EQ(output, expected);

    EXPECT_EQ(calls, 0);
    EXPECT_EQ(pool.attempts(), 0u);
    EXPECT_EQ(pool.used(), 0u);
}

TEST(FarVolumeMemory, MeshBoundedEqualsUnboundedAndPeakBoundary) {
    const FarVolumeTile tile = memBuild({{1, -1, -1}, {-16, 0}, memIdentity}, memPlane);
    const FarVolumeSampler sampler = memPlane;
    FarVolumeMeshError error = FarVolumeMeshError::AllocationFailure;

    FarVolumeMesh unbounded;
    ASSERT_TRUE(memMeshInto(tile, sampler, FarVolumeMemory{}, unbounded, error));

    FarVolumeMemoryPool pool(1ULL << 30);
    FarVolumeMemoryLease retained(pool);
    const FarVolumeMemory memory{&pool, &retained, nullptr};
    FarVolumeMesh bounded;
    ASSERT_TRUE(memMeshInto(tile, sampler, memory, bounded, error));
    EXPECT_EQ(bounded, unbounded);
    EXPECT_EQ(retained.bytes(), FarVolumeMeshRetainedBytes(bounded));
    EXPECT_EQ(pool.used(), retained.bytes());

    const std::uint64_t peak = pool.peak();
    ASSERT_GT(peak, 0u);
    {
        FarVolumeMemoryPool exact(peak);
        FarVolumeMemoryLease lease(exact);
        const FarVolumeMemory exact_memory{&exact, &lease, nullptr};
        FarVolumeMesh out;
        EXPECT_TRUE(memMeshInto(tile, sampler, exact_memory, out, error));
        EXPECT_EQ(out, unbounded);
    }
    {
        FarVolumeMemoryPool tight(peak - 1);
        FarVolumeMemoryLease lease(tight);
        const FarVolumeMemory tight_memory{&tight, &lease, nullptr};
        FarVolumeMesh out;
        EXPECT_FALSE(memMeshInto(tile, sampler, tight_memory, out, error));
        EXPECT_EQ(error, FarVolumeMeshError::AllocationFailure);
        EXPECT_EQ(out, FarVolumeMesh{});
        EXPECT_EQ(tight.used(), 0u);
        EXPECT_EQ(lease.bytes(), 0u);
    }
}

TEST(FarVolumeMemory, MeshRefusalSweepPreservesPreviousOutput) {
    const FarVolumeTile tile = memBuild({{1, 0, 0}, {-16, 0}, memIdentity}, memSparse);
    const FarVolumeSampler sampler = memSparse;
    FarVolumeMeshError error = FarVolumeMeshError::AllocationFailure;

    std::uint64_t total = 0;
    {
        FarVolumeMemoryPool probe(1ULL << 30);
        FarVolumeMemoryLease lease(probe);
        const FarVolumeMemory memory{&probe, &lease, nullptr};
        FarVolumeMesh mesh;
        ASSERT_TRUE(memMeshInto(tile, sampler, memory, mesh, error));
        total = probe.attempts();
    }
    ASSERT_GT(total, 0u);

    for (std::uint64_t k = 0; k < total; ++k) {
        FarVolumeMemoryPool pool(1ULL << 30);
        FarVolumeMemoryLease retained(pool);
        const FarVolumeMemory plain{&pool, &retained, nullptr};
        FarVolumeMesh previous;
        ASSERT_TRUE(memMeshInto(tile, sampler, plain, previous, error));
        const FarVolumeMesh expected = previous;
        const auto baseline = pool.used();
        const auto retained_before = retained.bytes();

        MemSpy spy;
        const FarVolumeMemory memory{&pool, &retained, &spy};
        pool.RefuseAttemptsFrom(pool.attempts() + k);
        EXPECT_FALSE(memMeshInto(tile, sampler, memory, previous, error)) << "attempt offset " << k;
        EXPECT_EQ(error, FarVolumeMeshError::AllocationFailure) << "attempt offset " << k;
        EXPECT_EQ(previous, expected) << "attempt offset " << k;
        EXPECT_EQ(retained.bytes(), retained_before) << "attempt offset " << k;
        EXPECT_EQ(pool.used(), baseline) << "attempt offset " << k;
        EXPECT_EQ(spy.outstanding, 0u) << "attempt offset " << k;
        EXPECT_LE(spy.peak, pool.peak()) << "attempt offset " << k;
        if (::testing::Test::HasFailure())
            return;
    }
}

TEST(FarVolumeMemory, CancelledSamplerLeavesPoolClean) {
    const FarVolumeTile tile = memBuild({{1, 0, 0}, {-16, 0}, memIdentity}, memSparse);
    FarVolumeMemoryPool pool(1ULL << 30);
    FarVolumeMemoryLease retained(pool);
    const FarVolumeMemory memory{&pool, &retained, nullptr};

    int calls = 0;
    const FarVolumeSampler cancelling =
        [&calls](const FarVolumePosition& p) -> std::optional<FarVolumeSample> {
        if (++calls == 50)
            return std::nullopt;
        return memSparse(p);
    };
    FarVolumeMesh output;
    FarVolumeMeshError error = FarVolumeMeshError::None;
    EXPECT_FALSE(MeshFarVolumeTileCharged(
        tile, memIdentity, cancelling, memMeshLimits, memory, output, &error));
    EXPECT_EQ(error, FarVolumeMeshError::SamplerFailure);
    EXPECT_EQ(calls, 50);
    EXPECT_EQ(output, FarVolumeMesh{});
    EXPECT_EQ(retained.bytes(), 0u);
    EXPECT_EQ(pool.used(), 0u);
}
