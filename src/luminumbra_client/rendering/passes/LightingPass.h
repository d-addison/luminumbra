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
    void init_lighting_fbo(u32 width, u32 height);
    void destroy_lighting_fbo();
    void reset_shader();

    void copy_lighting_color_to_opaque_texture(RenderPipeline& pipeline);
    void execute(RenderPipeline& pipeline, const Camera& camera);

    FrameBufferObject& lighting_fbo() { return m_lighting_fbo; }
    const FrameBufferObject& lighting_fbo() const { return m_lighting_fbo; }
    const std::unique_ptr<Shader>& shader() const { return m_lighting_shader; }

private:
    FrameBufferObject m_lighting_fbo;
    std::unique_ptr<Shader> m_lighting_shader;
};

} // namespace Luminumbra::Rendering
