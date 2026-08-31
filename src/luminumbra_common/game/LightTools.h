#pragma once

// game.light_tools: a PURE, DETERMINISTIC light-QUALITY scorer for the
// photography game loop (photography). Where PhotoScoring grades the COMPOSITION of a
// shot (thirds, exposure, focus, rarity), this grades the LIGHT itself: how the
// scene is lit independent of where the subject is framed. Golden-hour warmth, rim/
// back lighting, soft diffuse light vs harsh direct sun, and scene contrast. The
// camera/sky pipeline samples a LightScene (sun position, what the subject faces,
// cloud cover, ambient) and this returns a LightScore the loop can reward.
//
// SCOPE. NO render, NO camera, NO GL, NO entt, NO rng, NO wall-clock. It operates on
// a plain value struct so it can be unit-tested in isolation and fed by the
// runtime sky/lighting model. Keeping the rubric here, pure and
// dependency-free, lets the loop be tuned + reasoned about without dragging in the
// renderer.
//
// DETERMINISM CONTRACT (sim-adjacent; if it ever feeds world_hash it must be
// bit-stable). It uses ONLY Luminumbra::DeterministicMath (no libm transcendentals:
// no exp/log/pow; Cos/Sin/Sqrt are the allowed deterministic ops). All arithmetic is
// float +-*/ + Cos/Sin/Sqrt, evaluated term by term, so the same LightScene always
// yields the same LightScore (run==replay). There is NO rng (seed offset is reserved
// for collision-avoidance but the flow is pure and never consumes it), NO global
// state. ScoreLight is a pure function of its input.
//
// GATING. A default / degenerate scene is a DEFINED score — every axis and the total
// are clamped to [0,1]; there is no registry touch, so the scorer cannot perturb any
// baseline hash.

#include <cstdint>

#include "../core/DeterministicMath.h"

namespace luminumbra::game {

namespace DM = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset for the light-tools subsystem (registry: ... photo-scoring+24,
// weather-events+25, ..., decomposition+27, light-tools+28 — the free slot between
// decomposition+27 and circadian+29). The scorer is PURE (no rng), so this is recorded for
// collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kLightToolsSeedOffset = 28ull;

// ---------------------------------------------------------------------------
// Value types.
//
// LightScene — the lighting condition of a shot, all fields normalized [0,1]:
//   sun_elevation01 : sun height above the horizon. 0 = on the horizon
//                     (dawn/dusk, the GOLDEN HOUR), 0.5 = directly overhead
//                     (harsh noon), 1 = clamp ceiling. Higher = harsher.
//   sun_azimuth01   : sun compass direction [0,1) mapped around a full circle.
//                     Recorded for completeness / future directional rubric; the
//                     rim term uses subject_facing01 as the resolved alignment.
//   subject_facing01: alignment of the subject's facing vs the sun->camera axis.
//                     1 = the sun is BEHIND the subject relative to the camera
//                     (back-lit / rim-lit halo), 0 = the sun is in front of the
//                     subject (flat front lighting).
//   cloud_cover01   : sky cloud fraction. 0 = clear (hard, directional light, high
//                     contrast), 1 = overcast (soft diffuse light, low contrast).
//   ambient01       : ambient/fill light level. Lifts shadows; high ambient softens
//                     and reduces contrast.
// ---------------------------------------------------------------------------
struct LightScene {
    float sun_elevation01 = 0.5f;
    float sun_azimuth01 = 0.0f;
    float subject_facing01 = 0.0f;
    float cloud_cover01 = 0.0f;
    float ambient01 = 0.0f;
};

// The graded result. Each axis is clamped [0,1]; total is the weighted sum of the
// axes, also clamped [0,1].
struct LightScore {
    float golden_hour = 0.0f;
    float rim_light = 0.0f;
    float softness = 0.0f;
    float contrast = 0.0f;
    float total = 0.0f;
};

// ---------------------------------------------------------------------------
// Tuning constants. Authored as the nearest float; all math below is float +-*/
// plus DeterministicMath Cos/Sin/Sqrt.
// ---------------------------------------------------------------------------

// Total-score axis weights (sum to 1.0). Golden hour leads (it is the prize light),
// rim light and contrast shape it, softness is a portrait-leaning refinement.
inline constexpr float kwGolden = 0.36f;
inline constexpr float kwRim = 0.26f;
inline constexpr float kwContrast = 0.22f;
inline constexpr float kwSoftness = 0.16f;

// Golden-hour falloff: how quickly the golden reward drops as the sun climbs from
// the horizon (elevation 0) toward noon (elevation 0.5). Tuned so the reward is a
// smooth cosine ramp that reaches ~0 near overhead.
inline constexpr float kGoldenSpan = 0.5f; // elevation at which golden -> 0.

// Rim light: the back/side-lit halo only reads when the sun is LOW (grazing). This
// is the elevation at which the low-sun gate has fully closed.
inline constexpr float kRimLowSpan = 0.6f;

// Softness: clouds diffuse the light. Ambient fill also softens. Their blend.
inline constexpr float kSoftCloudWeight = 0.7f;
inline constexpr float kSoftAmbientWeight = 0.3f;

// Contrast rewards CLEAR skies (low cloud) and a LOW sun (long raking shadows); high
// ambient washes contrast out.
inline constexpr float kContrastSunSpan = 0.5f; // elevation at which the sun term -> 0.

// ---------------------------------------------------------------------------
// Small pure helpers (float +-*/ + DeterministicMath only — no libm transcendentals).
// ---------------------------------------------------------------------------
inline float LightClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// A smooth 0..1 ramp that is 1 at t=0 and 0 at t=1, using the deterministic cosine
// (half a cosine lobe): r(t) = 0.5*(1 + cos(pi*t)) for t in [0,1]. Monotonically
// DECREASING in t over [0,1]. Built only from DM::Cos so it stays bit-stable.
inline float CosFalloff01(float t) {
    const float tc = LightClamp01(t);
    return LightClamp01(0.5f * (1.0f + DM::Cos(DM::kPi * tc)));
}

// ---------------------------------------------------------------------------
// GOLDEN_HOUR. Peaks when the sun is LOW (elevation near 0 — the warm raking light
// of dawn/dusk) and falls toward harsh overhead noon (elevation ~0.5+). Heavy cloud
// cover mutes the golden glow (the warm directional light is the point), so a clear/
// light-cloud sky at a low sun scores best. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreGoldenHour(const LightScene& s) {
    const float elev = LightClamp01(s.sun_elevation01);
    // Map elevation 0 -> t=0 (full golden) and elevation kGoldenSpan -> t=1 (none),
    // clamped beyond. Cosine falloff gives the smooth golden->harsh ramp.
    const float t = elev / kGoldenSpan;
    float golden = CosFalloff01(t);

    // Overcast skies erase the warm directional glow; scale the golden reward down
    // as cloud cover rises (full clear keeps it, full overcast roughly halves it).
    const float cloud = LightClamp01(s.cloud_cover01);
    golden = golden * (1.0f - 0.5f * cloud);

    return LightClamp01(golden);
}

// ---------------------------------------------------------------------------
// RIM_LIGHT. Rewards BACK/SIDE lighting — a high subject_facing01 (the sun behind
// the subject, throwing a bright rim/halo) — but ONLY when the sun is LOW enough to
// graze the subject's edge. A high sun overhead produces no rim, however the subject
// faces. So rim = facing * low-sun-gate. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreRimLight(const LightScene& s) {
    const float facing = LightClamp01(s.subject_facing01);
    const float elev = LightClamp01(s.sun_elevation01);

    // Low-sun gate: 1 at the horizon, smoothly to 0 by kRimLowSpan elevation.
    const float gate = CosFalloff01(elev / kRimLowSpan);

    return LightClamp01(facing * gate);
}

// ---------------------------------------------------------------------------
// SOFTNESS. Rises with cloud cover (an overcast sky is a giant softbox — diffuse,
// shadowless light, flattering for portraits) and with ambient fill (which lifts and
// softens shadows). A blend of the two. High softness is good for portraits but it
// is the OPPOSITE of golden/contrast (which want hard directional light), captured by
// the separate axes + weights rather than mixed in here. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreSoftness(const LightScene& s) {
    const float cloud = LightClamp01(s.cloud_cover01);
    const float ambient = LightClamp01(s.ambient01);
    const float soft = kSoftCloudWeight * cloud + kSoftAmbientWeight * ambient;
    return LightClamp01(soft);
}

// ---------------------------------------------------------------------------
// CONTRAST. Rewards CLEAR skies (low cloud -> hard directional light -> deep shadows)
// and a LOW sun (long raking shadows sculpt the scene). High cloud cover and high
// ambient both wash contrast out. So contrast = clear-sky * low-sun, knocked down by
// ambient fill. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreContrast(const LightScene& s) {
    const float cloud = LightClamp01(s.cloud_cover01);
    const float ambient = LightClamp01(s.ambient01);
    const float elev = LightClamp01(s.sun_elevation01);

    const float clear = 1.0f - cloud;                            // clear-sky fraction.
    const float low_sun = CosFalloff01(elev / kContrastSunSpan); // 1 low .. 0 high.

    // Directional contrast from a clear sky + low raking sun, dimmed by ambient fill.
    const float contrast = clear * low_sun * (1.0f - 0.6f * ambient);
    return LightClamp01(contrast);
}

// ---------------------------------------------------------------------------
// ScoreLight. The public entry point: compute the four axes, then a clamped weighted
// total. Pure + deterministic; any LightScene yields a defined, bounded LightScore.
// ---------------------------------------------------------------------------
inline LightScore ScoreLight(const LightScene& scene) {
    LightScore out;
    out.golden_hour = ScoreGoldenHour(scene);
    out.rim_light = ScoreRimLight(scene);
    out.softness = ScoreSoftness(scene);
    out.contrast = ScoreContrast(scene);

    out.total = LightClamp01(kwGolden * out.golden_hour + kwRim * out.rim_light +
                             kwContrast * out.contrast + kwSoftness * out.softness);
    return out;
}

} // namespace luminumbra::game
