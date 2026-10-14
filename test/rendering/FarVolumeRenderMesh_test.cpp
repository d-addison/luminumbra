#include "luminumbra_client/rendering/FarVolumeRenderMesh.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;

FarVolumeMesh RenderSource() {
    FarVolumeMesh source;
    source.request = {{1, -2, 3}, {-32, 32}, 42};
    source.origin_meters = {-1024, -32, 1536};
    source.sampled_max_meters = {-512, 32, 2048};
    source.content = FarVolumeContent::Mixed;
    // Shared diagonal and different canonical normals must survive conversion.
    source.vertices = {{{0, 0, 0}, {0, 1, 0}, 4},
                       {{4, 0, 0}, {.6, .8, 0}, 5},
                       {{4, 4, 0}, {0, .8, .6}, 6},
                       {{0, 4, 0}, {0, 1, 0}, 7}};
    source.indices = {0, 1, 2, 0, 2, 3};
    return source;
}
void ExpectSameOutput(const FarVolumeRenderMesh& actual, const FarVolumeRenderMesh& expected) {
    EXPECT_EQ(actual.request, expected.request);
    EXPECT_EQ(actual.origin_meters, expected.origin_meters);
    EXPECT_EQ(actual.sampled_max_meters, expected.sampled_max_meters);
    EXPECT_EQ(actual.content, expected.content);
    ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
    for (std::size_t i = 0; i < actual.vertices.size(); ++i) {
        EXPECT_EQ(actual.vertices[i].position, expected.vertices[i].position);
        EXPECT_EQ(actual.vertices[i].normal, expected.vertices[i].normal);
        EXPECT_EQ(actual.vertices[i].material_id, expected.vertices[i].material_id);
    }
    EXPECT_EQ(actual.indices, expected.indices);
    ASSERT_EQ(actual.bounds.has_value(), expected.bounds.has_value());
    if (actual.bounds && expected.bounds) {
        EXPECT_EQ(actual.bounds->min, expected.bounds->min);
        EXPECT_EQ(actual.bounds->max, expected.bounds->max);
    }
}
} // namespace

TEST(FarVolumeRenderMesh, PreservesRelativeSharedGeometryNormalsAndMaterial) {
    const auto source = RenderSource();
    FarVolumeMemoryPool pool(4096);
    FarVolumeMemoryLease retained(pool);
    FarVolumeRenderMesh output;
    FarVolumeRenderError error;
    ASSERT_TRUE(
        AdaptFarVolumeRenderMesh(source, 4096, {&pool, &retained, nullptr}, output, &error));
    EXPECT_EQ(error, FarVolumeRenderError::None);
    EXPECT_EQ(output.terrain_content, source.content);
    EXPECT_EQ(output.terrain_indices, source.indices.size());
    EXPECT_EQ(output.water_indices, 0u);
    EXPECT_FALSE(output.water_complete);
    EXPECT_EQ(output.request, source.request);
    EXPECT_EQ(output.origin_meters, source.origin_meters);
    EXPECT_EQ(output.sampled_max_meters, source.sampled_max_meters);
    ASSERT_EQ(output.vertices.size(), 4u);
    EXPECT_EQ(output.indices, source.indices);
    EXPECT_EQ(output.vertices[0].position, Vec3(0, 0, 0));
    EXPECT_EQ(output.vertices[2].position, Vec3(4, 4, 0));
    EXPECT_EQ(output.vertices[1].normal, Vec3(.6f, .8f, 0));
    EXPECT_EQ(output.vertices[2].normal, Vec3(0, .8f, .6f));
    EXPECT_EQ(output.vertices[2].material_id, 6u);
    ASSERT_TRUE(output.bounds);
    if (output.bounds) {
        EXPECT_EQ(output.bounds->min, Vec3(0, 0, 0));
        EXPECT_EQ(output.bounds->max, Vec3(4, 4, 0));
    }
    EXPECT_EQ(retained.bytes(), FarVolumeRenderRetainedBytes(output));
    EXPECT_EQ(pool.used(), retained.bytes());
}

TEST(FarVolumeRenderMesh, RefusalAtEveryAdmissionPreservesPriorOutputAndLease) {
    const auto source = RenderSource();
    for (std::uint64_t admission = 0; admission < 2; ++admission) {
        FarVolumeMemoryPool pool(4096);
        FarVolumeMemoryLease retained(pool);
        FarVolumeRenderMesh output;
        ASSERT_TRUE(AdaptFarVolumeRenderMesh(source, 4096, {&pool, &retained, nullptr}, output));
        const auto previous = output;
        const auto held = retained.bytes();
        pool.RefuseAttemptsFrom(pool.attempts() + admission);
        FarVolumeRenderError error;
        EXPECT_FALSE(
            AdaptFarVolumeRenderMesh(source, 4096, {&pool, &retained, nullptr}, output, &error));
        EXPECT_EQ(error, FarVolumeRenderError::AllocationFailure);
        ExpectSameOutput(output, previous);
        EXPECT_EQ(retained.bytes(), held);
        EXPECT_EQ(pool.used(), held);
    }
}

TEST(FarVolumeRenderMesh, ReplacementPeakIncludesPriorAndNewBuffers) {
    const auto source = RenderSource();
    const auto bytes = source.vertices.size() * sizeof(VoxelVertex) +
                       source.indices.size() * sizeof(std::uint32_t);
    FarVolumeMemoryPool pool(bytes);
    FarVolumeMemoryLease retained(pool);
    FarVolumeRenderMesh output;
    ASSERT_TRUE(AdaptFarVolumeRenderMesh(source, bytes, {&pool, &retained, nullptr}, output));
    const auto previous = output;
    FarVolumeRenderError error;
    EXPECT_FALSE(
        AdaptFarVolumeRenderMesh(source, bytes, {&pool, &retained, nullptr}, output, &error));
    EXPECT_EQ(error, FarVolumeRenderError::AllocationFailure);
    EXPECT_EQ(pool.used(), bytes);
    ExpectSameOutput(output, previous);
}

TEST(FarVolumeRenderMesh, EmptyAirAndSolidRetainSpanAndReleasePriorGeometry) {
    for (const auto content : {FarVolumeContent::Air, FarVolumeContent::Solid}) {
        auto source = RenderSource();
        FarVolumeMemoryPool pool(4096);
        FarVolumeMemoryLease retained(pool);
        FarVolumeRenderMesh output;
        ASSERT_TRUE(AdaptFarVolumeRenderMesh(source, 4096, {&pool, &retained, nullptr}, output));
        source.content = content;
        source.vertices.clear();
        source.indices.clear();
        ASSERT_TRUE(AdaptFarVolumeRenderMesh(source, 0, {&pool, &retained, nullptr}, output));
        EXPECT_EQ(output.content, content);
        EXPECT_EQ(output.terrain_content, source.content);
        EXPECT_EQ(output.terrain_indices, source.indices.size());
        EXPECT_EQ(output.water_indices, 0u);
        EXPECT_FALSE(output.water_complete);
        EXPECT_EQ(output.request, source.request);
        EXPECT_EQ(output.origin_meters, source.origin_meters);
        EXPECT_EQ(output.sampled_max_meters, source.sampled_max_meters);
        EXPECT_TRUE(output.vertices.empty());
        EXPECT_TRUE(output.indices.empty());
        EXPECT_FALSE(output.bounds);
        EXPECT_EQ(retained.bytes(), 0u);
        EXPECT_EQ(pool.used(), 0u);
    }
}

TEST(FarVolumeRenderMesh, InvalidSourceAndByteLimitRefuseBeforeAllocation) {
    const auto original = RenderSource();
    FarVolumeMemoryPool pool(4096);
    FarVolumeMemoryLease retained(pool);
    FarVolumeRenderMesh output;
    ASSERT_TRUE(AdaptFarVolumeRenderMesh(original, 4096, {&pool, &retained, nullptr}, output));
    const auto previous = output;
    const auto attempts = pool.attempts();
    const auto held = retained.bytes();
    for (int mode = 0; mode < 8; ++mode) {
        auto source = original;
        if (mode == 0)
            source.indices[0] = 4;
        if (mode == 1)
            source.indices.pop_back();
        if (mode == 2)
            source.vertices[0].position[0] = std::numeric_limits<double>::infinity();
        if (mode == 3)
            source.vertices[0].normal = {0, 0, 0};
        if (mode == 4)
            source.vertices[0].normal[1] = std::numeric_limits<double>::max();
        if (mode == 5)
            source.origin_meters.x += 1;
        if (mode == 6)
            source.vertices[0].position[2] = 513;
        if (mode == 7)
            source.content = FarVolumeContent::Air;
        FarVolumeRenderError error;
        EXPECT_FALSE(
            AdaptFarVolumeRenderMesh(source, 4096, {&pool, &retained, nullptr}, output, &error));
        EXPECT_EQ(error, FarVolumeRenderError::InvalidSource);
        ExpectSameOutput(output, previous);
        EXPECT_EQ(pool.attempts(), attempts);
        EXPECT_EQ(pool.used(), held);
    }
    FarVolumeRenderError error;
    EXPECT_FALSE(
        AdaptFarVolumeRenderMesh(original, 1, {&pool, &retained, nullptr}, output, &error));
    EXPECT_EQ(error, FarVolumeRenderError::ByteLimit);
    EXPECT_EQ(pool.attempts(), attempts);
    ExpectSameOutput(output, previous);
    EXPECT_FALSE(AdaptFarVolumeRenderMesh(original, 4096, {}, output, &error));
    EXPECT_EQ(error, FarVolumeRenderError::InvalidMemory);
    EXPECT_FALSE(
        AdaptFarVolumeRenderMesh(original, 4096, {&pool, nullptr, nullptr}, output, &error));
    EXPECT_EQ(error, FarVolumeRenderError::InvalidMemory);
    EXPECT_EQ(pool.attempts(), attempts);
    EXPECT_EQ(pool.used(), held);
    ExpectSameOutput(output, previous);
}
