#pragma once

// creature PERCEPTION primitives — vision cones + hearing — the
// geometric read side of the senses (smell is the separate ScentField). Kept
// deliberately complementary: vision gives an EXACT bearing right now but only
// inside a forward cone with line-of-sight and good light; hearing gives a rough
// presence right now even behind/around obstacles; smell (ScentField) gives where
// a target WAS plus a trail. A perception/awareness layer fuses the three to feed
// the InstinctPlanner.
//
// PURE + DETERMINISTIC (sim contract): float-only, DeterministicMath::Sqrt only,
// NO libm transcendentals. The FOV is supplied as cos(half-angle) so the per-tick
// test needs no acos/cos — the caller precomputes it once at spawn (a creature's
// FOV is constant, or an evolvable genome gene). RunPerceptionSystemOnTick consumes
// these primitives in GameSession's fixed tick for entities with perception and
// awareness components; entities without those components do not participate.

#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

// True iff target (tx,tz) lies within `range` of the observer AND inside its
// forward vision cone. `fx,fz` is the observer's facing (expected unit-length);
// `cos_half_fov` = cos(FOV/2): 1.0 = blind-ahead-only, 0.0 = 180 deg, -1.0 = full
// 360 (sees everything in range). Line-of-sight / light attenuation are layered by
// the caller (this is the pure cone+range test). A target on top of the observer
// (dist ~ 0) is always visible.
[[nodiscard]] inline bool InVisionCone(
    float ox, float oz, float fx, float fz, float cos_half_fov, float range, float tx, float tz) {
    const float dx = tx - ox;
    const float dz = tz - oz;
    const float dist2 = dx * dx + dz * dz;
    if (dist2 > range * range)
        return false; // out of range
    if (dist2 < 1e-8f)
        return true; // coincident
    const float dist = Luminumbra::DeterministicMath::Sqrt(dist2);
    // Angle test without acos: dot(unit_to_target, facing) >= cos_half_fov
    //   (dx*fx + dz*fz) / dist >= cos_half_fov
    //   (dx*fx + dz*fz)        >= cos_half_fov * dist   (dist > 0)
    // Valid for any FOV in (0, 360]: a negative cos_half_fov (FOV > 180) yields a
    // negative threshold, so rear targets correctly pass for very wide cones.
    const float dot = dx * fx + dz * fz;
    return dot >= cos_half_fov * dist;
}

// Convert a FOV in DEGREES to the cos(half-angle) the cone test expects. Call it
// once at spawn / on a genome FOV gene and store the result (the per-tick cone
// test consumes the stored `vision_cos_half_fov`, never this conversion). Routed
// through DeterministicMath::Cos -- not std::cos -- so even the spawn-time value is
// cross-platform bit-stable (std::cos differs per libm; that is the whole reason
// DeterministicMath::Cos is a minimax polynomial). Provided so callers have a
// single conversion point.
[[nodiscard]] inline float FovDegreesToCosHalf(float fov_degrees) {
    const float half_rad = (fov_degrees * 0.5f) * 0.017453292519943295f; // pi/180
    return Luminumbra::DeterministicMath::Cos(half_rad);
}

// Perceived loudness of a sound of `source_loudness` emitted at distance `dist`
// for a listener whose `hearing_range` is the distance at which loudness reaches
// zero. Linear attenuation (1 - dist/range), clamped to >= 0; returns 0 beyond
// range or for a non-positive range. Hearing is omnidirectional (no cone) — that
// is the point of it relative to vision. Occlusion is layered by the caller (e.g.
// scale source_loudness down through cover). This is the distance-only base; the
// pitch-aware audiogram below composes from it.
[[nodiscard]] inline float
PerceivedLoudness(float dist, float source_loudness, float hearing_range) {
    if (hearing_range <= 0.0f || dist >= hearing_range || source_loudness <= 0.0f) {
        return 0.0f;
    }
    const float atten = 1.0f - (dist < 0.0f ? 0.0f : dist) / hearing_range;
    return source_loudness * atten;
}

// A listener's AUDIOGRAM: frequency-dependent hearing sensitivity. Sounds carry a
// normalized `pitch` in [0,1] (0 = lowest rumble, 1 = highest shriek). A listener
// is most sensitive at `peak_pitch`, with sensitivity falling linearly to 0 over
// `bandwidth` pitch units on either side; `gain` is overall loudness sensitivity
// (bigger ears -> hear fainter sounds farther) and `threshold` is the faintest
// perceived loudness that registers. Every field is an EVOLVABLE genome gene:
// predators tune peak_pitch to their prey's footstep band; prey widen bandwidth +
// alarm-call above the predator's range. All libm-free (triangular falloff).
struct HearingProfile {
    float range = 24.0f;     // m; distance attenuation reaches zero here
    float peak_pitch = 0.5f; // most-sensitive normalized pitch [0,1]
    float bandwidth = 0.5f;  // sensitivity reaches 0 this far in pitch from the peak
    float gain = 1.0f;       // overall sensitivity multiplier (loudness sensitivity)
    float threshold = 0.0f;  // perceived loudness must exceed this to be heard
};

// Triangular frequency sensitivity in [0,1]: 1 at peak_pitch, linearly to 0 at
// +/- bandwidth. Deterministic + libm-free. bandwidth <= 0 -> only the exact peak
// pitch is audible.
[[nodiscard]] inline float PitchSensitivity(const HearingProfile& ear, float pitch) {
    float d = pitch - ear.peak_pitch;
    if (d < 0.0f)
        d = -d;
    if (ear.bandwidth <= 0.0f)
        return d == 0.0f ? 1.0f : 0.0f;
    const float s = 1.0f - d / ear.bandwidth;
    return s > 0.0f ? s : 0.0f;
}

// Perceived loudness of a sound (`source_loudness`, normalized `source_pitch`) at
// distance `dist` THROUGH the listener's audiogram: distance attenuation x gain x
// pitch-sensitivity. Returns 0 when out of range, out of the audible band, or at
// or below the listener's threshold. `heard = HeardLoudness(...) > 0`.
[[nodiscard]] inline float
HeardLoudness(float dist, float source_loudness, float source_pitch, const HearingProfile& ear) {
    const float base = PerceivedLoudness(dist, source_loudness, ear.range);
    if (base <= 0.0f)
        return 0.0f;
    const float perceived = base * ear.gain * PitchSensitivity(ear, source_pitch);
    return perceived > ear.threshold ? perceived : 0.0f;
}

} // namespace luminumbra::ai
