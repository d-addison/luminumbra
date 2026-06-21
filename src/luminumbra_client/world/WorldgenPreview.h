#pragma once

// Spec 002 Item 1 — the create-world LIVE WORLD-PREVIEW DIORAMA controller.
//
// Owns a bounded preview world (a real Systems::SHIELD_WorldSystem built from
// CANDIDATE params/seed, streamed around a fixed center at a small radius), an
// orbit/turntable Camera, an offscreen FBO, and the current weather/time-of-day.
// It renders the candidate world through the SAME engine pipeline the game uses
// (renderPipeline.render_frame -> the FBO via set_offscreen_target), so water
// reads as water, waterfalls show, biomes are colored by the real shaders, and
// the sky is the real atmosphere. The diorama updates live as the user slides a
// knob (rebuild debounced ~250 ms, latest-wins) or changes weather/tod, and is
// orbited by dragging the mouse over the preview rect (drag = spin, scroll =
// zoom, reset re-centers).
//
// Determinism: this NEVER reimplements worldgen sampling. It builds candidate
// TerrainGenParams via the in-memory loader seam (LoadTerrainPresetFromJson)
// with the correct data root, constructs a real world system, and renders it.
//
// Threading: SHIELD_WorldSystem here runs with NO JobSystem (synchronous, like
// WorldGenViewer + the menu backdrop's bounded build), so construct + stream +
// render all happen on the calling (GL/main) thread.

#include <glad/glad.h>

#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include <entt/entt.hpp>

#include "luminumbra_common/systems/SHIELD_WorldSystem.h" // Systems::TerrainGenParams

namespace Luminumbra::Rendering { class RenderPipeline; class Camera; }
namespace Luminumbra { class JobSystem; }
namespace Luminumbra::Systems { class PhysicsSystem; }

namespace Luminumbra::Client {

class WorldgenPreview {
public:
    // weather selector parallel to RenderPipeline's WeatherType, kept local so
    // the UI/host can drive it without pulling the render header everywhere.
    enum class Weather { Clear = 0, Rain, Snow, Fog, Storm };

    WorldgenPreview();
    ~WorldgenPreview();

    WorldgenPreview(const WorldgenPreview&) = delete;
    WorldgenPreview& operator=(const WorldgenPreview&) = delete;

    // (Re)allocate the offscreen FBO at w x h. Idempotent if already this size.
    // Requires a current GL context. Safe to call repeatedly.
    void ensure_target(int width, int height);
    int target_width() const { return m_fbo_w; }
    int target_height() const { return m_fbo_h; }
    GLuint color_texture() const { return m_color_texture; }

    // Candidate params source. set_candidate stores the resolved preset JSON +
    // the data root and marks a pending rebuild (debounced in tick()). Latest
    // call wins. data_root is the absolute path to data/ so biome/structure
    // tables resolve correctly (the in-memory seam, not a temp file).
    void set_candidate(const nlohmann::json& resolved_preset_json,
                       const std::filesystem::path& data_root, int seed);
    // Directly set fixed params (tests / callers that already hold params). Marks
    // a pending rebuild, debounced like set_candidate.
    void set_params(const Systems::TerrainGenParams& params, int seed);

    // Live look controls (no rebuild — render-only).
    void set_weather(Weather w);
    Weather weather() const { return m_weather; }
    void set_time_of_day(float tod01); // 0=noon .. ~0.24 dusk (RenderPipeline scale)
    float time_of_day() const { return m_tod; }

    // Orbit / turntable camera.
    void orbit(float dyaw_deg, float dpitch_deg); // mouse drag delta
    void zoom(float dscroll);                     // scroll wheel delta
    void reset_view();
    float orbit_yaw() const { return m_yaw; }
    float orbit_pitch() const { return m_pitch; }
    float orbit_distance() const { return m_dist; }

    // Whether the preview screen is active. When inactive, tick()/render() are
    // cheap no-ops (paused) so the create screen costs nothing when hidden.
    void set_active(bool active) { m_active = active; }
    bool active() const { return m_active; }

    // Advance the debounce timer; performs at most ONE world rebuild when the
    // debounce window elapses on the latest pending candidate. Returns true if a
    // rebuild happened this tick. dt is seconds.
    bool tick(float dt);

    // Render the current candidate world into the offscreen FBO via the engine
    // pipeline. Builds the world lazily on first use. On a build failure keeps
    // the last good frame and sets last_build_failed(). Returns true if a frame
    // was rendered. dt is seconds (drives atmosphere/weather animation).
    // NOTE: this resizes the shared deferred pipeline to the FBO size and back,
    // which is fine for a one-shot headless capture/test but too costly per-frame
    // on a shared pipeline — the LIVE create screen uses render_to_backbuffer().
    bool render(Rendering::RenderPipeline& pipeline, float dt);

    // Live create-screen render: draw the candidate world FULL-SCREEN to the
    // backbuffer through the engine pipeline at its CURRENT size (no offscreen
    // FBO, no per-frame pipeline resize — the "framed hole" the create panel
    // frames). This is the cheap, smooth path used by the running game; the host
    // suppresses the separate menu backdrop while it's active so only ONE world
    // renders. Builds the world lazily/debounced like render(). Returns true if a
    // frame was drawn (false while a build is pending and no world exists yet).
    bool render_to_backbuffer(Rendering::RenderPipeline& pipeline, float dt);

    bool world_ready() const { return m_world != nullptr; }
    bool last_build_failed() const { return m_last_build_failed; }
    const std::string& last_error() const { return m_last_error; }
    // Generation counter: bumped once per ACTUAL world rebuild. Lets tests assert
    // latest-wins debouncing (N rapid set_params -> one rebuild -> +1 here).
    unsigned rebuild_generation() const { return m_rebuild_generation; }

    // The fixed world-space center the diorama orbits + is streamed around.
    static Luminumbra::Vec3 look_at_center();

private:
    void build_world_now();                 // synchronous rebuild from pending params
    void configure_camera(Rendering::Camera& cam) const; // orbit -> Camera pose
    void apply_look(Rendering::RenderPipeline& pipeline) const; // weather/tod/clouds

    // GL offscreen target.
    GLuint m_fbo = 0;
    GLuint m_color_texture = 0;
    GLuint m_depth_rbo = 0;
    int m_fbo_w = 0;
    int m_fbo_h = 0;

    // Candidate world params (resolved) + the live built world.
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_world;
    // The synchronous EnsureSurfaceReadyNear streaming path requires a non-null
    // physics system; lazily created on the first build (collision_radius 0 so it
    // only ever touches the single center chunk).
    std::unique_ptr<Systems::PhysicsSystem> m_physics;
    entt::registry m_registry; // owned scratch registry for the preview world
    Systems::TerrainGenParams m_pending_params;
    int m_pending_seed = 1337;
    bool m_have_pending = false;     // a candidate is queued
    bool m_pending_dirty = false;    // pending differs from the built world
    float m_debounce_remaining = 0.0f;
    static constexpr float kDebounceSeconds = 0.25f;
    unsigned m_rebuild_generation = 0;
    bool m_built_once = false;

    // Look state.
    Weather m_weather = Weather::Clear;
    float m_tod = 0.24f; // golden dusk by default (matches the blessed vista)
    float m_yaw = 45.0f;
    float m_pitch = 28.0f;   // look DOWN onto the diorama
    float m_dist = 120.0f;

    bool m_active = false;
    bool m_last_build_failed = false;
    std::string m_last_error;
};

} // namespace Luminumbra::Client
