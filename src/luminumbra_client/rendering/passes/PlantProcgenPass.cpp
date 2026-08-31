#include "PlantProcgenPass.h"

#include "PassGlHelpers.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <GLFW/glfw3.h> // glfwGetTime for render-only leaf wind animation
#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

PlantProcgenPass::PlantProcgenPass() = default;
PlantProcgenPass::~PlantProcgenPass() = default;

void PlantProcgenPass::init_shader(const std::filesystem::path& root_path) {
    const std::string vert = (root_path / "res/shaders/plant_procgen.vert").string();
    const std::string frag = (root_path / "res/shaders/plant_procgen.frag").string();
    m_shader = std::make_unique<Shader>(vert.c_str(), frag.c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shader ? m_shader->Id() : 0u, "shader.plant_procgen");
}

void PlantProcgenPass::init_buffers() {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "plant_procgen.vao");
    PassGl::label_gl_object(GL_BUFFER, m_vbo, "plant_procgen.vbo");
    PassGl::label_gl_object(GL_BUFFER, m_ebo, "plant_procgen.ebo");

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    // Attribute layout mirrors plant_procgen.vert (pos / normal / uv).
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, uv)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, color)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void PlantProcgenPass::destroy_buffers() {
    if (m_ebo) { glDeleteBuffers(1, &m_ebo); m_ebo = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    m_vbo_capacity_bytes = 0;
    m_ebo_capacity_bytes = 0;
    m_index_count = 0;
    m_has_geometry = false;
    m_signature = 0;
}

void PlantProcgenPass::reset_shader() {
    m_shader.reset();
}

void PlantProcgenPass::set_plants(const std::vector<Vertex>& vertices,
                                  const std::vector<std::uint32_t>& indices,
                                  std::uint64_t signature) {
    // Rebuild only when the signature changes (flag toggle / positions change),
    // so the steady-state frame does no upload work.
    if (m_has_geometry && signature == m_signature) {
        return;
    }
    m_signature = signature;
    if (vertices.empty() || indices.empty() || m_vbo == 0 || m_ebo == 0) {
        m_index_count = 0;
        m_has_geometry = false;
        return;
    }

    const GLsizei vbytes = static_cast<GLsizei>(vertices.size() * sizeof(Vertex));
    const GLsizei ibytes = static_cast<GLsizei>(indices.size() * sizeof(std::uint32_t));

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    if (vbytes > m_vbo_capacity_bytes) {
        glBufferData(GL_ARRAY_BUFFER, vbytes, vertices.data(), GL_STATIC_DRAW);
        m_vbo_capacity_bytes = vbytes;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, vbytes, vertices.data());
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    if (ibytes > m_ebo_capacity_bytes) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibytes, indices.data(), GL_STATIC_DRAW);
        m_ebo_capacity_bytes = ibytes;
    } else {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, ibytes, indices.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    m_index_count = indices.size();
    m_has_geometry = true;
}

void PlantProcgenPass::execute(const RenderContext& ctx, const Camera& camera) {
    if (!m_enabled || !m_has_geometry || m_index_count == 0) {
        return;
    }
    if (!m_shader || !m_shader->IsValid() || m_vao == 0) {
        return;
    }

    const glm::mat4 view = camera.GetViewMatrix();
    const glm::mat4 projection = camera.GetProjectionMatrix(
        static_cast<int>(ctx.screen_width), static_cast<int>(ctx.screen_height));

    m_shader->use();
    m_shader->setMat4("projection", projection);
    m_shader->setMat4("view", view);
    m_shader->setMat3("u_normalViewMatrix", glm::mat3(view));
    m_shader->setFloat("u_time", ctx.time_seconds);  // render-only leaf sway
    m_shader->setFloat("u_windStrength", 1.0f);
    // This pass now draws ONLY the gameplay marker octahedra (creatures / foragers /
    // crystals). Make them self-luminous beacons so they read as glowing motes in dark
    // caves where there is no sky light. Packed into gNormalMaterial.b; the lighting
    // pass adds an albedo-tinted glow faded by daylight. (Default uniform is 0.0, so
    // the G-buffer is pixel-identical whenever this is not set.)
    m_shader->setFloat("u_markerEmissive", 0.55f);

    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_index_count), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace Luminumbra::Rendering
