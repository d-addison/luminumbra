#pragma once

#include "../RenderContext.h"

#include <glad/glad.h>

// Spec 016 (016-P1-T01): the FINAL BLIT pass — the first pass routed through the
// RenderContext seam. It resolves the lit-scene color (ctx.lit_scene) to the
// destination (default framebuffer, or the offscreen preview target) with a 1:1
// filtered blit. Deliberately the lowest-coupling / most-reversible pass (spec
// 016 Phasing step 1) so the seam is proven before broad fan-out.
//
// Note: NO `#include "../RenderPipeline.h"` and execute() takes a RenderContext&,
// not a RenderPipeline& — this is the FR-A-003 decoupling the seam exists to
// enable. The pass owns no GL resources; the pipeline keeps the surrounding GPU
// timer + stats orchestration. Header-only (stateless) so the pilot needs no
// sources.cmake change (avoids a fresh-configure on this checkout).

namespace Luminumbra::Rendering {

class FinalBlitPass {
public:
    FinalBlitPass() = default;
    ~FinalBlitPass() = default;

    // Resolve ctx.lit_scene -> ctx.dest_fbo() (screen or offscreen preview).
    // Byte-identical to the pre-conversion inline block at RenderPipeline.cpp's
    // final-blit; the only change is that source/dest come from the RenderContext
    // + registry handle instead of pipeline privates.
    void execute(const RenderContext& ctx) const {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.lit_scene.id);

        const GLuint draw_fbo = ctx.dest_fbo().id;
        const GLsizei src_w = static_cast<GLsizei>(ctx.screen_width);
        const GLsizei src_h = static_cast<GLsizei>(ctx.screen_height);
        const GLsizei dst_w = static_cast<GLsizei>(ctx.dest_width());
        const GLsizei dst_h = static_cast<GLsizei>(ctx.dest_height());

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);

        // Clear the destination first to prevent artifacts.
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glBlitFramebuffer(0, 0, src_w, src_h, 0, 0, dst_w, dst_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    // Verbatim pre-conversion implementation, retained ONLY for the in-process
    // A/B parity gate (016 render gate): the parity harness renders this and
    // execute() into twin targets in the SAME frame and FLIP-diffs them, proving
    // the extraction is byte-identical. Deleted once the seam is fully blessed.
    static void execute_legacy(const RenderContext& ctx) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.lit_scene.id);
        const GLuint draw_fbo = ctx.offscreen_active ? ctx.offscreen_fbo : 0u;
        const GLuint dst_w = ctx.offscreen_active ? ctx.offscreen_w : ctx.screen_width;
        const GLuint dst_h = ctx.offscreen_active ? ctx.offscreen_h : ctx.screen_height;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBlitFramebuffer(0, 0, static_cast<GLint>(ctx.screen_width), static_cast<GLint>(ctx.screen_height),
                          0, 0, static_cast<GLint>(dst_w), static_cast<GLint>(dst_h),
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
};

} // namespace Luminumbra::Rendering
