#pragma once

#include "../RenderPipeline.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// Deferred lighting render pass extracted from RenderPipeline (T-I2-11e).
// Owns the lighting FBO (HDR color + opaque color copy + depth renderbuffer)
// and the lighting shader. The pipeline keeps orchestration order, the
// shared screen quad, G-Buffer/shadow/SSAO inputs (read through the pass
// accessors), light gathering, stats collection, and the GPU timer
// issue/collect calls.
class LightingPass {
public:
    LightingPass();
    ~LightingPass();

    void init_shader(const std::filesystem::path& root_path);
    // Root path retained so the lightning overlay program can be lazily built on
    // first strike (T-I5a-5).
    std::filesystem::path m_root_path;
    void init_lighting_fbo(u32 width, u32 height);
    void destroy_lighting_fbo();
    void reset_shader();

    void copy_lighting_color_to_opaque_texture(RenderPipeline& pipeline);
    void execute(RenderPipeline& pipeline, const Camera& camera);

    // T-I5a-5 (B3): full-scene lightning light-pulse + bolt overlay. A strike is a
    // deterministic SIM world event (in the `weather` world_hash sub-hash); this is
    // the one-way (F2) render response. Drawn as a tiny additive full-screen pass
    // into the lighting FBO AFTER the skybox/water/particles so the transient flash
    // + the screen-space bolt composite over BOTH the lit terrain and the sky (the
    // main lighting shader only shades G-buffer geometry; the skybox overwrites sky
    // pixels). A no-op (zero added cost) when no strike is active. Owned by the
    // LightingPass so the lightning injection stays part of the lighting subsystem.
    void execute_lightning_overlay(RenderPipeline& pipeline, const Camera& camera);

    FrameBufferObject& lighting_fbo() { return m_lighting_fbo; }
    const FrameBufferObject& lighting_fbo() const { return m_lighting_fbo; }
    const std::unique_ptr<Shader>& shader() const { return m_lighting_shader; }

private:
    FrameBufferObject m_lighting_fbo;
    std::unique_ptr<Shader> m_lighting_shader;
    // T-I5a-5 (B3): the lightning overlay program (full-screen additive). Lazily
    // built on first use so non-lightning frames pay nothing.
    std::unique_ptr<Shader> m_lightning_overlay_shader;
    // Scratch texture holding a copy of the composited scene the overlay reads
    // (additive over the current lighting FBO color). Sized to the framebuffer.
    unsigned int m_lightning_scene_copy = 0;
    int m_lightning_copy_w = 0;
    int m_lightning_copy_h = 0;
};

} // namespace Luminumbra::Rendering
