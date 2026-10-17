#include "luminumbra_client/rendering/FarVolumeRenderMesh.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "luminumbra_common/world/FarEditedBake.h"
#include "luminumbra_common/world/FarFrozenObservation.h"
#include "luminumbra_common/world/FarTierTable.h"
#include "luminumbra_common/world/FarVolume.h"
#include "luminumbra_common/world/FarVolumeMemory.h"
#include "luminumbra_common/world/FarVolumeTile.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

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
} // namespace

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
