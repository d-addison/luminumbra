#pragma once

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

struct FarWorldBiasedDepth {
    glm::vec3 view_position{};
    float depth = 0;
};

// Pure binary32 reference for the production fragment shader. Projection must
// be the actual jittered RH zero-to-one reversed-Z matrix. Refusal leaves out
// unchanged; no clamping can turn an invalid point into a visible surface.
bool ProjectFarWorldRayBias(const glm::vec3& position,
                            float bias_meters,
                            const glm::mat4& projection,
                            FarWorldBiasedDepth& out) noexcept;

} // namespace Luminumbra::Rendering
