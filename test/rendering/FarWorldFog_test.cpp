#include "luminumbra_client/rendering/FarWorldFog.h"

#include <gtest/gtest.h>

namespace {
using namespace Luminumbra::Rendering;
constexpr float radius = 16384.0f;

TEST(FarWorldFog, SmoothstepEndpointsAndIndependentMidpoint) {
    EXPECT_EQ(Luminumbra::World::kFarOuterRadiusMeters, 16384u);
    EXPECT_EQ(AerialOuterFade(0), 1);
    EXPECT_EQ(AerialOuterFade(0.9f * radius), 1);
    EXPECT_NEAR(AerialOuterFade(0.95f * radius), 0.5, 1e-6);
    EXPECT_EQ(AerialOuterFade(radius), 0);
    EXPECT_EQ(AerialOuterFade(20000), 0);
    float previous = 1;
    for (int d = 0; d <= 20000; ++d) {
        const auto fade = AerialOuterFade(static_cast<float>(d));
        EXPECT_LE(fade, previous);
        previous = fade;
    }
}

TEST(FarWorldFog, BothCapsFroxelModesAndAltitudeCannotWeakenOuterGuarantee) {
    for (const float cap : {0.94f, 0.9f, 0.5f}) {
        // Zero density, thin elevated aerosol, and saturated low-altitude fog.
        for (const float analytic : {0.0f, 0.001f, 0.9f, 1.0f}) {
            for (const bool froxel : {false, true}) {
                for (const float transmittance : {0.0f, 0.2f, 1.0f}) {
                    const glm::vec4 volume(0.4f, 0.2f, 0.1f, transmittance);
                    const auto legacy = ComposeFinalFog(
                        analytic, glm::vec3(0.3f), froxel, volume, radius, false, cap);
                    const auto ladder = ComposeFinalFog(
                        analytic, glm::vec3(0.3f), froxel, volume, radius, true, cap);
                    EXPECT_LE(legacy.a, cap);
                    EXPECT_EQ(ladder.a, 1);
                    EXPECT_LE(1 - ladder.a, 0.05f);
                    EXPECT_EQ(glm::vec3(ladder), glm::vec3(legacy));
                    EXPECT_EQ(
                        ComposeFinalFog(analytic, glm::vec3(0.3f), froxel, volume, 1000, true, cap),
                        legacy);
                }
            }
        }
    }
}

TEST(FarWorldFog, IndependentCompositionIncludesFroxelColorAndRetainsZeroFogColor) {
    const glm::vec3 color(0.3f, 0.4f, 0.6f);
    const glm::vec4 volume(0.2f, 0.1f, 0.05f, 0.25f);
    const auto result = ComposeFinalFog(1, color, true, volume, 0.95f * radius, true);
    const double composed = std::min(0.94 * 0.25 + 0.75, 0.94);
    EXPECT_NEAR(result.a, 1 - (1 - composed) * 0.5, 1e-6);
    for (int channel = 0; channel < 3; ++channel) {
        const double v = volume[channel];
        const double mapped =
            std::pow(v * (2.51 * v + 0.03) / (v * (2.43 * v + 0.59) + 0.14), 1.0 / 2.2);
        EXPECT_NEAR(
            result[channel], (color[channel] * 0.94 * 0.25 + mapped * 0.75) / composed, 1e-6);
    }
    EXPECT_EQ(ComposeFinalFog(0, color, true, {0, 0, 0, 1}, radius, true), glm::vec4(color, 1));
    // Evaluation might be clamped to 1000 m; the raw 20 km ray still fades fully.
    EXPECT_EQ(ComposeFinalFog(0, color, false, {}, 20000, true).a, 1);
}
} // namespace
