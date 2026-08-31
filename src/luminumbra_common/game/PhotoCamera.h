#pragma once

// game.photo_camera: a PURE, DETERMINISTIC camera/lens model for the
// photography loop (photography). Given a lens (focal length, aperture, focus
// distance, ISO, shutter) and a subject (distance, apparent size) it computes:
//   * depth of field (circle-of-confusion near/far focus limits + in-focus flag),
//   * exposure value (EV) and an exposure-quality score vs the scene luminance,
//   * a bokeh / subject-isolation score (shallow DoF + subject in focus).
//
// SCOPE. NO render, NO GL, NO entt, NO rng, NO wall-clock, NO global state. It
// operates on plain value structs (mirroring PhotoScoring.h's pure shape) so it
// can be unit-tested in isolation and fed by the runtime capture pipeline. The
// camera produces the optical facts (DoF, EV, isolation); the
// PhotoScoring rubric grades the resulting shot.
//
// DETERMINISM CONTRACT. This is sim-adjacent; if it ever feeds world_hash it must
// be bit-stable. It uses ONLY float +-*/ arithmetic plus an in-house libm-free
// log2 approximation (see Log2Approx below) — it AVOIDS std::log/std::log2/
// std::pow/std::exp entirely, consistent with the DeterministicMath contract
// (which bans libm transcendentals). Every function here is a pure function of its
// inputs: the same LensSettings/CameraSubject always yields the same result
// (run==replay). There is NO rng (this track has no seed offset).
//
// GATING. There is no entt registry and no per-entity state, so nothing here can
// perturb any baseline NetworkStateHash. Degenerate inputs (zero/negative focal
// length, aperture, or distances) are handled to defined, finite results rather
// than producing NaN/inf or dividing by zero.

#include <cstdint>
#include <cstring>

namespace luminumbra::game {

// ---------------------------------------------------------------------------
// Value types. Photographic units throughout: focal length in millimetres,
// aperture as an f-number (f/N, so f/1.4 is "wide"/large opening and f/22 is
// "small"), focus & subject distances in metres, ISO as the standard film-speed
// number (100 = base), shutter in seconds.
// ---------------------------------------------------------------------------
struct LensSettings {
    float focal_length_mm = 50.0f;
    float aperture_f = 2.8f; // f-number N (f/N): SMALL N = WIDE aperture.
    float focus_distance_m = 3.0f;
    float iso = 100.0f;
    float shutter_s = 0.008f; // ~1/125 s
};

// The thing being photographed: how far it is and its physical size (metres).
struct CameraSubject {
    float distance_m = 3.0f;
    float size_m = 0.5f;
};

// Depth-of-field result: the near and far distances (metres) within which the
// scene is acceptably sharp, and whether the subject falls inside that band.
// far_limit_m may be a large sentinel (kInfiniteFar) when the lens is focused at
// or beyond its hyperfocal distance (DoF extends to "infinity").
struct DofResult {
    float near_limit_m = 0.0f;
    float far_limit_m = 0.0f;
    float in_focus = 0.0f; // 1.0 if subject within [near,far], else 0.0
};

// ---------------------------------------------------------------------------
// Optical / scoring constants. Authored as the nearest float; all math below is
// float +-*/ (plus the in-house Log2Approx).
// ---------------------------------------------------------------------------

// Circle of confusion (mm) — the largest blur spot still read as "sharp". 0.03mm
// is the classic full-frame (35mm) standard. Used by the DoF near/far formulas.
inline constexpr float kCircleOfConfusionMm = 0.03f;

// Sentinel for "depth of field extends to infinity" (focused at/beyond
// hyperfocal). A large finite metre value keeps all arithmetic finite + stable.
inline constexpr float kInfiniteFar = 1.0e9f;

// Exposure rubric. ExposureValue is referenced against a TARGET EV derived from
// the scene luminance; quality peaks when the lens EV matches the scene and falls
// off (blown when the lens lets in too much light for the scene, dark when too
// little). kEvFalloff sets how fast quality drops per stop of mismatch.
inline constexpr float kEvFalloff = 0.45f; // per-stop quality falloff

// Map scene luminance [0,1] to a target EV. A mid-grey scene (lum ~0.18, the
// photographic 18% grey card) wants a mid EV; brighter scenes want a higher EV
// (stop down / faster shutter), darker scenes a lower EV. Linear, libm-free.
inline constexpr float kSceneEvMin = 6.0f;  // target EV at luminance 0
inline constexpr float kSceneEvSpan = 9.0f; // EV added as luminance -> 1

// Subject-isolation rubric. Isolation wants a SHALLOW depth of field (small
// near..far band relative to subject distance) AND the subject in focus AND a
// wide aperture (small f-number) for creamy bokeh.
inline constexpr float kIsoApertureRef = 8.0f; // f/8+ is "deep"/poor isolation
inline constexpr float kIsoDepthRef = 4.0f;    // DoF this many * distance = poor

// ---------------------------------------------------------------------------
// Small pure helpers (float +-*/ only — no libm).
// ---------------------------------------------------------------------------
inline float CameraClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

inline float AbsF(float v) {
    return v < 0.0f ? -v : v;
}

// ---------------------------------------------------------------------------
// Log2Approx — a libm-free base-2 logarithm for x > 0.
//
// METHOD. An IEEE-754 binary32 float is (1+m) * 2^e with the exponent stored in
// bits 23..30 and the 23-bit mantissa m in [0,1). We extract the exponent e
// directly from the bits (exact integer) and approximate log2(1+m) over m in
// [0,1) with a 3rd-order minimax-style polynomial. log2(x) = e + log2(1+m).
//
// All steps are integer bit ops + float +-*/ (no libm, no std::log/std::pow), so
// the result is a pure function of x's bits and bit-stable across platforms under
// -ffp-contract=off (pinned on luminumbra_common). The polynomial coefficients
// give max abs error ~3.5e-3 over a full octave — ample for EV (which is itself a
// log2 "stops" quantity tolerant to small fractional error) and far inside any
// sim tolerance. Non-positive inputs return a defined finite floor (kSceneEvMin-
// scale safe) rather than -inf/NaN: log2 of <=0 is undefined, so callers guard
// their domain (aperture/shutter are always > 0 here) and this is a backstop.
// ---------------------------------------------------------------------------
inline float Log2Approx(float x) {
    if (x <= 0.0f) {
        return -126.0f; // defined finite floor (smallest normal exponent) for the
                        // undefined log2(<=0); real call sites pass x > 0.
    }
    std::uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));

    // Exponent field (biased by 127); mantissa field (23 bits).
    const int exp_field = static_cast<int>((bits >> 23) & 0xFFu);
    const std::uint32_t mant_bits = bits & 0x7FFFFFu;

    // Reconstruct e and the fractional mantissa m in [0,1). For normals,
    // value = (1 + m) * 2^(exp_field - 127). (Subnormals — exp_field==0 — are far
    // outside this model's domain; treat as the floor.)
    if (exp_field == 0) {
        return -126.0f;
    }
    const float e = static_cast<float>(exp_field - 127);
    const float m = static_cast<float>(mant_bits) * (1.0f / 8388608.0f); // /2^23

    // log2(1+m) on m in [0,1): least-squares QUARTIC (Horner). Max abs err ~1.9e-4
    // (the earlier cubic was ~8e-3 at mid-octave, failing its own 4e-3 accuracy band).
    const float p = m * (1.43854537f + m * (-0.67807154f + m * (0.32361048f + m * -0.08427316f)));
    return e + p;
}

// ---------------------------------------------------------------------------
// ComputeDof. Classic circle-of-confusion depth of field.
//
//   Convert focal length f (mm) and CoC c (mm) to metres-consistent terms. The
//   hyperfocal distance H = f^2 / (N * c) + f  (in mm), converted to metres.
//   With subject focus distance s (m):
//     near = s*(H - f') / (H + s - 2*f')      [f' = focal length in metres]
//     far  = s*(H - f') / (H - s)             (far -> infinity when s >= H)
//   These are exact rational expressions: float +-*/ only, no transcendentals.
//   in_focus = 1 if the CameraSubject's distance lies within [near, far].
//
// Degenerate guards: non-positive focal length / aperture / focus distance yield
// a defined zero-DoF result (everything at the focus distance, subject in focus
// only if exactly there) rather than dividing by zero.
// ---------------------------------------------------------------------------
inline DofResult ComputeDof(const LensSettings& lens, const CameraSubject& subject) {
    DofResult out;

    const float f_mm = lens.focal_length_mm;
    const float N = lens.aperture_f;
    const float s = lens.focus_distance_m;

    if (f_mm <= 0.0f || N <= 0.0f || s <= 0.0f) {
        out.near_limit_m = s > 0.0f ? s : 0.0f;
        out.far_limit_m = s > 0.0f ? s : 0.0f;
        // With no real depth, the subject is "in focus" only if it sits at s.
        out.in_focus = (subject.distance_m == s) ? 1.0f : 0.0f;
        return out;
    }

    // Hyperfocal distance in mm, then to metres. H_mm = f^2/(N*c) + f.
    const float H_mm = (f_mm * f_mm) / (N * kCircleOfConfusionMm) + f_mm;
    const float H = H_mm * 0.001f;   // mm -> m
    const float f_m = f_mm * 0.001f; // focal length in metres

    // Near limit: s*(H - f) / (H + s - 2f).
    const float near_den = (H + s - 2.0f * f_m);
    float near_limit = s; // fallback
    if (near_den > 0.0f) {
        near_limit = (s * (H - f_m)) / near_den;
    }
    if (near_limit < 0.0f)
        near_limit = 0.0f;

    // Far limit: s*(H - f)/(H - s). When s >= H (focused at/beyond hyperfocal),
    // DoF extends to infinity -> use the large finite sentinel.
    float far_limit;
    const float far_den = (H - s);
    if (far_den <= 0.0f) {
        far_limit = kInfiniteFar;
    } else {
        far_limit = (s * (H - f_m)) / far_den;
        if (far_limit > kInfiniteFar || far_limit < 0.0f) {
            far_limit = kInfiniteFar;
        }
    }

    out.near_limit_m = near_limit;
    out.far_limit_m = far_limit;
    out.in_focus =
        (subject.distance_m >= near_limit && subject.distance_m <= far_limit) ? 1.0f : 0.0f;
    return out;
}

// ---------------------------------------------------------------------------
// ExposureValue. Photographic EV referenced to ISO 100:
//   EV = log2(N^2 / t) - log2(ISO / 100)
// where N = aperture f-number, t = shutter time (s). Larger N (stop down) or
// shorter t (faster shutter) RAISE EV (the scene must be brighter to expose);
// higher ISO LOWERS the required EV. Uses the libm-free Log2Approx, so it is
// deterministic. Degenerate (N<=0, t<=0, iso<=0) is guarded to finite values.
// ---------------------------------------------------------------------------
inline float ExposureValue(const LensSettings& lens) {
    const float N = lens.aperture_f > 0.0f ? lens.aperture_f : 1.0f;
    const float t = lens.shutter_s > 0.0f ? lens.shutter_s : 1.0f;
    const float iso = lens.iso > 0.0f ? lens.iso : 100.0f;

    const float ratio = (N * N) / t;      // > 0
    const float iso_ratio = iso / 100.0f; // > 0
    return Log2Approx(ratio) - Log2Approx(iso_ratio);
}

// ---------------------------------------------------------------------------
// ExposureQuality. [0,1], peaks when the lens EV matches the EV the SCENE wants
// for the given luminance and falls off per stop of mismatch. A scene luminance
// maps linearly to a target EV (kSceneEvMin + lum*kSceneEvSpan). If the lens EV
// is ABOVE the target the frame is under-exposed (dark — the lens demanded more
// light than present); BELOW the target it is over-exposed (blown). Both
// directions lose quality symmetrically per stop. Pure float +-*/ (the EV itself
// uses Log2Approx).
// ---------------------------------------------------------------------------
inline float ExposureQuality(const LensSettings& lens, float scene_luminance) {
    const float lum = CameraClamp01(scene_luminance);
    const float target_ev = kSceneEvMin + lum * kSceneEvSpan;
    const float lens_ev = ExposureValue(lens);

    const float stops_off = AbsF(lens_ev - target_ev);
    return CameraClamp01(1.0f - kEvFalloff * stops_off);
}

// ---------------------------------------------------------------------------
// SubjectIsolation. [0,1] bokeh / subject-isolation score: best when the depth of
// field is SHALLOW (small near..far band relative to subject distance), the
// subject is IN focus, and the aperture is WIDE (small f-number). A landscape-deep
// f/16 shot with everything sharp scores low; a creamy f/1.4 portrait with a tight
// DoF on the subject scores high. Pure: builds on ComputeDof + float +-*/.
//
// RUBRIC.
//   * aperture term: 1 at very wide, -> 0 as N approaches kIsoApertureRef (f/8+).
//   * depth term: 1 when the DoF band is tiny vs the subject distance, -> 0 when
//     it spans >= kIsoDepthRef * distance (or infinity).
//   * focus gate: if the subject is NOT inside the DoF band, isolation collapses
//     (the subject blur defeats "isolation"), scaled to a small residual.
// ---------------------------------------------------------------------------
inline float SubjectIsolation(const LensSettings& lens, const CameraSubject& subject) {
    const DofResult dof = ComputeDof(lens, subject);

    // Aperture term: wide (small N) is good. Linear ramp from f/1.0 (best) to the
    // ref f-number (worst).
    const float N = lens.aperture_f > 0.0f ? lens.aperture_f : kIsoApertureRef;
    const float aperture_term = CameraClamp01((kIsoApertureRef - N) / (kIsoApertureRef - 1.0f));

    // Depth term: shallow band relative to subject distance is good. Band width in
    // metres; an infinite far -> treated as maximally deep (term 0).
    const float dist = subject.distance_m > 0.0f ? subject.distance_m : 1.0f;
    float depth_term;
    if (dof.far_limit_m >= kInfiniteFar) {
        depth_term = 0.0f;
    } else {
        const float band = dof.far_limit_m - dof.near_limit_m; // >= 0
        const float band_ratio = band / (kIsoDepthRef * dist); // 0 = tiny
        depth_term = CameraClamp01(1.0f - band_ratio);
    }

    // Combine the optical terms (aperture + depth weighted), then gate on focus.
    float iso = 0.5f * aperture_term + 0.5f * depth_term;

    if (dof.in_focus < 0.5f) {
        // Subject blurred -> not "isolated", just out of focus. Collapse to a
        // small residual so a missed-focus wide-open shot still ranks below a
        // sharp one.
        iso = iso * 0.15f;
    }

    return CameraClamp01(iso);
}

} // namespace luminumbra::game
