#pragma once

// game.photo_filters: PURE, DETERMINISTIC post-capture FILTER/grade scoring
// (photography). After a shot is composed + scored (see systems/PhotoScoring.h), the
// player may apply a creative FILTER (a colour grade / look). This module answers a
// single, render-free question: how WELL does a chosen filter SUIT a given scene? A
// good filter amplifies the mood already latent in the scene; a wrong one fights it.
//
// SCOPE. NO render, NO camera, NO GL, NO entt, NO rng, NO wall-clock, NO global
// state. It operates on a plain FilterContext value struct describing the scene's
// colour/light character, so it can be unit-tested in isolation and fed by the
// runtime capture pipeline. Mirrors systems/PhotoScoring.h
// in shape (pure library, value structs, [0,1] outputs).
//
// DETERMINISM CONTRACT. Uses ONLY Luminumbra::DeterministicMath (no libm
// transcendentals: NO exp/log/pow; Sqrt is the IEEE-deterministic basic op). All
// arithmetic is float +-*/ evaluated term by term, so the same FilterContext always
// yields the same FilterScore (run==replay). ScoreFilter and BestFilter are pure
// functions of their inputs.
//
// RUBRIC (how each filter is judged to SUIT a scene; suitability rewards the
// scene CONDITION the filter is made for, mood reflects the emotional lift it adds
// where it fits):
//   * None     — neutral baseline. Constant, scene-independent suitability/mood; a
//                safe middle the stylized filters must BEAT to be chosen.
//   * Vivid    — for saturated subjects in GOOD light: rewards high
//                subject_saturation01 AND low low_light01 (punished in the dark,
//                where pushing saturation just amplifies noise).
//   * Noir     — for LOW-LIGHT, HIGH-CONTRAST scenes: drops colour and leans on
//                tonal drama, so it rewards low_light01 + scene_contrast01 and is
//                indifferent to (even helped by the absence of) saturation.
//   * Warm     — for already-WARM golden scenes: rewards high scene_warmth01 (a warm
//                grade on a warm scene deepens the golden hour).
//   * Cool     — for COLD/blue scenes: rewards LOW scene_warmth01 (a cool grade on a
//                cold scene reads crisp + wintry); the mirror of Warm.
//   * Dramatic — for HIGH-CONTRAST scenes generally: rewards scene_contrast01, with a
//                mild lift in low light (drama suits moody frames).

#include <cstdint>

#include "../core/DeterministicMath.h"

namespace luminumbra::game {

namespace DM = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset for the photo-filters subsystem (registry: ... scavenging+34,
// photo-filters+35 — the next free slot above scavenging). The module is PURE (no rng), so
// this is recorded for collision-avoidance only and is intentionally never consumed.
inline constexpr std::uint64_t kPhotoFilterSeedOffset = 35ull;

// ---------------------------------------------------------------------------
// Filters. None is the neutral baseline; the rest are creative looks.
// ---------------------------------------------------------------------------
enum class PhotoFilter : std::uint8_t {
    None = 0,
    Vivid,
    Noir,
    Warm,
    Cool,
    Dramatic,
};

// Number of filters (kept in sync with the enum) for BestFilter's scan.
inline constexpr int kPhotoFilterCount = 6;

// ---------------------------------------------------------------------------
// Scene character. All scalars are normalized [0,1].
//   scene_warmth01       — colour temperature: 0 = cold/blue, 1 = warm/golden.
//   scene_contrast01     — tonal contrast: 0 = flat, 1 = high-contrast.
//   subject_saturation01 — colourfulness of the subject: 0 = grey, 1 = vivid.
//   low_light01          — darkness of the scene: 0 = bright, 1 = very dark.
// ---------------------------------------------------------------------------
struct FilterContext {
    float scene_warmth01 = 0.5f;
    float scene_contrast01 = 0.5f;
    float subject_saturation01 = 0.5f;
    float low_light01 = 0.5f;
};

// The graded result for one filter against one scene. Each field is clamped [0,1].
//   suitability — how well the scene matches the CONDITION the filter is made for.
//   mood        — the emotional lift the filter adds where it fits.
//   total       — the weighted blend used for ranking (BestFilter maximizes this).
struct FilterScore {
    float suitability = 0.0f;
    float mood = 0.0f;
    float total = 0.0f;
};

// ---------------------------------------------------------------------------
// Tuning constants. Authored as the nearest float; all math below is float +-*/.
// ---------------------------------------------------------------------------

// Blend of suitability vs mood into the ranking total. Suitability leads (a filter
// must FIT before its mood lift counts); mood breaks ties between equally-fitting
// looks. Weights sum to 1.0.
inline constexpr float kwSuitability = 0.65f;
inline constexpr float kwMood = 0.35f;

// None's flat baseline: a neutral, scene-independent score the stylized filters must
// beat to be chosen. Set so a clearly-matched filter wins but a poorly-matched one
// (a vivid grade in the dark, a warm grade on an ice-blue scene) loses to None.
inline constexpr float kNoneSuitability = 0.55f;
inline constexpr float kNoneMood = 0.45f;

// ---------------------------------------------------------------------------
// Small pure helpers (float +-*/ only — no libm).
// ---------------------------------------------------------------------------
inline float FilterClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// ---------------------------------------------------------------------------
// Per-filter scorers. Each returns the {suitability, mood} pair for the named look
// against the scene; total is assembled in ScoreFilter. All inputs are clamped at
// the boundary so an out-of-range FilterContext can never push a result past [0,1].
// ---------------------------------------------------------------------------

// VIVID — saturated subjects in GOOD light. Suitability = saturation rewarded,
// darkness punished. In the dark, pushing saturation amplifies noise, so a high
// low_light scales the whole thing down. Mood = the punch a vivid grade adds, which
// only lands when there's colour AND light to work with.
inline FilterScore ScoreVivid(const FilterContext& c) {
    const float sat = FilterClamp01(c.subject_saturation01);
    const float dark = FilterClamp01(c.low_light01);
    const float light = 1.0f - dark;

    FilterScore s;
    // Suitability: saturation in good light. The (1-dark) factor gates it so a vivid
    // grade in a dark scene scores low even if the subject is nominally colourful.
    s.suitability = FilterClamp01(sat * Lerp(0.35f, 1.0f, light));
    // Mood: vivid pops when both colour and light are present.
    s.mood = FilterClamp01(0.5f * sat + 0.5f * (sat * light));
    return s;
}

// NOIR — LOW-LIGHT, HIGH-CONTRAST. Drops colour and leans on tonal drama, so it
// rewards darkness + contrast and is INDIFFERENT to saturation (it would discard it
// anyway). Mood is high wherever it fits — noir is an inherently moody look.
inline FilterScore ScoreNoir(const FilterContext& c) {
    const float dark = FilterClamp01(c.low_light01);
    const float contrast = FilterClamp01(c.scene_contrast01);

    FilterScore s;
    // Suitability: the geometric-ish blend of darkness and contrast (both wanted).
    s.suitability = FilterClamp01(0.55f * dark + 0.45f * contrast);
    // Mood: noir's drama scales with how much shadow + contrast it has to sculpt.
    s.mood = FilterClamp01(0.5f * dark + 0.5f * contrast + 0.15f * (dark * contrast));
    return s;
}

// WARM — already-WARM golden scenes. A warm grade on a warm scene deepens golden
// hour. Suitability rises with scene_warmth01. Mood rewards the cosy lift, with a
// touch of contrast for shape.
inline FilterScore ScoreWarm(const FilterContext& c) {
    const float warmth = FilterClamp01(c.scene_warmth01);
    const float contrast = FilterClamp01(c.scene_contrast01);

    FilterScore s;
    s.suitability = FilterClamp01(warmth);
    s.mood = FilterClamp01(0.8f * warmth + 0.2f * contrast);
    return s;
}

// COOL — COLD/blue scenes. The mirror of Warm: a cool grade on a cold scene reads
// crisp + wintry. Suitability rises as warmth FALLS (1 - warmth). Mood likewise.
inline FilterScore ScoreCool(const FilterContext& c) {
    const float cold = 1.0f - FilterClamp01(c.scene_warmth01);
    const float contrast = FilterClamp01(c.scene_contrast01);

    FilterScore s;
    s.suitability = FilterClamp01(cold);
    s.mood = FilterClamp01(0.8f * cold + 0.2f * contrast);
    return s;
}

// DRAMATIC — HIGH-CONTRAST generally, with a mild lift in low light (drama suits
// moody frames). Suitability is contrast-led; mood adds the emotional punch.
inline FilterScore ScoreDramatic(const FilterContext& c) {
    const float contrast = FilterClamp01(c.scene_contrast01);
    const float dark = FilterClamp01(c.low_light01);

    FilterScore s;
    s.suitability = FilterClamp01(0.8f * contrast + 0.2f * dark);
    s.mood = FilterClamp01(0.7f * contrast + 0.3f * dark);
    return s;
}

// NONE — neutral baseline. Flat, scene-independent score; the stylized filters must
// beat it to be chosen, and a poorly-matched stylized filter loses to it.
inline FilterScore ScoreNone(const FilterContext&) {
    FilterScore s;
    s.suitability = kNoneSuitability;
    s.mood = kNoneMood;
    return s;
}

// ---------------------------------------------------------------------------
// ScoreFilter. Public entry: dispatch to the per-filter scorer, then assemble the
// clamped weighted total. Pure + deterministic.
// ---------------------------------------------------------------------------
inline FilterScore ScoreFilter(PhotoFilter filter, const FilterContext& ctx) {
    FilterScore s;
    switch (filter) {
        case PhotoFilter::Vivid:
            s = ScoreVivid(ctx);
            break;
        case PhotoFilter::Noir:
            s = ScoreNoir(ctx);
            break;
        case PhotoFilter::Warm:
            s = ScoreWarm(ctx);
            break;
        case PhotoFilter::Cool:
            s = ScoreCool(ctx);
            break;
        case PhotoFilter::Dramatic:
            s = ScoreDramatic(ctx);
            break;
        case PhotoFilter::None:
        default:
            s = ScoreNone(ctx);
            break;
    }
    s.suitability = FilterClamp01(s.suitability);
    s.mood = FilterClamp01(s.mood);
    s.total = FilterClamp01(kwSuitability * s.suitability + kwMood * s.mood);
    return s;
}

// ---------------------------------------------------------------------------
// BestFilter. The filter maximizing total for the scene. Scans the enum in a fixed,
// deterministic order; ties keep the FIRST (lowest enum value) filter, so the result
// is fully reproducible. None being first means a tie defaults to the neutral look.
// ---------------------------------------------------------------------------
inline PhotoFilter BestFilter(const FilterContext& ctx) {
    PhotoFilter best = PhotoFilter::None;
    float best_total = ScoreFilter(PhotoFilter::None, ctx).total;
    for (int i = 1; i < kPhotoFilterCount; ++i) {
        const PhotoFilter f = static_cast<PhotoFilter>(i);
        const float t = ScoreFilter(f, ctx).total;
        if (t > best_total) {
            best_total = t;
            best = f;
        }
    }
    return best;
}

} // namespace luminumbra::game
