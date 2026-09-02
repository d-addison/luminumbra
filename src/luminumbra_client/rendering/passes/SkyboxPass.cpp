#include "SkyboxPass.h"

#include "../PassShaderLayouts.h" // enumerable ExpectedLayout registry
#include "PassGlHelpers.h"
#include "core/IsolationConfig.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

SkyboxPass::SkyboxPass() = default;
SkyboxPass::~SkyboxPass() = default;

void SkyboxPass::init_shader(const std::filesystem::path& root_path) {
    // the atmospheric skybox (scattering, clouds, stars, aurora)
    // supersedes the original flat-gradient skybox.frag. It honors the same
    // uniform interface (u_sunDirection/u_moonDirection/u_sunIntensity/u_time)
    // plus defaulted u_atmosDensity/u_cloudCoverage/u_skyTint uniforms.
    m_skybox_shader =
        std::make_unique<Shader>((root_path / "res/shaders/skybox.vert").string().c_str(),
                                 (root_path / "res/shaders/enhanced_skybox.frag").string().c_str());
    PassGl::label_gl_object(
        GL_PROGRAM, m_skybox_shader ? m_skybox_shader->Id() : 0u, "shader.skybox");
    if (m_skybox_shader && m_skybox_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("skybox"))
            m_skybox_shader->ValidateLayout(*layout);
    }

    // screen-space weather overlay (rain/snow/fog/storm). Reuses
    // the fullscreen-quad vertex stage shared by the SSAO passes.
    m_weather_shader =
        std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(),
                                 (root_path / "res/shaders/weather_system.frag").string().c_str());
    PassGl::label_gl_object(
        GL_PROGRAM, m_weather_shader ? m_weather_shader->Id() : 0u, "shader.weather_overlay");
    // NB: weather_system.frag declares gPosition but never samples it (stripped) ->
    // the registry entry excludes it, so this validates clean + non-vacuously.
    if (m_weather_shader && m_weather_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("weather_overlay"))
            m_weather_shader->ValidateLayout(*layout);
    }
}

void SkyboxPass::init_geometry() {
    // Canonical 36-vertex skybox cube. The previous inline array had lost two
    // floats (106 of 108), so the last four faces rasterized as garbage
    // triangles and the upper sky rendered as black wedges (first caught by
    // skybox_visual_smoke looking up 30 degrees).
    static constexpr float skyboxVertices[] = {
        // back face (z = -1)
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        // left face (x = -1)
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        // right face (x = +1)
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        // front face (z = +1)
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        // top face (y = +1)
        -1.0f,
        1.0f,
        -1.0f,
        1.0f,
        1.0f,
        -1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -1.0f,
        1.0f,
        1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        // bottom face (y = -1)
        -1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        1.0f,
        -1.0f,
        1.0f,
    };
    static_assert(sizeof(skyboxVertices) == 108 * sizeof(float), "skybox cube must be 36 vertices");
    glGenVertexArrays(1, &m_skybox_vao);
    glGenBuffers(1, &m_skybox_vbo);
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_skybox_vao, "skybox.vao");
    PassGl::label_gl_object(GL_BUFFER, m_skybox_vbo, "skybox.vbo");
    glBindVertexArray(m_skybox_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_skybox_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)nullptr);
    glBindVertexArray(0);
}

void SkyboxPass::destroy_geometry() {
    if (m_skybox_vao) {
        glDeleteVertexArrays(1, &m_skybox_vao);
        m_skybox_vao = 0;
    }
    if (m_skybox_vbo) {
        glDeleteBuffers(1, &m_skybox_vbo);
        m_skybox_vbo = 0;
    }
}

void SkyboxPass::reset_shader() {
    m_skybox_shader.reset();
    m_weather_shader.reset();
}

void SkyboxPass::execute(const RenderContext& ctx,
                         const Camera& camera,
                         bool draw_weather_overlay) {
    glDepthFunc(GL_LEQUAL);
    // The camera sits inside the skybox cube, so its upward faces wind
    // clockwise from the inside view and were backface-culled (black wedges
    // above ~50 degrees elevation, first caught by skybox_visual_smoke).
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    if (cull_was_enabled) {
        glDisable(GL_CULL_FACE);
    }
    m_skybox_shader->use();
    //  isolation backdrop override: flat-fill the background instead of the sky
    // dome so an isolated subsystem reads against a void/greenscreen/checker. Default
    // Scene -> mode 0 (byte-stable). (Transparent is a v1 void fallback; the RGBA
    // capture chain is a v2 deferral.)
    {
        namespace SH = Luminumbra::Client::ScenarioHarness;
        const SH::IsolationConfig& iso = *ctx.isolation;
        int backdrop_mode = 0;
        glm::vec3 backdrop_color(0.02f);
        switch (iso.backdrop) {
            case SH::BackdropMode::Void:
                backdrop_mode = 1;
                backdrop_color = glm::vec3(0.02f);
                break;
            case SH::BackdropMode::Greenscreen:
                backdrop_mode = 2;
                backdrop_color = glm::vec3(0.0f, 1.0f, 0.0f);
                break;
            case SH::BackdropMode::Checker:
                backdrop_mode = 3;
                break;
            case SH::BackdropMode::Transparent:
                backdrop_mode = 1;
                backdrop_color = glm::vec3(0.02f);
                break;
            case SH::BackdropMode::Scene:
            default:
                backdrop_mode = 0;
                break;
        }
        m_skybox_shader->setInt("u_backdropMode", backdrop_mode);
        m_skybox_shader->setVec3("u_backdropColor", backdrop_color);
    }
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                            (float)ctx.screen_width / (float)ctx.screen_height,
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
    glm::mat4 view = glm::mat4(glm::mat3(camera.GetViewMatrix())); // remove translation
    m_skybox_shader->setMat4("view", view);
    m_skybox_shader->setMat4("projection", projection);
    // The pipeline stores light-travel directions (sun shines downward at
    // noon). The skybox shader compares dot(viewDir, u_sunDirection) against
    // ~1 to place the sun/moon discs, so it needs the toward-body directions:
    // with the old wiring the discs sat below the horizon and never rendered.
    m_skybox_shader->setVec3("u_sunDirection", -ctx.sun.direction);
    m_skybox_shader->setVec3("u_moonDirection", -ctx.moon_direction);
    m_skybox_shader->setFloat("u_sunIntensity", ctx.sun.intensity);
    // continuous day->twilight->night factor from the
    // sun elevation (1 sun high, ~0 sun below horizon). The dome derives its
    // brightness/tint from this instead of the clamped u_sunIntensity, so the
    // dusk dome warms/darkens and the night dome goes genuinely dark in lockstep
    // with the terrain lighting that shares the same elevation signal.
    m_skybox_shader->setFloat("u_skyDayFactor", ctx.sky_day_factor);
    m_skybox_shader->setFloat("u_time", ctx.time_seconds);
    //  bind the Hillaire scattering LUTs. The sky-view LUT supplies the
    // dome COLOR and the transmittance LUT colors the sun disc; both are shared
    // with the lighting pass + aerial-perspective term for a coherent palette.
    const bool sky_lut_ready = ctx.sky_lut_ready;
    if (sky_lut_ready) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx.sky_view_lut.id);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx.transmittance_lut.id);
        m_skybox_shader->setInt("u_skyViewLut", 0);
        m_skybox_shader->setInt("u_transmittanceLut", 1);
    }
    m_skybox_shader->setInt("u_useSkyLut", sky_lut_ready ? 1 : 0);
    // dot(toward-sun, up): the transmittance LUT's mu axis. up is +Y; the
    // pipeline stores the light-travel direction, so toward-sun is -direction.
    m_skybox_shader->setFloat("u_sunCosZenith",
                              glm::dot(-ctx.sun.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    // push the wind-advected cloud-coverage state to the sky-dome.
    // The shader holds GLSL defaults, but the live scroll offset/coverage must be
    // pushed each frame or the dome clouds never drift (and never register with the
    // lighting-pass cast shadow that shares cloudCoverageAt).  .
    // The dome cloud LAYER is always present at the state's fair-weather coverage
    // (default 0.45) -- matching the legacy always-on sky clouds the SkyboxVisual
    // palette gate frames; the cloud.enabled flag gates the PROJECTED CAST SHADOW
    // + wind scroll in the lighting pass, not the visual dome layer.
    {
        const CloudRenderState& cloud = ctx.cloud_state;
        const float coverage = cloud.coverage_amount;
        m_skybox_shader->setVec2("u_cloudScrollOffset", cloud.scroll_offset);
        m_skybox_shader->setFloat("u_cloudCoverageAmount", coverage);
        m_skybox_shader->setFloat("u_cloudBiomeVariation", cloud.biome_variation);
        m_skybox_shader->setFloat("u_cloudPlaneHeight", cloud.plane_height);
        m_skybox_shader->setFloat("u_cloudShadowStrength", cloud.shadow_strength);
    }

    //
    //  (defect 5) AURORA gate -- a deep-night-only strength from the sun's RAW
    //  elevation. u_skyDayFactor cannot separate dusk (sun on the horizon) from
    //  night (sun below), so derive the gate here: the sun must be clearly below
    //  the horizon (sun_up_factor well negative) before the aurora opens. 0 through
    //  day + dusk, ramping to 1 only in deep night -> no aurora smear at dusk.
    //  (defect 3) NIGHT-STORM floor -- push the storm intensity so the shader can
    //  hold a small dark-grey sky floor at night (legible storm) without lifting
    //  the clear-night dome. 0 for clear sky.
    {
        // sun_up_factor = dot(light-travel-dir, down) = sun elevation sign; >0 day,
        // ~0 at the horizon (dusk/dawn), negative once the sun has set.
        const float sun_up_factor = glm::dot(ctx.sun.direction, glm::vec3(0.0f, -1.0f, 0.0f));
        // Open only once the sun is WELL below the horizon so the aurora is fully
        // absent through dusk/twilight (the TimeOfDaySweep aurora-gating asserts no
        // green chroma at dusk) and only the deep-night sky shows curtains: 0 at
        // sun_up_factor >= -0.22 (dusk/twilight), full by -0.42 (deep night).
        const float aurora_strength = glm::smoothstep(-0.22f, -0.42f, sun_up_factor);
        m_skybox_shader->setFloat("u_auroraStrength", aurora_strength);

        const float storm_floor = glm::clamp(ctx.weather_state->storm_intensity, 0.0f, 1.0f);
        m_skybox_shader->setFloat("u_stormSkyFloor", storm_floor);
    }
    glBindVertexArray(m_skybox_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    (*ctx.skybox_draw_counter)++;
    glBindVertexArray(0);
    if (sky_lut_ready) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (cull_was_enabled) {
        glEnable(GL_CULL_FACE);
    }
    glDepthFunc(GL_LESS);

    if (draw_weather_overlay && ctx.weather_type != WeatherType::None &&
        ctx.weather_intensity > 0.0f) {
        execute_weather_overlay(ctx, camera, projection);
    }
}

void SkyboxPass::execute_weather_overlay(const RenderContext& ctx, const Camera& camera) {
    if (ctx.weather_type == WeatherType::None || ctx.weather_intensity <= 0.0f) {
        return;
    }
    const glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                                  static_cast<float>(ctx.screen_width) /
                                                      static_cast<float>(ctx.screen_height),
                                                  camera.GetNearPlane(),
                                                  camera.GetFarPlane());
    execute_weather_overlay(ctx, camera, projection);
}

void SkyboxPass::execute_weather_overlay(const RenderContext& ctx,
                                         const Camera& camera,
                                         const glm::mat4& projection) {
    if (!m_weather_shader || !m_weather_shader->IsValid()) {
        return;
    }
    if (!ctx.lit_scene.id || !ctx.opaque_scene.id || !ctx.screen_quad_vao) {
        return;
    }

    // Snapshot the post-skybox scene into the opaque color texture (already
    // consumed by the water pass this frame, so it is free to reuse) so the
    // overlay can read the full scene while writing back into the lighting
    // FBO color attachment.
    //
    // this snapshot copy used to be done here via
    // pipeline.m_lighting_pass->copy_lighting_color_to_opaque_texture(pipeline).
    // A pass can no longer reach RenderPipeline, so the copy is RELOCATED to the
    // RenderPipeline call site, performed under the SAME guard (weather active +
    // valid overlay shader + complete lighting FBO + screen quad) immediately
    // before this overlay runs (identical sequence point). ctx.opaque_scene
    // therefore already holds the post-water snapshot when we read it below.
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.lit_scene.id);
    glDisable(GL_DEPTH_TEST);

    m_weather_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.opaque_scene.id);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_normal.id);
    m_weather_shader->setInt("u_sceneColor", 0);
    m_weather_shader->setInt("u_sceneDepth", 1);
    m_weather_shader->setInt("gPosition", 2);
    m_weather_shader->setInt("gNormal", 3);
    m_weather_shader->setFloat("u_time", ctx.time_seconds);
    m_weather_shader->setVec3("u_cameraPos", camera.Position);
    m_weather_shader->setMat4("u_inverseView", glm::inverse(camera.GetViewMatrix()));
    m_weather_shader->setMat4("u_inverseProjection", glm::inverse(projection));
    // Weather fog scattering follows the lighting-pass convention (the
    // light-travel direction), unlike the skybox disc uniforms above.
    m_weather_shader->setVec3("u_sunDirection", ctx.sun.direction);
    m_weather_shader->setVec3("u_sunColor", ctx.sun.color);
    m_weather_shader->setFloat("u_sunIntensity", ctx.sun.intensity);

    // the weather uniforms are SIM-DRIVEN when a replicated weather
    // state has been pushed (one-way, ): the WeatherSystem region category +
    // precip + nearest-storm + advected wind feed u_rainIntensity/u_snowIntensity/
    // u_fogDensity/u_stormIntensity/u_windDirection/u_windStrength directly. The
    // legacy set_weather DEBUG mapping (deriving the uniforms from a single
    // WeatherType+intensity) is the FALLBACK for non-driven callers. The wetness
    // material response (u_wetness) is: it shifts roughness/albedo by
    // local precip and never feeds back into the sim or world_hash.
    float rain = 0.0f;
    float snow = 0.0f;
    float fog = 0.0f;
    float storm = 0.0f;
    float wetness = 0.0f;
    glm::vec3 wind_dir(1.0f, 0.0f, 0.0f);
    float wind_strength = 0.0f;
    if (ctx.weather_state->driven) {
        const WeatherRenderState& w = *ctx.weather_state;
        rain = w.rain_intensity;
        snow = w.snow_intensity;
        fog = w.fog_density;
        storm = w.storm_intensity;
        wetness = w.wetness;
        wind_dir = w.wind_direction;
        wind_strength = w.wind_strength;
    } else {
        // Legacy DEBUG mapping. Rain carries a sub-lightning storm component
        // (overcast darkening without flashes) and light fog; Storm enables the
        // full storm path.
        const float intensity = ctx.weather_intensity;
        switch (ctx.weather_type) {
            case WeatherType::Rain:
                rain = intensity;
                storm = 0.25f * intensity;
                fog = 0.1f * intensity;
                break;
            case WeatherType::Snow:
                snow = intensity;
                fog = 0.05f * intensity;
                break;
            case WeatherType::Fog:
                fog = intensity;
                break;
            case WeatherType::Storm:
                rain = intensity;
                storm = intensity;
                break;
            case WeatherType::None:
            default:
                break;
        }
        wetness = rain;
        wind_strength = 0.3f * intensity;
    }
    m_weather_shader->setFloat("u_rainIntensity", rain);
    m_weather_shader->setFloat("u_snowIntensity", snow);
    m_weather_shader->setFloat("u_fogDensity", fog);
    m_weather_shader->setFloat("u_stormIntensity", storm);
    m_weather_shader->setFloat("u_wetness", wetness);
    m_weather_shader->setVec3("u_windDirection", wind_dir);
    m_weather_shader->setFloat("u_windStrength", wind_strength);

    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
}

} // namespace Luminumbra::Rendering
