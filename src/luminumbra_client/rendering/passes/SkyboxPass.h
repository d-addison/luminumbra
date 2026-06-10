#pragma once

#include "../RenderPipeline.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// Skybox render pass extracted from RenderPipeline (T-I2-11g). Owns the
// skybox shader and cube geometry; the pipeline keeps orchestration order,
// sun/moon state, stats collection, and the GPU timer issue/collect calls.
class SkyboxPass {
public:
    SkyboxPass();
    ~SkyboxPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_geometry();
    void destroy_geometry();
    void reset_shader();

    void execute(RenderPipeline& pipeline, const Camera& camera);

    const std::unique_ptr<Shader>& shader() const { return m_skybox_shader; }
    u32 vao() const { return m_skybox_vao; }
    u32 vbo() const { return m_skybox_vbo; }

private:
    std::unique_ptr<Shader> m_skybox_shader;
    u32 m_skybox_vao = 0;
    u32 m_skybox_vbo = 0;
};

} // namespace Luminumbra::Rendering
