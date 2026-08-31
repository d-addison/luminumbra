#pragma once

//  (, struct extraction): FrameBufferObject moved out of
// RenderPipeline.h so the LightingPass (which holds one by value) + the
// RenderContext seam reference it without pulling the RenderPipeline god-object.
// Definition unchanged; RenderPipeline.h includes this header in its place.

#include "../../include/luminumbra/core/Types.h"

namespace Luminumbra::Rendering {

struct FrameBufferObject {
    u32 fbo_id = 0;
    u32 color_texture = 0;
    u32 opaque_color_texture = 0;
    u32 depth_texture = 0;
};

} // namespace Luminumbra::Rendering
