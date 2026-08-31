#pragma once

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

//  rendering: SUN IRRADIANCE FROM TRANSMITTANCE.
//
// The direct-sun MAGNITUDE (what the lighting pass consumes as u_sun.color) is derived
// from the atmosphere's transmittance toward the sun times a top-of-atmosphere solar
// constant, replacing the old authored `smoothstep` intensity ramp + authored horizon
// hue mix. Because the longer optical path at low sun extinguishes blue first, this ONE
// physical term makes the sun both DIM and REDDEN across the golden hour — no
// separate hand-authored ramp.
//
//   t_now = LUT transmittance toward the sun at the current elevation (per-channel RGB)
//   t_ref = the overhead-sun transmittance reference (per-channel RGB)
//
// The TOA solar constant is CALIBRATED to the overhead reference: solar_constant = 1/t_ref,
// so the overhead sun yields unit irradiance and the NOON sun magnitude is preserved
// (the LodGround/RenderHealth noon baselines hold; the division form is bit-identical to
// the prior normalized-transmittance code at high sun). Only the low-sun arc changes.
// This is the answer to   (TOA solar constant + day exposure held at 1.12).
//
// A smooth horizon gate takes the disc to 0 below the horizon, where the transmittance
// LUT is physically meaningless (the sun has set; the moon path lights the night).
//
// Pure function — no GPU, deterministic, render-only (never feeds world_hash). Shared so
// the  contract gate exercises the SAME code the frame uses.
inline glm::vec3
SunIrradiance(const glm::vec3& t_now, const glm::vec3& t_ref, float sun_up_factor) {
    // transmittance × (1/t_ref) solar constant. Division (not reciprocal-multiply) so the
    // high-sun result is bit-identical to the prior `t_now / max(t_ref, 1e-4)` code.
    const glm::vec3 magnitude = t_now / glm::max(t_ref, glm::vec3(1e-4f));
    // Physical sun vanishes below the horizon; keep a thin sliver at/just-below for the
    // sunset glow, then off. (Old ramp used (-0.1, 0.15); this narrower gate lets the
    // transmittance own the low-sun falloff instead of an authored curve.)
    const float above_horizon = glm::smoothstep(-0.08f, 0.06f, sun_up_factor);
    return glm::max(magnitude, glm::vec3(0.0f)) * above_horizon;
}

// The authored sun white point (the prior `noonColor`). The transmittance term above
// carries all elevation-dependent hue/magnitude; this is just the high-sun tint.
inline glm::vec3 SunBaseHue() {
    return glm::vec3(1.0f, 0.95f, 0.85f);
}

} // namespace Luminumbra::Rendering
