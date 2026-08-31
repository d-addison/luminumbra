#pragma once

// game.photo_session: the photography-loop GLUE (photography). This is the
// thin, PURE layer that turns the three independently-graded photo libraries into
// ONE verdict and feeds the progression codex:
//
//   * systems/PhotoScoring.h  (luminumbra::photo) grades a framed PhotoShot into a
//     PhotoScore (composition / lighting / focus / rarity / total).
//   * game/PhotoCamera.h      (luminumbra::game)  models the lens optics: depth of
//     field, exposure quality vs the scene, and subject isolation / bokeh.
//   * game/PhotoCodex.h       (luminumbra::game)  is the per-species collection that
//     keeps the BEST score for each captured subject.
//
// EvaluateShot blends these into a ShotVerdict (composition / exposure /
// focus_isolation / total + a 0..5 star rating). CommitShot logs the verdict's
// total into the codex against the shot's main subject species, keeping the best.
//
// SCOPE. NO render, NO GL, NO entt, NO rng, NO wall-clock, NO global state. It
// operates on plain value structs (mirroring the pure shape of the libraries it
// composes) so it can be unit-tested in isolation and later driven by whatever
// capture pipeline the game grows. It does NOT reimplement any scoring — it calls
// the libraries' own functions and weights their outputs.
//
// DETERMINISM CONTRACT (sim-adjacent: if this ever feeds world_hash it must be
// bit-stable). All arithmetic here is float +-*/ and clamping; the only
// non-trivial math is delegated to the libraries (PhotoScoring uses
// DeterministicMath::Sqrt; PhotoCamera uses an in-house libm-free Log2Approx). No
// libm transcendentals are introduced here. The same ShotInput ALWAYS yields the
// same ShotVerdict (run==replay), and recording captures in any order yields the
// same codex state (the codex itself enforces species_id-sorted determinism).
// There is NO rng: this glue is a pure function flow and consumes no seed stream.
//
// GATING. There is no entt registry and no per-entity state, so nothing here can
// perturb any baseline NetworkStateHash. An empty / subject-less shot is a defined
// low verdict (the underlying ScorePhoto returns all-zero for an empty frame, and
// the optical terms degrade gracefully) rather than a NaN/inf.

#include "../systems/PhotoScoring.h" // luminumbra::photo: PhotoShot/PhotoScore/ScorePhoto
#include "PhotoCamera.h" // luminumbra::game:  LensSettings/ComputeDof/SubjectIsolation/ExposureQuality
#include "PhotoCodex.h" // luminumbra::game:  PhotoCodex::Record

namespace luminumbra::game {

// ---------------------------------------------------------------------------
// Input: everything needed to evaluate ONE captured shot. `composition` is the
// already-framed PhotoShot (subjects + exposure/focus) the PhotoScoring rubric
// grades; `lens` is the optics PhotoCamera grades; `scene_luminance` [0,1] drives
// the exposure target; the main_subject_* fields describe the principal subject so
// the optical DoF/isolation terms and the codex record key on it.
// ---------------------------------------------------------------------------
struct ShotInput {
    ::luminumbra::photo::PhotoShot composition; // framed subjects + camera exposure/focus
    LensSettings lens;                    // focal length / aperture / focus dist / iso / shutter
    float scene_luminance = 0.5f;         // [0,1] ambient scene brightness
    int main_species_id = 0;              // principal subject's species (codex key)
    float main_subject_distance_m = 3.0f; // principal subject distance (metres)
    float main_subject_size_m = 0.5f;     // principal subject physical size (metres)
    // ANNOTATION ONLY — the principal subject's behaviour/time/light context at capture.
    // EvaluateShot NEVER reads this (it is not a scoring input); CommitShot folds its
    // subject_action into the codex so behaviour objectives can match later.
    ObservationMetadata observation;
};

// ---------------------------------------------------------------------------
// Output: the blended verdict. Each axis is [0,1]; `total` is the clamped weighted
// blend; `stars` is a 0..5 rating derived monotonically from `total`.
// ---------------------------------------------------------------------------
struct ShotVerdict {
    float composition = 0.0f;     // from PhotoScoring's composition axis
    float exposure = 0.0f;        // blend of PhotoScoring lighting + PhotoCamera exposure quality
    float focus_isolation = 0.0f; // blend of PhotoScoring focus + PhotoCamera subject isolation
    float total = 0.0f;           // clamped weighted sum of the three axes
    int stars = 0;                // 0..5, monotonic in total
};

// ---------------------------------------------------------------------------
// VERDICT WEIGHTING (sum to 1.0). REBALANCED toward LENS CRAFT: focus/
// isolation now LEADS, composition second, exposure third. The old 0.40/0.30/0.30
// taught "composition > optics", which made the aperture/focus and manual-exposure
// mechanics nearly weightless feedback. Tilting toward focus (0.40) makes deliberate
// DoF the highest-value decision, while composition (0.35) still anchors framing so a
// creamy-bokeh snapshot of a badly-framed subject cannot win on lens alone (guarded by
// the WellComposedDeepBeatsBadlyComposedShallow test).
// ---------------------------------------------------------------------------
inline constexpr float kVerdictWComposition = 0.35f;
inline constexpr float kVerdictWExposure = 0.25f;
inline constexpr float kVerdictWFocusIsolation = 0.40f;

// Per-axis blend weights folding the optics into the rubric. REBALANCED so the lens
// facts LEAD on the two optical axes (the player's exposure + focus decisions are the
// new skill the mechanic teaches): the rubric (what is in frame) still anchors them so
// neither axis is pure-optics.
//   exposure  = kExpRubric * PhotoScoring.lighting + kExpOptics * PhotoCamera.ExposureQuality
//   focus_iso = kFiRubric  * PhotoScoring.focus    + kFiOptics  * PhotoCamera.SubjectIsolation
inline constexpr float kExpRubric = 0.4f;
inline constexpr float kExpOptics = 0.6f;
inline constexpr float kFiRubric = 0.4f;
inline constexpr float kFiOptics = 0.6f;

// ---------------------------------------------------------------------------
// STAR THRESHOLDS. Stars are a monotonic step function of `total` in [0,1]:
//   total <  0.20  -> 0 stars  (missed: blown / out of focus / badly framed)
//   total >= 0.20  -> 1 star
//   total >= 0.40  -> 2 stars
//   total >= 0.60  -> 3 stars
//   total >= 0.75  -> 4 stars
//   total >= 0.88  -> 5 stars  (a well-composed, well-exposed, sharply isolated shot)
// Non-decreasing in total: a higher total can never yield fewer stars.
// ---------------------------------------------------------------------------
inline constexpr float kStar1 = 0.20f;
inline constexpr float kStar2 = 0.40f;
inline constexpr float kStar3 = 0.60f;
inline constexpr float kStar4 = 0.75f;
inline constexpr float kStar5 = 0.88f;

// Local clamp (float +-*/ only; mirrors the libraries' helper to avoid pulling one
// namespace's helper into another).
inline float SessionClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// Map a [0,1] total to a 0..5 star rating. Monotonic non-decreasing step function.
inline int StarsForTotal(float total) {
    const float t = SessionClamp01(total);
    if (t >= kStar5)
        return 5;
    if (t >= kStar4)
        return 4;
    if (t >= kStar3)
        return 3;
    if (t >= kStar2)
        return 2;
    if (t >= kStar1)
        return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// EvaluateShot. PURE: blends the three libraries into a single verdict.
//
//   1. Grade the framed shot with PhotoScoring::ScorePhoto -> composition,
//      lighting, focus (rarity is intentionally NOT surfaced as its own session
//      axis; the codex/progression layer rewards rarity via collection breadth).
//   2. Grade the optics with PhotoCamera: ExposureQuality (vs scene luminance) and
//      SubjectIsolation (DoF / bokeh on the principal subject).
//   3. Blend per axis (rubric + optics), then take the session-weighted total and
//      derive stars.
//
// Deterministic: every step is float +-*/ + the libraries' own deterministic math.
// ---------------------------------------------------------------------------
inline ShotVerdict EvaluateShot(const ShotInput& in) {
    ShotVerdict v;

    // (0) No subject in frame -> a zero verdict (0 stars). The optical axes
    // (ExposureQuality / SubjectIsolation) depend only on lens + scene facts and are
    // nonzero even for an empty frame, so without this gate a photo of nothing could
    // still earn a star. A photograph with no subject is not a photograph.
    if (in.composition.subjects.empty())
        return v;

    // (1) Rubric: grade what is IN the frame.
    const ::luminumbra::photo::PhotoScore rubric = ::luminumbra::photo::ScorePhoto(in.composition);

    // (2) Optics: grade the lens facts against the scene + principal subject.
    CameraSubject subj;
    subj.distance_m = in.main_subject_distance_m;
    subj.size_m = in.main_subject_size_m;

    const float exposure_quality = ExposureQuality(in.lens, in.scene_luminance);
    const float isolation = SubjectIsolation(in.lens, subj);

    // (3) Blend per axis. Composition rides directly on the rubric (it is a pure
    // framing judgement with no optical counterpart here). Exposure and
    // focus/isolation fold the optics into the rubric so a great frame ruined by a
    // bad lens (or vice versa) lands in the middle.
    v.composition = SessionClamp01(rubric.composition);

    v.exposure = SessionClamp01(kExpRubric * SessionClamp01(rubric.lighting) +
                                kExpOptics * SessionClamp01(exposure_quality));

    v.focus_isolation = SessionClamp01(kFiRubric * SessionClamp01(rubric.focus) +
                                       kFiOptics * SessionClamp01(isolation));

    v.total = SessionClamp01(kVerdictWComposition * v.composition + kVerdictWExposure * v.exposure +
                             kVerdictWFocusIsolation * v.focus_isolation);

    v.stars = StarsForTotal(v.total);
    return v;
}

// ---------------------------------------------------------------------------
// CommitShot. Logs the verdict into the codex against the shot's MAIN subject
// species, recording the verdict's `total` as the capture score. The codex keeps
// the BEST score across repeated captures of the same species (a later, worse shot
// never lowers the record) and maintains its species_id-sorted determinism.
//
// PURE side effect: only mutates the passed codex; no global state, no rng. If the
// caller re-evaluates and re-commits the same shot it is idempotent in score (the
// codex max keeps the best) though it counts as another capture.
// ---------------------------------------------------------------------------
inline void CommitShot(PhotoCodex& codex, const ShotInput& in, const ShotVerdict& verdict) {
    codex.Record(in.main_species_id, verdict.total, in.observation.subject_action);
}

} // namespace luminumbra::game
