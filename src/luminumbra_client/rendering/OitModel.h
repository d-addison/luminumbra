#pragma once

#include <algorithm>
#include <cmath>

//   (,  ): the weighted-blended OIT model — the
// McGuire–Bavoil depth weight and the resolve algebra, shared by the
// glass_oit shaders and the OitModel gtests. ONE definition; the GLSL mirrors
// these exact expressions.
//
// WBOIT decision: weighted-blended first — its accumulation is a
// commutative SUM, so order independence holds BY CONSTRUCTION (up to float
// non-associativity, sub-LSB for a handful of panes); the per-pixel linked-list
// variant stays a measured contingency if stacked saturated panes ever need it.
//
// Render-only: never feeds the sim or world_hash.
namespace Luminumbra::Rendering::Oit {

// McGuire–Bavoil (2013) eq. 9-family depth weight: near surfaces dominate the
// weighted average, far ones fade. z is the positive view-space depth (metres).
inline float DepthWeight(float z, float alpha) {
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    const float d = std::max(z, 1e-2f) / 200.0f;
    const float w = std::clamp(0.03f / (1e-5f + d * d * d * d), 1e-2f, 3e3f);
    return a * w;
}

// The resolve: the weighted average of the accumulated premultiplied colors,
// with coverage = 1 - reveal (reveal is the product of (1 - alpha_i)).
inline float ResolveCoverage(float reveal) {
    return 1.0f - std::clamp(reveal, 0.0f, 1.0f);
}

} // namespace Luminumbra::Rendering::Oit
