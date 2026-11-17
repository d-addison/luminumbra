#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarFrozenObservation.h"
#include "luminumbra_common/world/FarLodStore.h"
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
#include <string>
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

TEST(FarFrozenObservation, SessionRenderResolvesPublishedTerrainAndWaterWithoutSaving) {
    auto source = Source();
    source.use = FarFrozenUse::SessionRender;
    const auto terrain = [](std::int64_t selected, float density) {
        return [selected, density](std::uint8_t, const FarVolumePosition& p) {
            return FarFrozenOverride<FarVolumeSample>{p.x <= selected ? FarFrozenPresence::Present
                                                                      : FarFrozenPresence::Absent,
                                                      {density, 9}};
        };
    };
    const auto water = [](std::int64_t selected, float height) {
        return [selected, height](std::int64_t x, std::int64_t) {
            return FarFrozenOverride<FarFrozenWaterSample>{
                x <= selected ? FarFrozenPresence::Present : FarFrozenPresence::Absent,
                {true, height}};
        };
    };
    source.live = terrain(0, -10.0f);
    source.parked = terrain(4, -20.0f);
    source.committed = terrain(8, -30.0f);
    source.live_water = water(0, 10.0f);
    source.parked_water = water(4, 20.0f);
    source.water = [](std::int64_t x, std::int64_t) {
        return FarFrozenWaterSample{x == 8, x == 8 ? 30.0f : 0.0f};
    };
    const FarFrozenRegion region{1, {{0, 0, 0}, {12, 0, 0}}};
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), frozen),
              Error::None);
    EXPECT_EQ(frozen->use(), FarFrozenUse::SessionRender);
    EXPECT_EQ(frozen->durable_identity().generation, 7u);
    const std::array<float, 4> densities{-10.0f, -20.0f, -30.0f, -0.25f};
    const std::array<float, 4> heights{10.0f, 20.0f, 30.0f, 0.0f};
    for (std::size_t i = 0; i < densities.size(); ++i) {
        const auto x = static_cast<std::int64_t>(i) * 4;
        const auto sample = frozen->Sample(1, {x, 0, 0});
        const auto wet = frozen->WaterSample(0, x, 0);
        ASSERT_TRUE(sample);
        ASSERT_TRUE(wet);
        if (!sample || !wet) {
            return;
        }
        EXPECT_EQ(sample->density, densities[i]);
        EXPECT_EQ(wet->wet, i != 3);
        EXPECT_EQ(wet->surface_height, heights[i]);
    }
    const auto fingerprint = frozen->content_identity();
    // Ownership changes preserve the resolved observations, including water.
    source.parked = [live = source.live, parked = source.parked](auto tier, const auto& p) {
        const auto value = live(tier, p);
        return value.presence == FarFrozenPresence::Absent ? parked(tier, p) : value;
    };
    source.parked_water = [live = source.live_water, parked = source.parked_water](auto x, auto z) {
        const auto value = live(x, z);
        return value.presence == FarFrozenPresence::Absent ? parked(x, z) : value;
    };
    source.live = {};
    source.live_water = {};
    std::shared_ptr<const FarFrozenObservation> parked;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), parked),
              Error::None);
    EXPECT_EQ(parked->content_identity(), fingerprint);
    source.parked = Constant(100.0f);
    source.parked_water = water(0, 100.0f);
    EXPECT_EQ(frozen->content_identity(), fingerprint);
    EXPECT_EQ(frozen->Sample(1, {0, 0, 0}), parked->Sample(1, {0, 0, 0}));
    EXPECT_EQ(frozen->WaterSample(0, 0, 0), parked->WaterSample(0, 0, 0));
}

TEST(FarFrozenObservation, SessionRenderRefusesExpectedPartialOrInvalidAuthorityTransactionally) {
    auto pool = Pool();
    const FarFrozenRegion region{1, {{0, 0, 0}, {0, 0, 0}}};
    std::shared_ptr<const FarFrozenObservation> output;
    auto source = Source();
    source.use = FarFrozenUse::SessionRender;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::None);
    const auto held = output;
    const auto used = pool->used();
    for (unsigned layer = 0; layer < 5; ++layer) {
        for (const auto presence :
             {FarFrozenPresence::Unavailable, static_cast<FarFrozenPresence>(255)}) {
            source = Source();
            source.use = FarFrozenUse::SessionRender;
            if (layer < 3) {
                auto* callback = layer == 0   ? &source.live
                                 : layer == 1 ? &source.parked
                                              : &source.committed;
                *callback = [presence](std::uint8_t, const FarVolumePosition& p) {
                    return FarFrozenOverride<FarVolumeSample>{
                        p.x == 0 ? presence : FarFrozenPresence::Absent, {}};
                };
            } else {
                auto* callback = layer == 3 ? &source.live_water : &source.parked_water;
                *callback = [presence](std::int64_t x, std::int64_t) {
                    return FarFrozenOverride<FarFrozenWaterSample>{
                        x == 0 ? presence : FarFrozenPresence::Absent, {}};
                };
            }
            EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
                      presence == FarFrozenPresence::Unavailable ? Error::Unavailable
                                                                 : Error::InvalidSource);
            EXPECT_EQ(output, held);
            EXPECT_EQ(pool->used(), used);
        }
    }
    source = Source();
    source.use = FarFrozenUse::SessionRender;
    source.water = [](std::int64_t, std::int64_t) {
        return std::optional<FarFrozenWaterSample>{};
    };
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::Unavailable);
    EXPECT_EQ(output, held);
    EXPECT_EQ(pool->used(), used);
    source = Source();
    source.use = FarFrozenUse::SessionRender;
    source.durable.state = FarEditedImageState::Prepared;
    const auto attempts = pool->attempts();
    EXPECT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), pool, output),
              Error::IdentityMismatch);
    EXPECT_EQ(output, held);
    EXPECT_EQ(pool->used(), used);
    EXPECT_EQ(pool->attempts(), attempts);
}

TEST(FarFrozenObservation, SessionRenderLeavesCommittedBytesAndFingerprintUnchanged) {
    auto source = Source();
    const FarFrozenRegion region{1, {{0, 0, 0}, {0, 0, 0}}};
    std::shared_ptr<const FarFrozenObservation> committed, session, recaptured;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), committed),
              Error::None);
    source.live = Constant(-10.0f);
    source.parked = Constant(-20.0f);
    source.live_water = [](std::int64_t, std::int64_t) {
        return FarFrozenOverride<FarFrozenWaterSample>{FarFrozenPresence::Present, {true, 12.0f}};
    };
    source.use = FarFrozenUse::SessionRender;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), session),
              Error::None);
    EXPECT_NE(session->content_identity(), committed->content_identity());
    source.use = FarFrozenUse::Committed;
    ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, Limits(), Pool(), recaptured),
              Error::None);
    EXPECT_EQ(committed->content_identity(), 11243690055911358406ULL);
    EXPECT_EQ(recaptured->content_identity(), committed->content_identity());
    for (std::int64_t z = -4; z <= 4; z += 4) {
        for (std::int64_t x = -4; x <= 4; x += 4) {
            EXPECT_EQ(recaptured->WaterSample(0, x, z), committed->WaterSample(0, x, z));
            for (std::int64_t y = -4; y <= 4; y += 4) {
                const auto before = committed->Sample(1, {x, y, z});
                const auto after = recaptured->Sample(1, {x, y, z});
                ASSERT_TRUE(before);
                ASSERT_TRUE(after);
                if (!before || !after) {
                    return;
                }
                EXPECT_EQ(std::bit_cast<std::uint32_t>(after->density),
                          std::bit_cast<std::uint32_t>(before->density));
                EXPECT_EQ(after->material, before->material);
            }
        }
    }
    EXPECT_EQ(session->durable_identity().generation, committed->durable_identity().generation);
}

TEST(FarFrozenObservation, SessionRenderCannotReplaceDurableBakeEvenWithMatchingIdentity) {
    auto source = Source();
    std::array<FarFrozenRegion, kFarTierCount> regions;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        regions[i] = SmallRegion(static_cast<std::uint8_t>(i + 1));
    }
    std::shared_ptr<const FarFrozenObservation> frozen;
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), frozen), Error::None);
    EditedImage edited;
    const FarEditedFootprint changed{{0, 0, 0}, {1, 1, 1}};
    FarEditedBakeResult baked;
    ASSERT_EQ(BakeFarEditedTilesFrozen(edited, {&changed, 1}, *frozen, BakeLimits(), baked),
              FarEditedBakeError::None);
    const auto bytes = baked.tiles.front().bytes;
    const auto count = baked.tiles.size();
    source.use = FarFrozenUse::SessionRender;
    source.live = Constant(-50.0f);
    ASSERT_EQ(CaptureFarFrozenObservation(regions, source, Limits(), Pool(), frozen), Error::None);
    EXPECT_EQ(BakeFarEditedTilesFrozen(edited, {&changed, 1}, *frozen, BakeLimits(), baked),
              FarEditedBakeError::IdentityMismatch);
    EXPECT_EQ(baked.tiles.size(), count);
    EXPECT_EQ(baked.tiles.front().bytes, bytes);
    EXPECT_EQ(baked.source.generation, 7u);
}

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
    for (const auto use :
         {FarFrozenUse::Committed, FarFrozenUse::Preview, FarFrozenUse::SessionRender}) {
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
    for (const auto use :
         {FarFrozenUse::Committed, FarFrozenUse::Preview, FarFrozenUse::SessionRender}) {
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
        const std::uint64_t per_region = 9ULL * (use != FarFrozenUse::Committed ? 3 : 1);
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

namespace {
struct RetainedObservationSource {
    std::array<FarFrozenRegion, 2> regions{
        {{1, {{8, 0, 0}, {8, 0, 0}}}, {1, {{0, 0, 0}, {0, 0, 0}}}}};
    bool current = true, retire_on_height = false;
    unsigned failure = 0;
    std::uint64_t callbacks = 0, retired_reads = 0, retire_at = 0;
    void Read() {
        ++callbacks;
        retired_reads += current ? 0u : 1u;
        if (callbacks == retire_at) {
            current = false;
        }
    }
    static bool Current(const void* context) noexcept {
        return static_cast<const RetainedObservationSource*>(context)->current;
    }
};
std::shared_ptr<FarFrozenCaptureInput>
RetainedInput(const std::shared_ptr<RetainedObservationSource>& owner) {
    auto input = std::make_shared<FarFrozenCaptureInput>();
    input->source = Source();
    input->source.use = FarFrozenUse::SessionRender;
    input->regions = owner->regions;
    input->owner = owner;
    input->current = RetainedObservationSource::Current;
    input->source.pristine.height = [source = owner.get()](std::int64_t,
                                                           std::int64_t) -> std::optional<float> {
        source->Read();
        if (source->retire_on_height) {
            source->current = false;
        }
        if (source->failure == 1) {
            return {};
        }
        if (source->failure == 2) {
            return std::numeric_limits<float>::infinity();
        }
        if (source->failure == 3) {
            throw std::runtime_error("unavailable observation");
        }
        return 0.25f;
    };
    input->source.pristine.density =
        [source = owner.get()](const FarVolumePosition& p, float h, std::uint32_t, FarCaveMode) {
            source->Read();
            return FarVolumeSample{float(p.y) - h, 3};
        };
    input->source.committed = input->source.parked =
        input->source.live = [source = owner.get()](std::uint8_t, const FarVolumePosition&) {
            source->Read();
            return FarFrozenOverride<FarVolumeSample>{};
        };
    input->source.water = [source = owner.get()](std::int64_t, std::int64_t) {
        source->Read();
        return FarFrozenWaterSample{false, 0};
    };
    input->source.live_water = input->source.parked_water = [source = owner.get()](std::int64_t,
                                                                                   std::int64_t) {
        source->Read();
        if (source->failure == 4) {
            return FarFrozenOverride<FarFrozenWaterSample>{FarFrozenPresence::Unavailable, {}};
        }
        if (source->failure == 5) {
            return FarFrozenOverride<FarFrozenWaterSample>{static_cast<FarFrozenPresence>(255), {}};
        }
        return FarFrozenOverride<FarFrozenWaterSample>{};
    };
    return input;
}
void ExpectSameObservation(const FarFrozenObservation& actual,
                           const FarFrozenObservation& expected) {
    EXPECT_EQ(actual.content_identity(), expected.content_identity());
    EXPECT_EQ(actual.payload_bytes(), expected.payload_bytes());
    ASSERT_EQ(actual.region_count(), expected.region_count());
    for (std::size_t i = 0; i < actual.region_count(); ++i) {
        const auto region = actual.Region(i);
        ASSERT_TRUE(region.has_value());
        if (!region.has_value()) {
            return;
        }
        const auto tier = FarTierAt(region->tier);
        ASSERT_TRUE(tier.has_value());
        if (!tier.has_value()) {
            return;
        }
        const auto step = std::int64_t(tier->sample_spacing_meters);
        EXPECT_EQ(actual.WaterBound(i), expected.WaterBound(i));
        for (auto z = region->support.min.z - step; z <= region->support.max.z + step; z += step) {
            for (auto x = region->support.min.x - step; x <= region->support.max.x + step;
                 x += step) {
                EXPECT_EQ(actual.Height(region->tier, x, z), expected.Height(region->tier, x, z));
                EXPECT_EQ(actual.WaterSample(i, x, z), expected.WaterSample(i, x, z));
                for (auto y = region->support.min.y - step; y <= region->support.max.y + step;
                     y += step) {
                    for (const bool pristine : {false, true}) {
                        EXPECT_EQ(actual.Sample(region->tier, {x, y, z}, pristine),
                                  expected.Sample(region->tier, {x, y, z}, pristine));
                    }
                }
            }
        }
    }
}

TEST(FarFrozenObservation, RetainedPumpsBoundCallbacksAndMatchEverySynchronousSample) {
    for (const auto use :
         {FarFrozenUse::Committed, FarFrozenUse::Preview, FarFrozenUse::SessionRender}) {
        for (const std::uint64_t budget : {1u, 7u, 64u}) {
            auto owner = std::make_shared<RetainedObservationSource>();
            auto input = RetainedInput(owner);
            input->source.use = use;
            std::shared_ptr<const FarFrozenObservation> expected;
            ASSERT_EQ(CaptureFarFrozenObservation(
                          input->regions, input->source, Limits(), Pool(), expected),
                      Error::None);
            auto pool = Pool();
            FarFrozenObservation::Capture capture;
            capture.Start(input, Limits(), pool);
            const std::weak_ptr<const FarFrozenCaptureInput> retained = input;
            input.reset();
            auto output = expected;
            EXPECT_EQ(capture.Pump(0, output).work, 0u);
            EXPECT_EQ(output, expected);
            bool complete = false, yielded = false;
            for (unsigned pump = 0; pump < 1024 && !complete; ++pump) {
                const auto before = owner->callbacks;
                const auto result = capture.Pump(budget, output);
                ASSERT_EQ(result.error, Error::None);
                EXPECT_LE(result.work, budget);
                EXPECT_LE(owner->callbacks - before, 6 * result.work);
                complete = result.complete;
                if (!complete) {
                    yielded = true;
                    EXPECT_EQ(output, expected);
                }
            }
            ASSERT_TRUE(complete);
            EXPECT_TRUE(yielded);
            EXPECT_FALSE(capture.active());
            EXPECT_TRUE(retained.expired());
            EXPECT_EQ(owner->retired_reads, 0u);
            ExpectSameObservation(*output, *expected);
        }
    }
}

TEST(FarFrozenObservation, CancelAndReplacementReleaseAllPartialOwnershipAndCharges) {
    FarFrozenObservation::Capture capture;
    auto pool = Pool();
    std::shared_ptr<const FarFrozenObservation> output;
    std::weak_ptr<RetainedObservationSource> lifetime;
    {
        auto owner = std::make_shared<RetainedObservationSource>();
        lifetime = owner;
        capture.Start(RetainedInput(owner), Limits(), pool);
    }
    const auto result = capture.Pump(12, output);
    ASSERT_EQ(result.error, Error::None);
    EXPECT_FALSE(result.complete);
    EXPECT_FALSE(output);
    EXPECT_GT(pool->used(), 0u);
    EXPECT_FALSE(lifetime.expired());
    capture.Cancel();
    EXPECT_FALSE(capture.active());
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(pool->used(), 0u);
    auto owner = std::make_shared<RetainedObservationSource>();
    capture.Start(RetainedInput(owner), Limits(), pool);
    EXPECT_FALSE(capture.Pump(12, output).complete);
    capture.Start(RetainedInput(owner), Limits(), pool);
    EXPECT_EQ(pool->used(), 0u);
    const auto completed = capture.Pump(1024, output);
    EXPECT_EQ(completed.error, Error::None);
    EXPECT_TRUE(completed.complete);
    EXPECT_FALSE(capture.active());
    EXPECT_TRUE(output);
}

TEST(FarFrozenObservation, MidCaptureRefusalsDiscardAndRestartWithoutPublishing) {
    for (unsigned failure = 1; failure <= 5; ++failure) {
        auto owner = std::make_shared<RetainedObservationSource>();
        auto input = RetainedInput(owner);
        auto pool = Pool();
        FarFrozenObservation::Capture capture;
        std::shared_ptr<const FarFrozenObservation> output;
        capture.Start(input, Limits(), pool);
        ASSERT_EQ(capture.Pump(12, output).error, Error::None);
        owner->failure = failure;
        const auto refused = capture.Pump(64, output);
        EXPECT_EQ(refused.error,
                  failure == 2   ? Error::NonFiniteSample
                  : failure == 5 ? Error::InvalidSource
                                 : Error::Unavailable);
        EXPECT_FALSE(output);
        EXPECT_FALSE(capture.active());
        EXPECT_EQ(pool->used(), 0u);
        owner->failure = 0;
        capture.Start(input, Limits(), pool);
        const auto completed = capture.Pump(1024, output);
        EXPECT_EQ(completed.error, Error::None);
        EXPECT_TRUE(completed.complete);
    }
}

TEST(FarFrozenObservation, ChangedSourcesAreFencedBetweenPumpsAndBetweenCallbacks) {
    for (const bool during_callback : {false, true}) {
        auto owner = std::make_shared<RetainedObservationSource>();
        auto input = RetainedInput(owner);
        auto pool = Pool();
        FarFrozenObservation::Capture capture;
        std::shared_ptr<const FarFrozenObservation> output;
        capture.Start(input, Limits(), pool);
        ASSERT_EQ(capture.Pump(8, output).error, Error::None);
        const auto before = owner->callbacks;
        owner->retire_on_height = during_callback;
        owner->current = during_callback;
        const auto refused = capture.Pump(64, output);
        EXPECT_EQ(refused.error, Error::IdentityMismatch);
        EXPECT_EQ(owner->callbacks - before, during_callback ? 1u : 0u);
        EXPECT_EQ(owner->retired_reads, 0u);
        EXPECT_FALSE(output);
        EXPECT_FALSE(capture.active());
        EXPECT_EQ(pool->used(), 0u);
        owner->current = true;
        owner->retire_on_height = false;
        capture.Start(input, Limits(), pool);
        EXPECT_TRUE(capture.Pump(1024, output).complete);
    }
}

TEST(FarFrozenObservation, EveryLayerFencesRetirementBeforeTheFollowingCallback) {
    // Nine columns precede density in each region: four callbacks per column.
    for (const std::uint64_t retire_at : {1u, 2u, 3u, 4u, 37u, 38u, 39u, 40u}) {
        auto owner = std::make_shared<RetainedObservationSource>();
        owner->retire_at = retire_at;
        auto input = RetainedInput(owner);
        auto pool = Pool();
        FarFrozenObservation::Capture capture;
        std::shared_ptr<const FarFrozenObservation> output;
        capture.Start(input, Limits(), pool);
        const auto result = capture.Pump(1024, output);
        EXPECT_EQ(result.error, Error::IdentityMismatch);
        EXPECT_EQ(owner->callbacks, retire_at);
        EXPECT_EQ(owner->retired_reads, 0u);
        EXPECT_FALSE(output);
        EXPECT_FALSE(capture.active());
        EXPECT_EQ(pool->used(), 0u);
    }
}

TEST(FarFrozenObservation, MissingRetainedInputsRefuseWithoutSourceReads) {
    auto owner = std::make_shared<RetainedObservationSource>();
    for (unsigned missing = 0; missing < 3; ++missing) {
        auto input = RetainedInput(owner);
        if (missing == 0) {
            input.reset();
        } else if (missing == 1) {
            input->owner.reset();
        } else {
            input->current = nullptr;
        }
        FarFrozenObservation::Capture capture;
        std::shared_ptr<const FarFrozenObservation> output;
        auto pool = Pool();
        capture.Start(input, Limits(), pool);
        EXPECT_EQ(capture.Pump(1, output).error,
                  missing == 0 ? Error::InvalidSource : Error::IdentityMismatch);
        EXPECT_FALSE(capture.active());
        EXPECT_FALSE(output);
        EXPECT_EQ(pool->used(), 0u);
        EXPECT_EQ(owner->callbacks, 0u);
    }
}

TEST(FarFrozenObservation, WorldIdentityPumpsSplineAndDependencyBytesAndRejectsGeneratorChanges) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.caves_enabled = false;
    auto dependencies = std::make_shared<WorldgenDependencySnapshot>();
    dependencies->manifest = {{}, {"", std::string(129, 'x'), "\xc3\xa9"}};
    params.worldgen_dependencies = dependencies;
    params.continental_spline.clear();
    for (int i = 0; i < 65; ++i) {
        params.continental_spline.push_back(
            {static_cast<float>(i) / 32 - 1, static_cast<float>(i)});
    }
    auto world =
        std::make_shared<Luminumbra::Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
    auto source = Source();
    source.use = FarFrozenUse::SessionRender;
    source.session_water_resolution = 4;
    source.pristine.field_identity =
        WorldFarVolumeIdentity(*world, source.pristine.caves, source.pristine.numeric, 4);
    FarFrozenWorldSource cursor;
    cursor.Start(*world, source);
    bool complete = false;
    std::uint64_t work = 0;
    for (unsigned pump = 0; pump < 256 && !complete; ++pump) {
        const auto result = cursor.Pump(1);
        ASSERT_EQ(result.error, Error::None);
        EXPECT_LE(result.work, 1u);
        work += result.work;
        complete = result.complete;
    }
    EXPECT_TRUE(complete);
    EXPECT_GE(work, 65u + 129u / 8u);
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        auto rejected = source;
        if (invalid == 0) {
            rejected.pristine.field_identity = 0;
        } else if (invalid == 1) {
            rejected.pristine.caves = static_cast<FarCaveMode>(255);
        } else if (invalid == 2) {
            rejected.pristine.numeric = static_cast<FarVolumeNumericProfile>(255);
        } else if (invalid == 3) {
            rejected.session_water_resolution = 1;
        } else {
            rejected.pristine.field_identity ^= 1;
        }
        cursor.Start(*world, rejected);
        EXPECT_EQ(cursor.Pump(256).error, Error::IdentityMismatch);
    }
    cursor.Start(*world, source);
    EXPECT_FALSE(cursor.Pump(1).complete);
    world->set_seed(2);
    EXPECT_EQ(cursor.Pump(1).error, Error::IdentityMismatch);
}
} // namespace
