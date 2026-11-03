#pragma once

#include <cmath>
#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

inline constexpr float kFarWorldPositionThresholdMeters = 2000.0f;

// G-buffer positions/depth use nearest sampling. Reconstruct the selected
// texel's ray, including for off-center AO/horizon taps and half-res centers.
inline glm::vec2 FarWorldPositionSampleUv(glm::vec2 uv, glm::vec2 texture_size) {
    return (glm::floor(uv * texture_size) + 0.5f) / texture_size;
}

// P is the actual G-buffer projection: C's selected plane plus current jitter.
// Reversed depth is zero-to-one; depth zero is cleared sky, never a surface.
// Mirrored by far_world_position.glsl. Failure leaves the output untouched.
inline bool ReconstructFarWorldDepthPosition(const glm::vec2& uv,
                                             float depth,
                                             const glm::mat4& inverse_projection,
                                             glm::vec3& position) {
    if (!std::isfinite(depth) || depth <= 0.0f || depth > 1.0f)
        return false;
    const glm::vec4 q = inverse_projection * glm::vec4(uv * 2.0f - 1.0f, depth, 1.0f);
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) ||
        std::abs(q.w) <= 1e-8f)
        return false;
    const glm::vec3 p = glm::vec3(q) / q.w;
    const float radial_distance = glm::length(p);
    if (!std::isfinite(radial_distance) || radial_distance <= 1e-6f || p.z >= 0.0f)
        return false;
    position = p;
    return true;
}

inline bool FarWorldDepthPosition(bool ladder,
                                  const glm::vec3& stored,
                                  const glm::vec2& uv,
                                  float depth,
                                  const glm::mat4& inverse_projection,
                                  glm::vec3& position) {
    if (!ladder) {
        position = stored;
        return true;
    }
    glm::vec3 reconstructed;
    if (!ReconstructFarWorldDepthPosition(uv, depth, inverse_projection, reconstructed))
        return false;
    if (glm::length(reconstructed) > kFarWorldPositionThresholdMeters) {
        position = reconstructed;
        return true;
    }
    if (!std::isfinite(stored.x) || !std::isfinite(stored.y) || !std::isfinite(stored.z))
        return false;
    position = stored;
    return true;
}
} // namespace Luminumbra::Rendering
