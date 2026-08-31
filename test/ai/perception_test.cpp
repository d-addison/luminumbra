// perception primitives — vision cone + hearing audiogram (pitch +
// loudness sensitivity). Pure deterministic geometry; world_hash-neutral.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/Perception.h"

namespace {
using luminumbra::ai::HeardLoudness;
using luminumbra::ai::HearingProfile;
using luminumbra::ai::InVisionCone;
using luminumbra::ai::PerceivedLoudness;
using luminumbra::ai::PitchSensitivity;
} // namespace

// --- Vision cone ---------------------------------------------------------------

TEST(Perception, VisionSeesTargetDeadAheadInRange) {
    // Observer at origin facing +X, 90 deg FOV (cos45 ~ 0.707), range 10.
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, 0.7071f, 10.0f, 5.0f, 0.0f));
}

TEST(Perception, VisionRejectsOutOfRange) {
    EXPECT_FALSE(InVisionCone(0, 0, 1, 0, 0.7071f, 10.0f, 20.0f, 0.0f));
}

TEST(Perception, VisionRejectsTargetOutsideNarrowCone) {
    // Target 90 deg to the side is outside a 90 deg (+/-45) cone.
    EXPECT_FALSE(InVisionCone(0, 0, 1, 0, 0.7071f, 10.0f, 0.0f, 5.0f));
}

TEST(Perception, WideConeSeesBehind) {
    // cos_half_fov = -1 -> full 360; a target directly behind is visible.
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, -1.0f, 10.0f, -5.0f, 0.0f));
    //...but a narrow forward cone does not see directly behind.
    EXPECT_FALSE(InVisionCone(0, 0, 1, 0, 0.7071f, 10.0f, -5.0f, 0.0f));
}

TEST(Perception, CoincidentTargetAlwaysVisible) {
    EXPECT_TRUE(InVisionCone(3, 3, 1, 0, 0.99f, 10.0f, 3.0f, 3.0f));
}

// --- Hearing: distance attenuation --------------------------------------------

TEST(Perception, LoudnessAttenuatesWithDistanceAndCutsOffAtRange) {
    const float near = PerceivedLoudness(2.0f, 10.0f, 20.0f);
    const float far = PerceivedLoudness(15.0f, 10.0f, 20.0f);
    EXPECT_GT(near, far);
    EXPECT_GT(far, 0.0f);
    EXPECT_FLOAT_EQ(PerceivedLoudness(20.0f, 10.0f, 20.0f), 0.0f); // at range -> silent
    EXPECT_FLOAT_EQ(PerceivedLoudness(25.0f, 10.0f, 20.0f), 0.0f); // beyond -> silent
}

// --- Hearing: audiogram (pitch + loudness sensitivity) ------------------------

TEST(Perception, PitchSensitivityPeaksAtTunedPitchAndFallsOff) {
    HearingProfile ear;
    ear.peak_pitch = 0.8f; // tuned high
    ear.bandwidth = 0.2f;
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.8f), 1.0f);    // at peak
    EXPECT_NEAR(PitchSensitivity(ear, 0.7f), 0.5f, 1e-4f); // half a bandwidth off
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.5f), 0.0f);    // beyond bandwidth -> deaf to it
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.1f), 0.0f);
}

TEST(Perception, TunedEarHearsMatchingPitchButNotOffBandPitch) {
    HearingProfile predator; // tuned to a high-pitched prey footstep band
    predator.range = 30.0f;
    predator.peak_pitch = 0.75f;
    predator.bandwidth = 0.15f;
    const float on_band = HeardLoudness(5.0f, 10.0f, 0.75f, predator);
    const float off_band = HeardLoudness(5.0f, 10.0f, 0.20f, predator); // low rumble
    EXPECT_GT(on_band, 0.0f);                                           // hears the matching pitch
    EXPECT_FLOAT_EQ(off_band, 0.0f); // deaf to the off-band pitch at the same loudness/dist
}

TEST(Perception, ThresholdGatesFaintSoundsAndGainOvercomesIt) {
    HearingProfile ear;
    ear.range = 20.0f;
    ear.peak_pitch = 0.5f;
    ear.bandwidth = 0.5f;
    ear.threshold = 4.0f; // only fairly-loud sounds register
    ear.gain = 1.0f;
    // base = 6 * (1 - 10/20) = 3.0 (on-peak pitch). 3.0 <= threshold 4 -> not heard.
    EXPECT_FLOAT_EQ(HeardLoudness(10.0f, 6.0f, 0.5f, ear), 0.0f);
    // Raising gain (more sensitive ears) -> 3.0 * 2 = 6.0 > 4 -> now heard.
    ear.gain = 2.0f;
    EXPECT_GT(HeardLoudness(10.0f, 6.0f, 0.5f, ear), 0.0f);
}
