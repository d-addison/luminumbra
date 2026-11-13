#include "luminumbra_common/world/FarVolume.h"
#include "luminumbra_common/world/FarVolumeMesher.h"
#include "luminumbra_common/world/FarVolumeTile.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vector>

namespace {
using namespace Luminumbra::World;

constexpr std::uint64_t kMemLargeLimit = std::uint64_t{1} << 40;
constexpr std::uint64_t kMemNoFailure = std::numeric_limits<std::uint64_t>::max();

FarVolumeCompilationLimits MemLimits() {
    // Explicit analytic-fixture bounds; not production queue/storage targets.
    return {{131 * 131, 20'000'000}, {131'072, 16'384}, {1'048'576, 3'000'000, 300'000, 1'000'000}};
}

FarVolumeAuthority MemPlane(FarVolumeNumericProfile profile) {
    return {42,
            FarCaveMode::BandLimited,
            profile,
            [](std::int64_t, std::int64_t) { return 0.0f; },
            [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
                return FarVolumeSample{static_cast<float>(p.y) + .25f, 7};
            }};
}

FarVolumeCoverageIntent MemIntentA() {
    return {{1, 0, 0}, {}};
}

FarVolumeCoverageIntent MemIntentB() {
    return {{1, 1, 0}, {}};
}

FarVolumeRequest MemHeld() {
    return {{3, 6, 7}, {-64, 64}, 91};
}

// Result of a clean run on a fresh pool with no prior output.
struct MemMeasure {
    FarVolumeCompilation compilation;
    std::uint64_t peak = 0;
    std::uint64_t retained = 0;
    std::uint64_t attempts = 0;
};

MemMeasure MemMeasureRun(const FarVolumeCoverageIntent& intent,
                         const FarVolumeAuthority& authority) {
    MemMeasure measure;
    FarVolumeMemoryPool pool(kMemLargeLimit);
    FarVolumeMemoryLease lease(pool);
    const bool ok = CompileFarVolumeCharged(intent,
                                            authority,
                                            MemLimits(),
                                            FarVolumeMemory{&pool, &lease, nullptr},
                                            measure.compilation);
    EXPECT_TRUE(ok);
    measure.peak = pool.peak();
    measure.retained = lease.bytes();
    measure.attempts = pool.attempts();
    return measure;
}

// Every attempt below 50, then a stride giving about 100 runs for long sweeps.
std::vector<std::uint64_t> MemSweepIndices(std::uint64_t attempts) {
    std::vector<std::uint64_t> indices;
    if (attempts <= 400) {
        for (std::uint64_t k = 0; k < attempts; ++k)
            indices.push_back(k);
        return indices;
    }
    for (std::uint64_t k = 0; k < 50; ++k)
        indices.push_back(k);
    const std::uint64_t step = (attempts - 50) / 50;
    for (std::uint64_t k = 50; k < attempts; k += step)
        indices.push_back(k);
    if (indices.back() != attempts - 1)
        indices.push_back(attempts - 1);
    return indices;
}

void MemChargedEqualsUnbounded(FarVolumeNumericProfile profile) {
    const auto authority = MemPlane(profile);
    FarVolumeCompilation expected;
    ASSERT_TRUE(CompileFarVolume(MemIntentA(), authority, MemLimits(), expected));
    FarVolumeMemoryPool pool(kMemLargeLimit);
    FarVolumeMemoryLease retained(pool);
    FarVolumeCompilation actual;
    FarVolumeFacadeFailure failure;
    ASSERT_TRUE(CompileFarVolumeCharged(MemIntentA(),
                                        authority,
                                        MemLimits(),
                                        FarVolumeMemory{&pool, &retained, nullptr},
                                        actual,
                                        &failure));
    EXPECT_TRUE(actual == expected);
    EXPECT_GT(retained.bytes(), 0u);
    EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(actual));
    EXPECT_EQ(pool.used(), retained.bytes());
}

// Refusal is about ownership/unwinding rather than full-tile mesh throughput.
// V5 still has the canonical 128x128 horizontal lattice, height discovery/cache,
// checksum, generation, gradient halo and mixed-mesh pipeline. Its 256m default
// band plus inclusive top plane is two V5 bricks tall; two small columns retain a mesh
// in each of two distinct tiles. Sweeping full V1 planes repeats millions of
// samples/large meshes ~100 times per profile and exceeded ASan's 300s limit.
FarVolumeAuthority MemSweepAuthority(FarVolumeNumericProfile profile) {
    return {
        42,
        FarCaveMode::BandLimited,
        profile,
        [](std::int64_t, std::int64_t) { return 0.0f; },
        [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
            const bool column_x = (p.x >= 0 && p.x <= 256) || (p.x >= 8192 && p.x <= 8192 + 256);
            const bool column_z = p.z >= 0 && p.z <= 256;
            return FarVolumeSample{column_x && column_z ? static_cast<float>(p.y) + .25f : 3.0f, 7};
        }};
}

void MemRefusalSweep(FarVolumeNumericProfile profile) {
    const auto authority = MemSweepAuthority(profile);
    const FarVolumeCoverageIntent intent_a{{5, 0, 0}, {}};
    const FarVolumeCoverageIntent intent_b{{5, 1, 0}, {}};
    const auto clean = MemMeasureRun(intent_a, authority);
    ASSERT_GT(clean.attempts, 0u);
    ASSERT_EQ(clean.compilation.mesh.content, FarVolumeContent::Mixed);
    ASSERT_FALSE(clean.compilation.mesh.vertices.empty());
    ASSERT_FALSE(clean.compilation.mesh.indices.empty());

    FarVolumeMemoryPool pool(kMemLargeLimit);
    FarVolumeMemoryLease retained(pool);
    FarVolumeCompilation previous;
    ASSERT_TRUE(CompileFarVolumeCharged(
        intent_b, authority, MemLimits(), FarVolumeMemory{&pool, &retained, nullptr}, previous));
    const FarVolumeCompilation saved = previous;
    const std::uint64_t held_bytes = retained.bytes();
    const std::uint64_t baseline_used = pool.used();
    // `previous` stays paired with `retained`; `saved` is a value copy for comparison only
    // (a copy need not preserve vector capacity).
    ASSERT_EQ(held_bytes, FarVolumeCompilationRetainedBytes(previous));

    for (const auto k : MemSweepIndices(clean.attempts)) {
        SCOPED_TRACE(static_cast<unsigned long long>(k));
        // Relative to the current counter: refused runs also advance it.
        pool.RefuseAttemptsFrom(pool.attempts() + k);
        FarVolumeFacadeFailure failure;
        EXPECT_FALSE(CompileFarVolumeCharged(intent_a,
                                             authority,
                                             MemLimits(),
                                             FarVolumeMemory{&pool, &retained, nullptr},
                                             previous,
                                             &failure));
        EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
        EXPECT_TRUE(previous == saved);
        EXPECT_EQ(retained.bytes(), held_bytes);
        EXPECT_EQ(pool.used(), baseline_used);
    }

    pool.RefuseAttemptsFrom(kMemNoFailure);
    ASSERT_TRUE(CompileFarVolumeCharged(
        intent_a, authority, MemLimits(), FarVolumeMemory{&pool, &retained, nullptr}, previous));
    EXPECT_TRUE(previous == clean.compilation);
    EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(previous));
    EXPECT_EQ(pool.used(), retained.bytes());
}

void MemPeakBoundary(FarVolumeNumericProfile profile) {
    const auto authority = MemPlane(profile);
    const auto clean = MemMeasureRun(MemIntentA(), authority);
    const std::uint64_t peak = clean.peak;
    ASSERT_GT(peak, 0u);
    {
        FarVolumeMemoryPool pool(peak);
        FarVolumeMemoryLease retained(pool);
        FarVolumeCompilation output;
        ASSERT_TRUE(CompileFarVolumeCharged(MemIntentA(),
                                            authority,
                                            MemLimits(),
                                            FarVolumeMemory{&pool, &retained, nullptr},
                                            output));
        EXPECT_TRUE(output == clean.compilation);
        EXPECT_EQ(pool.used(), retained.bytes());
    }
    {
        FarVolumeMemoryPool pool(peak - 1);
        FarVolumeMemoryLease retained(pool);
        FarVolumeCompilation output;
        FarVolumeFacadeFailure failure;
        EXPECT_FALSE(CompileFarVolumeCharged(MemIntentA(),
                                             authority,
                                             MemLimits(),
                                             FarVolumeMemory{&pool, &retained, nullptr},
                                             output,
                                             &failure));
        EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
        EXPECT_EQ(pool.used(), 0u);
        EXPECT_EQ(retained.bytes(), 0u);
        EXPECT_TRUE(output == FarVolumeCompilation{});
    }
}

} // namespace

TEST(FarVolumeFacadeMemory, ChargedCompileEqualsUnboundedAndAccountsRetainedBytes) {
    for (const auto profile :
         {FarVolumeNumericProfile::RawFloat, FarVolumeNumericProfile::LegacySigned16}) {
        SCOPED_TRACE(static_cast<int>(profile));
        MemChargedEqualsUnbounded(profile);
    }
}

TEST(FarVolumeFacadeMemory, RefusalSweepPreservesPreviousCompilation) {
    for (const auto profile :
         {FarVolumeNumericProfile::RawFloat, FarVolumeNumericProfile::LegacySigned16}) {
        SCOPED_TRACE(static_cast<int>(profile));
        MemRefusalSweep(profile);
    }
}

TEST(FarVolumeFacadeMemory, PeakBoundaryAdmitsPeakAndRefusesOneByteLess) {
    for (const auto profile :
         {FarVolumeNumericProfile::RawFloat, FarVolumeNumericProfile::LegacySigned16}) {
        SCOPED_TRACE(static_cast<int>(profile));
        MemPeakBoundary(profile);
    }
}

TEST(FarVolumeFacadeMemory, InvalidMemoryBindingsReportInvalidMemory) {
    auto authority = MemPlane(FarVolumeNumericProfile::RawFloat);
    std::uint64_t calls = 0;
    authority.height = [&calls](std::int64_t, std::int64_t) {
        ++calls;
        return 0.0f;
    };
    authority.density = [&calls](const FarVolumePosition& p,
                                 float,
                                 std::uint32_t,
                                 FarCaveMode) -> std::optional<FarVolumeSample> {
        ++calls;
        return FarVolumeSample{static_cast<float>(p.y) + .25f, 7};
    };
    FarVolumeMemoryPool pool(kMemLargeLimit);
    FarVolumeMemoryPool other(kMemLargeLimit);
    FarVolumeMemoryLease foreign(other);
    const std::vector<FarVolumeMemory> bindings{{&pool, nullptr, nullptr},
                                                {&pool, &foreign, nullptr}};
    for (const auto& memory : bindings) {
        FarVolumeCompilation output;
        FarVolumeFacadeFailure failure;
        EXPECT_FALSE(CompileFarVolumeCharged(
            MemIntentA(), authority, MemLimits(), memory, output, &failure));
        EXPECT_EQ(failure.code, FarVolumeFacadeError::InvalidMemory);
        EXPECT_TRUE(output == FarVolumeCompilation{});
        EXPECT_EQ(failure.work.height_samples, 0u);
    }
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(other.used(), 0u);
}

TEST(FarVolumeFacadeMemory, ResolveChargedHeightCacheRefusal) {
    const auto authority = MemPlane(FarVolumeNumericProfile::RawFloat);
    const FarVolumeAuthorityLimits limits = MemLimits().authority;
    const FarVolumeRequest held = MemHeld();

    FarVolumeMemoryPool small(1000);
    FarVolumeRequest output = held;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(ResolveFarVolumeCoverageCharged(MemIntentA(),
                                                 authority,
                                                 limits,
                                                 FarVolumeMemory{&small, nullptr, nullptr},
                                                 output,
                                                 &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    EXPECT_TRUE(output == held);
    EXPECT_EQ(small.used(), 0u);

    FarVolumeRequest expected;
    ASSERT_TRUE(ResolveFarVolumeCoverage(MemIntentA(), authority, limits, expected));
    FarVolumeMemoryPool large(kMemLargeLimit);
    FarVolumeRequest charged = held;
    ASSERT_TRUE(ResolveFarVolumeCoverageCharged(
        MemIntentA(), authority, limits, FarVolumeMemory{&large, nullptr, nullptr}, charged));
    EXPECT_TRUE(charged == expected);
    EXPECT_EQ(large.used(), 0u);
}

TEST(FarVolumeFacadeMemory, OverlappingJobsShareOnePool) {
    const auto authority = MemPlane(FarVolumeNumericProfile::RawFloat);
    const auto job1 = MemMeasureRun(MemIntentA(), authority);
    const auto job2 = MemMeasureRun(MemIntentB(), authority);
    ASSERT_GT(job1.retained, 0u);
    // The plane is translation invariant, so job2 has the same peak as job1; the
    // refusal below relies on that (job2 peak >= job1 peak).
    EXPECT_GE(job2.peak, job1.peak);
    const std::uint64_t limit = job1.peak + job1.retained - 1;

    FarVolumeMemoryPool pool(limit);
    FarVolumeMemoryLease lease1(pool);
    FarVolumeMemoryLease lease2(pool);
    FarVolumeCompilation out1;
    ASSERT_TRUE(CompileFarVolumeCharged(
        MemIntentA(), authority, MemLimits(), FarVolumeMemory{&pool, &lease1, nullptr}, out1));
    EXPECT_TRUE(out1 == job1.compilation);
    EXPECT_EQ(lease1.bytes(), job1.retained);

    FarVolumeCompilation out2;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompileFarVolumeCharged(MemIntentB(),
                                         authority,
                                         MemLimits(),
                                         FarVolumeMemory{&pool, &lease2, nullptr},
                                         out2,
                                         &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    EXPECT_TRUE(out2 == FarVolumeCompilation{});
    EXPECT_EQ(lease2.bytes(), 0u);
    EXPECT_TRUE(out1 == job1.compilation);
    EXPECT_EQ(lease1.bytes(), job1.retained);
    EXPECT_EQ(pool.used(), job1.retained);

    out1 = FarVolumeCompilation{}; // free the buffers before releasing their charge
    lease1.Reset();
    EXPECT_EQ(pool.used(), 0u);
    ASSERT_TRUE(CompileFarVolumeCharged(
        MemIntentB(), authority, MemLimits(), FarVolumeMemory{&pool, &lease2, nullptr}, out2));
    EXPECT_TRUE(out2 == job2.compilation);
    EXPECT_EQ(lease2.bytes(), job2.retained);
    EXPECT_EQ(pool.used(), job2.retained);
}

TEST(FarVolumeFacadeMemory, CancelledAuthorityLeavesPoolClean) {
    FarVolumeMemoryPool pool(kMemLargeLimit);
    FarVolumeMemoryLease prior(pool);
    FarVolumeCompilation prior_output;
    ASSERT_TRUE(CompileFarVolumeCharged(MemIntentB(),
                                        MemPlane(FarVolumeNumericProfile::RawFloat),
                                        MemLimits(),
                                        FarVolumeMemory{&pool, &prior, nullptr},
                                        prior_output));
    const std::uint64_t baseline = pool.used();

    auto cancelled = MemPlane(FarVolumeNumericProfile::RawFloat);
    std::uint64_t calls = 0;
    cancelled.density = [&calls](const FarVolumePosition& p,
                                 float,
                                 std::uint32_t,
                                 FarCaveMode) -> std::optional<FarVolumeSample> {
        if (++calls > 500)
            return std::nullopt;
        return FarVolumeSample{static_cast<float>(p.y) + .25f, 7};
    };
    FarVolumeMemoryLease lease(pool);
    FarVolumeCompilation output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompileFarVolumeCharged(MemIntentA(),
                                         cancelled,
                                         MemLimits(),
                                         FarVolumeMemory{&pool, &lease, nullptr},
                                         output,
                                         &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::SamplerFailure);
    EXPECT_EQ(pool.used(), baseline);
    EXPECT_EQ(lease.bytes(), 0u);
    EXPECT_EQ(prior.bytes(), FarVolumeCompilationRetainedBytes(prior_output));
    EXPECT_TRUE(output == FarVolumeCompilation{});
}

TEST(FarVolumeFacadeMemory, LegacyChecksumChargedRefusalReturnsFalseWithoutChangingOutput) {
    const auto authority = MemPlane(FarVolumeNumericProfile::LegacySigned16);
    FarVolumeCompilation compiled;
    ASSERT_TRUE(CompileFarVolume(MemIntentA(), authority, MemLimits(), compiled));
    std::uint32_t expected = 0;
    ASSERT_TRUE(FarVolumeLegacyChecksum(compiled.tile, FarCaveMode::BandLimited, expected));
    ASSERT_TRUE(compiled.legacy_checksum.has_value());
    EXPECT_EQ(expected, compiled.legacy_checksum.value_or(0u));

    FarVolumeMemoryPool refusing(kMemLargeLimit);
    refusing.RefuseAttemptsFrom(refusing.attempts());
    std::uint32_t output = 0xDEADBEEFu;
    EXPECT_FALSE(FarVolumeLegacyChecksumCharged(compiled.tile,
                                                FarCaveMode::BandLimited,
                                                FarVolumeMemory{&refusing, nullptr, nullptr},
                                                output));
    EXPECT_EQ(output, 0xDEADBEEFu);
    EXPECT_EQ(refusing.used(), 0u);

    FarVolumeMemoryPool large(kMemLargeLimit);
    std::uint32_t charged = 0;
    EXPECT_TRUE(FarVolumeLegacyChecksumCharged(compiled.tile,
                                               FarCaveMode::BandLimited,
                                               FarVolumeMemory{&large, nullptr, nullptr},
                                               charged));
    EXPECT_EQ(charged, expected);
    EXPECT_EQ(large.used(), 0u);
}

namespace {
FarVolumeAuthority SpanSparseAuthority(FarVolumeNumericProfile profile) {
    auto authority = MemPlane(profile);
    authority.density = [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{p.x == 0 && p.y == 0 && p.z == 0 ? -1.0f : 1.0f, 7};
    };
    return authority;
}
FarVolumeCompilationLimits SpanSparseLimits() {
    // One candidate layer, one mixed brick and one triangle per page. This keeps
    // exhaustive allocation refusal bounded even under instrumented execution.
    return {{131 * 131, 130'000}, {1024, 1}, {64, 1000, 3, 3}};
}

TEST(FarVolumeFacadeMemory, ExplicitSpanEveryAllocationRefusalPreservesOutputAndReservation) {
    const std::array<FarVolumeRequest, 2> pages{
        {{{5, 0, 0}, {-256, 0}, 42}, {{5, 0, 0}, {0, 256}, 42}}};
    for (const auto profile :
         {FarVolumeNumericProfile::RawFloat, FarVolumeNumericProfile::LegacySigned16}) {
        const auto authority = SpanSparseAuthority(profile);
        for (const auto& request : pages) {
            FarVolumeMemoryPool pool(kMemLargeLimit);
            FarVolumeMemoryLease retained(pool);
            FarVolumeCompilation output;
            ASSERT_TRUE(CompileFarVolumeSpanCharged(
                request, authority, SpanSparseLimits(), {&pool, &retained, nullptr}, output));
            const auto attempts = pool.attempts();
            ASSERT_GT(attempts, 0u);
            ASSERT_LT(attempts, 1000u);
            ASSERT_FALSE(output.mesh.indices.empty());
            const auto saved = output;
            const auto held = retained.bytes();
            // Refuse every successful-run admission in turn, including height,
            // retained buffers, all map nodes and checksum scratch.
            for (std::uint64_t allocation = 0; allocation < attempts; ++allocation) {
                SCOPED_TRACE(allocation);
                pool.RefuseAttemptsFrom(pool.attempts() + allocation);
                FarVolumeFacadeFailure failure;
                EXPECT_FALSE(CompileFarVolumeSpanCharged(request,
                                                         authority,
                                                         SpanSparseLimits(),
                                                         {&pool, &retained, nullptr},
                                                         output,
                                                         &failure));
                EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
                EXPECT_EQ(output, saved);
                EXPECT_EQ(retained.bytes(), held);
                EXPECT_EQ(pool.used(), held);
            }
            pool.RefuseAttemptsFrom(kMemNoFailure);
            ASSERT_TRUE(CompileFarVolumeSpanCharged(
                request, authority, SpanSparseLimits(), {&pool, &retained, nullptr}, output));
            EXPECT_EQ(output, saved);
            EXPECT_EQ(pool.used(), retained.bytes());
            FarVolumeCompilation uncharged, null_pool;
            ASSERT_TRUE(CompileFarVolumeSpan(request, authority, SpanSparseLimits(), uncharged));
            ASSERT_TRUE(
                CompileFarVolumeSpanCharged(request, authority, SpanSparseLimits(), {}, null_pool));
            EXPECT_EQ(output, uncharged);
            EXPECT_EQ(output, null_pool);
        }
    }
}

TEST(FarVolumeFacadeMemory, ExplicitSpanCancellationUnwindsGenerationAndMesh) {
    const FarVolumeRequest request{{5, 0, 0}, {-256, 0}, 42};
    const auto limits = SpanSparseLimits();
    // Cancel the first height, a generation density and the first mesher sample.
    for (const std::uint64_t stop : {0u, 5u, 128001u}) {
        auto authority = SpanSparseAuthority(FarVolumeNumericProfile::RawFloat);
        std::uint64_t heights = 0, densities = 0;
        authority.height = [&](std::int64_t, std::int64_t) -> std::optional<float> {
            ++heights;
            return stop == 0 ? std::nullopt : std::optional<float>{0.0f};
        };
        authority.density = [&](const FarVolumePosition& p,
                                float,
                                std::uint32_t,
                                FarCaveMode) -> std::optional<FarVolumeSample> {
            ++densities;
            if (densities == stop) {
                return std::nullopt;
            }
            return FarVolumeSample{p.x == 0 && p.y == 0 && p.z == 0 ? -1.0f : 1.0f, 7};
        };
        FarVolumeMemoryPool pool(kMemLargeLimit);
        FarVolumeMemoryLease retained(pool);
        FarVolumeCompilation output;
        output.mesh.vertices.emplace_back();
        output.tile.request = MemHeld();
        ASSERT_TRUE(retained.TryCharge(FarVolumeCompilationRetainedBytes(output)));
        const auto saved = output;
        const auto held = retained.bytes();
        FarVolumeFacadeFailure failure;
        EXPECT_FALSE(CompileFarVolumeSpanCharged(
            request, authority, limits, {&pool, &retained, nullptr}, output, &failure));
        EXPECT_EQ(failure.code, FarVolumeFacadeError::SamplerFailure);
        EXPECT_EQ(densities, stop);
        EXPECT_GT(heights, 0u);
        if (stop > 128000) {
            EXPECT_EQ(failure.mesh, FarVolumeMeshError::SamplerFailure);
        } else {
            EXPECT_EQ(failure.generation, FarVolumeBuildError::SamplerFailure);
        }
        EXPECT_EQ(output, saved);
        EXPECT_EQ(retained.bytes(), held);
        EXPECT_EQ(pool.used(), held);
    }
}

TEST(FarVolumeFacadeMemory, ExplicitSpanInvalidMemoryAndFirstAllocationRefuseBeforeCallbacks) {
    auto authority = MemPlane(FarVolumeNumericProfile::RawFloat);
    std::uint64_t calls = 0;
    authority.height = [&](std::int64_t, std::int64_t) {
        ++calls;
        return 0.0f;
    };
    const FarVolumeRequest request{{5, 0, 0}, {-256, 0}, 42};
    FarVolumeMemoryPool pool(kMemLargeLimit), other(kMemLargeLimit);
    FarVolumeMemoryLease retained(other);
    FarVolumeCompilation output;
    output.tile.request = MemHeld();
    const auto held = output;
    for (auto* binding : {static_cast<FarVolumeMemoryLease*>(nullptr), &retained}) {
        FarVolumeFacadeFailure failure;
        EXPECT_FALSE(CompileFarVolumeSpanCharged(
            request, authority, SpanSparseLimits(), {&pool, binding, nullptr}, output, &failure));
        EXPECT_EQ(failure.code, FarVolumeFacadeError::InvalidMemory);
        EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
        EXPECT_EQ(output, held);
        EXPECT_EQ(pool.attempts(), 0u);
    }
    FarVolumeMemoryLease valid(pool);
    pool.RefuseAttemptsFrom(0);
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompileFarVolumeSpanCharged(
        request, authority, SpanSparseLimits(), {&pool, &valid, nullptr}, output, &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
    EXPECT_EQ(output, held);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(other.used(), 0u);
}
} // namespace
