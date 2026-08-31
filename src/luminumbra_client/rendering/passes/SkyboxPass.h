#pragma once

#include "../RenderContext.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// Skybox render pass extracted from RenderPipeline. Owns the
// skybox shader and cube geometry; the pipeline keeps orchestration order,
// sun/moon state, stats collection, and the GPU timer issue/collect calls.
// also owns the optional screen-space weather overlay
// (weather_system.frag). The pipeline can defer the overlay so transparent
// water still blends over the sky before rain/fog composite over the full scene.
//
// converted to the RenderContext seam. execute and the
// weather overlay read frame state through a const RenderContext& (built by
// RenderPipeline::make_skybox_context) instead of RenderPipeline&. The cross-pass
// opaque-snapshot copy that the overlay used to perform via m_lighting_pass is
// RELOCATED to the RenderPipeline call site (done under the same guard, at the
// same sequence point); skybox_draws is bumped through ctx.skybox_draw_counter.
class SkyboxPass {
public:
    SkyboxPass();
    ~SkyboxPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_geometry();
    void destroy_geometry();
    void reset_shader();

    void execute(const RenderContext& ctx, const Camera& camera, bool draw_weather_overlay = true);
    void execute_weather_overlay(const RenderContext& ctx, const Camera& camera);

    const std::unique_ptr<Shader>& shader() const {
        return m_skybox_shader;
    }
    const std::unique_ptr<Shader>& weather_shader() const {
        return m_weather_shader;
    }
    u32 vao() const {
        return m_skybox_vao;
    }
    u32 vbo() const {
        return m_skybox_vbo;
    }

private:
    void execute_weather_overlay(const RenderContext& ctx,
                                 const Camera& camera,
                                 const glm::mat4& projection);

    std::unique_ptr<Shader> m_skybox_shader;
    std::unique_ptr<Shader> m_weather_shader;
    u32 m_skybox_vao = 0;
    u32 m_skybox_vbo = 0;
};

} // namespace Luminumbra::Rendering
