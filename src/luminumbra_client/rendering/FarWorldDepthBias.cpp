#include "FarWorldDepthBias.h"

#include <cmath>

namespace Luminumbra::Rendering {
namespace {
bool Finite(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

bool ProjectFarWorldRayBias(const glm::vec3& position,
                            float bias_meters,
                            const glm::mat4& projection,
                            FarWorldBiasedDepth& out) noexcept {
    if (!Finite(position) || !std::isfinite(bias_meters) || bias_meters < 0)
        return false;
    const float radius = glm::length(position);
    if (!std::isfinite(radius) || radius <= 0)
        return false;
    const auto biased = position * (1.0f + bias_meters / radius);
    if (!Finite(biased))
        return false;
    const auto clip = projection * glm::vec4(biased, 1.0f);
    if (!Finite(glm::vec3(clip)) || !std::isfinite(clip.w) || clip.w <= 0)
        return false;
    float depth = clip.z / clip.w;
    // The production float matrix can round the near endpoint one ULP above
    // one. Match the shader's exact binary32 successor, not an epsilon band.
    if (depth > 1.0f && depth <= std::nextafter(1.0f, 2.0f))
        depth = 1.0f;
    if (!std::isfinite(depth) || depth < 0 || depth > 1)
        return false;
    out = {biased, depth};
    return true;
}
} // namespace Luminumbra::Rendering
