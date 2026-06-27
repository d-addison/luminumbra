#include "WaterPass.h"

#include "PassGlHelpers.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

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
    // T-I2-16a: offscreen caustics generation reuses the shared fullscreen
    // quad layout (lighting_pass.vert) with the dormant caustics fragment
    // shader.
    m_caustics_shader = std::make_unique<Shader>((root_path / "res/shaders/lighting_pass.vert").string().c_str(), (root_path / "res/shaders/caustics_generator.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_caustics_shader ? m_caustics_shader->Id() : 0u, "shader.water_caustics");
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

    // Offscreen caustics target. Zero-initialized so consumers that sample it
    // before the first generated frame read black, exactly matching the old
    // fallback behavior. Mirrored repeat avoids hard tile seams: the
    // wave-interference pattern is not toroidally tileable.
    const std::vector<unsigned char> zeroed(
        static_cast<std::size_t>(kCausticsResolution) * static_cast<std::size_t>(kCausticsResolution) * 4u, 0u);
    glGenTextures(1, &m_caustics_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_caustics_texture, "water.caustics.texture");
    glBindTexture(GL_TEXTURE_2D, m_caustics_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kCausticsResolution, kCausticsResolution, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeroed.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_caustics_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_caustics_fbo);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_caustics_fbo, "water.caustics.fbo");
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_caustics_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &m_caustics_fbo);
        m_caustics_fbo = 0;
        glDeleteTextures(1, &m_caustics_texture);
        m_caustics_texture = 0;
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WaterPass::destroy_water_fallback_textures() {
    if (m_water_flat_normal_texture) { glDeleteTextures(1, &m_water_flat_normal_texture); m_water_flat_normal_texture = 0; }
    if (m_water_neutral_flow_texture) { glDeleteTextures(1, &m_water_neutral_flow_texture); m_water_neutral_flow_texture = 0; }
    if (m_water_black_texture) { glDeleteTextures(1, &m_water_black_texture); m_water_black_texture = 0; }
    if (m_water_underwater_texture) { glDeleteTextures(1, &m_water_underwater_texture); m_water_underwater_texture = 0; }
    if (m_caustics_fbo) { glDeleteFramebuffers(1, &m_caustics_fbo); m_caustics_fbo = 0; }
    if (m_caustics_texture) { glDeleteTextures(1, &m_caustics_texture); m_caustics_texture = 0; }
}

void WaterPass::reset_shader() {
    m_water_shader.reset();
    m_caustics_shader.reset();
}

// Renders the animated caustics pattern into the offscreen target. Runs at
// the start of execute(), so the cost is reported inside water_gpu_ms. The
// lighting pass (which runs earlier in the frame) samples the previous
// frame's pattern through black_texture(); a one-frame lag is invisible for
// a slowly-flowing intensity field.
void WaterPass::generate_caustics(const RenderContext& ctx) {
    if (m_caustics_fbo == 0 || !m_caustics_shader || !m_caustics_shader->IsValid()) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_caustics_fbo);
    glViewport(0, 0, kCausticsResolution, kCausticsResolution);
    glDisable(GL_DEPTH_TEST);

    m_caustics_shader->use();
    m_caustics_shader->setFloat("u_time", ctx.time_seconds);
    m_caustics_shader->setVec2("u_resolution", glm::vec2(kCausticsResolution, kCausticsResolution));

    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.lit_scene.id);
    glViewport(0, 0, ctx.screen_width, ctx.screen_height);
}

WaterDrawStats WaterPass::execute(const RenderContext& ctx, const WaterPassInput& input, const Camera& camera) {
    WaterDrawStats stats;
    // --- 0. Generate the animated caustics pattern (offscreen) ---
    generate_caustics(ctx);

    // --- 1. Set OpenGL State ---
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    // --- 2. Activate Shader and Set Uniforms ---
    m_water_shader->use();

    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)ctx.screen_width / (float)ctx.screen_height, camera.GetNearPlane(), camera.GetFarPlane());
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
    m_water_shader->setVec2("u_screen_size", glm::vec2(ctx.screen_width, ctx.screen_height));
    m_water_shader->setFloat("u_time", ctx.time_seconds);
    m_water_shader->setVec3("u_sun_direction", ctx.sun.direction);
    m_water_shader->setVec3("u_sun_color", ctx.sun.color);
    m_water_shader->setVec3("u_sky_color", approximate_sky_reflection_color(ctx.sun.intensity));
    m_water_shader->setVec3("u_shallow_color", glm::vec3(0.3, 0.8, 0.7));
    m_water_shader->setVec3("u_deep_color", glm::vec3(0.02, 0.18, 0.34));
    m_water_shader->setFloat("u_water_depth_scaler", 0.2f);
    m_water_shader->setFloat("u_reflection_power", 0.7f);

    // Bind textures (as before)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.opaque_scene.id);
    m_water_shader->setInt("u_opaque_scene_color", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    m_water_shader->setInt("u_opaque_depth", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_water_flat_normal_texture);
    m_water_shader->setInt("u_normal_map", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_water_neutral_flow_texture);
    m_water_shader->setInt("u_flow_map", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, black_texture()); // generated caustics, black fallback otherwise
    m_water_shader->setInt("u_caustics_texture", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_water_underwater_texture);
    m_water_shader->setInt("u_underwater_texture", 5);
    // T-I2-16c: shoreline foam is generated procedurally in the shader; the
    // old u_foam_texture slot (black fallback) is gone.

    // --- 3. Draw Water Meshes ---
    // Spec 016: iterate the pre-built draw list (same chunk order -> byte-stable);
    // stats returned for the call site to fold in.
    for (const auto& item : input.draw_items) {
        m_water_shader->setMat4("u_model", item.model);
        glBindVertexArray(item.vao_id);
        glDrawElements(GL_TRIANGLES, item.element_count, GL_UNSIGNED_INT, 0);
        stats.water_draws++;
        stats.water_indices += item.element_count;
    }

    // --- 4. Restore OpenGL State ---
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    return stats;
}

} // namespace Luminumbra::Rendering
