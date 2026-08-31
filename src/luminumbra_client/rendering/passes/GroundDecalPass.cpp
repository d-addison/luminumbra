#include "GroundDecalPass.h"

#include "../Camera.h"
#include "../PassShaderLayouts.h" // enumerable ExpectedLayout registry
#include "../RenderContext.h"     // position texture + camera via ctx
#include "../Shader.h"
#include "PassGlHelpers.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace Luminumbra::Rendering {

GroundDecalPass::GroundDecalPass() = default;
GroundDecalPass::~GroundDecalPass() = default;

void GroundDecalPass::init_shader(const std::filesystem::path& root_path) {
    const std::string vert = (root_path / "res/shaders/fullscreen_tri.vert").string();
    const std::string frag = (root_path / "res/shaders/scent_decal.frag").string();
    m_shader = std::make_unique<Shader>(vert.c_str(), frag.c_str());
    // validate gPosition + u_scentField against the registry.
    if (m_shader && m_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("scent_decal"))
            m_shader->ValidateLayout(*layout);
    }
}

void GroundDecalPass::init_buffers() {
    glGenVertexArrays(1, &m_vao); // empty; the VS builds the triangle from gl_VertexID
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "ground_decal.vao");
}

void GroundDecalPass::destroy_buffers() {
    if (m_scent_tex) {
        glDeleteTextures(1, &m_scent_tex);
        m_scent_tex = 0;
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    m_tex_extent = 0;
    m_active = false;
}

void GroundDecalPass::reset_shader() {
    m_shader.reset();
}

void GroundDecalPass::update_scent(const ScentFieldRenderMirror& mirror) {
    if (!mirror.valid || !mirror.any_scent || mirror.cells <= 0 ||
        mirror.rg.size() != static_cast<std::size_t>(mirror.cells) * mirror.cells * 2) {
        m_active = false;
        return;
    }
    const int n = mirror.cells;

    if (m_scent_tex == 0 || m_tex_extent != n) {
        if (m_scent_tex)
            glDeleteTextures(1, &m_scent_tex);
        glGenTextures(1, &m_scent_tex);
        PassGl::label_gl_object(GL_TEXTURE, m_scent_tex, "ground_decal.scent_rg16f");
        glBindTexture(GL_TEXTURE_2D, m_scent_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, n, n, 0, GL_RG, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_tex_extent = n;
    } else {
        glBindTexture(GL_TEXTURE_2D, m_scent_tex);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RG, GL_FLOAT, mirror.rg.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    m_origin_x = mirror.origin_x;
    m_origin_z = mirror.origin_z;
    const float world_span = static_cast<float>(n) * mirror.cell_size;
    m_inv_world_span = (world_span > 0.0f) ? (1.0f / world_span) : 0.0f;
    m_active = true;
}

void GroundDecalPass::execute(const RenderContext& ctx) {
    if (!m_active || !m_shader || !m_shader->IsValid() || m_vao == 0 || m_scent_tex == 0)
        return;

    // The decal projects each ground pixel through the inverse view; the frame camera
    // supplies it (identity fallback only if a context arrives without one).
    const glm::mat4 inverse_view =
        ctx.camera ? glm::inverse(ctx.camera->GetViewMatrix()) : glm::mat4(1.0f);

    m_shader->use();
    m_shader->setMat4("u_inverseView", inverse_view);
    m_shader->setVec2("u_scentWorldOrigin", glm::vec2(m_origin_x, m_origin_z));
    m_shader->setFloat("u_scentInvWorldSpan", m_inv_world_span);
    m_shader->setFloat("u_scentScale", m_scent_scale);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id);
    m_shader->setInt("gPosition", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_scent_tex);
    m_shader->setInt("u_scentField", 1);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace Luminumbra::Rendering
