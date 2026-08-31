#pragma once

#include "../FrameBufferObject.h"
#include "../RenderContext.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Shader;
class RenderResourceRegistry;

// Deferred lighting render pass extracted from RenderPipeline.
// Owns the lighting FBO (HDR color + opaque color copy + depth renderbuffer)
// and the lighting shader. -T12: routed through the RenderContext
// seam — execute/copy/overlay read frame state (g-buffer/shadow/ssao/caustics/
// terrain/aether/light state, screen, the shared quad, stats out-pointer) from
// the RenderContext instead of RenderPipeline. The shadow-cascade fixup (which
// mutates the shared ShadowMap private state + calls a pipeline-private) is
// hoisted to make_lighting_context at the call site; ctx.cascade_splits +
// ctx.light_space_matrices carry the resolved values.
class LightingPass {
public:
    LightingPass();
    ~LightingPass();

    void init_shader(const std::filesystem::path& root_path);
    // Root path retained so the lightning overlay program can be lazily built on
    // first strike.
    std::filesystem::path m_root_path;
    // the lighting FBO (HDR color attachment + depth
    // renderbuffer) and the standalone opaque-color copy are REGISTRY-OWNED. The
    // lightning scene-copy scratch (m_lightning_scene_copy) stays pass-owned - it
    // is a lazily-sized strike-only scratch, not created here. The
    // FrameBufferObject struct caches the owned ids so every reader is unchanged.
    void init_lighting_fbo(RenderResourceRegistry& registry, u32 width, u32 height);
    void destroy_lighting_fbo(RenderResourceRegistry& registry);
    void reset_shader();

    void copy_lighting_color_to_opaque_texture(const RenderContext& ctx);
    void execute(const RenderContext& ctx);

    // full-scene lightning light-pulse + bolt overlay. A strike is a
    // deterministic SIM world event (in the `weather` world_hash sub-hash); this is
    // the one-way  render response. Drawn as a tiny additive full-screen pass
    // into the lighting FBO AFTER the skybox/water/particles so the transient flash
    // + the screen-space bolt composite over BOTH the lit terrain and the sky (the
    // main lighting shader only shades G-buffer geometry; the skybox overwrites sky
    // pixels). A no-op (zero added cost) when no strike is active. Owned by the
    // LightingPass so the lightning injection stays part of the lighting subsystem.
    void execute_lightning_overlay(const RenderContext& ctx);

    FrameBufferObject& lighting_fbo() {
        return m_lighting_fbo;
    }
    const FrameBufferObject& lighting_fbo() const {
        return m_lighting_fbo;
    }
    const std::unique_ptr<Shader>& shader() const {
        return m_lighting_shader;
    }

private:
    FrameBufferObject m_lighting_fbo;
    std::unique_ptr<Shader> m_lighting_shader;
    // the lightning overlay program (full-screen additive). Lazily
    // built on first use so non-lightning frames pay nothing.
    std::unique_ptr<Shader> m_lightning_overlay_shader;
    // Scratch texture holding a copy of the composited scene the overlay reads
    // (additive over the current lighting FBO color). Sized to the framebuffer.
    unsigned int m_lightning_scene_copy = 0;
    int m_lightning_copy_w = 0;
    int m_lightning_copy_h = 0;
};

} // namespace Luminumbra::Rendering