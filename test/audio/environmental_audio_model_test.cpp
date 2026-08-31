//  +  ( ranks ~93/100): CPU-only model tests for the
// day/night soundscape gating/crossfade and the biome/weather reverb mapping.
//
// The header under test (src/luminumbra_client/audio/EnvironmentalAudioModel.h)
// is pure std math — no miniaudio, no audio device, no GL — so these anchors run
// in the default ctest lane on any machine:
//   * day bed silent at night, night bed silent at day (sun-elevation gating)
//   * crossfade weights sum to ~1 through the whole twilight band
//   * the crossfade converges within "a few seconds" and is monotone
//   * reverb params are monotone in acoustic-space size
//   * the delay-line reverb PROXY mapping is stable (feedback strictly < 1).
#include <gtest/gtest.h>

#include "luminumbra_client/audio/EnvironmentalAudioModel.h"

#include <cmath>

namespace {

using namespace Luminumbra::Client::AudioModel;

// ---: night factor gating curve -----------------------------------

TEST(NightFactorTest, FullDayAboveDayEdge) {
    // High noon and anything at/above the day edge is FULL day (night factor 0).
    EXPECT_FLOAT_EQ(NightFactor(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(NightFactor(0.5f), 0.0f);
    EXPECT_FLOAT_EQ(NightFactor(kDayEdgeSinElevation), 0.0f);
}

TEST(NightFactorTest, FullNightBelowNightEdge) {
    // Sun well below the horizon (and at/below the night edge) is FULL night.
    EXPECT_FLOAT_EQ(NightFactor(-1.0f), 1.0f);
    EXPECT_FLOAT_EQ(NightFactor(-0.5f), 1.0f);
    EXPECT_FLOAT_EQ(NightFactor(kNightEdgeSinElevation), 1.0f);
}

TEST(NightFactorTest, TwilightIsSmoothAndMonotone) {
    // Across the twilight band the factor is strictly inside (0, 1), monotone
    // non-increasing in sun elevation, and hits ~0.5 mid-band (smoothstep).
    float prev = NightFactor(-1.0f);
    EXPECT_FLOAT_EQ(prev, 1.0f);
    for (float s = -1.0f; s <= 1.0f; s += 0.01f) {
        const float nf = NightFactor(s);
        EXPECT_GE(nf, 0.0f);
        EXPECT_LE(nf, 1.0f);
        EXPECT_LE(nf, prev + 1e-6f)
            << "night factor must not rise as the sun rises (s=" << s << ")";
        prev = nf;
    }
    const float mid = 0.5f * (kDayEdgeSinElevation + kNightEdgeSinElevation);
    EXPECT_NEAR(NightFactor(mid), 0.5f, 1e-4f);
    EXPECT_GT(NightFactor(mid), 0.0f);
    EXPECT_LT(NightFactor(mid), 1.0f);
}

// ---: crossfade weights --------------------------------------------

TEST(DayNightCrossfadeTest, DayBedSilentAtNight) {
    const DayNightWeights w = DayNightCrossfade(NightFactor(-0.5f)); // deep night
    EXPECT_FLOAT_EQ(w.day, 0.0f);
    EXPECT_FLOAT_EQ(w.night, 1.0f);
}

TEST(DayNightCrossfadeTest, NightBedSilentAtDay) {
    const DayNightWeights w = DayNightCrossfade(NightFactor(0.5f)); // high day
    EXPECT_FLOAT_EQ(w.day, 1.0f);
    EXPECT_FLOAT_EQ(w.night, 0.0f);
}

TEST(DayNightCrossfadeTest, WeightsSumToOneAcrossTheBand) {
    for (float s = -1.0f; s <= 1.0f; s += 0.005f) {
        const DayNightWeights w = DayNightCrossfade(NightFactor(s));
        EXPECT_NEAR(w.day + w.night, 1.0f, 1e-5f) << "at sinElev=" << s;
        EXPECT_GE(w.day, 0.0f);
        EXPECT_GE(w.night, 0.0f);
    }
    // Out-of-range night factors are clamped, still summing to 1.
    const DayNightWeights lo = DayNightCrossfade(-0.5f);
    const DayNightWeights hi = DayNightCrossfade(1.5f);
    EXPECT_FLOAT_EQ(lo.day + lo.night, 1.0f);
    EXPECT_FLOAT_EQ(hi.day + hi.night, 1.0f);
}

// ---: crossfade smoothing ------------------------------------------

TEST(SmoothTowardsTest, ConvergesWithinAFewSecondsAndIsMonotone) {
    // Step day -> night: iterate the smoother at the environmental-audio tick
    // rate (0.1 s). It must move monotonically, pass 63% by ~tau, exceed 95% by
    // ~3*tau (the audible crossfade is "a few seconds"), and land EXACTLY on
    // target (the 0.5% snap) — analytically at tau*ln(200) ~ 5.3*tau, so give
    // the loop 8*tau of headroom.
    float value = 0.0f;
    const float target = 1.0f;
    const float dt = 0.1f;
    const float horizon = 8.0f * kDayNightCrossfadeTau;
    float t = 0.0f;
    float prev = value;
    bool reached = false;
    float reach_time = 0.0f;
    while (t < horizon) {
        value = SmoothTowards(value, target, dt, kDayNightCrossfadeTau);
        t += dt;
        EXPECT_GE(value, prev) << "crossfade must be monotone";
        EXPECT_LE(value, 1.0f);
        prev = value;
        if (!reached && value == target) {
            reached = true;
            reach_time = t;
        }
        if (std::fabs(t - kDayNightCrossfadeTau) < dt * 0.5f) {
            EXPECT_GT(value, 0.60f) << "~63% of the step by one tau";
        }
        if (std::fabs(t - 3.0f * kDayNightCrossfadeTau) < dt * 0.5f) {
            EXPECT_GT(value, 0.949f) << "~95% of the step by three tau";
        }
    }
    EXPECT_TRUE(reached) << "the snap must land the crossfade exactly on target";
    EXPECT_LT(reach_time, 6.0f * kDayNightCrossfadeTau);
    EXPECT_GT(reach_time, kDayNightCrossfadeTau) << "no instant cut";
}

TEST(SmoothTowardsTest, DegenerateInputsSnapToTarget) {
    EXPECT_FLOAT_EQ(SmoothTowards(0.0f, 1.0f, 0.1f, 0.0f), 1.0f);  // tau <= 0
    EXPECT_FLOAT_EQ(SmoothTowards(0.0f, 1.0f, -0.1f, 2.5f), 1.0f); // dt < 0
    EXPECT_FLOAT_EQ(SmoothTowards(0.7f, 0.7f, 0.1f, 2.5f), 0.7f);  // at target
}

// ---: biome size -> reverb params -----------------------------------

TEST(BiomeReverbFromSizeTest, MonotoneInBiomeSize) {
    BiomeReverbParams prev = BiomeReverbFromSize(0.0f);
    for (float s = 0.0f; s <= 1.0f; s += 0.01f) {
        const BiomeReverbParams p = BiomeReverbFromSize(s);
        EXPECT_GE(p.wet, prev.wet) << "wet must rise with space size (s=" << s << ")";
        EXPECT_LE(p.dry, prev.dry) << "dry must fall with space size (s=" << s << ")";
        EXPECT_GE(p.decay, prev.decay) << "decay must rise with space size (s=" << s << ")";
        EXPECT_GE(p.wet, 0.0f);
        EXPECT_LE(p.wet, 1.0f);
        EXPECT_GE(p.dry, 0.0f);
        EXPECT_LE(p.dry, 1.0f);
        EXPECT_GE(p.decay, 0.0f);
        prev = p;
    }
}

TEST(BiomeReverbFromSizeTest, EndpointsMatchAuthoredEnvelope) {
    // The curve's endpoints pin the authored profile envelope: open field
    // (Outdoor:.1/.9/.3) up to canyon (.7/.3/3.0).
    const BiomeReverbParams open_field = BiomeReverbFromSize(0.0f);
    EXPECT_FLOAT_EQ(open_field.wet, 0.1f);
    EXPECT_FLOAT_EQ(open_field.dry, 0.9f);
    EXPECT_FLOAT_EQ(open_field.decay, 0.3f);
    const BiomeReverbParams canyon = BiomeReverbFromSize(1.0f);
    EXPECT_FLOAT_EQ(canyon.wet, 0.7f);
    EXPECT_FLOAT_EQ(canyon.dry, 0.3f);
    EXPECT_FLOAT_EQ(canyon.decay, 3.0f);
    // Out-of-range sizes clamp instead of extrapolating.
    EXPECT_FLOAT_EQ(BiomeReverbFromSize(-1.0f).wet, open_field.wet);
    EXPECT_FLOAT_EQ(BiomeReverbFromSize(2.0f).decay, canyon.decay);
}

// ---: delay-line reverb proxy mapping --------------------------------

TEST(ReverbProxyTest, FeedbackMonotoneInDecayAndAlwaysStable) {
    float prev_feedback = -1.0f;
    for (float decay = 0.0f; decay <= 12.0f; decay += 0.1f) {
        const ReverbProxyParams p = ReverbProxyFromParams(0.4f, 0.6f, decay);
        EXPECT_GE(p.feedback, prev_feedback) << "feedback must be monotone in decay";
        EXPECT_LT(p.feedback, 1.0f) << "feedback >= 1 runs away (decay=" << decay << ")";
        EXPECT_LE(p.feedback, kReverbProxyMaxFeedback + 1e-6f);
        prev_feedback = p.feedback;
    }
    // Zero decay = zero feedback (a single dry slap, no tail).
    EXPECT_FLOAT_EQ(ReverbProxyFromParams(0.4f, 0.6f, 0.0f).feedback, 0.0f);
}

TEST(ReverbProxyTest, WetDryClampedPassThrough) {
    const ReverbProxyParams p = ReverbProxyFromParams(0.25f, 0.75f, 1.0f);
    EXPECT_FLOAT_EQ(p.wet, 0.25f);
    EXPECT_FLOAT_EQ(p.dry, 0.75f);
    // Out-of-range mixes clamp to [0, 1] (never inverted, never amplified).
    const ReverbProxyParams hi = ReverbProxyFromParams(1.5f, -0.5f, 1.0f);
    EXPECT_FLOAT_EQ(hi.wet, 1.0f);
    EXPECT_FLOAT_EQ(hi.dry, 0.0f);
    // Negative decay is treated as zero (stable, tail-free).
    EXPECT_FLOAT_EQ(ReverbProxyFromParams(0.2f, 0.8f, -3.0f).feedback, 0.0f);
}

TEST(ReverbProxyTest, BiomeSizeSweepYieldsMonotoneProxyFeedback) {
    // End-to-end model chain: bigger biome -> longer authored decay -> more
    // proxy feedback (the audible tail actually grows with the space).
    float prev = -1.0f;
    for (float s = 0.0f; s <= 1.0f; s += 0.05f) {
        const BiomeReverbParams biome = BiomeReverbFromSize(s);
        const ReverbProxyParams proxy = ReverbProxyFromParams(biome.wet, biome.dry, biome.decay);
        EXPECT_GE(proxy.feedback, prev);
        EXPECT_LT(proxy.feedback, 1.0f);
        prev = proxy.feedback;
    }
}

} // namespace
