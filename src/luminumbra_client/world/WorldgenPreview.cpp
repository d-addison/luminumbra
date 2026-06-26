#include "world/WorldgenPreview.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "core/Log.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "rendering/Camera.h"
#include "rendering/RenderPipeline.h"

namespace Luminumbra::Client {

namespace {
// The diorama center: a fixed world column the preview world is streamed around
// and the orbit camera looks at. Y picks the lit valley band (same vantage band
// the menu backdrop frames), so the slice reads as a model on a table.
constexpr float kCenterX = 8.0f;
constexpr float kCenterY = 40.0f;
constexpr float kCenterZ = 8.0f;
// Bounded streaming radius (chunks). Small enough to hold the create-screen
// frame budget at the preview FBO size (see the spike numbers in the handoff
// notes); large enough that the framed slice fills the diorama.
constexpr int kSurfaceRadius = 4;
constexpr int kCollisionRadius = 0; // no gameplay collision needed for a preview
}  // namespace

Luminumbra::Vec3 WorldgenPreview::look_at_center() {
    return Luminumbra::Vec3(kCenterX, kCenterY, kCenterZ);
}

WorldgenPreview::WorldgenPreview() {
    // Seed the pending params with engine defaults so an immediate render (before
    // any set_candidate) still shows a sane world rather than nothing.
    m_pending_params = Systems::TerrainGenParams();
    m_have_pending = true;
    m_pending_dirty = true;
}

WorldgenPreview::~WorldgenPreview() {
    if (m_color_texture) glDeleteTextures(1, &m_color_texture);
    if (m_depth_rbo) glDeleteRenderbuffers(1, &m_depth_rbo);
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    // Tear down in dependency order: water holds a SHIELD_WorldSystem*, the world holds
    // collision refs into physics. Drop water -> world -> (physics destructs last by member order).
    m_water.reset();
    m_world.reset();
}

void WorldgenPreview::ensure_target(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (m_fbo != 0 && width == m_fbo_w && height == m_fbo_h) {
        return;
    }
    m_fbo_w = width;
    m_fbo_h = height;

    if (m_color_texture == 0) glGenTextures(1, &m_color_texture);
    glBindTexture(GL_TEXTURE_2D, m_color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_fbo_w, m_fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (m_depth_rbo == 0) glGenRenderbuffers(1, &m_depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_fbo_w, m_fbo_h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    if (m_fbo == 0) glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color_texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depth_rbo);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LUMINUMBRA_CORE_ERROR("WorldgenPreview FBO incomplete (status 0x{:x})", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WorldgenPreview::set_candidate(const nlohmann::json& resolved_preset_json,
                                    const std::filesystem::path& data_root, int seed) {
    // Build candidate TerrainGenParams IN MEMORY via the additive loader seam,
    // with the correct data root (so biome/structure tables resolve right). On a
    // parse failure keep the previous pending params + record the error.
    world::TerrainPresetLoadResult result =
        world::LoadTerrainPresetFromJson(resolved_preset_json, data_root, "<preview-candidate>");
    if (!result.ok) {
        m_last_build_failed = true;
        m_last_error = result.errors.empty() ? "candidate preset parse failed"
                                             : result.errors.front();
        LUMINUMBRA_CORE_WARN("WorldgenPreview candidate parse failed: {}", m_last_error);
        return;
    }
    set_params(result.params, seed);
}

void WorldgenPreview::set_params(const Systems::TerrainGenParams& params, int seed) {
    m_pending_params = params;
    m_pending_seed = seed;
    m_have_pending = true;
    m_pending_dirty = true;
    // Debounced, latest-wins: every rapid change just re-arms the timer, so a
    // burst of slider changes collapses into a single rebuild when it settles.
    m_debounce_remaining = kDebounceSeconds;
}

void WorldgenPreview::set_weather(Weather w) { m_weather = w; }

void WorldgenPreview::set_time_of_day(float tod01) {
    m_tod = std::clamp(tod01, 0.0f, 1.0f);
}

void WorldgenPreview::orbit(float dyaw_deg, float dpitch_deg) {
    m_yaw += dyaw_deg;
    // Wrap yaw to keep it bounded.
    if (m_yaw >= 360.0f) m_yaw -= 360.0f;
    if (m_yaw < 0.0f) m_yaw += 360.0f;
    // Pitch: keep above the table (look down) and below straight-down so the
    // diorama always reads as a model, never an underside or a flythrough.
    m_pitch = std::clamp(m_pitch + dpitch_deg, 5.0f, 80.0f);
}

void WorldgenPreview::zoom(float dscroll) {
    // Scroll up (positive) zooms in (smaller distance).
    m_dist = std::clamp(m_dist - dscroll * 8.0f, 40.0f, 320.0f);
}

void WorldgenPreview::reset_view() {
    m_yaw = 45.0f;
    m_pitch = 28.0f;
    m_dist = 120.0f;
}

bool WorldgenPreview::tick(float dt) {
    if (!m_active) return false;
    if (!m_have_pending || !m_pending_dirty) {
        // No pending change; nothing to rebuild. (A first-use build is forced by
        // render() when no world exists yet.)
        return false;
    }
    if (m_debounce_remaining > 0.0f) {
        m_debounce_remaining -= dt;
        if (m_debounce_remaining > 0.0f) {
            return false; // still settling
        }
    }
    build_world_now();
    return true;
}

void WorldgenPreview::build_world_now() {
    // Bring up the physics system lazily on the first build (the synchronous
    // EnsureSurfaceReadyNear streaming path requires a non-null physics system;
    // collision_radius 0 keeps it to the single center chunk).
    if (!m_physics) {
        m_physics = std::make_unique<Systems::PhysicsSystem>();
        m_physics->startup();
    }

    // Reconstruct the preview world from the pending candidate params/seed. No
    // JobSystem -> EnsureSurfaceReadyNear builds + meshes its bounded chunk set
    // synchronously on this thread (the per-chunk jobs run inline).
    m_registry.clear();
    // Drop the old water system FIRST (it points at the world we're about to replace).
    m_water.reset();
    m_world = std::make_unique<Systems::SHIELD_WorldSystem>(
        /*job_system*/ nullptr, /*water_system*/ nullptr, m_pending_params, m_pending_seed);
    // Link a water system (no JobSystem needed — WaterSystem never dereferences it), exactly as
    // the game world does (GameSession). This is what makes a lake/ocean preset build its water
    // meshes and render water as water; without it the water render path crashes on a null system.
    m_water = std::make_unique<Systems::WaterSystem>(/*job_system*/ nullptr, m_world.get());
    m_world->SetWaterSystem(m_water.get());
    m_world->EnsureSurfaceReadyNear(look_at_center(), m_physics.get(), kSurfaceRadius, kCollisionRadius);
    // Pull the streamed chunks into the renderable set.
    m_world->update(m_registry, look_at_center(), m_physics.get());

    m_pending_dirty = false;
    m_built_once = true;
    m_last_build_failed = false;
    m_last_error.clear();
    ++m_rebuild_generation;
}

void WorldgenPreview::configure_camera(Rendering::Camera& cam) const {
    // Orbit/turntable: place the camera on a sphere around the fixed look-at,
    // then point it back at the center. yaw/pitch are the orbit angles; the
    // Camera's own Yaw/Pitch are set so its Front looks at the center.
    const float yaw_r = glm::radians(m_yaw);
    const float pitch_r = glm::radians(m_pitch);
    const glm::vec3 center(kCenterX, kCenterY, kCenterZ);
    glm::vec3 offset;
    offset.x = std::cos(pitch_r) * std::cos(yaw_r);
    offset.y = std::sin(pitch_r);
    offset.z = std::cos(pitch_r) * std::sin(yaw_r);
    cam.Position = center + offset * m_dist;
    // Camera Front must point from Position toward center: invert the offset.
    cam.Yaw = m_yaw + 180.0f;
    cam.Pitch = -m_pitch;
    cam.Zoom = 50.0f; // diorama FOV
    cam.updateCameraVectors();
}

void WorldgenPreview::apply_look(Rendering::RenderPipeline& pipeline) const {
    // Live weather / time-of-day through the SAME pipeline knobs the game uses.
    Rendering::WeatherType wt = Rendering::WeatherType::None;
    float intensity = 0.0f;
    switch (m_weather) {
        case Weather::Clear: wt = Rendering::WeatherType::None;  intensity = 0.0f; break;
        case Weather::Rain:  wt = Rendering::WeatherType::Rain;  intensity = 0.7f; break;
        case Weather::Snow:  wt = Rendering::WeatherType::Snow;  intensity = 0.7f; break;
        case Weather::Fog:   wt = Rendering::WeatherType::Fog;   intensity = 0.6f; break;
        case Weather::Storm: wt = Rendering::WeatherType::Storm; intensity = 0.9f; break;
    }
    pipeline.set_weather(wt, intensity);
    pipeline.set_time_of_day(m_tod);
    Rendering::CloudRenderState clouds;
    clouds.enabled = true;
    clouds.coverage_amount = (m_weather == Weather::Clear) ? 0.35f : 0.7f;
    clouds.plane_height = 900.0f;
    pipeline.set_cloud_state(clouds);
}

bool WorldgenPreview::render(Rendering::RenderPipeline& pipeline, float dt) {
    if (!m_active) return false;
    if (m_fbo == 0) {
        return false; // no target allocated yet
    }
    // Lazy first build so a screen that becomes active renders immediately.
    if (!m_built_once || m_world == nullptr) {
        build_world_now();
    }
    if (m_world == nullptr) {
        return false; // build failed; caller keeps the last good frame
    }

    Rendering::Camera cam(glm::vec3(kCenterX, kCenterY, kCenterZ + m_dist));
    configure_camera(cam);
    apply_look(pipeline);

    // Redirect the engine pipeline's final blit into the preview FBO and size the
    // internal passes to the preview dims for this frame, then restore. (Costly on
    // a shared pipeline — used only for one-shot headless capture/tests.)
    const u32 prev_w = pipeline.screen_width();
    const u32 prev_h = pipeline.screen_height();
    pipeline.on_resize(static_cast<u32>(m_fbo_w), static_cast<u32>(m_fbo_h));
    pipeline.set_offscreen_target(m_fbo, static_cast<u32>(m_fbo_w), static_cast<u32>(m_fbo_h));

    pipeline.render_frame(m_registry, *m_world, cam, dt, /*wireframe*/ false);

    pipeline.clear_offscreen_target();
    if (prev_w != 0 && prev_h != 0) {
        pipeline.on_resize(prev_w, prev_h);
    }
    return true;
}

bool WorldgenPreview::render_to_backbuffer(Rendering::RenderPipeline& pipeline, float dt) {
    if (!m_active) return false;
    // Lazy first build so a screen that becomes active renders immediately.
    if (!m_built_once || m_world == nullptr) {
        build_world_now();
    }
    if (m_world == nullptr) {
        return false; // build failed; caller keeps the last good frame
    }

    Rendering::Camera cam(glm::vec3(kCenterX, kCenterY, kCenterZ + m_dist));
    configure_camera(cam);
    apply_look(pipeline);

    // Draw straight to the backbuffer at the pipeline's CURRENT (full-screen) size
    // — no offscreen target, no on_resize. The create panel frames this as the
    // diorama "window"; the menu backdrop is suppressed by the host while active,
    // so this is the single world render on the create screen.
    pipeline.render_frame(m_registry, *m_world, cam, dt, /*wireframe*/ false);
    return true;
}

} // namespace Luminumbra::Client
