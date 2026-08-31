#pragma once

//  (, struct extraction): ShadowMap moved out of RenderPipeline.h so
// ShadowPass, LightingPass, and the shared PassGlHelpers cascade-split helpers can
// reference it without pulling the RenderPipeline god-object. Definition unchanged;
// RenderPipeline.h includes this header in its place.

#include "../../include/luminumbra/core/Types.h"

#include <glm/glm.hpp>
#include <vector>

namespace Luminumbra::Rendering {

struct ShadowMap {
    u32 fbo_id = 0;
    u32 depth_texture_array = 0;
    u32 resolution = 2048;
    static constexpr int CASCADE_COUNT = 4;
    std::vector<glm::mat4> light_space_matrices;
    std::vector<float> cascade_splits;
};

} // namespace Luminumbra::Rendering
