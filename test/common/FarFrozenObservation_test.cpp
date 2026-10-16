#include "luminumbra/core/Types.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarFrozenObservation.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "rendering/FarVolumeRenderMesh.h"
#include "world/FarEditedBake.h"
#include "world/FarTierTable.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeTile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::World;
using Error = FarFrozenCaptureError;

FarFrozenCaptureLimits Limits() {
    return {64, 2'000'000, 8'000'000, 6'000'000, 64ULL * 1024 * 1024};
}
FarFrozenCaptureSource Source() {
    FarFrozenCaptureSource source;
    source.pristine.field_identity = 41;
    source.content_identity = 42;
    source.durable = {7, FarEditedImageState::Committed};
    source.pristine.numeric = FarVolumeNumericProfile::LegacySigned16;
    source.pristine.height = [](std::int64_t, std::int64_t) {
        return 0.25f;
    };
    source.pristine.density = [](const FarVolumePosition& p, float h, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{float(p.y) - h, 3};
    };
    source.committed = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{};
    };
    source.water = [](std::int64_t, std::int64_t) {
        return FarFrozenWaterSample{false, 0};
    };
    return source;
}
FarFrozenRegion SmallRegion(std::uint8_t tier = 1) {
    const auto dimensions = FarTierAt(tier);
    if (!dimensions.has_value()) {
        ADD_FAILURE() << "Invalid region tier: " << int(tier);
        return {};
    }
    const auto edge = std::int64_t(dimensions->brick_edge_meters);
    return {tier, {{-edge, -edge, -edge}, {edge, edge, edge}}};
}
std::shared_ptr<FarVolumeMemoryPool> Pool() {
    return std::make_shared<FarVolumeMemoryPool>(64ULL * 1024 * 1024);
}
FarFrozenTerrainOverride Constant(float value) {
    return [value](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{FarFrozenPresence::Present, {value, 9}};
    };
}
template<class Callback>
class CopyCountingCallback {
    Callback callback;
    std::size_t& copies;
    std::size_t& calls;

public:
    CopyCountingCallback(Callback value, std::size_t& copy_count, std::size_t& call_count)
        : callback(std::move(value))
        , copies(copy_count)
        , calls(call_count) {}
    CopyCountingCallback(const CopyCountingCallback& other)
        : callback(other.callback)
        , copies(other.copies)
        , calls(other.calls) {
        ++copies;
    }
    CopyCountingCallback(CopyCountingCallback&&) = default;
    template<class... Args>
    auto operator()(Args&&... args) const {
        ++calls;
        return callback(std::forward<Args>(args)...);
    }
};
FarEditedBakeLimits BakeLimits() {
    FarEditedBakeLimits limits;
    limits.max_edits = 8;
    limits.max_source_records = 8;
    limits.max_affected_bricks = 10000;
    limits.max_tiles = 64;
    limits.max_brick_visits_per_tier.fill(10000);
    limits.max_source_samples = 10000;
    limits.max_source_probes = 1000000;
    limits.max_density_samples = 1000000;
    limits.max_scratch_bytes = 16ULL * 1024 * 1024;
    limits.max_output_bytes = 16ULL * 1024 * 1024;
    return limits;
}
class EditedImage final : public FarEditedSourceView {
    std::array<float, 8> density{-1, -1, -1, -1, -1, -1, -1, -1};
    FarEditedLattice record{{0, 0, 0}, 1, {2, 2, 2}, density, {}, 6};
    FarEditedImageIdentity identity{7, FarEditedImageState::Committed};

public:
    void SetIdentity(FarEditedImageIdentity value) {
        identity = value;
    }
    FarEditedImageIdentity Identity() const override {
        return identity;
    }
    std::span<const FarEditedLattice> Lattices() const override {
        return {&record, 1};
    }
};
} // namespace

TEST(FarFrozenObservation, WorkerCompilationRetainsOnlyFrozenMemoryAfterOwnerRelease) {
    auto source = Source();
    std::size_t owner_calls = 0;
    auto owner = std::make_shared<float>(0.25f);
    std::weak_ptr<float> const weak_owner = owner;
    source.pristine.height = [owner, &owner_calls](std::int64_t, std::int64_t) {
        ++owner_calls;
        return *owner;
    };
    const FarVolumeRequest request{{5, -1, -1}, {-256, 256}, 42};
    auto pool = Pool();
    std::weak_ptr<FarVolumeMemoryPool> const weak_pool = pool;
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(CaptureFarVolumeObservation(request, source, Limits(), pool, frozen), Error::None);
    const auto observed_calls = owner_calls;
    auto authority = frozen->Authority(5);
    auto pristine = frozen->PristineSampler();
    const auto identity = frozen->content_identity();
    *owner = 100;
    owner.reset();
    source = {};
    frozen.reset();
    pool.reset();
    EXPECT_TRUE(weak_owner.expired());
    EXPECT_FALSE(weak_pool.expired());
    FarVolumeCompilation output;
    const FarVolumeCompilationLimits limits{
        {131ULL * 131, 1'000'000},
        {2ULL * 32ULL * 32, 32ULL * 32},
        {32ULL * 32 * 64, 300'000, 129ULL * 129, 128ULL * 128 * 6}};
    auto worker = std::async(std::launch::async, [&] {
        return CompileFarVolume({request.key, {}}, authority, limits, output);
    });
    ASSERT_TRUE(worker.get());
    EXPECT_EQ(output.tile.request.span, request.span);
    EXPECT_EQ(output.tile.request.field_identity, identity);
    EXPECT_EQ(owner_calls, observed_calls);
    ASSERT_FALSE(output.mesh.vertices.empty());
    for (const auto& vertex : output.mesh.vertices)
        EXPECT_DOUBLE_EQ(vertex.normal[1], 1);
    EXPECT_EQ(pristine(5, {-8192, 0, -8192}), (std::optional<FarVolumeSample>{{-0.25f, 3}}));
    authority = {};
    pristine = {};
    EXPECT_TRUE(weak_pool.expired());
}

TEST(FarFrozenObservation, AllTiersIncludeEveryAxisNormalHaloAndRefuseOutsideSupport) {
    auto source = Source();
    std::vector<FarFrozenRegion> regions;
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier)
        regions.push_back(SmallRegion(tier));
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), frozen), Error::None);
    for (const auto& region : regions) {
        const auto tier = FarTierAt(region.tier);
        ASSERT_TRUE(tier.has_value());
        if (!tier.has_value())
            return;
        const auto step = std::int64_t(tier->sample_spacing_meters);
        for (auto z = region.support.min.z; z <= region.support.max.z; z += step)
            for (auto y = region.support.min.y; y <= region.support.max.y; y += step)
                for (auto x = region.support.min.x; x <= region.support.max.x; x += step) {
                    const FarVolumePosition p{x, y, z};
                    for (const auto offset : std::array<FarVolumePosition, 7>{{{},
                                                                               {step, 0, 0},
                                                                               {-step, 0, 0},
                                                                               {0, step, 0},
                                                                               {0, -step, 0},
                                                                               {0, 0, step},
                                                                               {0, 0, -step}}}) {
                        const FarVolumePosition q{p.x + offset.x, p.y + offset.y, p.z + offset.z};
                        const auto sample = frozen->Sample(region.tier, q, true);
                        ASSERT_TRUE(sample.has_value());
                        if (!sample.has_value())
                            return;
                        EXPECT_EQ(sample->density, float(q.y) - 0.25f);
                        ASSERT_TRUE(frozen->Height(region.tier, q.x, q.z));
                    }
                }
        EXPECT_FALSE(frozen->Sample(region.tier, {region.support.min.x - 2 * step, 0, 0}));
        EXPECT_FALSE(frozen->Sample(region.tier, {1, 0, 0}));
    }
}

TEST(FarFrozenObservation, PreviewResolvesLiveThenParkedThenCommittedThenPristine) {
    auto source = Source();
    source.use = FarFrozenUse::Preview;
    source.committed = Constant(-10);
    source.parked = [](std::uint8_t, const FarVolumePosition& p) {
        return FarFrozenOverride<FarVolumeSample>{
            p.x <= 0 ? FarFrozenPresence::Present : FarFrozenPresence::Absent, {-20, 4}};
    };
    source.live = [](std::uint8_t, const FarVolumePosition& p) {
        return FarFrozenOverride<FarVolumeSample>{
            p.x < 0 ? FarFrozenPresence::Present : FarFrozenPresence::Absent, {-30, 5}};
    };
    source.parked_water = [](std::int64_t x, std::int64_t) {
        return FarFrozenOverride<FarFrozenWaterSample>{
            x <= 0 ? FarFrozenPresence::Present : FarFrozenPresence::Absent, {true, 20}};
    };
    source.live_water = [](std::int64_t x, std::int64_t) {
        return FarFrozenOverride<FarFrozenWaterSample>{
            x < 0 ? FarFrozenPresence::Present : FarFrozenPresence::Absent, {true, 30}};
    };
    const auto region = SmallRegion();
    std::shared_ptr<const FarFrozenObservation> preview, committed;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), preview),
              Error::None);
    const auto live_sample = preview->Sample(1, {-4, 0, 0});
    const auto parked_sample = preview->Sample(1, {0, 0, 0});
    const auto committed_sample = preview->Sample(1, {4, 0, 0});
    ASSERT_TRUE(live_sample.has_value());
    if (!live_sample.has_value())
        return;
    ASSERT_TRUE(parked_sample.has_value());
    if (!parked_sample.has_value())
        return;
    ASSERT_TRUE(committed_sample.has_value());
    if (!committed_sample.has_value())
        return;
    EXPECT_EQ(live_sample->density, -30);
    EXPECT_EQ(parked_sample->density, -20);
    EXPECT_EQ(committed_sample->density, -10);
    const auto live_water = preview->WaterSample(0, -4, 0);
    const auto parked_water = preview->WaterSample(0, 0, 0);
    const auto committed_water = preview->WaterSample(0, 4, 0);
    ASSERT_TRUE(live_water.has_value());
    if (!live_water.has_value())
        return;
    ASSERT_TRUE(parked_water.has_value());
    if (!parked_water.has_value())
        return;
    ASSERT_TRUE(committed_water.has_value());
    if (!committed_water.has_value())
        return;
    EXPECT_EQ(live_water->surface_height, 30);
    EXPECT_EQ(parked_water->surface_height, 20);
    EXPECT_FALSE(committed_water->wet);
    source.use = FarFrozenUse::Committed;
    source.live = [](std::uint8_t, const FarVolumePosition&) -> FarFrozenOverride<FarVolumeSample> {
        throw std::runtime_error("unexpected preview read");
    };
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), committed),
              Error::None);
    const auto terrain = committed->Sample(1, {-4, 0, 0});
    const auto water = committed->WaterSample(0, -4, 0);
    ASSERT_TRUE(terrain.has_value());
    if (!terrain.has_value())
        return;
    ASSERT_TRUE(water.has_value());
    if (!water.has_value())
        return;
    EXPECT_EQ(terrain->density, -10);
    EXPECT_FALSE(water->wet);
    EXPECT_EQ(committed->durable_identity().generation, preview->durable_identity().generation);
    EXPECT_NE(committed->content_identity(), preview->content_identity());
    source.committed = Source().committed;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), committed),
              Error::None);
    const auto pristine = committed->Sample(1, {4, 0, 0});
    ASSERT_TRUE(pristine.has_value());
    if (!pristine.has_value())
        return;
    EXPECT_EQ(pristine->density, -0.25f);
}

TEST(FarFrozenObservation, BakeUsesExistingDurableImageAndNeverPreviewSamples) {
    auto source = Source();
    source.use = FarFrozenUse::Preview;
    source.live = Constant(100);
    source.parked = Constant(200);
    source.committed = Constant(300);
    // Small finite supports enclose every edited brick sample in all tiers.
    std::vector<FarFrozenRegion> regions;
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier)
        regions.push_back(SmallRegion(tier));
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), frozen), Error::None);
    EditedImage edited;
    const FarEditedFootprint changed{{0, 0, 0}, {1, 1, 1}};
    FarEditedBakeResult baked, expected;
    ASSERT_EQ(BakeFarEditedTilesFrozen(edited, {&changed, 1}, *frozen, BakeLimits(), baked),
              FarEditedBakeError::None);
    ASSERT_EQ(BakeFarEditedTiles(
                  edited,
                  {&changed, 1},
                  [](std::uint8_t, const FarVolumePosition& p) {
                      return std::optional<FarVolumeSample>{{float(p.y) - 0.25f, 3}};
                  },
                  BakeLimits(),
                  expected),
              FarEditedBakeError::None);
    ASSERT_EQ(baked.tiles.size(), expected.tiles.size());
    for (std::size_t i = 0; i < baked.tiles.size(); ++i) {
        EXPECT_EQ(baked.tiles[i].bytes, expected.tiles[i].bytes);
        EXPECT_EQ(baked.tiles[i].max_member_generation, 6u);
    }
    const auto previous = baked.tiles.front().bytes;
    edited.SetIdentity({8, FarEditedImageState::Committed});
    EXPECT_EQ(BakeFarEditedTilesFrozen(edited, {&changed, 1}, *frozen, BakeLimits(), baked),
              FarEditedBakeError::IdentityMismatch);
    EXPECT_EQ(baked.tiles.front().bytes, previous);
    edited.SetIdentity({7, FarEditedImageState::Prepared});
    source.durable = edited.Identity();
    source.use = FarFrozenUse::Committed;
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), frozen), Error::None);
    ASSERT_EQ(BakeFarEditedTilesFrozen(edited, {&changed, 1}, *frozen, BakeLimits(), baked),
              FarEditedBakeError::None);
    EXPECT_EQ(baked.source.state, FarEditedImageState::Prepared);
    EXPECT_FALSE(frozen->Authority(1).density);
    source.pristine.numeric = FarVolumeNumericProfile::RawFloat;
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), frozen), Error::None);
    EXPECT_EQ(BakeFarEditedTilesFrozen(edited, {&changed, 1}, *frozen, BakeLimits(), baked),
              FarEditedBakeError::IdentityMismatch);
}

TEST(FarFrozenObservation, MissingCommittedResolverRefusesWithoutLosingDurableEdits) {
    auto source = Source();
    source.committed = Constant(-10);
    const auto region = SmallRegion();
    auto pool = Pool();
    std::shared_ptr<const FarFrozenObservation> output;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::None);
    EXPECT_EQ(output->Sample(1, {0, 0, 0}), (std::optional<FarVolumeSample>{{-10, 9}}));
    const auto previous = output;
    const auto held = pool->used(), attempts = pool->attempts();
    std::size_t calls = 0;
    source.pristine.height = [&calls](std::int64_t, std::int64_t) {
        ++calls;
        return 0.25f;
    };
    source.committed = {};
    // Preview authority cannot substitute for missing durable authority.
    source.live = Constant(-30);
    source.parked = Constant(-20);
    for (const auto use : {FarFrozenUse::Committed, FarFrozenUse::Preview}) {
        source.use = use;
        EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
                  Error::InvalidSource);
        EXPECT_EQ(
            CaptureFarVolumeObservation({{1, 0, 0}, {-16, 16}, 42}, source, Limits(), pool, output),
            Error::InvalidSource);
        EXPECT_EQ(output, previous);
        EXPECT_EQ(pool->used(), held);
        EXPECT_EQ(pool->attempts(), attempts);
        EXPECT_EQ(calls, 0u);
    }
}

TEST(FarFrozenObservation, WaterWorkCapIncludesAllRegionsHalosAndPreviewLayers) {
    const std::array<FarFrozenRegion, 2> regions{
        {{1, {{0, 0, 0}, {0, 0, 0}}}, {1, {{4, 0, 0}, {4, 0, 0}}}}};
    for (const auto use : {FarFrozenUse::Committed, FarFrozenUse::Preview}) {
        auto source = Source();
        source.use = use;
        std::size_t height_calls = 0, water_calls = 0;
        source.pristine.height = [&height_calls](std::int64_t, std::int64_t) {
            ++height_calls;
            return 0.25f;
        };
        source.water = [&water_calls](std::int64_t, std::int64_t) {
            ++water_calls;
            return FarFrozenWaterSample{false, 0};
        };
        source.live_water = source.parked_water = [&water_calls](std::int64_t, std::int64_t) {
            ++water_calls;
            return FarFrozenOverride<FarFrozenWaterSample>{};
        };
        // Each point support has a 3 by 3 horizontal halo. Overlaps are sampled
        // separately; preview falls through both override layers to base water.
        const std::uint64_t per_region = 9ULL * (use == FarFrozenUse::Preview ? 3 : 1);
        const auto total = 2 * per_region;
        auto limits = Limits();
        limits.max_water_samples = per_region;
        auto pool = Pool();
        std::shared_ptr<const FarFrozenObservation> output;
        for (const auto& region : regions) {
            water_calls = 0;
            ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, limits, pool, output),
                      Error::None);
            EXPECT_EQ(water_calls, per_region);
        }
        const auto previous = output;
        const auto held = pool->used(), attempts = pool->attempts();
        height_calls = water_calls = 0;
        for (const auto cap : {per_region, total - 1}) {
            limits.max_water_samples = cap;
            EXPECT_EQ(CaptureFarFrozenObservation(regions, source, limits, pool, output),
                      Error::WorkLimit);
            EXPECT_EQ(output, previous);
            EXPECT_EQ(pool->used(), held);
            EXPECT_EQ(pool->attempts(), attempts);
            EXPECT_EQ(height_calls, 0u);
            EXPECT_EQ(water_calls, 0u);
        }
        limits.max_water_samples = total;
        ASSERT_EQ(CaptureFarFrozenObservation(regions, source, limits, pool, output), Error::None);
        EXPECT_EQ(height_calls, 18u);
        EXPECT_EQ(water_calls, total);
    }
}

TEST(FarFrozenObservation, ExplicitWetDryUnavailableAndNonFiniteRefusals) {
    auto source = Source();
    const auto region = SmallRegion();
    auto pool = Pool();
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, frozen),
              Error::None);
    EXPECT_EQ(frozen->WaterCapability(0), FarFrozenWaterCapability::ExplicitlyDry);
    const auto dry_bound = frozen->WaterBound(0);
    ASSERT_TRUE(dry_bound.has_value());
    if (!dry_bound.has_value())
        return;
    EXPECT_TRUE(dry_bound->known_dry);
    source.water = [](std::int64_t x, std::int64_t) {
        return FarFrozenWaterSample{x <= 0, float(x) / 4};
    };
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, frozen),
              Error::None);
    EXPECT_EQ(frozen->WaterCapability(0), FarFrozenWaterCapability::Wet);
    EXPECT_EQ(frozen->WaterBound(0), (std::optional<FarFrozenWaterBound>{{false, -5, 5}}));
    const auto previous = frozen;
    const auto held = pool->used();
    source.water = [](std::int64_t, std::int64_t) -> std::optional<FarFrozenWaterSample> {
        return std::nullopt;
    };
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, frozen),
              Error::Unavailable);
    EXPECT_EQ(frozen, previous);
    EXPECT_EQ(pool->used(), held);
    source = Source();
    source.committed = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{FarFrozenPresence::Unavailable, {}};
    };
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, frozen),
              Error::Unavailable);
    source.committed = Constant(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, frozen),
              Error::NonFiniteSample);
    source = Source();
    source.water = [](std::int64_t, std::int64_t) {
        return FarFrozenWaterSample{false, std::numeric_limits<float>::infinity()};
    };
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, frozen),
              Error::NonFiniteSample);
    EXPECT_EQ(frozen, previous);
    EXPECT_EQ(pool->used(), held);
    EXPECT_EQ(frozen->WaterCapability(1), FarFrozenWaterCapability::Unavailable);
}

TEST(FarFrozenObservation, EveryByteAdmissionFailurePreservesImageAndReleasesCharges) {
    auto source = Source();
    const auto region = SmallRegion();
    auto measured = Pool();
    std::shared_ptr<const FarFrozenObservation> previous;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), measured, previous),
              Error::None);
    const auto bytes = measured->used(), attempts = measured->attempts();
    ASSERT_GT(attempts, 1u);
    for (std::uint64_t fail = 0; fail < attempts; ++fail) {
        auto pool = Pool();
        pool->RefuseAttemptsFrom(fail);
        auto output = previous;
        EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
                  Error::ByteLimit);
        EXPECT_EQ(output, previous);
        EXPECT_EQ(pool->used(), 0u);
    }
    for (const auto cap : {std::uint64_t(0), bytes - 1, bytes, bytes + 1}) {
        auto pool = std::make_shared<FarVolumeMemoryPool>(cap);
        auto output = previous;
        const auto result =
            CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output);
        EXPECT_EQ(result, cap < bytes ? Error::ByteLimit : Error::None);
        if (cap < bytes) {
            EXPECT_EQ(output, previous);
            EXPECT_EQ(pool->used(), 0u);
        }
        auto limits = Limits();
        limits.max_bytes = cap;
        output = previous;
        EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, limits, Pool(), output),
                  cap < bytes ? Error::ByteLimit : Error::None);
    }
    auto pool = std::make_shared<FarVolumeMemoryPool>(2 * bytes - 1);
    std::shared_ptr<const FarFrozenObservation> output;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::None);
    const auto held = output;
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::ByteLimit);
    EXPECT_EQ(output, held);
    EXPECT_EQ(pool->used(), bytes);
}

TEST(FarFrozenObservation, WorkCoordinateIdentityAndPreparedPublicationRefusals) {
    auto source = Source();
    auto region = SmallRegion();
    auto pool = Pool();
    std::shared_ptr<const FarFrozenObservation> output;
    for (int cap = 0; cap < 4; ++cap) {
        auto limits = Limits();
        if (cap == 0)
            limits.max_regions = 0;
        if (cap == 1)
            limits.max_height_samples = 0;
        if (cap == 2)
            limits.max_density_samples = 0;
        if (cap == 3)
            limits.max_water_samples = 0;
        EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, limits, pool, output),
                  Error::WorkLimit);
    }
    EXPECT_EQ(pool->attempts(), 0u);
    region.support.min.x = -kFarVolumeExactCoordinateLimitMeters;
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::CoordinateRange);
    region = SmallRegion();
    region.support.min.x += 1;
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::InvalidRegion);
    source.durable.state = FarEditedImageState::Prepared;
    source.use = FarFrozenUse::Preview;
    region = SmallRegion();
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::IdentityMismatch);
    source.use = FarFrozenUse::Committed;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::None);
    EXPECT_FALSE(output->Authority(1).density);
    EXPECT_EQ(
        CaptureFarVolumeObservation({{1, 0, 0}, {-16, 16}, 99}, source, Limits(), pool, output),
        Error::IdentityMismatch);
}

TEST(FarFrozenObservation, CanonicalOrderingAndVolatileFingerprintAreRepeatable) {
    auto source = Source();
    source.use = FarFrozenUse::Preview;
    std::array<FarFrozenRegion, 2> regions{SmallRegion(2), SmallRegion(1)};
    std::shared_ptr<const FarFrozenObservation> first, second;
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), first), Error::None);
    std::reverse(regions.begin(), regions.end());
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), second), Error::None);
    EXPECT_EQ(first->content_identity(), second->content_identity());
    const auto first_region = first->Region(0);
    ASSERT_TRUE(first_region.has_value());
    if (!first_region.has_value())
        return;
    EXPECT_EQ(first_region->tier, 1);
    source.live = Constant(9);
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), second), Error::None);
    EXPECT_NE(first->content_identity(), second->content_identity());
    EXPECT_EQ(first->durable_identity().generation, second->durable_identity().generation);
}

TEST(FarFrozenObservation, ParkingRetainsUnsavedPreviewWithTheSameRenderFingerprint) {
    auto source = Source();
    source.use = FarFrozenUse::Preview;
    source.live = Constant(-30);
    const auto region = SmallRegion();
    std::shared_ptr<const FarFrozenObservation> live, parked;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), live),
              Error::None);
    source.parked = std::move(source.live);
    source.live = {};
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), parked),
              Error::None);
    EXPECT_EQ(live->content_identity(), parked->content_identity());
    EXPECT_EQ(live->Sample(1, {0, 0, 0}), parked->Sample(1, {0, 0, 0}));
    EXPECT_EQ(live->durable_identity().generation, parked->durable_identity().generation);
}

TEST(FarFrozenObservation, OversizedWorkAndOverflowRefuseBeforeOwnerCallbacksOrAllocation) {
    auto source = Source();
    std::size_t calls = 0;
    source.pristine.height = [&calls](std::int64_t, std::int64_t) {
        ++calls;
        return 0.25f;
    };
    auto limits = Limits();
    limits.max_density_samples = std::numeric_limits<std::uint64_t>::max();
    const auto extent = kFarVolumeExactCoordinateLimitMeters - 8;
    const FarFrozenRegion region{1, {{-extent, -extent, -extent}, {extent, extent, extent}}};
    auto pool = Pool();
    std::shared_ptr<const FarFrozenObservation> output;
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, limits, pool, output),
              Error::WorkLimit);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(pool->attempts(), 0u);
    EXPECT_EQ(pool->used(), 0u);
}

TEST(FarFrozenObservation, ProductionCaptureNeverCopiesTargetsOnRefusalOrSuccess) {
    Luminumbra::Systems::TerrainGenParams params;
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.continental_spline = {{-1, 0}, {1, 0}};
    params.height_offset = 0.25f;
    params.caves_enabled = false;
    auto world =
        std::make_unique<Luminumbra::Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
    auto source = Source();
    const auto region = SmallRegion();
    std::shared_ptr<const FarFrozenObservation> output;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), output),
              Error::None);
    const auto previous = output;
    source.use = FarFrozenUse::Preview;
    source.committed = Constant(-10);
    source.parked = source.live = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{};
    };
    source.parked_water = source.live_water = [](std::int64_t, std::int64_t) {
        return FarFrozenOverride<FarFrozenWaterSample>{};
    };
    std::array<std::size_t, 8> copies{}, calls{};
    const auto track = [&](auto& callback, std::size_t index) {
        callback = CopyCountingCallback{std::move(callback), copies[index], calls[index]};
    };
    track(source.pristine.height, 0);
    track(source.pristine.density, 1);
    track(source.committed, 2);
    track(source.parked, 3);
    track(source.live, 4);
    track(source.water, 5);
    track(source.parked_water, 6);
    track(source.live_water, 7);
    copies.fill(0);
    calls.fill(0);
    const FarVolumeRequest request{{5, 0, 0}, {-256, 256}, 42};
    auto pool = Pool();
    for (int cap = 0; cap < 6; ++cap) {
        auto limits = Limits();
        if (cap == 0)
            limits = {};
        if (cap == 1)
            limits.max_height_samples = 0;
        if (cap == 2)
            limits.max_density_samples = 0;
        if (cap == 3)
            limits.max_water_samples = 0;
        if (cap == 4)
            limits.max_bytes = 0;
        if (cap == 5)
            pool = std::make_shared<FarVolumeMemoryPool>(0);
        EXPECT_EQ(CaptureWorldFarVolumeObservation(*world, request, source, limits, pool, output),
                  cap < 4 ? Error::WorkLimit : Error::ByteLimit);
        EXPECT_EQ(output, previous);
        EXPECT_EQ(pool->used(), 0u);
        if (cap < 5) {
            EXPECT_EQ(pool->attempts(), 0u);
        }
        for (std::size_t i = 0; i < copies.size(); ++i) {
            EXPECT_EQ(copies[i], 0u) << i;
            EXPECT_EQ(calls[i], 0u) << i;
        }
    }
    ASSERT_EQ(CaptureWorldFarVolumeObservation(*world, request, source, Limits(), Pool(), output),
              Error::None);
    const auto live_height = world->GetTerrainHeightAt(0, 0);
    const auto live_density =
        world->SamplePristineFarDensity({0, 0, 0}, live_height, 64, source.pristine.caves);
    EXPECT_NE(output, previous);
    for (std::size_t i = 0; i < copies.size(); ++i) {
        EXPECT_EQ(copies[i], 0u) << i;
        EXPECT_EQ(calls[i] == 0, i < 2) << i;
    }
    const auto captured_calls = calls;
    source = {};
    world.reset();
    EXPECT_EQ(output->Sample(5, {0, 0, 0}), (std::optional<FarVolumeSample>{{-10, 9}}));
    const auto pristine = output->Sample(5, {0, 0, 0}, true);
    ASSERT_TRUE(pristine.has_value());
    if (!pristine.has_value())
        return;
    EXPECT_EQ(std::bit_cast<std::uint32_t>(pristine->density),
              std::bit_cast<std::uint32_t>(live_density));
    EXPECT_EQ(output->WaterSample(0, 0, 0), (std::optional<FarFrozenWaterSample>{{false, 0}}));
    EXPECT_EQ(calls, captured_calls);
}

TEST(FarFrozenObservation, ProductionCaptureLeavesSimulationStateUnchangedAndSurvivesClose) {
    Luminumbra::Systems::TerrainGenParams params;
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.continental_spline = {{-1, 0}, {1, 0}};
    params.height_offset = 0.25f;
    params.caves_enabled = false;
    auto world =
        std::make_unique<Luminumbra::Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
    const auto full_lattice = std::size_t(Luminumbra::CHUNK_SIZE_X + 1) *
                              std::size_t(Luminumbra::CHUNK_SIZE_Y + 1) *
                              std::size_t(Luminumbra::CHUNK_SIZE_Z + 1);
    chunk->sdf_data.assign(full_lattice, -1);
    chunk->material_data.assign(full_lattice, 7);
    chunk->mark_voxel_data_dirty();
    chunk->current_lod.store(0);
    chunk->current_water_resolution.store(4);
    chunk->water_depth_mm.assign(16, 250);
    chunk->water_bed_mm.assign(16, -1000);
    chunk->water_edge_flux.assign(32, 2);
    chunk->water_level_data.assign(16, -0.75f);
    chunk->water_rest_level.assign(16, -1);
    chunk->has_water_sim.store(true);
    ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    const auto state = [&chunk] {
        return std::tuple{chunk->sdf_data,
                          chunk->heightmap_data,
                          chunk->material_data,
                          chunk->pending_sdf_data,
                          chunk->pending_heightmap_data,
                          chunk->pending_material_data,
                          chunk->water_depth_mm,
                          chunk->water_bed_mm,
                          chunk->water_edge_flux,
                          chunk->water_level_data,
                          chunk->water_flow_data,
                          chunk->water_sim_terrain_height,
                          chunk->water_rest_level,
                          chunk->get_state(),
                          chunk->current_lod.load(),
                          chunk->current_water_resolution.load(),
                          chunk->has_water_sim.load(),
                          chunk->has_collision.load(),
                          chunk->is_water_sleeping.load(),
                          chunk->is_voxel_data_dirty(),
                          chunk->voxel_revision(),
                          chunk->sdf_provenance()};
    };
    const auto before_state = state();
    const auto params_hash = ComputeTerrainParamsHash(world->get_params(), world->get_seed());
    const auto before = world->debug_water_state_hash();
    const auto cursor = world->GetWaterSimWindowCursor();
    const auto epoch = world->water_epoch();
    const auto revision = world->far_lod_authority_revision();
    const auto chunks = world->snapshot_streamed_chunks();
    auto source = Source();
    source.use = FarFrozenUse::Preview;
    source.live_water = [chunk](std::int64_t x, std::int64_t z) {
        if (x != 0 || z != 0)
            return FarFrozenOverride<FarFrozenWaterSample>{};
        return FarFrozenOverride<FarFrozenWaterSample>{
            FarFrozenPresence::Present,
            {chunk->water_depth_mm.front() > 0,
             float(chunk->water_bed_mm.front() + chunk->water_depth_mm.front()) / 1000}};
    };
    source.pristine.height = [](std::int64_t, std::int64_t) -> std::optional<float> {
        throw std::runtime_error("production terrain must replace this callback");
    };
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(CaptureWorldFarVolumeObservation(
                  *world, {{5, 0, 0}, {-256, 256}, 42}, source, Limits(), Pool(), frozen),
              Error::None);
    const auto previous = frozen;
    auto refused_limits = Limits();
    refused_limits.max_density_samples = 0;
    EXPECT_EQ(CaptureWorldFarVolumeObservation(
                  *world, {{5, 0, 0}, {-256, 256}, 42}, source, refused_limits, Pool(), frozen),
              Error::WorkLimit);
    EXPECT_EQ(frozen, previous);
    const auto after = world->debug_water_state_hash();
    EXPECT_EQ(before.hash, after.hash);
    EXPECT_EQ(before.water_chunks, after.water_chunks);
    EXPECT_EQ(world->GetWaterSimWindowCursor(), cursor);
    EXPECT_EQ(world->water_epoch(), epoch);
    EXPECT_EQ(world->far_lod_authority_revision(), revision);
    EXPECT_EQ(world->snapshot_streamed_chunks(), chunks);
    EXPECT_EQ(state(), before_state);
    EXPECT_EQ(ComputeTerrainParamsHash(world->get_params(), world->get_seed()), params_hash);
    EXPECT_EQ(world->get_seed(), 1337);
    EXPECT_EQ(world->get_params().height_offset, params.height_offset);
    const auto live_height = world->GetTerrainHeightAt(0, 0);
    world.reset();
    source = {};
    chunk->water_depth_mm.front() = 0;
    const auto captured_height = frozen->Height(5, 0, 0);
    ASSERT_TRUE(captured_height.has_value());
    if (!captured_height.has_value())
        return;
    EXPECT_EQ(std::bit_cast<std::uint32_t>(*captured_height),
              std::bit_cast<std::uint32_t>(live_height));
    ASSERT_TRUE(frozen->Sample(5, {0, 0, 0}));
    EXPECT_EQ(frozen->WaterCapability(0), FarFrozenWaterCapability::Wet);
    EXPECT_EQ(frozen->WaterSample(0, 0, 0), (std::optional<FarFrozenWaterSample>{{true, -0.75f}}));
}

TEST(FarFrozenObservation, RendererWaterBridgeUsesFrozenCombinedIdentityAndRefusesPartialSupport) {
    using namespace Luminumbra::Rendering;
    auto source = Source();
    source.water = [](std::int64_t, std::int64_t) {
        return FarFrozenWaterSample{true, 0};
    };
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(
        CaptureFarVolumeObservation({{1, 0, -1}, {-16, 16}, 42}, source, Limits(), Pool(), frozen),
        Error::None);
    FarWorldFrozenWaterObservation const water(frozen, {1, 0, -1});
    EXPECT_EQ(water.content_identity(), frozen->Authority(1).field_identity);
    const auto water_bound = water.WaterBound({1, 0, -1});
    ASSERT_TRUE(water_bound.has_value());
    if (!water_bound.has_value())
        return;
    EXPECT_FALSE(water.WaterBound({1, 1, -1}));
    FarVolumeRenderMesh terrain, output;
    terrain.request = {{1, 0, -1}, {-16, 16}, frozen->content_identity()};
    terrain.origin_meters = {0, -16, -512};
    terrain.sampled_max_meters = {512, 16, 0};
    FarVolumeMemoryPool mesh_pool(16ULL * 1024 * 1024);
    FarVolumeMemoryLease retained(mesh_pool);
    ASSERT_TRUE(
        BuildFarWorldWaterMesh(terrain,
                               water,
                               *water_bound,
                               {129ULL * 129, 128ULL * 128, 100000, 200000, 8ULL * 1024 * 1024},
                               {&mesh_pool, &retained, nullptr},
                               output));
    EXPECT_TRUE(output.water_complete);
    EXPECT_EQ(output.water_indices, 128u * 128u * 6);
    FarWorldFrozenWaterObservation const missing(frozen, {1, 1, -1});
    EXPECT_EQ(missing.WaterCapability(), FarWorldWaterCapability::Unavailable);
    const auto region = SmallRegion();
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), frozen),
              Error::None);
    FarWorldFrozenWaterObservation const partial(frozen, {1, 0, 0});
    EXPECT_EQ(partial.WaterCapability(), FarWorldWaterCapability::Unavailable);
}
