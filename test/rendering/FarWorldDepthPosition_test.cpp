#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarWorldDepthPosition.h"
#include "luminumbra_client/rendering/RenderContext.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;

TEST(FarWorldDepthPosition, EffectiveSelectionKeepsDefaultRefusedAndPreviewOnStoredPositions) {
    Camera camera;
    for (const auto selection :
         {SelectFarWorld(FarWorldMode::Legacy, FarWorldPurpose::World, FarWorldRefusal::None),
          SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::Preview, FarWorldRefusal::None),
          SelectFarWorld(
              FarWorldMode::Ladder, FarWorldPurpose::OffscreenPreview, FarWorldRefusal::None),
          SelectFarWorld(FarWorldMode::Ladder,
                         FarWorldPurpose::World,
                         FarWorldRefusal::ImmutableAuthorityUnavailable)}) {
        camera.SelectFarWorldPlane(selection.effective);
        RenderContext ctx;
        ctx.set_gbuffer_depth_contract(selection.effective == FarWorldMode::Ladder,
                                       camera.GetProjectionMatrix(900, 600),
                                       {.001f, -.002f});
        EXPECT_FALSE(ctx.far_world_ladder);
        EXPECT_EQ(camera.GetFarPlane(), kLegacyFarPlaneMeters);
        glm::vec3 position;
        ASSERT_TRUE(FarWorldDepthPosition(
            ctx.far_world_ladder, {1, 2, -16000}, {.5f, .5f}, 0, glm::mat4(0), position));
        EXPECT_EQ(position, glm::vec3(1, 2, -16000));
    }
}

// Double-precision scalar oracle: no production matrix projection/inversion.
double ExpectedDepth(double axial, double far_plane) {
    return 0.1 * (far_plane / axial - 1) / (far_plane - 0.1);
}

TEST(FarWorldDepthPosition, ProductionWriterProjectionRoundTripsWithAerialInverseAndNonzeroJitter) {
    Camera camera;
    const auto selection =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    camera.SelectFarWorldPlane(selection.effective); // C/S9's production plane selection.
    ASSERT_EQ(camera.GetFarPlane(), kFarPlaneMeters);
    for (const auto jitter : {glm::vec2(.001f, -.002f), glm::vec2(-.0015f, .0007f)}) {
        RenderContext ctx;
        const auto selected_projection = camera.GetProjectionMatrix(900, 600);
        ctx.set_gbuffer_depth_contract(
            selection.effective == FarWorldMode::Ladder, selected_projection, jitter);
        ASSERT_TRUE(ctx.far_world_ladder);
        // This field is uploaded by every Ladder G-buffer writer, including
        // PlantProcgenPass; AerialPass uploads the inverse from the same contract.
        const auto& writer_projection = ctx.gbuffer_projection;
        const auto& aerial_inverse = ctx.gbuffer_inverse_projection;
        for (const glm::vec3 expected : {glm::vec3(20, -10, -176),
                                         glm::vec3(-130, 240, -2001),
                                         glm::vec3(1400, -500, -8192),
                                         glm::vec3(-1000, 800, -16000)}) {
            const glm::vec4 clip = writer_projection * glm::vec4(expected, 1);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            glm::vec3 reconstructed;
            ASSERT_TRUE(ReconstructFarWorldDepthPosition(
                glm::vec2(ndc) * .5f + .5f, ndc.z, aerial_inverse, reconstructed));
            for (int i = 0; i < 3; ++i)
                EXPECT_NEAR(reconstructed[i], expected[i], .01f);

            // Negative control reproduces the old marker writer: correct plane,
            // missing jitter. It must not satisfy the production contract.
            const glm::vec4 old_clip = selected_projection * glm::vec4(expected, 1);
            const glm::vec3 old_ndc = glm::vec3(old_clip) / old_clip.w;
            glm::vec3 mismatched;
            ASSERT_TRUE(ReconstructFarWorldDepthPosition(
                glm::vec2(old_ndc) * .5f + .5f, old_ndc.z, aerial_inverse, mismatched));
            EXPECT_GT(glm::distance(mismatched, expected), .1f);
        }
    }
}

TEST(FarWorldDepthPosition, IndependentBothPlanesOffAxisPitchAndJitter) {
    constexpr double pi = 3.14159265358979323846;
    const double focal = 1 / std::tan(45.0 * pi / 360.0);
    for (const float plane : {kLegacyFarPlaneMeters, kFarPlaneMeters}) {
        for (const double pitch : {-35.0, 27.0}) {
            const double angle = pitch * pi / 180;
            for (const float distance : {176.0f, 1999.9f, 2000.1f, plane - 100}) {
                // Rotate a world-relative ray independently in double precision.
                const double world_y = distance * 0.15;
                const glm::dvec3 expected(distance * 0.1,
                                          world_y * std::cos(angle) - distance * std::sin(angle),
                                          -world_y * std::sin(angle) - distance * std::cos(angle));
                for (const glm::vec2 jitter : {glm::vec2(0), glm::vec2(0.001f, -0.002f)}) {
                    const float depth = static_cast<float>(ExpectedDepth(-expected.z, plane));
                    const glm::vec2 uv(
                        static_cast<float>(
                            (focal * expected.x / (1.5 * -expected.z) - jitter.x + 1) / 2),
                        static_cast<float>((focal * expected.y / -expected.z - jitter.y + 1) / 2));
                    RenderContext ctx;
                    const auto projection =
                        ReversedZPerspective(glm::radians(45.0f), 1.5f, .1f, plane);
                    ctx.set_gbuffer_depth_contract(true, projection, jitter);
                    glm::vec3 result;
                    ASSERT_TRUE(ReconstructFarWorldDepthPosition(
                        uv, depth, ctx.gbuffer_inverse_projection, result));
                    for (int i = 0; i < 3; ++i)
                        EXPECT_NEAR(result[i], expected[i], 0.004);
                    EXPECT_EQ(ctx.gbuffer_projection[2][0], projection[2][0] + jitter.x);
                    EXPECT_EQ(ctx.gbuffer_projection[2][1], projection[2][1] + jitter.y);
                }
            }
        }
    }
}

TEST(FarWorldDepthPosition, ExactThresholdUsesDepthAndNeighborsClassifyIndependently) {
    EXPECT_EQ(FarWorldPositionSampleUv({0.27f, 0.76f}, {4, 4}), glm::vec2(.375f, .875f));
    glm::mat4 inverse(0);
    inverse[3][3] = 1;
    glm::vec3 output;
    const glm::vec3 quantized(0, 0, -2000);
    for (const float radial : {1999.99f, 2000.0f, 2000.01f, 16000.0f}) {
        inverse[3][2] = -radial;
        ASSERT_TRUE(FarWorldDepthPosition(true, quantized, {0.5f, 0.5f}, .5f, inverse, output));
        EXPECT_EQ(output.z, radial <= 2000 ? quantized.z : -radial);
    }
    // The same call contract is applied to a near center and each far neighbor.
    inverse[3][2] = -1999.0f;
    ASSERT_TRUE(FarWorldDepthPosition(true, {7, 8, -1999}, {.5f, .5f}, .5f, inverse, output));
    EXPECT_EQ(output, glm::vec3(7, 8, -1999));
    inverse[3][2] = -16000;
    ASSERT_TRUE(FarWorldDepthPosition(true, {7, 8, -1999}, {.6f, .5f}, .5f, inverse, output));
    EXPECT_EQ(output, glm::vec3(0, 0, -16000));
}

TEST(FarWorldDepthPosition, BiasedDepthReconstructsBiasedPositionWithoutSubtractingBias) {
    constexpr double focal = 2.4142135623730950488;
    const glm::dvec3 original(800, 400, -8000);
    const double factor = 1 + 32 / std::sqrt(glm::dot(original, original));
    const auto expected = original * factor;
    const glm::vec2 uv(static_cast<float>((focal * expected.x / (1.5 * -expected.z) + 1) / 2),
                       static_cast<float>((focal * expected.y / -expected.z + 1) / 2));
    const auto projection = ReversedZPerspective(glm::radians(45.0f), 1.5f, .1f, kFarPlaneMeters);
    glm::vec3 result;
    ASSERT_TRUE(
        FarWorldDepthPosition(true,
                              glm::vec3(original),
                              uv,
                              static_cast<float>(ExpectedDepth(-expected.z, kFarPlaneMeters)),
                              glm::inverse(projection),
                              result));
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(result[i], expected[i], .002);
    EXPECT_NEAR(glm::length(result) - glm::length(glm::vec3(original)), 32, .002);
}

TEST(FarWorldDepthPosition, ClearNonfiniteDegenerateAndLegacyBehavior) {
    const auto projection = ReversedZPerspective(glm::radians(45.0f), 1.5f, .1f, kFarPlaneMeters);
    const auto inverse = glm::inverse(projection);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    glm::vec3 output(1, 2, 3);
    for (const float depth : {0.0f, -1.0f, 1.01f, nan, inf}) {
        EXPECT_FALSE(
            FarWorldDepthPosition(true, {0, 0, -16000}, {.5f, .5f}, depth, inverse, output));
        EXPECT_EQ(output, glm::vec3(1, 2, 3));
    }
    for (const auto& invalid : {glm::mat4(0), glm::mat4(nan), glm::mat4(inf), glm::mat4(1)}) {
        EXPECT_FALSE(ReconstructFarWorldDepthPosition({.5f, .5f}, .5f, invalid, output));
        EXPECT_EQ(output, glm::vec3(1, 2, 3));
    }
    EXPECT_FALSE(ReconstructFarWorldDepthPosition({nan, inf}, .5f, inverse, output));
    EXPECT_FALSE(FarWorldDepthPosition(true, {nan, 0, -1}, {.5f, .5f}, .5f, inverse, output));
    ASSERT_TRUE(FarWorldDepthPosition(false, {4, 5, -6000}, {nan, inf}, nan, {}, output));
    EXPECT_EQ(output, glm::vec3(4, 5, -6000));
}
} // namespace
