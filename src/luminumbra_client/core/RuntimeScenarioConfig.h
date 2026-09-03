#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "core/CaptureScale.h"
#include "nlohmann/json.hpp"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Luminumbra::Client::ScenarioHarness {

bool HasCommandLineFlag(int argc, char* argv[], const std::string& flag);
std::string
GetCommandLineOption(int argc, char* argv[], const std::string& flag, const std::string& fallback);
int GetCommandLineIntOption(int argc, char* argv[], const std::string& flag, int fallback);
uint64_t
GetCommandLineUInt64Option(int argc, char* argv[], const std::string& flag, uint64_t fallback);

// --- Window modes ---
// The client renders into one of these top-level window arrangements. Headless
// is a first-class mode (no GL context window is shown) that gates/servers use.
// borderless is the interactive default (monitor work-area fullscreen window).
enum class WindowMode {
    Windowed = 0, // resizable, decorated window at the requested resolution
    Borderless,   // borderless window covering the monitor work-area (default)
    Fullscreen,   // exclusive fullscreen at the monitor's native video mode
    Headless,     // hidden window / offscreen (gates + server effectively use)
};

// CLI string -> WindowMode. Unknown/empty strings return the supplied fallback.
WindowMode ParseWindowMode(const std::string& value, WindowMode fallback);
const char* WindowModeName(WindowMode mode);

// --- Capture-pin contract (, gate protection) ---
// Every pixel-ROI gate depends on captures running at exactly this size. When a
// scenario/capture run is active the harness PINS the framebuffer to this size
// regardless of --window-mode / --resolution, and records the active window
// mode + framebuffer size into each capture analysis artifact so the offline
// analysis can hard-fail if a capture ever ran at a non-pinned size.
inline constexpr int kCapturePinnedWidth = 3840;
inline constexpr int kCapturePinnedHeight = 1600;
// Resolution-relative gate-threshold scaling (kThresholdTuning* + ScalePinned*).
// Dependency-free header so the scaling math is unit-testable without GL.
// (Included here so every gate in RuntimeScenarioHarness.cpp sees the helpers.)

struct RuntimeScenarioConfig {
    std::string scenario;
    bool auto_create_world = false;
    bool auto_enter_world = false;
    bool no_audio = false;
    bool no_ui = false;
    bool hidden_window = false;
    //  isolation/layer render mode (--isolation-layers <csv>, --isolation-backdrop
    // <void|greenscreen|checker>). Empty/"scene" = no isolation (byte-stable). Parsed
    // to an IsolationConfig (core/IsolationConfig.h) where consumed.
    std::string isolation_layers;
    std::string isolation_backdrop;
    //  #1b-lush: render-only foliage density multiplier for showcase/photo scenes
    // (--foliage-density-scale, default 1.0 = the biome-tracked default, gates untouched).
    float foliage_density_scale = 1.0f;
    // Number of avatars to spawn in skinned_mesh_visual_smoke as a row of
    // player stand-ins (--avatars; 0/1 = the unchanged single-rig gate behavior,
    // >=2 = a multiplayer SHOWCASE row with a widened camera). Visualization only.
    int avatars = 0;
    //   integration: drive the showcase row's render avatars through the
    // in-process replication pipeline (server->snapshot->client->interpolate) instead
    // of a scripted transform walk, so the on-screen view is literally network-driven.
    bool replicated = false;
    //  cinematic: a scripted wildlife scene -- an animal wanders to water, a human
    // shoots an arrow beside it (Jolt projectile), the splash scares the animal and it
    // flees. Reuses the skinned_mesh_visual_smoke two-creature spawn (animal + human).
    bool wildlife = false;
    int timed_run_seconds = 0;
    int readiness_timeout_seconds = 120;
    int horizon_radius = 12;
    int collision_radius = 4;
    int coverage_radius = 3;
    size_t min_renderable_chunks = 64;
    size_t min_collision_chunks = 9;
    uint64_t memory_watermark_mb = 0;
    std::filesystem::path artifact_dir;
    std::filesystem::path audio_telemetry_path;
    std::filesystem::path crash_dir;
    // Persistence runtime roundtrip: which half of the roundtrip
    // this process runs ("save" or "load") and the shared session directory
    // the world snapshot travels through.
    std::string persistence_phase;
    std::filesystem::path persistence_session_dir;
    // player_view_smoke: world preset to create the automated test
    // world from (--world-preset; empty falls back to "mountains", the
    // worst-case preset for surface-span coverage).
    std::string world_preset;
    // creature_slice_smoke: root-relative path of the game
    // archetype JSON to spawn (--creature-archetype). The engine harness
    // carries no game nouns; the validator supplies the content path.
    std::string creature_archetype;
    // Skinned-mesh UV texture set: root-relative.ltex
    // paths handed to RenderPipeline::load_skinned_texture_set. Data-driven:
    // for the creature slice these are read from the creature archetype JSON;
    // for the noun-free skinned-mesh visual they default to a generic test
    // texture under data/textures/test/. Empty = keep the flat fallback.
    std::string skinned_albedo_texture;
    std::string skinned_normal_texture;

    // --- Window modes ---
    // Requested top-level window arrangement (--window-mode) and windowed-mode
    // resolution (--resolution WxH). These describe the INTERACTIVE window only;
    // when a scenario/capture run is active the framebuffer is pinned to
    // kCapturePinnedWidth x kCapturePinnedHeight regardless of these values.
    WindowMode window_mode = WindowMode::Borderless;
    int windowed_width = kCapturePinnedWidth;
    int windowed_height = kCapturePinnedHeight;

    bool active() const {
        return !scenario.empty();
    }
    bool auto_world_smoke() const {
        return scenario == "auto_world_smoke";
    }
    bool lod_ground_smoke() const {
        return scenario == "lod_ground_smoke";
    }
    bool water_visual_smoke() const {
        return scenario == "water_visual_smoke";
    }
    bool material_visual_smoke() const {
        return scenario == "material_visual_smoke";
    }
    bool skybox_visual_smoke() const {
        return scenario == "skybox_visual_smoke";
    }
    bool weather_visual_smoke() const {
        return scenario == "weather_visual_smoke";
    }
    //  spawns the fixture particle emitter, snapshots the
    // sim-deterministic emitter descriptor set, and captures a particle render.
    bool particle_emitter_determinism_smoke() const {
        return scenario == "particle_emitter_determinism_smoke";
    }
    // partly-cloudy fixture; asserts the moving cast-shadow signature
    // on terrain (ROI luminance delta as a cloud-shadow edge drifts) + cloud layer
    // present in the sky.
    bool cloud_shadow_smoke() const {
        return scenario == "cloud_shadow_smoke";
    }
    // instanced foliage scatter. Loads the scatter set, builds the
    // deterministic per-chunk scatter over the visible live ring, samples the
    // wind field at the camera (calm vs windy phases), captures a frame and
    // snapshots the instance set so the FoliageInstancing gate can assert coverage
    // density vs the biome table, distance-fade (no foliage beyond the live ring),
    // wind-sway response (calm vs windy displacement differs), and the FoliagePass
    // GPU-timer budget.  (one-way, never writes world_hash).
    bool foliage_visual_smoke() const {
        return scenario == "foliage_visual_smoke";
    }
    // rain precipitation through the  particle framework, driven
    // by the replicated weather state and wind-advected (slant) from the  wind
    // field. Captures a calm vs a windy rain frame so the gate can assert precip
    // particles present AND that they slant with wind.
    bool precipitation_smoke() const {
        return scenario == "precipitation_smoke";
    }
    bool timeofday_sweep_smoke() const {
        return scenario == "timeofday_sweep_smoke";
    }
    bool lod_boundary_oscillation_smoke() const {
        return scenario == "lod_boundary_oscillation_smoke";
    }
    bool lod_seam_arrival_smoke() const {
        return scenario == "lod_seam_arrival_smoke";
    }
    bool persistence_roundtrip_smoke() const {
        return scenario == "persistence_roundtrip_smoke";
    }
    bool player_view_smoke() const {
        return scenario == "player_view_smoke";
    }
    bool farlod_horizon_smoke() const {
        return scenario == "farlod_horizon_smoke";
    }
    bool skinned_mesh_visual_smoke() const {
        return scenario == "skinned_mesh_visual_smoke";
    }
    bool creature_slice_smoke() const {
        return scenario == "creature_slice_smoke";
    }
    bool window_mode_stress_smoke() const {
        return scenario == "window_mode_stress_smoke";
    }
    // client renders a server-owned world over the lockstep transport.
    bool networked_session_smoke() const {
        return scenario == "networked_session_smoke";
    }
    //  deterministic multi-angle/time/weather/season world
    // capture matrix for orchestrator visual review.  (drives the
    // existing one-way atmospheric/foliage bridges; never writes world_hash). The
    // scenario is selected by --scenario=world_visual_sweep OR the
    // LUMINUMBRA_VISUAL_SWEEP=1 env flag (the latter mapped in
    // ParseRuntimeScenarioConfig so existing auto/server launches can opt in).
    bool world_visual_sweep() const {
        return scenario == "world_visual_sweep";
    }
    bool forced_crash() const {
        return scenario == "forced_crash";
    }

    // True for any scenario that captures pixel-ROI screenshots and therefore
    // requires the framebuffer pinned to kCapturePinnedWidth/Height. The
    // window-mode stress run toggles modes mid-run but still PINS the
    // framebuffer for its capture phase, so it is included here.
    bool requires_pinned_capture() const {
        return active();
    }
};

RuntimeScenarioConfig
ParseRuntimeScenarioConfig(int argc, char* argv[], const std::filesystem::path& root_dir);

std::string TimestampUtc();
std::string TimestampForFile();

// Capture-pin metadata block embedded in every capture analysis artifact
//. The offline gate asserts pinned == true and that
// {capture_width, capture_height} == {kCapturePinnedWidth, kCapturePinnedHeight}.
nlohmann::json
CapturePinMetadata(WindowMode active_window_mode, int capture_width, int capture_height);

nlohmann::json Vec3ToJson(const Luminumbra::Vec3& value);
nlohmann::json IVec3ToJson(const Luminumbra::IVec3& value);

// --- GL debug-output runtime counters ---
// Cumulative KHR_debug message counters since boot. Incremented by the app's
// GL debug callback (app/InputCallbacks.cpp), read by the runtime-state
// recorder's health snapshots and by every QA analysis writer. Client-lib
// telemetry (not harness code): the shipping client records these too.
extern std::atomic<uint64_t> g_gl_debug_message_count;
extern std::atomic<uint64_t> g_gl_debug_error_count;
extern std::atomic<uint64_t> g_gl_debug_warning_count;
extern std::atomic<uint64_t> g_gl_debug_notification_count;

struct GLDebugRuntimeStats {
    uint64_t messages = 0;
    uint64_t errors = 0;
    uint64_t warnings = 0;
    uint64_t notifications = 0;
};

GLDebugRuntimeStats CurrentGLDebugRuntimeStats();

} // namespace Luminumbra::Client::ScenarioHarness
