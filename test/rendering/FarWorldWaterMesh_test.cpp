#include "luminumbra_client/rendering/FarWorldWaterMesh.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;
FarVolumeRenderMesh Terrain(std::int64_t tx = 0) {
    FarVolumeRenderMesh mesh;
    mesh.request = {{1, tx, -1}, {-32, 32}, 42};
    mesh.origin_meters = {tx * 512, -32, -512};
    mesh.sampled_max_meters = {tx * 512 + 512, 32, 0};
    return mesh;
}
FarWorldWaterLimits Limits() {
    return {16641, 16384, 100000, 200000, 4 * 1024 * 1024};
}
class Lake final : public FarWorldWaterObservation {
public:
    std::uint64_t content_identity() const noexcept override {
        return 42;
    }
    FarWorldWaterCapability WaterCapability() const noexcept override {
        return FarWorldWaterCapability::Wet;
    }
    std::optional<FarWorldWaterBound> WaterBound(const FarTileKey&) const override {
        return FarWorldWaterBound{false, -10, 10};
    }
    std::optional<FarWorldWaterSample> WaterSample(std::int64_t x, std::int64_t z) const override {
        return FarWorldWaterSample{x == 0, float(double(x) / 1024 + double(z) / 2048)};
    }
};
} // namespace

TEST(FarWorldWaterMesh, KnownDryCompletesEmptyAndWetCompletesPresentationMixed) {
    FarVolumeMemoryPool pool(8 * 1024 * 1024);
    FarVolumeMemoryLease retained(pool);
    FarVolumeRenderMesh result;
    FarWorldWaterError error;
    FarWorldConstantWaterObservation dry(42, false, 0), sea(42, true, SEA_LEVEL);
    ASSERT_TRUE(BuildFarWorldWaterMesh(
        Terrain(), dry, {true, 0, 0}, Limits(), {&pool, &retained, nullptr}, result, &error));
    EXPECT_TRUE(result.water_complete);
    EXPECT_TRUE(result.water_known_dry);
    EXPECT_TRUE(result.indices.empty());
    ASSERT_TRUE(BuildFarWorldWaterMesh(Terrain(),
                                       sea,
                                       {false, SEA_LEVEL, SEA_LEVEL},
                                       Limits(),
                                       {&pool, &retained, nullptr},
                                       result,
                                       &error));
    EXPECT_FALSE(result.water_known_dry);
    EXPECT_EQ(result.water_indices, 16384u * 6);
    EXPECT_EQ(result.terrain_indices, 0u);
    EXPECT_EQ(result.terrain_content, FarVolumeContent::Air);
    EXPECT_EQ(result.content, FarVolumeContent::Mixed);
    EXPECT_EQ(result.vertices.size(), 16384u * 4);
    EXPECT_EQ(result.vertices.front().position.y + float(result.origin_meters.y), SEA_LEVEL);
    EXPECT_EQ(result.vertices.front().material_id, 200u);
    EXPECT_EQ(pool.used(), retained.bytes());
}

TEST(FarWorldWaterMesh, AnyWetCornerSharedNegativeBorderVariableLevelsAndUpwardWinding) {
    FarVolumeMemoryPool pool(8 * 1024 * 1024);
    FarVolumeMemoryLease left_charge(pool), right_charge(pool);
    FarVolumeRenderMesh left, right;
    Lake lake;
    ASSERT_TRUE(BuildFarWorldWaterMesh(
        Terrain(-1), lake, {false, -10, 10}, Limits(), {&pool, &left_charge, nullptr}, left));
    ASSERT_TRUE(BuildFarWorldWaterMesh(
        Terrain(0), lake, {false, -10, 10}, Limits(), {&pool, &right_charge, nullptr}, right));
    ASSERT_EQ(left.water_indices, 128u * 6);
    ASSERT_EQ(right.water_indices, left.water_indices);
    for (std::size_t cell = 0; cell < 128; ++cell) {
        for (std::size_t border = 0; border < 2; ++border) {
            const auto& a = left.vertices[cell * 4 + 2 + border].position;
            const auto& b = right.vertices[cell * 4 + border].position;
            EXPECT_EQ(a.x + float(left.origin_meters.x), b.x);
            EXPECT_EQ(a.y, b.y);
            EXPECT_EQ(a.z, b.z);
        }
    }
    for (std::size_t i = 0; i < left.indices.size(); i += 3) {
        const auto a = left.vertices[left.indices[i]].position;
        const auto b = left.vertices[left.indices[i + 1]].position;
        const auto c = left.vertices[left.indices[i + 2]].position;
        EXPECT_GT(glm::cross(b - a, c - a).y, 0);
    }
    EXPECT_NE(left.vertices.front().position.y, left.vertices.back().position.y);
}

TEST(FarWorldWaterMesh, WorkByteSpanAndAllocationRefusalsPreserveOldOutput) {
    for (int refusal = 0; refusal < 6; ++refusal) {
        FarVolumeMemoryPool pool(8 * 1024 * 1024);
        FarVolumeMemoryLease retained(pool);
        FarVolumeRenderMesh result;
        FarWorldConstantWaterObservation water(42, true, 0);
        ASSERT_TRUE(BuildFarWorldWaterMesh(
            Terrain(), water, {false, 0, 0}, Limits(), {&pool, &retained, nullptr}, result));
        const auto old_bytes = retained.bytes();
        auto limits = Limits();
        auto bound = FarWorldWaterBound{false, 0, 0};
        FarWorldWaterError expected = FarWorldWaterError::WorkLimit, error;
        if (refusal == 0)
            --limits.samples;
        if (refusal == 1)
            --limits.cells;
        if (refusal == 2)
            limits.vertices = 3;
        if (refusal == 3) {
            limits.bytes = 1;
            expected = FarWorldWaterError::ByteLimit;
        }
        if (refusal == 4) {
            bound.max_y = 33;
            expected = FarWorldWaterError::SpanRefused;
        }
        if (refusal == 5) {
            pool.RefuseAttemptsFrom(pool.attempts());
            expected = FarWorldWaterError::AllocationFailure;
        }
        EXPECT_FALSE(BuildFarWorldWaterMesh(
            Terrain(), water, bound, limits, {&pool, &retained, nullptr}, result, &error));
        EXPECT_EQ(error, expected);
        EXPECT_EQ(result.water_indices, 16384u * 6);
        EXPECT_EQ(retained.bytes(), old_bytes);
        EXPECT_EQ(pool.used(), old_bytes);
    }
}

TEST(FarWorldWaterMesh, UnavailableAndNonfiniteAreNeverDry) {
    class Refused final : public FarWorldWaterObservation {
    public:
        std::uint64_t content_identity() const noexcept override {
            return 42;
        }
    } unavailable;
    FarVolumeMemoryPool pool(8 * 1024 * 1024);
    FarVolumeMemoryLease retained(pool);
    FarVolumeRenderMesh result;
    FarWorldWaterError error;
    EXPECT_FALSE(BuildFarWorldWaterMesh(Terrain(),
                                        unavailable,
                                        {true, 0, 0},
                                        Limits(),
                                        {&pool, &retained, nullptr},
                                        result,
                                        &error));
    EXPECT_EQ(error, FarWorldWaterError::Unavailable);
    EXPECT_FALSE(result.water_complete);
    FarWorldConstantWaterObservation invalid(42, true, std::numeric_limits<float>::infinity());
    EXPECT_FALSE(BuildFarWorldWaterMesh(
        Terrain(), invalid, {false, 0, 0}, Limits(), {&pool, &retained, nullptr}, result, &error));
    EXPECT_EQ(error, FarWorldWaterError::InvalidObservation);
    EXPECT_EQ(pool.used(), 0u);
}

TEST(FarWorldWaterMesh, CombinedGeometryChecksTotalsOffsetsAndOldNewPeakCharges) {
    FarVolumeMemoryPool pool(16 * 1024 * 1024);
    FarVolumeMemoryLease terrain_charge(pool), combined_charge(pool);
    FarVolumeMesh source;
    source.request = {{1, 0, -1}, {-32, 32}, 42};
    source.origin_meters = {0, -32, -512};
    source.sampled_max_meters = {512, 32, 0};
    source.content = FarVolumeContent::Mixed;
    source.vertices = {
        {{0, 0, 0}, {0, 1, 0}, 1}, {{0, 0, 4}, {0, 1, 0}, 1}, {{4, 0, 0}, {0, 1, 0}, 1}};
    source.indices = {0, 1, 2};
    FarVolumeRenderMesh terrain, combined;
    ASSERT_TRUE(AdaptFarVolumeRenderMesh(source, 4096, {&pool, &terrain_charge, nullptr}, terrain));
    FarWorldConstantWaterObservation water(42, true, 0);
    auto limits = Limits();
    limits.vertices = 65536;
    FarWorldWaterError error;
    EXPECT_FALSE(BuildFarWorldWaterMesh(terrain,
                                        water,
                                        {false, 0, 0},
                                        limits,
                                        {&pool, &combined_charge, nullptr},
                                        combined,
                                        &error));
    EXPECT_EQ(error, FarWorldWaterError::WorkLimit);
    limits = Limits();
    limits.indices = 98304;
    EXPECT_FALSE(BuildFarWorldWaterMesh(terrain,
                                        water,
                                        {false, 0, 0},
                                        limits,
                                        {&pool, &combined_charge, nullptr},
                                        combined,
                                        &error));
    EXPECT_EQ(error, FarWorldWaterError::WorkLimit);
    ASSERT_TRUE(BuildFarWorldWaterMesh(
        terrain, water, {false, 0, 0}, Limits(), {&pool, &combined_charge, nullptr}, combined));
    EXPECT_EQ(combined.terrain_indices, 3u);
    EXPECT_EQ(combined.water_indices, 98304u);
    EXPECT_EQ(combined.indices.size(), 98307u);
    EXPECT_EQ(combined.indices[3], 3u);
    EXPECT_EQ(combined.vertices[0].material_id, 1u);
    EXPECT_EQ(combined.vertices[3].material_id, 200u);
    EXPECT_EQ(pool.used(), terrain_charge.bytes() + combined_charge.bytes());
    EXPECT_GE(pool.peak(), pool.used() + kFarWorldWaterSamples * sizeof(FarWorldWaterSample));
}
