#pragma once

// the create-world LIVE WORLD-PREVIEW DIORAMA controller.
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
// Threading: the world BUILD (worldgen + meshing + WaterSystem init)
// is CPU-only (no GL) and runs on a background worker thread so a debounced knob
// change never stalls the create screen. The worker writes ONLY *pending* members
// (m_pending_world / m_pending_water / m_pending_registry); the render/main thread
// reads the *live* members and performs the pending->live swap + bumps
// m_rebuild_generation ON THE GL THREAD (so the  foliage rebuild + any GL
// upload stay main-thread). Same seed/params/sequence -> identical world; only
// WHEN the rebuild completes changes (render-only -> zero world_hash impact).

#include <glad/glad.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <entt/entt.hpp>

#include "luminumbra_common/systems/SHIELD_WorldSystem.h" // Systems::TerrainGenParams

// the FoliagePass ChunkScatter record is stored as a cached member so
// the preview scatters vegetation exactly like the real world path. The shared
// surface-query context/callback (ScenarioHarness::FoliageScatterContext /
// FoliageSurfaceQuery) are only used in the.cpp, so that header is included there.
#include "rendering/passes/FoliagePass.h" // Rendering::FoliagePass::ChunkScatter

namespace Luminumbra::Rendering {
class RenderPipeline;
class Camera;
class ParticlePass;
} // namespace Luminumbra::Rendering
namespace Luminumbra {
class JobSystem;
}
namespace Luminumbra::Systems {
class PhysicsSystem;
class WaterSystem;
} // namespace Luminumbra::Systems

namespace Luminumbra::Client {

class WorldgenPreview {
public:
    // weather selector parallel to RenderPipeline's WeatherType, kept local so
    // the UI/host can drive it without pulling the render header everywhere.
    enum class Weather {
        Clear = 0,
        Rain,
        Snow,
        Fog,
        Storm
    };

    WorldgenPreview();
    ~WorldgenPreview();

    WorldgenPreview(const WorldgenPreview&) = delete;
    WorldgenPreview& operator=(const WorldgenPreview&) = delete;

    // (Re)allocate the offscreen FBO at w x h. Idempotent if already this size.
    // Requires a current GL context. Safe to call repeatedly.
    void ensure_target(int width, int height);
    int target_width() const {
        return m_fbo_w;
    }
    int target_height() const {
        return m_fbo_h;
    }
    GLuint color_texture() const {
        return m_color_texture;
    }

    // Candidate params source. set_candidate stores the resolved preset JSON +
    // the data root and marks a pending rebuild (debounced in tick). Latest
    // call wins. data_root is the absolute path to data/ so biome/structure
    // tables resolve correctly (the in-memory seam, not a temp file).
    void set_candidate(const nlohmann::json& resolved_preset_json,
                       const std::filesystem::path& data_root,
                       int seed);
    // Directly set fixed params (tests / callers that already hold params). Marks
    // a pending rebuild, debounced like set_candidate.
    void set_params(const Systems::TerrainGenParams& params, int seed);

    // Live look controls (no rebuild — render-only).
    void set_weather(Weather w);
    Weather weather() const {
        return m_weather;
    }
    void set_time_of_day(float tod01); // 0=noon.. ~0.24 dusk (RenderPipeline scale)
    float time_of_day() const {
        return m_tod;
    }

    // Orbit / turntable camera.
    void orbit(float dyaw_deg, float dpitch_deg); // mouse drag delta
    void zoom(float dscroll);                     // scroll wheel delta
    void reset_view();
    float orbit_yaw() const {
        return m_yaw;
    }
    float orbit_pitch() const {
        return m_pitch;
    }
    float orbit_distance() const {
        return m_dist;
    }

    // Whether the preview screen is active. When inactive, tick/render are
    // cheap no-ops (paused) so the create screen costs nothing when hidden.
    void set_active(bool active) {
        m_active = active;
    }
    bool active() const {
        return m_active;
    }

    // drop any preview precipitation emitters from the shared
    // ParticlePass (called by the host when the create screen deactivates so the
    // preview's rain/snow never lingers into the menu backdrop or the game). A
    // cheap no-op when no precipitation is active. Render-only.
    void clear_precipitation(Rendering::RenderPipeline& pipeline);

    // Advance the debounce timer; when the debounce window elapses on the latest
    // pending candidate this SIGNALS the background worker to (re)build the world
    //. The actual pending->live swap happens later, on the GL thread, in
    // render/render_to_backbuffer. Returns true if a build was signalled this
    // tick. dt is seconds.
    bool tick(float dt);

    // Render the current candidate world into the offscreen FBO via the engine
    // pipeline. Builds the world lazily on first use. On a build failure keeps
    // the last good frame and sets last_build_failed. Returns true if a frame
    // was rendered. dt is seconds (drives atmosphere/weather animation).
    // NOTE: this resizes the shared deferred pipeline to the FBO size and back,
    // which is fine for a one-shot headless capture/test but too costly per-frame
    // on a shared pipeline — the LIVE create screen uses render_to_backbuffer.
    bool render(Rendering::RenderPipeline& pipeline, float dt);

    // Live create-screen render: draw the candidate world FULL-SCREEN to the
    // backbuffer through the engine pipeline at its CURRENT size (no offscreen
    // FBO, no per-frame pipeline resize — the "framed hole" the create panel
    // frames). This is the cheap, smooth path used by the running game; the host
    // suppresses the separate menu backdrop while it's active so only ONE world
    // renders. Builds the world lazily/debounced like render. Returns true if a
    // frame was drawn (false while a build is pending and no world exists yet).
    bool render_to_backbuffer(Rendering::RenderPipeline& pipeline, float dt);

    bool world_ready() const {
        return m_world != nullptr;
    }
    bool last_build_failed() const {
        return m_last_build_failed;
    }
    const std::string& last_error() const {
        return m_last_error;
    }
    // Generation counter: bumped once per ACTUAL world rebuild. Lets tests assert
    // latest-wins debouncing (N rapid set_params -> one rebuild -> +1 here).
    unsigned rebuild_generation() const {
        return m_rebuild_generation;
    }

    // The fixed world-space center the diorama orbits + is streamed around.
    static Luminumbra::Vec3 look_at_center();

private:
    // the background worker loop + the helpers it/the main thread use.
    void start_worker();        // spin up m_build_thread (once)
    void worker_loop();         // worker: drains m_build_pending
    void build_world_pending(); // CPU-only build into PENDING members (worker thread)
    void swap_pending_into_live(
        Rendering::RenderPipeline&
            pipeline); // GL thread: drain far-LOD off the OLD world, then pending->live + bump
                       // generation + foliage refresh marker
    void configure_camera(Rendering::Camera& cam) const;        // orbit -> Camera pose
    void apply_look(Rendering::RenderPipeline& pipeline) const; // weather/tod/clouds
    // spawn/maintain camera-followed PRECIPITATION particles in the
    // shared pipeline ParticlePass so the preview's rain/snow/storm read as REAL
    // falling particles (not just a sky/fog tint). Re-centres the emitter on the
    // orbit camera each frame and re-spawns only when the weather changes. The
    // particle MOTION is render-only — it never touches world_hash (one-way rule).
    void apply_precipitation(Rendering::RenderPipeline& pipeline, const Rendering::Camera& cam);

    // (re)build the deterministic foliage scatter for the live world's
    // bounded chunk set, but only once per actual world rebuild (cached by
    // m_rebuild_generation so it is a one-shot per world build, not per-frame).
    void rebuild_foliage_if_needed(Rendering::RenderPipeline& pipeline);

    // GL offscreen target.
    GLuint m_fbo = 0;
    GLuint m_color_texture = 0;
    GLuint m_depth_rbo = 0;
    int m_fbo_w = 0;
    int m_fbo_h = 0;

    // --- LIVE world (read by the render/main thread only). ---
    // Candidate world params (resolved) + the live built world.
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_world;
    // The water system linked to m_world. WITHOUT this, a lake/archipelago (water) preset
    // builds + renders with a null water system and the water render path crashes (0xC0000005)
    // the create-world "lake" crash. Linking it (like the game world) makes water meshes
    // generate and renders water as water (the header's stated intent). Declared AFTER m_world
    // so it destructs FIRST (it holds a SHIELD_WorldSystem*); also reset before each rebuild.
    std::unique_ptr<Systems::WaterSystem> m_water;
    // The synchronous EnsureSurfaceReadyNear streaming path requires a non-null
    // physics system; lazily created on the first build (collision_radius 0 so it
    // only ever touches the single center chunk). Shared by the worker (build) and
    // the main thread (update during swap); created on the main thread BEFORE the
    // worker is launched so the worker never races its creation.
    std::unique_ptr<Systems::PhysicsSystem> m_physics;
    entt::registry m_registry; // LIVE renderable registry for the preview world

    // The pipeline last rendered through. Far-LOD tile-build jobs (owned by the
    // pipeline) sample the LIVE preview world on worker threads; before this world
    // is FREED (swap adopts the next build, or the dtor at teardown) those jobs must
    // be drained or they read freed memory (the create-screen pan use-after-free).
    // The pipeline is constructed before this controller and outlives it, so this
    // raw pointer is valid in the dtor. Set on each render; null until first render.
    Rendering::RenderPipeline* m_drain_pipeline = nullptr;

    // --- PENDING world (written ONLY by the worker thread). The main/render
    // thread reads these solely inside swap_pending_into_live once m_build_done
    // is set, then moves them into the live members. Ordered AFTER the live
    // members so they tear down first; reset water-before-world to match the dtor
    // dependency order (water holds a SHIELD_WorldSystem*). ---
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_pending_world;
    std::unique_ptr<Systems::WaterSystem> m_pending_water;
    entt::registry m_pending_registry;

    // --- Debounce + the queued candidate (guarded by m_candidate_mutex; the
    // worker snapshots them under the lock at the start of each build). ---
    std::mutex m_candidate_mutex;
    Systems::TerrainGenParams m_pending_params; // latest requested params
    int m_pending_seed = 1337;
    bool m_have_pending = false;  // a candidate is queued
    bool m_pending_dirty = false; // pending differs from the built world
    float m_debounce_remaining = 0.0f;
    static constexpr float kDebounceSeconds = 0.25f;
    unsigned m_rebuild_generation = 0;
    bool m_built_once = false;

    // --- Worker thread + its signals. ---
    std::thread m_build_thread;
    std::mutex m_build_mutex;
    std::condition_variable m_build_cv;
    std::atomic<bool> m_build_pending{false}; // main->worker: build the queued candidate
    std::atomic<bool> m_build_done{false};    // worker->main: pending world is ready to swap
    std::atomic<bool> m_shutdown{false};      // dtor: stop the worker
    // A build has been requested and not yet adopted by a swap (covers the window
    // where the worker has consumed m_build_pending but not yet set m_build_done).
    // The lazy first-build kick checks this so it never double-submits.
    std::atomic<bool> m_build_inflight{false};
    bool m_first_build_requested = false; // the lazy first-build kick fired once (don't re-kick)

    // foliage scatter cache (render-only). The scatter is a deterministic
    // pure function of the world; rebuild it once per actual world rebuild.
    std::vector<Rendering::FoliagePass::ChunkScatter>
        m_foliage_scatter;                    // per-chunk inputs for the live world
    unsigned m_last_foliage_generation = ~0u; // m_rebuild_generation the scatter was built at
    bool m_foliage_loaded = false;            // scatter archetype set loaded once
    std::filesystem::path m_data_root;        // data/ root (for the scatter set json)

    // Look state.
    Weather m_weather = Weather::Clear;
    float m_tod = 0.24f; // golden dusk by default (matches the blessed vista)
    float m_yaw = 45.0f;
    float m_pitch = 28.0f; // look DOWN onto the diorama
    float m_dist = 120.0f;

    bool m_active = false;
    bool m_last_build_failed = false;
    std::string m_last_error;

    // precipitation emitter state (render-only). m_precip_emitter_id is
    // the live rain/snow emitter in the shared ParticlePass (kInvalidEmitter when
    // none); m_precip_spawned_for tracks which weather it was spawned for so a
    // weather change re-spawns the right emitter.
    std::uint32_t m_precip_emitter_id = 0xFFFFFFFFu; // ParticlePass::kInvalidEmitter
    Weather m_precip_spawned_for = Weather::Clear;
};

} // namespace Luminumbra::Client
