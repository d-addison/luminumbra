#include "luminumbra_common/world/FarVolumeRuntime.h"

#include "luminumbra_common/world/FarTierTable.h"
#include "luminumbra_common/world/FarVolume.h"
#include "luminumbra_common/world/FarVolumeMemory.h"
#include "luminumbra_common/world/FarVolumeMesher.h"
#include "luminumbra_common/world/FarVolumeTile.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <type_traits>

namespace {
using namespace Luminumbra::World;
constexpr FarVolumeSpanLedgerLimits kLedgerLimits{-4096, 4096, 1024};
} // namespace

TEST(FarVolumeRuntime, KeyRoundTripIncludesEveryTierAndNegativeCoordinates) {
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        for (const auto coordinate : {-3, 0, 7}) {
            const FarTileKey key{tier, coordinate, -coordinate};
            FarVolumeTileKey volume;
            ASSERT_TRUE(ToFarVolumeTileKey(key, volume));
            EXPECT_EQ(volume, (FarVolumeTileKey{tier, coordinate, -coordinate}));
            FarTileKey roundtrip;
            ASSERT_TRUE(ToFarTileKey(volume, roundtrip));
            EXPECT_EQ(roundtrip, key);
        }
    }
}

TEST(FarVolumeRuntime, KeyRefusalRetainsOutputAtDomainAndIntegerBoundaries) {
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const auto edge = kFarTierTable[tier - 1].tile_edge_meters;
        const auto limit = kFarVolumeExactCoordinateLimitMeters / edge;
        const auto t = static_cast<std::uint8_t>(tier);
        FarVolumeTileKey output{2, 4, 5};
        const auto previous = output;
        for (const auto x : {-limit,
                             limit - 1,
                             std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max()}) {
            EXPECT_FALSE(ToFarVolumeTileKey({t, x, 0}, output));
            EXPECT_EQ(output, previous);
        }
        EXPECT_TRUE(ToFarVolumeTileKey({t, -limit + 1, limit - 2}, output));
    }
    FarTileKey key{3, 4, 5};
    EXPECT_FALSE(ToFarTileKey({std::numeric_limits<std::size_t>::max(), 0, 0}, key));
    EXPECT_EQ(key, (FarTileKey{3, 4, 5}));
    FarVolumeTileKey volume{3, 4, 5};
    EXPECT_FALSE(ToFarVolumeTileKey({0, 0, 0}, volume));
    EXPECT_FALSE(ToFarVolumeTileKey({6, 0, 0}, volume));
    EXPECT_EQ(volume, (FarVolumeTileKey{3, 4, 5}));
}

TEST(FarVolumeRuntime, SpanUnionNeverShrinksAndGenerationOnlyTracksGrowth) {
    FarVolumeSpanLedger ledger;
    EXPECT_FALSE(ledger.Intent());
    EXPECT_FALSE(ledger.Extend({0, 1}, kLedgerLimits));
    ASSERT_TRUE(ledger.Reset({1, -1, 2}));
    EXPECT_EQ(ledger.generation(), 0u);
    ASSERT_TRUE(ledger.Extend({100, 100}, kLedgerLimits));
    EXPECT_EQ(ledger.generation(), 1u);
    ASSERT_TRUE(ledger.Extend({-200, 400}, kLedgerLimits));
    EXPECT_EQ(ledger.generation(), 2u);
    ASSERT_TRUE(ledger.Extend({0, 20}, kLedgerLimits));
    EXPECT_EQ(ledger.generation(), 2u);
    const auto union_intent = ledger.Intent();
    ASSERT_TRUE(union_intent);
    if (union_intent) {
        EXPECT_EQ(union_intent->extra_span, (FarVolumeCoverageSpan{-200, 400}));
    }
    EXPECT_FALSE(ledger.Covers({-192, 400}));
    EXPECT_FALSE(ledger.Covers({-208, 384}));
    EXPECT_TRUE(ledger.Covers({-208, 400}));
    EXPECT_FALSE(ledger.Covers({-209, 400}));
}

TEST(FarVolumeRuntime, SpanGrowthRefusalIsTransactionalIncludingInvalidReset) {
    FarVolumeSpanLedger ledger;
    ASSERT_TRUE(ledger.Reset({2, 1, -2}));
    ASSERT_TRUE(ledger.Extend({-128, 128}, kLedgerLimits));
    const auto previous = ledger.Intent();
    const auto generation = ledger.generation();
    for (const auto span : {FarVolumeCoverageSpan{-4097, 0},
                            {0, 4097},
                            {-600, 600},
                            {1, -1},
                            {0, std::numeric_limits<float>::infinity()},
                            {std::numeric_limits<float>::quiet_NaN(), 0}}) {
        EXPECT_FALSE(ledger.Extend(span, kLedgerLimits));
        EXPECT_EQ(ledger.Intent(), previous);
        EXPECT_EQ(ledger.generation(), generation);
    }
    EXPECT_FALSE(ledger.Reset({0, 0, 0}));
    EXPECT_EQ(ledger.Intent(), previous);
    EXPECT_EQ(ledger.generation(), generation);
    EXPECT_FALSE(ledger.Extend({0, 0}, {-4096, 4096, 0}));
    EXPECT_FALSE(ledger.Extend({0, 0}, {4096, -4096, 1024}));
    EXPECT_FALSE(ledger.Extend({0, 0}, {-8388608, 8388608, 1024}));
    EXPECT_EQ(ledger.Intent(), previous);
    EXPECT_EQ(ledger.generation(), generation);
    ASSERT_TRUE(ledger.Reset({3, -1, 1}));
    EXPECT_EQ(ledger.generation(), 0u);
    const auto reset_intent = ledger.Intent();
    ASSERT_TRUE(reset_intent);
    if (reset_intent) {
        EXPECT_FALSE(reset_intent->extra_span);
    }
}

namespace {
constexpr FarVolumePageBudget kPageBudget{std::numeric_limits<std::uint64_t>::max(),
                                          std::numeric_limits<std::uint64_t>::max(),
                                          std::numeric_limits<std::uint64_t>::max(),
                                          std::numeric_limits<std::uint64_t>::max()};
constexpr FarVolumeRequest kPageRequest{{1, -1, 1}, {-80, 112}, 73};
static_assert(std::is_trivially_copyable_v<FarVolumePagePlan>);
static_assert(std::is_trivially_copyable_v<FarVolumePageCompleteness>);

class PageAllocationCounter final : public std::pmr::memory_resource {
public:
    std::uint64_t allocations() const noexcept {
        return allocations_;
    }

private:
    std::uint64_t allocations_ = 0;
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
} // namespace

TEST(FarVolumeRuntime, PagesUseSignedGridAndClipOnlyOuterWindows) {
    FarVolumePagePlan plan;
    FarVolumeRequest page{{3, 2, 1}, {0, 256}, 5};
    const auto previous = page;
    EXPECT_EQ(plan.size(), 0u);
    EXPECT_FALSE(plan.Page(0, page));
    EXPECT_EQ(page, previous);
    for (const auto span : {FarVolumeSpan{-112, -16}, {-80, 112}, {0, 128}, {-128, 0}, {-16, 16}}) {
        auto request = kPageRequest;
        request.span = span;
        ASSERT_TRUE(plan.Reset(request, 64, 4));
        auto next_y = span.min_y;
        for (std::uint64_t index = 0; index < plan.size(); ++index) {
            ASSERT_TRUE(plan.Page(index, page));
            EXPECT_EQ(page.key, request.key);
            EXPECT_EQ(page.field_identity, request.field_identity);
            EXPECT_EQ(page.span.min_y, next_y);
            EXPECT_GT(page.span.max_y, page.span.min_y);
            EXPECT_LE(page.span.max_y - page.span.min_y, 64);
            if (index != 0) {
                EXPECT_EQ(page.span.min_y % 64, 0);
            }
            if (index + 1 != plan.size()) {
                EXPECT_EQ(page.span.max_y % 64, 0);
            }
            next_y = page.span.max_y;
        }
        EXPECT_EQ(next_y, span.max_y);
        const auto last = page;
        EXPECT_FALSE(plan.Page(plan.size(), page));
        EXPECT_FALSE(plan.Page(std::numeric_limits<std::uint64_t>::max(), page));
        EXPECT_EQ(page, last);
    }
    ASSERT_TRUE(plan.Reset(kPageRequest, 64, 4));
    ASSERT_EQ(plan.size(), 4u);
    ASSERT_TRUE(plan.Page(0, page));
    EXPECT_EQ(page.span, (FarVolumeSpan{-80, -64}));
    ASSERT_TRUE(plan.Page(1, page));
    EXPECT_EQ(page.span, (FarVolumeSpan{-64, 0}));
    ASSERT_TRUE(plan.Page(2, page));
    EXPECT_EQ(page.span, (FarVolumeSpan{0, 64}));
    ASSERT_TRUE(plan.Page(3, page));
    EXPECT_EQ(page.span, (FarVolumeSpan{64, 112}));
    ASSERT_TRUE(plan.Reset(kPageRequest, 48, 5));
    ASSERT_EQ(plan.size(), 5u);
    ASSERT_TRUE(plan.Page(0, page));
    EXPECT_EQ(page.span, (FarVolumeSpan{-80, -48}));
    ASSERT_TRUE(plan.Page(1, page));
    EXPECT_EQ(page.span, (FarVolumeSpan{-48, 0}));
    ASSERT_TRUE(plan.Page(4, page));
    EXPECT_EQ(page.span, (FarVolumeSpan{96, 112}));
}

TEST(FarVolumeRuntime, PagesAdmitMaximumStrideAndExactCapForEveryTier) {
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const auto edge = static_cast<std::int64_t>(kFarTierTable[tier - 1].brick_edge_meters);
        FarVolumeRequest request{{tier, -1, 0}, {-4096, 4096}, 1};
        FarVolumePagePlan plan;
        ASSERT_TRUE(plan.Reset(request, kFarVolumeMaxPageStrideMeters, 2));
        EXPECT_EQ(plan.size(), 2u);
        EXPECT_FALSE(plan.Reset(request, kFarVolumeMaxPageStrideMeters, 1));
        EXPECT_FALSE(plan.Reset(request, kFarVolumeMaxPageStrideMeters + edge, 2));
        FarVolumePageWork work;
        ASSERT_TRUE(plan.Admit(kPageBudget, work));
        EXPECT_EQ(work.candidates, 1024u * static_cast<std::uint64_t>(4096 / edge));
        EXPECT_EQ(work.generation_samples, work.candidates * 125);
        request.span = {-edge * 512, edge * 512};
        ASSERT_TRUE(plan.Reset(request, edge, kFarVolumeMaxPages));
        EXPECT_EQ(plan.size(), kFarVolumeMaxPages);
        request.span.max_y += edge;
        EXPECT_FALSE(plan.Reset(request, edge, kFarVolumeMaxPages));
        EXPECT_EQ(plan.size(), kFarVolumeMaxPages);
    }
}

TEST(FarVolumeRuntime, InvalidAndOverflowingPagePlansPreserveDescriptor) {
    FarVolumePagePlan plan;
    ASSERT_TRUE(plan.Reset(kPageRequest, 64, 4));
    FarVolumeRequest expected;
    ASSERT_TRUE(plan.Page(0, expected));
    for (const auto span :
         {FarVolumeSpan{0, 0},
          {16, -16},
          {-1, 16},
          {0, 17},
          {std::numeric_limits<std::int64_t>::min(), 16},
          {-16, std::numeric_limits<std::int64_t>::max()},
          {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()},
          {-8388608, 0},
          {0, 8388608}}) {
        auto request = kPageRequest;
        request.span = span;
        EXPECT_FALSE(plan.Reset(request, 64, 4));
        FarVolumeRequest unchanged;
        ASSERT_TRUE(plan.Page(0, unchanged));
        EXPECT_EQ(unchanged, expected);
        EXPECT_EQ(plan.size(), 4u);
    }
    for (const auto stride : {std::numeric_limits<std::int64_t>::min(),
                              std::int64_t{-64},
                              std::int64_t{0},
                              std::int64_t{1},
                              std::int64_t{63},
                              std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_FALSE(plan.Reset(kPageRequest, stride, 4));
    }
    for (const auto cap : {std::uint64_t{0},
                           std::uint64_t{3},
                           kFarVolumeMaxPages + 1,
                           std::numeric_limits<std::uint64_t>::max()}) {
        EXPECT_FALSE(plan.Reset(kPageRequest, 64, cap));
    }
    for (const auto key : {FarVolumeTileKey{0, 0, 0},
                           {6, 0, 0},
                           {1, std::numeric_limits<std::int64_t>::min(), 0},
                           {1, 0, std::numeric_limits<std::int64_t>::max()},
                           {1, 16383, 0},
                           {1, 0, -16384}}) {
        auto request = kPageRequest;
        request.key = key;
        EXPECT_FALSE(plan.Reset(request, 64, 4));
    }
    FarVolumeRequest unchanged;
    ASSERT_TRUE(plan.Page(0, unchanged));
    EXPECT_EQ(unchanged, expected);
    // Nearest brick-aligned endpoints whose complete halo remains exact.
    EXPECT_TRUE(plan.Reset({{1, 0, 0}, {-8388592, -8388576}, 0}, 16, 1));
    EXPECT_TRUE(plan.Reset({{1, 0, 0}, {8388576, 8388592}, 0}, 16, 1));
}

TEST(FarVolumeRuntime, PageInteriorsRemainIdenticalWhenBothEndsExtend) {
    FarVolumePagePlan initial, extended;
    ASSERT_TRUE(initial.Reset(kPageRequest, 64, 4));
    ASSERT_TRUE(
        extended.Reset({kPageRequest.key, {-208, 240}, kPageRequest.field_identity}, 64, 8));
    for (const auto index : {1u, 2u}) {
        FarVolumeRequest before, after;
        ASSERT_TRUE(initial.Page(index, before));
        ASSERT_TRUE(extended.Page(index + 2, after));
        EXPECT_EQ(before, after);
    }
    FarVolumePageWork before, after;
    ASSERT_TRUE(initial.Admit(kPageBudget, before));
    ASSERT_TRUE(extended.Admit(kPageBudget, after));
    EXPECT_EQ(before, after);
}

TEST(FarVolumeRuntime, PageCompletenessRejectsEveryNonexactAcknowledgement) {
    const FarVolumePagePlan empty;
    FarVolumePagePlan plan;
    FarVolumePageCompleteness no_pages(empty);
    EXPECT_FALSE(no_pages.Complete());
    EXPECT_FALSE(no_pages.Acknowledge(kPageRequest));
    ASSERT_TRUE(plan.Reset(kPageRequest, 64, 4));
    FarVolumeRequest first, second, third, fourth;
    ASSERT_TRUE(plan.Page(0, first));
    ASSERT_TRUE(plan.Page(1, second));
    ASSERT_TRUE(plan.Page(2, third));
    ASSERT_TRUE(plan.Page(3, fourth));
    FarVolumePageCompleteness fold(plan);
    // The fold retains the original descriptor even if the source is reset.
    ASSERT_TRUE(plan.Reset({kPageRequest.key, {0, 16}, 4}, 64, 1));
    EXPECT_FALSE(fold.Acknowledge(second));
    EXPECT_EQ(fold.accepted(), 0u);
    ASSERT_TRUE(fold.Acknowledge(first));
    EXPECT_FALSE(fold.Complete());
    for (const auto bad : {first,
                           third,
                           FarVolumeRequest{second.key, {-80, 0}, second.field_identity},
                           FarVolumeRequest{second.key, {-64, -16}, second.field_identity},
                           FarVolumeRequest{second.key, {-64, 16}, second.field_identity},
                           FarVolumeRequest{{1, 0, 0}, second.span, second.field_identity},
                           FarVolumeRequest{second.key, second.span, second.field_identity + 1}}) {
        EXPECT_FALSE(fold.Acknowledge(bad));
        EXPECT_EQ(fold.accepted(), 1u);
        EXPECT_FALSE(fold.Complete());
    }
    ASSERT_TRUE(fold.Acknowledge(second));
    EXPECT_FALSE(fold.Acknowledge(fourth));
    ASSERT_TRUE(fold.Acknowledge(third));
    EXPECT_FALSE(fold.Complete());
    ASSERT_TRUE(fold.Acknowledge(fourth));
    EXPECT_TRUE(fold.Complete());
    EXPECT_FALSE(fold.Acknowledge(fourth));
    EXPECT_EQ(fold.accepted(), 4u);
    EXPECT_TRUE(fold.Complete());
}

TEST(FarVolumeRuntime, PageAdmissionDerivesCanonicalWorkCachesHalosAndBuffers) {
    FarVolumePagePlan plan;
    for (const auto span : {FarVolumeSpan{0, 16}, {48, 80}, {-16, 80}}) {
        ASSERT_TRUE(plan.Reset({kPageRequest.key, span, 73}, 64, 3));
        FarVolumePageWork work;
        ASSERT_TRUE(plan.Admit(kPageBudget, work));
        const std::uint64_t layers = span.min_y == -16 ? 4 : 1;
        const auto vertical = layers * 4;
        EXPECT_EQ(work.candidates, std::uint64_t{32} * 32 * layers);
        EXPECT_EQ(work.generation_samples, work.candidates * kFarVolumeBrickSamples);
        EXPECT_EQ(work.cells, work.candidates * 64);
        EXPECT_EQ(work.indices, work.cells * 15);
        EXPECT_EQ(work.mesh_samples, std::uint64_t{131} * 131 * (vertical + 3));
        EXPECT_EQ(work.height_samples, std::uint64_t{131} * 131);
        EXPECT_EQ(work.density_samples, work.generation_samples + work.mesh_samples);
        EXPECT_EQ(work.sample_cache_entries, work.mesh_samples);
        EXPECT_EQ(work.gradient_cache_entries, std::uint64_t{129} * 129 * (vertical + 1));
        EXPECT_EQ(work.vertices,
                  std::uint64_t{2} * 128 * 129 * (vertical + 1) + vertical * 129u * 129u);
        EXPECT_EQ(work.edge_cache_entries, work.vertices);
        EXPECT_EQ(work.buffer_bytes,
                  2 * (work.candidates * sizeof(FarVolumeBrick) +
                       work.vertices * sizeof(FarVolumeVertex) +
                       work.indices * sizeof(std::uint32_t)) +
                      2 * work.candidates * sizeof(const FarVolumeBrick*) +
                      work.height_samples * sizeof(std::optional<float>));
    }
}

TEST(FarVolumeRuntime, RefusedPageAdmissionPrecedesCanonicalCallbacksAndAllocation) {
    FarVolumePagePlan plan;
    FarVolumePageWork required;
    EXPECT_FALSE(plan.Admit(kPageBudget, required));
    EXPECT_EQ(required, FarVolumePageWork{});
    ASSERT_TRUE(plan.Reset({kPageRequest.key, {0, 16}, 73}, 64, 1));
    ASSERT_TRUE(plan.Admit(kPageBudget, required));
    const FarVolumePageBudget exact{required.candidates,
                                    required.density_samples + required.height_samples,
                                    required.buffer_bytes,
                                    required.sample_cache_entries +
                                        required.gradient_cache_entries +
                                        required.edge_cache_entries};
    FarVolumePageWork output;
    ASSERT_TRUE(plan.Admit(exact, output));
    EXPECT_EQ(output, required);
    FarVolumeMemoryPool pool(required.buffer_bytes);
    FarVolumeMemoryLease retained(pool);
    PageAllocationCounter upstream;
    const FarVolumeMemory memory{&pool, &retained, &upstream};
    std::uint64_t calls = 0;
    const FarVolumeSampler sampler = [&calls](const FarVolumePosition& position) {
        ++calls;
        return std::optional<FarVolumeSample>{{static_cast<float>(position.y) - 8.0f, 0}};
    };
    FarVolumeTile tile;
    const auto build_if_admitted = [&](const FarVolumePageBudget& budget) {
        FarVolumePageWork work;
        if (!plan.Admit(budget, work))
            return false;
        FarVolumeRequest request;
        if (!plan.Page(0, request))
            return false;
        return BuildFarVolumeTileCharged(
            request,
            {work.candidates, static_cast<std::size_t>(work.candidates)},
            memory,
            sampler,
            tile);
    };
    for (const auto budget : {FarVolumePageBudget{exact.max_candidates - 1,
                                                  exact.max_sample_calls,
                                                  exact.max_buffer_bytes,
                                                  exact.max_cache_entries},
                              FarVolumePageBudget{exact.max_candidates,
                                                  exact.max_sample_calls - 1,
                                                  exact.max_buffer_bytes,
                                                  exact.max_cache_entries},
                              FarVolumePageBudget{exact.max_candidates,
                                                  exact.max_sample_calls,
                                                  exact.max_buffer_bytes - 1,
                                                  exact.max_cache_entries},
                              FarVolumePageBudget{exact.max_candidates,
                                                  exact.max_sample_calls,
                                                  exact.max_buffer_bytes,
                                                  exact.max_cache_entries - 1}}) {
        const auto previous = output;
        EXPECT_FALSE(plan.Admit(budget, output));
        EXPECT_EQ(output, previous);
        EXPECT_FALSE(build_if_admitted(budget));
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(upstream.allocations(), 0u);
        EXPECT_EQ(pool.attempts(), 0u);
        EXPECT_EQ(pool.used(), 0u);
    }
    ASSERT_TRUE(build_if_admitted(exact));
    EXPECT_EQ(calls, required.generation_samples);
    EXPECT_GT(pool.attempts(), 0u);
    EXPECT_EQ(tile.candidates_visited, required.candidates);
    EXPECT_EQ(tile.bricks.size(), required.candidates);
}

TEST(FarVolumeRuntime, PageWorkBoundsCanonicalMixedGenerationAndChargedMeshing) {
    FarVolumePagePlan plan;
    ASSERT_TRUE(plan.Reset({{1, 0, 0}, {0, 16}, 73}, 64, 1));
    FarVolumePageWork work;
    ASSERT_TRUE(plan.Admit(kPageBudget, work));
    FarVolumeRequest request;
    ASSERT_TRUE(plan.Page(0, request));
    FarVolumeMemoryPool pool(std::numeric_limits<std::uint64_t>::max());
    FarVolumeMemoryLease tile_lease(pool), mesh_lease(pool);
    PageAllocationCounter upstream;
    std::uint64_t calls = 0;
    const FarVolumeSampler sampler = [&calls](const FarVolumePosition& position) {
        ++calls;
        return std::optional<FarVolumeSample>{{static_cast<float>(position.y) - 8.0f, 19}};
    };
    FarVolumeTile tile;
    ASSERT_TRUE(
        BuildFarVolumeTileCharged(request,
                                  {work.candidates, static_cast<std::size_t>(work.candidates)},
                                  {&pool, &tile_lease, &upstream},
                                  sampler,
                                  tile));
    EXPECT_EQ(tile.bricks.size(), work.candidates);
    EXPECT_EQ(calls, work.generation_samples);
    FarVolumeMesh mesh;
    ASSERT_TRUE(MeshFarVolumeTileCharged(tile,
                                         request.field_identity,
                                         sampler,
                                         {work.cells,
                                          work.mesh_samples,
                                          static_cast<std::size_t>(work.vertices),
                                          static_cast<std::size_t>(work.indices)},
                                         {&pool, &mesh_lease, &upstream},
                                         mesh));
    EXPECT_EQ(mesh.cells_visited, work.cells);
    EXPECT_LE(mesh.samples_evaluated, work.mesh_samples);
    EXPECT_EQ(calls, work.generation_samples + mesh.samples_evaluated);
    EXPECT_LE(calls, work.density_samples);
    EXPECT_FALSE(mesh.vertices.empty());
    EXPECT_LE(mesh.vertices.capacity(), work.vertices);
    EXPECT_LE(mesh.indices.capacity(), work.indices);
    EXPECT_GT(upstream.allocations(), 0u);
    EXPECT_LE(upstream.allocations(),
              work.sample_cache_entries + work.gradient_cache_entries + work.edge_cache_entries +
                  1);
    EXPECT_LE(FarVolumeTileRetainedBytes(tile) + FarVolumeMeshRetainedBytes(mesh),
              work.buffer_bytes);
}

TEST(FarVolumeRuntime, RequestOrderProducesIdenticalUnionAndResolvedCoverage) {
    FarVolumeSpanLedger first, second;
    ASSERT_TRUE(first.Reset({3, 0, 0}));
    ASSERT_TRUE(second.Reset({3, 0, 0}));
    for (const auto span : {FarVolumeCoverageSpan{-256, 0}, {0, 256}, {-128, 384}})
        ASSERT_TRUE(first.Extend(span, kLedgerLimits));
    for (const auto span : {FarVolumeCoverageSpan{-128, 384}, {0, 256}, {-256, 0}})
        ASSERT_TRUE(second.Extend(span, kLedgerLimits));
    EXPECT_EQ(first.Intent(), second.Intent());
    EXPECT_TRUE(first.Covers({-256, 384}));
    EXPECT_TRUE(second.Covers({-256, 384}));
    // Generations count actual growth events; each caller binds the local value.
    EXPECT_EQ(first.generation(), 3u);
    EXPECT_EQ(second.generation(), 2u);
}
