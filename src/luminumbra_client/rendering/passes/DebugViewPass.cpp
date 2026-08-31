#include "DebugViewPass.h"

#include "../Camera.h"            // near/far planes for Depth-mode linearization
#include "../PassShaderLayouts.h" // enumerable ExpectedLayout registry
#include "../RenderContext.h"     // the pass reads the G-buffer via ctx handles
#include "../Shader.h"
#include "PassGlHelpers.h"

namespace Luminumbra::Rendering {

DebugViewPass::DebugViewPass() = default;
DebugViewPass::~DebugViewPass() = default;

void DebugViewPass::init_shader(const std::filesystem::path& root_path) {
    const std::string vert = (root_path / "res/shaders/fullscreen_tri.vert").string();
    const std::string frag = (root_path / "res/shaders/debug_view.frag").string();
    m_shader = std::make_unique<Shader>(vert.c_str(), frag.c_str());
    // validate the four G-buffer samplers against the registry.
    if (m_shader && m_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("debug_view"))
            m_shader->ValidateLayout(*layout);
    }
}

void DebugViewPass::init_buffers() {
    glGenVertexArrays(1, &m_vao); // empty; the VS builds the triangle from gl_VertexID
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "debug_view.vao");
}

void DebugViewPass::destroy_buffers() {
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
}

void DebugViewPass::reset_shader() {
    m_shader.reset();
}

void DebugViewPass::execute(const RenderContext& ctx) {
    // Default-OFF guard: Mode::None (0) is a true no-op so the normal render is untouched.
    if (m_mode == Mode::None)
        return;
    if (!m_shader || !m_shader->IsValid() || m_vao == 0)
        return;

    // Diagnostic overlay: never depth-test or blend; we fully replace the bound target.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Near/far come from the frame camera (ctx), used only for Depth-mode linearization.
    // Fall back to the historic defaults if a context arrives without a camera.
    const float near_plane = ctx.camera ? ctx.camera->GetNearPlane() : 0.1f;
    const float far_plane = ctx.camera ? ctx.camera->GetFarPlane() : 4000.0f;

    m_shader->use();
    m_shader->setInt("u_mode", m_mode);
    m_shader->setFloat("u_near", near_plane);
    m_shader->setFloat("u_far", far_plane);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id);
    m_shader->setInt("gPosition", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_normal.id);
    m_shader->setInt("gNormalMaterial", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_albedo.id);
    m_shader->setInt("gAlbedo", 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    m_shader->setInt("gDepth", 3);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace Luminumbra::Rendering
