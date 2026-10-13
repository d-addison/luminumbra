#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarWorldDepthBias.h"
#include "luminumbra_client/rendering/FarWorldOwnership.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;

// Independent finite reversed-Z equation, derived from near=1 and far=0.
// No matrix construction, multiplication or production bias helper in oracle.
double ExpectedDepth(double axial_distance, double far_plane) {
    constexpr double near_plane = .1;
    return near_plane * (far_plane / axial_distance - 1.0) / (far_plane - near_plane);
}

glm::mat4 Projection(float far_plane) {
    return ReversedZPerspective(glm::radians(45.0f), 1.5f, .1f, far_plane);
}

TEST(FarWorldDepthBias, IndependentProjectedDepthAtBothPlanes) {
    for (const float far_plane : {kLegacyFarPlaneMeters, kFarPlaneMeters}) {
        const auto projection = Projection(far_plane);
        for (const float distance : {.1f, 176.0f, 1000.0f, far_plane - 32, far_plane}) {
            for (const float bias : {0.0f, 2.0f, 32.0f}) {
                SCOPED_TRACE(::testing::Message() << far_plane << ',' << distance << ',' << bias);
                FarWorldBiasedDepth result;
                const bool valid = distance + bias <= far_plane;
                ASSERT_EQ(ProjectFarWorldRayBias({0, 0, -distance}, bias, projection, result),
                          valid);
                if (valid) {
                    EXPECT_NEAR(result.depth, ExpectedDepth(distance + bias, far_plane), 1e-7);
                    EXPECT_NEAR(result.view_position.z, -distance - bias, .002);
                }
            }
        }
    }
}

TEST(FarWorldDepthBias, AwayDirectionAndFinerCoarserPriority) {
    const glm::vec3 point(120, -340, -1000);
    FarWorldBiasedDepth unbiased;
    ASSERT_TRUE(ProjectFarWorldRayBias(point, 0, Projection(kFarPlaneMeters), unbiased));
    float previous_depth = unbiased.depth;
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const auto dimensions = FarTierAt(tier);
        ASSERT_TRUE(dimensions);
        const float bias = static_cast<float>(dimensions->sample_spacing_meters) / 2;
        FarWorldBiasedDepth result;
        ASSERT_TRUE(ProjectFarWorldRayBias(point, bias, Projection(kFarPlaneMeters), result));
        EXPECT_NEAR(glm::length(result.view_position) - glm::length(point), bias, .0002);
        EXPECT_NEAR(
            glm::length(glm::cross(glm::normalize(point), glm::normalize(result.view_position))),
            0,
            1e-7);
        EXPECT_GT(glm::dot(result.view_position - point, point), 0);
        EXPECT_LT(result.depth, previous_depth); // GL_GREATER: finer coincident surface wins.
        previous_depth = result.depth;
    }
}

TEST(FarWorldDepthBias, ElevatedPitchedCamerasUseViewRayAndUnbiasedHorizontalClip) {
    for (const float pitch : {-35.0f, 27.0f}) {
        Camera camera({37.1f, 583.32f, 19.77f}, {0, 1, 0}, -70, pitch);
        camera.SelectFarWorldPlane(FarWorldMode::Ladder);
        const glm::vec3 relative(300, 600, -900);
        const auto point = glm::mat3(camera.GetViewMatrix()) * relative;
        FarWorldBiasedDepth result;
        ASSERT_TRUE(
            ProjectFarWorldRayBias(point, 32, camera.GetProjectionMatrix(900, 600), result));
        const double axial = -static_cast<double>(point.z) * (1 + 32.0 / glm::length(relative));
        EXPECT_NEAR(result.depth, ExpectedDepth(axial, kFarPlaneMeters), 1e-8);
        EXPECT_NEAR(glm::length(result.view_position), glm::length(relative) + 32, .0002);
    }
    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const double outer = FarWantedRadiusMeters(tier);
        EXPECT_TRUE(FarWorldFragmentInClip({0, 600, -outer}, outer));
        EXPECT_FALSE(FarWorldFragmentInClip({0, 600, -outer - .01}, outer));
        EXPECT_TRUE(FarWorldFragmentInClip({0, 176, 0}, outer));
        EXPECT_FALSE(FarWorldFragmentInClip({0, 175.99, 0}, outer));
    }
}

TEST(FarWorldDepthBias, ZeroNonfiniteAndInvalidProjectionRefuseWithoutPublication) {
    const auto projection = Projection(kFarPlaneMeters);
    FarWorldBiasedDepth result{{1, 2, 3}, .5f};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const auto& point : {glm::vec3(0),
                              glm::vec3(nan, 0, -200),
                              glm::vec3(0, inf, -200),
                              glm::vec3(0, 0, -inf),
                              glm::vec3(0, 0, 200),
                              glm::vec3(0, 0, -kFarPlaneMeters),
                              glm::vec3(0, 0, -std::numeric_limits<float>::denorm_min()),
                              glm::vec3(std::numeric_limits<float>::max(), 0, -200)}) {
        EXPECT_FALSE(ProjectFarWorldRayBias(point, 2, projection, result));
        EXPECT_EQ(result.view_position, glm::vec3(1, 2, 3));
        EXPECT_EQ(result.depth, .5f);
    }
    EXPECT_FALSE(ProjectFarWorldRayBias({0, 0, -.01f}, 0, projection, result));
    for (const float bias : {nan, inf, -1.0f})
        EXPECT_FALSE(ProjectFarWorldRayBias({0, 0, -200}, bias, projection, result));
    auto invalid = projection;
    invalid[0][0] = nan;
    EXPECT_FALSE(ProjectFarWorldRayBias({0, 0, -200}, 2, invalid, result));
    EXPECT_FALSE(ProjectFarWorldRayBias({0, 0, -200}, 2, glm::mat4(0), result));
}

TEST(FarWorldDepthBias, JitterPreservesScreenFootprintAndCameraMotion) {
    Camera camera({37, 583, 19}, {0, 1, 0}, -70, 27);
    camera.SelectFarWorldPlane(FarWorldMode::Ladder);
    auto projection = camera.GetProjectionMatrix(900, 600);
    const glm::vec2 jitter(.001f, -.002f);
    const auto unjittered = projection;
    projection[2][0] += jitter.x;
    projection[2][1] += jitter.y;
    const glm::vec3 world = camera.Position + camera.Front * 1000.0f + camera.Right * 80.0f;
    const auto point = glm::vec3(camera.GetViewMatrix() * glm::vec4(world, 1));
    FarWorldBiasedDepth result;
    ASSERT_TRUE(ProjectFarWorldRayBias(point, 32, projection, result));
    const auto raster = projection * glm::vec4(point, 1);
    const auto biased = projection * glm::vec4(result.view_position, 1);
    EXPECT_NEAR(raster.x / raster.w, biased.x / biased.w, 1e-7);
    EXPECT_NEAR(raster.y / raster.w, biased.y / biased.w, 1e-7);
    const auto previous = unjittered * camera.GetViewMatrix() * glm::vec4(world, 1);
    const auto motion = glm::vec2(raster) / raster.w + jitter - glm::vec2(previous) / previous.w;
    EXPECT_NEAR(motion.x, 0, 1e-6);
    EXPECT_NEAR(motion.y, 0, 1e-6);
    const auto inverse =
        glm::inverse(projection) * glm::vec4(glm::vec2(raster) / raster.w, result.depth, 1);
    EXPECT_NEAR(glm::length(glm::vec3(inverse) / inverse.w - result.view_position), 0, .001);
}

TEST(FarWorldDepthBias, FrameSelectedPlaneUsesEffectiveRendererAndPreviewPolicy) {
    Camera camera;
    EXPECT_EQ(camera.GetFarPlane(), 3200);
    for (const auto purpose :
         {FarWorldPurpose::World, FarWorldPurpose::Preview, FarWorldPurpose::OffscreenPreview}) {
        const auto selection = SelectFarWorld(FarWorldMode::Ladder, purpose, FarWorldRefusal::None);
        camera.SelectFarWorldPlane(selection.effective);
        const float expected = purpose == FarWorldPurpose::World ? 17408.0f : 3200.0f;
        ASSERT_EQ(camera.GetFarPlane(), expected);
        EXPECT_EQ(camera.GetNearPlane(), .1f);
        const auto projection = camera.GetProjectionMatrix(900, 600);
        const auto far_clip = projection * glm::vec4(0, 0, -expected, 1);
        EXPECT_NEAR(far_clip.z / far_clip.w, 0, 1e-10);
        // Production consumer uploads/frustum/capture are exercised in the
        // software-GL FarWorldPlaneCoherenceThroughProductionConsumers test.
    }
    const auto refused = SelectFarWorld(FarWorldMode::Ladder,
                                        FarWorldPurpose::World,
                                        FarWorldRefusal::ImmutableAuthorityUnavailable);
    camera.SelectFarWorldPlane(refused.effective);
    EXPECT_EQ(camera.GetFarPlane(), 3200);
}
} // namespace
