#pragma once

//  rendering ( /  ): the photo-mode MANUAL exposure model.
//
// This is the shared, header-only seam that turns the photographer's lens
// (aperture / shutter / ISO -> a photographic Exposure Value) into the RENDER
// exposure multiplier fed to the lighting pass via RenderContext.exposure, plus
// the precedence rule that lets that manual override win over the automatic
// time-of-day exposure curve. BOTH the client push site (main_client.cpp)
// and the pipeline's RenderContext assembly (RenderPipeline.cpp) call these exact
// functions, so the gate that unit-tests them exercises the SAME code the shipping
// frame uses -- there is no second copy to drift.
//
// DETERMINISM. Render-only. The exposure multiplier feeds ONLY u_exposure in the
// lighting pass; it NEVER feeds world_hash. Because it is not
// hashed it is free to use std::exp2 (libm) -- unlike PhotoCamera.h, which is
// sim-adjacent and libm-free by contract. ExposureValue itself (from PhotoCamera.h)
// stays on its in-house libm-free Log2Approx, so the EV the HUD meter shows and the
// EV this mapping consumes are bit-identical.

#include <cmath>

#include <glm/glm.hpp> // glm::smoothstep / glm::mix for the auto TOD exposure curve

#include "luminumbra_common/game/PhotoCamera.h" // luminumbra::game::LensSettings + ExposureValue

namespace Luminumbra::Rendering {

// The day/noon exposure anchor: manual exposure equals this when the lens sits at
// the default (see ManualExposureEvRef), so TOGGLING into photo mode at the default
// lens is continuous with the noon look. MUST equal RenderPipeline's kDayExposure
// (== m_scene_exposure default) -- the analytic TOD curve's DAY value.
inline constexpr float kManualExposureM0 = 1.12f;

// Usable clamp band for the manual multiplier (~3.5 stops below M0 to ~2.8 above).
// Extreme lens settings would otherwise crush to pure black / blow to pure white;
// bounding keeps the tool expressive but sane (and the gate's assertions finite).
inline constexpr float kManualExposureMin = 0.1f;
inline constexpr float kManualExposureMax = 8.0f;

// The reference EV at which manual exposure == kManualExposureM0. Defined as the EV
// of the DEFAULT lens (LensSettings{}), computed through the SAME ExposureValue the
// mapping uses, so ManualExposureMultiplier(default lens) == M0 exactly (self-
// calibrating -- no magic EV constant that could drift from the default lens).
inline float ManualExposureEvRef() {
    return luminumbra::game::ExposureValue(luminumbra::game::LensSettings{});
}

// Map a photographic lens to the RENDER exposure multiplier. PURE-MANUAL (scene-
// INDEPENDENT, per the photography model): stopping down / faster shutter / lower ISO
// RAISE the lens EV and DARKEN the frame; opening up / slower shutter / higher ISO
// LOWER the EV and BRIGHTEN it. Each stop of EV mismatch is a factor of two
// (mult = M0 * 2^(EV_ref - lens_ev)), clamped to the usable band. Render-only.
inline float ManualExposureMultiplier(const luminumbra::game::LensSettings& lens) {
    const float ev_ref = ManualExposureEvRef();
    const float lens_ev = luminumbra::game::ExposureValue(lens);
    float mult = kManualExposureM0 * std::exp2(ev_ref - lens_ev);
    if (mult < kManualExposureMin)
        mult = kManualExposureMin;
    if (mult > kManualExposureMax)
        mult = kManualExposureMax;
    return mult;
}

// Automatic time-of-day exposure fallback, used when GPU metering is disabled.
// It is a pure function of render-derived sun elevation (zero readback and
// deterministic). The atmosphere model spans orders of magnitude day to night; a
// fixed exposure leaves night a flat crush and golden hour over-bright. DAY == kManualExposureM0
// (1.12) so the noon image is preserved and toggling photo mode at the default lens is continuous;
// night is lifted for a navigable moonlit scene; a gentle dip through the low-sun golden band adds
// mood/contrast. GPU metering and manual photo EV both override this fallback.
// Render-only; never included in world_hash.
inline float AutoExposureForElevation(float sunUpFactor) {
    constexpr float kNightExposure = 1.75f; // lift so a moonlit night stays dim-but-navigable
    constexpr float kGoldenExposure =
        1.02f; // gentle dip through the low-sun golden band (mood/contrast)
    const float day = glm::smoothstep(-0.05f, 0.30f, sunUpFactor); // 1 high sun -> 0 below horizon
    const float golden =
        day * (1.0f - glm::smoothstep(0.18f, 0.45f, sunUpFactor)); // peaks low-but-positive
    float exposure = glm::mix(kNightExposure, kManualExposureM0, day);
    exposure = glm::mix(exposure, kGoldenExposure, golden);
    return exposure;
}

// Precedence for the RenderContext.exposure seam: a photo-mode manual override (> 0)
// WINS over the analytic time-of-day base; a negative override (the -1
// sentinel set when photo mode is inactive) falls back to the base. This is the exact
// rule applied at RenderPipeline's ctx assembly.
inline float SelectRenderExposure(float manual_override, float tod_base) {
    return (manual_override > 0.0f) ? manual_override : tod_base;
}

} // namespace Luminumbra::Rendering
