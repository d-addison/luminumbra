#pragma once

// game.photo_scoring: a PURE, DETERMINISTIC capture-composition scorer.
// This is the de-risked CORE of the photography game loop (photography): given the
// subjects framed in a shot + the camera's exposure/focus, produce a 0..1 score
// across five axes (composition, lighting, focus, rarity, aether_glow) and a
// weighted total. The aether_glow axis is a BONUS on top
// of the base four: its input defaults to 0 so pre-aether shots score
// byte-identically.
//
// SCOPE. NO render, NO camera, NO GL, NO entt. It operates on plain value structs
// so it can be unit-tested in isolation and consumed by the runtime capture
// pipeline (the camera produces a PhotoShot; this scores it). Keeping the
// rubric here, pure and dependency-free, lets the loop be tuned + reasoned about
// without dragging in the renderer.
//
// DETERMINISM CONTRACT (this is sim-adjacent; if it ever feeds world_hash it must
// be bit-stable). It uses ONLY Luminumbra::DeterministicMath (no libm
// transcendentals: no exp/log/pow; Sqrt is the IEEE-deterministic basic op). All
// arithmetic is float +-*/ + Sqrt, evaluated term by term, so the same PhotoShot
// always yields the same PhotoScore (run==replay). There is NO rng (seed offset
// +24 is reserved for collision-avoidance but the flow is pure and never consumes
// it), NO wall-clock, NO global state. ScorePhoto is a pure function of its input.
//
// GATING. An empty shot (no subjects) is a DEFINED low score (all axes 0, total 0)
// there is nothing to compose, light, or focus on. The scorer never touches a
// registry, so it cannot perturb any baseline hash.

#include <cstdint>
#include <vector>

#include "../core/DeterministicMath.h"

namespace luminumbra::photo {

namespace DM = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset for the photo-scoring subsystem (registry: wind+11,
// weather+12/13, aether+14, plant+15, creature-repro+16, fire+17, soil+18,
// pollination+19, disease+20, photo-scoring+24). The scorer is PURE (no rng), so
// this is recorded for collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kPhotoSeedOffset = 24ull;

// ---------------------------------------------------------------------------
// Value types. Frame coordinates are NORMALIZED device coords in [-1, 1] on both
// axes (0,0 = dead center, +-1 = the frame edges). `size` is the subject's
// apparent footprint as a fraction of the frame [0,1]; `light` is the subject's
// measured luminance [0,1] (0 = black, 1 = blown out); species_id identifies the
// creature/subject so rarity can weight it.
// ---------------------------------------------------------------------------
struct PhotoSubject {
    float ndc_x = 0.0f;
    float ndc_y = 0.0f;
    float size = 0.0f;
    float light = 0.0f;
    int species_id = 0;
};

// A single captured shot: the subjects in frame plus the camera's exposure and
// focus, both normalized [0,1] (exposure: 0 dark.. 1 over-exposed, ~0.5 ideal;
// focus: 0 fully out of focus.. 1 tack sharp on the focal plane).
struct PhotoShot {
    std::vector<PhotoSubject> subjects;
    float exposure = 0.5f;
    float focus = 1.0f;
    // sampled energy-field level in frame [0,1]. The
    // CALLER supplies the already-sampled composite (the stateful layer when
    // sim.aether_state is ON, else the re-derivable ambience -- the same scalar
    // contract StimulusContext::aether_level carries; the scorer never touches
    // a field system). Default 0 == no energy in frame == ZERO aether_glow
    // contribution, so existing callers/tests are byte-unchanged.
    float aether = 0.0f;
};

// The graded result. Each axis is clamped [0,1]; total is the weighted sum of the
// axes, also clamped [0,1].
struct PhotoScore {
    float composition = 0.0f;
    float lighting = 0.0f;
    float focus = 0.0f;
    float rarity = 0.0f;
    float aether_glow = 0.0f; //  bonus axis; 0 whenever shot.aether is 0
    float total = 0.0f;
};

// ---------------------------------------------------------------------------
// Tuning constants. Authored as the nearest float; all math below is float +-*/.
// ---------------------------------------------------------------------------

// Rule-of-thirds gridline coordinate in [-1,1] NDC: the thirds sit at +-1/3.
inline constexpr float kThird = 0.33333334f;

// How sharply the composition reward falls off with distance from the nearest
// rule-of-thirds point. Larger = tighter sweet spot.
inline constexpr float kThirdsFalloff = 1.6f;

// Edge-clip penalty: a subject whose center sits within this margin of a frame
// edge (|ndc| > 1 - margin) is treated as clipped and penalized.
inline constexpr float kEdgeMargin = 0.12f;

// Ideal exposure (mid) and the half-width of the "well exposed" plateau. Lighting
// falls off as exposure departs from kIdealExposure.
inline constexpr float kIdealExposure = 0.5f;
inline constexpr float kExposureFalloff = 2.2f;

// Ideal subject luminance for the LIGHTING axis: mid-to-high, but a blown-out
// (light -> 1) subject is heavily penalized.
inline constexpr float kIdealLight = 0.7f;
inline constexpr float kLightFalloff = 2.5f;
inline constexpr float kBlowoutKnee = 0.92f;    // light above this is "blown"
inline constexpr float kBlowoutPenalty = 0.85f; // fraction removed at full blow

// Total-score axis weights (sum to 1.0). Composition and lighting lead; focus and
// rarity refine.
inline constexpr float kwComposition = 0.34f;
inline constexpr float kwLighting = 0.30f;
inline constexpr float kwFocus = 0.20f;
inline constexpr float kwRarity = 0.16f;

// aether_glow BONUS weight. Deliberately NOT folded into the base
// four weights (which still sum to 1.0): with shot.aether == 0 the bonus term
// is exactly +0.0f and the total is BYTE-IDENTICAL to the pre-aether rubric,
// while a glowing frame can lift the total (still clamped [0,1]).
inline constexpr float kwAetherGlow = 0.10f;

// ---------------------------------------------------------------------------
// Small pure helpers (float +-*/ only — no libm).
// ---------------------------------------------------------------------------
inline float Clamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

inline float AbsF(float v) {
    return v < 0.0f ? -v : v;
}

// |a| of a 2D offset via the IEEE-deterministic Sqrt (the one allowed root op).
inline float Dist2D(float dx, float dy) {
    return DM::Sqrt(dx * dx + dy * dy);
}

// Distance from a coordinate to the NEAREST rule-of-thirds line on one axis: the
// two thirds sit at +kThird and -kThird, so the nearest is min(|c-1/3|,|c+1/3|).
inline float DistToNearestThird(float c) {
    const float d_pos = AbsF(c - kThird);
    const float d_neg = AbsF(c + kThird);
    return d_pos < d_neg ? d_pos : d_neg;
}

// ---------------------------------------------------------------------------
// COMPOSITION. Per subject, reward proximity to the nearest rule-of-thirds
// INTERSECTION (one of the four (+-1/3, +-1/3) power points) and penalize edge
// clipping. The "main" subject (largest apparent size) dominates; remaining
// subjects contribute a balance bonus when the frame is not lopsided. Returns
// [0,1].
// ---------------------------------------------------------------------------
inline float ScoreComposition(const PhotoShot& shot) {
    if (shot.subjects.empty())
        return 0.0f;

    // Identify the main subject as the largest by apparent size (ties: first).
    std::size_t main = 0;
    for (std::size_t i = 1; i < shot.subjects.size(); ++i) {
        if (shot.subjects[i].size > shot.subjects[main].size)
            main = i;
    }

    const PhotoSubject& m = shot.subjects[main];

    // Distance from the main subject to the nearest power point (intersection of
    // the nearest vertical + horizontal third). dx,dy each in [0, ~1.33].
    const float dx = DistToNearestThird(m.ndc_x);
    const float dy = DistToNearestThird(m.ndc_y);
    const float d = Dist2D(dx, dy);

    // Linear-ish falloff: 1 at the power point, decreasing with distance. Clamped.
    float thirds = Clamp01(1.0f - kThirdsFalloff * d);

    // Edge-clip penalty: if the main subject's center is within kEdgeMargin of an
    // edge (|ndc| > 1 - margin), scale composition down toward 0 at the very edge.
    const float edge_x = AbsF(m.ndc_x);
    const float edge_y = AbsF(m.ndc_y);
    const float worst_edge = edge_x > edge_y ? edge_x : edge_y;
    if (worst_edge > 1.0f - kEdgeMargin) {
        // overrun in [0, kEdgeMargin] -> penalty fraction in [0,1].
        const float overrun = worst_edge - (1.0f - kEdgeMargin);
        const float frac = Clamp01(overrun / kEdgeMargin);
        thirds = thirds * (1.0f - frac);
    }

    // Balance bonus: with >1 subject, reward a framing whose subjects' size-weighted
    // centroid sits near the center (not all crammed to one side). Single-subject
    // shots get the neutral mid balance so they are neither rewarded nor punished.
    float balance = 0.5f;
    if (shot.subjects.size() > 1) {
        float sw = 0.0f, cx = 0.0f, cy = 0.0f;
        for (const auto& s : shot.subjects) {
            const float w = s.size > 0.0f ? s.size : 0.0f;
            sw += w;
            cx += w * s.ndc_x;
            cy += w * s.ndc_y;
        }
        if (sw > 0.0f) {
            cx = cx / sw;
            cy = cy / sw;
            const float off = Dist2D(cx, cy); // 0 centered.. ~1.41 cornered
            balance = Clamp01(1.0f - 0.7f * off);
        }
    }

    // Composition = mostly the main-subject thirds placement, lifted by balance.
    const float comp = 0.78f * thirds + 0.22f * balance;
    return Clamp01(comp);
}

// ---------------------------------------------------------------------------
// LIGHTING. Reward mid-to-high subject luminance (ideal ~kIdealLight) and a
// well-centered camera exposure; heavily penalize a BLOWN-OUT subject (light near
// 1) and a too-dark one (light near 0). Averaged over subjects, then folded with
// the exposure term. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreLighting(const PhotoShot& shot) {
    if (shot.subjects.empty())
        return 0.0f;

    float acc = 0.0f;
    for (const auto& s : shot.subjects) {
        const float l = Clamp01(s.light);
        // Falloff around the ideal luminance.
        float lit = Clamp01(1.0f - kLightFalloff * AbsF(l - kIdealLight));
        // Extra blow-out penalty as light pushes past the knee toward 1.0.
        if (l > kBlowoutKnee) {
            const float over = (l - kBlowoutKnee) / (1.0f - kBlowoutKnee); // [0,1]
            lit = lit * (1.0f - kBlowoutPenalty * Clamp01(over));
        }
        acc += lit;
    }
    const float subj_lit = acc / static_cast<float>(shot.subjects.size());

    // Exposure term: peaks at kIdealExposure, falls off either side; over-exposure
    // (exposure -> 1) is as bad as under.
    const float expo =
        Clamp01(1.0f - kExposureFalloff * AbsF(Clamp01(shot.exposure) - kIdealExposure));

    // Lighting = subject luminance lifted/limited by the camera exposure.
    return Clamp01(0.65f * subj_lit + 0.35f * expo);
}

// ---------------------------------------------------------------------------
// FOCUS. Reward a high camera focus value (tack-sharp focal plane) combined with
// a well-centered exposure (a properly exposed frame reads as sharper). The main
// (largest) subject is assumed to be the focal target, so an in-focus shot with a
// prominent subject scores best. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreFocus(const PhotoShot& shot) {
    if (shot.subjects.empty())
        return 0.0f;

    const float f = Clamp01(shot.focus);

    // Main subject prominence (largest size) — a sharp focus on a tiny subject
    // reads as slightly less impactful than on a bold one. size already [0,1].
    float main_size = 0.0f;
    for (const auto& s : shot.subjects) {
        if (s.size > main_size)
            main_size = s.size;
    }
    main_size = Clamp01(main_size);

    // Good exposure supports the perception of sharpness.
    const float expo =
        Clamp01(1.0f - kExposureFalloff * AbsF(Clamp01(shot.exposure) - kIdealExposure));

    // Focus dominates; prominence + exposure refine it.
    const float foc = 0.7f * f + 0.18f * main_size + 0.12f * expo;
    return Clamp01(foc);
}

// ---------------------------------------------------------------------------
// RARITY. Reward rarer species. Without a caller-supplied weight table we use a
// simple, deterministic 1/(count) model over the species PRESENT in the shot: a
// frame with one species of a kind is rarer (higher) than one packed with many of
// the same; and capturing several DISTINCT species lifts the score. species_id is
// only an identity key here — its magnitude carries no meaning. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreRarity(const PhotoShot& shot) {
    if (shot.subjects.empty())
        return 0.0f;

    // Distinct species present (small N, linear scan keeps it allocation-light and
    // fully order-deterministic).
    std::vector<int> ids;
    std::vector<int> counts;
    ids.reserve(shot.subjects.size());
    counts.reserve(shot.subjects.size());
    for (const auto& s : shot.subjects) {
        bool found = false;
        for (std::size_t k = 0; k < ids.size(); ++k) {
            if (ids[k] == s.species_id) {
                ++counts[k];
                found = true;
                break;
            }
        }
        if (!found) {
            ids.push_back(s.species_id);
            counts.push_back(1);
        }
    }

    // Per-species rarity = 1/count (an abundant cluster of one species is common).
    // Average the per-species rarity, then lift by the number of DISTINCT species
    // (capturing variety is prized) up to a saturation.
    float acc = 0.0f;
    for (std::size_t k = 0; k < counts.size(); ++k) {
        acc += 1.0f / static_cast<float>(counts[k]); // counts[k] >= 1
    }
    const float mean_rarity = acc / static_cast<float>(counts.size());

    // Variety bonus: 1 distinct species -> 0; saturates as distinct grows.
    const float distinct = static_cast<float>(ids.size());
    const float variety = Clamp01((distinct - 1.0f) / 3.0f); // full at 4+ species

    return Clamp01(0.75f * mean_rarity + 0.25f * variety);
}

// ---------------------------------------------------------------------------
// AETHER_GLOW. Reward capturing energy phenomena: a pure,
// LINEAR monotonic map of the caller-sampled energy level in frame. Kept linear
// (a single clamp, no falloff shaping) so the monotonic non-decreasing property
// is trivially provable and a zero input contributes EXACTLY zero. An empty
// shot stays the defined all-zero score (the gating rule above): with no
// subject framed there is nothing for the glow to illuminate. Returns [0,1].
// ---------------------------------------------------------------------------
inline float ScoreAetherGlow(const PhotoShot& shot) {
    if (shot.subjects.empty())
        return 0.0f;
    return Clamp01(shot.aether);
}

// ---------------------------------------------------------------------------
// ScorePhoto. The public entry point: compute the five axes, then a clamped
// weighted total. Pure + deterministic; an empty shot is a defined all-zero
// score. The aether_glow bonus term is exactly +0.0f when shot.aether is 0, so
// pre-aether inputs produce a bit-identical total (byte-neutral by default).
// ---------------------------------------------------------------------------
inline PhotoScore ScorePhoto(const PhotoShot& shot) {
    PhotoScore out;
    if (shot.subjects.empty()) {
        return out; // all axes + total = 0: defined low score for an empty frame.
    }

    out.composition = ScoreComposition(shot);
    out.lighting = ScoreLighting(shot);
    out.focus = ScoreFocus(shot);
    out.rarity = ScoreRarity(shot);
    out.aether_glow = ScoreAetherGlow(shot);

    out.total =
        Clamp01(kwComposition * out.composition + kwLighting * out.lighting + kwFocus * out.focus +
                kwRarity * out.rarity + kwAetherGlow * out.aether_glow);
    return out;
}

} // namespace luminumbra::photo
