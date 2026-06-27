#include "DebugViewPass.h"

#include "PassGlHelpers.h"
#include "../Shader.h"
#include "../GBuffer.h"   // Spec 016: GBuffer struct (extracted; no god-object dep)

namespace Luminumbra::Rendering {

DebugViewPass::DebugViewPass() = default;
DebugViewPass::~DebugViewPass() = default;

void DebugViewPass::init_shader(const std::filesystem::path& root_path) {
    const std::string vert = (root_path / "res/shaders/fullscreen_tri.vert").string();
    const std::string frag = (root_path / "res/shaders/debug_view.frag").string();
    m_shader = std::make_unique<Shader>(vert.c_str(), frag.c_str());
}

void DebugViewPass::init_buffers() {
    glGenVertexArrays(1, &m_vao);   // empty; the VS builds the triangle from gl_VertexID
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "debug_view.vao");
}

void DebugViewPass::destroy_buffers() {
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
}

void DebugViewPass::reset_shader() { m_shader.reset(); }

void DebugViewPass::execute(const GBuffer& gbuffer, int mode) {
    // Default-OFF guard: Mode::None (0) is a true no-op so the normal render is untouched.
    if (mode == Mode::None) return;
    if (!m_shader || !m_shader->IsValid() || m_vao == 0) return;

    // Diagnostic overlay: never depth-test or blend; we fully replace the bound target.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    m_shader->use();
    m_shader->setInt("u_mode", mode);
    m_shader->setFloat("u_near", m_near);
    m_shader->setFloat("u_far", m_far);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gbuffer.position_texture);
    m_shader->setInt("gPosition", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gbuffer.normal_texture);
    m_shader->setInt("gNormalMaterial", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gbuffer.albedo_texture);
    m_shader->setInt("gAlbedo", 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, gbuffer.depth_texture);
    m_shader->setInt("gDepth", 3);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace Luminumbra::Rendering
