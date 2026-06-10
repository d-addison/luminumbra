#include "SkyboxPass.h"

#include "PassGlHelpers.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

SkyboxPass::SkyboxPass() = default;
SkyboxPass::~SkyboxPass() = default;

void SkyboxPass::init_shader(const std::filesystem::path& root_path) {
    m_skybox_shader = std::make_unique<Shader>(
        (root_path / "res/shaders/skybox.vert").string().c_str(),
        (root_path / "res/shaders/skybox.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_skybox_shader ? m_skybox_shader->Id() : 0u, "shader.skybox");
}

void SkyboxPass::init_geometry() {
    float skyboxVertices[] = { -1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,1.0f,-1.0f,1.0f,1.0f,-1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,-1.0f,1.0f,1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,1.0f };
    glGenVertexArrays(1, &m_skybox_vao);
    glGenBuffers(1, &m_skybox_vbo);
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_skybox_vao, "skybox.vao");
    PassGl::label_gl_object(GL_BUFFER, m_skybox_vbo, "skybox.vbo");
    glBindVertexArray(m_skybox_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_skybox_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

void SkyboxPass::destroy_geometry() {
    if (m_skybox_vao) { glDeleteVertexArrays(1, &m_skybox_vao); m_skybox_vao = 0; }
    if (m_skybox_vbo) { glDeleteBuffers(1, &m_skybox_vbo); m_skybox_vbo = 0; }
}

void SkyboxPass::reset_shader() {
    m_skybox_shader.reset();
}

void SkyboxPass::execute(RenderPipeline& pipeline, const Camera& camera) {
    glDepthFunc(GL_LEQUAL);
    m_skybox_shader->use();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = glm::mat4(glm::mat3(camera.GetViewMatrix())); // remove translation
    m_skybox_shader->setMat4("view", view);
    m_skybox_shader->setMat4("projection", projection);
    m_skybox_shader->setVec3("u_sunDirection", pipeline.m_sun.direction);
    m_skybox_shader->setVec3("u_moonDirection", pipeline.m_moonDirection);
    m_skybox_shader->setFloat("u_sunIntensity", pipeline.m_sun.intensity);
    m_skybox_shader->setFloat("u_time", (float)glfwGetTime());
    glBindVertexArray(m_skybox_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    pipeline.m_last_render_pass_stats.skybox_draws++;
    glBindVertexArray(0);
    glDepthFunc(GL_LESS);
}

} // namespace Luminumbra::Rendering
