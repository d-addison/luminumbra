#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>

#include "../ScentFieldRenderMirror.h"

namespace Luminumbra::Rendering {

class Shader;
struct RenderContext;

// Render-only pheromone-trail ground decal. A fullscreen deferred
// decal: it reads the G-buffer view-space position, projects each ground pixel into
// the ScentField grid, and additively tints the albedo where food/home trails exist.
// Fed a ONE-WAY ScentFieldRenderMirror each frame; the sim never reads it back, so the
// pass is determinism-neutral. Default-OFF until a valid, non-empty mirror is uploaded.
class GroundDecalPass {
public:
    GroundDecalPass();
    ~GroundDecalPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers(); // empty VAO (verts from gl_VertexID) + lazy scent texture
    void destroy_buffers();
    void reset_shader();

    // Upload the latest sim->render scent snapshot (RG16F). No-op (and clears active)
    // when the mirror is invalid or all-zero, so the default render is byte-identical.
    void update_scent(const ScentFieldRenderMirror& mirror);

    bool active() const {
        return m_active;
    }

    // Draw the fullscreen decal. Caller binds the G-buffer FBO + the albedo draw buffer
    // and sets additive blend.  pass contract: the view-space position
    // texture comes from the RenderContext G-buffer handle and the inverse-view from
    // ctx.camera. A no-op unless a valid, non-empty scent mirror has been uploaded.
    void execute(const RenderContext& ctx);

private:
    std::unique_ptr<Shader> m_shader;
    GLuint m_vao = 0;       // empty VAO; the VS builds a fullscreen triangle
    GLuint m_scent_tex = 0; // RG16F, cells x cells
    int m_tex_extent = 0;
    bool m_active = false;
    float m_origin_x = 0.0f, m_origin_z = 0.0f, m_inv_world_span = 0.0f;
    float m_scent_scale = 0.9f; // raw Sample() -> intensity (peak deposit ~3.4 -> visible gradient)
};

} // namespace Luminumbra::Rendering
