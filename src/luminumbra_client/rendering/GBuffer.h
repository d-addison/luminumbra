#pragma once

//  (, struct extraction): GBuffer moved out of RenderPipeline.h so
// passes (DebugView, GBuffer, Lighting, Water, Foliage, Particle, Skybox) and the
// frame-scan tool can reference the deferred attachments by struct without pulling
// the RenderPipeline god-object. Definition unchanged; RenderPipeline.h includes
// this header in its place.

#include "../../include/luminumbra/core/Types.h"

namespace Luminumbra::Rendering {

struct GBuffer {
    u32 fbo_id = 0;
    u32 position_texture = 0;
    u32 normal_texture = 0;
    u32 albedo_texture = 0;
    u32 material_texture = 0;
    // RG16F screen-space motion vectors at COLOR_ATTACHMENT4, consumed by TAAU.
    // The G-buffer reprojects through the previous camera view-projection and
    // instanced foliage evaluates its previous wind-sway position. Independent
    // rigid-object transforms and skinned bone poses still lack previous-frame history.
    u32 motion_vector_texture = 0;
    u32 depth_texture = 0;
};

} // namespace Luminumbra::Rendering
