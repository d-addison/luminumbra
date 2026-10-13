#pragma once

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

struct FarWorldBiasedDepth {
    glm::vec3 view_position{};
    float depth = 0;
};

// Pure binary32 reference for the production fragment shader. Projection must
// be the actual jittered RH zero-to-one reversed-Z matrix. Refusal leaves out
// unchanged. Only the production float near endpoint's one-ULP overshoot is
// clamped to exactly one; all other invalid depths are refused.
bool ProjectFarWorldRayBias(const glm::vec3& position,
                            float bias_meters,
                            const glm::mat4& projection,
                            FarWorldBiasedDepth& out) noexcept;

} // namespace Luminumbra::Rendering
