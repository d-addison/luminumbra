#pragma once

#include <glm/glm.hpp>

#include <algorithm>

//   (,  ): the tinted-transmission model shared by the
// shadow-tint shader, the (future, ) OIT glass shader, and the bit-exact
// gtests — one definition, no shader/CPU drift.
//
// Beer–Lambert with the absorption coefficient expressed through the authored tint:
// `tint` is the transmission of a UNIT-thickness pane (artist-friendly: "the color
// the light becomes through one pane"), so transmission through thickness d is
//   T(d) = tint^d        (== exp(ln(tint) * d), the Beer–Lambert exponential)
// Multiple panes multiply naturally: the shadow-tint pass draws each pane with
// GL_DST_COLOR/GL_ZERO blending, accumulating exactly this product per texel.
//
// Render-only: never feeds the sim or world_hash.
namespace Luminumbra::Rendering {

inline glm::vec3 GlassTransmission(glm::vec3 unit_tint, float thickness) {
    unit_tint = glm::clamp(unit_tint, glm::vec3(0.0f), glm::vec3(1.0f));
    const float d = std::max(thickness, 0.0f);
    return glm::vec3(std::pow(unit_tint.r, d), std::pow(unit_tint.g, d), std::pow(unit_tint.b, d));
}

} // namespace Luminumbra::Rendering
