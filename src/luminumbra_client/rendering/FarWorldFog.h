#pragma once

#include "luminumbra_common/world/FarHorizon.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

inline float
AerialOuterFade(float ray_distance,
                float outer_radius = static_cast<float>(World::kFarOuterRadiusMeters)) {
    const float t = std::clamp(
        (ray_distance - 0.9f * outer_radius) / (outer_radius - 0.9f * outer_radius), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Inputs are the analytic fog and its display-space in-scatter. Froxel RGB
// is HDR radiance and A is transmittance, as in volumetric_lighting.frag.
// Both historical caps apply before the final Ladder fade. Color is retained.
inline glm::vec4 ComposeFinalFog(float analytic_fog,
                                 glm::vec3 inscatter,
                                 bool froxel_on,
                                 const glm::vec4& froxel,
                                 float raw_ray_distance,
                                 bool ladder,
                                 float cap = 0.94f) {
    float fog = std::min(analytic_fog, cap);
    if (froxel_on) {
        glm::vec3 fx_l(froxel);
        fx_l = fx_l * (2.51f * fx_l + 0.03f) / (fx_l * (2.43f * fx_l + 0.59f) + 0.14f);
        fx_l = glm::pow(glm::max(fx_l, glm::vec3(0)), glm::vec3(1.0f / 2.2f));
        const float fx_fog = std::clamp(1.0f - froxel.a, 0.0f, 1.0f);
        const glm::vec3 blended_num = inscatter * fog * froxel.a + fx_l * fx_fog;
        const float out_fog = std::clamp(fog * froxel.a + fx_fog, 0.0f, cap);
        if (out_fog > 1e-4f)
            inscatter = blended_num / out_fog;
        fog = out_fog;
    }
    // A unit fade is an identity: 1 - (1 - fog) would lose low alpha bits.
    if (ladder && AerialOuterFade(raw_ray_distance) < 1.0f)
        fog = 1.0f - (1.0f - fog) * AerialOuterFade(raw_ray_distance);
    return {inscatter, fog};
}
} // namespace Luminumbra::Rendering
