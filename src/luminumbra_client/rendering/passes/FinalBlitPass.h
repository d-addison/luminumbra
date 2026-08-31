#pragma once

#include "../RenderContext.h"

#include <glad/glad.h>

// The final blit pass resolves the lit-scene color (ctx.lit_scene) to the
// destination (default framebuffer, or the offscreen preview target) with a 1:1
// filtered blit. It depends only on RenderContext, owns no GL resources, and
// leaves timer and statistics orchestration to the pipeline.

namespace Luminumbra::Rendering {

class FinalBlitPass {
public:
    FinalBlitPass() = default;
    ~FinalBlitPass() = default;

    // Resolve ctx.lit_scene -> ctx.dest_fbo (screen or offscreen preview).
    // Byte-identical to the pre-conversion inline block at RenderPipeline.cpp's
    // final-blit; the only change is that source/dest come from the RenderContext
    // + registry handle instead of pipeline privates.
    void execute(const RenderContext& ctx) const {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.lit_scene.id);

        const GLuint draw_fbo = ctx.dest_fbo().id;
        // the source lit_scene is the INTERNAL (scaled) extent; blitting it to the
        // output/preview dest with GL_LINEAR is the render-scale upscale. At scale 1.0
        // internal==screen so this is the byte-identical 1:1 blit as before.
        const GLsizei src_w = static_cast<GLsizei>(ctx.internal_w());
        const GLsizei src_h = static_cast<GLsizei>(ctx.internal_h());
        const GLsizei dst_w = static_cast<GLsizei>(ctx.dest_width());
        const GLsizei dst_h = static_cast<GLsizei>(ctx.dest_height());

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);

        // Clear the destination first to prevent artifacts.
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glBlitFramebuffer(0, 0, src_w, src_h, 0, 0, dst_w, dst_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
};

} // namespace Luminumbra::Rendering
