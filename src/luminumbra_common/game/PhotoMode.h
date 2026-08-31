#pragma once

// game.photo_mode: the photography CAPTURE LOOP glue (feature).
//
// This is the thin runtime layer that turns a frame's worth of visible subjects +
// a lens into the existing ShotInput the landed scorers already consume, then scores
// + commits it via PhotoSession (EvaluateShot / CommitShot). It adds NO scoring math:
// every grade still comes from PhotoScoring / PhotoCamera / PhotoSession verbatim.
//
//   capture pipeline (this header):
//     live camera + visible entities  --(client adapter, NOT here)-->  PhotoSubjectView[]
//     PhotoSubjectView[] + lens + scene --> BuildShotInput --> ShotInput
//     ShotInput + PhotoCodex            --> CaptureShot     --> ShotVerdict (+ codex feed)
//
// SCOPE. NO render, NO GL, NO entt, NO rng, NO wall-clock, NO global state. It
// operates on plain value structs (a PhotoSubjectView is one ALREADY-projected /
// derived subject) so it is unit-testable headlessly and the determinism fixture can
// pin it. The registry/GL adapter that PRODUCES the PhotoSubjectViews lives on the
// client side (main_client.cpp); keeping it out of here is what makes this a pure,
// read-mostly observer: nothing here can touch a mutable registry, a tick, or a seed
// stream.
//
// DETERMINISM CONTRACT. All arithmetic is float +-*/ and clamping; the only
// non-trivial math is delegated to the libraries (PhotoScoring uses
// DeterministicMath::Sqrt; PhotoCamera uses an in-house libm-free Log2Approx). No
// libm transcendentals are introduced here. The same PhotoSubjectView[] + lens +
// scene ALWAYS yields the same ShotInput, hence the same ShotVerdict (run==replay),
// and CaptureShot's codex feed is order-independent (the codex itself enforces
// species_id-sorted determinism).
//
// GATING. PhotoModeState + PhotoCodex are CLIENT-ONLY progression containers that
// never touch an entt registry or any baseline NetworkStateHash (PhotoCodex.h
// documents this). A capture is a pure read of camera + component state, so photo
// mode perturbs no world_hash — the sim-isolation guard test asserts this.

#include <cstdint>
#include <string>
#include <vector>

#include "../systems/PhotoScoring.h" // luminumbra::photo: PhotoSubject/PhotoShot
#include "PhotoCamera.h"             // luminumbra::game: LensSettings
#include "PhotoSession.h" // luminumbra::game: ShotInput/ShotVerdict/EvaluateShot/CommitShot

namespace luminumbra::game {

// ---------------------------------------------------------------------------
// PhotoModeState — the client-only photo-mode session state. NOT in world_hash:
// a plain value struct that the client owns and the lens-nudge inputs mutate. The
// `lens` is reused verbatim from PhotoCamera.h so BuildShotInput/CaptureShot feed
// the existing optical scorer.
// ---------------------------------------------------------------------------
struct PhotoModeState {
    bool active = false;     // enter/exit photo mode (TogglePhotoMode input)
    LensSettings lens;       // mutated by the aperture/focus nudge inputs
    int captures = 0;        // session capture counter (telemetry only)
    float last_total = 0.0f; // last verdict total (UI readout)
    int last_stars = 0;      // last verdict stars  (UI readout)
};

// ---------------------------------------------------------------------------
// PhotoSubjectView — ONE candidate subject for the current frame, already projected
// into NDC and with its derived footprint / luminance / species resolved by the
// client adapter. This is the seam between the GL/registry world (which produces
// these) and the pure scoring flow (which consumes them). All fields are plain
// values so the whole capture flow is deterministic + testable headlessly.
// ---------------------------------------------------------------------------
struct PhotoSubjectView {
    float ndc_x = 0.0f; // projected to normalized device coords [-1,1]
    float ndc_y = 0.0f;
    float size = 0.0f;       // apparent footprint as a fraction of the frame [0,1]
    float light = 0.0f;      // derived subject luminance [0,1]
    int species_id = 0;      // codex key (a deterministic proxy is fine for the spike)
    float distance_m = 3.0f; // metres from camera (drives the optical DoF/isolation terms)
    float size_m = 0.5f;     // physical size in metres
    bool in_frustum = true;  // out-of-frustum views are dropped by BuildShotInput
    int subject_action = -1; // the subject's behaviour at capture (brain action; <0 = none).
                             // Carried so the PRINCIPAL subject's behaviour reaches the
                             // capture's ObservationMetadata ( behaviour objectives).
};

// ---------------------------------------------------------------------------
// Small pure clamp helper (float +-*/ only — no libm). Local copy so this header
// does not pull one namespace's helper into another.
// ---------------------------------------------------------------------------
inline float PhotoModeClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// ---------------------------------------------------------------------------
// BuildShotInput. PURE: assemble the existing ShotInput from a frame's worth of
// subject views + the active lens + the scene luminance.
//
//   * Drop any view not in_frustum (it is not in the photograph).
//   * Map each remaining view to the existing photo::PhotoSubject (ndc/size/light/
//     species) — a 1:1 field copy, NO derivation of a new score.
//   * Pick the MAIN subject as the largest apparent `size` (ties: first), matching
//     PhotoScoring's own "main = largest" rule, and key the optical terms + codex on
//     it (main_species_id / main_subject_distance_m / main_subject_size_m).
//   * Drive the framed-shot exposure/focus + scene luminance + lens from the inputs.
//
// An empty frame (no in-frustum views) yields a subject-less ShotInput, which
// EvaluateShot already defines as the zero (0-star) verdict — a photo of nothing.
// ---------------------------------------------------------------------------
inline ShotInput BuildShotInput(const std::vector<PhotoSubjectView>& views,
                                const LensSettings& lens,
                                float scene_luminance,
                                float frame_exposure = 0.5f,
                                float frame_focus = 1.0f,
                                const ObservationMetadata& observation = {}) {
    ShotInput in;
    in.lens = lens;
    in.scene_luminance = PhotoModeClamp01(scene_luminance);
    in.composition.exposure = PhotoModeClamp01(frame_exposure);
    in.composition.focus = PhotoModeClamp01(frame_focus);
    // Annotation: carry the caller's context (time_of_day) and stamp the light from the
    // scene luminance the shot was built with. The principal subject's behaviour is filled
    // below once the main subject is resolved.
    in.observation = observation;
    in.observation.scene_luminance = in.scene_luminance;

    // Project each in-frustum view into the existing PhotoSubject, tracking the
    // largest by apparent size to pick the principal subject. Iterate in the given
    // order so the main-subject tie-break (first of equal size) is deterministic.
    std::size_t main_idx = 0;
    bool have_main = false;
    float main_size = -1.0f;

    for (const PhotoSubjectView& view : views) {
        if (!view.in_frustum)
            continue; // not in the photograph

        ::luminumbra::photo::PhotoSubject s;
        s.ndc_x = view.ndc_x;
        s.ndc_y = view.ndc_y;
        s.size = PhotoModeClamp01(view.size);
        s.light = PhotoModeClamp01(view.light);
        s.species_id = view.species_id;

        // The just-pushed subject is index (size) BEFORE the push.
        const std::size_t this_idx = in.composition.subjects.size();
        in.composition.subjects.push_back(s);

        if (s.size > main_size) {
            main_size = s.size;
            main_idx = this_idx;
            have_main = true;
        }
    }

    // Key the optical (DoF/isolation) terms + the codex record on the main subject.
    if (have_main) {
        // The main subject's source view (same order index — we only appended
        // in-frustum views, but main_idx is the index INTO the appended vector, so
        // re-derive the matching view by walking the in-frustum sequence again).
        std::size_t appended = 0;
        for (const PhotoSubjectView& view : views) {
            if (!view.in_frustum)
                continue;
            if (appended == main_idx) {
                in.main_species_id = view.species_id;
                in.main_subject_distance_m = view.distance_m;
                in.main_subject_size_m = view.size_m;
                in.observation.subject_action =
                    view.subject_action; // principal subject's behaviour
                break;
            }
            ++appended;
        }
    }

    return in;
}

// ---------------------------------------------------------------------------
// CaptureShot. PURE: score + commit, returning the verdict. Reuses
// EvaluateShot/CommitShot VERBATIM (no new scoring math) and feeds the codex against
// the shot's main species. The codex keeps the BEST score per species.
//
// A SUBJECT-LESS shot (a photo of nothing) is scored (a defined zero verdict) but is
// NOT committed: recording an empty frame would pollute the codex with a phantom
// species-0 entry. This gate is a capture-loop policy, not a scoring change — the
// verdict EvaluateShot returns is untouched.
// ---------------------------------------------------------------------------
inline ShotVerdict CaptureShot(PhotoCodex& codex, const ShotInput& shot) {
    const ShotVerdict verdict = EvaluateShot(shot);
    if (!shot.composition.subjects.empty()) {
        CommitShot(codex, shot, verdict);
    }
    return verdict;
}

// ---------------------------------------------------------------------------
// Persisted gallery sidecar. A tiny, deterministic JSON-shaped text record written
// next to the captured PPM (no nlohmann dependency in this pure header — the client
// already owns JSON if it prefers; this string is the canonical, byte-stable form
// the playtest + gate inspect). Floats are emitted with fixed precision so the
// sidecar is reproducible for a given verdict + lens.
// ---------------------------------------------------------------------------
struct PhotoSidecar {
    std::string stamp;               // capture identifier (e.g. timestamp or frame index)
    ShotVerdict verdict;             // stars + axes + total
    int species_id = 0;              // principal subject
    LensSettings lens;               // the lens the shot was taken with
    ObservationMetadata observation; // behaviour/time/light context at capture ()
};

// Format one float with 6 fixed decimals using only integer/char ops (no <iomanip>,
// no libm) so the sidecar text is byte-stable. Handles the [0, ~1e9] range the
// fields live in (stars/axes are [0,1]; lens focal length / focus distance are small
// positive metres/mm). Negative values are emitted with a leading '-'.
inline std::string PhotoFormatFixed6(float value) {
    std::string out;
    if (value < 0.0f) {
        out.push_back('-');
        value = -value;
    }
    // Round to 6 decimals: scale, add 0.5, truncate. Within field ranges this stays
    // well inside uint64 range.
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(static_cast<double>(value) * 1000000.0 + 0.5);
    const std::uint64_t whole = scaled / 1000000ull;
    const std::uint64_t frac = scaled % 1000000ull;

    // Integer part.
    if (whole == 0) {
        out.push_back('0');
    } else {
        std::string digits;
        std::uint64_t w = whole;
        while (w > 0) {
            digits.push_back(static_cast<char>('0' + (w % 10)));
            w /= 10;
        }
        for (std::size_t i = digits.size(); i > 0; --i)
            out.push_back(digits[i - 1]);
    }
    out.push_back('.');
    // Fractional part, zero-padded to 6 digits.
    char fbuf[6];
    std::uint64_t f = frac;
    for (int i = 5; i >= 0; --i) {
        fbuf[i] = static_cast<char>('0' + (f % 10));
        f /= 10;
    }
    for (int i = 0; i < 6; ++i)
        out.push_back(fbuf[i]);
    return out;
}

// Serialize a sidecar to a deterministic JSON string. Pure: same sidecar -> same
// bytes. The shape is { stamp, stars, composition, exposure, focus_isolation, total,
// species_id, lens{ focal_length_mm, aperture_f, focus_distance_m, iso, shutter_s } }.
inline std::string SerializePhotoSidecar(const PhotoSidecar& side) {
    std::string out;
    out += "{\n";
    out += "  \"stamp\": \"" + side.stamp + "\",\n";
    out += "  \"stars\": " + std::to_string(side.verdict.stars) + ",\n";
    out += "  \"composition\": " + PhotoFormatFixed6(side.verdict.composition) + ",\n";
    out += "  \"exposure\": " + PhotoFormatFixed6(side.verdict.exposure) + ",\n";
    out += "  \"focus_isolation\": " + PhotoFormatFixed6(side.verdict.focus_isolation) + ",\n";
    out += "  \"total\": " + PhotoFormatFixed6(side.verdict.total) + ",\n";
    out += "  \"species_id\": " + std::to_string(side.species_id) + ",\n";
    out += "  \"lens\": {\n";
    out += "    \"focal_length_mm\": " + PhotoFormatFixed6(side.lens.focal_length_mm) + ",\n";
    out += "    \"aperture_f\": " + PhotoFormatFixed6(side.lens.aperture_f) + ",\n";
    out += "    \"focus_distance_m\": " + PhotoFormatFixed6(side.lens.focus_distance_m) + ",\n";
    out += "    \"iso\": " + PhotoFormatFixed6(side.lens.iso) + ",\n";
    out += "    \"shutter_s\": " + PhotoFormatFixed6(side.lens.shutter_s) + "\n";
    out += "  },\n";
    out += "  \"observation\": {\n";
    out += "    \"subject_action\": " + std::to_string(side.observation.subject_action) + ",\n";
    out += "    \"time_of_day\": " + PhotoFormatFixed6(side.observation.time_of_day) + ",\n";
    out += "    \"scene_luminance\": " + PhotoFormatFixed6(side.observation.scene_luminance) + "\n";
    out += "  }\n";
    out += "}\n";
    return out;
}

} // namespace luminumbra::game
