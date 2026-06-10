#include "WaterPass.h"

#include "GBufferPass.h"
#include "LightingPass.h"
#include "PassGlHelpers.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include "luminumbra_common/world/Chunk.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

namespace Luminumbra::Rendering {

namespace {

GLuint make_solid_rgba_texture(const unsigned char rgba[4], const std::string& label) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    PassGl::label_gl_object(GL_TEXTURE, texture, label);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

} // namespace

WaterPass::WaterPass() = default;
WaterPass::~WaterPass() = default;

void WaterPass::init_shader(const std::filesystem::path& root_path) {
    m_water_shader = std::make_unique<Shader>((root_path / "res/shaders/water.vert").string().c_str(), (root_path / "res/shaders/water.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_water_shader ? m_water_shader->Id() : 0u, "shader.water");
}

void WaterPass::init_water_fallback_textures() {
    const unsigned char flat_normal[4] = {128, 128, 255, 255};
    const unsigned char neutral_flow[4] = {128, 128, 0, 0};
    const unsigned char black[4] = {0, 0, 0, 255};
    const unsigned char underwater[4] = {5, 28, 48, 255};

    m_water_flat_normal_texture = make_solid_rgba_texture(flat_normal, "water.fallback.flat_normal");
    m_water_neutral_flow_texture = make_solid_rgba_texture(neutral_flow, "water.fallback.neutral_flow");
    m_water_black_texture = make_solid_rgba_texture(black, "water.fallback.black");
    m_water_underwater_texture = make_solid_rgba_texture(underwater, "water.fallback.underwater");
}

void WaterPass::destroy_water_fallback_textures() {
    if (m_water_flat_normal_texture) { glDeleteTextures(1, &m_water_flat_normal_texture); m_water_flat_normal_texture = 0; }
    if (m_water_neutral_flow_texture) { glDeleteTextures(1, &m_water_neutral_flow_texture); m_water_neutral_flow_texture = 0; }
    if (m_water_black_texture) { glDeleteTextures(1, &m_water_black_texture); m_water_black_texture = 0; }
    if (m_water_underwater_texture) { glDeleteTextures(1, &m_water_underwater_texture); m_water_underwater_texture = 0; }
}

void WaterPass::reset_shader() {
    m_water_shader.reset();
}

void WaterPass::execute(RenderPipeline& pipeline,
                        const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                        const Camera& camera) {
    // --- 1. Set OpenGL State ---
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    // --- 2. Activate Shader and Set Uniforms ---
    m_water_shader->use();

    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();

    // Set matrices
    m_water_shader->setMat4("u_view", view);
    m_water_shader->setMat4("u_projection", projection);
    m_water_shader->setMat4("u_inverse_view", glm::inverse(view));
    m_water_shader->setMat4("u_inverse_projection", glm::inverse(projection));
    // <<< OPTIMIZATION: Set the new pre-combined matrix for the SSR loop
    m_water_shader->setMat4("u_view_projection", projection * view);

    // Set scene and material properties (as before)
    m_water_shader->setVec3("u_camera_pos", camera.Position);
    m_water_shader->setVec2("u_screen_size", glm::vec2(pipeline.m_screen_width, pipeline.m_screen_height));
    m_water_shader->setFloat("u_time", static_cast<float>(glfwGetTime()));
    m_water_shader->setVec3("u_sun_direction", pipeline.m_sun.direction);
    m_water_shader->setVec3("u_sun_color", pipeline.m_sun.color);
    m_water_shader->setVec3("u_shallow_color", glm::vec3(0.3, 0.8, 0.7));
    m_water_shader->setVec3("u_deep_color", glm::vec3(0.02, 0.18, 0.34));
    m_water_shader->setFloat("u_water_depth_scaler", 0.2f);
    m_water_shader->setFloat("u_reflection_power", 0.7f);

    // Bind textures (as before)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pipeline.m_lighting_pass->lighting_fbo().opaque_color_texture);
    m_water_shader->setInt("u_opaque_scene_color", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().depth_texture);
    m_water_shader->setInt("u_opaque_depth", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_water_flat_normal_texture);
    m_water_shader->setInt("u_normal_map", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_water_neutral_flow_texture);
    m_water_shader->setInt("u_flow_map", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_water_black_texture);
    m_water_shader->setInt("u_caustics_texture", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_water_underwater_texture);
    m_water_shader->setInt("u_underwater_texture", 5);

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_water_black_texture);
    m_water_shader->setInt("u_foam_texture", 6);

    // --- 3. Draw Water Meshes ---
    for (const auto& chunk : renderable_chunks) {
        auto it = pipeline.m_water_render_data.find(chunk.id);
        if (it != pipeline.m_water_render_data.end() && it->second.element_count > 0) {
            const auto& render_data = it->second;

            glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk.coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z)));
            m_water_shader->setMat4("u_model", model);

            glBindVertexArray(render_data.vao_id);
            glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
            pipeline.m_last_render_pass_stats.water_draws++;
            pipeline.m_last_render_pass_stats.water_indices_drawn += render_data.element_count;
        }
    }

    // --- 4. Restore OpenGL State ---
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
}

} // namespace Luminumbra::Rendering
