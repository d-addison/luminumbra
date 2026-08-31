#pragma once
//  +  (, ranks ~93/100): the PURE math behind the
// day/night soundscape crossfade and the biome/weather reverb mapping.
//
// This header is deliberately dependency-free (std only: no miniaudio, no glm,
// no engine headers) so the model is unit-testable CPU-only with no audio
// device (test/audio/environmental_audio_model_test.cpp) and so both the live
// client (EnvironmentalAudioSystem / MiniaudioManager) and any telemetry
// harness compute EXACTLY the same numbers.
//
// All of this is render/client-side audio dressing: nothing here touches the
// simulation, world_hash, or the visual gates.

#include <algorithm>
#include <cmath>

namespace Luminumbra::Client::AudioModel {

// ---------------------------------------------------------------------------
// day/night gating driven by SUN ELEVATION.
//
// The input is sin(sun elevation angle) in [-1, 1] — i.e. the vertical
// component of the normalized TOWARD-sun direction. The client feeds it from
// the render pipeline's time-of-day state (the audio system never reaches into
// render globals; see EnvironmentalAudioSystem::SetSunElevation).
//
// Gating band (astronomically motivated):
//   sinElev >= kDayEdgeSinElevation  (~ sun 3 degrees UP)   -> full day
//   sinElev <= kNightEdgeSinElevation (~ sun 6 degrees DOWN, civil twilight)
//                                                           -> full night
// with a smoothstep blend across the twilight band in between, so dawn/dusk
// birdsong/cricket handover tracks the actual light level, not a hard switch.
// ---------------------------------------------------------------------------

inline constexpr float kNightEdgeSinElevation = -0.10f; // sun ~5.7 deg below horizon: full night
inline constexpr float kDayEdgeSinElevation = 0.05f;    // sun ~2.9 deg above horizon: full day

// Time constant (seconds) of the day<->night bed crossfade. The smoothed
// factor covers ~63% of a step change per tau; it converges (>95%) within
// ~3*tau, i.e. "a few seconds", never a hard cut.
inline constexpr float kDayNightCrossfadeTau = 2.5f;

// Below this crossfade weight a bed is treated as silent and its loop is
// STOPPED (saves mixer work; also makes "day bed silent at night" literal).
inline constexpr float kBedSilenceFloor = 0.01f;

inline float Clamp01(float v) {
    return std::min(1.0f, std::max(0.0f, v));
}

inline float Smoothstep01(float t) {
    t = Clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

// Night factor in [0, 1]: 0 = full day, 1 = full night, smooth and monotone
// (non-increasing) in the sun elevation across the twilight band.
inline float NightFactor(float sinSunElevation) {
    const float t = (sinSunElevation - kNightEdgeSinElevation) /
                    (kDayEdgeSinElevation - kNightEdgeSinElevation);
    return 1.0f - Smoothstep01(t);
}

// Complementary linear crossfade weights for the two ambient beds. By
// construction day + night == 1 exactly, so the combined ambience level holds
// steady through dawn/dusk instead of dipping or doubling.
struct DayNightWeights {
    float day = 1.0f;
    float night = 0.0f;
};

inline DayNightWeights DayNightCrossfade(float nightFactor) {
    const float nf = Clamp01(nightFactor);
    return DayNightWeights{1.0f - nf, nf};
}

// Frame-rate-independent exponential approach of `current` toward `target`
// with time constant `tauSeconds`. Used to smooth the night factor so the bed
// handover is a crossfade over a few seconds rather than a cut. Snaps the
// last <0.5% residual so the anchors (exactly-silent / exactly-full) are
// actually reached in finite time.
inline float SmoothTowards(float current, float target, float dtSeconds, float tauSeconds) {
    if (tauSeconds <= 0.0f || dtSeconds < 0.0f) {
        return target;
    }
    const float alpha = 1.0f - std::exp(-dtSeconds / tauSeconds);
    float next = current + (target - current) * alpha;
    if (std::fabs(next - target) < 0.005f) {
        next = target;
    }
    return next;
}

// ---------------------------------------------------------------------------
// biome -> reverb-parameter mapping.
//
// The canonical size01 -> (wet, dry, decay) curve. size01 is the perceived
// acoustic-space size in [0, 1] (0 ~ open field, 1 ~ canyon). The endpoints
// and monotone envelope match the hand-authored profiles that already ship
// (CreateEnvironmentProfile / biomes.json): Outdoor(.1/.9/.3)... Canyon
// (.7/.3/3.0). Authored biomes keep their authored values (biomes.json is the
// source of truth there); this curve serves procedural/unauthored biomes and
// pins the monotonicity contract the gtest asserts.
// ---------------------------------------------------------------------------

struct BiomeReverbParams {
    float wet = 0.1f;
    float dry = 0.9f;
    float decay = 0.3f; // pseudo-RT60 seconds
};

inline BiomeReverbParams BiomeReverbFromSize(float size01) {
    const float s = Clamp01(size01);
    BiomeReverbParams p;
    p.wet = 0.1f + 0.6f * s;   // monotone UP:   bigger space -> wetter
    p.dry = 0.9f - 0.6f * s;   // monotone DOWN: bigger space -> less direct
    p.decay = 0.3f + 2.7f * s; // monotone UP:   bigger space -> longer tail
    return p;
}

// ---------------------------------------------------------------------------
// reverb PROXY parameter mapping for the miniaudio delay-line node.
//
// The vendored miniaudio (0.11.22, vendor/CMakeLists.txt FetchContent pin) has
// NO built-in reverb DSP node. The runtime therefore uses a single feedback
// delay line (ma_delay_node — core miniaudio >= 0.11) as an HONEST PROXY: a
// short slap-back with feedback reads as early reflections + tail, which is
// convincing for outdoor/canyon/cave ambience even though it intentionally does
// not model a diffuse convolution reverb.
//
// Mapping:
//   wet/dry  -> the delay node's wet/dry mix (clamped [0, 1]).
//   decay    -> the delay line FEEDBACK gain. Authored decay is pseudo-RT60
//               seconds (0.3.. 3.0); feedback must stay < 1 or the line runs
//               away, so decay maps through d/(d + kHalf), capped at
//               kReverbProxyMaxFeedback. Monotone in decay, always stable.
//   delay time is FIXED at node init (kReverbProxyDelayMs): ma_delay's buffer
//               is allocated at init and miniaudio has no runtime delay-time
//               setter, so only the mix/feedback are driven live.
// ---------------------------------------------------------------------------

inline constexpr float kReverbProxyDelayMs = 90.0f;     // fixed slap-back time
inline constexpr float kReverbProxyMaxFeedback = 0.85f; // hard stability cap (< 1)
inline constexpr float kReverbProxyDecayHalf = 1.2f;    // decay (s) giving ~half-scale feedback

struct ReverbProxyParams {
    float wet = 0.0f;
    float dry = 1.0f;
    float feedback = 0.0f;
};

inline ReverbProxyParams ReverbProxyFromParams(float wet, float dry, float decaySeconds) {
    ReverbProxyParams p;
    p.wet = Clamp01(wet);
    p.dry = Clamp01(dry);
    const float d = std::max(0.0f, decaySeconds);
    p.feedback = std::min(kReverbProxyMaxFeedback, d / (d + kReverbProxyDecayHalf));
    return p;
}

} // namespace Luminumbra::Client::AudioModel
