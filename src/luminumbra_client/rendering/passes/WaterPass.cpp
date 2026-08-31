#include "WaterPass.h"

#include "../PassShaderLayouts.h" // enumerable ExpectedLayout registry
#include "../RenderResourceRegistry.h"
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
    m_water_shader =
        std::make_unique<Shader>((root_path / "res/shaders/water.vert").string().c_str(),
                                 (root_path / "res/shaders/water.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_water_shader ? m_water_shader->Id() : 0u, "shader.water");
    // validate water.frag's sampler bindings (the caustics generator
    // program binds no samplers, so it has no registry entry).
    if (m_water_shader && m_water_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("water"))
            m_water_shader->ValidateLayout(*layout);
    }
    // offscreen caustics generation reuses the shared fullscreen
    // quad layout (lighting_pass.vert) with the dormant caustics fragment
    // shader.
    m_caustics_shader = std::make_unique<Shader>(
        (root_path / "res/shaders/lighting_pass.vert").string().c_str(),
        (root_path / "res/shaders/caustics_generator.frag").string().c_str());
    PassGl::label_gl_object(
        GL_PROGRAM, m_caustics_shader ? m_caustics_shader->Id() : 0u, "shader.water_caustics");
}

void WaterPass::init_water_fallback_textures(RenderResourceRegistry& registry) {
    const unsigned char flat_normal[4] = {128, 128, 255, 255};
    const unsigned char neutral_flow[4] = {128, 128, 0, 0};
    const unsigned char black[4] = {0, 0, 0, 255};
    const unsigned char underwater[4] = {5, 28, 48, 255};

    // Fallback textures stay PASS-OWNED: 1x1 solid-color inputs with uploaded
    // pixel data, not render targets.
    m_water_flat_normal_texture =
        make_solid_rgba_texture(flat_normal, "water.fallback.flat_normal");
    m_water_neutral_flow_texture =
        make_solid_rgba_texture(neutral_flow, "water.fallback.neutral_flow");
    m_water_black_texture = make_solid_rgba_texture(black, "water.fallback.black");
    m_water_underwater_texture = make_solid_rgba_texture(underwater, "water.fallback.underwater");

    // the offscreen caustics target is registry-owned. The desc
    // reproduces the retired glTexImage2D/glTexParameter call (RGBA8, LINEAR,
    // MIRRORED_REPEAT - mirrored repeat avoids hard tile seams since the
    //  pattern is not toroidally tileable). It is then cleared to
    // (0,0,0,0), byte-identical to the retired zero-initialized upload, so
    // consumers that sample it before the first generated frame read black.
    TextureDesc caustics;
    caustics.width = kCausticsResolution;
    caustics.height = kCausticsResolution;
    caustics.internal_format = GL_RGBA8;
    caustics.format = GL_RGBA;
    caustics.type = GL_UNSIGNED_BYTE;
    caustics.min_filter = GL_LINEAR;
    caustics.mag_filter = GL_LINEAR;
    caustics.wrap_s = GL_MIRRORED_REPEAT;
    caustics.wrap_t = GL_MIRRORED_REPEAT;
    caustics.expected_layout = "color_attachment";
    caustics.debug_label = "water.caustics.texture";
    m_caustics_texture = registry.create_texture("water_caustics", caustics).id;

    FboDesc caustics_fbo;
    caustics_fbo.attachments = {{GL_COLOR_ATTACHMENT0, "water_caustics"}};
    caustics_fbo.debug_label = "water.caustics.fbo";
    m_caustics_fbo = registry.create_fbo("water_caustics_fbo", caustics_fbo).id;
    if (m_caustics_fbo == 0) {
        // Match the retired failure path: drop both so consumers fall back to black.
        registry.destroy_owned("water_caustics");
        m_caustics_texture = 0;
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_caustics_fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WaterPass::destroy_water_fallback_textures(RenderResourceRegistry& registry) {
    if (m_water_flat_normal_texture) {
        glDeleteTextures(1, &m_water_flat_normal_texture);
        m_water_flat_normal_texture = 0;
    }
    if (m_water_neutral_flow_texture) {
        glDeleteTextures(1, &m_water_neutral_flow_texture);
        m_water_neutral_flow_texture = 0;
    }
    if (m_water_black_texture) {
        glDeleteTextures(1, &m_water_black_texture);
        m_water_black_texture = 0;
    }
    if (m_water_underwater_texture) {
        glDeleteTextures(1, &m_water_underwater_texture);
        m_water_underwater_texture = 0;
    }
    // The caustics target is registry-owned.
    registry.destroy_owned("water_caustics_fbo");
    registry.destroy_owned("water_caustics");
    m_caustics_fbo = 0;
    m_caustics_texture = 0;
}

void WaterPass::reset_shader() {
    m_water_shader.reset();
    m_caustics_shader.reset();
}

// Renders the animated caustics pattern into the offscreen target. Runs at
// the start of execute, so the cost is reported inside water_gpu_ms. The
// lighting pass (which runs earlier in the frame) samples the previous
// frame's pattern through black_texture; a one-frame lag is invisible for
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
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // water composite into the internal scene
}

WaterDrawStats
WaterPass::execute(const RenderContext& ctx, const WaterPassInput& input, const Camera& camera) {
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

    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                            (float)ctx.screen_width / (float)ctx.screen_height,
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
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
    m_water_shader->setVec2(
        "u_screen_size",
        glm::vec2(ctx.internal_w(), ctx.internal_h())); // samples the internal scene
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
    // shoreline foam is generated procedurally in the shader; the
    // old u_foam_texture slot (black fallback) is gone.

    // --- 3. Draw Water Meshes ---
    // iterate the pre-built draw list (same chunk order -> byte-stable);
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
