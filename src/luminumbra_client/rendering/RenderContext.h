#pragma once

#include "../../include/luminumbra/core/Types.h"
#include "RenderFrameTypes.h" // DirectionalLight/PointLight/WeatherType/Weather/Cloud/Lightning (by value/ptr)
#include "RenderResourceHandles.h"
#include "RenderResourceRegistry.h"

#include <cstddef>
#include <glm/glm.hpp>
#include <vector>

// Group L `isolation` points at the harness IsolationConfig (fwd-decl only).
namespace Luminumbra::Client::ScenarioHarness {
struct IsolationConfig;
}
namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}

// the per-frame render PASS CONTRACT.
//
// A pass receives a RenderContext& (frame/camera/time + screen/target geometry +
// typed resource handles via the registry) instead of `RenderPipeline&`. This is
// the engine-owned seam: passes read what they declare from the context and write
// to handles, with no access to RenderPipeline privates and no `friend`.
//
// It is also the seam 's RHI backend implements BEHIND: the handles here
// are backend-agnostic; today they wrap GL names, later a Diligent backend.
//
// Minimal during the FinalBlit pilot and grown additively as each
// pass converts; fields are added, never removed, so a converting pass only ever
// gains context it can read.

namespace Luminumbra::Rendering {

class Camera;

struct RenderContext {
    // Frame / camera / time.
    u64 frame_index = 0;
    const Camera* camera = nullptr;
    float delta_time = 0.0f;
    glm::mat4 view = glm::mat4(1.0f);
    glm::mat4 projection = glm::mat4(1.0f);

    // Backbuffer / screen geometry (== OUTPUT extent).
    u32 screen_width = 0;
    u32 screen_height = 0;

    //  render-scale seam: the internal (scaled) render extent the scaled scene
    // passes render at (== screen_* at render_scale 1.0). Passes that render into the
    // scaled G-buffer / lighting intermediates (aerial, god-rays) viewport at internal_*;
    // the taau resolve reads internal (u_texel = 1/internal) but writes output. Falls back
    // to screen_* when a builder leaves it 0 (an output-res pass).
    u32 internal_width = 0;
    u32 internal_height = 0;
    u32 internal_w() const {
        return internal_width != 0 ? internal_width : screen_width;
    }
    u32 internal_h() const {
        return internal_height != 0 ? internal_height : screen_height;
    }

    // Offscreen-target state (  preview path). When active the
    // final image is written to offscreen_fbo at offscreen_w/h instead of the
    // default framebuffer.
    bool offscreen_active = false;
    u32 offscreen_fbo = 0;
    u32 offscreen_w = 0;
    u32 offscreen_h = 0;

    // Typed resource access.
    RenderResourceRegistry* registry = nullptr;

    // The lit-scene color source produced upstream (adopted into the registry as
    // "lit_scene"). The FinalBlit pass reads this and resolves it to the target.
    FboHandle lit_scene{};

    // ---  field-union (grown additively per converted pass) ---
    // Group A: shared fullscreen-quad VAO (plain GL name, reused by every
    // fullscreen pass: Ssao, Aerial, GodRays, Taau, Skybox-overlay, Lighting, Water).
    u32 screen_quad_vao = 0;
    // Group B: G-buffer attachment handles (adopted by name from the live G-buffer).
    TextureHandle gbuffer_position{};
    TextureHandle gbuffer_normal{};
    // SSAO quality selector (0 legacy, 1/2 GTAO, 3 half-res GTAO) <- m_ssao_quality.
    int ssao_quality = 0;

    // Group B (remaining) — G-buffer attachment handles (adopt_texture/adopt_fbo by name).
    TextureHandle gbuffer_albedo{};   // <- gbuffer().albedo_texture
    TextureHandle gbuffer_material{}; // <- gbuffer().material_texture
    TextureHandle gbuffer_depth{};    // <- gbuffer().depth_texture
    TextureHandle motion_vectors{};   // <- gbuffer().motion_vector_texture
    FboHandle gbuffer_fbo{};          // <- gbuffer().fbo_id

    // Group C (remaining) — lighting/scene targets (lit_scene already present).
    TextureHandle lit_scene_color{};      // <- lighting_fbo().color_texture
    TextureHandle opaque_scene{};         // <- lighting_fbo().opaque_color_texture
    RenderbufferHandle lit_scene_depth{}; // <- lighting_fbo().depth_texture

    // Group D — sky/atmosphere LUTs.
    TextureHandle sky_view_lut{};      // <- m_sky_lut.sky_view_texture()
    TextureHandle transmittance_lut{}; // <- m_sky_lut.transmittance_texture()
    bool sky_lut_ready = false;        // <- m_sky_lut.ready()

    // Group E (remaining) — shadow / SSAO / caustics reads.
    TextureHandle shadow_depth_array{}; // <- m_shadow_pass->shadow_map().depth_texture_array
    // the tinted-transmission cascade (white = identity).
    TextureHandle shadow_tint_array{}; // <- m_shadow_pass->tint_texture_array()
    TextureHandle ssao_blur{};         // <- m_ssao_pass->ssao().ssaoColorBufferBlur
    TextureHandle caustics_tex{};      // <- m_water_pass->black_texture()

    // Group F — terrain/material arrays (GBuffer + Lighting).
    TextureHandle material_lut{};      // <- m_materialLUT
    TextureHandle terrain_textures{};  // <- m_terrainTextureArray
    TextureHandle terrain_normals{};   // <- m_terrainNormalArray
    TextureHandle terrain_roughness{}; // <- m_terrainRoughnessArray
    int terrain_roughness_valid = 0;   // <- m_terrainRoughnessValid
    TextureHandle skinned_textures{};  // <- m_skinnedTextureArray
    int skinned_albedo_layer = 0;      // <- m_skinnedAlbedoLayer
    int skinned_normal_layer = 0;      // <- m_skinnedNormalLayer

    // Group G — aether field (Lighting).
    TextureHandle aether_field{}; // <- m_aetherFieldTexture
    bool aether_active = false;   // <- m_aetherFieldActive
    //  ( -8): a dual RG32F upload is live — the glow
    // color mixes toward the polarity poles. false = untouched glow color
    // (pixel-identical), including for every single-channel upload.
    bool aether_polarity_active = false;             // <- m_aetherPolarityActive
    int aether_extent = 0;                           // <- m_aetherFieldExtent
    float aether_cell_size = 0.0f;                   // <- m_aetherFieldCellSize
    glm::vec2 aether_world_origin = glm::vec2(0.0f); // <- m_aetherFieldWorldOrigin
    // the glow grade. Defaults mirror the
    // lighting_pass.frag GLSL initializers EXACTLY, so an untouched context is
    // pixel-identical; a game system (or the  panel) can now grade the glow.
    glm::vec3 aether_glow_color{0.30f, 0.55f, 0.95f};
    float aether_glow_intensity = 2.0f;
    //  ( -6): emissive materials scale by
    // (1 + aether * modulation). 0.0 = multiply by exactly 1.0 = pixel-identical.
    float aether_material_modulation = 0.0f;
    // render-only snow ground cover [0,1]; 0 = untouched.
    float snow_cover = 0.0f;

    // Group H — light/atmosphere scalars & vectors.
    DirectionalLight sun{};                        // <- m_sun (by value)
    glm::vec3 sky_ambient_color = glm::vec3(0.0f); // <- m_skyAmbientColor
    glm::vec3 moon_light_dir = glm::vec3(0.0f);    // <- m_moonLightDir
    glm::vec3 moon_direction = glm::vec3(0.0f);    // <- m_moonDirection (distinct; keep both)
    float moon_illumination =
        1.0f; // <- m_moonIllumination (rendering: lunar phase / "two night modes")
    glm::vec3 moon_radiance =
        glm::vec3(0.40f, 0.52f, 0.92f); // <- m_moonRadiance (rendering : the moon's
                                        // dedicated cool key colour; default == prior shader const)
    float sky_day_factor = 0.0f;        // <- m_skyDayFactor
    float underwater_factor = 0.0f;     // <- m_underwater_factor
    float emissive_lut_scale = 0.0f;    // <- kEmissiveLutScale
    glm::vec4 cascade_splits =
        glm::vec4(0.0f); // <- shadow_map.cascade_splits[1..4] (resolved at call site)
    const std::vector<glm::mat4>* light_space_matrices =
        nullptr;                                           // <- &shadow_map.light_space_matrices
    const std::vector<PointLight>* point_lights = nullptr; // <- &m_point_lights_this_frame
    CloudRenderState cloud_state{};                        // <- m_cloud_state (by value)
    const WeatherRenderState* weather_state = nullptr;     // <- &m_weather_state
    WeatherType weather_type = WeatherType::None;          // <- m_weather_type
    float weather_intensity = 0.0f;                        // <- m_weather_intensity
    const LightningRenderState* lightning_state = nullptr; // <- &m_lightning_state

    // Group I — Aerial atmosphere floats (resolved at call site AFTER LUMIN_ATMOS override).
    float aerial_density = 0.0f;      // <- atmo.aerial_density
    float aerial_max_distance = 0.0f; // <- atmo.aerial_max_distance
    float inscatter_strength = 0.0f;  // <- atmo.inscatter_strength
    float atmosphere_warmth = 0.0f;   // <- atmo.warmth

    // Group J — GodRays sun-screen (computed at call site from m_sun.direction + view/proj).
    float sun_visible = 0.0f;
    float sun_uv_x = 0.0f;
    float sun_uv_y = 0.0f;

    // Group K — frame time (single glfwGetTime snapshot per frame; parity-critical).
    float time_seconds = 0.0f; // <- glfwGetTime() snapshot (mirrors m_wall_clock_time)

    // Group L — GBuffer frame state.
    const glm::vec4* frustum_planes = nullptr;   // <- frustum_planes[6] built in render_frame
    glm::vec2 taau_jitter_ndc = glm::vec2(0.0f); // <- taau_jitter_ndc()
    glm::mat4 prev_view_proj = glm::mat4(1.0f);  // <- prev_view_proj()
    float prev_time = 0.0f;                      // <- prev_time()
    const Client::ScenarioHarness::IsolationConfig* isolation = nullptr; // <- &m_isolation_config

    // Group M — stat out-pointers (passes that mutate stats in-place).
    std::size_t* lighting_draws = nullptr; // <- &m_last_render_pass_stats.lighting_draws
    std::size_t* skybox_draw_counter =
        nullptr; // <- &m_last_render_pass_stats.skybox_draws (size_t)

    // Group N —  rendering: exposure seam slot. Sentinel 0 = "unset" → the
    // lighting pass falls back to its static LUMIN_GRADE exposure (byte-identical today).
    //  populates this with the deterministic time-of-day exposure curve;  with the
    //  metered value; photo-mode manual EV overrides. Render-only — it feeds
    // only u_exposure, never world_hash.
    float exposure = 0.0f;

    // Group O — froxel volumetric quality (0 = both compute stages are no-ops).
    int volumetric_quality = 0; // <- m_volumetric_quality

    // Group P — waterfall live-water mirror. The pass reads this one-way world
    // view to extinguish a dammed/drained fall; it never mutates simulation state.
    const Systems::SHIELD_WorldSystem* world_system = nullptr;

    // Destination resolution helpers (screen vs offscreen preview target).
    FboHandle dest_fbo() const {
        return offscreen_active ? adopt_fbo(offscreen_fbo) : default_framebuffer();
    }
    u32 dest_width() const {
        return offscreen_active ? offscreen_w : screen_width;
    }
    u32 dest_height() const {
        return offscreen_active ? offscreen_h : screen_height;
    }
};

} // namespace Luminumbra::Rendering
