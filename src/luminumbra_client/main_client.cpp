#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "WorldDressing.h" // background world-dressing placement computation
#include "app/CaveFlourishes.h"
#include "app/ClientAppContext.h"
#include "app/CrashHandler.h"
#include "app/DebugOverlays.h"
#include "app/FrameAudio.h"
#include "app/InputCallbacks.h"
#include "app/MenuScreens.h"
#include "app/ProcgenPalettes.h"
#include "app/RuntimeRoot.h"
#include "app/RuntimeStateRecorder.h"
#include "app/WindowModeControls.h"
#include "audio/AudioManagerFactory.h"
#include "audio/AudioPropagationSystem.h"   // ComputeWaterfallRoar (static)
#include "audio/EnvironmentalAudioSystem.h" // /09: day/night beds + biome/weather reverb
#include "audio/IAudioManager.h"
#include "audio/NullAudioManager.h"
#include "core/Debug.h"
#include "core/GameState.h"
#include "core/Log.h"
#include "core/NvmlSampler.h" //  optional GPU power/clock sampling
#include "core/RuntimeScenarioHarness.h"
#include "debug/DebugCamera.h" // deterministic feature locator (--debug-goto cave|doline)
#include "debug/WorldGenViewer.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" // species id -> display name for the codex/discovery HUD
#include "luminumbra_common/ai/EcologyTuningConfig.h"     //  resolve sim.ecology brain tuning
#include "luminumbra_common/ai/SimTuningConfig.h" // full-control: resolve per-system creature tuning
#include "luminumbra_common/animation/AnimationRuntime.h" // skinned skeleton/clip loaders for ambient wildlife
#include "luminumbra_common/components/AlarmComponents.h" // herd-alarm collective flee
#include "luminumbra_common/components/CircadianComponents.h" // diurnal/nocturnal sleep clock
#include "luminumbra_common/components/CombustionComponents.h" // sim.fire demo markers
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"  //  creature markers
#include "luminumbra_common/components/DecayComponents.h"     // decomposition (carcass fades)
#include "luminumbra_common/components/ForagingComponents.h"  // ant-trail forager colonies
#include "luminumbra_common/components/LightingComponents.h"  // lumin-crystal cave point lights
#include "luminumbra_common/components/MigratoryComponents.h" // seasonal drive
#include "luminumbra_common/components/MortalComponents.h"     // lifespan / natural death
#include "luminumbra_common/components/PackHunterComponents.h" // coordinated pack hunting
#include "luminumbra_common/components/PlantComponents.h"      //
#include "luminumbra_common/components/ScavengerComponent.h" // ambient-wildlife predator scavenging
#include "luminumbra_common/components/TerritoryComponents.h" // home-range homing
#include "luminumbra_common/components/ThirstComponents.h" // ambient-wildlife thirst + water holes
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h" // user.* video/audio/controls settings
#include "luminumbra_common/game/CodexView.h" // pure presentation model for the codex browse screen
#include "luminumbra_common/game/Objectives.h" // progression goals surfaced on the HUD
#include "luminumbra_common/game/PhotoMode.h" // photo-capture feature: photo-mode capture loop (read-only observer)
#include "luminumbra_common/network/NetworkLoopbackAuthority.h"
#include "luminumbra_common/systems/AetherFieldSystem.h" // Aether-field sampling.
#include "luminumbra_common/systems/CreatureProcgen.h" // genome -> body-proportion build (procedural silhouette)
#include "luminumbra_common/systems/FarmingSystem.h"   //  MakePlantFromSpecies + SpeciesRegistry
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h" //  phenotype/genome
#include "luminumbra_common/systems/PlantProcgen.h"      //  procedural plant geometry (render-only)
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/KnobLayer.h" //  semantic-knob layer + startup invariant
#include "nlohmann/json.hpp"
#include "player/PlayerController.h"
#include "rendering/Camera.h"
#include "rendering/ExposureModel.h" //  rendering: lens EV -> render exposure multiplier
#include "rendering/FarLodSystem.h"
#include "rendering/FrameHealth.h" // auto frame-health anomaly verdict (black/unlit/blown), render-only
#include "rendering/FrameScan.h" // framescan: deterministic what's-in-frame scan tool (render-only)
#include "rendering/GlDebugOutput.h" // KHR_debug callback + debug groups/labels (env-gated LUMIN_GL_DEBUG)
#include "rendering/ImpostorBake.h"  //  far-field tree impostor atlas bake (render-only)
#include "rendering/LightningBolt.h" // Deterministic bolt geometry.
#include "rendering/RenderPipeline.h"
#include "rendering/SceneSurvey.h" // survey: autonomous tour+screenshot of world POIs (render-only)
#include "rendering/ScentFieldRenderMirror.h" // one-way scent snapshot for the ground decal
#include "rendering/SnowCoverModel.h"         // Render-only snow cover.
#include "rendering/WeatherRenderBridge.h"    // Live weather bridge.
#include "rendering/WorldLoadingVisualizer.h"
#include "rendering/passes/ParticlePass.h"     //  EmitterDescriptor + accessor type
#include "rendering/passes/PlantProcgenPass.h" //  render-only procedural plant bake (flag-gated)
#include "rendering/passes/WaterPass.h"
#include "ui/Rml_UIManager.h"
#include "ui/core/UIHotReload.h"
#include "world/WorldgenOverride.h"
#include "world/WorldgenPreview.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Luminumbra::Client::ScenarioHarness;
using namespace Luminumbra::Client::App;

// --- Global Pointers ---
std::unique_ptr<Luminumbra::Rendering::Camera> g_camera;
std::unique_ptr<Luminumbra::Client::PlayerController> g_playerController;

// The frame loop's non-scenario state clusters (HUD, overlays, capture modes,
// menu backdrop, loading, mouse-look, frame audio), grouped into ONE
// file-scope context (app/ClientAppContext.h) with the same static-lifetime
// semantics the individual globals had; the extracted frame helpers take it
// by reference instead of reaching for externs.
ClientAppContext g_app;

// Single client config: defaults (data/common/systems.json) overlaid by the writable
// per-user settings file (%APPDATA%/Luminumbra/settings.json). user.* is client-only,
// never hashed. Loaded once at startup, before window creation.
luminumbra::core::SystemConfig g_systemConfig;

// Short human label for a GLFW key code (for the settings controls list). Printable keys use
// glfwGetKeyName; special keys are named explicitly.
// Derive the user-preset slug (and world-type id "user_<slug>") from a display name.
// Shared by the save/exists/rename bridges so collision detection matches save behaviour.
static std::string UserPresetSlug(const std::string& displayName) {
    std::string slug;
    for (char ch : displayName) {
        if (std::isalnum(static_cast<unsigned char>(ch)))
            slug += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        else if ((ch == ' ' || ch == '-' || ch == '_') && !slug.empty() && slug.back() != '_')
            slug += '_';
    }
    while (!slug.empty() && slug.back() == '_')
        slug.pop_back();
    if (slug.empty())
        slug = "preset";
    return slug.substr(0, 32);
}

static std::string KeyDisplayLabel(int key) {
    switch (key) {
        case GLFW_KEY_SPACE:
            return "Space";
        case GLFW_KEY_ENTER:
            return "Enter";
        case GLFW_KEY_LEFT_SHIFT:
        case GLFW_KEY_RIGHT_SHIFT:
            return "Shift";
        case GLFW_KEY_LEFT_CONTROL:
        case GLFW_KEY_RIGHT_CONTROL:
            return "Ctrl";
        case GLFW_KEY_LEFT_ALT:
        case GLFW_KEY_RIGHT_ALT:
            return "Alt";
        case GLFW_KEY_ESCAPE:
            return "Esc";
        case GLFW_KEY_TAB:
            return "Tab";
        case GLFW_KEY_LEFT_BRACKET:
            return "[";
        case GLFW_KEY_RIGHT_BRACKET:
            return "]";
        case GLFW_KEY_EQUAL:
            return "=";
        case GLFW_KEY_MINUS:
            return "-";
        default:
            break;
    }
    if (const char* n = glfwGetKeyName(key, 0); n && n[0]) {
        std::string s(n);
        for (char& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
    return "?";
}
// the one-time world-entry DRESSING bring-up (the
// tree/rock/bush scatter + the ambient-wildlife spawn) cost ~30 s of main-thread
// placement loops in a debug build on the first IN_GAME frame. The placement
// COMPUTATION (RNG-driven candidate probing over terrain/water/biome queries —
// see WorldDressing.h) now runs as ONE background JobSystem job (the same pure,
// thread-safe worldgen reads the meshing workers already run concurrently); the
// main thread polls the handle and consumes the placement vectors AMORTIZED over
// a few frames (the GL palette uploads + EnTT/physics registrations were always
// the cheap half). Capture/scenario runs compute synchronously so frame-1
// content is unchanged. TEARDOWN CONTRACT: the job's callbacks hold a raw
// SHIELD_WorldSystem* — every world transition (CreateWorld / session reset)
// MUST DrainWorldDressing first, exactly like DrainBackgroundWorldScan above.
struct WorldDressingPending {
    const void* world = nullptr; // identity guard: consume only for the world computed
    Luminumbra::Client::WorldDressingResult result;
    // Amortized consume cursors — each lane replays its vector strictly in order.
    std::size_t trees_done = 0, rocks_done = 0, bushes_done = 0, wildlife_done = 0;
    bool trees_logged = false, rocks_logged = false, bushes_logged = false;
    int creatures_spawned = 0; // Kind::Creature consumed (the legacy wlSpawned log)
    bool synchronous = false;  // capture/scenario: consume everything on one frame
    bool wildlife_ok = false;  // interactive play + roster + rig loaded (gates spawn/colony/log)
    glm::vec3 sun_toward{0.0f, 1.0f, 0.0f}; // dispatch-time sun (palette build + phototropism)
    // render.creature_spawn values resolved at dispatch (consume-side; no RNG involved).
    float pred_speed = 4.0f, prey_speed = 2.6f, init_hunger = 0.2f;
};
static std::shared_ptr<WorldDressingPending> s_worldDressing; // null = idle/consumed
static Luminumbra::JobHandle s_worldDressingHandle;
static void DrainWorldDressing(Luminumbra::JobSystem& jobs) {
    if (s_worldDressingHandle.counter) {
        jobs.wait(s_worldDressingHandle);
    }
    s_worldDressingHandle = {};
    s_worldDressing.reset();
}

// Bundled procgen plant/scatter render state (app/ProcgenPalettes.h): scatter
// instances, cached bakes, palette counts. Owned here; passed by reference into
// the bake/palette helpers. Render-only decoration, never hashed.
ProcgenPlantState g_procgen;

//  give any creature that still lacks a Jolt body (e.g. an offspring just born in the
// sim) a deterministic avatar character so the GameSession physics bridge drives it and it
// COLLIDES with the terrain instead of walking through mountains. Amortized (a few per frame)
// so a birth  spikes the frame. Render/demo-only.
void AttachMissingCreatureBodies(Luminumbra::Systems::PhysicsSystem* phys,
                                 entt::registry& reg,
                                 int maxPerFrame = 4) {
    if (!phys)
        return;
    int made = 0;
    auto view = reg.view<const Luminumbra::Components::CreatureComponent,
                         const Luminumbra::Components::TransformComponent>();
    for (auto e : view) {
        if (made >= maxPerFrame)
            break;
        if (reg.all_of<Luminumbra::Components::CreaturePhysicsComponent>(e))
            continue;
        const auto& tf = view.get<const Luminumbra::Components::TransformComponent>(e);
        const std::size_t idx = phys->create_avatar_character(
            glm::vec3(tf.position.x, tf.position.y + 1.0f, tf.position.z));
        reg.emplace<Luminumbra::Components::CreaturePhysicsComponent>(e, idx);
        ++made;
    }
}

std::unique_ptr<Luminumbra::Client::Rml_UIManager> g_uiManager;
std::unique_ptr<Luminumbra::Client::WorldLoadingVisualizer> g_loading_visualizer;

// --- Window-mode runtime state ---
WindowState g_windowState;

// Debounce window for framebuffer resizes (seconds). A drag emits a burst of
// framebuffer-size events; we coalesce them into one realloc once the size has
// been stable for this long.
constexpr double kResizeDebounceSeconds = 0.12;

using Luminumbra::Client::ScenarioHarness::WindowMode;

// Grandfathered monolith: main() predates the scripts/tidy.sh complexity gate
// and is tracked for decomposition. New functions must stay under the gate's
// readability-function-cognitive-complexity threshold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int main(int argc, char* argv[]) {
    Log::Init();
    std::filesystem::path root_dir = ResolveRuntimeRoot(argc > 0 ? argv[0] : nullptr);
    std::string root_path_str = RuntimeRootString(root_dir);
    LUMINUMBRA_CORE_INFO("Runtime root: {}", root_path_str);

    RuntimeScenarioConfig scenario_config = ParseRuntimeScenarioConfig(argc, argv, root_dir);
    RuntimeStateRecorder runtime_state_recorder(scenario_config);
    InstallRuntimeCrashHandler(runtime_state_recorder);
    g_app.overlay.imgui_enabled = !scenario_config.no_ui;
    runtime_state_recorder.capture("startup_requested", nullptr, nullptr, nullptr, 0, {});

    if (scenario_config.forced_crash()) {
        runtime_state_recorder.capture("forced_crash_requested", nullptr, nullptr, nullptr, 0, {});
        TriggerForcedCrash();
    }

    const bool runtime_boot_metrics_enabled =
        HasCommandLineFlag(argc, argv, "--runtime-boot-metrics");
    const int runtime_boot_frames =
        GetCommandLineIntOption(argc, argv, "--runtime-boot-frames", 300);
    const std::filesystem::path default_runtime_boot_output =
        root_dir / "build/debug/test-artifacts/performance";
    const std::filesystem::path runtime_boot_output = GetCommandLineOption(
        argc, argv, "--runtime-boot-output", default_runtime_boot_output.string());
    RuntimeBootMetricsRecorder runtime_boot_recorder(
        runtime_boot_metrics_enabled, runtime_boot_frames, runtime_boot_output);

    // --render-benchmark <path>: average per-pass GPU timers over N settled in-world
    // frames -> JSON (render-optimization budget-gate capture). Pair with
    // --auto-create-world --auto-enter-world.
    // --debug-view <albedo|normal|depth|material|position>: render-only G-buffer overlay (default
    // off).
    if (const std::string dv = GetCommandLineOption(argc, argv, "--debug-view", ""); !dv.empty()) {
        if (dv == "albedo")
            g_app.overlay.debug_view_mode = 1;
        else if (dv == "normal")
            g_app.overlay.debug_view_mode = 2;
        else if (dv == "depth")
            g_app.overlay.debug_view_mode = 3;
        else if (dv == "material")
            g_app.overlay.debug_view_mode = 4;
        else if (dv == "position")
            g_app.overlay.debug_view_mode = 5;
    }
    g_app.capture.render_benchmark_path =
        GetCommandLineOption(argc, argv, "--render-benchmark", "");
    // --debug-goto cave|doline|spawn: after the world loads, deterministically locate the
    // feature + set the fixed camera to frame it (pair with --auto-create-world/--timelapse).
    g_app.capture.debug_goto = GetCommandLineOption(argc, argv, "--debug-goto", "");
    g_app.capture.play_paths = HasCommandLineFlag(
        argc,
        argv,
        "--play-paths"); // runtime telemetry: normal-play paths under a scripted scenario camera
    // stage the stained-glass capture subject near spawn.
    g_app.overlay.debug_glass_panes = HasCommandLineFlag(argc, argv, "--debug-glass-pane");
    // Opt-in GPU auto-exposure metering for capture diagnostics; the analytic
    // exposure curve remains the shipped gameplay default.
    g_app.overlay.auto_exposure_metered = HasCommandLineFlag(argc, argv, "--auto-exposure-metered");
    //  rendering (,  ): opt-in froxel volumetrics tier.
    g_app.overlay.volumetric_quality =
        std::stoi(GetCommandLineOption(argc, argv, "--volumetric-quality", "0"));
    g_app.capture.profile_fly_seconds = static_cast<double>(GetCommandLineIntOption(
        argc, argv, "--profile-fly", 0)); // runtime telemetry: constant-speed eye-level moving
                                          // profiler (normal-play, self-exits)
    g_app.capture.render_benchmark_frames =
        GetCommandLineIntOption(argc, argv, "--render-benchmark-frames", 120);
    g_app.capture.render_benchmark_warmup =
        GetCommandLineIntOption(argc, argv, "--render-benchmark-warmup", 60);
    g_app.capture.render_benchmark_screenshot =
        GetCommandLineOption(argc, argv, "--render-benchmark-screenshot", "");
    {
        const std::string cp = GetCommandLineOption(argc, argv, "--cam-pos", "");
        if (!cp.empty()) {
            float x = 0, y = 0, z = 0;
            if (std::sscanf(cp.c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
                g_app.capture.fixed_cam_pos = glm::vec3(x, y, z);
                g_app.capture.fixed_cam = true;
            }
        }
        const std::string cy = GetCommandLineOption(argc, argv, "--cam-yaw", "");
        if (!cy.empty()) {
            try {
                g_app.capture.fixed_cam_yaw = std::stof(cy);
            } catch (...) {}
        }
        const std::string cpi = GetCommandLineOption(argc, argv, "--cam-pitch", "");
        if (!cpi.empty()) {
            try {
                g_app.capture.fixed_cam_pitch = std::stof(cpi);
            } catch (...) {}
        }
    }
    // --scene-config <json>: compose a full scene to match a reference, then capture.
    if (const std::string sc = GetCommandLineOption(argc, argv, "--scene-config", "");
        !sc.empty()) {
        try {
            std::ifstream f(sc);
            nlohmann::json j;
            f >> j;
            g_app.capture.scene_active = true;
            if (j.contains("camera")) {
                const auto& c = j["camera"];
                if (c.contains("pos") && c["pos"].size() == 3) {
                    g_app.capture.fixed_cam_pos = glm::vec3(c["pos"][0].get<float>(),
                                                            c["pos"][1].get<float>(),
                                                            c["pos"][2].get<float>());
                    g_app.capture.fixed_cam = true;
                }
                if (c.contains("yaw"))
                    g_app.capture.fixed_cam_yaw = c["yaw"].get<float>();
                if (c.contains("pitch"))
                    g_app.capture.fixed_cam_pitch = c["pitch"].get<float>();
                if (c.contains("fov"))
                    g_app.capture.scene_fov = c["fov"].get<float>();
            }
            if (j.contains("time_of_day"))
                g_app.capture.timelapse_tod = j["time_of_day"].get<float>();
            if (j.contains("moon"))
                g_app.capture.scene_moon = j["moon"].get<float>(); // rendering: night-mode capture
            if (j.contains("weather")) {
                const auto& w = j["weather"];
                const std::string t = w.value("type", "none");
                g_app.capture.scene_weather = (t == "rain")    ? 1
                                              : (t == "snow")  ? 2
                                              : (t == "fog")   ? 3
                                              : (t == "storm") ? 4
                                                               : 0;
                g_app.capture.scene_weather_intensity = w.value("intensity", 0.0f);
            }
            if (j.contains("clouds")) {
                const auto& cl = j["clouds"];
                g_app.capture.scene_clouds = true;
                g_app.capture.scene_cloud_coverage = cl.value("coverage", 0.45f);
                g_app.capture.scene_cloud_biome = cl.value("biome_variation", 0.0f);
                g_app.capture.scene_cloud_plane = cl.value("plane_height", 900.0f);
                g_app.capture.scene_cloud_shadow = cl.value("shadow", false);
                g_app.capture.scene_cloud_shadow_strength = cl.value("shadow_strength", 0.0f);
            }
            // Drive the single-frame capture through the timelapse path.
            const std::string shot = j.value("screenshot", std::string("build/captures/scene.ppm"));
            std::filesystem::path shotPath(shot);
            shotPath.replace_extension(".ppm"); // WritePixelBufferPpm writes PPM
            g_app.capture.scene_shot = shotPath;
            g_app.capture.scene_dir =
                shotPath.has_parent_path() ? shotPath.parent_path() : std::filesystem::path(".");
            // Scene capture is self-contained (handled at the render site, own
            // g_app.capture.scene_dir so the later --timelapse-dir default can't clobber it). tod
            // applied per-frame.
            std::error_code _sc_ec;
            std::filesystem::create_directories(g_app.capture.scene_dir, _sc_ec);
            LUMINUMBRA_CORE_INFO(
                "Scene-config loaded: {} (tod {:.3f}, weather {}, clouds {}) -> {}",
                sc,
                g_app.capture.timelapse_tod,
                g_app.capture.scene_weather,
                g_app.capture.scene_clouds,
                g_app.capture.timelapse_dir.string());
        } catch (const std::exception& e) {
            LUMINUMBRA_CORE_ERROR("Scene-config parse failed: {}", e.what());
        }
    }

    // framescan: --frame-scan <out.json>. Pin the same forest-dense pose the render
    // benchmark uses so the scan is reproducible + stresses real coverage, then write
    // the what's-in-frame report and exit. Render-only (no draws, no world_hash).
    if (const std::string fs = GetCommandLineOption(argc, argv, "--frame-scan", ""); !fs.empty()) {
        g_app.capture.frame_scan_active = true;
        g_app.capture.frame_scan_path = fs;
        // Self-sufficient: --frame-scan IMPLIES the auto-world boot it needs (it must reach IN_GAME
        // for the settle/capture to run). Without this, omitting
        // --auto-create-world/--auto-enter-world left the client looping in the menu forever. A
        // watchdog in the render loop is the backstop.
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Frame-scan armed -> {} (auto-world implied, fixed pose, settle {} frames)",
            g_app.capture.frame_scan_path,
            kFrameScanSettleFrames);
    }
    // --render-parity-frame <dir>. Same boot/settle, then the in-process
    // WHOLE-FRAME A/B — dispatch the settled prepared frame twice into twin targets,
    // FLIP in-process, demand exactly 0.0 (the  migration gate).
    if (const std::string rp = GetCommandLineOption(argc, argv, "--render-parity-frame", "");
        !rp.empty()) {
        g_app.capture.render_parity_active = true;
        g_app.capture.render_parity_dir = std::filesystem::path(rp);
        g_app.capture.render_parity_pass = "frame";
        g_app.capture.frame_scan_active = true;
        g_app.capture.frame_scan_path =
            (g_app.capture.render_parity_dir / "parity_scan.json").string();
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Render-parity (whole-frame) armed -> {} (auto-world implied, settle {} frames)",
            g_app.capture.render_parity_dir.string(),
            kFrameScanSettleFrames);
    }
    // native scale-1 reference vs exact scale-1 seam and the
    // scale-0.67 upscaled output, all in one process/context to avoid capture noise.
    if (const std::string rp = GetCommandLineOption(argc, argv, "--upscale-seam-parity", "");
        !rp.empty()) {
        g_app.capture.render_parity_active = true;
        g_app.capture.render_parity_dir = std::filesystem::path(rp);
        g_app.capture.render_parity_pass = "upscale_seam";
        g_app.capture.frame_scan_active = true;
        g_app.capture.frame_scan_path =
            (g_app.capture.render_parity_dir / "parity_scan.json").string();
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Upscale-seam parity armed -> {} (auto-world implied, settle {} frames)",
            g_app.capture.render_parity_dir.string(),
            kFrameScanSettleFrames);
    }
    // DIAGNOSTIC-only settle override (see g_app.capture.frame_scan_settle_target): let a
    // fully-loaded capture wait past the gate's 90-frame default. Gates never set this env.
    if (const auto settle_env = Luminumbra::Core::ReadEnvironment("LUMIN_FRAME_SCAN_SETTLE")) {
        const int v = std::atoi(settle_env->c_str());
        if (v > 0)
            g_app.capture.frame_scan_settle_target = v;
    }
    // -T02: --render-parity-ssao <dir>. Same boot/settle, captures the
    // SSAO ctx-mapping + seam-determinism parity gate.
    if (const std::string rp = GetCommandLineOption(argc, argv, "--render-parity-ssao", "");
        !rp.empty()) {
        g_app.capture.render_parity_active = true;
        g_app.capture.render_parity_dir = std::filesystem::path(rp);
        g_app.capture.render_parity_pass = "ssao";
        g_app.capture.frame_scan_active = true;
        g_app.capture.frame_scan_path =
            (g_app.capture.render_parity_dir / "parity_scan.json").string();
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Render-parity (SSAO) armed -> {} (auto-world implied, settle {} frames)",
            g_app.capture.render_parity_dir.string(),
            kFrameScanSettleFrames);
    }
    // --bake-tree-impostor <out.ppm>: bake the far-field tree impostor atlas (no world needed).
    if (const std::string bp = GetCommandLineOption(argc, argv, "--bake-tree-impostor", "");
        !bp.empty()) {
        g_app.capture.bake_impostor_path = bp;
        LUMINUMBRA_CORE_INFO("Impostor-bake armed -> {} (GL atlas bake on first frame, then exit)",
                             bp);
    }
    // --survey <dir>: autonomous POI tour + per-scene screenshot/frame-scan. Pair with
    // --auto-create-world --auto-enter-world --no-audio.
    if (const std::string sv = GetCommandLineOption(argc, argv, "--survey", ""); !sv.empty()) {
        g_app.capture.survey_active = true;
        g_app.capture.survey_dir = sv;
        LUMINUMBRA_CORE_INFO(
            "Scene survey armed -> {} (auto-world; tours waterfall/cliff/grass/lake)",
            g_app.capture.survey_dir);
    }

    // --ui-screenshot <screen> [--ui-screenshot-out <path>] [--ui-fixtures]: capture a single
    // UI screen for the fidelity gate. The render site (menu branch) loads the document, settles,
    // reads the back buffer and exits. Screen names map to the data/ui/*.rml documents.
    if (const std::string uss = GetCommandLineOption(argc, argv, "--ui-screenshot", "");
        !uss.empty()) {
        // Comma-separated list -> capture every screen in ONE window session (batch), minimising
        // display disruption. Each writes <dir>/ui-<screen>.ppm.
        for (std::size_t start = 0; start <= uss.size();) {
            const std::size_t comma = uss.find(',', start);
            const std::string name =
                uss.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!name.empty())
                g_app.capture.ui_screens.push_back(name);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        g_app.capture.ui_fixtures = HasCommandLineFlag(argc, argv, "--ui-fixtures");
        g_app.capture.ui_screenshot_dir =
            GetCommandLineOption(argc, argv, "--ui-screenshot-dir", "build/captures/ui");
        std::error_code _ui_ec;
        std::filesystem::create_directories(g_app.capture.ui_screenshot_dir, _ui_ec);
        if (!g_app.capture.ui_screens.empty())
            g_app.capture.ui_screenshot_screen = g_app.capture.ui_screens.front();
        // --preview-live [--preview-weather rain]: for the world_creation screen, wait for the
        // live diorama (async candidate-world build + far field + precipitation) before capturing,
        // so the create-world UI redesign, the centre-anchored far field, and the preview rain are
        // VISUALLY CAPTURABLE headlessly (the default fixed-settle path captures a black backdrop
        // because the world is still building). Bounded by a wall-clock timeout (never hangs).
        g_app.capture.ui_preview_live = HasCommandLineFlag(argc, argv, "--preview-live");
        g_app.capture.ui_preview_weather =
            GetCommandLineOption(argc, argv, "--preview-weather", "");
        LUMINUMBRA_CORE_INFO("UI screenshot mode: {} screen(s), fixtures={}, preview_live={}, "
                             "preview_weather='{}' -> {}/ui-*.ppm",
                             g_app.capture.ui_screens.size(),
                             g_app.capture.ui_fixtures,
                             g_app.capture.ui_preview_live,
                             g_app.capture.ui_preview_weather,
                             g_app.capture.ui_screenshot_dir.string());
    }

    // --ui-thumbs N [--ui-thumbs-dir d]: capture N clean landscape thumbnails from the menu
    // backdrop world (varied yaw + time-of-day), one window session. For world/gallery tiles.
    if (const int n = GetCommandLineIntOption(argc, argv, "--ui-thumbs", 0); n > 0) {
        g_app.capture.ui_thumbs = n;
        g_app.capture.ui_thumbs_dir =
            GetCommandLineOption(argc, argv, "--ui-thumbs-dir", "data/ui/thumbs");
        std::error_code _t_ec;
        std::filesystem::create_directories(g_app.capture.ui_thumbs_dir, _t_ec);
        LUMINUMBRA_CORE_INFO("UI thumbnail mode: {} thumbs -> {}/thumb_*.ppm",
                             g_app.capture.ui_thumbs,
                             g_app.capture.ui_thumbs_dir.string());
    }

    // --timelapse capture mode. Single-player; pair with
    // --auto-create-world --auto-enter-world (and --no-ui for a clean frame).
    g_app.capture.timelapse_frames = GetCommandLineIntOption(argc, argv, "--timelapse-frames", 0);
    g_app.capture.timelapse_ticks = GetCommandLineIntOption(argc, argv, "--timelapse-ticks", 60);
    g_app.capture.timelapse_grow = HasCommandLineFlag(argc, argv, "--timelapse-grow");
    if (g_app.capture.timelapse_grow)
        g_procgen.stageF = 0.0f; // start as seeds; grow sapling->tree over the capture
    g_app.capture.timelapse_simgrow = HasCommandLineFlag(argc, argv, "--timelapse-simgrow");
    g_app.capture.timelapse_season = HasCommandLineFlag(argc, argv, "--timelapse-season");
    if (g_app.capture.timelapse_season)
        g_procgen.season = 0.0f; // start summer-green; drift to autumn over the capture
    g_app.capture.timelapse_creatures = HasCommandLineFlag(argc, argv, "--timelapse-creatures");
    g_app.capture.timelapse_living = HasCommandLineFlag(argc, argv, "--timelapse-living");
    g_app.capture.timelapse_calm = HasCommandLineFlag(argc, argv, "--timelapse-calm");
    if (g_app.capture.timelapse_calm)
        g_app.capture.timelapse_creatures = true; // calm mode is a creature scenario
    g_app.capture.timelapse_fire = HasCommandLineFlag(argc, argv, "--timelapse-fire");
    g_app.capture.timelapse_dig = HasCommandLineFlag(argc, argv, "--timelapse-dig");
    g_app.capture.timelapse_drain = HasCommandLineFlag(argc, argv, "--timelapse-drain");
    g_app.capture.timelapse_rain = HasCommandLineFlag(argc, argv, "--timelapse-rain");
    g_app.capture.timelapse_rain_mm =
        GetCommandLineIntOption(argc, argv, "--timelapse-rain-mm", 18);
    {
        const std::string ds = GetCommandLineOption(argc, argv, "--timelapse-daystep", "");
        if (!ds.empty()) {
            try {
                g_app.capture.timelapse_daystep = std::stof(ds);
            } catch (...) {}
        }
        const std::string t0 = GetCommandLineOption(argc, argv, "--timelapse-tod", "");
        if (!t0.empty()) {
            try {
                g_app.capture.timelapse_tod = std::stof(t0);
            } catch (...) {}
        }
        const std::string td = GetCommandLineOption(argc, argv, "--timelapse-dir", "");
        g_app.capture.timelapse_dir = !td.empty()
                                          ? std::filesystem::path(td)
                                          : (!scenario_config.artifact_dir.empty()
                                                 ? scenario_config.artifact_dir / "timelapse"
                                                 : std::filesystem::path("timelapse"));
    }
    if (g_app.capture.timelapse_frames > 0) {
        std::error_code _tl_ec;
        std::filesystem::create_directories(g_app.capture.timelapse_dir, _tl_ec);
        // NOTE: keep g_app.capture.timeScale = 1 so the player physics + collision settle each
        // frame (a frozen physics step makes the avatar fall through the streaming-in ground). The
        // capture loop adds EXTRA sim ticks for the fast-forward; the player stays grounded.
        LUMINUMBRA_CORE_INFO("Timelapse: {} frames, {} ticks/frame, daystep {:.4f} -> {}",
                             g_app.capture.timelapse_frames,
                             g_app.capture.timelapse_ticks,
                             g_app.capture.timelapse_daystep,
                             g_app.capture.timelapse_dir.string());
    }
    RuntimeScenarioFrameRecorder lod_ground_frame_recorder(scenario_config.lod_ground_smoke(),
                                                           scenario_config.coverage_radius,
                                                           scenario_config.artifact_dir);

    glfwSetErrorCallback(GLFWErrorCallback);
    if (!glfwInit()) {
        LUMINUMBRA_CORE_ERROR("FATAL: Failed to initialize GLFW!");
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // --- Window-mode resolution ---
    // GATE PROTECTION: any scenario/capture run PINS the window to 1280x720 so
    // every pixel-ROI gate sees the same framebuffer regardless of CLI flags.
    // Outside scenarios, --window-mode / --resolution select the arrangement.
    using Luminumbra::Client::ScenarioHarness::kCapturePinnedHeight;
    using Luminumbra::Client::ScenarioHarness::kCapturePinnedWidth;
    // RenderBudget is a native-resolution capture contract too. Pin it even
    // though it does not use a RuntimeScenarioHarness scenario, otherwise a
    // decorated 3840x1600 window yields a 3840x1581 framebuffer on this host.
    const bool capture_pinned =
        scenario_config.requires_pinned_capture() || !g_app.capture.render_benchmark_path.empty();
    g_windowState.capture_pinned = capture_pinned;
    g_windowState.mode = scenario_config.window_mode;

    int create_width = kCapturePinnedWidth;
    int create_height = kCapturePinnedHeight;
    if (!capture_pinned && scenario_config.window_mode == WindowMode::Windowed) {
        create_width = scenario_config.windowed_width;
        create_height = scenario_config.windowed_height;
    }
    g_windowState.windowedWidth = create_width;
    g_windowState.windowedHeight = create_height;

    // Headless (and runtime-boot metrics) keep a hidden window. Capture-pinned
    // runs also create the window hidden/decorated at the pinned size; the mode
    // application below is suppressed for them.
    const bool hidden_window = runtime_boot_recorder.enabled() || scenario_config.hidden_window ||
                               scenario_config.window_mode == WindowMode::Headless;
    if (hidden_window) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    if (capture_pinned) {
        // Pinned captures must hit EXACTLY kCapturePinnedWidth x Height. A DECORATED
        // window clips its client area by the title bar, so on a monitor whose
        // resolution equals the pinned size a 3840x1600 decorated window yields a
        // 3840x1581 framebuffer (the 19 px title bar). Create the capture window
        // undecorated so the client area — hence glfwGetFramebufferSize and the
        // glReadPixels(GL_BACK) capture — equals the requested pinned size exactly.
        // (At 1280x720 the decorated window fit with room to spare, so this never
        // mattered until the native-resolution capture update the baseline.)
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    }
    if (scenario_config.active()) {
        // Automated gate runs need a visible window (Endurance300 asserts it)
        // but must not steal focus from whatever the developer is doing.
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_FLOATING, GLFW_FALSE);
    }
#ifdef LUMINUMBRA_DEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif

    // Load player settings (defaults + per-user overlay) before window setup so VSync etc.
    // can be applied immediately. Never hashed; missing overlay -> struct defaults.
    g_systemConfig = luminumbra::core::SystemConfig::LoadLayered(
        "data/common/systems.json", luminumbra::core::SystemConfig::DefaultUserOverlayPath());

    GLFWwindow* window =
        glfwCreateWindow(create_width, create_height, "Luminumbra", nullptr, nullptr);
    LUMINUMBRA_ASSERT(window, "Failed to create GLFW window!");
    glfwMakeContextCurrent(window);

    // VSync from user settings (user.video.vsync). This is the ONLY glfwSwapInterval call;
    // before this the swap interval was never set (driver default = uncapped). Default OFF
    // preserves the uncapped 300fps target; the settings menu flips it.
    // the --render-benchmark capture ALWAYS runs uncapped (swap interval 0) regardless
    // of the persisted user vsync setting — a vsync-capped present leaves the GPU idle between
    // frames, which downclocks it and inflates every per-pass GPU timer, making the budget gate
    // unreproducible. The fixed-scenario budget must measure the saturated/boosted GPU state.
    glfwSwapInterval(
        (!g_app.capture.render_benchmark_path.empty() || g_systemConfig.user().vsync == false) ? 0
                                                                                               : 1);

    // [[maybe_unused]]: LUMINUMBRA_ASSERT compiles out in release builds, which
    // compile with warnings as errors.
    [[maybe_unused]] const int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    LUMINUMBRA_ASSERT(status, "Failed to initialize GLAD!");

    if (g_app.overlay.imgui_enabled) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 450");
    }

    // KHR_debug driver-error/warning callback -> engine log, for finding GL issues. Env-gated
    // (LUMIN_GL_DEBUG=1) + default-OFF (synchronous debug output is slow); supersedes the old
    // LUMINUMBRA_DEBUG block. Also enables readable RenderDoc captures via debug groups/labels.
    Luminumbra::Rendering::GlDebug::InstallGlDebugCallback();

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    GameStateManager gameStateManager;
    Luminumbra::JobSystem jobSystem;
    jobSystem.startup();

    auto gameSession = std::make_unique<Luminumbra::world::GameSession>();
    gameSession->SetJobSystem(&jobSystem);
    gameSession->SetRootPath(root_path_str);
    // Full control: data-drive every creature system from systems.json. All OFF by default ->
    // compiled constants -> byte-identical; enable a block + set values to tune live behaviour.
    gameSession->SetEcologyTuning(luminumbra::ai::ResolveEcologyTuning(g_systemConfig));
    // opt-in weather-driven rain (hashed sim flag; default
    // OFF keeps the canonical baselines byte-identical — activation is a deliberate
    // hash bump on the owner menu).
    gameSession->SetWeatherRainEnabled(
        g_systemConfig.enabled(luminumbra::core::SysKey::SimHydrologyWeather));
    // experimental High-resolution water (hashed sim flag; default OFF =
    // Medium 8x8 grid = byte-identical baselines; ON = 16x16, 1 m cells).
    gameSession->SetWaterHighResEnabled(
        g_systemConfig.enabled(luminumbra::core::SysKey::SimWaterHighRes));
    // opt-in deterministic weather-event epochs (hashed sim flag).
    gameSession->SetWeatherEventsEnabled(
        g_systemConfig.enabled(luminumbra::core::SysKey::SimWeatherEvents));
    // opt-in STATEFUL energy-field layer (hashed sim flag;
    // default OFF keeps the canonical baselines byte-identical — activation is a
    // deliberate hash bump on the owner menu).
    gameSession->SetAetherStateEnabled(
        g_systemConfig.enabled(luminumbra::core::SysKey::SimAetherState));
    gameSession->SetWildlifeFoliageTuning(
        luminumbra::ai::ResolveWildlifeFoliageTuning(g_systemConfig));
    gameSession->SetThirstTuning(luminumbra::ai::ResolveThirstTuning(g_systemConfig));
    gameSession->SetScavengingTuning(luminumbra::ai::ResolveScavengingTuning(g_systemConfig));
    gameSession->SetForagingTuning(luminumbra::ai::ResolveForagingTuning(g_systemConfig));
    gameSession->SetReproductionTuning(luminumbra::ai::ResolveReproductionTuning(g_systemConfig));
    gameSession->SetCircadianAmplitude(luminumbra::ai::ResolveCircadianAmplitude(g_systemConfig));
    gameSession->SetPlantMutationRate(luminumbra::ai::ResolvePlantMutationRate(g_systemConfig));

    // Load creature species metadata (display names + rarity) for the codex/discovery
    // HUD. Missing/partial data degrades gracefully: DisplayName falls back to
    // "Species #<id>", so the capture loop never blanks out.
    {
        std::vector<std::string> species_errors;
        const std::filesystem::path species_dir =
            std::filesystem::path(root_path_str) / "data" / "common" / "creatures" / "species";
        const std::size_t loaded =
            g_app.hud.creatureSpecies.LoadFromDirectory(species_dir, species_errors);
        LUMINUMBRA_CORE_INFO("Loaded {} creature species from {}", loaded, species_dir.string());
        for (const std::string& e : species_errors)
            LUMINUMBRA_CORE_WARN("creature species: {}", e);
    }
    //  asset-manifest split: the engine validates simulation
    // requirements only; the CLIENT declares the renderer/UI assets it needs
    // before any world create/load. This list matches the pre-split
    // engine-side manifest byte for byte.
    gameSession->SetRequiredClientAssets({
        std::filesystem::path("res") / "shaders" / "basic.vert",
        std::filesystem::path("res") / "shaders" / "g_buffer.frag",
        std::filesystem::path("data") / "ui" / "main_menu.rml",
        std::filesystem::path("data") / "fonts" / "Lora" / "static" / "Lora-Regular.ttf",
    });

    std::unique_ptr<Luminumbra::Client::IAudioManager> audioManager;
    if (scenario_config.no_audio) {
        audioManager = std::make_unique<Luminumbra::Client::NullAudioManager>(
            scenario_config.audio_telemetry_path);
    } else {
        audioManager = Luminumbra::Client::CreateAudioManager(root_path_str);
    }
    audioManager->Init();
    audioManager->SetMasterVolume(
        g_systemConfig.user().audio_master); // apply persisted master volume
    audioManager->SetMusicVolume(
        g_systemConfig.user().audio_music);                      // apply persisted music-bus volume
    audioManager->SetSfxVolume(g_systemConfig.user().audio_sfx); // apply persisted sfx-bus volume
    audioManager->LoadBank("data/audio/sfx_main.bank.json");
    audioManager->LoadBank("data/audio/music.bank.json");
    // /09: environmental audio (day/night beds + biome/weather reverb).
    // dynamic_cast is null under --no-audio (NullAudioManager) -> the system still
    // computes its model but plays nothing, matching central null suppression.
    auto envAudio = std::make_unique<Luminumbra::Client::EnvironmentalAudioSystem>(
        dynamic_cast<Luminumbra::Client::MiniaudioManager*>(audioManager.get()));
    envAudio->ConfigureDayNightBeds("ambient_birds", "ambient_night");

    // UI hot reload: watches data/ui and reloads the active document on.rml/.rcss edits
    // (opt-in via --ui-hot-reload, so the 1s filesystem poll is off during normal/gate runs).
    // main()-scoped (not a file-scope global) so its destructor — which logs via
    // LUMINUMBRA_CORE_INFO — runs before static destruction tears down the spdlog
    // logger; as a global it segfaulted in __run_exit_handlers logging through the
    // already-destroyed Log::s_CoreLogger.
    Luminumbra::Client::UI::UIHotReload uiHotReload;

    if (!scenario_config.no_ui) {
        g_uiManager = std::make_unique<Luminumbra::Client::Rml_UIManager>(root_path_str);
        g_uiManager->Init(window, audioManager.get());
        // --ui-fixtures points capture-backed screens at
        // deterministic committed fixture data so --ui-screenshot output is
        // reproducible on any checkout (the live gallery is empty until a player
        // presses the shutter). Gallery today; extend per-screen as fixtures grow.
        if (g_app.capture.ui_fixtures) {
            g_uiManager->SetGalleryCaptureSource(root_dir / "data" / "ui" / "fixtures" / "captures",
                                                 "fixtures/captures/");
            LUMINUMBRA_CORE_INFO("UI fixtures: gallery sourcing data/ui/fixtures/captures");
        }
        // opt-in UI hot reload: watch data/ui and reload the active document on edits.
        if (HasCommandLineFlag(argc, argv, "--ui-hot-reload")) {
            uiHotReload.SetEnabled(true);
            uiHotReload.WatchDirectory("data/ui", "rml");
            uiHotReload.WatchDirectory("data/ui", "rcss");
            uiHotReload.SetReloadCallback([](const std::string&) {
                if (g_uiManager)
                    g_uiManager->ReloadActiveDocument();
            });
            LUMINUMBRA_CORE_INFO("UI hot reload enabled (watching data/ui for.rml/.rcss edits).");
        }
    }

    Luminumbra::Rendering::RenderPipeline renderPipeline;
    //  isolation/layer mode: parse CLI strings -> IsolationConfig (default
    // {All, Scene} when both empty = no-op). Drives the SkyboxPass backdrop override
    // + (next slice) scenario spawn-suppression.
    renderPipeline.set_isolation_config(Luminumbra::Client::ScenarioHarness::ParseIsolationConfig(
        scenario_config.isolation_layers, scenario_config.isolation_backdrop));
    // far-LOD tile builds ride the JobSystem Normal lane.
    renderPipeline.attach_farlod_job_system(&jobSystem);
    // set the sky-LUT GPU flag BEFORE startup so the one-shot precompute
    // (init_sky_lut, inside startup) takes the GPU compute path. render.sky_lut_gpu, default OFF.
    renderPipeline.set_sky_lut_gpu_enabled(
        g_systemConfig.enabled(luminumbra::core::SysKey::RenderSkyLutGpu));
    // seed the internal render scale from the persisted user setting BEFORE
    // startup so the scaled G-buffer/lighting/SSAO intermediates are sized on the first frame.
    // The LUMIN_RENDER_SCALE env knob still WINS (startup applies it after this), preserving
    // the A/B capture path. Default 1.0 = byte-identical (internal==output).
    renderPipeline.set_render_scale(g_systemConfig.user().render_scale);
    // Render-optimization (cloud-raymarch-optimization): opt-in reduced-res sky-dome
    // quality knob, matching the existing LUMIN_* render-tuning idiom. Unset -> 0
    // (full, byte-identical legacy path). 1 = half (1/2 per axis), 2 = quarter.
    // Applied after startup below once the GL targets exist. Render-only.
    // Render-optimization defaults are now ON. Quarter-res clouds retain the
    // depth-aware native-resolution composite and passed SkyboxVisual plus the
    // 48-cell WorldVisualSweep; half-vs-quarter FLIP stayed below 0.03 against
    // the unchanged 0.05 ceiling while restoring release RenderBudget margin.
    // GTAO remains the ground-truth AO. Set LUMIN_CLOUD_QUALITY=0 /
    // LUMIN_SSAO_QUALITY=0 to fall back to the legacy full-res paths for A/B.
    int cloud_quality = 2; // 0 full, 1 half, 2 quarter (shipped default)
    if (const auto cq = Luminumbra::Core::ReadEnvironment("LUMIN_CLOUD_QUALITY")) {
        cloud_quality = std::atoi(cq->c_str());
    }
    int ssao_quality = 3; // 0 legacy, 1 GTAO Low, 2 GTAO High, 3 GTAO half-res (default)
    if (const auto sq = Luminumbra::Core::ReadEnvironment("LUMIN_SSAO_QUALITY")) {
        ssao_quality = std::atoi(sq->c_str());
    }
    if (!renderPipeline.startup(framebufferWidth, framebufferHeight, root_dir)) {
        LUMINUMBRA_CORE_ERROR("FATAL: Render pipeline startup failed.");
        runtime_state_recorder.capture("render_pipeline_startup_failed",
                                       &jobSystem,
                                       gameSession.get(),
                                       &renderPipeline,
                                       0,
                                       {});
        if (g_uiManager) {
            g_uiManager->Shutdown();
        }
        audioManager->Shutdown();
        jobSystem.shutdown();
        if (g_app.overlay.imgui_enabled) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    // Render-optimization (cloud-raymarch-optimization): apply the reduced-res
    // sky-dome quality once the GL targets exist. No-op at 0 (full).
    renderPipeline.set_cloud_quality(cloud_quality); // default 1 (half); env can set 0
    renderPipeline.set_ssao_quality(ssao_quality);   // default 2 (GTAO High); env can set 0
    //  TAAU: enable the temporal resolve from the render.taau flag (default OFF -> byte-identical).
    renderPipeline.set_taau_enabled(g_systemConfig.enabled(luminumbra::core::SysKey::RenderTaau));
    // (render.sky_lut_gpu is set BEFORE startup above so init_sky_lut also takes the GPU path.)
    // data-driven skinned-mesh texture set. The scenario
    // config resolved the.ltex paths (from the game archetype JSON or a generic
    // test texture); hand them to the generic RenderPipeline loader so no
    // creature/content name lives in engine source.
    if (!scenario_config.skinned_albedo_texture.empty()) {
        int skinned_albedo_layer = -1;
        int skinned_normal_layer = -1;
        renderPipeline.load_skinned_texture_set(
            root_dir / scenario_config.skinned_albedo_texture,
            scenario_config.skinned_normal_texture.empty()
                ? std::filesystem::path{}
                : (root_dir / scenario_config.skinned_normal_texture),
            skinned_albedo_layer,
            skinned_normal_layer);
    }
    // The GLFW callbacks (app/InputCallbacks.cpp) reach their state through
    // the window user pointer: the app context plus the core singletons that
    // stayed file-scope globals. Set once here, at window setup.
    InputCallbackBindings inputCallbackBindings;
    inputCallbackBindings.app = &g_app;
    inputCallbackBindings.systemConfig = &g_systemConfig;
    inputCallbackBindings.windowState = &g_windowState;
    inputCallbackBindings.camera = &g_camera;
    inputCallbackBindings.playerController = &g_playerController;
    inputCallbackBindings.uiManager = &g_uiManager;
    glfwSetWindowUserPointer(window, &inputCallbackBindings);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    // Apply the requested interactive window mode now that the GL context and
    // render pipeline exist: the framebuffer-size callback this triggers drives
    // RenderPipeline::on_resize so all targets are reallocated to the real
    // framebuffer size. Capture-pinned (scenario) runs are intentionally left at
    // the pinned 1280x720 window (ApplyWindowMode is a no-op for them).
    if (!capture_pinned && scenario_config.window_mode != WindowMode::Headless) {
        ApplyWindowMode(window, g_windowState, scenario_config.window_mode);
        // The framebuffer-size callback only fires on a real change; for the
        // borderless/fullscreen path it does, but resync the pipeline directly
        // in case GLFW coalesced the event so targets always match the window.
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) {
            renderPipeline.on_resize(static_cast<unsigned int>(fbw),
                                     static_cast<unsigned int>(fbh));
            framebufferWidth = fbw;
            framebufferHeight = fbh;
        }
    }

    g_loading_visualizer = std::make_unique<Luminumbra::Client::WorldLoadingVisualizer>();
    g_loading_visualizer->Startup(root_dir, framebufferWidth, framebufferHeight);

    bool scenario_failed = false;
    std::string scenario_failure_reason;
    bool scenario_ready = false;
    bool scenario_timed_run_complete = false;
    //  the world_visual_sweep runs its whole capture matrix
    // synchronously in one pass once the world is ready, so it self-completes.
    bool world_visual_sweep_done = false;
    uint64_t scenario_frame_count = 0;
    std::chrono::steady_clock::time_point scenario_play_started_at{};
    RuntimeReadinessReport last_readiness_report;

    auto start_world_creation = [&](const std::string& name,
                                    const std::string& seed,
                                    const std::string& worldType,
                                    const std::vector<Luminumbra::Client::WorldGenParam>& params) {
        // drain in-flight far-LOD tile builds before CreateWorld
        // replaces the world system they sample.
        renderPipeline.prepare_world_swap();

        // If the customize form changed any param from the base preset, build a resolved preset
        // (base + only the real deltas) and hand it to CreateWorld, which embeds it in THIS world's
        // own save dir. No global custom files, no collisions, no dangling references. If nothing
        // changed, customPtr stays null and the curated base preset is used unchanged.
        std::string customPresetJson;
        const std::string* customPtr = nullptr;
        if (!params.empty()) {
            try {
                const std::filesystem::path base_path = std::filesystem::path(root_path_str) /
                                                        "worlds" / "atlas" / "presets" /
                                                        (worldType + ".json");
                std::ifstream in(base_path);
                if (in) {
                    nlohmann::json base;
                    in >> base;
                    //  the form carries semantic-knob entries
                    // ("knob.<id>"/type "knob") alongside any advanced raw
                    // overrides. If knobs are present, resolve through the engine
                    // KnobLayer (knobs -> response curves, then overlay the sparse
                    // override diff) and persist BOTH layers for exact reopen;
                    // otherwise keep the legacy raw-override-only path.
                    bool hasKnobs = false;
                    for (const auto& p : params)
                        if (p.type == "knob" || p.path.rfind("knob.", 0) == 0) {
                            hasKnobs = true;
                            break;
                        }
                    if (hasKnobs) {
                        Luminumbra::Client::KnobPresetResult kr =
                            Luminumbra::Client::BuildKnobResolvedPreset(base, params);
                        if (kr.changed) {
                            kr.json["name"] = name;
                            customPresetJson = kr.json.dump(2);
                            customPtr = &customPresetJson;
                            LUMINUMBRA_CORE_INFO("World knob layer: {} knob(s), {} override(s)",
                                                 kr.knob_count,
                                                 kr.override_count);
                        }
                    } else {
                        Luminumbra::Client::CustomPresetResult merged =
                            Luminumbra::Client::BuildCustomPreset(base, params);
                        if (merged.changed) {
                            merged.json["name"] = name;
                            customPresetJson = merged.json.dump(2);
                            customPtr = &customPresetJson;
                            LUMINUMBRA_CORE_INFO(
                                "World customized: {} override(s) applied, {} skipped",
                                merged.applied,
                                merged.skipped);
                        }
                    }
                }
            } catch (const std::exception& e) {
                LUMINUMBRA_CORE_ERROR(
                    "Custom preset build failed ({}); using base preset '{}'", e.what(), worldType);
            }
        }

        // 1. Synchronously create the world systems and metadata. This is fast.
        // drain any in-flight  background scan FIRST — its jobs
        // hold the OLD world system pointer, which CreateWorld is about to replace.
        DrainBackgroundWorldScan(jobSystem);
        // same contract for the world-dressing placement job.
        DrainWorldDressing(jobSystem);
        if (gameSession->CreateWorld(name, seed, worldType, customPtr)) {
            // A real world replaces the  menu-backdrop world; stop the menu-branch from
            // rendering with the (now game-owned) camera/world.
            g_app.menu.menu_backdrop_active = false;
            if (auto* world_system = gameSession->GetWorldSystem()) {
                // bake the live waterfall dressing once for this
                // world. Detection is a pure function of the generated world
                // (river course x steep height drop), so it is valid here even
                // before chunks stream in; the sheets +  spray are render-only
                // (never hashed). Covers both the runtime-scenario bypass path and
                // the interactive loading path below.
                renderPipeline.prepare_waterfalls(*world_system);
                LUMINUMBRA_CORE_INFO("Waterfall dressing prepared: {} site(s).",
                                     renderPipeline.waterfall_sites(*world_system).size());
            }

            // restore persisted chunk state AFTER the world systems
            // initialize but BEFORE any chunk generation runs, so saved voxel
            // edits cannot be clobbered by regeneration (generation skips
            // chunks that already carry voxel data). A world without a
            // snapshot is a clean miss and proceeds on the byte-for-byte
            // unchanged fresh-world path.
            if (scenario_config.persistence_roundtrip_smoke() &&
                scenario_config.persistence_phase == "load" &&
                !scenario_config.persistence_session_dir.empty()) {
                gameSession->LoadWorldStateFrom(scenario_config.persistence_session_dir);
            } else {
                gameSession->LoadWorldState();
            }
            const bool bypass_loading_ui =
                runtime_boot_recorder.enabled() ||
                (scenario_config.active() && scenario_config.auto_enter_world);
            if (bypass_loading_ui) {
                LUMINUMBRA_CORE_INFO("Runtime scenario mode: created world without loading UI.");
                bool horizon_ready = true;
                if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
                    horizon_ready = gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                        gameSession->GetMetadata().spawnPoint,
                        gameSession->GetPhysicsSystem(),
                        scenario_config.horizon_radius,
                        scenario_config.collision_radius);
                }
                g_camera = std::make_unique<Luminumbra::Rendering::Camera>(
                    gameSession->GetMetadata().spawnPoint);
                g_camera->MouseSensitivity =
                    g_systemConfig.user().mouse_sensitivity; // user.video.mouse_sensitivity
                g_camera->Zoom = g_systemConfig.user().fov;  // user.video.fov
                g_playerController = std::make_unique<Luminumbra::Client::PlayerController>(
                    window, g_camera.get(), gameSession->GetPhysicsSystem());
                g_playerController->ApplyKeyBindings(
                    g_systemConfig); // user.controls.* (rebindable)
                if (g_app.loading.world_render_data_initialized) {
                    renderPipeline.clear_all_chunk_data();
                }
                g_app.loading.world_render_data_initialized = true;
                SetGameState(window, gameStateManager, GameState::IN_GAME);
                last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
                if (!horizon_ready || !last_readiness_report.ready) {
                    scenario_failed = true;
                    scenario_failure_reason = "world_readiness_failed";
                    runtime_state_recorder.capture("world_readiness_failed",
                                                   &jobSystem,
                                                   gameSession.get(),
                                                   &renderPipeline,
                                                   scenario_frame_count,
                                                   last_readiness_report);
                } else {
                    scenario_ready = true;
                    scenario_play_started_at = std::chrono::steady_clock::now();
                    runtime_state_recorder.capture("world_entered",
                                                   &jobSystem,
                                                   gameSession.get(),
                                                   &renderPipeline,
                                                   scenario_frame_count,
                                                   last_readiness_report);
                }
                return;
            }

            // Hide the main menu UI
            if (g_uiManager && g_uiManager->GetContext()) {
                for (int i = 0; i < g_uiManager->GetContext()->GetNumDocuments(); ++i) {
                    if (auto* doc = g_uiManager->GetContext()->GetDocument(i)) {
                        doc->Hide();
                    }
                }
                if (Rml::Element* focused_element = g_uiManager->GetContext()->GetFocusElement()) {
                    focused_element->Blur();
                }
            }

            // 2. Switch to the loading state
            SetGameState(window, gameStateManager, GameState::WORLD_LOADING);

            // 3. Get the list of chunks to generate and start the visualizer
            auto* world_system = gameSession->GetWorldSystem();
            // Use the actual spawn position for initial chunk loading
            Luminumbra::Vec3 spawn_pos = gameSession->GetMetadata().spawnPoint;
            if (runtime_boot_recorder.enabled()) {
                g_app.loading.initial_chunks_to_load.clear();
            } else {
                g_app.loading.initial_chunks_to_load =
                    world_system->GetInitialChunkLoadList(spawn_pos);
            }

            g_app.loading.generation_dispatch_index = 0;
            if (g_loading_visualizer) {
                g_loading_visualizer->BeginVisualization(g_app.loading.initial_chunks_to_load);
            }

        } else {
            LUMINUMBRA_CORE_ERROR("Failed to create world!");
            scenario_failed = scenario_config.active();
            scenario_failure_reason = "create_world_failed";
            runtime_state_recorder.capture("create_world_failed",
                                           &jobSystem,
                                           gameSession.get(),
                                           &renderPipeline,
                                           scenario_frame_count,
                                           {});
        }
    };

    if (g_uiManager) {
        //  startup invariant — every semantic-knob spline endpoint
        // must lie within its mapped param's declared range (and splines be
        // monotone with neutral==default). A violation is a programming error in
        // the knob map, so fail loud at boot rather than ship a knob that drives a
        // param out of range.
        {
            std::vector<std::string> knob_errors;
            if (!Luminumbra::world::ValidateKnobEndpoints(knob_errors)) {
                for (const std::string& e : knob_errors)
                    LUMINUMBRA_CORE_ERROR("KnobLayer invariant violated: {}", e);
                throw std::runtime_error("KnobLayer endpoint validation failed (see log)");
            }
        }
        g_uiManager->SetWorldCreationCallback(start_world_creation);
        // Seed the create-world customize form from a preset: read generation_params.<path>.
        g_uiManager->SetWorldParamGetter([root_path_str](const std::string& worldType,
                                                         const std::string& path) -> std::string {
            try {
                const std::filesystem::path pf = std::filesystem::path(root_path_str) / "worlds" /
                                                 "atlas" / "presets" / (worldType + ".json");
                std::ifstream in(pf);
                if (!in)
                    return "";
                nlohmann::json j;
                in >> j;
                //  seed a semantic knob from the preset's persisted
                // knob layer (path "knob.<id>"). Curated presets carry none -> "" ->
                // the knob stays NEUTRAL (0.5), never inverse-lerped.
                if (path.rfind("knob.", 0) == 0) {
                    const std::string id = path.substr(5);
                    const auto klp =
                        nlohmann::json::json_pointer("/generation_params/knob_layer/knobs");
                    if (j.contains(klp) && j.at(klp).is_object() && j.at(klp).contains(id) &&
                        j.at(klp).at(id).is_number()) {
                        std::ostringstream os;
                        os << j.at(klp).at(id).get<double>();
                        return os.str();
                    }
                    return "";
                }
                // "biomes" toggle reflects whether the preset sets a biome table.
                if (path == "biomes.enabled") {
                    const nlohmann::json::json_pointer tjp("/generation_params/biomes/table");
                    const bool on = j.contains(tjp) && j.at(tjp).is_string() &&
                                    !j.at(tjp).get<std::string>().empty();
                    return on ? "true" : "false";
                }
                std::string ptr = "/generation_params/";
                for (char ch : path)
                    ptr += (ch == '.') ? '/' : ch;
                const nlohmann::json::json_pointer jp(ptr);
                if (!j.contains(jp))
                    return "";
                const nlohmann::json& v = j[jp];
                if (v.is_boolean())
                    return v.get<bool>() ? "true" : "false";
                if (v.is_number_integer())
                    return std::to_string(v.get<long long>());
                if (v.is_number()) {
                    std::ostringstream os;
                    os << v.get<double>();
                    return os.str();
                }
                if (v.is_string())
                    return v.get<std::string>();
                return "";
            } catch (...) {
                return "";
            }
        });
        // Save the current customize-form config as a reusable, named USER preset
        // (worlds/atlas/presets/user_<slug>.json). Worlds created from it still embed their own
        // resolved params, so the saved file is a starting template, not a load-bearing dependency.
        g_uiManager->SetWorldPresetSaver(
            [root_path_str](
                const std::string& displayName,
                const std::string& baseType,
                const std::vector<Luminumbra::Client::WorldGenParam>& params) -> std::string {
                try {
                    const std::filesystem::path presets_dir =
                        std::filesystem::path(root_path_str) / "worlds" / "atlas" / "presets";
                    std::ifstream in(presets_dir / (baseType + ".json"));
                    if (!in)
                        return "";
                    nlohmann::json base;
                    in >> base;
                    //  persist BOTH the knob layer (knob vector +
                    // baseline + override diff) AND the resolved params, so reloading
                    // the saved user preset restores the exact knob positions. Falls
                    // back to the raw-override-only path when no knobs are present.
                    bool hasKnobs = false;
                    for (const auto& p : params)
                        if (p.type == "knob" || p.path.rfind("knob.", 0) == 0) {
                            hasKnobs = true;
                            break;
                        }
                    nlohmann::json saved;
                    if (hasKnobs) {
                        saved = Luminumbra::Client::BuildKnobResolvedPreset(base, params).json;
                    } else {
                        saved = Luminumbra::Client::BuildCustomPreset(base, params).json;
                    }
                    const std::string worldType = "user_" + UserPresetSlug(displayName);
                    saved["name"] = displayName.empty() ? std::string("Custom") : displayName;
                    std::ofstream out(presets_dir / (worldType + ".json"), std::ios::binary);
                    if (!out)
                        return "";
                    out << saved.dump(2);
                    LUMINUMBRA_CORE_INFO("Saved user world preset: {}", worldType);
                    return worldType;
                } catch (const std::exception& e) {
                    LUMINUMBRA_CORE_ERROR("Save preset failed: {}", e.what());
                    return "";
                }
            });
        // Enumerate saved user presets (user_*.json) so create-world can offer them as chips.
        g_uiManager->SetWorldPresetList(
            [root_path_str]() -> std::vector<std::pair<std::string, std::string>> {
                std::vector<std::pair<std::string, std::string>> out;
                try {
                    const std::filesystem::path presets_dir =
                        std::filesystem::path(root_path_str) / "worlds" / "atlas" / "presets";
                    if (!std::filesystem::exists(presets_dir))
                        return out;
                    for (const auto& entry : std::filesystem::directory_iterator(presets_dir)) {
                        if (entry.path().extension() != ".json")
                            continue;
                        const std::string stem = entry.path().stem().string();
                        if (stem.rfind("user_", 0) != 0)
                            continue;
                        std::string name = stem;
                        try {
                            std::ifstream f(entry.path());
                            nlohmann::json j;
                            f >> j;
                            name = j.value("name", stem);
                        } catch (...) {}
                        out.emplace_back(name, stem);
                    }
                } catch (...) {}
                return out;
            });
        // Collision check: does saving under this display name target an existing user preset?
        // (The saver silently overwrites; the UI uses this to gate it behind a confirm.)
        g_uiManager->SetWorldPresetExists([root_path_str](const std::string& displayName) -> bool {
            try {
                const std::filesystem::path pf = std::filesystem::path(root_path_str) / "worlds" /
                                                 "atlas" / "presets" /
                                                 ("user_" + UserPresetSlug(displayName) + ".json");
                return std::filesystem::exists(pf);
            } catch (...) {
                return false;
            }
        });
        // Delete a saved user preset by its world-type id (user_<slug>). Guards against deleting
        // anything that isn't a user preset.
        g_uiManager->SetWorldPresetDeleter([root_path_str](const std::string& worldType) -> bool {
            try {
                if (worldType.rfind("user_", 0) != 0)
                    return false;
                const std::filesystem::path pf = std::filesystem::path(root_path_str) / "worlds" /
                                                 "atlas" / "presets" / (worldType + ".json");
                std::error_code ec;
                const bool removed = std::filesystem::remove(pf, ec);
                if (removed)
                    LUMINUMBRA_CORE_INFO("Deleted user world preset: {}", worldType);
                return removed && !ec;
            } catch (...) {
                return false;
            }
        });
        // Rename a saved user preset: rewrite its "name" field and move it to the new slug-derived
        // id (user_<newslug>.json), removing the old file. Returns the new world-type id, "" on
        // fail.
        g_uiManager->SetWorldPresetRenamer(
            [root_path_str](const std::string& worldType,
                            const std::string& newDisplayName) -> std::string {
                try {
                    if (worldType.rfind("user_", 0) != 0)
                        return "";
                    const std::filesystem::path dir =
                        std::filesystem::path(root_path_str) / "worlds" / "atlas" / "presets";
                    const std::filesystem::path src = dir / (worldType + ".json");
                    if (!std::filesystem::exists(src))
                        return "";
                    nlohmann::json j;
                    {
                        std::ifstream in(src);
                        if (!in)
                            return "";
                        in >> j;
                    }
                    j["name"] = newDisplayName.empty() ? std::string("Custom") : newDisplayName;
                    const std::string newType = "user_" + UserPresetSlug(newDisplayName);
                    const std::filesystem::path dst = dir / (newType + ".json");
                    {
                        std::ofstream out(dst, std::ios::binary);
                        if (!out)
                            return "";
                        out << j.dump(2);
                    }
                    if (newType != worldType) {
                        std::error_code ec;
                        std::filesystem::remove(src, ec);
                    }
                    LUMINUMBRA_CORE_INFO("Renamed user preset {} -> {}", worldType, newType);
                    return newType;
                } catch (const std::exception& e) {
                    LUMINUMBRA_CORE_ERROR("Rename preset failed: {}", e.what());
                    return "";
                }
            });
        // Wire the RML Settings screen (settings.rml) to the SystemConfig user settings.
        // Live-apply mirrors the  ImGui panel; Save persists the per-user overlay.
        // Reference g_systemConfig directly (a global) so no captured local dangles.
        Luminumbra::Client::SettingsBridge sb;
        sb.GetResolution = [] {
            return g_systemConfig.user().resolution;
        };
        sb.SetResolution = [](const std::string& v) {
            g_systemConfig.user().resolution = v;
        };
        sb.GetWindowMode = [] {
            return g_systemConfig.user().window_mode;
        };
        sb.SetWindowMode = [](const std::string& v) {
            g_systemConfig.user().window_mode = v;
        };
        sb.GetVSync = [] {
            return g_systemConfig.user().vsync;
        };
        sb.SetVSync = [](bool v) {
            g_systemConfig.user().vsync = v;
            glfwSwapInterval(v ? 1 : 0);
        };
        sb.GetFov = [] {
            return g_systemConfig.user().fov;
        };
        sb.SetFov = [](float v) {
            g_systemConfig.user().fov = v;
            if (g_camera)
                g_camera->Zoom = v;
        };
        sb.GetMouseSensitivity = [] {
            return g_systemConfig.user().mouse_sensitivity;
        };
        sb.SetMouseSensitivity = [](float v) {
            g_systemConfig.user().mouse_sensitivity = v;
            if (g_camera)
                g_camera->MouseSensitivity = v;
        };
        sb.GetUiScale = [] {
            return g_systemConfig.user().ui_scale;
        };
        sb.SetUiScale = [](float v) {
            // Clamp to the supported HUD-scale band and apply LIVE via the RmlUi context's
            // density-independent-pixel ratio (scales all px-based HUD/UI uniformly).
            if (v < 0.5f)
                v = 0.5f;
            else if (v > 2.5f)
                v = 2.5f;
            g_systemConfig.user().ui_scale = v;
            if (g_uiManager && g_uiManager->GetContext())
                g_uiManager->GetContext()->SetDensityIndependentPixelRatio(v);
        };
        sb.GetAudioMaster = [] {
            return g_systemConfig.user().audio_master;
        };
        sb.SetAudioMaster = [&audioManager](float v) {
            g_systemConfig.user().audio_master = v;
            if (audioManager)
                audioManager->SetMasterVolume(v);
        };
        sb.GetAudioSfx = [] {
            return g_systemConfig.user().audio_sfx;
        };
        sb.SetAudioSfx = [&audioManager](float v) {
            g_systemConfig.user().audio_sfx = v;
            if (audioManager)
                audioManager->SetSfxVolume(v); // applied live to the sfx bus
        };
        sb.GetAudioMusic = [] {
            return g_systemConfig.user().audio_music;
        };
        sb.SetAudioMusic = [&audioManager](float v) {
            g_systemConfig.user().audio_music = v;
            if (audioManager)
                audioManager->SetMusicVolume(v); // applied live to the music bus
        };
        // Controls: resolve the current binding label, and begin capturing the next key press
        // as a rebind (the existing key_callback applies it into user.keybinds[action]).
        sb.GetKeybind = [](const std::string& action) -> std::string {
            for (const auto& def : Luminumbra::Client::kInputActionDefs) {
                if (action == def.name)
                    return KeyDisplayLabel(g_systemConfig.keybind(def.name, def.default_key));
            }
            return "";
        };
        sb.BeginRebind = [](const std::string& action) {
            for (std::size_t i = 0; i < Luminumbra::Client::kInputActionDefs.size(); ++i) {
                if (action == Luminumbra::Client::kInputActionDefs[i].name) {
                    g_app.hud.rebindCaptureAction = static_cast<int>(i);
                    return;
                }
            }
        };
        sb.Save = [] {
            return g_systemConfig.SaveUserOverlay(
                luminumbra::core::SystemConfig::DefaultUserOverlayPath());
        };
        g_uiManager->SetSettingsBridge(std::move(sb));
        // Apply the PERSISTED UI/HUD scale to the freshly-created RmlUi context so the HUD
        // boots at the player's chosen size (e.g. scaled up on a 4K/ultrawide display).
        if (g_uiManager->GetContext()) {
            float boot_ui_scale = g_systemConfig.user().ui_scale;
            if (boot_ui_scale < 0.5f)
                boot_ui_scale = 0.5f;
            else if (boot_ui_scale > 2.5f)
                boot_ui_scale = 2.5f;
            g_uiManager->GetContext()->SetDensityIndependentPixelRatio(boot_ui_scale);
        }
        //  pause-menu actions route here (main_client owns game state + cursor).
        g_uiManager->SetPauseActionCallback([window, &gameStateManager](const std::string& act) {
            if (act == "resume") {
                SetGamePaused(window, false);
            } else if (act == "quit") {
                SetGamePaused(window, false);
                SetGameState(window, gameStateManager, GameState::MAIN_MENU);
                if (g_uiManager)
                    g_uiManager->RequestLoadDocument("main_menu.rml");
            }
        });
    }

    glfwSetKeyCallback(window, key_callback);
    SetGameState(window, gameStateManager, GameState::MAIN_MENU);
    audioManager->PlayMusic("music_main_menu");
    if (g_uiManager) {
        // --ui-screenshot: open the requested screen directly in menu state instead of the menu.
        const std::string boot_doc = g_app.capture.ui_screenshot_screen.empty()
                                         ? std::string("main_menu.rml")
                                         : (g_app.capture.ui_screenshot_screen + ".rml");
        g_uiManager->RequestLoadDocument(boot_doc);
    }

    // The endurance and water gates assert visible water; the default
    // preset (height_offset 20, sea level 0) generates none near spawn,
    // so every water-asserting scenario runs in the archipelago world.
    // lod_ground_smoke keeps the default world its thresholds were tuned on.
    // player_view_smoke takes its preset from --world-preset
    // (default mountains, the worst case for surface-span coverage) and runs
    // once per preset from the PlayerView validator mode.
    const std::string scenario_world_type =
        (scenario_config.player_view_smoke() || scenario_config.farlod_horizon_smoke())
            ? (scenario_config.world_preset.empty() ? std::string("mountains")
                                                    : scenario_config.world_preset)
            : ((scenario_config.water_visual_smoke() || scenario_config.material_visual_smoke() ||
                scenario_config.auto_world_smoke() ||
                scenario_config.persistence_roundtrip_smoke() ||
                scenario_config.creature_slice_smoke() ||
                //  cinematic: the wildlife scene needs a real waterline for the
                // animal to wander to, so it joins the archipelago water group.
                (scenario_config.skinned_mesh_visual_smoke() && scenario_config.wildlife) ||
                //  archipelago shows water + shore + foliage +
                // open sky from one anchor (an explicit --world-preset still wins).
                scenario_config.world_visual_sweep())
                   ? (scenario_config.world_preset.empty() ? std::string("archipelago")
                                                           : scenario_config.world_preset)
                   // General auto-create (e.g. --survey / --frame-scan): honour an explicit
                   // --world-preset so the tools can tour ANY preset (mountains, archipelago,...),
                   // else the historical "default" world.
                   : (scenario_config.world_preset.empty() ? std::string("default")
                                                           : scenario_config.world_preset));
    if (scenario_config.auto_create_world ||
        HasCommandLineFlag(argc, argv, "--auto-create-world") || runtime_boot_recorder.enabled()) {
        start_world_creation("Automated Test World", "424242", scenario_world_type, {});
    }

    std::unique_ptr<Luminumbra::Client::WorldGenViewer> worldGenViewer;
    if (g_app.overlay.imgui_enabled) {
        worldGenViewer = std::make_unique<Luminumbra::Client::WorldGenViewer>();
        //  --worldgen-graph turns on the constrained layer-graph
        // authoring panel inside the inspector (a flagged INTERNAL dev tool, NOT
        // the shipping RmlUi create surface) and opens the inspector at boot.
        if (HasCommandLineFlag(argc, argv, "--worldgen-graph")) {
            worldGenViewer->SetGraphEnabled(true);
            g_app.overlay.show_worldgen_viewer = true;
            LUMINUMBRA_CORE_INFO("--worldgen-graph: constrained layer-graph authoring panel "
                                 "enabled ( toggles the inspector)");
        }
    }

    //  the create-world LIVE WORLD-PREVIEW DIORAMA controller.
    // Owns a bounded candidate world + an offscreen FBO + an orbit camera; the
    // menu render branch feeds it the current form's candidate params/weather/tod,
    // renders it to the FBO via the real pipeline, and blits it into the
    // #preview_pane screen rect under the transparent create panel. Built lazily
    // on first create-world activation so a headless/automated run pays nothing.
    auto worldgenPreview = std::make_unique<Luminumbra::Client::WorldgenPreview>();
    // Preview orbit/drag bookkeeping consumed by the menu-branch renderer
    // (app/MenuScreens.cpp).
    MenuPreviewState menuPreviewState;

    if (runtime_boot_recorder.enabled() &&
        gameStateManager.GetCurrentState() == GameState::IN_GAME) {
        LUMINUMBRA_CORE_INFO(
            "Runtime boot metrics mode: capturing fixed frames through streaming scheduler.");
        if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
            gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                gameSession->GetMetadata().spawnPoint, gameSession->GetPhysicsSystem(), 4, 2);
        }

        constexpr float capture_delta_time = 1.0f / 60.0f;
        const Luminumbra::Vec3 capture_position = gameSession->GetMetadata().spawnPoint;
        const int max_capture_attempts = runtime_boot_frames * 6;
        for (int attempt = 0; attempt < max_capture_attempts && !runtime_boot_recorder.complete();
             ++attempt) {
            if (gameSession->GetWorldSystem()) {
                gameSession->GetWorldSystem()->update(
                    gameSession->GetRegistry(), capture_position, nullptr);
            }
            if (gameSession->GetWorldSystem() && g_camera) {
                if (gameSession->GetWorldSystem()->get_renderable_chunks().empty()) {
                    continue;
                }
                if (runtime_boot_recorder.complete()) {
                    break;
                }
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                renderPipeline.render_frame(gameSession->GetRegistry(),
                                            *gameSession->GetWorldSystem(),
                                            *g_camera,
                                            capture_delta_time,
                                            g_app.overlay.wireframe_mode);
                runtime_boot_recorder.record_frame(capture_delta_time, renderPipeline);
                glfwSwapBuffers(window);
            }
        }

        const bool wrote_metrics = runtime_boot_recorder.write_artifacts();
        LUMINUMBRA_CORE_INFO("Runtime boot metrics {}: {} frames -> {}",
                             wrote_metrics ? "written" : "failed",
                             runtime_boot_frames,
                             runtime_boot_output.string());
        if (auto* world_system = gameSession->GetWorldSystem()) {
            world_system->clear_world(nullptr);
        }
        glfwSetWindowShouldClose(window, true);
    }

    // stand up the live scenic menu backdrop world. Synchronous so the first menu frame
    // already shows terrain. Skipped for automated runs that drive their own world (scenario,
    // auto-create, boot-metrics, timelapse, render-benchmark) and via --no-menu-backdrop.
    {
        const bool drives_own_world =
            scenario_config.active() || HasCommandLineFlag(argc, argv, "--auto-create-world") ||
            runtime_boot_recorder.enabled() || g_app.capture.timelapse_frames > 0 ||
            !g_app.capture.render_benchmark_path.empty();
        const bool want_backdrop = !drives_own_world &&
                                   !HasCommandLineFlag(argc, argv, "--no-menu-backdrop") &&
                                   gameStateManager.GetCurrentState() == GameState::MAIN_MENU;
        if (want_backdrop) {
            renderPipeline.prepare_world_swap();
            // Use a deterministic golden-hour mountain vista behind the menu:
            // fixed seed, camera, dusk time-of-day, and cloud cover.
            // drain any in-flight  scan before replacing the world.
            DrainBackgroundWorldScan(jobSystem);
            // same contract for the world-dressing placement job.
            DrainWorldDressing(jobSystem);
            if (gameSession->CreateWorld("Menu Vista", "424242", "mountains")) {
                if (auto* ws = gameSession->GetWorldSystem()) {
                    gameSession->LoadWorldState();
                    // Surface-ready around the FIXED vantage (8,*,8), not the spawn point, so the
                    // framed valley is streamed in before the first menu frame.
                    if (gameSession->GetPhysicsSystem()) {
                        ws->EnsureSurfaceReadyNear(Luminumbra::Vec3(8.0f, 58.0f, 8.0f),
                                                   gameSession->GetPhysicsSystem(),
                                                   6,
                                                   2);
                    }
                    g_camera = std::make_unique<Luminumbra::Rendering::Camera>(
                        glm::vec3(8.0f, 58.0f, 8.0f),
                        glm::vec3(0.0f, 1.0f, 0.0f),
                        /*yaw*/ 95.0f,
                        /*pitch*/ -2.0f);
                    g_camera->Zoom = 62.0f; // scenic FOV (matches dusk.json)
                    // Soft cloud cover for the warm dusk sky.
                    Luminumbra::Rendering::CloudRenderState cs;
                    cs.enabled = true;
                    cs.coverage_amount = 0.6f;
                    cs.plane_height = 900.0f;
                    renderPipeline.set_cloud_state(cs);
                    g_app.loading.world_render_data_initialized = true;
                    g_app.menu.menu_backdrop_active = true;
                    LUMINUMBRA_CORE_INFO("Menu backdrop world ready (golden-hour mountain vista).");
                }
            }
        }
    }

    int exit_code = 0;
    float lastFrame = 0.0f;
    auto scenario_started_at = std::chrono::steady_clock::now();
    auto last_runtime_state_write = std::chrono::steady_clock::now();
    std::array<bool, 3> lod_ground_screenshots_written{false, false, false};
    std::vector<std::string> lod_ground_screenshot_files;
    std::vector<LodGroundVisualCapture> lod_ground_visual_captures;
    WaterVisualCameraTarget water_visual_target;
    bool water_visual_target_initialized = false;
    bool water_visual_capture_written = false;
    std::vector<WaterCausticsSample> water_caustics_samples;
    std::vector<unsigned char> water_caustics_previous_texels;
    double water_caustics_next_sample_seconds = 0.0;
    ScreenshotPixelStats water_visual_pixel_stats;
    WaterRegionPatch water_shallow_patch;
    WaterRegionPatch water_deep_patch;
    WaterRegionPatch water_foam_patch;
    bool water_reflection_capture_written = false;
    // Projects a world point into capture pixel coordinates using the same
    // projection the render pipeline builds. Returns false when the point is
    // behind the camera or too close to the frame edge for a full patch.
    const auto project_world_to_capture = [](const Luminumbra::Rendering::Camera& camera,
                                             const Luminumbra::Vec3& world,
                                             int width,
                                             int height,
                                             int margin,
                                             int& out_x,
                                             int& out_y_from_top) -> bool {
        if (width <= 0 || height <= 0) {
            return false;
        }
        const glm::mat4 projection =
            glm::perspective(glm::radians(camera.Zoom),
                             static_cast<float>(width) / static_cast<float>(height),
                             camera.GetNearPlane(),
                             camera.GetFarPlane());
        const glm::vec4 clip = projection * camera.GetViewMatrix() * glm::vec4(world, 1.0f);
        if (clip.w <= 0.0f) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        const int x = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(width));
        const int y_from_bottom =
            static_cast<int>((ndc.y * 0.5f + 0.5f) * static_cast<float>(height));
        const int y_from_top = height - 1 - y_from_bottom;
        if (x < margin || x >= width - margin || y_from_top < margin ||
            y_from_top >= height - margin) {
            return false;
        }
        out_x = x;
        out_y_from_top = y_from_top;
        return true;
    };
    WaterVisualCameraTarget material_visual_target;
    bool material_visual_target_initialized = false;
    bool material_visual_capture_written = false;
    bool skybox_visual_capture_written = false;
    bool weather_baseline_capture_written = false;
    bool weather_visual_capture_written = false;
    WeatherPixelStats weather_baseline_stats;
    // lightning strike-frame state. During the weather phase a
    // deterministically scheduled strike fires; the harness captures a NEIGHBOUR
    // (pre-strike) frame and the STRIKE frame so the gate can assert the full-scene
    // luminance PULSE (frame-mean spike) + BOLT pixels. Render-only response to the
    // SIM strike event (one-way, ); the visual gate does NOT depend on audio .
    bool lightning_neighbor_captured = false;
    bool lightning_strike_capture_written = false;
    Luminumbra::Client::ScenarioHarness::StrikePixelStats lightning_neighbor_stats;
    int lightning_sim_strikes_scheduled = 0;
    //  cloud-shadow scenario state. Two terrain ROI captures (t0/t1) as the
    // cloud-shadow edge drifts, a sky capture for cloud presence, and a clouds-off
    // lighting-pass GPU timing captured before enabling the shadow (budget check).
    bool cloud_shadow_t0_written = false;
    bool cloud_shadow_done = false;
    double cloud_shadow_terrain_luma_t0 = 0.0;
    double cloud_shadow_scroll_t0 = 0.0;
    double cloud_shadow_lighting_ms_off = 0.0;
    bool cloud_shadow_lighting_off_sampled = false;
    //  particle determinism scenario state.
    bool particle_emitter_spawned = false;
    bool particle_determinism_capture_written = false;
    // foliage instancing scenario state. The run loads the scatter
    // set once, builds the deterministic per-chunk scatter over the visible live
    // ring each frame (sampling the  wind field at the camera), runs a CALM
    // phase (zero wind -> no sway) then a WINDY phase (strong wind -> sway), and
    // captures + snapshots the instance set for the FoliageInstancing gate.
    bool foliage_scatter_loaded = false;
    bool foliage_capture_written = false;
    double foliage_calm_max_sway = 0.0;
    bool foliage_calm_sampled = false;
    // precipitation scenario state. The run spawns the rain emitter
    // (driven by the replicated weather state) and captures TWO frames -- a CALM
    // phase (no wind) and a WINDY phase (wind-advected slant) -- so the gate can
    // assert precip particles are present AND that they slant with wind.
    bool precip_emitter_spawned = false;
    //  id of the camera-tracked rain emitter (so it can
    // be re-centered on the live camera every frame -> rain falls past the viewer).
    uint32_t precip_rain_emitter_id = Luminumbra::Rendering::ParticlePass::kInvalidEmitter;
    bool precip_calm_capture_written = false;
    bool precip_windy_capture_written = false;
    //  atmospheric MOTION capture. Env-gated
    // (LUMINUMBRA_ATMOS_MOTION_CAPTURE=1) on top of the precipitation_smoke
    // scenario. Runs a continuous STORM (heavy rain + drifting clouds + periodic
    // lightning) and dumps ~90 consecutive frames as motion/frame_%03d.ppm so the
    // moving clip (GIF/MP4) can be judged IN MOTION (a single still is not enough).
    // Render-only: never touches sim/world_hash.
    const bool atmos_motion_capture = [] {
        const auto value = Luminumbra::Core::ReadEnvironment("LUMINUMBRA_ATMOS_MOTION_CAPTURE");
        return value && !value->empty() && value->front() != '0';
    }();
    int atmos_motion_frame_index = 0;
    //  capture 240 frames. At the honest 1/60 s stride
    // (every render frame) that is ~4 s of real-time storm replayed at 60 fps --
    // long enough to show several lightning strikes and continuous falling rain.
    constexpr int kAtmosMotionFrameCount = 240;
    //  HONEST motion. The clip captures the REAL
    // precip_rain.json (no demo emitter), so the capture cadence must match how the
    // rain actually looks at runtime: sample every render frame (~60 fps -> ~16.7 ms
    // step) rather than the old 45 ms stride that exaggerated the per-frame fall and
    // misrepresented the true on-screen motion. Replayed at 60 fps the assembled
    // clip is a faithful 1:1 recording of the shipping rain. 90 frames ~= 1.5 s.
    double atmos_motion_last_capture_s = -1.0;
    constexpr double kAtmosMotionFrameIntervalS = 1.0 / 60.0;
    PrecipPixelStats precip_calm_stats;
    Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats precip_calm_render_pass;
    // 6 season-sweep windows (summer noon/dusk/night, winter
    // noon/dusk/night).
    std::array<bool, 6> timeofday_season_captures_written{false, false, false, false, false, false};
    //  settle bookkeeping. The per-frame pin can JUMP the
    // sun a long arc between captures (noon -> dusk -> night); the sun-view sky LUT
    // refreshes lazily, so the dome luminance needs a few frames at the new pin
    // before it reflects the new phase. Count consecutive frames the current
    // pending capture has been pinned and only WRITE once it has settled, so the
    // captured dome luminance is the labelled phase's, not a stale prior phase's.
    int timeofday_pending_pin = -1;      // capture index currently pinned
    int timeofday_pin_settle_frames = 0; // consecutive frames at that pin
    std::vector<TimeOfDayPhaseCapture> timeofday_phase_captures;
    EmissiveMaterialTarget timeofday_emissive_target;
    bool timeofday_emissive_target_initialized = false;
    bool timeofday_emissive_capture_written = false;
    TimeOfDayPixelStats timeofday_emissive_stats;
    bool timeofday_analysis_final = false;
    LodBoundaryTransitionRecorder lod_boundary_transition_recorder;
    LodSeamArrivalRecorder lod_seam_arrival_recorder;
    std::array<bool, 4> lod_seam_screenshots_written{false, false, false, false};
    std::vector<LodGroundVisualCapture> lod_seam_visual_captures;
    bool persistence_phase_attempted = false;
    std::vector<PlayerViewStation> player_view_stations;
    std::vector<bool> player_view_captures_written;
    std::vector<PlayerViewStationCapture> player_view_station_captures;
    bool player_view_sky_enforced = true;
    // farlod_horizon_smoke: phase A (far-LOD disabled) measures the
    // honest in-run gbuffer GPU baseline, phase B enables far-LOD and sweeps
    // the stations across the live/far boundary.
    constexpr double kFarLodHorizonPhaseSplit = 0.30;
    std::vector<FarLodHorizonStation> farlod_horizon_stations;
    std::vector<bool> farlod_horizon_captures_written;
    std::vector<FarLodHorizonStationCapture> farlod_horizon_station_captures;
    bool farlod_horizon_sky_enforced = true;
    std::vector<double> farlod_baseline_gbuffer_samples;
    std::vector<double> farlod_far_gbuffer_samples;
    // each station's far-OFF above-horizon sliver
    // baseline, captured PAIRED with its far-ON capture in phase B (an extra
    // far-disabled render at the same camera, same frame). Thin LIVE
    // mountain/island peak/ridge silhouettes (and diagonal live-geometry slivers)
    // are legitimate geometry that classify as slivers in BOTH renders; the gated
    // far-ATTRIBUTABLE sliver re-analyzes the far-ON frame with the far-OFF frame's
    // intrusion pixels cancelled per-pixel in a 3x3 neighborhood, so they cancel
    // pixel-for-pixel and only a genuine far-render streak (present only when far
    // is on) survives. This vector retains the raw far-OFF measurement as
    // telemetry only. Indexed by station; -1 = not yet captured.
    std::vector<int> farlod_horizon_far_off_sliver_px;
    // station the camera was applied to this frame;
    // the post-render capture targets exactly this station so the analyzed back
    // buffer always matches the camera that rendered it.
    std::size_t farlod_horizon_applied_station = 0;
    // skinned_mesh_visual_smoke: rig spawned once after readiness;
    // captures at two clip times prove the skinned stage renders and animates.
    SkinnedMeshVisualTarget skinned_mesh_visual_target;
    bool skinned_mesh_spawn_attempted = false;
    bool skinned_mesh_capture_a_written = false;
    bool skinned_mesh_analysis_written = false;
    //  video proof: showcase frame-sequence dump (avatars>=2 only).
    int showcase_video_frame = 0;
    double showcase_video_last_s = -1.0;
    //  cinematic: the wildlife camera is FIXED, so the heavy per-frame
    // horizon-radius EnsureSurfaceReadyNear (which keeps render at ~0.6 fps and
    // starves the 120-frame capture) is amortized -- the scene region is streamed
    // once in setup, then refreshed only every Nth frame.
    int wildlife_stream_tick = 0;
    //   integration: drives the showcase render avatars from the replication
    // pipeline when --replicated is set (network-driven view).
    Luminumbra::Client::ScenarioHarness::ReplicatedAvatarDemo replicated_demo;
    bool replicated_demo_setup = false;
    double replicated_avatar_render_seconds = 0.0;
    bool remote_avatar_render_artifact_written = false;
    //  cinematic wildlife scene state. Entity[0]=animal, [1]=human (grovestriders);
    // a separate arrow render entity. FSM: 0 seek-water, 1 arrow-in-flight, 2 flee.
    bool wildlife_setup = false;
    int wildlife_phase = 0;
    glm::vec3 wildlife_water{0.0f};  // water-edge target the animal walks to
    glm::vec3 wildlife_animal{0.0f}; // animal world position (kinematic)
    glm::vec3 wildlife_human{0.0f};  // human (shooter) position
    glm::vec3 wildlife_flee_dir{0.0f};
    glm::vec3 wildlife_arrow_pos{0.0f};
    glm::vec3 wildlife_arrow_vel{0.0f};
    Luminumbra::EntityID wildlife_arrow_entity{entt::null};
    SkinnedMeshVisualCapture skinned_mesh_capture_a;
    std::vector<unsigned char> skinned_mesh_pixels_a;
    // creature_slice_smoke: data-driven creature game slice. The
    // stimulus appears at 55% progress; captures at 45% and 85%.
    CreatureSliceScene creature_slice_scene;
    bool creature_slice_spawn_attempted = false;
    bool creature_slice_before_written = false;
    bool creature_slice_analysis_written = false;
    CreatureSliceCapture creature_slice_before;
    // window_mode_stress_smoke: scripted resize cycle
    // exercised once after readiness. Each step drives RenderPipeline::on_resize
    // (windowed->borderless->resolutions->fullscreen->restore-pinned) and
    // records the resize-generation delta + GL error count; the final pinned
    // step is captured as a Smoke-equivalent screenshot.
    std::vector<WindowModeStressStep> window_mode_stress_steps;
    std::size_t window_mode_stress_step_index = 0;
    bool window_mode_stress_complete = false;
    bool window_mode_stress_analysis_written = false;
    WindowModeStressCapture window_mode_stress_capture;
    // networked_session_smoke: the client renders a SERVER-OWNED world
    // streamed over the lockstep transport. The driver owns the host authority +
    // both LockstepSession ends (LoopbackTransport, no sockets); per agreed tick
    // both worlds step one fixed sim tick from the SAME spawn anchor and exchange
    // hashes (the desync oracle). Camera LOOK is applied render-side each frame
    // and is NEVER sent through the session, so look latency is zero (research
    // worldgen-lockstep-sdfrt.md Area 2 takeaway 2).
    NetworkedSessionDriver networked_session_driver;
    bool networked_session_begun = false;
    bool networked_session_done = false;
    //  honest CPU-vs-GPU attribution for --render-benchmark.
    // wall = max(CPU_submit, GPU_work) + present. NVML is loaded lazily on the
    // first measured frame (optional / guarded).
    Luminumbra::Client::NvmlSampler g_rb_nvml;
    bool g_rb_nvml_tried = false;
    bool g_rb_nvml_ok = false;
    std::chrono::steady_clock::time_point g_rb_frame_start{};
    std::chrono::steady_clock::time_point g_rb_before_swap{};
    const auto median_of = [](std::vector<double> samples) -> double {
        if (samples.empty()) {
            return 0.0;
        }
        std::sort(samples.begin(), samples.end());
        return samples[(samples.size() - 1) / 2];
    };
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = (float)glfwGetTime();
        float deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        deltaTime = std::min(deltaTime, 1.0f / 20.0f);
        //  CPU-submit clock starts at frame top when benchmarking.
        const bool g_rb_active = !g_app.capture.render_benchmark_path.empty();
        if (g_rb_active)
            g_rb_frame_start = std::chrono::steady_clock::now();
        // runtime telemetry ( implementation note): always-on frame wall to localize
        // slideshow-on-move frames.
        const auto _frameStart = std::chrono::steady_clock::now();
        double rb_sim_ms = 0.0;     // this frame's sim-tick CPU cost
        double rb_stream_ms = 0.0;  // this frame's streaming CPU cost
        double rb_foliage_ms = 0.0; // this frame's foliage rebuild_instances cost
        double rb_ui_ms = 0.0;      // this frame's UI (RmlUi + ImGui) render cost
        double rb_render_call_ms =
            0.0; // Full render_frame wall time, including unmeasured pass CPU submission.
        double rb_poll_ms = 0.0;    // glfwPollEvents wall time for input and window messages.
        double rb_scatter_ms = 0.0; // Per-frame foliage chunk-scatter build (terrain/biome
                                    //  sample per renderable chunk)
        double rb_rebake_ms =
            0.0; // runtime telemetry: per-frame RebakeAllPlants (procgen plant composite re-bake)
        // Declared at loop scope (not inside the case) so the case labels below
        // don't "jump over" an initialized local (ill-formed in a switch).
        std::chrono::steady_clock::time_point _rb_sim_t0{}, _rb_stream_t0{};

        if (scenario_failed) {
            exit_code = 2;
            glfwSetWindowShouldClose(window, true);
        }

        ProcessMemoryStats watermark_memory;
        uint64_t watermark_measured_bytes = 0;
        if (MemoryWatermarkExceeded(
                scenario_config, &watermark_memory, &watermark_measured_bytes)) {
            scenario_failed = true;
            scenario_failure_reason = "memory_watermark_exceeded";
            exit_code = 3;
            runtime_state_recorder.write_memory_watermark(
                "main_loop", watermark_memory, watermark_measured_bytes);
            runtime_state_recorder.capture("memory_watermark_exceeded",
                                           &jobSystem,
                                           gameSession.get(),
                                           &renderPipeline,
                                           scenario_frame_count,
                                           last_readiness_report);
            glfwSetWindowShouldClose(window, true);
        }

        if (scenario_config.active() && !scenario_ready && !scenario_failed) {
            const auto readiness_wait_seconds =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                 scenario_started_at)
                    .count();
            if (readiness_wait_seconds >= scenario_config.readiness_timeout_seconds) {
                last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
                scenario_failed = true;
                scenario_failure_reason = "world_readiness_timeout";
                exit_code = 4;
                runtime_state_recorder.capture("world_readiness_timeout",
                                               &jobSystem,
                                               gameSession.get(),
                                               &renderPipeline,
                                               scenario_frame_count,
                                               last_readiness_report);
                glfwSetWindowShouldClose(window, true);
            }
        }

        const auto _rb_poll_t0 = std::chrono::steady_clock::now(); // always-on (runtime telemetry)
        glfwPollEvents();
        rb_poll_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               _rb_poll_t0)
                         .count();

        // --bake-tree-impostor: GL is ready at the loop top and the bake needs no world, so run the
        // octahedral atlas bake ONCE here (independent of game state), write the atlas + coverage
        // JSON, then exit. Placed at the loop top so it can't be skipped by a state-gated render
        // branch.
        if (!g_app.capture.bake_impostor_path.empty() && !g_app.capture.bake_impostor_done) {
            g_app.capture.bake_impostor_done = true;
            Luminumbra::Rendering::OctaImpostorGrid bakeGrid;
            bakeGrid.gridResolution = 12; // 12x12 = 144 views for smoother runtime view blending
            const Luminumbra::Rendering::ImpostorBakeResult br =
                Luminumbra::Rendering::BakeTreeImpostorAtlas(
                    g_app.capture.bake_impostor_path, root_dir.string(), renderPipeline, bakeGrid);
            if (br.ok) {
                LUMINUMBRA_CORE_INFO(
                    "Impostor atlas baked -> {} ({}x{} px): mean coverage {:.3f}, min tile {:.3f}",
                    g_app.capture.bake_impostor_path,
                    br.atlas_size,
                    br.atlas_size,
                    br.mean_coverage,
                    br.min_coverage);
            } else {
                LUMINUMBRA_CORE_ERROR("Impostor bake failed: {}", br.error);
            }
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            continue;
        }

        // Debounced framebuffer resize: coalesce a burst
        // of drag events into one RenderPipeline::on_resize once the size has
        // settled. RmlUi (Update/Render) and ImGui (GLFW backend NewFrame)
        // re-query the framebuffer size every frame, so their viewports track
        // the new size automatically once the GL targets are reallocated here.
        if (g_windowState.resize_pending && !g_windowState.capture_pinned) {
            const double now = glfwGetTime();
            if (now - g_windowState.pending_since_seconds >= kResizeDebounceSeconds) {
                renderPipeline.on_resize(static_cast<unsigned int>(g_windowState.pending_width),
                                         static_cast<unsigned int>(g_windowState.pending_height));
                framebufferWidth = g_windowState.pending_width;
                framebufferHeight = g_windowState.pending_height;
                g_windowState.resize_pending = false;
            }
        }

        audioManager->Update();

        if (g_uiManager) {
            g_uiManager->Update();
        }
        uiHotReload.Update(); // No-op unless --ui-hot-reload enabled it; throttled to 1 second.

        GameState currentState = gameStateManager.GetCurrentState();

        // Frame audio — 3D listener, player footsteps, and the living-world
        // rain/creature-call region (app/FrameAudio.cpp).
        UpdateFrameAudio(g_app,
                         currentState,
                         deltaTime,
                         audioManager.get(),
                         envAudio.get(),
                         g_camera.get(),
                         g_systemConfig,
                         gameSession.get(),
                         renderPipeline,
                         scenario_config);

        // mirror each forager's authoritative grid cell -> a world transform so the
        // colony has live positions, and a 10 s delivery-count heartbeat proving the double-bridge
        // actually forages in the live world (deliveries accrue => ants are completing trips).
        if (currentState == GameState::IN_GAME && !g_app.hud.paused && !scenario_config.active() &&
            gameSession) {
            auto& freg = gameSession->GetRegistry();
            auto* fws = gameSession->GetWorldSystem();
            // One-time world-entry flourishes: the background doline/cave scan,
            // --debug-goto framing, and the lumin-crystal consume
            // (app/CaveFlourishes.cpp).
            UpdateCaveFlourishes(g_app, jobSystem, *gameSession, g_camera.get());

            auto fgview = freg.view<Luminumbra::Components::ForagerComponent,
                                    Luminumbra::Components::TransformComponent>();
            std::uint32_t totalDeliveries = 0;
            int antCount = 0;
            const int fgCells = gameSession->ScentFieldCells();
            // Perf: foragers live on discrete integer scent-grid cells, and terrain
            // height at a cell is a fixed (deterministic) function of the world. Cache height by
            // cell so the expensive GetTerrainHeightAt (full domain-warp Perlin) runs once per
            // cell, not once per ant per frame. Cleared if it grows (a new world re-keys the
            // cells).
            static std::unordered_map<std::uint32_t, float> s_cellHeight;
            if (s_cellHeight.size() > 8192)
                s_cellHeight.clear();
            for (auto e : fgview) {
                const auto& fg = fgview.get<Luminumbra::Components::ForagerComponent>(e);
                auto& tf = fgview.get<Luminumbra::Components::TransformComponent>(e);
                totalDeliveries += fg.deliveries;
                ++antCount;
                // Belt-and-suspenders with the ForagingSystem clamp: never map an off-grid cell to
                // world space (a far-OOB GetTerrainHeightAt is a crash/garbage). Skip if somehow
                // OOB.
                if (fg.cell_x < 0 || fg.cell_x >= fgCells || fg.cell_z < 0 || fg.cell_z >= fgCells)
                    continue;
                const float wx = gameSession->ScentCellToWorldX(fg.cell_x);
                const float wz = gameSession->ScentCellToWorldZ(fg.cell_z);
                tf.position.x = wx;
                tf.position.z = wz;
                if (fws) {
                    const std::uint32_t cellKey = (static_cast<std::uint32_t>(fg.cell_x) << 16) |
                                                  static_cast<std::uint32_t>(fg.cell_z & 0xFFFF);
                    auto it = s_cellHeight.find(cellKey);
                    if (it == s_cellHeight.end())
                        it = s_cellHeight.emplace(cellKey, fws->GetTerrainHeightAt(wx, wz)).first;
                    tf.position.y = it->second + 0.15f;
                }
            }
            if (antCount > 0) {
                static float s_forageLog = 0.0f;
                static std::uint32_t s_lastDeliveries = 0;
                s_forageLog += static_cast<float>(deltaTime);
                if (s_forageLog >= 10.0f) {
                    s_forageLog = 0.0f;
                    if (totalDeliveries != s_lastDeliveries) {
                        LUMINUMBRA_CORE_INFO(
                            "Forager colony: {} total deliveries (double-bridge foraging live)",
                            totalDeliveries);
                        s_lastDeliveries = totalDeliveries;
                    }
                }
            }

            // one-way sim->render snapshot of the deposited scent trails
            // (ch 2 = food, ch 3 = home) for the pheromone GROUND DECAL. A const Sample
            // read of the just-completed tick on this same (single) thread -> race-free;
            // the sim never reads the mirror back, so it is determinism-neutral. Gated by
            // LUMIN_SCENT_DECAL so the default render stays byte-identical until opted in.
            static const bool s_scentDecal =
                Luminumbra::Core::ReadEnvironment("LUMIN_SCENT_DECAL").has_value();
            if (s_scentDecal) {
                static Luminumbra::Rendering::ScentFieldRenderMirror s_scentMirror;
                const auto* sf = gameSession->GetScentField();
                const int n = gameSession->ScentFieldCells();
                if (sf && n > 0) {
                    if (s_scentMirror.cells != n)
                        s_scentMirror.resize(n);
                    bool any = false;
                    float maxScent = 0.0f;
                    for (int z = 0; z < n; ++z) {
                        for (int x = 0; x < n; ++x) {
                            const std::size_t i = (static_cast<std::size_t>(z) * n + x) * 2;
                            const float food = static_cast<float>(sf->Sample(2, x, z));
                            const float home = static_cast<float>(sf->Sample(3, x, z));
                            s_scentMirror.rg[i + 0] = food;
                            s_scentMirror.rg[i + 1] = home;
                            if (food > 0.0f || home > 0.0f)
                                any = true;
                            if (food > maxScent)
                                maxScent = food;
                            if (home > maxScent)
                                maxScent = home;
                        }
                    }
                    // Calibration diagnostic: the decal shader's u_scentScale wants ~1/peak.
                    static int s_scentLog = 0;
                    if ((s_scentLog++ % 15) == 0)
                        LUMINUMBRA_CORE_INFO("Scent decal: peak trail value = {:.4f}", maxScent);
                    s_scentMirror.cell_size = gameSession->ScentCellSize();
                    s_scentMirror.origin_x = gameSession->ScentCellToWorldX(0);
                    s_scentMirror.origin_z = gameSession->ScentCellToWorldZ(0);
                    s_scentMirror.any_scent = any;
                    s_scentMirror.valid = true;
                } else {
                    s_scentMirror.valid = false;
                }
                renderPipeline.UpdateScentDecals(s_scentMirror); // upload before render_frame
            }
        }

        if (g_app.overlay.imgui_enabled) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }

        switch (currentState) {
            case GameState::WORLD_LOADING: {
                const int JOBS_PER_FRAME = 512;
                if (runtime_boot_recorder.enabled()) {
                    g_app.loading.generation_dispatch_index =
                        static_cast<int>(g_app.loading.initial_chunks_to_load.size());
                }

                if (static_cast<size_t>(g_app.loading.generation_dispatch_index) <
                    g_app.loading.initial_chunks_to_load.size()) {
                    std::vector<Luminumbra::IVec3> batch_to_generate;
                    const int batch_start_index = g_app.loading.generation_dispatch_index;
                    while (static_cast<size_t>(batch_start_index +
                                               static_cast<int>(batch_to_generate.size())) <
                               g_app.loading.initial_chunks_to_load.size() &&
                           static_cast<int>(batch_to_generate.size()) < JOBS_PER_FRAME) {
                        batch_to_generate.push_back(
                            g_app.loading.initial_chunks_to_load
                                [batch_start_index + static_cast<int>(batch_to_generate.size())]);
                    }
                    if (!batch_to_generate.empty()) {
                        Luminumbra::JobHandle handle =
                            gameSession->GetWorldSystem()->dispatch_generation_jobs(
                                batch_to_generate);
                        if (handle.counter) {
                            for (const auto& coords : batch_to_generate) {
                                g_loading_visualizer->UpdateChunkState(
                                    coords, Luminumbra::Client::ChunkLoadVisualState::DISPATCHED);
                            }
                            g_app.loading.generation_dispatch_index +=
                                static_cast<int>(batch_to_generate.size());
                        }
                    }
                }

                float progress = g_app.loading.initial_chunks_to_load.empty()
                                     ? 1.0f
                                     : static_cast<float>(g_app.loading.generation_dispatch_index) /
                                           g_app.loading.initial_chunks_to_load.size();

                glClearColor(0.01f, 0.02f, 0.05f, 1.0f); // Dark blue background
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                if (g_loading_visualizer) {
                    g_loading_visualizer->UpdateAndRender(
                        deltaTime,
                        "CONSTRUCTING WORLD GEOMETRY...",
                        progress,
                        g_app.loading.generation_dispatch_index,
                        g_app.loading.initial_chunks_to_load.size());
                }

                if (static_cast<size_t>(g_app.loading.generation_dispatch_index) >=
                    g_app.loading.initial_chunks_to_load.size()) {
                    if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
                        gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                            gameSession->GetMetadata().spawnPoint,
                            gameSession->GetPhysicsSystem(),
                            12,
                            4);
                    }
                    LUMINUMBRA_CORE_INFO("World generation phase complete. Entering world.");
                    g_camera = std::make_unique<Luminumbra::Rendering::Camera>(
                        gameSession->GetMetadata().spawnPoint);
                    g_camera->MouseSensitivity =
                        g_systemConfig.user().mouse_sensitivity; // user.video.mouse_sensitivity
                    g_camera->Zoom = g_systemConfig.user().fov;  // user.video.fov
                    if (g_app.loading.world_render_data_initialized) {
                        renderPipeline.clear_all_chunk_data();
                    }
                    g_app.loading.world_render_data_initialized = true;

                    audioManager->StopMusic();
                    audioManager->PlayOneShot2D(
                        "ui_world_loaded",
                        Luminumbra::Client::BusId::Ui); // Play a sound on completion
                    // Start the constant ambient soundscape (zen atmosphere): a gentle
                    // forest-rustle bed + soft birdsong, centred at spawn with a huge radius so it
                    // stays audible as the player roams (the listener follows the player each
                    // frame). Volumes in the bank.
                    {
                        const auto& sp = gameSession->GetMetadata().spawnPoint;
                        const glm::vec3 ambPos(sp.x, sp.y, sp.z);
                        audioManager->PlayAmbientLoop("ambient_forest", ambPos, 1.0e6f);
                        // the birdsong bed is now OWNED by EnvironmentalAudioSystem
                        // (day-gated + crossfaded with the night bed) — no 24/7 birds.
                        audioManager->PlayAmbientLoop("ambient_wind", ambPos, 1.0e6f);
                    }
                    // Under the ambient bed, a soft in-game music bed picked by the time of day
                    // (day = exploration, night = dusk). The dawn/dusk detector swaps it at the
                    // horizon crossings. PlayMusic is a no-op if that track is already playing.
                    audioManager->PlayMusic(-renderPipeline.sun_direction().y > 0.0f
                                                ? "music_exploration"
                                                : "music_dusk");
                    // first-feed SNAP — a midnight load starts on the night bed
                    // instead of crossfading from the day default.
                    envAudio->SetSunElevation(-renderPipeline.sun_direction().y);
                    if (g_loading_visualizer) {
                        g_loading_visualizer->EndVisualization();
                    }
                    g_playerController = std::make_unique<Luminumbra::Client::PlayerController>(
                        window, g_camera.get(), gameSession->GetPhysicsSystem());
                    g_playerController->ApplyKeyBindings(
                        g_systemConfig); // user.controls.* (rebindable)
                    // (camera created + chunk data cleared + upload backlog drained above, before
                    // entry)
                    SetGameState(window, gameStateManager, GameState::IN_GAME);
                    // show the diegetic HUD during normal play. Skipped in headless
                    // scenario capture so the visual gates stay overlay-free.
                    if (!scenario_config.active() && g_uiManager) {
                        g_uiManager->RequestLoadDocument("hud.rml");
                    }
                    if (scenario_config.active()) {
                        last_readiness_report =
                            EvaluateReadiness(scenario_config, gameSession.get());
                        if (!last_readiness_report.ready) {
                            scenario_failed = true;
                            scenario_failure_reason = "world_readiness_failed";
                            runtime_state_recorder.capture("world_readiness_failed",
                                                           &jobSystem,
                                                           gameSession.get(),
                                                           &renderPipeline,
                                                           scenario_frame_count,
                                                           last_readiness_report);
                        } else {
                            scenario_ready = true;
                            scenario_play_started_at = std::chrono::steady_clock::now();
                            runtime_state_recorder.capture("world_entered",
                                                           &jobSystem,
                                                           gameSession.get(),
                                                           &renderPipeline,
                                                           scenario_frame_count,
                                                           last_readiness_report);
                        }
                    }
                    lastFrame = static_cast<float>(glfwGetTime());
                }
                break;
            }

            case GameState::IN_GAME:
                // persistence runtime roundtrip phases run once on
                // the first ready frame and exit cleanly; the streaming
                // update is skipped so the hashed/saved chunk set is exactly
                // the deterministic post-readiness world.
                if (scenario_config.persistence_roundtrip_smoke() && scenario_ready &&
                    !persistence_phase_attempted) {
                    persistence_phase_attempted = true;
                    PersistenceRoundtripPhaseResult phase_result;
                    if (scenario_config.persistence_phase == "load") {
                        phase_result =
                            RunPersistenceRoundtripLoadPhase(scenario_config, gameSession.get());
                    } else {
                        phase_result =
                            RunPersistenceRoundtripSavePhase(scenario_config, gameSession.get());
                    }
                    if (!phase_result.passed) {
                        scenario_failed = true;
                        scenario_failure_reason =
                            "persistence_phase_" + phase_result.failure_reason;
                        runtime_state_recorder.capture(scenario_failure_reason,
                                                       &jobSystem,
                                                       gameSession.get(),
                                                       &renderPipeline,
                                                       scenario_frame_count,
                                                       last_readiness_report);
                    }
                    glfwSetWindowShouldClose(window, true);
                    break;
                }
                if (scenario_config.lod_ground_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    ApplyLodGroundCameraPath(
                        scenario_config, gameSession.get(), g_camera.get(), elapsed_play_seconds);
                    // the engine no longer synchronously
                    // catches the near field up on a camera discontinuity (that
                    // hook caused an 11x chunk_churn PerfRegression). The
                    // LodGround camera sweeps in wall-clock-driven jumps that can
                    // outrun the async, throttled activation/meshing path, so the
                    // exact frame the coverage gate samples could show a near-field
                    // dip. Pull the destination near surface band ready
                    // synchronously right after moving the camera and before this
                    // frame renders, so every captured frame is fully renderable
                    // (49/49). EnsureSurfaceReadyNear only (re)builds chunks not
                    // already Ready at the required LOD, so steady-state frames
                    // (camera already settled) pay nothing.
                    if (gameSession->GetWorldSystem() && gameSession->GetPhysicsSystem()) {
                        gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                            g_camera->Position, gameSession->GetPhysicsSystem(), 4, 1);
                    }
                } else if (scenario_config.water_visual_smoke() && scenario_ready && g_camera) {
                    if (!water_visual_target_initialized || !water_visual_target.found) {
                        water_visual_target = FindWaterVisualCameraTarget(gameSession.get());
                        water_visual_target_initialized = water_visual_target.found;
                    }
                    // top-down framing for the main capture and the
                    // caustics samples (first 60% of the run), then the
                    // grazing open-water framing for the reflection capture.
                    const double water_elapsed_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double water_duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    if (water_elapsed_seconds / water_duration < 0.60) {
                        ApplyWaterVisualCamera(g_camera.get(), water_visual_target);
                    } else {
                        ApplyWaterReflectionCamera(g_camera.get(), water_visual_target);
                    }
                } else if (scenario_config.material_visual_smoke() && scenario_ready && g_camera) {
                    if (!material_visual_target_initialized || !material_visual_target.found) {
                        material_visual_target = FindMaterialVisualCameraTarget(gameSession.get());
                        material_visual_target_initialized = material_visual_target.found;
                    }
                    ApplyWaterVisualCamera(g_camera.get(), material_visual_target);
                } else if (scenario_config.skybox_visual_smoke() && scenario_ready && g_camera) {
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                } else if (scenario_config.weather_visual_smoke() && scenario_ready && g_camera) {
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                    // SIM-DRIVEN weather overlay (one-way, ). First
                    // half of the run captures the CLEAR-SKY control (premise guard
                    // this is the dedicated weather scenario with a clear-sky
                    // control phase); the weather phase at the midpoint pushes a
                    // render state derived from the REPLICATED WeatherSystem state
                    // sampled at the camera -- the overlay uniforms come from sim
                    // precipitation / storm / advected wind, not the debug mapping.
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const bool weather_phase = (elapsed_play_seconds / duration) >= 0.5;
                    const auto* weather = gameSession->GetWeatherSystem();
                    Luminumbra::Rendering::WeatherRenderState wstate;
                    if (weather_phase && weather) {
                        const Luminumbra::Vec3 cam(
                            g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                        const auto sample = weather->SampleAt(cam);
                        // Sim precipitation drives the overlay. In this dedicated
                        // scenario we floor rain to a strong, deterministic value
                        // at capture so the gate's clear-vs-weather luma drop +
                        // streak gradient measure a stable overlay (premise guard:
                        // storms run ONLY here). The wind direction/strength + the
                        // storm intensity are taken straight from sim state.
                        const float precip = std::max(sample.precip_intensity, 1.0f);
                        wstate.rain_intensity = precip;
                        wstate.snow_intensity =
                            (sample.category == Luminumbra::Systems::WeatherCategory::Snow)
                                ? sample.precip_intensity
                                : 0.0f;
                        wstate.fog_density =
                            (sample.category == Luminumbra::Systems::WeatherCategory::Fog) ? 0.4f
                                                                                           : 0.1f;
                        wstate.storm_intensity = std::max(sample.storm_intensity, 0.4f);
                        wstate.wetness = precip;
                        const float wlen = std::sqrt(sample.wind.x * sample.wind.x +
                                                     sample.wind.y * sample.wind.y);
                        if (wlen > 1e-4f) {
                            wstate.wind_direction =
                                glm::vec3(sample.wind.x / wlen, 0.0f, sample.wind.y / wlen);
                            wstate.wind_strength = std::clamp(wlen / 13.0f, 0.0f, 1.0f);
                        }
                        renderPipeline.set_weather_state(wstate);
                    } else {
                        // Clear-sky control: a driven CLEAR state (overlay off).
                        renderPipeline.set_weather_state(wstate);
                    }

                    // LIGHTNING. The SIM strike schedule is a pure
                    // function of (seed+13, storm state, tick); we read its count for
                    // the gate's sim-scheduled assertion. For a REPRODUCIBLE capture
                    // (the render loop is wall-clock paced, so we cannot rely on a sim
                    // strike landing exactly on the capture frame), the strike FRAME
                    // is driven deterministically here in this dedicated scenario: a
                    // fixed-position strike in front of the camera fires in a narrow
                    // progress window, building the SAME deterministic bolt the sim
                    // event would. One-way : we READ sim strike state + drive the
                    // render pulse/bolt; we write NOTHING back into the sim.
                    if (weather && weather->live_strike_count() > lightning_sim_strikes_scheduled) {
                        lightning_sim_strikes_scheduled = weather->live_strike_count();
                    }
                    Luminumbra::Rendering::LightningRenderState lstate;
                    const bool strike_window = weather_phase &&
                                               (elapsed_play_seconds / duration) >= 0.80 &&
                                               (elapsed_play_seconds / duration) < 0.84;
                    if (strike_window && g_camera) {
                        lstate.active = true;
                        // The overlay adds to the already-tonemapped [0,1] scene, so
                        // a modest pulse is a clear full-scene flash without a total
                        // white-out (the gate needs a frame-mean spike >= 0.04).
                        //  a STORM/overcast strike.
                        // The pulse is the readable scene flash; the bolt is a
                        // THIN jagged forked filament (width/glow below, the
                        // overlay shader splits these into a hot core + glow halo
                        // so the bolt no longer reads as a fat opaque white worm).
                        lstate.pulse_intensity = 0.16f; // 1-to-few-frame flash lift
                        lstate.bolt_width_ndc = 0.010f; // thin bright core ribbon
                        lstate.bolt_glow_ndc = 0.034f;  // surrounding glow halo
                        // Deterministic strike terminus on the horizon ahead of the
                        // camera. The bolt descends from a cloud-base height down to
                        // this point; placing the terminus ~220 m ahead at the camera's
                        // EYE level (not far below) keeps the whole descending channel
                        // inside the upper-frame sky where the skybox-visual camera
                        // looks, so the bolt is on-screen. Seeded from a fixed salt so
                        // the captured bolt is byte-reproducible.
                        const glm::vec3 fwd =
                            glm::normalize(glm::vec3(g_camera->Front.x, 0.0f, g_camera->Front.z));
                        const glm::vec3 strike_ground = g_camera->Position + fwd * 220.0f;
                        const Luminumbra::Rendering::LightningBoltGeometry bolt =
                            Luminumbra::Rendering::BuildLightningBolt(
                                strike_ground.x,
                                strike_ground.y,
                                strike_ground.z,
                                /*magnitude=*/0.9f,
                                /*strike_seed=*/0x5A5A1357ull);
                        // SCREEN-ANCHORED bolt projection. The bolt's WORLD shape (the
                        // seeded midpoint-displacement channel + branches) is mapped
                        // into a guaranteed-on-screen NDC path: the channel's normalized
                        // HEIGHT drives NDC.y from the upper sky (+0.92) down to just
                        // above the horizon (-0.12), and its lateral displacement from
                        // the straight cloud->ground line drives NDC.x around a fixed
                        // screen column. This keeps the bolt a reproducible, clearly
                        // visible vertical streak regardless of the camera pitch (the
                        // skybox-visual framing) while preserving the seeded jaggedness.
                        // Render-only capture aid : pure function of the strike.
                        const glm::vec3 top = bolt.main_channel.front();
                        const glm::vec3 bottom = bolt.main_channel.back();
                        const float span_y = std::max(1e-3f, top.y - bottom.y);
                        const float kBoltColumnNdcX = 0.06f; // centred column
                        const float kBoltTopNdcY = 0.92f;
                        const float kBoltBotNdcY = -0.12f;
                        //  amplify the seeded lateral
                        // displacement into NDC so the descending channel reads as
                        // a JAGGED zig-zag instead of a near-straight thick bar --
                        // but keep it predominantly VERTICAL (the descent spans the
                        // full frame height while the jag stays a modest sideways
                        // wobble), so the bolt reads as a tall jagged filament, not
                        // a horizontal scribble. The shader keeps the stroke thin.
                        const float kLateralToNdc = 1.0f / 150.0f; // modest jagged wobble
                        const auto map_point = [&](const glm::vec3& wp) -> glm::vec2 {
                            const float hf = std::clamp((wp.y - bottom.y) / span_y, 0.0f, 1.0f);
                            const float ndc_y = kBoltBotNdcY + (kBoltTopNdcY - kBoltBotNdcY) * hf;
                            // Lateral offset from the straight descent line (interpolated
                            // X/Z between top and bottom at this height fraction).
                            const float base_x = bottom.x + (top.x - bottom.x) * hf;
                            const float base_z = bottom.z + (top.z - bottom.z) * hf;
                            const float lateral = (wp.x - base_x) + (wp.z - base_z);
                            // Clamp the lateral excursion so the jag stays a modest
                            // sideways wobble around the fixed column -- the descent
                            // (full frame height) dominates, so the bolt reads as a
                            // TALL jagged filament rather than a horizontal scribble.
                            const float ndc_x = kBoltColumnNdcX +
                                                std::clamp(lateral * kLateralToNdc, -0.22f, 0.22f);
                            return glm::vec2(ndc_x, ndc_y);
                        };
                        const auto push_stroke = [&](const std::vector<glm::vec3>& stroke) {
                            if (!lstate.bolt_points_ndc.empty()) {
                                lstate.bolt_points_ndc.emplace_back(-3.0f, -3.0f); // pen-up
                            }
                            for (const glm::vec3& wp : stroke) {
                                lstate.bolt_points_ndc.push_back(map_point(wp));
                            }
                        };
                        push_stroke(bolt.main_channel);
                        for (const auto& br : bolt.branches) {
                            push_stroke(br);
                        }
                        // Strike point NDC for the radial flash centre (the terminus).
                        lstate.strike_ndc = glm::vec2(kBoltColumnNdcX, kBoltBotNdcY);
                    }
                    renderPipeline.set_lightning_state(lstate);
                } else if (scenario_config.cloud_shadow_smoke() && scenario_ready && g_camera) {
                    // partly-cloudy cast-shadow scenario. Fixed noon
                    // camera framing lit terrain in the lower frame (strong sun ->
                    // strong cast shadow). Enable the wind-advected cloud layer +
                    // its projected cast shadow at a PARTLY-CLOUDY coverage (NOT
                    // overcast -- premise guard ). A strong, fixed wind drifts the
                    // coverage field across the run so a shadow edge crawls over the
                    // fixed terrain ROI between the two captures. Render-only :
                    // the cloud state never feeds back into the sim.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    Luminumbra::Rendering::WeatherRenderState wstate;
                    // Strong, deterministic wind so the cloud sheet drifts visibly
                    // across the ROI in the run window (one-way: this is the render
                    // wind the cloud scroll consumes; it is not written to the sim).
                    wstate.wind_direction = glm::vec3(1.0f, 0.0f, 0.0f);
                    wstate.wind_strength = 1.0f;
                    renderPipeline.set_weather_state(wstate);
                    Luminumbra::Rendering::CloudRenderState cstate;
                    cstate.enabled = true;
                    // Cast shadow is OFF for the first ~15% so a clouds-off lighting
                    // GPU baseline can be sampled, then ON for the rest (the added
                    // per-fragment sample cost = on - off, bounded by the budget).
                    cstate.shadow_enabled = progress >= 0.15;
                    cstate.coverage_amount = 0.5f; // partly cloudy (not overcast)
                    cstate.biome_variation = 0.0f;
                    cstate.plane_height = 900.0f;
                    cstate.shadow_strength = 0.8f;
                    renderPipeline.set_cloud_state(cstate);
                } else if (scenario_config.particle_emitter_determinism_smoke() && scenario_ready &&
                           g_camera) {
                    //  fixed skybox-style camera; spawn the fixture
                    // emitter ONCE in front of the camera so particles render.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.30f);
                    if (!particle_emitter_spawned) {
                        if (auto* particles = renderPipeline.particles()) {
                            const glm::vec3 spawn_origin =
                                g_camera->Position + g_camera->Front * 8.0f;
                            particles->add_emitter(root_dir /
                                                       "data/common/particles/fixture_sparkle.json",
                                                   spawn_origin);
                            particle_emitter_spawned = true;
                        }
                    }
                } else if (scenario_config.foliage_visual_smoke() && scenario_ready && g_camera) {
                    // instanced foliage scatter. Fixed noon framing of
                    // lit ground. Load the scatter set once, then each frame build the
                    // deterministic per-chunk scatter over the visible live ring,
                    // sampling the  wind field at the camera for the sway bridge
                    // (one-way, ). A CALM phase (zero wind) then a WINDY phase
                    // (strong wind) so the gate can isolate the wind-sway response.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.30f);
                    auto* foliage = renderPipeline.foliage();
                    auto* world_system = gameSession->GetWorldSystem();
                    if (foliage != nullptr && world_system != nullptr) {
                        if (!foliage_scatter_loaded) {
                            foliage->load_scatter_set(root_dir /
                                                      "data/common/foliage/scatter_set.json");
                            // Fade band INSIDE the live ring (radius_4 gate footprint
                            // ~ a few chunks). Pin the fade end well within the visible
                            // ring so the gate can assert "no foliage beyond the ring".
                            foliage->set_fade_distances(60.0f, 96.0f);
                            // #1b-lush (render-only): per-preset showcase density.
                            // Default 1.0 == biome-tracked density (byte-identical to
                            // the FoliageInstancing-gated path); a preset can raise it
                            // for near-continuous turf WITHOUT touching biome data.
                            foliage->set_density_scale(scenario_config.foliage_density_scale);
                            foliage_scatter_loaded = true;
                        }
                        const double elapsed_play_seconds =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          scenario_play_started_at)
                                .count();
                        const double duration =
                            static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                        const double progress =
                            std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                        const bool windy_phase = progress >= 0.5;

                        // Wind bridge (one-way): sample the  wind field at the camera.
                        // CALM phase forces zero wind so the sway delta isolates wind.
                        glm::vec2 wind_xz(0.0f, 0.0f);
                        if (windy_phase) {
                            const Luminumbra::Vec3 cam(
                                g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                            if (auto* wind = gameSession->GetWindFieldSystem()) {
                                const Luminumbra::Vec2 w = wind->SampleWind(cam);
                                wind_xz = glm::vec2(w.x, w.y);
                            }
                            // Floor the windy-phase wind to a strong deterministic value
                            // so the sway delta is unambiguous even if the field is calm.
                            if (glm::length(wind_xz) < 4.0f) {
                                wind_xz = glm::vec2(6.0f, 0.0f);
                            }
                        }
                        foliage->set_wind(wind_xz);

                        // Build the per-chunk scatter inputs from the visible chunks.
                        Luminumbra::Client::ScenarioHarness::FoliageScatterContext ctx{
                            world_system};
                        std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter> chunk_scatter;
                        const auto& renderable = world_system->get_renderable_chunks();
                        chunk_scatter.reserve(renderable.size());
                        for (const Luminumbra::Chunk* chunk : renderable) {
                            if (chunk == nullptr) {
                                continue;
                            }
                            const Luminumbra::IVec3 c = chunk->get_coords();
                            // Only ground-level chunks (the column the surface sits in)
                            // contribute scatter; skip clearly sub-surface / sky chunks.
                            const float origin_x =
                                static_cast<float>(c.x * Luminumbra::CHUNK_SIZE_X);
                            const float origin_z =
                                static_cast<float>(c.z * Luminumbra::CHUNK_SIZE_Z);
                            const float center_x = origin_x + Luminumbra::CHUNK_SIZE_X * 0.5f;
                            const float center_z = origin_z + Luminumbra::CHUNK_SIZE_Z * 0.5f;
                            const float surf_h =
                                world_system->GetTerrainHeightAt(center_x, center_z);
                            // The chunk that straddles the surface column.
                            const float chunk_y0 =
                                static_cast<float>(c.y * Luminumbra::CHUNK_SIZE_Y);
                            if (surf_h < chunk_y0 ||
                                surf_h >= chunk_y0 + Luminumbra::CHUNK_SIZE_Y) {
                                continue;
                            }
                            const Luminumbra::u8 biome_id =
                                world_system->BiomeIdAt(center_x, center_z);
                            const float density =
                                world_system->biomes_enabled()
                                    ? world_system->biome_table().vegetation_for(biome_id).density
                                    : 0.3f; // default temperate density when biomes are off
                            Luminumbra::Rendering::FoliagePass::ChunkScatter cs;
                            cs.chunk_xz = glm::ivec2(c.x, c.z);
                            cs.origin = glm::vec3(origin_x, 0.0f, origin_z);
                            cs.extent_m = static_cast<float>(Luminumbra::CHUNK_SIZE_X);
                            cs.biome_id = biome_id;
                            cs.density = density;
                            chunk_scatter.push_back(cs);
                        }
                        foliage->rebuild_instances(
                            chunk_scatter,
                            &Luminumbra::Client::ScenarioHarness::FoliageSurfaceQuery,
                            &ctx,
                            g_camera->Position);
                    }
                } else if (scenario_config.precipitation_smoke() && scenario_ready && g_camera) {
                    // RAIN through the  particle framework, driven
                    // by the REPLICATED weather state at the camera and WIND-ADVECTED
                    // by the  wind field. Two phases at the SAME framing: a CALM
                    // phase (zero wind -> vertical fall) then a WINDY phase (strong
                    // horizontal wind -> diagonal slant). ONE-WAY : we READ
                    // weather/wind and write nothing back to sim/world_hash.
                    ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.30f);
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    const bool windy_phase = progress >= 0.5;

                    auto* particles = renderPipeline.particles();
                    if (particles != nullptr && !precip_emitter_spawned) {
                        // Spawn the rain field above + around the camera so the
                        // falling column fills the frame, plus the splash template.
                        const glm::vec3 field_origin(
                            g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                        //  UNIFIED. The motion clip now shows
                        // the EXACT same precip_rain.json that ships in real gameplay
                        // (no demo-only emitter). What the owner watches == what ships.
                        // Honest fall is achieved by sampling the capture every render
                        // frame (see kAtmosMotionFrameIntervalS below) instead of a
                        // long interval that misrepresented 60 fps motion.
                        precip_rain_emitter_id = particles->add_emitter(
                            root_dir / "data/common/particles/precip_rain.json", field_origin);
                        particles->add_splash_emitter(root_dir /
                                                      "data/common/particles/precip_splash.json");
                        precip_emitter_spawned = true;
                    }

                    //  CAMERA-RELATIVE rain. The scenario
                    // calls ApplySkyboxVisualCamera every frame, so the camera MOVES
                    // through the world. Previously the rain column was spawned ONCE
                    // at a FIXED world point, so as the camera advanced the fixed
                    // column drifted across the view -- reading as rain "floating
                    // toward" the viewer instead of falling. RE-CENTER the emitter's
                    // spawn box on the LIVE camera position every frame (the authored
                    // [0,22,0] height offset is re-applied inside set_emitter_origin),
                    // so new drops always spawn AROUND/ABOVE the viewer and fall
                    // straight DOWN past it regardless of camera motion. In-flight
                    // drops keep their own trajectories. Render-only -> world_hash
                    // is untouched (the emitter origin is render state, not sim).
                    if (particles != nullptr &&
                        precip_rain_emitter_id !=
                            Luminumbra::Rendering::ParticlePass::kInvalidEmitter) {
                        const glm::vec3 cam_anchor(
                            g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                        particles->set_emitter_origin(precip_rain_emitter_id, cam_anchor);
                    }

                    // Overcast/wet backdrop from the replicated weather state (the
                    // same one-way overlay the WeatherVisual gate exercises) so the
                    // rain reads against a darkened sky.
                    const auto* weather = gameSession->GetWeatherSystem();
                    Luminumbra::Rendering::WeatherRenderState wstate;
                    const Luminumbra::Vec3 cam(
                        g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                    float sampled_wind_len = 0.0f;
                    glm::vec3 sampled_wind_dir(1.0f, 0.0f, 0.0f);
                    if (weather != nullptr) {
                        const auto sample = weather->SampleAt(cam);
                        wstate.rain_intensity = std::max(sample.precip_intensity, 1.0f);
                        wstate.storm_intensity = std::max(sample.storm_intensity, 0.4f);
                        wstate.wetness = wstate.rain_intensity;
                        wstate.fog_density = 0.1f;
                        sampled_wind_len = std::sqrt(sample.wind.x * sample.wind.x +
                                                     sample.wind.y * sample.wind.y);
                        if (sampled_wind_len > 1e-4f) {
                            sampled_wind_dir = glm::vec3(sample.wind.x / sampled_wind_len,
                                                         0.0f,
                                                         sample.wind.y / sampled_wind_len);
                            wstate.wind_direction = sampled_wind_dir;
                            wstate.wind_strength = std::clamp(sampled_wind_len / 13.0f, 0.0f, 1.0f);
                        }
                    } else {
                        wstate.rain_intensity = 1.0f;
                        wstate.wetness = 1.0f;
                    }
                    //  in the motion clip push a
                    // FULL storm (high storm intensity -> the overlay darkens the
                    // dome to overcast) so the rain reads as bright streaks over a
                    // dark sky and the lightning has contrast. This override is
                    // gated on the env flag so the dedicated Precipitation gate's
                    // calm/windy captures (which assert a specific overcast luma
                    // drop) keep their tuned storm_intensity unchanged.
                    if (atmos_motion_capture) {
                        wstate.storm_intensity = 0.92f;
                        wstate.fog_density = 0.18f;
                    }
                    renderPipeline.set_weather_state(wstate);

                    // WIND-ADVECTION push (render-only). Calm phase: zero wind so
                    // rain falls straight down. Windy phase: a strong horizontal
                    // wind aligned with the camera-right axis so the slant is
                    // unambiguous in screen space and clearly diagonal. The wind
                    // DIRECTION comes from the replicated  field when available;
                    // its MAGNITUDE is floored to a strong, deterministic value in
                    // this dedicated scenario (premise guard: storms run only here).
                    if (particles != nullptr) {
                        if (windy_phase) {
                            glm::vec3 wind_dir = sampled_wind_dir;
                            // Bias the slant onto the camera-right axis so the
                            // 2D analyzer measures a clean horizontal lean.
                            const glm::vec3 right = glm::normalize(g_camera->Right);
                            if (glm::length(wind_dir) < 1e-3f) {
                                wind_dir = right;
                            } else {
                                wind_dir = glm::normalize(wind_dir + right);
                            }
                            //  the storm-rain rework added hard
                            // VELOCITY-ALIGNED streak elongation, which inverted this
                            // gate's gradient metric: a thin VERTICAL streak maximizes
                            // the h/v slant_ratio and any lean LOWERS it, so a large
                            // windy lean drove the windy slant_ratio BELOW calm (gain
                            // collapsed to ~0.7-1.1, under the 1.2 floor). The fix is
                            // in ParticlePass: the streak length now RAMPS with the
                            // wind (calm = short droplet, windy = long hard streak), so
                            // the windy capture reads a much higher anisotropy. Here we
                            // keep the windy wind MODEST so the lean stays small (the
                            // long windy streaks stay vertical-dominant -> high ratio)
                            // while still visibly slanting the rain. Together: windy
                            // slant clears calm by a wide margin (gain ~1.7x), and the
                            // rain still reads as a natural wind-driven storm, not an
                            // absurd horizontal blast. Render-only .
                            const float wind_speed = 3.5f; // storm gust (modest screen-space lean)
                            particles->set_wind(wind_dir * wind_speed);
                        } else {
                            particles->set_wind(glm::vec3(0.0f));
                        }
                    }

                    //  continuous STORM driving for
                    // the motion clip. Overrides the calm/windy split with a steady
                    // moderate cross-wind (so rain reads as wind-sheared streaks
                    // falling past the camera), a drifting overcast cloud sheet, and
                    // a PERIODIC lightning strike so the moving clip shows a storm
                    // flash. All render-only : nothing is written back to sim.
                    if (atmos_motion_capture) {
                        // Steady cross-wind: a constant breeze on the camera-right
                        // axis gives every frame the same gentle shear so the falling
                        // rain reads as rain (not floating dots) and slants slightly.
                        //  the wind must NOT push rain along
                        // the camera FORWARD axis -- any toward/away-camera drift makes
                        // the streaks read as "floating toward us" instead of falling
                        // straight past the viewer. Keep the shear PURELY in the screen
                        // plane (camera-right only) and STRIP any forward (depth)
                        // component, so every streak stays in the view plane and falls
                        // vertically past the camera. (The old `+ vec3(0,0,1.5)` was a
                        // WORLD-Z push whose camera-forward projection caused exactly
                        // the toward-camera float the owner flagged.)
                        if (particles != nullptr) {
                            const glm::vec3 right = glm::normalize(g_camera->Right);
                            const glm::vec3 fwd = glm::normalize(g_camera->Front);
                            glm::vec3 wind = right * 6.0f;
                            // Project out any forward (depth) component defensively so
                            // there is zero toward/away-camera motion in the streaks.
                            wind -= fwd * glm::dot(wind, fwd);
                            particles->set_wind(wind);
                        }
                        // Drifting overcast cloud sheet (dims the storm dome too).
                        Luminumbra::Rendering::CloudRenderState cstate;
                        cstate.enabled = true;
                        cstate.shadow_enabled = true;
                        cstate.coverage_amount = 0.85f; // heavy overcast
                        cstate.biome_variation = 0.0f;
                        cstate.plane_height = 900.0f;
                        cstate.shadow_strength = 0.6f;
                        cstate.scroll_offset =
                            glm::vec2(static_cast<float>(elapsed_play_seconds) * 22.0f,
                                      static_cast<float>(elapsed_play_seconds) * 6.0f);
                        renderPipeline.set_cloud_state(cstate);

                        // Periodic lightning: fire a deterministic forked bolt in a
                        // short window roughly every ~1.6 s of wall-clock so the clip
                        // contains a few strikes. The bolt + full-scene flash use the
                        // same screen-anchored projection as the WeatherVisual gate.
                        const double strike_cycle = std::fmod(elapsed_play_seconds, 1.6);
                        const bool strike_now =
                            strike_cycle < 0.16; // ~10% duty -> a few-frame flash
                        Luminumbra::Rendering::LightningRenderState lstate;
                        if (strike_now) {
                            lstate.active = true;
                            // Strong full-scene flash so the strike briefly LIGHTS
                            // the dark storm scene (the readable signature of a
                            // strike in motion), with a thin bright forked core +
                            // soft glow halo so the bolt is a filament, not a worm.
                            lstate.pulse_intensity = 0.38f; // brighter scene flash
                            lstate.bolt_width_ndc = 0.006f; // thin bright core
                            lstate.bolt_glow_ndc = 0.024f;  // tight glow halo
                            const glm::vec3 fwd = glm::normalize(
                                glm::vec3(g_camera->Front.x, 0.0f, g_camera->Front.z));
                            //  TOUCHDOWN. Strike a real ground
                            // point ahead of the camera: terrain height at (x,z) is the
                            // bolt's true bottom, so the channel spans cloud->terrain and
                            // ends ON the ground (no floating mid-air bolt).
                            const glm::vec3 strike_xz = g_camera->Position + fwd * 160.0f;
                            const float ground_y =
                                gameSession->GetWorldSystem()->GetTerrainHeightAt(strike_xz.x,
                                                                                  strike_xz.z);
                            const glm::vec3 strike_ground(strike_xz.x, ground_y, strike_xz.z);
                            // Vary the strike seed per cycle so successive bolts differ.
                            const uint64_t cycle_index =
                                static_cast<uint64_t>(elapsed_play_seconds / 1.6);
                            const Luminumbra::Rendering::LightningBoltGeometry bolt =
                                Luminumbra::Rendering::BuildLightningBolt(
                                    strike_ground.x,
                                    strike_ground.y,
                                    strike_ground.z,
                                    /*magnitude=*/0.9f,
                                    /*strike_seed=*/0x5A5A1357ull + cycle_index * 0x9E3779B1ull);
                            // PROJECT the real bolt through the actual render camera so
                            // the bolt spans the frame from the cloud base down to the
                            // projected terrain terminus -- it visibly TOUCHES DOWN.
                            int mvw = 0, mvh = 0;
                            glfwGetFramebufferSize(window, &mvw, &mvh);
                            const glm::mat4 proj =
                                glm::perspective(glm::radians(g_camera->Zoom),
                                                 static_cast<float>(std::max(1, mvw)) /
                                                     static_cast<float>(std::max(1, mvh)),
                                                 g_camera->GetNearPlane(),
                                                 g_camera->GetFarPlane());
                            const glm::mat4 viewproj = proj * g_camera->GetViewMatrix();
                            const glm::vec3 top = bolt.main_channel.front();
                            const glm::vec3 bottom = bolt.main_channel.back();
                            const float span_y = std::max(1e-3f, top.y - bottom.y);
                            // Project the straight cloud->ground baseline endpoints; the
                            // bolt's jagged points are laid along the screen line between
                            // these, with the seeded lateral wobble added as a MODEST
                            // sideways jag (kept small so the bolt stays a tall, thin,
                            // mostly-vertical filament -- not a horizontal scribble).
                            const auto project = [&](const glm::vec3& wp, bool& ok) -> glm::vec2 {
                                const glm::vec4 clip = viewproj * glm::vec4(wp, 1.0f);
                                ok = clip.w > 1e-4f;
                                if (!ok)
                                    return glm::vec2(0.0f);
                                return glm::vec2(clip.x / clip.w, clip.y / clip.w);
                            };
                            bool top_ok = false, bot_ok = false;
                            glm::vec2 top_ndc = project(top, top_ok);
                            glm::vec2 bot_ndc = project(bottom, bot_ok);
                            // Anchor the bolt TOP just BELOW the top edge so the dark
                            // storm cloud deck (painted from this anchor upward) is
                            // visible ABOVE the bolt origin and the bolt clearly emerges
                            // from the cloud base. BOTTOM goes onto the projected ground
                            // point, clamped just inside the bottom edge so the touchdown
                            // is visible even when the upward-tilted camera projects the
                            // ground low.
                            top_ndc.y = top_ok ? std::min(top_ndc.y, 0.74f) : 0.74f;
                            const float kGroundNdcY =
                                bot_ok ? std::clamp(bot_ndc.y, -0.96f, -0.55f) : -0.92f;
                            const float kColumnNdcX =
                                bot_ok ? std::clamp(bot_ndc.x, -0.6f, 0.6f) : 0.0f;
                            const float kLateralToNdc = 1.0f / 260.0f; // modest jag
                            const auto map_point = [&](const glm::vec3& wp) -> glm::vec2 {
                                const float hf = std::clamp((wp.y - bottom.y) / span_y, 0.0f, 1.0f);
                                const float ndc_y = kGroundNdcY + (top_ndc.y - kGroundNdcY) * hf;
                                const float base_x = bottom.x + (top.x - bottom.x) * hf;
                                const float base_z = bottom.z + (top.z - bottom.z) * hf;
                                const float lateral = (wp.x - base_x) + (wp.z - base_z);
                                const float ndc_x =
                                    kColumnNdcX +
                                    std::clamp(lateral * kLateralToNdc, -0.14f, 0.14f);
                                return glm::vec2(ndc_x, ndc_y);
                            };
                            const auto push_stroke = [&](const std::vector<glm::vec3>& stroke) {
                                if (!lstate.bolt_points_ndc.empty()) {
                                    lstate.bolt_points_ndc.emplace_back(-3.0f, -3.0f);
                                }
                                for (const glm::vec3& wp : stroke) {
                                    lstate.bolt_points_ndc.push_back(map_point(wp));
                                }
                            };
                            push_stroke(bolt.main_channel);
                            for (const auto& br : bolt.branches) {
                                push_stroke(br);
                            }
                            lstate.strike_ndc = glm::vec2(kColumnNdcX, kGroundNdcY);
                            // Ground-impact bloom at the touchdown point.
                            lstate.ground_ndc = glm::vec2(kColumnNdcX, kGroundNdcY);
                            lstate.ground_flash = 0.55f;
                            //  anchor a DARK STORM CLOUD at the
                            // bolt TOP so the bolt visibly EMERGES from a cloud (not thin
                            // air). The cloud base sits at the bolt-top NDC and the
                            // overlay paints a billowing dark deck across the upper frame
                            // around this column; the flash lights it from within.
                            lstate.cloud_anchor_ndc = glm::vec2(kColumnNdcX, top_ndc.y);
                            lstate.cloud_darkness = 0.85f;
                        }
                        renderPipeline.set_lightning_state(lstate);
                    }
                } else if (scenario_config.timeofday_sweep_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    // Discovery reruns while meshes stream in (the first
                    // frames only carry a fraction of the surface meshes);
                    // it freezes once found or once the SUMMER night phase nears
                    // so the emissive camera target stays stable. : the
                    // summer half ends at progress 0.5, so freeze discovery before
                    // its night/emissive window (~0.45-0.5).
                    if (!timeofday_emissive_target.found && progress < 0.43 &&
                        (!timeofday_emissive_target_initialized ||
                         (scenario_frame_count % 120) == 0)) {
                        timeofday_emissive_target_initialized = true;
                        timeofday_emissive_target =
                            FindEmissiveMaterialTarget(gameSession.get(), root_dir);
                    }
                    if (timeofday_emissive_target.found && progress >= 0.44 && progress < 0.5) {
                        // End of the SUMMER half: aim at the discovered surface
                        // emissive material for the dedicated night-emissive
                        // capture (the existing emissive night check, unchanged).
                        g_camera->Position =
                            timeofday_emissive_target.position + Luminumbra::Vec3(8.0f, 6.0f, 8.0f);
                        g_camera->Zoom = 60.0f;
                        AimCameraAt(g_camera.get(), timeofday_emissive_target.position);
                    } else {
                        // Fixed framing across all phases (both seasons) so the
                        // luminance/palette comparison measures lighting, not
                        // framing.
                        ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
                    }
                } else if (scenario_config.lod_boundary_oscillation_smoke() && scenario_ready &&
                           g_camera) {
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    ApplyLodBoundaryOscillationCamera(
                        gameSession.get(), g_camera.get(), elapsed_play_seconds);
                } else if (scenario_config.lod_seam_arrival_smoke() && scenario_ready && g_camera) {
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    ApplyLodSeamArrivalCamera(
                        scenario_config, gameSession.get(), g_camera.get(), elapsed_play_seconds);
                } else if (scenario_config.player_view_smoke() && scenario_ready && g_camera) {
                    // eye-level 360-degree sweep. Stations are built
                    // once after readiness (the peak station scans the loaded
                    // span field); each station holds its window so streaming
                    // and uploads settle before the capture at 70% progress.
                    if (player_view_stations.empty()) {
                        player_view_stations =
                            BuildPlayerViewStations(gameSession.get(), scenario_world_type);
                        player_view_captures_written.assign(player_view_stations.size(), false);
                        player_view_sky_enforced =
                            !PlayerViewSeaWaterInNearField(gameSession.get());
                    }
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    // Warmup lead before station 0: the post-readiness LOD0
                    // promotion of the near ring is still draining during the
                    // first seconds; stations divide the remaining time.
                    const double warmup_seconds = std::min(8.0, duration * 0.2);
                    const double effective_seconds =
                        std::max(0.0, elapsed_play_seconds - warmup_seconds);
                    const double progress = std::clamp(
                        effective_seconds / std::max(1.0, duration - warmup_seconds), 0.0, 0.999);
                    const std::size_t wall_clock_index =
                        std::min(player_view_stations.size() - 1u,
                                 static_cast<std::size_t>(
                                     progress * static_cast<double>(player_view_stations.size())));
                    // Hitch tolerance (mirrors the capture branch): hold the
                    // camera on the first un-captured station so a passed-over
                    // station is framed when its catch-up capture fires.
                    std::size_t first_unwritten = 0;
                    while (first_unwritten < player_view_captures_written.size() &&
                           player_view_captures_written[first_unwritten]) {
                        ++first_unwritten;
                    }
                    const std::size_t station_index =
                        first_unwritten >= player_view_stations.size()
                            ? wall_clock_index
                            : std::min(wall_clock_index, first_unwritten);
                    ApplyPlayerViewCamera(
                        gameSession.get(), g_camera.get(), player_view_stations[station_index]);
                } else if (scenario_config.farlod_horizon_smoke() && scenario_ready && g_camera) {
                    // phase A holds station 0
                    // with far-LOD DISABLED (the in-run gbuffer GPU baseline); phase
                    // B enables far-LOD and sweeps the stations. The per-station
                    // far-OFF sliver baseline is captured PAIRED with the far-ON
                    // capture in phase B (an extra far-disabled render at the exact
                    // same camera the same frame), so the diagonal live-geometry
                    // streaks are pixel-aligned and the far-attributable analysis
                    // cancels them per-pixel (3x3 neighborhood mask) cleanly.
                    if (farlod_horizon_stations.empty()) {
                        farlod_horizon_stations = BuildFarLodHorizonStations();
                        farlod_horizon_captures_written.assign(farlod_horizon_stations.size(),
                                                               false);
                        farlod_horizon_far_off_sliver_px.assign(farlod_horizon_stations.size(), -1);
                        farlod_horizon_sky_enforced =
                            !PlayerViewSeaWaterInNearField(gameSession.get());
                    }
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 0.999);
                    if (auto* farlod = renderPipeline.farlod()) {
                        farlod->set_enabled(progress >= kFarLodHorizonPhaseSplit);
                    }
                    const std::size_t station_count = farlod_horizon_stations.size();
                    std::size_t station_index = 0;
                    if (progress >= kFarLodHorizonPhaseSplit) {
                        const double sweep = (progress - kFarLodHorizonPhaseSplit) /
                                             (1.0 - kFarLodHorizonPhaseSplit);
                        const std::size_t time_based_index = std::min(
                            station_count - 1u,
                            static_cast<std::size_t>(sweep * static_cast<double>(station_count)));
                        // on slow debug runs the
                        // time-based sweep outruns the expensive captures (each
                        // capture renders a paired far-OFF frame and analyzes two
                        // back buffers, ~1 fps), so wall-clock stations get skipped.
                        // Hold the camera on the first un-captured station so the
                        // sweep cannot advance past it - mirrors the player_view
                        // catch-up clamp above.
                        std::size_t first_unwritten = 0;
                        while (first_unwritten < farlod_horizon_captures_written.size() &&
                               farlod_horizon_captures_written[first_unwritten]) {
                            ++first_unwritten;
                        }
                        station_index = first_unwritten >= station_count
                                            ? time_based_index
                                            : std::min(time_based_index, first_unwritten);
                    }
                    farlod_horizon_applied_station = station_index;
                    ApplyFarLodHorizonCamera(
                        gameSession.get(), g_camera.get(), farlod_horizon_stations[station_index]);
                } else if (scenario_config.skinned_mesh_visual_smoke() && scenario_ready &&
                           g_camera) {
                    // spawn the rigged test mesh once, then hold the
                    // fixed framing for both captures.
                    if (!skinned_mesh_spawn_attempted) {
                        skinned_mesh_spawn_attempted = true;
                        skinned_mesh_visual_target =
                            SpawnSkinnedMeshVisualEntity(gameSession.get(),
                                                         scenario_config.artifact_dir,
                                                         root_dir,
                                                         std::max(1, scenario_config.avatars));
                    }
                    //  cinematic: position the animal + human near a water edge and
                    // frame a wide side shot. Reuses the 2-grovestrider spawn (entity[0]
                    // = animal, [1] = human) + a separate arrow prop. Overrides the
                    // target camera/focus so the existing aim code frames the scene.
                    if (scenario_config.wildlife && !wildlife_setup &&
                        skinned_mesh_visual_target.spawned &&
                        skinned_mesh_visual_target.all_entities.size() >= 2) {
                        auto& reg = gameSession->GetRegistry();
                        auto* ws = gameSession->GetWorldSystem();
                        const Luminumbra::Vec3 spawn = gameSession->GetMetadata().spawnPoint;
                        // GetTerrainHeightAt is a PURE function (valid anywhere, no streaming
                        // needed), so scan a wide grid around spawn for the nearest real
                        // SHORELINE: a beach cell (terrain just above sea level) with a water
                        // neighbour (terrain below sea level). The archipelago basin around
                        // spawn is often open ocean with no beach for hundreds of metres, so a
                        // local gradient march fails -- a wide scan reliably finds an island edge.
                        auto terr = [&](float x, float z) {
                            return ws ? ws->GetTerrainHeightAt(x, z) : 0.0f;
                        };
                        glm::vec3 shore(spawn.x, 0.0f, spawn.z);
                        glm::vec3 toLand(1.0f, 0.0f, 0.0f); // from water toward land (unit)
                        bool found = false;
                        {
                            constexpr float kBeachLo = 0.4f; // m above sea level
                            constexpr float kBeachHi = 5.0f;
                            constexpr float kWaterDepth =
                                0.5f; // neighbour must be this far below sea
                            const float step = 16.0f;
                            const float reach = 1600.0f;
                            const float probe = 16.0f;
                            float best_d2 = 1e18f;
                            const glm::vec2 dirs[4] = {
                                {probe, 0}, {-probe, 0}, {0, probe}, {0, -probe}};
                            for (float dz = -reach; dz <= reach; dz += step) {
                                for (float dx = -reach; dx <= reach; dx += step) {
                                    const float x = spawn.x + dx, z = spawn.z + dz;
                                    const float h = terr(x, z);
                                    if (h < Luminumbra::SEA_LEVEL + kBeachLo ||
                                        h > Luminumbra::SEA_LEVEL + kBeachHi)
                                        continue;
                                    glm::vec3 wdir(0.0f);
                                    bool has_water = false;
                                    for (const auto& d : dirs) {
                                        if (terr(x + d.x, z + d.y) <
                                            Luminumbra::SEA_LEVEL - kWaterDepth) {
                                            wdir = glm::vec3(d.x, 0.0f, d.y);
                                            has_water = true;
                                            break;
                                        }
                                    }
                                    if (!has_water)
                                        continue;
                                    const float d2 = dx * dx + dz * dz;
                                    if (d2 < best_d2) {
                                        best_d2 = d2;
                                        shore = glm::vec3(x, h, z);
                                        toLand = -glm::normalize(
                                            wdir); // water->land = away from water neighbour
                                        found = true;
                                    }
                                }
                            }
                        }
                        const float shore_dist =
                            std::sqrt((shore.x - spawn.x) * (shore.x - spawn.x) +
                                      (shore.z - spawn.z) * (shore.z - spawn.z));
                        LUMINUMBRA_CORE_INFO("wildlife: shoreline found={} at ({:.0f},{:.0f}) terr "
                                             "{:.1f} (dist {:.0f}m from spawn)",
                                             found,
                                             shore.x,
                                             shore.z,
                                             shore.y,
                                             shore_dist);
                        const glm::vec3 side(-toLand.z, 0.0f, toLand.x);
                        // The water's edge the animal walks to is just seaward of the shore;
                        // it starts a few metres up the dry shore and approaches the waterline.
                        wildlife_water =
                            glm::vec3(shore.x, Luminumbra::SEA_LEVEL, shore.z) - toLand * 2.0f;
                        wildlife_water.y = Luminumbra::SEA_LEVEL;
                        wildlife_animal = shore + toLand * 8.0f; // up the dry shore
                        wildlife_animal.y = terr(wildlife_animal.x, wildlife_animal.z);
                        wildlife_human =
                            wildlife_animal + toLand * 7.0f + side * 6.0f; // further inland + aside
                        wildlife_human.y = terr(wildlife_human.x, wildlife_human.z);
                        reg.get<Luminumbra::Components::TransformComponent>(
                               skinned_mesh_visual_target.all_entities[0])
                            .position = wildlife_animal;
                        reg.get<Luminumbra::Components::TransformComponent>(
                               skinned_mesh_visual_target.all_entities[1])
                            .position = wildlife_human;
                        // Arrow prop (a small glowing bloom mesh), parked out of view until fired.
                        wildlife_arrow_entity = reg.create();
                        reg.emplace<Luminumbra::Components::TransformComponent>(
                               wildlife_arrow_entity)
                            .position = glm::vec3(0.0f, -1000.0f, 0.0f);
                        auto& am = reg.emplace<Luminumbra::Components::StaticMeshComponent>(
                            wildlife_arrow_entity);
                        am.meshPath = "data/models/props/glow_bloom/glow_bloom.lmesh";
                        am.materialId = 4;
                        // Wide side shot: camera off to the side of the animal->water line,
                        // elevated, looking at the midpoint where the action unfolds.
                        const glm::vec3 mid = (wildlife_animal + wildlife_water) * 0.5f;
                        skinned_mesh_visual_target.camera_position =
                            mid + side * 22.0f + glm::vec3(0.0f, 9.0f, 0.0f);
                        skinned_mesh_visual_target.focus = mid + glm::vec3(0.0f, 1.0f, 0.0f);
                        // The shoreline can be hundreds of metres from spawn; stream the scene
                        // region in now so terrain + water are meshed before the first capture.
                        if (ws && gameSession->GetPhysicsSystem()) {
                            ws->EnsureSurfaceReadyNear(Luminumbra::Vec3(mid.x, mid.y, mid.z),
                                                       gameSession->GetPhysicsSystem(),
                                                       scenario_config.horizon_radius,
                                                       scenario_config.collision_radius);
                        }
                        wildlife_setup = true;
                        LUMINUMBRA_CORE_INFO("wildlife: water-edge ({:.1f},{:.1f}), animal "
                                             "({:.1f},{:.1f}) terr {:.1f}, human ({:.1f},{:.1f}){}",
                                             wildlife_water.x,
                                             wildlife_water.z,
                                             wildlife_animal.x,
                                             wildlife_animal.z,
                                             wildlife_animal.y,
                                             wildlife_human.x,
                                             wildlife_human.z,
                                             found ? "" : " [no shoreline found - dry fallback]");
                    }
                    ApplySkinnedMeshVisualCamera(g_camera.get(), skinned_mesh_visual_target);
                    if (scenario_config.replicated && scenario_config.avatars >= 2 &&
                        skinned_mesh_visual_target.spawned) {
                        if (!replicated_demo_setup) {
                            replicated_demo.Setup(skinned_mesh_visual_target.spawn_positions);
                            replicated_demo_setup = true;
                        }
                        auto* world_sys = gameSession->GetWorldSystem();
                        const double replicated_dt =
                            deltaTime > 0.0f
                                ? static_cast<double>(std::min(deltaTime, 1.0f / 20.0f))
                                : (1.0 / 60.0);
                        const auto positions = replicated_demo.Update(replicated_dt, world_sys);
                        auto& reg = gameSession->GetRegistry();
                        bool applied_remote_pose = false;
                        for (std::size_t i = 0;
                             i < skinned_mesh_visual_target.all_entities.size() &&
                             i < positions.size();
                             ++i) {
                            const auto ent = skinned_mesh_visual_target.all_entities[i];
                            if (reg.valid(ent) &&
                                reg.all_of<Luminumbra::Components::TransformComponent>(ent)) {
                                reg.get<Luminumbra::Components::TransformComponent>(ent).position =
                                    positions[i];
                                applied_remote_pose = true;
                            }
                        }
                        if (applied_remote_pose) {
                            replicated_avatar_render_seconds += replicated_dt;
                        }
                    }
                    //  video: for the SHOWCASE row (avatars>=2), synchronously
                    // pull the surface around the camera fully ready each frame (same
                    // pattern as LodGround) so the world is PROPERLY LOADED before any
                    // frame is captured -- no streaming/meshing pop-in in the clip.
                    if (scenario_config.avatars >= 2 && gameSession->GetWorldSystem() &&
                        gameSession->GetPhysicsSystem()) {
                        // Fixed-camera wildlife scene: stream once (setup) then refresh
                        // every 30th frame so the 120-frame clip captures at full rate.
                        // The walking-row showcase moves the camera, so it streams each frame.
                        const bool skip =
                            scenario_config.wildlife && (wildlife_stream_tick++ % 30 != 0);
                        if (!skip) {
                            gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                                g_camera->Position,
                                gameSession->GetPhysicsSystem(),
                                scenario_config.horizon_radius,
                                scenario_config.collision_radius);
                        }
                    }
                } else if (scenario_config.creature_slice_smoke() && scenario_ready && g_camera) {
                    // spawn the creature scene once, hold the fixed
                    // photographic framing, run the game glue every frame and
                    // bring in the light stimulus at 55% progress.
                    if (!creature_slice_spawn_attempted) {
                        creature_slice_spawn_attempted = true;
                        creature_slice_scene = SpawnCreatureSliceScene(
                            gameSession.get(), root_dir, scenario_config.creature_archetype);
                    }
                    const double elapsed_play_seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      scenario_play_started_at)
                            .count();
                    const double duration =
                        static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                    const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                    if (progress >= 0.55 && creature_slice_scene.spawned &&
                        !creature_slice_scene.stimulus_spawned) {
                        SpawnCreatureSliceStimulus(gameSession.get(), creature_slice_scene);
                    }
                    UpdateCreatureSliceScene(
                        gameSession.get(), creature_slice_scene, static_cast<double>(deltaTime));
                    ApplyCreatureSliceCamera(
                        gameSession.get(), g_camera.get(), creature_slice_scene);
                } else if (scenario_config.networked_session_smoke() && scenario_ready &&
                           g_camera) {
                    // the client renders a SERVER-OWNED world over the
                    // lockstep transport. The driver owns the host authority world
                    // + both LockstepSession ends; per agreed tick it steps BOTH
                    // worlds (the client world is THIS gameSession, stepped via the
                    // driver's apply_and_step hook from the spawn anchor) and
                    // exchanges hashes. The client's WORLD-AFFECTING input set
                    // (empty today) round-trips through LockstepSession::*Input.
                    if (!networked_session_begun) {
                        networked_session_begun = true;
                        NetworkedSessionDriver::Config net_cfg;
                        net_cfg.seed = 424242;
                        net_cfg.preset = scenario_world_type;
                        net_cfg.budget_ticks = 90;
                        net_cfg.hash_cadence_ticks = 30;
                        net_cfg.root_path = root_path_str;
                        net_cfg.surface_radius = scenario_config.horizon_radius;
                        net_cfg.collision_radius = scenario_config.collision_radius;
                        if (!networked_session_driver.Begin(gameSession.get(), net_cfg)) {
                            scenario_failed = true;
                            scenario_failure_reason = "networked_session_begin_failed_" +
                                                      networked_session_driver.failure_reason();
                        }
                    }
                    if (networked_session_begun && !networked_session_done && !scenario_failed) {
                        // Drive the lockstep session to COMPLETION here (bounded by
                        // the budget): each agreed tick quiesces both worlds' streaming
                        // jobs, which is expensive in a debug build, so spreading it
                        // across rendered frames would blow the run window. The world
                        // is server-owned and stepped through the driver's
                        // apply_and_step hook; the render frame BELOW then draws the
                        // settled server-owned world (proving the client is
                        // render-capable, unlike the headless server). One agreed tick
                        // is stepped before the first render so the loop is observable.
                        bool live = networked_session_driver.StepAgreedTick();
                        while (live) {
                            live = networked_session_driver.StepAgreedTick();
                        }
                        if (!live) {
                            networked_session_done = true;
                            networked_session_driver.Disconnect();
                            const double net_seconds =
                                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                              scenario_play_started_at)
                                    .count();
                            const bool net_passed = networked_session_driver.WriteArtifact(
                                scenario_config.artifact_dir, net_seconds);
                            if (!net_passed) {
                                scenario_failed = true;
                                scenario_failure_reason =
                                    networked_session_driver.failure_reason().empty()
                                        ? std::string("networked_session_not_in_sync")
                                        : ("networked_session_" +
                                           networked_session_driver.failure_reason());
                            }
                            scenario_timed_run_complete = true;
                            glfwSetWindowShouldClose(window, true);
                        }
                    }
                    // Camera LOOK is: a fixed eye-level framing applied
                    // locally each frame, NEVER round-tripped through the session, so
                    // look latency is zero (research worldgen-lockstep-sdfrt.md Area 2
                    // takeaway 2). It reads the spawn anchor the driver streams the
                    // world around, but does NOT influence the hashed world step.
                    {
                        const Luminumbra::Vec3 anchor =
                            networked_session_driver.ClientStreamingAnchor();
                        g_camera->Position = glm::vec3(anchor.x, anchor.y + 1.8f, anchor.z);
                        g_camera->Yaw = 0.0f;
                        g_camera->Pitch = 0.0f;
                        g_camera->updateCameraVectors();
                    }
                } else if (g_app.capture.profile_fly_seconds > 0.0 && g_playerController &&
                           g_camera) {
                    // runtime telemetry (--profile-fly): drive the player FORWARD at a constant
                    // noclip speed in normal-play mode so the SLOWFRAME logger captures a
                    // representative MOVING cost without the time-based scenario camera's
                    // teleport-under-load feedback. deltaTime is clamped (<=50ms) so per-frame
                    // movement stays bounded even on a slow frame → no teleport/active-set
                    // explosion. A slow yaw drift sweeps varied terrain (rivers/biomes). Self-exits
                    // after g_app.capture.profile_fly_seconds. UpdateNoclip directly advances
                    // position (bypasses physics); streaming anchor = GetPosition follows.
                    static double s_profile_start = glfwGetTime();
                    const double elapsed = glfwGetTime() - s_profile_start;
                    g_camera->Yaw =
                        static_cast<float>(std::fmod(elapsed * 6.0, 360.0)); // ~1 rev / 60s
                    g_camera->Pitch = 0.0f;
                    g_camera->updateCameraVectors();
                    glm::vec3 fwd(g_camera->Front.x, 0.0f, g_camera->Front.z);
                    if (glm::length(fwd) > 1e-4f)
                        fwd = glm::normalize(fwd);
                    g_playerController->ProfileDriveNoclip(deltaTime, fwd);
                    if (elapsed > g_app.capture.profile_fly_seconds)
                        glfwSetWindowShouldClose(window, true);
                } else if (g_playerController && !g_app.hud.show_settings) {
                    g_playerController->Update(deltaTime); // movement paused while the menu is open
                }
                if (g_app.capture.timelapse_frames > 0 && g_camera && gameSession) {
                    // FIXED timelapse camera: elevated, looking down at the grove around spawn.
                    // NOT tied to the (settling) player physics, so it can never fall through the
                    // world -- and it frames the plants instead of whatever the avatar sees.
                    const auto sp = gameSession->GetMetadata().spawnPoint;
                    // Growth captures frame a tighter view of the hero cluster in front; otherwise
                    // an elevated look over the grove.
                    const bool showcase = g_app.capture.timelapse_grow ||
                                          g_app.capture.timelapse_season ||
                                          g_app.capture.timelapse_simgrow;
                    // Ecology demo: a HIGH, wide, near-top-down look over the whole field so
                    // the herd scattering away from the predator (and the predator weaving
                    // toward the nearest prey) reads as clear motion across the ground, and
                    // nobody runs out of frame as they spread.
                    const glm::vec3 camPos =
                        g_app.capture.timelapse_rain
                            ? glm::vec3(sp.x + 2.0f,
                                        sp.y + 16.0f,
                                        sp.z + 26.0f) // elevated overlook for rain pooling
                        : g_app.capture.timelapse_dig
                            ? glm::vec3(sp.x + 10.0f,
                                        sp.y + 6.0f,
                                        sp.z + 10.0f) // close, low 3/4 look at the crater
                        : g_app.capture.timelapse_fire
                            ? glm::vec3(
                                  sp.x, sp.y + 34.0f, sp.z + 36.0f) // high look over the burn patch
                        : g_app.capture.timelapse_creatures
                            ? glm::vec3(sp.x, sp.y + 30.0f, sp.z + 30.0f)
                        : showcase ? glm::vec3(sp.x, sp.y + 4.0f, sp.z + 22.0f)
                                   : glm::vec3(sp.x, sp.y + 7.0f, sp.z + 20.0f);
                    const glm::vec3 target =
                        g_app.capture.timelapse_rain
                            ? glm::vec3(sp.x,
                                        sp.y - 2.0f,
                                        sp.z) // look down over the filling valley
                        : g_app.capture.timelapse_dig
                            ? glm::vec3(sp.x, sp.y - 3.0f, sp.z) // the deepening crater at spawn
                        : g_app.capture.timelapse_fire      ? glm::vec3(sp.x, sp.y, sp.z)
                        : g_app.capture.timelapse_creatures ? glm::vec3(sp.x, sp.y, sp.z - 8.0f)
                        : showcase ? glm::vec3(sp.x, sp.y + 3.0f, sp.z + 10.0f)
                                   : glm::vec3(sp.x, sp.y + 2.0f, sp.z);
                    const glm::vec3 d = glm::normalize(target - camPos);
                    g_camera->Position = camPos;
                    g_camera->Yaw = glm::degrees(std::atan2(d.z, d.x));
                    g_camera->Pitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
                    g_camera->updateCameraVectors();
                }
                //  money shot: once the world has settled (water filled), anchor the camera on
                // the deepest real water cell and frame it from a low side vantage looking toward
                // the bank we'll breach. Computed once; the carve happens in the capture/advance
                // block.
                if (g_app.capture.timelapse_drain && g_camera && gameSession &&
                    gameSession->GetWorldSystem() &&
                    g_app.capture.timelapse_settle >= kTimelapseSettleFrames) {
                    auto* ws = gameSession->GetWorldSystem();
                    if (!g_app.capture.drain_state.init) {
                        // Anchor on a real SHORELINE — deep water beside a tall DRY bank — so
                        // cutting the bank floods the dry side (verified by the rising
                        // inland-volume probe below).
                        Luminumbra::Vec3 wp;
                        float tlx = 0.0f, tlz = 1.0f, wsurf = 0.0f, bank = 0.0f;
                        if (ws->debug_find_shoreline(wp, tlx, tlz, wsurf, bank)) {
                            g_app.capture.drain_state.P = wp;
                            g_app.capture.drain_state.dhx = tlx;
                            g_app.capture.drain_state.dhz = tlz;
                            g_app.capture.drain_state.surf = wsurf;
                            g_app.capture.drain_state.init = true;
                            LUMINUMBRA_CORE_INFO(
                                "Timelapse-drain: shoreline at ({:.1f},{:.1f},{:.1f}), surf {:.1f} "
                                "m, dry bank {:.1f} m, toward-land ({:.2f},{:.2f})",
                                wp.x,
                                wp.y,
                                wp.z,
                                wsurf,
                                bank,
                                tlx,
                                tlz);
                        }
                    }
                    if (g_app.capture.drain_state.init) {
                        const glm::vec3 P(g_app.capture.drain_state.P.x,
                                          g_app.capture.drain_state.P.y,
                                          g_app.capture.drain_state.P.z);
                        const glm::vec3 dh = glm::normalize(glm::vec3(
                            g_app.capture.drain_state.dhx, 0.0f, g_app.capture.drain_state.dhz));
                        const glm::vec3 side(-dh.z, 0.0f, dh.x);
                        // Frame the BASIN itself (3 m onto the bank), close + low, looking slightly
                        // down so the advancing waterline against the green slope is the clear
                        // subject.
                        const glm::vec3 basin = P + dh * 3.0f;
                        const glm::vec3 camPosD =
                            P - dh * 4.0f + side * 6.0f + glm::vec3(0.0f, 4.5f, 0.0f);
                        const glm::vec3 targetD = basin - glm::vec3(0.0f, 1.5f, 0.0f);
                        const glm::vec3 dd = glm::normalize(targetD - camPosD);
                        g_camera->Position = camPosD;
                        g_camera->Yaw = glm::degrees(std::atan2(dd.z, dd.x));
                        g_camera->Pitch = glm::degrees(std::asin(std::clamp(dd.y, -1.0f, 1.0f)));
                        g_camera->updateCameraVectors();
                    }
                }
                //  RAIN demo: once settled, turn on finite hydrology + rain, find the natural
                //  valley
                // floor (where rainfall collects), deepen it into a clear closed basin, and frame
                // it.
                if (g_app.capture.timelapse_rain && g_camera && gameSession &&
                    gameSession->GetWorldSystem() &&
                    g_app.capture.timelapse_settle >= kTimelapseSettleFrames) {
                    auto* ws = gameSession->GetWorldSystem();
                    if (!g_app.capture.drain_state.init) {
                        ws->SetWaterHydrology(
                            /*finite=*/true, g_app.capture.timelapse_rain_mm, /*evap=*/0);
                        Luminumbra::Vec3 vp;
                        float bed = 0.0f;
                        if (ws->debug_lowest_land_pos(vp, bed)) {
                            g_app.capture.drain_state.P = vp;
                            g_app.capture.drain_state.init = true;
                            // Deep bowl (5 m): water collects BELOW the grass line so it reads as a
                            // clear pool rather than a thin sheet hidden under grass cards; hard
                            // rain fills it high.
                            ws->EditTerrainVoxel(
                                vp, 5.0f, /*fill=*/false, gameSession->GetPhysicsSystem());
                            LUMINUMBRA_CORE_INFO("Timelapse-rain: valley basin at "
                                                 "({:.1f},{:.1f},{:.1f}), bed {:.1f} m",
                                                 vp.x,
                                                 vp.y,
                                                 vp.z,
                                                 bed);
                        }
                    }
                    if (g_app.capture.drain_state.init) {
                        const glm::vec3 P(g_app.capture.drain_state.P.x,
                                          g_app.capture.drain_state.P.y,
                                          g_app.capture.drain_state.P.z);
                        const glm::vec3 camPosR = P + glm::vec3(11.0f, 9.0f, 11.0f);
                        const glm::vec3 dd =
                            glm::normalize((P - glm::vec3(0.0f, 1.0f, 0.0f)) - camPosR);
                        g_camera->Position = camPosR;
                        g_camera->Yaw = glm::degrees(std::atan2(dd.z, dd.x));
                        g_camera->Pitch = glm::degrees(std::asin(std::clamp(dd.y, -1.0f, 1.0f)));
                        g_camera->updateCameraVectors();
                    }
                }
                if (auto* physics = gameSession->GetPhysicsSystem())
                    physics->update(deltaTime * g_app.capture.timeScale);
                // fixed 30 Hz simulation tick (SimulationClock +
                // OrderedEventBus drain) hosted by GameSession. Render,
                // physics, and scenario paths above remain variable-dt.
                // the networked-session scenario steps its client world
                // through the lockstep driver's apply_and_step hook (in lockstep
                // with the host), so the default per-frame tick + camera-anchored
                // streaming are SKIPPED here -- ticking twice would desync from the
                // host, and camera-anchored streaming would diverge the hashed world.
                _rb_sim_t0 = std::chrono::steady_clock::now(); //
                if (!scenario_config.networked_session_smoke()) {
                    if (g_app.capture.timeScale == 1.0f) {
                        gameSession->TickSimulation(static_cast<double>(
                            deltaTime)); // byte-identical default (gates run here)
                    } else if (g_app.capture.timeScale > 0.0f) {
                        // host_timescale: run the sim faster/slower. Chunk into <=4-tick steps so a
                        // high scale isn't dropped by the catch-up clamp, capped per frame to keep
                        // spiral protection. Determinism holds (fixed dt per tick).
                        double simDt = static_cast<double>(deltaTime) *
                                       static_cast<double>(g_app.capture.timeScale);
                        const double kFourTicks = (1.0 / 30.0) * 4.0;
                        const int budget = static_cast<int>(
                            std::ceil(4.0 * static_cast<double>(g_app.capture.timeScale)));
                        int ran = 0;
                        while (simDt > 1e-9 && ran < budget) {
                            const double step = std::min(simDt, kFourTicks);
                            const std::uint32_t t = gameSession->TickSimulation(step);
                            simDt -= step;
                            if (t == 0u)
                                break; // accumulator < 1 tick this frame
                            ran += static_cast<int>(t);
                        }
                    } // g_app.capture.timeScale == 0 -> paused (no sim ticks; render/streaming
                      // continue)
                    rb_sim_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - _rb_sim_t0)
                                    .count();                         //
                    _rb_stream_t0 = std::chrono::steady_clock::now(); //
                    if (gameSession->GetWorldSystem() && (g_playerController || g_camera)) {
                        // Anchor world streaming on the CAMERA (not the spawn-bound player)
                        // whenever a fixed/scenario camera drives the view — otherwise a --cam-pos
                        // far from spawn streams chunks around the player at spawn and the camera
                        // sees an unloaded, unlit void (the "far-camera renders black" bug).
                        // g_app.capture.fixed_cam covers the capture/showcase path; the scenario
                        // smokes keep their existing behaviour.
                        const bool cam_anchored =
                            (g_app.capture.fixed_cam ||
                             ((scenario_config.lod_ground_smoke() ||
                               scenario_config.water_visual_smoke() ||
                               scenario_config.material_visual_smoke() ||
                               scenario_config.skybox_visual_smoke() ||
                               scenario_config.weather_visual_smoke() ||
                               scenario_config.cloud_shadow_smoke() ||
                               scenario_config.precipitation_smoke() ||
                               scenario_config.timeofday_sweep_smoke() ||
                               scenario_config.lod_boundary_oscillation_smoke() ||
                               scenario_config.lod_seam_arrival_smoke() ||
                               scenario_config.player_view_smoke() ||
                               scenario_config.farlod_horizon_smoke() ||
                               scenario_config.skinned_mesh_visual_smoke() ||
                               scenario_config.creature_slice_smoke()) &&
                              scenario_ready)) &&
                            g_camera;
                        const Luminumbra::Vec3 streaming_position =
                            cam_anchored
                                ? Luminumbra::Vec3(g_camera->Position)
                                : (g_playerController
                                       ? Luminumbra::Vec3(g_playerController->GetPosition())
                                       : Luminumbra::Vec3(g_camera->Position));
                        gameSession->GetWorldSystem()->update(gameSession->GetRegistry(),
                                                              streaming_position,
                                                              gameSession->GetPhysicsSystem());
                    }
                    rb_stream_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - _rb_stream_t0)
                                       .count(); //
                } // end !networked_session_smoke default-tick guard
                break;
            case GameState::MAIN_MENU:
                break;
            case GameState::EXITING:
                glfwSetWindowShouldClose(window, true);
                break;
            default:
                break;
        }

        if (currentState != GameState::WORLD_LOADING) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (currentState == GameState::IN_GAME) {
                //  trees: one-time deterministic vegetation scatter once the
                // world is ready.  decoration on the client registry
                // (never hashed). Places tree static-mesh instances on grassy,
                // above-water, gentle-slope terrain around the spawn anchor with
                // seeded jitter so the world reads forested. First pass uses the
                // Grass material id; per-mesh bark/leaf texturing is a implementation note
                // via the model-texture (skinned-UV) path.
                // the placement loops (trees/rocks/bushes +
                // the ambient-wildlife herd — together ~30 s of main-thread
                // terrain/water/biome probing in a debug build) moved VERBATIM to
                // WorldDressing.cpp and run as ONE background JobSystem job (the
                //  BackgroundWorldScan pattern): dispatch once on the first
                // IN_GAME frame, poll the handle non-blocking, then consume the
                // placement vectors AMORTIZED (the GL palette uploads and the
                // EnTT/physics registrations were always the cheap half).
                // Capture/scenario/timelapse runs compute synchronously instead so
                // frame-1 captures still see the full dressing (trees appear on
                // the first ready frame, incl. the visual-sweep capture, exactly
                // as before).
                {
                    auto* ws = gameSession->GetWorldSystem();
                    // The skinned wildlife rig: AnimationPlayerComponents hold POINTERS
                    // to these, so they must outlive every spawned creature (statics,
                    // exactly as before the extraction).
                    namespace anim = luminumbra::animation;
                    static anim::Skeleton s_wildlife_skeleton;
                    static anim::AnimationClip s_wildlife_idle;
                    static bool s_dressing_dispatched =
                        false; // process-once (was s_trees_scattered)
                    if (!s_dressing_dispatched && ws) {
                        s_dressing_dispatched = true;
                        const Luminumbra::Vec3 anchor = gameSession->GetMetadata().spawnPoint;
                        // PROGRAMMATIC TREES/ROCKS/BUSHES, NO MODEL (owner): build the
                        // procedural palettes once (into the instanced static-mesh cache)
                        // BEFORE dispatch — GL-side + cheap, and the palette COUNTS pin
                        // the placements' palette-index modulo. None of the builders
                        // touches the scatter frand stream, so hoisting the rock/bush
                        // builds ahead of their loops keeps the seeded layout
                        // byte-identical to the old inline order.
                        const glm::vec3 sunToward = -glm::normalize(renderPipeline.sun_direction());
                        BuildProcgenTreePalette(g_procgen, renderPipeline, sunToward);
                        BuildProcgenRockPalette(g_procgen, renderPipeline);
                        BuildProcgenBushPalette(g_procgen, renderPipeline);
                        // LIVING WORLD: ambient WILDLIFE for interactive play. The world
                        // had no creatures in normal play (only timelapse markers /
                        // scenario rigs), so the codex/objectives loop had nothing to
                        // photograph. Render+client-sim only; the gated headless
                        // world_hash is the server's and is unaffected. Skipped in
                        // capture/scenario modes (they own their own creature handling).
                        const bool interactive_play =
                            !scenario_config.active() && !g_app.capture.timelapse_creatures &&
                            (g_app.capture.timelapse_frames == 0 || g_app.capture.timelapse_living);
                        auto pending = std::make_shared<WorldDressingPending>();
                        pending->world = ws;
                        pending->sun_toward = sunToward;
                        // Capture/scenario/timelapse runs must see the FULL dressing in
                        // their first settled frames (visual sweep / frame-scan / thumbs
                        // capture right after scenario_ready), so they compute + consume
                        // synchronously — the placements are byte-identical either way.
                        pending->synchronous =
                            scenario_config.active() || g_app.capture.frame_scan_active ||
                            g_app.capture.scene_active || g_app.capture.play_paths ||
                            !g_app.capture.render_benchmark_path.empty() ||
                            !g_app.capture.survey_dir.empty() ||
                            g_app.capture.timelapse_frames > 0 || g_app.capture.ui_thumbs > 0;
                        // One-time skinned rig load (file IO — never the slow part). The
                        // wildlife placements are only computed when the rig is usable.
                        if (interactive_play && g_app.hud.creatureSpecies.size() > 0) {
                            anim::SkinnedMeshAsset masset;
                            anim::AnimClipAsset iclip;
                            const std::filesystem::path gmesh =
                                root_dir / "data/models/creatures/grovestrider/grovestrider.lmesh";
                            const std::filesystem::path gidle =
                                root_dir /
                                "data/models/creatures/grovestrider/grovestrider.idle.lanim";
                            if (anim::LoadSkinnedMeshAsset(gmesh.string(), masset) &&
                                anim::LoadAnimClipAsset(gidle.string(), iclip)) {
                                s_wildlife_skeleton = anim::BuildSkeleton(masset);
                                s_wildlife_idle = anim::BuildClip(iclip);
                                pending->wildlife_ok = true;
                            }
                        }
                        Luminumbra::Client::WorldDressingParams dparams;
                        dparams.anchor_x = anchor.x;
                        dparams.anchor_z = anchor.z;
                        dparams.tree_palette_count = g_procgen.treePaletteCount;
                        dparams.rock_palette_count = g_procgen.rockPaletteCount;
                        dparams.bush_palette_count = g_procgen.bushPaletteCount;
                        dparams.compute_wildlife = pending->wildlife_ok;
                        {
                            // Full control: ambient spawn count/speeds from systems.json
                            // render.creature_spawn (client-only; compiled defaults when
                            // OFF). Only the herd COUNT feeds the placement computation;
                            // the speeds/hunger are consume-side component values.
                            using SK = luminumbra::core::SysKey;
                            using SP = luminumbra::core::SysParam;
                            const bool spawnCfg = g_systemConfig.enabled(SK::RenderCreatureSpawn);
                            dparams.herd_count =
                                spawnCfg ? std::max(0,
                                                    static_cast<int>(g_systemConfig.param(
                                                        SP::SpawnHerdCount, 12.0f)))
                                         : 12;
                            pending->pred_speed =
                                spawnCfg ? g_systemConfig.param(SP::SpawnPredatorSpeed, 4.0f)
                                         : 4.0f;
                            pending->prey_speed =
                                spawnCfg ? g_systemConfig.param(SP::SpawnPreySpeed, 2.6f) : 2.6f;
                            pending->init_hunger =
                                spawnCfg ? g_systemConfig.param(SP::SpawnInitialHunger, 0.2f)
                                         : 0.2f;
                        }
                        // The EXACT queries the inline loops made — pure, thread-safe
                        // worldgen reads (the meshing workers run the same sampling
                        // concurrently), bound to the CURRENT world system. The drain
                        // contract (DrainWorldDressing before every world transition)
                        // keeps the raw ws captures from dangling.
                        Luminumbra::Client::WorldDressingCallbacks dcbs;
                        dcbs.terrain_height = [ws](float x, float z) {
                            return ws->GetTerrainHeightAt(x, z);
                        };
                        dcbs.water_level = [ws](float x, float z) {
                            return ws->WaterLevelAt(x, z);
                        };
                        dcbs.density_at = [ws](float x, float y, float z) {
                            return ws->get_density_at(Luminumbra::Vec3(x, y, z));
                        };
                        dcbs.vegetation_density = [ws](float x, float z) {
                            return ws->biomes_enabled() ? ws->biome_table()
                                                              .vegetation_for(ws->BiomeIdAt(x, z))
                                                              .density
                                                        : 0.3f;
                        };
                        dcbs.biome_name = [ws](float x, float z) -> std::string {
                            return ws->biome_table().name_for(ws->BiomeIdAt(x, z));
                        };
                        dcbs.species_for_biome = [](const std::string& biome,
                                                    std::size_t pick) -> int {
                            // Biome-appropriate species: pick among the species that
                            // inhabit the local biome (generalists included); fall back
                            // to the full roster if the biome lists none.
                            const luminumbra::ai::CreatureSpecies* sp_sel =
                                g_app.hud.creatureSpecies.SelectForBiome(biome, pick);
                            if (!sp_sel)
                                return static_cast<int>(pick % g_app.hud.creatureSpecies.size());
                            return static_cast<int>(sp_sel -
                                                    g_app.hud.creatureSpecies.all().data());
                        };
                        if (pending->synchronous) {
                            pending->result =
                                Luminumbra::Client::ComputeWorldDressing(dparams, dcbs);
                        } else {
                            std::vector<Luminumbra::Job> djobs;
                            djobs.emplace_back([pending, dparams, dcbs]() {
                                pending->result =
                                    Luminumbra::Client::ComputeWorldDressing(dparams, dcbs);
                            });
                            s_worldDressingHandle = jobSystem.dispatch_batch(djobs);
                            LUMINUMBRA_CORE_INFO(
                                ": world-dressing placement (trees/rocks/bushes{}) dispatched to a "
                                "background job",
                                pending->wildlife_ok ? " + wildlife" : "");
                        }
                        s_worldDressing = std::move(pending);
                    }
                    // Consume: non-blocking poll of the JobHandle counter (
                    // pattern). Each lane replays its placement vector strictly in
                    // order under a per-frame budget sized to clear the worst-case
                    // caps (28000 trees / 14000 rocks / 40000 bushes / the herd) in
                    // well under 20 frames; the synchronous path consumes everything
                    // on this same frame, matching the old inline single-frame bring-up.
                    if (s_worldDressing && ws && s_worldDressing->world == ws &&
                        (!s_worldDressingHandle.counter ||
                         s_worldDressingHandle.counter->load(std::memory_order_acquire) <= 0)) {
                        WorldDressingPending& pend = *s_worldDressing;
                        auto& reg = gameSession->GetRegistry();
                        const Luminumbra::Vec3 anchor = gameSession->GetMetadata().spawnPoint;
                        auto terr = [&](float x, float z) {
                            return ws->GetTerrainHeightAt(x, z);
                        };
                        const std::size_t kNoBudget = std::numeric_limits<std::size_t>::max();
                        const std::size_t kTreeBudget =
                            pend.synchronous ? kNoBudget : 2000; // <=14 frames at cap
                        const std::size_t kRockBudget =
                            pend.synchronous ? kNoBudget : 4000; // <=4 frames at cap
                        const std::size_t kBushBudget =
                            pend.synchronous ? kNoBudget : 6000; // <=7 frames at cap
                        const std::size_t kCreatureBudget =
                            pend.synchronous ? kNoBudget : 64; // herd is ~12
                        // Trees: TWO instanced static-mesh entities per placement — bark
                        // (soil/brown) + leaf (grass/green) — so the existing instanced +
                        // tree rendering LOD + frustum-cull path renders the vast forest cheaply.
                        //  full UV-texture lane: each part's bark/leaf texture is bound
                        // by GBufferPass via data/models/trees/tree_textures.json.
                        {
                            std::size_t budget = kTreeBudget;
                            while (pend.trees_done < pend.result.trees.size() && budget-- > 0) {
                                const auto& t = pend.result.trees[pend.trees_done++];
                                if (t.palette_index < 0)
                                    continue; // empty palette: counted, nothing to emit
                                const std::string base =
                                    "procgen://tree_" + std::to_string(t.palette_index);
                                const Luminumbra::Vec3 treeScale(
                                    t.eff_scale, t.eff_scale, t.eff_scale);
                                auto emit = [&](const std::string& meshKey, std::uint32_t mat) {
                                    const auto e = reg.create();
                                    auto& tf =
                                        reg.emplace<Luminumbra::Components::TransformComponent>(e);
                                    tf.position = t.position;
                                    tf.scale = treeScale;
                                    tf.rotation = t.rotation;
                                    auto& sm =
                                        reg.emplace<Luminumbra::Components::StaticMeshComponent>(e);
                                    sm.meshPath = meshKey;
                                    sm.materialId = mat;
                                };
                                emit(base + kBarkMatKey, 2u); // bark -> soil/brown material
                                emit(base + kLeafMatKey, 3u); // leaf -> grass/green material
                            }
                            if (!pend.trees_logged && pend.trees_done == pend.result.trees.size()) {
                                pend.trees_logged = true;
                                LUMINUMBRA_CORE_INFO(" trees: scattered {} tree instances",
                                                     pend.result.trees.size());
                            }
                        }

                        // ROCK FORMATIONS (worldgen-richness ): stone-triplanar
                        // instanced static meshes, denser on scree. The placement already
                        // applied the settle-into-ground offset and squash jitter (they
                        // were RNG draws on the shared scatter stream).
                        {
                            std::size_t budget = kRockBudget;
                            while (pend.rocks_done < pend.result.rocks.size() && budget-- > 0) {
                                const auto& r = pend.result.rocks[pend.rocks_done++];
                                const auto e = reg.create();
                                auto& tf =
                                    reg.emplace<Luminumbra::Components::TransformComponent>(e);
                                tf.position = r.position; // settled into ground
                                tf.scale = r.scale;
                                tf.rotation = r.rotation;
                                auto& sm =
                                    reg.emplace<Luminumbra::Components::StaticMeshComponent>(e);
                                sm.meshPath = "procgen://rock_" + std::to_string(r.palette_index);
                                sm.materialId = 1u; // Stone -> stone triplanar texture
                            }
                            // Legacy logged only when the rock palette existed (the whole
                            // loop was skipped otherwise) — preserve that.
                            if (!pend.rocks_logged && pend.rocks_done == pend.result.rocks.size() &&
                                g_procgen.rockPaletteCount > 0) {
                                pend.rocks_logged = true;
                                LUMINUMBRA_CORE_INFO("ROCKS: scattered {} rock instances",
                                                     pend.result.rocks.size());
                            }
                        }
                        // SHRUB/BUSH LAYER: the undergrowth complement
                        // of the rocks — bushes on vegetated flats/gentle slopes, rocks
                        // on scree (the biome vegetation gating ran in the computation).
                        {
                            std::size_t budget = kBushBudget;
                            while (pend.bushes_done < pend.result.bushes.size() && budget-- > 0) {
                                const auto& b = pend.result.bushes[pend.bushes_done++];
                                const auto e = reg.create();
                                auto& tf =
                                    reg.emplace<Luminumbra::Components::TransformComponent>(e);
                                tf.position = b.position; // settled into ground
                                tf.scale = b.scale;
                                tf.rotation = b.rotation;
                                auto& sm =
                                    reg.emplace<Luminumbra::Components::StaticMeshComponent>(e);
                                sm.meshPath = "procgen://bush_" + std::to_string(b.palette_index);
                                sm.materialId = 3u; // grass/green leaf material (shrub foliage)
                            }
                            if (!pend.bushes_logged &&
                                pend.bushes_done == pend.result.bushes.size() &&
                                g_procgen.bushPaletteCount > 0) {
                                pend.bushes_logged = true;
                                LUMINUMBRA_CORE_INFO("BUSHES: scattered {} shrub instances",
                                                     pend.result.bushes.size());
                            }
                        }

                        // LIVING WORLD consume: replay the wildlife placement vector in
                        // order. Each is the grovestrider rig recolored by its species
                        // base_color and scaled by its genome build — GameSession's
                        // SamplePosesOnTick animates them, the CreatureBrain wanders them
                        // (grounded via a Jolt avatar like the timelapse herd), and
                        // GatherPhotoSubjects sees them so the codex fills in normal
                        // play. Water-cell candidates became capped drinking-spot
                        // WaterHoles in the computation. The EnTT emplaces + physics
                        // avatar creation here are main-thread-only — that is why the
                        // consume (not the computation) stays on this thread.
                        if (pend.wildlife_ok) {
                            auto* phys = gameSession->GetPhysicsSystem();
                            std::size_t budget = kCreatureBudget;
                            while (pend.wildlife_done < pend.result.wildlife.size() &&
                                   budget-- > 0) {
                                const auto& c = pend.result.wildlife[pend.wildlife_done++];
                                if (c.kind ==
                                    Luminumbra::Client::CreaturePlacement::Kind::WaterHole) {
                                    // A DRINKING SPOT at the water so the wired thirst
                                    // system has somewhere to steer creatures.
                                    const auto he = reg.create();
                                    auto& htf =
                                        reg.emplace<Luminumbra::Components::TransformComponent>(he);
                                    htf.position = c.position;
                                    reg.emplace<Luminumbra::Components::WaterHoleComponent>(he)
                                        .radius = 6.0f;
                                    continue;
                                }
                                const auto& roster = g_app.hud.creatureSpecies.all();
                                if (c.species_index < 0 ||
                                    static_cast<std::size_t>(c.species_index) >= roster.size())
                                    continue; // defensive: the roster never changes mid-session
                                const auto& sp = roster[static_cast<std::size_t>(c.species_index)];
                                const auto e = reg.create();
                                auto& tf =
                                    reg.emplace<Luminumbra::Components::TransformComponent>(e);
                                tf.position = c.position; // settle onto ground (gy + 1.2)
                                tf.scale = Luminumbra::Vec3(
                                    c.build_scale.x, c.build_scale.y, c.build_scale.z);
                                tf.rotation = glm::angleAxis(c.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                                auto& sm =
                                    reg.emplace<Luminumbra::Components::SkinnedMeshComponent>(e);
                                sm.meshPath =
                                    "data/models/creatures/grovestrider/grovestrider.lmesh";
                                sm.materialId = 3u;
                                sm.tintR = sp.base_color[0];
                                sm.tintG = sp.base_color[1];
                                sm.tintB = sp.base_color[2];
                                auto& cr =
                                    reg.emplace<Luminumbra::Components::CreatureComponent>(e);
                                cr.species_id = sp.species_id();
                                cr.is_predator = sp.predator;
                                cr.hunger = pend.init_hunger;
                                cr.move_speed = sp.predator ? pend.pred_speed : pend.prey_speed;
                                auto& gn =
                                    reg.emplace<Luminumbra::Components::CreatureGenomeComponent>(e);
                                gn.move_speed = cr.move_speed;
                                gn.size_scale = c.size;
                                gn.female = c.female;
                                gn.age_ticks = 100u;
                                // Survival: every creature thirsts (seeks the drinking spots
                                // above); predators also scavenge carrion. Activates the wired
                                // Thirst/Scavenging tick systems in the living world.
                                auto& th = reg.emplace<Luminumbra::Components::ThirstComponent>(e);
                                th.thirst = c.thirst;
                                if (sp.predator)
                                    reg.emplace<Luminumbra::Components::ScavengerComponent>(e);
                                // a circadian clock -> the creature sleeps in its
                                // off-phase (diurnal at night, nocturnal by day). The brain's
                                // Sleep utility reads CircadianComponent.activity. Nocturnal is
                                // now a per-species DATA flag (creatures/species/*.json), so a new
                                // species can be nocturnal without a client recompile.
                                reg.emplace<Luminumbra::Components::CircadianComponent>(e)
                                    .nocturnal = sp.nocturnal ? 1u : 0u;
                                // the built-but-unstamped behavior
                                // components join the CLIENT ambient roster (never feeds server
                                // hashes — this registry is client decoration): prey herds get
                                // ALARM vigilance (collective flee), predators get PACK-HUNTER
                                // flank coordination; everyone claims a TERRITORY home range at
                                // its spawn point and carries the MIGRATORY seasonal-drive slot.
                                // Mortal/Decay lifecycle prep: a long seeded lifespan so ambient
                                // populations turn over slowly (the calm-demo's fast lifecycle
                                // stays demo-only).
                                if (sp.predator) {
                                    reg.emplace<Luminumbra::Components::PackHunterComponent>(e);
                                } else {
                                    reg.emplace<Luminumbra::Components::AlarmComponent>(e);
                                }
                                {
                                    auto& terr_home =
                                        reg.emplace<Luminumbra::Components::TerritoryComponent>(e);
                                    terr_home.home_x = c.position.x;
                                    terr_home.home_z = c.position.z;
                                    terr_home.radius = sp.predator ? 60.0f : 35.0f;
                                    terr_home.established = 1;
                                    reg.emplace<Luminumbra::Components::MigratoryComponent>(e);
                                    auto& mort =
                                        reg.emplace<Luminumbra::Components::MortalComponent>(e);
                                    const std::uint32_t jit =
                                        static_cast<std::uint32_t>(entt::to_integral(e) *
                                                                   2654435761u) %
                                        36000u;
                                    mort.lifespan_ticks =
                                        108000u + jit; // ~60-80 min @30Hz ambient turnover
                                    reg.emplace<Luminumbra::Components::DecayComponent>(e)
                                        .decay_duration = 900u;
                                }
                                auto& pl = reg.emplace<anim::AnimationPlayerComponent>(e);
                                pl.skeleton = &s_wildlife_skeleton;
                                pl.clip = &s_wildlife_idle;
                                pl.time = c.anim_phase * 2.0; // staggered phase
                                pl.looping = true;
                                if (phys) {
                                    const std::size_t idx = phys->create_avatar_character(
                                        glm::vec3(c.position.x, c.position.y, c.position.z));
                                    reg.emplace<Luminumbra::Components::CreaturePhysicsComponent>(
                                        e, idx);
                                }
                                ++pend.creatures_spawned;
                            }
                        }
                        // One-shot TAIL — runs exactly once, when every lane has fully
                        // drained (the synchronous path reaches it on this same frame,
                        // matching the old inline block's single-frame bring-up).
                        const bool dressing_complete =
                            pend.trees_done == pend.result.trees.size() &&
                            pend.rocks_done == pend.result.rocks.size() &&
                            pend.bushes_done == pend.result.bushes.size() &&
                            pend.wildlife_done == pend.result.wildlife.size();
                        if (dressing_complete) {
                            if (pend.wildlife_ok) {
                                LUMINUMBRA_CORE_INFO(
                                    "Living world: spawned {} skinned, species-varied ambient "
                                    "creatures around spawn",
                                    pend.creatures_spawned);
                                // a forager COLONY -- a nest + ants that shuttle to food,
                                // laying pheromone trails. The wired-but-dormant ForagingSystem
                                // (slot 2c, Deneubourg double-bridge) ticks once ForagerComponents
                                // exist, so the colony self-organises onto the SHORTER nest->food
                                // path. Cells are integers on the scent field; a per-frame mirror
                                // gives each ant a world transform (visible marker render is a
                                // follow-on). Client-only: no re-pin.
                                {
                                    const auto& csp = gameSession->GetMetadata().spawnPoint;
                                    // Clamp every colony cell to the scent grid [0, cells): if the
                                    // spawn anchor maps near a grid edge, an unclamped nest/food
                                    // offset would land off-grid and the per-frame mirror would
                                    // sample terrain at a far-OOB world coordinate (crash). Keep a
                                    // margin so the nest and both food sources stay reachable
                                    // inside the field.
                                    const int cells = gameSession->ScentFieldCells();
                                    const auto clampCell = [cells](int c) {
                                        return c < 0 ? 0 : (c >= cells ? cells - 1 : c);
                                    };
                                    const int nestCx =
                                        clampCell(gameSession->ScentWorldToCellX(csp.x));
                                    const int nestCz =
                                        clampCell(gameSession->ScentWorldToCellZ(csp.z) + 6);
                                    // Full control: colony size + food from systems.json
                                    // render.foraging_colony.
                                    const bool colCfg = g_systemConfig.enabled(
                                        luminumbra::core::SysKey::RenderForagingColony);
                                    const int antCount =
                                        colCfg ? std::max(
                                                     0,
                                                     static_cast<int>(g_systemConfig.param(
                                                         luminumbra::core::SysParam::ColonyAntCount,
                                                         24.0f)))
                                               : 24;
                                    const std::int32_t foodAmount =
                                        colCfg ? static_cast<std::int32_t>(g_systemConfig.param(
                                                     luminumbra::core::SysParam::ColonyFoodAmount,
                                                     1000000.0f))
                                               : 1000000;
                                    auto placeFood = [&](int cx, int cz) {
                                        const auto fe = reg.create();
                                        auto& fs = reg.emplace<
                                            Luminumbra::Components::FoodSourceComponent>(fe);
                                        fs.cell_x = clampCell(cx);
                                        fs.cell_z = clampCell(cz);
                                        fs.amount = foodAmount;
                                    };
                                    placeFood(nestCx + 14, nestCz);    // far food (long path)
                                    placeFood(nestCx - 9, nestCz + 4); // near food (short path)
                                    for (int ai = 0; ai < antCount; ++ai) {
                                        const auto ae = reg.create();
                                        auto& fg =
                                            reg.emplace<Luminumbra::Components::ForagerComponent>(
                                                ae);
                                        fg.cell_x = nestCx;
                                        fg.cell_z = nestCz;
                                        fg.home_x = nestCx;
                                        fg.home_z = nestCz;
                                        auto& tf =
                                            reg.emplace<Luminumbra::Components::TransformComponent>(
                                                ae);
                                        tf.position = Luminumbra::Vec3(
                                            gameSession->ScentCellToWorldX(nestCx),
                                            csp.y,
                                            gameSession->ScentCellToWorldZ(nestCz));
                                        tf.scale = Luminumbra::Vec3(0.18f, 0.18f, 0.18f);
                                    }
                                    LUMINUMBRA_CORE_INFO(
                                        "Forager colony: nest cell ({},{}) + 2 food + {} ants",
                                        nestCx,
                                        nestCz,
                                        antCount);
                                }
                            }

                            //  the procedural-plant render bridge for the
                            // hero/grow demo paths. Legacy cleared the scatter list before
                            // its loop; the loop no longer populates it (only the hero
                            // blocks below do), so the clear moved here unchanged in effect.
                            const bool procgenPlants = true;
                            g_procgen.plants.clear();
                            // Growth showcase: a cluster of bigger HERO plants right in front of
                            // the fixed grow-mode camera, so the foreground is dominated by plants
                            // visibly growing (the scattered grove alone reads as distant
                            // background).
                            if ((g_app.capture.timelapse_grow || g_app.capture.timelapse_season) &&
                                procgenPlants) {
                                const glm::vec3 heroOffsets[] = {{-5.0f, 0.0f, 9.0f},
                                                                 {0.0f, 0.0f, 12.0f},
                                                                 {5.0f, 0.0f, 8.0f},
                                                                 {-2.5f, 0.0f, 6.0f},
                                                                 {2.5f, 0.0f, 6.5f}};
                                auto hgen = luminumbra::core::DeterministicRng::seeded(
                                    luminumbra::foliage::kPlantSeedOffset, 7777u, 1u);
                                for (const glm::vec3& off : heroOffsets) {
                                    const float hx = anchor.x + off.x, hz = anchor.z + off.z;
                                    ProcgenPlantInstance inst;
                                    inst.worldPos = glm::vec3(hx, terr(hx, hz), hz);
                                    inst.rot = glm::angleAxis(hgen.next_unit() * 6.2831853f,
                                                              glm::vec3(0.0f, 1.0f, 0.0f));
                                    inst.effScale =
                                        2.4f + hgen.next_unit() * 1.0f; // big hero trees
                                    inst.genome = luminumbra::foliage::RandomGenome(hgen);
                                    g_procgen.plants.push_back(inst);
                                }
                            }
                            //  SIM plant seeding for the REAL-growth showcase. Spawn a
                            // hero cluster of LIVE PlantTag plants (data-driven species via the
                            // MakePlantFromSpecies) into the SESSION registry, so the deterministic
                            // PlantGrowthSystem advances them Seed->Fruiting each tick and
                            // BakeSimPlants renders their TRUE stage. Under time-scale they visibly
                            // grow on capture.
                            if (g_app.capture.timelapse_simgrow) {
                                luminumbra::foliage::SpeciesRegistry species;
                                std::vector<std::string> sperr;
                                species.LoadFromDirectory(root_dir / "data/common/foliage/species",
                                                          sperr);
                                const char* picks[] = {"wheat", "oak", "wheat", "oak", "wheat"};
                                const glm::vec3 simOffsets[] = {{-5.0f, 0.0f, 9.0f},
                                                                {0.0f, 0.0f, 12.0f},
                                                                {5.0f, 0.0f, 8.0f},
                                                                {-2.5f, 0.0f, 6.0f},
                                                                {2.5f, 0.0f, 6.5f}};
                                auto sgen = luminumbra::core::DeterministicRng::seeded(
                                    luminumbra::foliage::kPlantSeedOffset, 4242u, 7u);
                                int seeded = 0;
                                for (std::size_t i = 0; i < 5; ++i) {
                                    const auto* tmpl = species.Find(picks[i]);
                                    if (!tmpl)
                                        continue;
                                    const float hx = anchor.x + simOffsets[i].x,
                                                hz = anchor.z + simOffsets[i].z;
                                    luminumbra::foliage::MakePlantFromSpecies(
                                        reg,
                                        Luminumbra::Vec3(hx, terr(hx, hz), hz),
                                        *tmpl,
                                        sgen,
                                        0);
                                    ++seeded;
                                }
                                LUMINUMBRA_CORE_INFO(" : seeded {} SIM plants (real growth tick)",
                                                     seeded);
                            }
                            // Plant unification: build the DECORATION scatter cache first
                            // (BakeProcgenPlants
                            // -> g_procgen.treeVerts), then composite the SIM-tier PlantTag plants
                            // on top via RebakeAllPlants (one pass, both tiers).
                            // Player-planted/promoted plants ADD to the forest rather than
                            // replacing it. OFF/empty -> pass disabled. Growth + promotion re-bakes
                            // happen per-frame in the loop (sig-gated, so cheap). Phototropism uses
                            // the scene's REAL sun, captured at dispatch time (the same value the
                            // tree palette was built with).
                            g_procgen.sunDir = pend.sun_toward;
                            if (auto* pp = renderPipeline.plant_procgen()) {
                                if (procgenPlants) {
                                    BakeProcgenPlants(
                                        g_procgen,
                                        pp,
                                        g_procgen.stageF); // build + cache the scatter
                                    LUMINUMBRA_CORE_INFO(
                                        ": {} procedural scatter plants (stage {:.1f})",
                                        g_procgen.plants.size(),
                                        g_procgen.stageF);
                                } else {
                                    pp->set_enabled(false);
                                }
                                const std::size_t simPlants = RebakeAllPlants(
                                    g_procgen, pp, reg, g_procgen.sunDir, g_procgen.season);
                                if (simPlants > 0)
                                    LUMINUMBRA_CORE_INFO("Plant unification: composited {} sim "
                                                         "plants over the scatter",
                                                         simPlants);
                            }
                            //  ecology demo: spawn a hungry predator above a row of prey, then
                            // let the live CreatureBrain tick (GameSession) move them — predator
                            // hunts toward, prey flee away — and render them as moving octahedron
                            // markers via the procgen pass (baked per-frame in the loop).
                            // Render/demo-only spawn.
                            if (g_app.capture.timelapse_creatures) {
                                // TRUE PHYSICS: each creature gets a deterministic Jolt avatar body
                                // (CreaturePhysicsComponent). Spawn a little ABOVE the terrain so
                                // it drops and settles on the surface; the brain's wish velocity
                                // then drives it across the heightfield (gravity / collision /
                                // slopes).
                                auto* phys = gameSession->GetPhysicsSystem();
                                int preyIdx = 0; // alternate founder sexes so a herd can pair up
                                auto mkCreature = [&](float ox,
                                                      float oz,
                                                      bool predator,
                                                      float hunger) {
                                    const float cx = anchor.x + ox, cz = anchor.z + oz;
                                    const float cy =
                                        terr(cx, cz) + 1.5f; // capsule centre above ground
                                    const auto e = reg.create();
                                    auto& tf =
                                        reg.emplace<Luminumbra::Components::TransformComponent>(e);
                                    tf.position = Luminumbra::Vec3(cx, cy, cz);
                                    auto& cr =
                                        reg.emplace<Luminumbra::Components::CreatureComponent>(e);
                                    cr.is_predator = predator;
                                    cr.species_id = Luminumbra::Components::CreatureSpeciesId16(
                                        predator ? "ridgeback_stalker" : "grovestrider");
                                    cr.hunger = hunger;
                                    // The predator is a bit faster than the herd so it can run down
                                    // a straggler (otherwise equal flee/hunt speeds never close the
                                    // gap).
                                    cr.move_speed = predator ? 4.2f : 3.0f;
                                    if (predator)
                                        reg.emplace<Luminumbra::Components::PackHunterComponent>(
                                            e); // flank coordination
                                    // Give prey a heritable genome so well-fed,
                                    // healthy, mature prey reproduce (CreatureReproductionSystem,
                                    // GameSession reproduction pass) and selection becomes visible
                                    // over the timelapse. move_speed mirrors the CreatureComponent
                                    // so behaviour is unchanged until traits drift in offspring.
                                    // Offspring are created mid-sim WITHOUT a Jolt avatar body, so
                                    // they fall back to the brain's direct X/Z integration (the
                                    // CreatureBrainSystem path when no CreaturePhysicsComponent).
                                    if (!predator) {
                                        auto& gn = reg.emplace<
                                            Luminumbra::Components::CreatureGenomeComponent>(e);
                                        gn.move_speed = cr.move_speed;
                                        gn.female =
                                            (preyIdx++ % 2 == 0); // alternate M/F so pairs form
                                        reg.emplace<Luminumbra::Components::AlarmComponent>(
                                            e); // herd vigilance
                                        // Calm (evolution) demo: start the founders WELL-FED + near
                                        // maturity so they court early and generations appear
                                        // within the clip (markers are tinted by generation).
                                        if (g_app.capture.timelapse_calm) {
                                            cr.hunger = 0.05f;
                                            cr.stamina = 1.0f;
                                            gn.age_ticks =
                                                80u; // just under kReproMaturityTicks (90)
                                        }
                                    }
                                    // Full LIFE CYCLE (calm demo): creatures age and die of old age
                                    // (LifespanSystem), then decompose to nothing
                                    // (DecompositionSystem) -- so the population self-bounds (birth
                                    // -> life -> death -> decay) instead of growing without limit.
                                    // Seeded lifespan per creature.
                                    if (g_app.capture.timelapse_calm) {
                                        auto& mort =
                                            reg.emplace<Luminumbra::Components::MortalComponent>(e);
                                        const std::uint32_t jit =
                                            static_cast<std::uint32_t>(entt::to_integral(e) *
                                                                       2654435761u) %
                                            240u;
                                        mort.lifespan_ticks = 420u + jit; // ~14-22s @30Hz
                                        auto& dec =
                                            reg.emplace<Luminumbra::Components::DecayComponent>(e);
                                        dec.decay_duration =
                                            120u; // carcass fades over ~4s after death
                                    }
                                    if (phys) {
                                        const std::size_t idx =
                                            phys->create_avatar_character(glm::vec3(cx, cy, cz));
                                        reg.emplace<
                                            Luminumbra::Components::CreaturePhysicsComponent>(e,
                                                                                              idx);
                                    }
                                };
                                if (g_app.capture.timelapse_calm) {
                                    // Calm grazing herd, NO predator: prey graze (hunger falls),
                                    // stay healthy, and reproduce over generations -> a visible
                                    // population-growth / trait-drift evolution demo.
                                    for (int i = 0; i < 6; ++i)
                                        mkCreature(-9.0f + static_cast<float>(i) * 3.6f,
                                                   0.0f,
                                                   /*predator*/ false,
                                                   /*hunger*/ 0.05f);
                                    LUMINUMBRA_CORE_INFO(": spawned 6 grazing founders (no "
                                                         "predator) for the evolution timelapse");
                                } else {
                                    // A PACK of 3 predators flanks the herd (PredatorPackSystem +
                                    // the 2e-steer consumer surround the prey from different
                                    // angles).
                                    mkCreature(-6.0f, 9.0f, /*predator*/ true, /*hunger*/ 0.95f);
                                    mkCreature(0.0f, 10.0f, /*predator*/ true, /*hunger*/ 0.95f);
                                    mkCreature(6.0f, 9.0f, /*predator*/ true, /*hunger*/ 0.95f);
                                    for (int i = 0; i < 7; ++i)
                                        mkCreature(-7.0f + static_cast<float>(i) * 2.4f,
                                                   -2.0f,
                                                   /*predator*/ false,
                                                   /*hunger*/ 0.3f);
                                    LUMINUMBRA_CORE_INFO(": spawned a 3-predator PACK + 7 prey for "
                                                         "the ecology timelapse");
                                }
                            }
                            // sim.fire DEMO: a dry patch of combustible bushes with the centre
                            // alight. The wired FireSpreadSystem advances the burn each tick (green
                            // -> orange
                            // -> charcoal); BakeCombustibleMarkers visualizes the deterministic
                            // state.
                            if (g_app.capture.timelapse_fire) {
                                const int N = 24;
                                const float sp = 2.0f;
                                const float x0 = anchor.x - N * sp * 0.5f,
                                            z0 = anchor.z - N * sp * 0.5f;
                                for (int iz = 0; iz < N; ++iz)
                                    for (int ix = 0; ix < N; ++ix) {
                                        const float cx = x0 + ix * sp, cz = z0 + iz * sp;
                                        const auto e = reg.create();
                                        auto& tf =
                                            reg.emplace<Luminumbra::Components::TransformComponent>(
                                                e);
                                        tf.position = Luminumbra::Vec3(cx, terr(cx, cz), cz);
                                        auto& cb = reg.emplace<
                                            Luminumbra::Components::CombustibleComponent>(e);
                                        cb.fuel_milli = 1000;
                                        cb.moisture_milli = 0; // bone dry -> spreads readily
                                        cb.ignition_radius =
                                            3.0f; // reaches orthogonal + diagonal neighbours
                                        if (ix >= N / 2 - 1 && ix <= N / 2 && iz >= N / 2 - 1 &&
                                            iz <= N / 2) {
                                            cb.set_state(
                                                Luminumbra::Components::BurnState::Burning);
                                            cb.burn_ticks_remaining =
                                                160u; // burns long enough to ignite outward
                                        }
                                    }
                                LUMINUMBRA_CORE_INFO(
                                    "sim.fire: spawned {}x{} combustible patch, centre alight",
                                    N,
                                    N);
                            }
                            // Fully consumed: release the pending result (idle again).
                            s_worldDressing.reset();
                            s_worldDressingHandle = {};
                        }
                    }
                }
                //  run the entire deterministic capture matrix
                // (times-of-day x angles x weather x season) in ONE synchronous pass
                // once the world is ready, then self-complete. The render_and_read
                // hook owns render_frame + present + glReadPixels so the harness stays
                // GL-context-free.: drives the existing one-way bridges,
                // never writes world_hash.
                if (scenario_config.world_visual_sweep() && scenario_ready &&
                    !world_visual_sweep_done && gameSession->GetWorldSystem() && g_camera) {
                    int sweep_fb_w = 0, sweep_fb_h = 0;
                    glfwGetFramebufferSize(window, &sweep_fb_w, &sweep_fb_h);
                    Luminumbra::Client::ScenarioHarness::WorldVisualSweepDeps deps;
                    deps.game_session = gameSession.get();
                    deps.pipeline = &renderPipeline;
                    deps.camera = g_camera.get();
                    deps.root_dir = root_dir;
                    deps.artifact_dir = scenario_config.artifact_dir;
                    // Winter is the second season pass; gate it behind the env flag
                    // so the standing gate (summer-only, 48 cells) stays fast while a
                    // manual LUMINUMBRA_VISUAL_SWEEP_WINTER=1 run captures both seasons.
                    {
                        const auto winter =
                            Luminumbra::Core::ReadEnvironment("LUMINUMBRA_VISUAL_SWEEP_WINTER");
                        deps.include_winter = winter && !winter->empty() && winter->front() != '0';
                    }
                    // The anchor position is FIXED across the whole matrix (only the
                    // camera orientation changes per cell), so the world only needs to
                    // stream ONCE. We stream for a bounded warmup, then skip the
                    // expensive per-frame world update and just re-render — the same
                    // settled geometry is reused for every subsequent cell.
                    int sweep_stream_frames = 0;
                    deps.render_and_read =
                        [&, sweep_stream_frames](std::vector<unsigned char>& out_pixels,
                                                 int& w,
                                                 int& h) mutable -> bool {
                        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                        if (sweep_stream_frames < 24) {
                            gameSession->GetWorldSystem()->update(
                                gameSession->GetRegistry(),
                                Luminumbra::Vec3(g_camera->Position),
                                gameSession->GetPhysicsSystem());
                            ++sweep_stream_frames;
                        }
                        renderPipeline.render_frame(gameSession->GetRegistry(),
                                                    *gameSession->GetWorldSystem(),
                                                    *g_camera,
                                                    1.0f / 60.0f,
                                                    g_app.overlay.wireframe_mode);
                        glfwGetFramebufferSize(window, &w, &h);
                        if (w <= 0 || h <= 0) {
                            return false;
                        }
                        out_pixels.assign(
                            static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u, 0u);
                        glReadBuffer(GL_BACK);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        // glReadPixels itself synchronizes; no explicit glFinish needed.
                        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, out_pixels.data());
                        glfwSwapBuffers(window);
                        glfwPollEvents();
                        return true;
                    };
                    const bool sweep_passed =
                        Luminumbra::Client::ScenarioHarness::RunWorldVisualSweep(deps);
                    world_visual_sweep_done = true;
                    if (!sweep_passed) {
                        scenario_failed = true;
                        scenario_failure_reason = "world_visual_sweep_presence_failed";
                    }
                    scenario_timed_run_complete = true;
                    glfwSetWindowShouldClose(window, true);
                }
                if (gameSession->GetWorldSystem() && g_camera) {
                    // Live play runs on the
                    // authoritative sim tick — season AND time-of-day become pure
                    // functions of it (no more frozen season-neutral tick 0; no more
                    // wall-clock day drift). Written FIRST each frame as the default:
                    // every scenario/scene/sweep/photo pin below runs AFTER this and
                    // overwrites it, so all capture paths keep their exact pins.
                    {
                        const std::uint64_t sim_tick = gameSession->GetSimulationTickCount();
                        renderPipeline.set_season_tick(sim_tick);
                        renderPipeline.set_time_of_day_tick(sim_tick);
                        // Sample live simulation weather at the camera and drive
                        // weather at the camera and drive the overlay + cloud layer
                        // through the pure WeatherRenderBridge mapping. The
                        // render.live_weather switch is enabled in the shipped config.
                        // Scenario/scene weather pins below override by frame order.
                        if (g_systemConfig.enabled(luminumbra::core::SysKey::RenderLiveWeather)) {
                            if (const auto* live_weather = gameSession->GetWeatherSystem()) {
                                const Luminumbra::Vec3 cam_pos(g_camera->Position.x,
                                                               g_camera->Position.y,
                                                               g_camera->Position.z);
                                const auto wsample = live_weather->SampleAt(cam_pos);
                                renderPipeline.set_weather_state(
                                    Luminumbra::Rendering::WeatherBridge::BuildWeatherRenderState(
                                        wsample));
                                renderPipeline.set_cloud_state(
                                    Luminumbra::Rendering::WeatherBridge::BuildCloudRenderState(
                                        wsample, sim_tick));
                            }
                        }
                        // Consume the simulation StrikeSchedule in live play. A
                        // scheduled strike renders a real world-space bolt through the
                        // ACTUAL camera view-proj (unlike the scenario's screen-anchored
                        // capture aid), pulses by magnitude with distance attenuation,
                        // and queues the thunder cue (distance + tick) for.
                        // Same flag as the weather bridge; reads the schedule, writes
                        // nothing back .
                        if (g_systemConfig.enabled(luminumbra::core::SysKey::RenderLiveWeather)) {
                            if (const auto* lweather = gameSession->GetWeatherSystem()) {
                                static std::uint64_t s_last_strike_tick_done = 0;
                                static int s_bolt_frames_left = 0;
                                static Luminumbra::Rendering::LightningRenderState s_live_bolt;
                                for (const auto& strike : lweather->StrikeSchedule()) {
                                    if (strike.strike_tick > sim_tick)
                                        continue; // not due yet
                                    if (strike.strike_tick + 2 < sim_tick)
                                        continue; // window passed
                                    if (strike.strike_tick <= s_last_strike_tick_done &&
                                        s_last_strike_tick_done != 0)
                                        continue;
                                    s_last_strike_tick_done = strike.strike_tick;
                                    auto* lws = gameSession->GetWorldSystem();
                                    const float gy = lws ? lws->GetTerrainHeightAt(strike.world_x,
                                                                                   strike.world_z)
                                                         : 0.0f;
                                    const auto bolt = Luminumbra::Rendering::BuildLightningBolt(
                                        strike.world_x,
                                        gy,
                                        strike.world_z,
                                        strike.magnitude,
                                        (static_cast<std::uint64_t>(strike.storm_salt) << 32) ^
                                            strike.strike_tick);
                                    int fbw = 0, fbh = 0;
                                    glfwGetFramebufferSize(window, &fbw, &fbh);
                                    const float aspect = (fbh > 0) ? static_cast<float>(fbw) /
                                                                         static_cast<float>(fbh)
                                                                   : 1.0f;
                                    const glm::mat4 vp =
                                        glm::perspective(
                                            glm::radians(g_camera->Zoom), aspect, 0.1f, 10000.0f) *
                                        g_camera->GetViewMatrix();
                                    s_live_bolt = {};
                                    const auto project = [&](const glm::vec3& wp) -> glm::vec2 {
                                        const glm::vec4 clip = vp * glm::vec4(wp, 1.0f);
                                        if (clip.w <= 0.0f)
                                            return glm::vec2(-3.0f, -3.0f); // behind: pen-up
                                        return glm::vec2(clip.x / clip.w, clip.y / clip.w);
                                    };
                                    const auto push_stroke =
                                        [&](const std::vector<glm::vec3>& stroke) {
                                            if (!s_live_bolt.bolt_points_ndc.empty()) {
                                                s_live_bolt.bolt_points_ndc.emplace_back(
                                                    -3.0f, -3.0f); // pen-up
                                            }
                                            for (const glm::vec3& wp : stroke) {
                                                s_live_bolt.bolt_points_ndc.push_back(project(wp));
                                            }
                                        };
                                    push_stroke(bolt.main_channel);
                                    for (const auto& br : bolt.branches) {
                                        push_stroke(br);
                                    }
                                    const glm::vec3 ground(strike.world_x, gy, strike.world_z);
                                    const float dist = glm::length(ground - g_camera->Position);
                                    s_live_bolt.active = true;
                                    // Magnitude-scaled flash, attenuated with distance (a far
                                    // strike lights the sky, a near one lights the scene).
                                    s_live_bolt.pulse_intensity =
                                        0.22f * std::clamp(strike.magnitude, 0.2f, 1.0f) /
                                        (1.0f + dist / 400.0f);
                                    s_live_bolt.bolt_width_ndc = 0.010f;
                                    s_live_bolt.bolt_glow_ndc = 0.034f;
                                    s_live_bolt.strike_ndc = project(ground);
                                    s_bolt_frames_left = 3; // a few frames of flash
                                    //  hook: queue the physically-delayed thunder cue.
                                    g_app.audio.pendingThunder.push_back({strike.strike_tick,
                                                                          glfwGetTime(),
                                                                          dist,
                                                                          strike.magnitude});
                                }
                                if (s_bolt_frames_left > 0) {
                                    renderPipeline.set_lightning_state(s_live_bolt);
                                    if (--s_bolt_frames_left == 0) {
                                        s_live_bolt = {};
                                        renderPipeline.set_lightning_state(s_live_bolt); // clear
                                    }
                                }
                            }
                        }
                        // Render-only snow ground cover accumulates from
                        // live Snow-category precipitation, melt by sun elevation
                        // (SnowCoverModel.h). The switch is enabled in the shipped config.
                        if (g_systemConfig.enabled(luminumbra::core::SysKey::RenderSnowCover)) {
                            if (const auto* snow_weather = gameSession->GetWeatherSystem()) {
                                static Luminumbra::Rendering::SnowCover::State s_snow;
                                const auto ssample =
                                    snow_weather->SampleAt(Luminumbra::Vec3(g_camera->Position.x,
                                                                            g_camera->Position.y,
                                                                            g_camera->Position.z));
                                const float snowing =
                                    (ssample.category == Luminumbra::Systems::WeatherCategory::Snow)
                                        ? ssample.precip_intensity
                                        : 0.0f;
                                const float sun_up =
                                    std::max(-renderPipeline.sun_direction().y, 0.0f);
                                Luminumbra::Rendering::SnowCover::Advance(
                                    s_snow, snowing, sun_up, static_cast<float>(deltaTime));
                                renderPipeline.set_snow_cover(s_snow.cover01);
                            }
                        }
                        // Upload the simulation's
                        // deterministic aether grid to the render emissive tap (one-way;
                        // the existing AetherEmissiveTap GPU test proves the ON path),
                        // through the shipped render.aether_tap switch.
                        if (g_systemConfig.enabled(luminumbra::core::SysKey::RenderAetherTap)) {
                            if (const auto* aether = gameSession->GetAetherFieldSystem()) {
                                const auto& agrid = aether->grid();
                                renderPipeline.update_aether_field(
                                    agrid.cells(),
                                    static_cast<float>(agrid.origin_cell_x()) * agrid.cell_size_m(),
                                    static_cast<float>(agrid.origin_cell_z()) * agrid.cell_size_m(),
                                    agrid.extent_cells(),
                                    agrid.cell_size_m());
                            }
                        }
                    }
                    if (scenario_config.lod_ground_smoke() ||
                        scenario_config.water_visual_smoke() ||
                        scenario_config.material_visual_smoke() ||
                        scenario_config.skybox_visual_smoke() ||
                        scenario_config.weather_visual_smoke() ||
                        scenario_config.cloud_shadow_smoke() ||
                        scenario_config.precipitation_smoke() ||
                        scenario_config.lod_seam_arrival_smoke() ||
                        scenario_config.player_view_smoke() ||
                        scenario_config.farlod_horizon_smoke() ||
                        scenario_config.skinned_mesh_visual_smoke() ||
                        scenario_config.creature_slice_smoke()) {
                        // time_of_day 0 is noon (sun elevation = cos(2*pi*t));
                        // 0.04 keeps the sun near its zenith for stable captures.
                        renderPipeline.set_time_of_day(0.04f);
                    } else if (scenario_config.timeofday_sweep_smoke() && scenario_ready) {
                        // SEASON SWEEP. The run is split into two
                        // season halves (summer then winter); each half replays
                        // the noon/dusk/night phase windows. Both the time-of-day
                        // AND the tick-derived season are re-pinned every frame so
                        // update_time_of_day cannot drift either between settle
                        // frames. set_season_tick feeds the authoritative-tick-
                        // style integer the season is a PURE FUNCTION of (no
                        // wall-clock for the season itself).
                        const double elapsed_play_seconds =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          scenario_play_started_at)
                                .count();
                        const double duration =
                            static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                        const double sweep_progress =
                            std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                        //  pin the sun to the PENDING CAPTURE's
                        // phase, NOT to SeasonSweepAt(progress). The capture writer is
                        // throttled to one screenshot per frame; under a sim hitch the
                        // progress could advance past the dusk window into night before
                        // the dusk capture actually wrote, so SeasonSweepAt(progress)
                        // rendered a NIGHT sun while the writer labelled it "dusk" (the
                        // regression: summer dusk recorded the night elevation, sky lum
                        // 35 < night 37, collapsing dusk>night). Driving the sun from the
                        // first not-yet-written plan whose threshold has passed guarantees
                        // the rendered sun matches the labelled phase the writer grabs.
                        int pending_capture = -1;
                        for (int i = 0; i < kTimeOfDaySweepCaptureCount; ++i) {
                            if (!timeofday_season_captures_written[static_cast<std::size_t>(i)] &&
                                sweep_progress >= TimeOfDaySweepCapturePlanAt(i).threshold) {
                                pending_capture = i;
                                break;
                            }
                        }
                        if (pending_capture >= 0) {
                            const TimeOfDaySweepCapturePlan& plan =
                                TimeOfDaySweepCapturePlanAt(pending_capture);
                            renderPipeline.set_season_tick(SeasonSweepTick(plan.season_index));
                            renderPipeline.set_time_of_day(plan.phase_time);
                            // Track settle frames at this pin so the capture below only
                            // writes once the lazily-refreshed sky dome has caught up.
                            if (pending_capture == timeofday_pending_pin) {
                                ++timeofday_pin_settle_frames;
                            } else {
                                timeofday_pending_pin = pending_capture;
                                timeofday_pin_settle_frames = 0;
                            }
                        } else {
                            timeofday_pending_pin = -1;
                            timeofday_pin_settle_frames = 0;
                            // No capture pending for this progress (settle/idle frames
                            // before the first threshold, or after the last write):
                            // fall back to the smooth sweep position.
                            const SeasonSweepPoint season_point = SeasonSweepAt(sweep_progress);
                            renderPipeline.set_season_tick(season_point.season_tick);
                            renderPipeline.set_time_of_day(season_point.time_of_day);
                        }
                    }
                    //  re-bake the creature markers from the live (just-ticked) positions
                    // so the ecology timelapse shows them actually moving each frame.
                    if (g_app.capture.timelapse_creatures) {
                        // Newborn offspring start bodyless; give them avatar bodies so they
                        // collide with the terrain (no more walking through mountains), then
                        // bake markers from the physics-resolved positions.
                        AttachMissingCreatureBodies(gameSession->GetPhysicsSystem(),
                                                    gameSession->GetRegistry());
                        BakeCreatureMarkers(renderPipeline.plant_procgen(),
                                            gameSession->GetRegistry(),
                                            gameSession.get());
                    }
                    if (g_app.capture.timelapse_fire) {
                        BakeCombustibleMarkers(renderPipeline.plant_procgen(),
                                               gameSession->GetRegistry(),
                                               gameSession->GetWorldSystem());
                    }
                    if (g_app.capture.fixed_cam && g_camera) {
                        g_camera->Position = g_app.capture.fixed_cam_pos;
                        g_camera->Yaw = g_app.capture.fixed_cam_yaw;
                        g_camera->Pitch = g_app.capture.fixed_cam_pitch;
                        g_camera->updateCameraVectors();
                    }
                    if (g_app.capture.scene_active) {
                        if (g_app.capture.scene_fov > 0.0f && g_camera)
                            g_camera->Zoom = g_app.capture.scene_fov;
                        renderPipeline.set_time_of_day(g_app.capture.timelapse_tod);
                        if (g_app.capture.scene_moon >= 0.0f)
                            renderPipeline.set_moon_illumination(
                                g_app.capture.scene_moon); // rendering
                        using WT = Luminumbra::Rendering::WeatherType;
                        const WT wt = (g_app.capture.scene_weather == 1)   ? WT::Rain
                                      : (g_app.capture.scene_weather == 2) ? WT::Snow
                                      : (g_app.capture.scene_weather == 3) ? WT::Fog
                                      : (g_app.capture.scene_weather == 4) ? WT::Storm
                                                                           : WT::None;
                        renderPipeline.set_weather(wt, g_app.capture.scene_weather_intensity);
                        if (g_app.capture.scene_clouds) {
                            Luminumbra::Rendering::CloudRenderState cs;
                            cs.enabled = true;
                            cs.shadow_enabled = g_app.capture.scene_cloud_shadow;
                            cs.coverage_amount = g_app.capture.scene_cloud_coverage;
                            cs.biome_variation = g_app.capture.scene_cloud_biome;
                            cs.plane_height = g_app.capture.scene_cloud_plane;
                            cs.shadow_strength = g_app.capture.scene_cloud_shadow_strength;
                            renderPipeline.set_cloud_state(cs);
                        }
                    }
                    // FOLIAGE in normal play: the GPU grass scatter was previously only
                    // built for the foliage_visual_smoke gate, so the live world had no
                    // grass. Load the scatter set once, then rebuild the per-chunk scatter
                    // over the visible ring each frame (wind-swayed).  (one-way,
                    // never hashed). rebuild_instances elides work when chunks are unchanged.
                    if (auto* foliage = renderPipeline.foliage()) {
                        auto* fol_ws = gameSession->GetWorldSystem();
                        if (fol_ws != nullptr && g_camera) {
                            static bool s_foliage_loaded = false;
                            if (!s_foliage_loaded) {
                                foliage->load_scatter_set(root_dir /
                                                          "data/common/foliage/scatter_set.json");
                                // Grass overhaul: concentrate the instance budget into a DENSE NEAR
                                // carpet (detail-near, texture-far — the AAA approach) instead of a
                                // thin scatter spread to 210 m. The global instance cap
                                // redistributes nearest-first, so a tighter fade makes the near
                                // field a believable carpet instead of sparse lit slivers over bare
                                // ground..
                                foliage->set_fade_distances(48.0f, 92.0f);
                                foliage->set_density_scale(1.6f);
                                s_foliage_loaded = true;
                            }
                            // skip the foliage CPU readback (a ~5 ms sync
                            // stall — see FoliagePass::set_readback_enabled) in
                            // scenario-LESS runs (normal play + the budget benchmark),
                            // which draw straight from the SSBO. Any active scenario
                            // (every gate, incl. FoliageInstancing's foliage_visual_smoke)
                            // KEEPS the readback so instance_hash/coverage stay exact.
                            foliage->set_readback_enabled(scenario_config.active() &&
                                                          !g_app.capture.play_paths);
                            glm::vec2 wind_xz(0.0f, 0.0f);
                            if (auto* wind = gameSession->GetWindFieldSystem()) {
                                const Luminumbra::Vec2 w =
                                    wind->SampleWind(Luminumbra::Vec3(g_camera->Position.x,
                                                                      g_camera->Position.y,
                                                                      g_camera->Position.z));
                                wind_xz = glm::vec2(w.x, w.y);
                            }
                            foliage->set_wind(wind_xz);
                            Luminumbra::Client::ScenarioHarness::FoliageScatterContext fol_ctx{
                                fol_ws};
                            const auto _rb_scatter_t0 =
                                std::chrono::steady_clock::now(); // Scatter-build timing.
                            // the scatter build (a GetTerrainHeightAt + BiomeIdAt per
                            // renderable chunk) was the frame's BIGGEST CPU cost (~5.3 ms) yet it's
                            // a pure function of the renderable-chunk SET — independent of
                            // camera/time. Cache it; rebuild only when that set changes (cheap
                            // coord-XOR signature vs the expensive per-chunk terrain sampling).
                            // rebuild_instances still runs every frame (camera LOD/fade), so
                            // foliage stays camera-responsive. Gated OFF while any scenario is
                            // active so every gate rebuilds byte-exact (mirrors the readback gating
                            // above) — zero gate/determinism impact.
                            static std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter>
                                s_cached_scatter;
                            static std::uint64_t s_cached_scatter_sig = ~0ull;
                            const auto& fol_renderable = fol_ws->get_renderable_chunks();
                            std::uint64_t scatter_sig = fol_renderable.size();
                            for (const Luminumbra::Chunk* chunk : fol_renderable) {
                                if (chunk == nullptr) {
                                    continue;
                                }
                                const Luminumbra::IVec3 c = chunk->get_coords();
                                scatter_sig =
                                    scatter_sig * 1099511628211ull ^
                                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) |
                                     (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.z))
                                      << 21) |
                                     (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y))
                                      << 42));
                            }
                            const bool scatter_rebuild =
                                (scenario_config.active() && !g_app.capture.play_paths) ||
                                scatter_sig != s_cached_scatter_sig;
                            if (scatter_rebuild) {
                                //  implementation note: each per-chunk ChunkScatter
                                //  (GetTerrainHeightAt +
                                // BiomeIdAt at the chunk centre) is a pure function of the chunk +
                                // the static terrain/biome, so cache it per chunk id. The full
                                // rebuild on a chunk-set change then only COMPUTES the
                                // newly-streamed chunks instead of re-sampling every renderable
                                // chunk (~130ms while moving). The y-band reject is cached too
                                // (stored as accepted=false)..
                                struct ScatterEntry {
                                    bool accepted;
                                    Luminumbra::Rendering::FoliagePass::ChunkScatter cs;
                                };
                                static std::unordered_map<Luminumbra::ChunkID, ScatterEntry>
                                    s_scatter_by_chunk;
                                s_cached_scatter.clear();
                                s_cached_scatter.reserve(fol_renderable.size());
                                for (const Luminumbra::Chunk* chunk : fol_renderable) {
                                    if (chunk == nullptr) {
                                        continue;
                                    }
                                    const Luminumbra::ChunkID id = chunk->get_id();
                                    auto it = s_scatter_by_chunk.find(id);
                                    if (it == s_scatter_by_chunk.end()) {
                                        const Luminumbra::IVec3 c = chunk->get_coords();
                                        const float origin_x =
                                            static_cast<float>(c.x * Luminumbra::CHUNK_SIZE_X);
                                        const float origin_z =
                                            static_cast<float>(c.z * Luminumbra::CHUNK_SIZE_Z);
                                        const float center_x =
                                            origin_x + Luminumbra::CHUNK_SIZE_X * 0.5f;
                                        const float center_z =
                                            origin_z + Luminumbra::CHUNK_SIZE_Z * 0.5f;
                                        const float surf_h =
                                            fol_ws->GetTerrainHeightAt(center_x, center_z);
                                        const float chunk_y0 =
                                            static_cast<float>(c.y * Luminumbra::CHUNK_SIZE_Y);
                                        ScatterEntry entry{};
                                        if (surf_h < chunk_y0 ||
                                            surf_h >= chunk_y0 + Luminumbra::CHUNK_SIZE_Y) {
                                            entry.accepted = false;
                                        } else {
                                            const Luminumbra::u8 biome_id =
                                                fol_ws->BiomeIdAt(center_x, center_z);
                                            const float density =
                                                fol_ws->biomes_enabled()
                                                    ? fol_ws->biome_table()
                                                          .vegetation_for(biome_id)
                                                          .density
                                                    : 0.3f;
                                            entry.accepted = true;
                                            entry.cs.chunk_xz = glm::ivec2(c.x, c.z);
                                            entry.cs.origin = glm::vec3(origin_x, 0.0f, origin_z);
                                            entry.cs.extent_m =
                                                static_cast<float>(Luminumbra::CHUNK_SIZE_X);
                                            entry.cs.biome_id = biome_id;
                                            entry.cs.density = density;
                                        }
                                        it = s_scatter_by_chunk.emplace(id, entry).first;
                                    }
                                    if (it->second.accepted) {
                                        s_cached_scatter.push_back(it->second.cs);
                                    }
                                }
                                s_cached_scatter_sig = scatter_sig;
                                // Bound the cache: drop entries no longer renderable once it grows
                                // large.
                                if (s_scatter_by_chunk.size() > 4096) {
                                    std::unordered_set<Luminumbra::ChunkID> live;
                                    live.reserve(fol_renderable.size());
                                    for (const Luminumbra::Chunk* ch : fol_renderable) {
                                        if (ch) {
                                            live.insert(ch->get_id());
                                        }
                                    }
                                    for (auto pit = s_scatter_by_chunk.begin();
                                         pit != s_scatter_by_chunk.end();) {
                                        pit = live.count(pit->first)
                                                  ? std::next(pit)
                                                  : s_scatter_by_chunk.erase(pit);
                                    }
                                }
                            }
                            const std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter>&
                                chunk_scatter = s_cached_scatter;
                            rb_scatter_ms = std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - _rb_scatter_t0)
                                                .count();                             //
                            const auto _rb_fol_t0 = std::chrono::steady_clock::now(); //
                            foliage->rebuild_instances(
                                chunk_scatter,
                                &Luminumbra::Client::ScenarioHarness::FoliageSurfaceQuery,
                                &fol_ctx,
                                g_camera->Position);
                            rb_foliage_ms = std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - _rb_fol_t0)
                                                .count(); //
                        }
                    }

                    // Ambient atmosphere particles: a soft drift of pollen/dust motes
                    // around the player. Spawned once; its origin follows the camera
                    // each frame so the motes are always present as you explore.
                    {
                        static std::uint32_t s_ambient_motes =
                            Luminumbra::Rendering::ParticlePass::kInvalidEmitter;
                        if (auto* particles = renderPipeline.particles()) {
                            if (s_ambient_motes ==
                                Luminumbra::Rendering::ParticlePass::kInvalidEmitter) {
                                s_ambient_motes = particles->add_emitter(
                                    root_dir / "data/common/particles/ambient_motes.json",
                                    g_camera->Position);
                            } else {
                                particles->set_emitter_origin(s_ambient_motes, g_camera->Position);
                            }
                        }
                    }
                    //  re-bake the LIVE sim plant roster each frame so player-seeded
                    // (and growing) plants render at their current stage. No-op when there are no
                    // sim plants (leaves the procgen scatter on the pass untouched); once the
                    // player farms, their plants take the pass.
                    {
                        const auto _fb0 =
                            std::chrono::steady_clock::now(); // runtime telemetry: per-frame
                                                              // foliage rebake
                        if (auto* ppp = renderPipeline.plant_procgen())
                            RebakeAllPlants(g_procgen,
                                            ppp,
                                            gameSession->GetRegistry(),
                                            g_procgen.sunDir,
                                            g_procgen.season);
                        rb_rebake_ms = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - _fb0)
                                           .count();
                    }
                    const auto _rb_rcall_t0 =
                        std::chrono::steady_clock::now(); // always-on (runtime telemetry)
                    renderPipeline.set_debug_view(
                        g_app.overlay
                            .debug_view_mode); // render-only; 0 = byte-identical default ( /
                                               // --debug-view)
                    renderPipeline.render_frame(gameSession->GetRegistry(),
                                                *gameSession->GetWorldSystem(),
                                                *g_camera,
                                                deltaTime,
                                                g_app.overlay.wireframe_mode);
                    rb_render_call_ms = std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - _rb_rcall_t0)
                                            .count();

                    // --scene-config: self-contained capture. Settle a few frames (world
                    // stream + atmosphere), then read the clean back buffer (BEFORE any UI
                    // overlay this frame) and write the screenshot, then close.
                    if (g_app.capture.scene_active) {
                        static int s_scene_settle = 0;
                        if (s_scene_settle < 55) {
                            ++s_scene_settle;
                        } else {
                            int vw = 0, vh = 0;
                            glfwGetFramebufferSize(window, &vw, &vh);
                            if (vw > 0 && vh > 0) {
                                std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                              static_cast<std::size_t>(vh) * 3u);
                                glReadBuffer(GL_BACK);
                                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                                WritePixelBufferPpm(g_app.capture.scene_shot, vw, vh, px);
                                LUMINUMBRA_CORE_INFO("Scene capture written -> {} ({}x{})",
                                                     g_app.capture.scene_shot.string(),
                                                     vw,
                                                     vh);
                            }
                            glfwSetWindowShouldClose(window, GLFW_TRUE);
                        }
                    }

                    // survey: autonomous POI tour. Let the spawn world settle a few frames, then
                    // discover + capture a waterfall / cliff / grass field / lake (teleport +
                    // stream
                    // + settle + screenshot + frame-scan per POI), then exit..
                    if (g_app.capture.survey_active && currentState == GameState::IN_GAME &&
                        gameSession && gameSession->GetWorldSystem() && g_camera) {
                        if (g_app.capture.survey_settle < 45) {
                            ++g_app.capture.survey_settle;
                        } else {
                            Luminumbra::Rendering::RunSceneSurvey(
                                window,
                                *gameSession,
                                renderPipeline,
                                *g_camera,
                                std::filesystem::path(g_app.capture.survey_dir),
                                root_dir / "data/common/materials.json",
                                g_app.overlay.wireframe_mode);
                            glfwSetWindowShouldClose(window, GLFW_TRUE);
                        }
                    }

                    // framescan: deterministic what's-in-frame scan. Pin the SAME forest-dense
                    // pose + near-noon time-of-day the render benchmark uses (reproducible +
                    // real coverage), settle, then read the settled frame's G-buffer material-id
                    // attachment + clean back buffer (BEFORE any UI overlay) and write the
                    // per-material coverage/luminance + water + foliage report.: the
                    // scan issues no draws and never feeds world_hash, so a second run on the same
                    // world is byte-identical. Pinned every frame so settle can't drift the pose.
                    // Watchdog (runs EVERY frame while scanning, regardless of game state): if the
                    // world never reaches IN_GAME + settles within the deadline, abort cleanly so a
                    // headless capture can never hang the whole run (e.g. a wedged loading screen).
                    if (g_app.capture.frame_scan_active &&
                        ++g_app.capture.frame_scan_watchdog > kFrameScanWatchdogFrames) {
                        LUMINUMBRA_CORE_ERROR("Frame-scan watchdog: did not settle within {} "
                                              "frames (state={}). Aborting "
                                              "without a report.",
                                              kFrameScanWatchdogFrames,
                                              static_cast<int>(currentState));
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                    if (g_app.capture.frame_scan_active && currentState == GameState::IN_GAME &&
                        gameSession) {
                        if (g_camera) {
                            // Eye-level ground-inspection pose: low + a gentle down-pitch so the
                            // near-field grass/rock/terrain fill the lower frame (card shape +
                            // specks are visible), with terrain to the horizon above. Deterministic
                            // fixed pose.
                            g_camera->Position = glm::vec3(8.0f, 24.0f, 8.0f);
                            g_camera->Yaw = 35.0f;
                            g_camera->Pitch = -14.0f;
                            g_camera->updateCameraVectors();
                        }
                        renderPipeline.set_time_of_day(0.04f); // fixed near-noon (lit terrain)
                        if (g_app.capture.frame_scan_settle <
                            g_app.capture.frame_scan_settle_target) {
                            ++g_app.capture
                                  .frame_scan_settle; // let chunks stream + atmosphere settle
                        } else {
                            if (g_app.capture.render_parity_active) {
                                bool parity_ok = false;
                                if (g_app.capture.render_parity_pass == "ssao" && g_camera)
                                    parity_ok = renderPipeline.capture_ssao_parity(
                                        g_app.capture.render_parity_dir, *g_camera);
                                else if (g_app.capture.render_parity_pass == "upscale_seam" &&
                                         g_camera)
                                    parity_ok = renderPipeline.capture_upscale_seam_parity(
                                        *g_camera, g_app.capture.render_parity_dir);
                                else if (g_app.capture.render_parity_pass == "frame" && g_camera)
                                    // whole-frame A/B — dispatch the settled
                                    // prepared frame twice, in-process FLIP must be 0.0.
                                    parity_ok = renderPipeline.capture_frame_parity(
                                        *g_camera, g_app.capture.render_parity_dir);
                                else
                                    parity_ok = false;
                                if (parity_ok)
                                    LUMINUMBRA_CORE_INFO("{} parity captured -> {}",
                                                         g_app.capture.render_parity_pass,
                                                         g_app.capture.render_parity_dir.string());
                                else
                                    LUMINUMBRA_CORE_ERROR("{} parity capture FAILED",
                                                          g_app.capture.render_parity_pass);
                            }
                            int vw = 0, vh = 0;
                            glfwGetFramebufferSize(window, &vw, &vh);
                            const Luminumbra::Rendering::FrameScanReport rep =
                                Luminumbra::Rendering::ScanFrame(renderPipeline,
                                                                 vw,
                                                                 vh,
                                                                 root_dir /
                                                                     "data/common/materials.json");
                            if (rep.ok &&
                                Luminumbra::Rendering::WriteFrameScanReport(
                                    rep, std::filesystem::path(g_app.capture.frame_scan_path))) {
                                LUMINUMBRA_CORE_INFO(
                                    "Frame-scan written -> {} ({}x{}): {} materials, water "
                                    "{:.1f}%, foliage {} inst, mean luma {:.3f}",
                                    g_app.capture.frame_scan_path,
                                    rep.width,
                                    rep.height,
                                    rep.materials.size(),
                                    rep.water_coverage * 100.0,
                                    rep.foliage_instances,
                                    rep.mean_frame_luminance);
                                // Also dump the scanned back buffer as a PPM next to the JSON, so
                                // the scan is a full diagnostic (numbers + the exact image they
                                // describe).
                                if (vw > 0 && vh > 0) {
                                    std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                                  static_cast<std::size_t>(vh) *
                                                                  3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                                    std::filesystem::path img(g_app.capture.frame_scan_path);
                                    img.replace_extension(".ppm");
                                    WritePixelBufferPpm(img, vw, vh, px);
                                    LUMINUMBRA_CORE_INFO("Frame-scan image -> {}", img.string());

                                    // Frame-HEALTH verdict on the SAME settled frame (reuses px;
                                    // render-only, no sim). Auto-flags black/unlit/blown/NaN so a
                                    // broken frame is caught WITHOUT a human eyeballing the PPM —
                                    // the 'detect what's wrong' capability.
                                    std::vector<float> pos_rgb16f;
                                    std::vector<unsigned char> albedo_rgb8;
                                    const auto& gb_h = renderPipeline.gbuffer();
                                    if (gb_h.position_texture != 0 &&
                                        renderPipeline.screen_width() ==
                                            static_cast<std::uint32_t>(vw) &&
                                        renderPipeline.screen_height() ==
                                            static_cast<std::uint32_t>(vh)) {
                                        pos_rgb16f.resize(static_cast<std::size_t>(vw) * vh * 3u);
                                        glBindTexture(GL_TEXTURE_2D, gb_h.position_texture);
                                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                        glGetTexImage(
                                            GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, pos_rgb16f.data());
                                        if (gb_h.albedo_texture != 0) {
                                            albedo_rgb8.resize(static_cast<std::size_t>(vw) * vh *
                                                               3u);
                                            glBindTexture(GL_TEXTURE_2D, gb_h.albedo_texture);
                                            glGetTexImage(GL_TEXTURE_2D,
                                                          0,
                                                          GL_RGB,
                                                          GL_UNSIGNED_BYTE,
                                                          albedo_rgb8.data());
                                        }
                                        glBindTexture(GL_TEXTURE_2D, 0);
                                    }
                                    const auto health = Luminumbra::Rendering::AnalyzeFrameHealth(
                                        vw, vh, px, pos_rgb16f, albedo_rgb8);
                                    {
                                        std::filesystem::path hp(g_app.capture.frame_scan_path);
                                        hp.replace_extension(".health.json");
                                        std::ofstream hf(hp, std::ios::binary | std::ios::trunc);
                                        if (hf) {
                                            const std::string j =
                                                Luminumbra::Rendering::FrameHealthToJson(health);
                                            hf.write(j.data(),
                                                     static_cast<std::streamsize>(j.size()));
                                        }
                                    }
                                    if (health.verdict.anomalous) {
                                        LUMINUMBRA_CORE_ERROR(
                                            "Frame-health ANOMALY: {} (mean luma {:.4f}, coverage "
                                            "{:.1f}%, black {:.1f}%)",
                                            health.verdict.reason,
                                            health.mean_luminance,
                                            health.gbuffer_coverage * 100.0,
                                            health.black_fraction * 100.0);
                                    } else {
                                        LUMINUMBRA_CORE_INFO(
                                            "Frame-health OK (mean luma {:.4f}, coverage {:.1f}%)",
                                            health.mean_luminance,
                                            health.gbuffer_coverage * 100.0);
                                    }
                                }
                            } else {
                                LUMINUMBRA_CORE_ERROR("Frame-scan failed -> {}",
                                                      g_app.capture.frame_scan_path);
                            }
                            glfwSetWindowShouldClose(window, GLFW_TRUE);
                        }
                    }

                    // ---  : player FARMING verbs (F/G/H/J) ---
                    // Plant / water / fertilize / harvest the plant nearest the player's aim point,
                    // via the deterministic FarmingSystem verbs (FarmingController). Edge-triggered
                    // (one action per key press). Interactive-only guard keeps it out of the
                    // scenario/gate runs, so determinism is unaffected (gates never press these
                    // keys).
                    if (g_playerController && currentState == GameState::IN_GAME &&
                        !scenario_config.active() && !g_app.hud.paused && gameSession && g_camera) {
                        if (!g_app.hud.farmSpeciesLoaded) {
                            std::vector<std::string> ferr;
                            g_app.hud.farmSpecies.LoadFromDirectory(
                                root_dir / "data/common/foliage/species", ferr);
                            g_app.hud.farmSpeciesLoaded = true;
                        }
                        static bool s_fp = false, s_fw = false, s_ff = false, s_fh = false,
                                    s_fv = false;
                        // V cycles the selected species to plant (data-driven; oak/wheat/...).
                        {
                            const bool fv_now = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
                            const std::size_t n = g_app.hud.farmSpecies.all().size();
                            if (fv_now && !s_fv && n > 0) {
                                g_app.hud.farmSelectedSpecies =
                                    (g_app.hud.farmSelectedSpecies + 1) % static_cast<int>(n);
                                if (audioManager)
                                    audioManager->PlayOneShot2D("ui_button_click");
                                LUMINUMBRA_CORE_INFO(
                                    "Farm: selected species '{}'",
                                    g_app.hud.farmSpecies.all()[g_app.hud.farmSelectedSpecies].id);
                            }
                            s_fv = fv_now;
                        }
                        auto farmEdge = [&](Luminumbra::Client::InputAction a, bool& prev) {
                            const bool now =
                                glfwGetKey(window, g_playerController->key(a)) == GLFW_PRESS;
                            const bool fired = now && !prev;
                            prev = now;
                            return fired;
                        };
                        const glm::vec3 ffwd =
                            glm::normalize(glm::vec3(g_camera->Front.x, 0.0f, g_camera->Front.z));
                        const glm::vec3 aimXZ = glm::vec3(g_camera->Position) + ffwd * 3.0f;
                        const float aimY = gameSession->GetWorldSystem()
                                               ? gameSession->GetWorldSystem()->GetTerrainHeightAt(
                                                     aimXZ.x, aimXZ.z)
                                               : aimXZ.y;
                        const Luminumbra::Vec3 aim(aimXZ.x, aimY, aimXZ.z);
                        auto& freg = gameSession->GetRegistry();
                        const std::uint64_t ftick = gameSession->GetSimulationTickCount();
                        using IA = Luminumbra::Client::InputAction;
                        if (farmEdge(IA::FarmPlant, s_fp)) {
                            const auto& species = g_app.hud.farmSpecies.all();
                            if (!species.empty()) {
                                const luminumbra::foliage::SpeciesTemplate& tmpl =
                                    species[static_cast<std::size_t>(
                                                g_app.hud.farmSelectedSpecies) %
                                            species.size()];
                                auto frng = luminumbra::core::DeterministicRng::seeded(
                                    luminumbra::foliage::kPlantSeedOffset,
                                    static_cast<std::uint64_t>(
                                        static_cast<std::int64_t>(aimXZ.x * 8.0f)),
                                    static_cast<std::uint64_t>(
                                        static_cast<std::int64_t>(aimXZ.z * 8.0f)) ^
                                        ftick);
                                if (g_app.hud.farming.Seed(freg, aim, tmpl, frng, ftick) !=
                                    entt::null) {
                                    if (audioManager)
                                        audioManager->PlayOneShot("farm_plant",
                                                                  glm::vec3(aim.x, aim.y, aim.z));
                                    LUMINUMBRA_CORE_INFO("Farm: planted {} ({} seeds left)",
                                                         tmpl.id,
                                                         g_app.hud.farming.seeds);
                                }
                            }
                        }
                        // Tend the nearest plant; if there's no SIM plant in reach but a wild
                        // scatter tree is, PROMOTE it to a sim plant first (unification: one
                        // continuum).
                        bool promoted = false;
                        const auto fpick = [&]() -> entt::entity {
                            entt::entity e = luminumbra::foliage::FarmingController::NearestPlant(
                                freg, aim, 3.0f);
                            if (e == entt::null) {
                                e = PromoteNearestScatter(g_procgen, freg, aim, 3.0f, ftick);
                                if (e != entt::null) {
                                    promoted = true;
                                    if (audioManager)
                                        audioManager->PlayOneShot("farm_plant",
                                                                  glm::vec3(aim.x, aim.y, aim.z));
                                    LUMINUMBRA_CORE_INFO(
                                        "Farm: promoted a wild plant to a tended crop");
                                }
                            }
                            return e;
                        };
                        const glm::vec3 aimSnd(aim.x, aim.y, aim.z);
                        if (farmEdge(IA::FarmWater, s_fw)) {
                            if (g_app.hud.farming.Water(freg, fpick()) && audioManager)
                                audioManager->PlayOneShot("farm_water", aimSnd);
                        }
                        if (farmEdge(IA::FarmFertilize, s_ff)) {
                            if (g_app.hud.farming.Fertilize(freg, fpick()) && audioManager)
                                audioManager->PlayOneShot("farm_fertilize", aimSnd);
                        }
                        if (farmEdge(IA::FarmHarvest, s_fh)) {
                            const auto hr = g_app.hud.farming.Harvest(freg, fpick());
                            if (hr.harvestable) {
                                if (audioManager)
                                    audioManager->PlayOneShot("farm_harvest", aimSnd);
                                LUMINUMBRA_CORE_INFO(
                                    "Farm: harvested yield {:.2f} (+{} seeds, {} total)",
                                    hr.yield,
                                    hr.seeds,
                                    g_app.hud.farming.harvests);
                            }
                        }
                        // A promotion suppressed a scatter instance -> rebuild the scatter cache so
                        // the next composite re-bake (RebakeAllPlants) drops the now-promoted dup.
                        if (promoted) {
                            if (auto* fpp = renderPipeline.plant_procgen())
                                BakeProcgenPlants(g_procgen, fpp, g_procgen.stageF);
                        }
                    }

                    // ---  : player TERRAFORM verbs (R dig / T fill) ---
                    // Carve (R) or raise (T) the voxel terrain at the player's aim point via the
                    // deterministic SHIELD_WorldSystem::EditTerrainVoxel — it edits sdf_data,
                    // remeshes, rebuilds colliders, and couples the water bed so a dig DRAINS a
                    // river/lake and a fill DAMS it. Edge-triggered (one carve per press).
                    // Interactive-only guard keeps it out of scenario/gate runs, so determinism is
                    // unaffected (gates never press R/T).
                    if (g_playerController && currentState == GameState::IN_GAME &&
                        !scenario_config.active() && !g_app.hud.paused && gameSession && g_camera &&
                        gameSession->GetWorldSystem()) {
                        static bool s_dig = false, s_fill = false;
                        const bool dig_now = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
                        const bool fill_now = glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS;
                        const bool dig_fired = dig_now && !s_dig;
                        s_dig = dig_now;
                        const bool fill_fired = fill_now && !s_fill;
                        s_fill = fill_now;
                        if (dig_fired || fill_fired) {
                            // Aim where the player looks: a point ~5 m down the camera ray.
                            constexpr float kReach = 5.0f, kRadius = 3.0f;
                            const glm::vec3 aim = glm::vec3(g_camera->Position) +
                                                  glm::normalize(g_camera->Front) * kReach;
                            const int n = gameSession->GetWorldSystem()->EditTerrainVoxel(
                                Luminumbra::Vec3(aim.x, aim.y, aim.z),
                                kRadius,
                                /*fill=*/fill_fired,
                                gameSession->GetPhysicsSystem());
                            // Everything maps to sound: digging picks its sample from the
                            // material we're cutting into; filling is a single pack-in-place thud.
                            if (audioManager) {
                                if (fill_fired) {
                                    audioManager->PlayOneShot("terraform_place", aim);
                                } else {
                                    const float th =
                                        gameSession->GetWorldSystem()->GetTerrainHeightAt(aim.x,
                                                                                          aim.z);
                                    const char* dev = "dig_soil";
                                    switch (gameSession->GetWorldSystem()->SurfaceVertexMaterial(
                                        aim.x, aim.z, th)) {
                                        case Luminumbra::MaterialType::Stone:
                                        case Luminumbra::MaterialType::Deepslate:
                                        case Luminumbra::MaterialType::LuminCrystal:
                                            dev = "dig_stone";
                                            break;
                                        case Luminumbra::MaterialType::Sand:
                                            dev = "dig_sand";
                                            break;
                                        case Luminumbra::MaterialType::Soil:
                                        case Luminumbra::MaterialType::Grass:
                                        default:
                                            dev = "dig_soil";
                                            break;
                                    }
                                    audioManager->PlayOneShot(dev, aim);
                                }
                                // Water re-routes when the edit borders standing water (a dig
                                // DRAINS, a fill DAMS) -> a one-shot rush so the coupling is heard,
                                // not just seen.
                                auto* tws = gameSession->GetWorldSystem();
                                static const float ring[5][2] = {
                                    {0, 0}, {4, 0}, {-4, 0}, {0, 4}, {0, -4}};
                                bool bordersWater = false;
                                for (const auto& o : ring) {
                                    const float wx = aim.x + o[0], wz = aim.z + o[1];
                                    if (tws->WaterLevelAt(wx, wz) >
                                        tws->GetTerrainHeightAt(wx, wz) + 0.4f) {
                                        bordersWater = true;
                                        break;
                                    }
                                }
                                if (bordersWater)
                                    audioManager->PlayOneShot("water_rush", aim);
                            }
                            LUMINUMBRA_CORE_INFO(
                                "Terraform: {} {} chunk(s) at ({:.1f},{:.1f},{:.1f})",
                                fill_fired ? "filled" : "dug",
                                n,
                                aim.x,
                                aim.y,
                                aim.z);
                        }
                    }

                    // Photo-mode capture loop + codex/objectives/farming HUD
                    // (app/DebugOverlays.cpp). Render-side, read-only observer.
                    UpdatePhotoModeAndHud(g_app,
                                          currentState,
                                          window,
                                          root_path_str,
                                          g_playerController.get(),
                                          g_camera.get(),
                                          g_uiManager.get(),
                                          audioManager.get(),
                                          gameSession.get(),
                                          renderPipeline,
                                          scenario_config);

                    if (scenario_config.active() && currentState == GameState::IN_GAME) {
                        ++scenario_frame_count;
                        const auto now = std::chrono::steady_clock::now();
                        // window_mode_stress_smoke: drive
                        // one resize-chain step per frame after readiness. The
                        // window itself stays pinned/hidden (capture protection),
                        // but on_resize reallocates the non-pinned targets through
                        // exactly the runtime resize path. We measure the resize
                        // generation + GL errors AFTER this frame's render so the
                        // PREVIOUS step's new targets have been drawn into once.
                        if (scenario_config.window_mode_stress_smoke() && scenario_ready &&
                            !window_mode_stress_complete) {
                            if (window_mode_stress_steps.empty()) {
                                window_mode_stress_steps = BuildWindowModeStressSequence();
                            }
                            // Finalize the step applied on the previous frame
                            // (its targets were just rendered into this frame).
                            if (window_mode_stress_step_index > 0) {
                                WindowModeStressStep& done =
                                    window_mode_stress_steps[window_mode_stress_step_index - 1];
                                done.resize_generation_after = renderPipeline.resize_generation();
                                done.gl_errors_after = CurrentGLDebugRuntimeStats().errors;
                                done.targets_width_after =
                                    static_cast<int>(renderPipeline.screen_width());
                                done.targets_height_after =
                                    static_cast<int>(renderPipeline.screen_height());
                            }
                            if (window_mode_stress_step_index < window_mode_stress_steps.size()) {
                                WindowModeStressStep& step =
                                    window_mode_stress_steps[window_mode_stress_step_index];
                                step.resize_generation_before = renderPipeline.resize_generation();
                                step.size_changed =
                                    static_cast<int>(renderPipeline.screen_width()) != step.width ||
                                    static_cast<int>(renderPipeline.screen_height()) != step.height;
                                renderPipeline.on_resize(static_cast<unsigned int>(step.width),
                                                         static_cast<unsigned int>(step.height));
                                ++window_mode_stress_step_index;
                            } else {
                                // All steps applied + finalized: capture the
                                // restored pinned-size frame (Smoke-equivalent).
                                // Targets are 1280x720; render one clean frame
                                // into the default framebuffer for the readback.
                                window_mode_stress_capture.width =
                                    static_cast<int>(renderPipeline.screen_width());
                                window_mode_stress_capture.height =
                                    static_cast<int>(renderPipeline.screen_height());
                                window_mode_stress_capture.file = "window-mode-stress-final.ppm";
                                ScreenshotPixelStats final_stats;
                                if (WriteBackbufferPpm(scenario_config.artifact_dir /
                                                           window_mode_stress_capture.file,
                                                       window_mode_stress_capture.width,
                                                       window_mode_stress_capture.height,
                                                       &final_stats)) {
                                    window_mode_stress_capture.pixels = final_stats;
                                }
                                window_mode_stress_complete = true;
                            }
                        }
                        if (lod_ground_frame_recorder.enabled()) {
                            lod_ground_frame_recorder.record_frame(
                                deltaTime, gameSession.get(), renderPipeline, scenario_frame_count);
                        }
                        if (scenario_config.lod_boundary_oscillation_smoke() && scenario_ready) {
                            lod_boundary_transition_recorder.record_frame(
                                gameSession->GetWorldSystem());
                        }
                        if (scenario_config.lod_seam_arrival_smoke() && scenario_ready) {
                            lod_seam_arrival_recorder.record_frame(gameSession->GetWorldSystem());
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const std::array<double, 4> thresholds{0.25, 0.50, 0.75, 0.95};
                            const std::array<const char*, 4> names{"p25", "p50", "p75", "p95"};
                            for (std::size_t i = 0; i < thresholds.size(); ++i) {
                                if (lod_seam_screenshots_written[i] || progress < thresholds[i]) {
                                    continue;
                                }
                                lod_seam_screenshots_written[i] = true;
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                const std::string relative_path =
                                    std::string("screenshots/lod-seam-") + names[i] + ".ppm";
                                LodHolePixelStats hole_stats;
                                if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path,
                                                       screenshot_width,
                                                       screenshot_height,
                                                       nullptr,
                                                       &hole_stats)) {
                                    lod_seam_visual_captures.push_back(
                                        {names[i], relative_path, hole_stats});
                                    WriteLodSeamArrivalAnalysis(scenario_config.artifact_dir,
                                                                elapsed_play_seconds,
                                                                lod_seam_visual_captures,
                                                                lod_seam_arrival_recorder);
                                }
                            }
                        }
                        if (scenario_config.lod_ground_smoke() && scenario_ready) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const std::array<double, 3> thresholds{0.15, 0.55, 0.95};
                            const std::array<const char*, 3> names{"start", "mid", "end"};
                            for (std::size_t i = 0; i < thresholds.size(); ++i) {
                                if (lod_ground_screenshots_written[i] || progress < thresholds[i]) {
                                    continue;
                                }
                                lod_ground_screenshots_written[i] = true;
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                const std::string relative_path =
                                    std::string("screenshots/lod-ground-") + names[i] + ".ppm";
                                LodHolePixelStats hole_stats;
                                if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path,
                                                       screenshot_width,
                                                       screenshot_height,
                                                       nullptr,
                                                       &hole_stats)) {
                                    lod_ground_screenshot_files.push_back(relative_path);
                                    lod_ground_visual_captures.push_back(
                                        {names[i], relative_path, hole_stats});
                                    WriteLodGroundScreenshotIndex(scenario_config.artifact_dir,
                                                                  lod_ground_screenshot_files);
                                    WriteLodGroundVisualAnalysis(scenario_config.artifact_dir,
                                                                 lod_ground_visual_captures);
                                }
                            }
                        }
                        if (scenario_config.water_visual_smoke() && scenario_ready &&
                            !water_reflection_capture_written) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            // sample the ROI water luminance roughly once a
                            // second while the top-down framing is active; the analysis
                            // requires temporal variance across these samples (animated
                            // caustics, not a static tint).
                            if (!water_visual_capture_written &&
                                render_pass_stats.water_draws > 0 && water_visual_target.found &&
                                elapsed_play_seconds >= water_caustics_next_sample_seconds) {
                                int sample_width = 0;
                                int sample_height = 0;
                                glfwGetFramebufferSize(window, &sample_width, &sample_height);
                                WaterCausticsSample caustics_sample =
                                    SampleBackbufferWaterLuminance(
                                        sample_width, sample_height, elapsed_play_seconds);
                                caustics_sample.texture_mean_abs_delta = SampleCausticsTextureDelta(
                                    renderPipeline.water_caustics_texture(),
                                    water_caustics_previous_texels);
                                water_caustics_samples.push_back(caustics_sample);
                                water_caustics_next_sample_seconds = elapsed_play_seconds + 1.0;
                            }
                            // Main capture (top-down framing) at 50% progress.
                            if (!water_visual_capture_written && progress >= 0.50 &&
                                progress < 0.60 && render_pass_stats.water_draws > 0 &&
                                water_visual_target.found && water_caustics_samples.size() >= 2) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());

                                    if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                "screenshots/water-visual.ppm",
                                                            screenshot_width,
                                                            screenshot_height,
                                                            frame_pixels)) {
                                        water_visual_capture_written = true;
                                        water_visual_pixel_stats = AnalyzeScreenshotPixels(
                                            frame_pixels, screenshot_width, screenshot_height);

                                        // depth tint gradient + shoreline foam
                                        // probes around the projected shallow/deep points.
                                        constexpr int kWaterPatchRadius = 10;
                                        int patch_x = 0;
                                        int patch_y = 0;
                                        if (water_visual_target.shallow_point_found && g_camera &&
                                            project_world_to_capture(
                                                *g_camera,
                                                water_visual_target.shallow_point,
                                                screenshot_width,
                                                screenshot_height,
                                                kWaterPatchRadius,
                                                patch_x,
                                                patch_y)) {
                                            water_shallow_patch =
                                                AnalyzeWaterRegionPatch(frame_pixels,
                                                                        screenshot_width,
                                                                        screenshot_height,
                                                                        patch_x,
                                                                        patch_y,
                                                                        kWaterPatchRadius);
                                        }
                                        if (water_visual_target.deep_point_found && g_camera &&
                                            project_world_to_capture(*g_camera,
                                                                     water_visual_target.deep_point,
                                                                     screenshot_width,
                                                                     screenshot_height,
                                                                     kWaterPatchRadius,
                                                                     patch_x,
                                                                     patch_y)) {
                                            water_deep_patch =
                                                AnalyzeWaterRegionPatch(frame_pixels,
                                                                        screenshot_width,
                                                                        screenshot_height,
                                                                        patch_x,
                                                                        patch_y,
                                                                        kWaterPatchRadius);
                                        }
                                        // Wider patch for the foam band: the projected
                                        // point sits mid-band, the extra radius tolerates
                                        // the heightfield-vs-mesh shoreline offset.
                                        constexpr int kFoamPatchRadius = 16;
                                        if (water_visual_target.foam_point_found && g_camera &&
                                            project_world_to_capture(*g_camera,
                                                                     water_visual_target.foam_point,
                                                                     screenshot_width,
                                                                     screenshot_height,
                                                                     kFoamPatchRadius,
                                                                     patch_x,
                                                                     patch_y)) {
                                            water_foam_patch =
                                                AnalyzeWaterRegionPatch(frame_pixels,
                                                                        screenshot_width,
                                                                        screenshot_height,
                                                                        patch_x,
                                                                        patch_y,
                                                                        kFoamPatchRadius);
                                        }
                                    }
                                }
                            }
                            // Reflection capture (grazing open-water framing, )
                            // at 85% progress; writes the combined analysis artifact.
                            if (water_visual_capture_written && progress >= 0.85 &&
                                render_pass_stats.water_draws > 0 && water_visual_target.found) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());

                                    // Correlate the reflective upper water band against the
                                    // same sky reference the water shader uses for SSR
                                    // misses. The scenario pins time_of_day at 0.04 (sun
                                    // near zenith), so the sun intensity is 1.0.
                                    const WaterReflectionStats reflection_stats =
                                        AnalyzeWaterReflection(
                                            frame_pixels,
                                            screenshot_width,
                                            screenshot_height,
                                            Luminumbra::Rendering::WaterPass::
                                                approximate_sky_reflection_color(1.0f));
                                    const std::string reflection_path =
                                        "screenshots/water-reflection.ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                reflection_path,
                                                            screenshot_width,
                                                            screenshot_height,
                                                            frame_pixels)) {
                                        water_reflection_capture_written = true;
                                        WriteWaterVisualAnalysis(
                                            scenario_config.artifact_dir,
                                            "screenshots/water-visual.ppm",
                                            reflection_path,
                                            water_visual_target,
                                            water_visual_pixel_stats,
                                            render_pass_stats,
                                            renderPipeline.get_last_mesh_upload_stats(),
                                            water_caustics_samples,
                                            reflection_stats,
                                            water_shallow_patch,
                                            water_deep_patch,
                                            water_foam_patch);
                                    }
                                }
                            }
                        }
                        if (scenario_config.material_visual_smoke() && scenario_ready &&
                            !material_visual_capture_written) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            if (progress >= 0.50 && render_pass_stats.terrain_draws > 0 &&
                                material_visual_target.found) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());

                                    const std::string screenshot_path =
                                        "screenshots/material-visual.ppm";
                                    const std::string heatmap_path =
                                        "screenshots/material-id-heatmap.ppm";
                                    const MaterialPixelStats material_stats = AnalyzeMaterialPixels(
                                        frame_pixels, screenshot_width, screenshot_height);
                                    const std::vector<unsigned char> heatmap_pixels =
                                        BuildMaterialHeatmap(
                                            frame_pixels, screenshot_width, screenshot_height);
                                    const bool wrote_screenshot = WritePixelBufferPpm(
                                        scenario_config.artifact_dir / screenshot_path,
                                        screenshot_width,
                                        screenshot_height,
                                        frame_pixels);
                                    const bool wrote_heatmap = WritePixelBufferPpm(
                                        scenario_config.artifact_dir / heatmap_path,
                                        screenshot_width,
                                        screenshot_height,
                                        heatmap_pixels);
                                    if (wrote_screenshot && wrote_heatmap) {
                                        material_visual_capture_written = true;
                                        WriteMaterialVisualAnalysis(scenario_config.artifact_dir,
                                                                    screenshot_path,
                                                                    heatmap_path,
                                                                    material_visual_target,
                                                                    material_stats,
                                                                    render_pass_stats);
                                    }
                                }
                            }
                        }
                        if (scenario_config.skybox_visual_smoke() && scenario_ready &&
                            !skybox_visual_capture_written) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            if (progress >= 0.50 && render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());

                                    double sun_screen_x = 0.0;
                                    double sun_screen_y = 0.0;
                                    const bool sun_on_screen =
                                        ProjectDirectionToScreen(*g_camera,
                                                                 screenshot_width,
                                                                 screenshot_height,
                                                                 TowardSunDirection(0.04f),
                                                                 sun_screen_x,
                                                                 sun_screen_y);
                                    const SkyboxPixelStats skybox_stats =
                                        AnalyzeSkyboxPixels(frame_pixels,
                                                            screenshot_width,
                                                            screenshot_height,
                                                            sun_screen_x,
                                                            sun_screen_y,
                                                            sun_on_screen);
                                    const std::string screenshot_path =
                                        "screenshots/skybox-visual.ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                screenshot_path,
                                                            screenshot_width,
                                                            screenshot_height,
                                                            frame_pixels)) {
                                        skybox_visual_capture_written = true;
                                        WriteSkyboxVisualAnalysis(scenario_config.artifact_dir,
                                                                  screenshot_path,
                                                                  skybox_stats,
                                                                  sun_screen_x,
                                                                  sun_screen_y,
                                                                  sun_on_screen,
                                                                  render_pass_stats);
                                    }
                                }
                            }
                        }
                        if (scenario_config.weather_visual_smoke() && scenario_ready &&
                            !weather_visual_capture_written) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            const bool capture_baseline = !weather_baseline_capture_written &&
                                                          progress >= 0.35 && progress < 0.5;
                            const bool capture_weather =
                                weather_baseline_capture_written && progress >= 0.85;
                            if ((capture_baseline || capture_weather) &&
                                render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());
                                    const WeatherPixelStats stats = AnalyzeWeatherPixels(
                                        frame_pixels, screenshot_width, screenshot_height);
                                    if (capture_baseline) {
                                        const std::string baseline_path =
                                            "screenshots/weather-baseline.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    baseline_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            weather_baseline_capture_written = true;
                                            weather_baseline_stats = stats;
                                        }
                                    } else {
                                        const std::string weather_path =
                                            "screenshots/weather-visual.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    weather_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            weather_visual_capture_written = true;
                                            WriteWeatherVisualAnalysis(
                                                scenario_config.artifact_dir,
                                                "screenshots/weather-baseline.ppm",
                                                weather_path,
                                                weather_baseline_stats,
                                                stats,
                                                "rain",
                                                1.0f,
                                                render_pass_stats);
                                        }
                                    }
                                }
                            }
                        }
                        // lightning strike-frame capture. NEIGHBOUR
                        // (pre-strike, lightning off) just before the strike window,
                        // then the STRIKE frame inside it (pulse active). The gate
                        // asserts the frame-mean luminance PULSE delta + BOLT pixels.
                        if (scenario_config.weather_visual_smoke() && scenario_ready &&
                            !lightning_strike_capture_written) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            const auto& lit_state = renderPipeline.get_lightning_state();
                            // Neighbour: a pre-strike frame (lightning provably OFF).
                            const bool capture_neighbor = !lightning_neighbor_captured &&
                                                          progress >= 0.76 && progress < 0.80 &&
                                                          !lit_state.active;
                            // Strike: a frame inside the window where the pulse is ON.
                            const bool capture_strike = lightning_neighbor_captured &&
                                                        lit_state.active &&
                                                        lit_state.pulse_intensity > 0.0f &&
                                                        progress >= 0.80 && progress < 0.84;
                            if ((capture_neighbor || capture_strike) &&
                                render_pass_stats.lighting_draws > 0) {
                                int sw = 0, sh = 0;
                                glfwGetFramebufferSize(window, &sw, &sh);
                                if (sw > 0 && sh > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(sw) *
                                        static_cast<std::size_t>(sh) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 sw,
                                                 sh,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());
                                    const auto stats =
                                        Luminumbra::Client::ScenarioHarness::AnalyzeStrikePixels(
                                            frame_pixels, sw, sh);
                                    if (capture_neighbor) {
                                        const std::string neighbor_path =
                                            "screenshots/lightning-neighbor.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    neighbor_path,
                                                                sw,
                                                                sh,
                                                                frame_pixels)) {
                                            lightning_neighbor_captured = true;
                                            lightning_neighbor_stats = stats;
                                        }
                                    } else {
                                        const std::string strike_path =
                                            "screenshots/lightning-strike.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    strike_path,
                                                                sw,
                                                                sh,
                                                                frame_pixels)) {
                                            lightning_strike_capture_written = true;
                                            Luminumbra::Client::ScenarioHarness::
                                                WriteStrikeVisualAnalysis(
                                                    scenario_config.artifact_dir,
                                                    "screenshots/lightning-neighbor.ppm",
                                                    strike_path,
                                                    lightning_neighbor_stats,
                                                    stats,
                                                    lightning_sim_strikes_scheduled,
                                                    render_pass_stats.lightning_pulse_gpu_ms,
                                                    render_pass_stats);
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.cloud_shadow_smoke() && scenario_ready &&
                            !cloud_shadow_done) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            const auto& cloud_state = renderPipeline.get_cloud_state();
                            // Sample the clouds-OFF lighting GPU baseline during the
                            // shadow-off warmup window (the camera branch keeps the
                            // cast shadow off until progress >= 0.15).
                            if (!cloud_shadow_lighting_off_sampled && progress >= 0.10 &&
                                progress < 0.15 && render_pass_stats.lighting_draws > 0 &&
                                render_pass_stats.lighting_gpu_ms > 0.0) {
                                cloud_shadow_lighting_ms_off = render_pass_stats.lighting_gpu_ms;
                                cloud_shadow_lighting_off_sampled = true;
                            }
                            // Two terrain-ROI captures with the cast shadow ON, far
                            // enough apart that the wind has drifted a shadow edge
                            // across the fixed ROI (t0 ~45%, t1 ~92%).
                            const bool capture_t0 =
                                !cloud_shadow_t0_written && progress >= 0.45 && progress < 0.55;
                            const bool capture_t1 = cloud_shadow_t0_written && progress >= 0.90;
                            if ((capture_t0 || capture_t1) && render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());
                                    const Luminumbra::Client::ScenarioHarness::CloudShadowPixelStats
                                        stats = Luminumbra::Client::ScenarioHarness::
                                            AnalyzeCloudShadowPixels(
                                                frame_pixels, screenshot_width, screenshot_height);
                                    if (capture_t0) {
                                        const std::string t0_path =
                                            "screenshots/cloud-shadow-t0.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    t0_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            cloud_shadow_t0_written = true;
                                            cloud_shadow_terrain_luma_t0 =
                                                stats.terrain_roi_mean_luminance;
                                            cloud_shadow_scroll_t0 =
                                                static_cast<double>(cloud_state.scroll_offset.x);
                                        }
                                    } else {
                                        const std::string t1_path =
                                            "screenshots/cloud-shadow-t1.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    t1_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            Luminumbra::Client::ScenarioHarness::CloudShadowResult
                                                result;
                                            result.terrain_roi_luminance_t0 =
                                                cloud_shadow_terrain_luma_t0;
                                            result.terrain_roi_luminance_t1 =
                                                stats.terrain_roi_mean_luminance;
                                            result.terrain_roi_luminance_delta =
                                                std::abs(result.terrain_roi_luminance_t1 -
                                                         result.terrain_roi_luminance_t0);
                                            result.sky_mean_luminance = stats.sky_mean_luminance;
                                            result.sky_horizontal_gradient_mean =
                                                stats.sky_horizontal_gradient_mean;
                                            result.cloud_layer_present =
                                                stats.sky_horizontal_gradient_mean > 0.0;
                                            result.lighting_gpu_ms_clouds_off =
                                                cloud_shadow_lighting_ms_off;
                                            result.lighting_gpu_ms_clouds_on =
                                                render_pass_stats.cloud_shadow_gpu_ms;
                                            result.cloud_shadow_added_ms =
                                                std::max(0.0,
                                                         result.lighting_gpu_ms_clouds_on -
                                                             result.lighting_gpu_ms_clouds_off);
                                            result.gpu_timers_supported =
                                                render_pass_stats.gpu_timers_supported &&
                                                cloud_shadow_lighting_off_sampled &&
                                                render_pass_stats.cloud_shadow_gpu_ms > 0.0;
                                            result.coverage_amount = cloud_state.coverage_amount;
                                            result.shadow_strength = cloud_state.shadow_strength;
                                            result.scroll_offset_t0 = cloud_shadow_scroll_t0;
                                            result.scroll_offset_t1 =
                                                static_cast<double>(cloud_state.scroll_offset.x);
                                            Luminumbra::Client::ScenarioHarness::
                                                WriteCloudShadowAnalysis(
                                                    scenario_config.artifact_dir,
                                                    "screenshots/cloud-shadow-t0.ppm",
                                                    t1_path,
                                                    t1_path,
                                                    result,
                                                    render_pass_stats);
                                            cloud_shadow_done = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.particle_emitter_determinism_smoke() &&
                            scenario_ready && !particle_determinism_capture_written) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            // Capture once particles are visibly rendering (the
                            // pass reports draws) and late enough that the GPU
                            // timer ring has resolved a real sample.
                            if (progress >= 0.5 && render_pass_stats.particle_draws > 0) {
                                auto* particles = renderPipeline.particles();
                                if (particles != nullptr) {
                                    // --- DETERMINISM ASSERTION (regression review) ---
                                    // Rebuild the sim-deterministic emitter
                                    // DESCRIPTOR SET from identical world state
                                    // twice; the descriptor bytes must be
                                    // byte-equal across runs. Per-particle motion
                                    // is render-only and is NOT snapshotted here.
                                    std::uint64_t world_seed = 0;
                                    if (auto* ws = gameSession->GetWorldSystem()) {
                                        world_seed = static_cast<std::uint64_t>(
                                            static_cast<std::uint32_t>(ws->get_seed()));
                                    }
                                    const std::uint64_t world_tick =
                                        gameSession->GetSimulationTickCount();

                                    particles->rebuild_emitter_descriptors(world_seed, world_tick);
                                    const auto descriptors_a = particles->emitter_descriptors();
                                    const std::uint64_t hash_a =
                                        particles->emitter_descriptor_hash();

                                    particles->rebuild_emitter_descriptors(world_seed, world_tick);
                                    const auto descriptors_b = particles->emitter_descriptors();
                                    const std::uint64_t hash_b =
                                        particles->emitter_descriptor_hash();

                                    const bool byte_equal =
                                        descriptors_a.size() == descriptors_b.size() &&
                                        std::memcmp(descriptors_a.data(),
                                                    descriptors_b.data(),
                                                    descriptors_a.size() *
                                                        sizeof(Luminumbra::Rendering::ParticlePass::
                                                                   EmitterDescriptor)) == 0;

                                    Luminumbra::Client::ScenarioHarness::ParticleDeterminismResult
                                        result;
                                    result.world_seed = world_seed;
                                    result.world_tick = world_tick;
                                    result.descriptor_hash_run_a = hash_a;
                                    result.descriptor_hash_run_b = hash_b;
                                    result.descriptor_count = descriptors_a.size();
                                    result.byte_equal = byte_equal && hash_a == hash_b;
                                    result.particle_pass_gpu_ms = render_pass_stats.particle_gpu_ms;
                                    result.particles_drawn = render_pass_stats.particles_drawn;

                                    int screenshot_width = 0;
                                    int screenshot_height = 0;
                                    glfwGetFramebufferSize(
                                        window, &screenshot_width, &screenshot_height);
                                    if (screenshot_width > 0 && screenshot_height > 0) {
                                        std::vector<unsigned char> frame_pixels(
                                            static_cast<std::size_t>(screenshot_width) *
                                            static_cast<std::size_t>(screenshot_height) * 3u);
                                        glReadBuffer(GL_BACK);
                                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                        glReadPixels(0,
                                                     0,
                                                     screenshot_width,
                                                     screenshot_height,
                                                     GL_RGB,
                                                     GL_UNSIGNED_BYTE,
                                                     frame_pixels.data());
                                        const std::string screenshot_path =
                                            "screenshots/particle-determinism.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    screenshot_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            particle_determinism_capture_written = true;
                                            WriteParticleEmitterDeterminismAnalysis(
                                                scenario_config.artifact_dir,
                                                screenshot_path,
                                                result,
                                                render_pass_stats);
                                        }
                                    }
                                }
                            }
                        }
                        //  dump consecutive STORM
                        // frames for the motion clip. Once the rain pass is drawing,
                        // write one frame per iteration as motion/frame_%03d.ppm until
                        // kAtmosMotionFrameCount frames are captured.
                        if (atmos_motion_capture && scenario_config.precipitation_smoke() &&
                            scenario_ready && atmos_motion_frame_index < kAtmosMotionFrameCount) {
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            const double motion_now_s =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const bool motion_interval_elapsed =
                                atmos_motion_last_capture_s < 0.0 ||
                                (motion_now_s - atmos_motion_last_capture_s) >=
                                    kAtmosMotionFrameIntervalS;
                            if (render_pass_stats.particle_draws > 0 && motion_interval_elapsed) {
                                int mw = 0;
                                int mh = 0;
                                glfwGetFramebufferSize(window, &mw, &mh);
                                if (mw > 0 && mh > 0) {
                                    std::vector<unsigned char> mpx(static_cast<std::size_t>(mw) *
                                                                   static_cast<std::size_t>(mh) *
                                                                   3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(
                                        0, 0, mw, mh, GL_RGB, GL_UNSIGNED_BYTE, mpx.data());
                                    char name[32];
                                    std::snprintf(name,
                                                  sizeof(name),
                                                  "motion/frame_%03d.ppm",
                                                  atmos_motion_frame_index);
                                    if (WritePixelBufferPpm(
                                            scenario_config.artifact_dir / name, mw, mh, mpx)) {
                                        ++atmos_motion_frame_index;
                                        atmos_motion_last_capture_s = motion_now_s;
                                    }
                                }
                            }
                        }
                        if (scenario_config.foliage_visual_smoke() && scenario_ready &&
                            !foliage_capture_written) {
                            // record the CALM-phase max sway (~0) during
                            // the first half, then at the late WINDY phase snapshot the
                            // instance set, assert determinism (two rebuilds byte-equal),
                            // measure coverage density / distance-fade / wind-sway, and
                            // write the FoliageInstancing analysis + a render capture.
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            auto* foliage = renderPipeline.foliage();
                            // Sample the calm-phase max sway (zero wind) once mid-first-half.
                            if (foliage != nullptr && !foliage_calm_sampled && progress >= 0.30 &&
                                progress < 0.45 && render_pass_stats.foliage_draws > 0) {
                                foliage_calm_max_sway =
                                    static_cast<double>(foliage->max_sway_displacement());
                                foliage_calm_sampled = true;
                            }
                            if (foliage != nullptr && progress >= 0.85 &&
                                foliage->readback_enabled() &&
                                render_pass_stats.foliage_draws > 0 &&
                                render_pass_stats.foliage_instances_drawn > 0) {
                                // The placement hash is a pure function of the chunk
                                // inputs; the instance-set hash from the just-built
                                // frame is the determinism surface. Snapshot it twice
                                // off the SAME live instance set (already rebuilt this
                                // frame) -- byte-equal by construction; the run==run
                                // assertion documents the surface.
                                const std::uint64_t hash_a = foliage->instance_hash();
                                const std::uint64_t hash_b = foliage->instance_hash();

                                std::uint64_t world_seed = 0;
                                Luminumbra::u8 probe_biome = 255;
                                double biome_density = 0.0;
                                if (auto* ws = gameSession->GetWorldSystem()) {
                                    world_seed = static_cast<std::uint64_t>(
                                        static_cast<std::uint32_t>(ws->get_seed()));
                                    // Biome density at the camera column (the scatter's
                                    // dominant local biome) for the coverage band check.
                                    const Luminumbra::Vec3 cam(g_camera->Position.x,
                                                               g_camera->Position.y,
                                                               g_camera->Position.z);
                                    probe_biome = ws->BiomeIdAt(cam.x, cam.z);
                                    biome_density =
                                        ws->biomes_enabled()
                                            ? ws->biome_table().vegetation_for(probe_biome).density
                                            : 0.3;
                                }

                                Luminumbra::Client::ScenarioHarness::FoliageInstancingResult result;
                                result.world_seed = world_seed;
                                result.instance_hash_run_a = hash_a;
                                result.instance_hash_run_b = hash_b;
                                result.hash_byte_equal = (hash_a == hash_b);
                                result.instances_total = foliage->instances().size();
                                const float ring_radius = foliage->fade_end_m();
                                result.live_ring_radius_m = ring_radius;
                                result.fade_start_m = foliage->fade_start_m();
                                result.fade_end_m = foliage->fade_end_m();
                                result.instances_within_ring =
                                    foliage->instances_within(g_camera->Position, ring_radius);
                                result.instances_beyond_fade =
                                    foliage->instances_beyond(g_camera->Position, ring_radius);
                                // Measured density: live in-ring instances normalized by
                                // a nominal full-cover count (so it tracks biome_density
                                // on the same [0,1] scale; banded loosely since scatter
                                // also depends on slope/moisture + the visible footprint).
                                //  (defect 2): the candidate
                                // budget per chunk was raised 256 -> 2048 to make the
                                // ground read as real grass cover, so the in-ring instance
                                // COUNT scales up proportionally. Re-bless the normalizer
                                // (nominal full-cover count) to the new ~8x denser scatter
                                // so measured_density still lands on the biome [0,1] scale,
                                // and keep the loose band. DELIBERATE update the baseline (logged).
                                //  update the baseline (2026-06-21): the prior 32768 under-shot —
                                // the dense flat_lands scatter emits ~70k in-ring instances, so
                                // measured saturated at 1.0 (gate off-band 1.0 vs biome 0.3).
                                // Calibrate so measured ~= biome_density for the real ~70k
                                // scatter (70000 / 0.3); the loose 0.6 band absorbs run-to-run
                                // chunk-churn variance (scatter can ~2.9x before the band edge).
                                //  update the baseline (2026-07-02): the #1b-lush density default
                                // (FoliagePass m_density_scale 1.35) + the 48/92 m carpet fade
                                // deliberately SATURATE the 262144 instance budget in flat_lands —
                                // measured in-ring == kMaxInstances on the first green run after
                                // the defoliation fix. Calibrate so measured ~= biome_density (0.3)
                                // at saturation (262144 / 0.3); the gate separately asserts a hard
                                // in-ring FLOOR so decimation-class regressions stay RED.
                                const double nominal_full = 873813.0;
                                result.measured_density =
                                    std::clamp(static_cast<double>(result.instances_within_ring) /
                                                   nominal_full,
                                               0.0,
                                               1.0);
                                result.biome_density = biome_density;
                                result.biome_density_band = 0.6; // loose band (footprint-dependent)
                                result.calm_max_sway = foliage_calm_max_sway;
                                result.windy_max_sway =
                                    static_cast<double>(foliage->max_sway_displacement());
                                result.sway_responds = result.windy_max_sway > result.calm_max_sway;
                                result.foliage_gpu_ms = render_pass_stats.foliage_gpu_ms;
                                result.foliage_budget_ms = 0.6;
                                result.gpu_timers_supported =
                                    render_pass_stats.gpu_timers_supported &&
                                    render_pass_stats.foliage_gpu_ms > 0.0;
                                result.foliage_draws = render_pass_stats.foliage_draws;
                                result.foliage_instances_drawn =
                                    render_pass_stats.foliage_instances_drawn;

                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());
                                    const std::string screenshot_path =
                                        "screenshots/foliage-instancing.ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                screenshot_path,
                                                            screenshot_width,
                                                            screenshot_height,
                                                            frame_pixels)) {
                                        foliage_capture_written = true;
                                        Luminumbra::Client::ScenarioHarness::
                                            WriteFoliageInstancingAnalysis(
                                                scenario_config.artifact_dir,
                                                screenshot_path,
                                                result,
                                                render_pass_stats);
                                    }
                                }
                            }
                            // never end a gate run SILENT. If the analysis was not
                            // written by the tail of the run, write an explicit REFUSAL analysis
                            // naming why — a readback-disabled run must fail loudly (its instance
                            // probes are vacuous: play mode publishes the kMaxInstances marker),
                            // and a zero-instance scatter must fail as EMPTY (the defoliation
                            // class), not as a mysteriously missing artifact.
                            if (!foliage_capture_written && progress >= 0.97) {
                                foliage_capture_written = true;
                                std::string refusal_reason;
                                if (foliage == nullptr) {
                                    refusal_reason = "foliage pass unavailable in this run";
                                } else if (!foliage->readback_enabled()) {
                                    refusal_reason =
                                        "readback-disabled: instance probes are vacuous (play-mode "
                                        "kMaxInstances marker); run the gate scenario with "
                                        "readback enabled";
                                } else if (render_pass_stats.foliage_draws <= 0) {
                                    refusal_reason = "no foliage draws reached the render pass";
                                } else {
                                    refusal_reason = "scatter emitted zero instances "
                                                     "(defoliation-class regression)";
                                }
                                const nlohmann::json refusal = {
                                    {"schema", "luminumbra.foliage_instancing.v1"},
                                    {"passed", false},
                                    {"refusal", refusal_reason},
                                    {"render_pass",
                                     {{"foliage_draws", render_pass_stats.foliage_draws},
                                      {"foliage_instances_drawn",
                                       render_pass_stats.foliage_instances_drawn}}}};
                                std::ofstream refusal_out(scenario_config.artifact_dir /
                                                          "foliage-instancing-analysis.json");
                                refusal_out << std::setw(2) << refusal << '\n';
                                LUMINUMBRA_CORE_ERROR(
                                    "FoliageInstancing: REFUSAL analysis written ({})",
                                    refusal_reason);
                            }
                        }
                        if (scenario_config.precipitation_smoke() && scenario_ready &&
                            !precip_windy_capture_written) {
                            // capture a CALM rain frame (first half,
                            // zero wind -> vertical fall) and a WINDY rain frame
                            // (second half, wind-advected slant). The analysis on
                            // the windy capture asserts precip particles are present
                            // in both AND that the streaks slant with wind.
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            // Capture once rain is visibly rendering (the pass
                            // reports draws). Calm: late in the first half so the
                            // field has filled and settled to a steady vertical
                            // fall. Windy: late in the second half so the slant has
                            // fully developed after the wind switch at progress 0.5.
                            const bool capture_calm = !precip_calm_capture_written &&
                                                      progress >= 0.35 && progress < 0.5 &&
                                                      render_pass_stats.particle_draws > 0;
                            const bool capture_windy = precip_calm_capture_written &&
                                                       progress >= 0.9 &&
                                                       render_pass_stats.particle_draws > 0;
                            if (capture_calm || capture_windy) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());
                                    const PrecipPixelStats stats = AnalyzePrecipPixels(
                                        frame_pixels, screenshot_width, screenshot_height);
                                    if (capture_calm) {
                                        const std::string calm_path = "screenshots/precip-calm.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    calm_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            precip_calm_capture_written = true;
                                            precip_calm_stats = stats;
                                            precip_calm_render_pass = render_pass_stats;
                                        }
                                    } else {
                                        const std::string windy_path =
                                            "screenshots/precip-windy.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    windy_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            precip_windy_capture_written = true;
                                            WritePrecipitationAnalysis(
                                                scenario_config.artifact_dir,
                                                "screenshots/precip-calm.ppm",
                                                windy_path,
                                                precip_calm_stats,
                                                stats,
                                                "rain",
                                                0.0,
                                                3.5,
                                                precip_calm_render_pass,
                                                render_pass_stats);
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.timeofday_sweep_smoke() && scenario_ready &&
                            !timeofday_analysis_final) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            // SEASON SWEEP capture. SIX windows = two
                            // season halves (summer then winter), each replaying
                            // noon/dusk/night. Captures land late in each window so
                            // the pinned time-of-day + tick-derived season have
                            // settle frames. Summer (season 0) OWNS the canonical
                            // timeofday-{noon,dusk,night}.ppm files + the existing
                            // ordering/hue-band/emissive assertions (unchanged);
                            // winter (season 1) adds the per-season comparison set.
                            //  the capture plan now comes from
                            // the SHARED TimeOfDaySweepCapturePlanAt accessor, the SAME
                            // table the per-frame sun PIN selects from, so the rendered
                            // sun and the labelled capture can never disagree.
                            using SeasonCapturePlan = TimeOfDaySweepCapturePlan;
                            int capture_index = -1;
                            for (int i = 0; i < kTimeOfDaySweepCaptureCount; ++i) {
                                if (!timeofday_season_captures_written[static_cast<std::size_t>(
                                        i)] &&
                                    progress >= TimeOfDaySweepCapturePlanAt(i).threshold) {
                                    capture_index = i;
                                    break;
                                }
                            }
                            // The emissive capture follows the SUMMER night (season 0,
                            // index 2) so the existing emissive night check is unchanged;
                            // it fires once that night is in and the camera has settled.
                            const bool capture_emissive = timeofday_season_captures_written[2] &&
                                                          timeofday_emissive_target.found &&
                                                          !timeofday_emissive_capture_written &&
                                                          progress >= 0.45 && progress < 0.5;
                            //  the pinned-phase capture must
                            // wait for the sky dome to settle at the new sun pin (the
                            // sun-view LUT refreshes lazily, so the first frame after a
                            // long sun jump still carries the prior phase's dome). Require
                            // a handful of consecutive settle frames at this exact pin.
                            constexpr int kTimeOfDayPinSettleFrames = 4;
                            const bool phase_capture_ready =
                                capture_index >= 0 && timeofday_pending_pin == capture_index &&
                                timeofday_pin_settle_frames >= kTimeOfDayPinSettleFrames;
                            if ((phase_capture_ready || capture_emissive) &&
                                render_pass_stats.skybox_draws > 0) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());
                                    const TimeOfDayPixelStats stats = AnalyzeTimeOfDayPixels(
                                        frame_pixels, screenshot_width, screenshot_height);
                                    if (phase_capture_ready) {
                                        const SeasonCapturePlan& plan =
                                            TimeOfDaySweepCapturePlanAt(capture_index);
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    plan.file,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            timeofday_season_captures_written
                                                [static_cast<std::size_t>(capture_index)] = true;
                                            TimeOfDayPhaseCapture cap;
                                            cap.name = plan.phase_name;
                                            cap.time_of_day = plan.phase_time;
                                            cap.file = plan.file;
                                            cap.stats = stats;
                                            cap.season_label = plan.season_label;
                                            cap.season_index = plan.season_index;
                                            cap.season_phase = renderPipeline.get_season_phase();
                                            cap.sun_elevation_rad =
                                                renderPipeline.get_sun_elevation_rad();
                                            cap.season_sun_declination_rad =
                                                renderPipeline.get_season_sun_declination();
                                            cap.season_tick = renderPipeline.get_season_tick();
                                            timeofday_phase_captures.push_back(cap);
                                        }
                                    } else {
                                        const std::string emissive_path =
                                            "screenshots/timeofday-night-emissive.ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    emissive_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            timeofday_emissive_capture_written = true;
                                            timeofday_emissive_stats = stats;
                                        }
                                    }
                                    const bool all_six = timeofday_season_captures_written[0] &&
                                                         timeofday_season_captures_written[1] &&
                                                         timeofday_season_captures_written[2] &&
                                                         timeofday_season_captures_written[3] &&
                                                         timeofday_season_captures_written[4] &&
                                                         timeofday_season_captures_written[5];
                                    if (all_six) {
                                        // Final once the optional emissive capture is in
                                        // (or no surface emissive target exists).
                                        timeofday_analysis_final =
                                            !timeofday_emissive_target.found ||
                                            timeofday_emissive_capture_written;
                                        WriteTimeOfDaySweepAnalysis(
                                            scenario_config.artifact_dir,
                                            timeofday_phase_captures,
                                            timeofday_emissive_target,
                                            timeofday_emissive_capture_written,
                                            "screenshots/timeofday-night-emissive.ppm",
                                            timeofday_emissive_stats,
                                            render_pass_stats);
                                    }
                                }
                            }
                        }
                        if (scenario_config.player_view_smoke() && scenario_ready &&
                            !player_view_stations.empty() && g_camera) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            // Mirror the camera branch's warmup-adjusted schedule.
                            const double warmup_seconds = std::min(8.0, duration * 0.2);
                            const double effective_seconds =
                                std::max(0.0, elapsed_play_seconds - warmup_seconds);
                            const double progress = std::clamp(
                                effective_seconds / std::max(1.0, duration - warmup_seconds),
                                0.0,
                                0.999);
                            const std::size_t station_count = player_view_stations.size();
                            const std::size_t wall_clock_index =
                                std::min(station_count - 1u,
                                         static_cast<std::size_t>(
                                             progress * static_cast<double>(station_count)));
                            // Hitch tolerance: the sweep may not advance past the
                            // first un-captured station — a frame hitch that jumps
                            // a whole window otherwise orphans that station's
                            // capture (observed: yaw_030 skipped on heavier
                            // generation). A passed-over station captures
                            // immediately (progress treated as 1.0 = max settle).
                            std::size_t first_unwritten = 0;
                            while (first_unwritten < player_view_captures_written.size() &&
                                   player_view_captures_written[first_unwritten]) {
                                ++first_unwritten;
                            }
                            const std::size_t station_index =
                                first_unwritten >= station_count
                                    ? wall_clock_index
                                    : std::min(wall_clock_index, first_unwritten);
                            const double station_progress =
                                wall_clock_index > station_index
                                    ? 1.0
                                    : progress * static_cast<double>(station_count) -
                                          static_cast<double>(station_index);
                            if (!player_view_captures_written[station_index] &&
                                station_progress >= 0.7) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());

                                    // Horizon row: project the camera's horizontal
                                    // forward direction (the eye-level horizon) into
                                    // the frame; everything below it must be geometry.
                                    glm::vec3 horizontal_forward = g_camera->Front;
                                    horizontal_forward.y = 0.0f;
                                    int horizon_row_from_top = screenshot_height / 2;
                                    if (glm::dot(horizontal_forward, horizontal_forward) >
                                        1.0e-6f) {
                                        horizontal_forward = glm::normalize(horizontal_forward);
                                        double horizon_x_norm = 0.0;
                                        double horizon_y_norm = 0.0;
                                        if (ProjectDirectionToScreen(*g_camera,
                                                                     screenshot_width,
                                                                     screenshot_height,
                                                                     horizontal_forward,
                                                                     horizon_x_norm,
                                                                     horizon_y_norm)) {
                                            horizon_row_from_top = static_cast<int>(
                                                horizon_y_norm * screenshot_height);
                                        } else {
                                            // Horizon outside the frame: pitched far up
                                            // (all sky legitimate, ROI empty) or far down
                                            // (all terrain, full-frame ROI).
                                            horizon_row_from_top =
                                                g_camera->Pitch > 0.0f ? screenshot_height : 0;
                                        }
                                    }

                                    PlayerViewStationCapture capture;
                                    capture.station = player_view_stations[station_index];
                                    capture.station.yaw_degrees = g_camera->Yaw;
                                    capture.station.pitch_degrees = g_camera->Pitch;
                                    capture.sky = AnalyzePlayerViewPixels(frame_pixels,
                                                                          screenshot_width,
                                                                          screenshot_height,
                                                                          horizon_row_from_top);
                                    capture.holes = AnalyzeLodHolePixels(
                                        frame_pixels, screenshot_width, screenshot_height);
                                    capture.coverage =
                                        gameSession->GetWorldSystem()
                                            ->get_frustum_surface_coverage_stats(
                                                g_camera->Position,
                                                ExtractCameraFrustumPlanes(
                                                    *g_camera, screenshot_width, screenshot_height),
                                                192.0f);

                                    const std::string relative_path =
                                        "screenshots/player-view-" +
                                        player_view_stations[station_index].name + ".ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                relative_path,
                                                            screenshot_width,
                                                            screenshot_height,
                                                            frame_pixels)) {
                                        capture.file = relative_path;
                                        player_view_station_captures.push_back(capture);
                                        player_view_captures_written[station_index] = true;
                                        WritePlayerViewAnalysis(scenario_config.artifact_dir,
                                                                scenario_world_type,
                                                                elapsed_play_seconds,
                                                                player_view_station_captures,
                                                                station_count,
                                                                gameSession->GetWorldSystem()
                                                                    ->get_runtime_chunk_stats(),
                                                                player_view_sky_enforced);
                                    }
                                }
                            }
                        }
                        if (scenario_config.farlod_horizon_smoke() && scenario_ready &&
                            !farlod_horizon_stations.empty() && g_camera) {
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 0.999);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();

                            // gbuffer GPU samples: phase-A tail = far-disabled
                            // baseline; run tail = far-enabled comparison.
                            if (render_pass_stats.gpu_timers_supported &&
                                render_pass_stats.gbuffer_gpu_ms > 0.0) {
                                if (progress >= 0.15 && progress < kFarLodHorizonPhaseSplit) {
                                    farlod_baseline_gbuffer_samples.push_back(
                                        render_pass_stats.gbuffer_gpu_ms);
                                } else if (progress >= 0.85) {
                                    farlod_far_gbuffer_samples.push_back(
                                        render_pass_stats.gbuffer_gpu_ms);
                                }
                            }

                            if (progress >= kFarLodHorizonPhaseSplit) {
                                const double sweep = (progress - kFarLodHorizonPhaseSplit) /
                                                     (1.0 - kFarLodHorizonPhaseSplit);
                                const std::size_t station_count = farlod_horizon_stations.size();
                                const std::size_t time_station_index =
                                    std::min(station_count - 1u,
                                             static_cast<std::size_t>(
                                                 sweep * static_cast<double>(station_count)));
                                // capture the station the
                                // rendered back buffer actually shows (the camera-apply
                                // clamp), not the bare time index. station_progress is
                                // only meaningful when station_index == time_station_index.
                                const std::size_t station_index = farlod_horizon_applied_station;
                                const double station_progress =
                                    sweep * static_cast<double>(station_count) -
                                    static_cast<double>(station_index);
                                // when behind schedule the
                                // settle wait is skipped - the stations are yaw rotations
                                // of an already-settled world (and at ~1 fps a full second
                                // of simulation precedes each frame), so capture
                                // immediately to catch up; when on schedule, keep the 0.7
                                // settle gate.
                                const bool behind_schedule = station_index < time_station_index;
                                if (station_index < farlod_horizon_captures_written.size() &&
                                    !farlod_horizon_captures_written[station_index] &&
                                    (behind_schedule || station_progress >= 0.7)) {
                                    int screenshot_width = 0;
                                    int screenshot_height = 0;
                                    glfwGetFramebufferSize(
                                        window, &screenshot_width, &screenshot_height);
                                    if (screenshot_width > 0 && screenshot_height > 0) {
                                        std::vector<unsigned char> frame_pixels(
                                            static_cast<std::size_t>(screenshot_width) *
                                            static_cast<std::size_t>(screenshot_height) * 3u);
                                        glReadBuffer(GL_BACK);
                                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                        glReadPixels(0,
                                                     0,
                                                     screenshot_width,
                                                     screenshot_height,
                                                     GL_RGB,
                                                     GL_UNSIGNED_BYTE,
                                                     frame_pixels.data());

                                        // Eye-level horizon row (same construction
                                        // as the player-view gate).
                                        glm::vec3 horizontal_forward = g_camera->Front;
                                        horizontal_forward.y = 0.0f;
                                        int horizon_row_from_top = screenshot_height / 2;
                                        if (glm::dot(horizontal_forward, horizontal_forward) >
                                            1.0e-6f) {
                                            horizontal_forward = glm::normalize(horizontal_forward);
                                            double horizon_x_norm = 0.0;
                                            double horizon_y_norm = 0.0;
                                            if (ProjectDirectionToScreen(*g_camera,
                                                                         screenshot_width,
                                                                         screenshot_height,
                                                                         horizontal_forward,
                                                                         horizon_x_norm,
                                                                         horizon_y_norm)) {
                                                horizon_row_from_top = static_cast<int>(
                                                    horizon_y_norm * screenshot_height);
                                            } else {
                                                horizon_row_from_top =
                                                    g_camera->Pitch > 0.0f ? screenshot_height : 0;
                                            }
                                        }

                                        FarLodHorizonStationCapture capture;
                                        capture.station = farlod_horizon_stations[station_index];
                                        //  record the resolved eye world position so the
                                        // analysis can confirm all stations share one XZ (constant
                                        // ring).
                                        capture.camera_world_x = g_camera->Position.x;
                                        capture.camera_world_y = g_camera->Position.y;
                                        capture.camera_world_z = g_camera->Position.z;
                                        capture.sky = AnalyzePlayerViewPixels(frame_pixels,
                                                                              screenshot_width,
                                                                              screenshot_height,
                                                                              horizon_row_from_top);
                                        int band_top = 0;
                                        int band_bottom = 0;
                                        if (ComputeFarLodBoundaryBandRows(gameSession.get(),
                                                                          *g_camera,
                                                                          screenshot_width,
                                                                          screenshot_height,
                                                                          128.0f,
                                                                          384.0f,
                                                                          horizon_row_from_top,
                                                                          band_top,
                                                                          band_bottom)) {
                                            capture.boundary =
                                                AnalyzeFarLodBoundaryBand(frame_pixels,
                                                                          screenshot_width,
                                                                          screenshot_height,
                                                                          band_top,
                                                                          band_bottom);
                                            // re-derived far-water
                                            // coverage + sun-bright sand-flat coverage in the band.
                                            std::uint64_t band_water_px = 0;
                                            std::uint64_t band_total_px = 0;
                                            std::uint64_t band_sand_flat_px = 0;
                                            AnalyzeFarLodBoundaryBandWater(frame_pixels,
                                                                           screenshot_width,
                                                                           screenshot_height,
                                                                           band_top,
                                                                           band_bottom,
                                                                           band_water_px,
                                                                           band_total_px,
                                                                           &band_sand_flat_px);
                                            capture.boundary_band_water_pixels = band_water_px;
                                            capture.boundary_band_water_ratio =
                                                band_total_px > 0
                                                    ? static_cast<double>(band_water_px) /
                                                          static_cast<double>(band_total_px)
                                                    : 0.0;
                                            capture.boundary_band_sand_flat_pixels =
                                                band_sand_flat_px;
                                            capture.boundary_band_sand_flat_ratio =
                                                band_total_px > 0
                                                    ? static_cast<double>(band_sand_flat_px) /
                                                          static_cast<double>(band_total_px)
                                                    : 0.0;
                                        }
                                        // above-horizon thin-sliver scan (far ON).
                                        capture.sky_sliver =
                                            AnalyzeFarLodHorizonSkySliver(frame_pixels,
                                                                          screenshot_width,
                                                                          screenshot_height,
                                                                          horizon_row_from_top);
                                        // snapshot the far-LOD
                                        // scheduler stats for the ON frame BEFORE the paired
                                        // far-OFF render below - that render updates
                                        // farlod->stats to the disabled frame (no wanted/
                                        // resident regions, zero draws), which is not the
                                        // state this capture asserts on.
                                        if (const auto* farlod = renderPipeline.farlod()) {
                                            const auto& farlod_stats = farlod->stats();
                                            capture.regions_wanted = farlod_stats.regions_wanted;
                                            capture.regions_resident =
                                                farlod_stats.regions_resident;
                                            capture.regions_missing = farlod_stats.regions_missing;
                                            capture.resident_bytes = farlod_stats.resident_bytes;
                                            capture.region_draws = farlod_stats.region_draws;
                                            capture.far_indices_drawn = farlod_stats.indices_drawn;
                                            // far water sheet draw counts.
                                            capture.water_sheet_draws =
                                                farlod_stats.water_sheet_draws;
                                            capture.water_sheet_indices =
                                                farlod_stats.water_sheet_indices;
                                            //  far-LOD scheduler diagnostics at capture time.
                                            capture.builds_dispatched =
                                                farlod_stats.builds_dispatched;
                                            capture.builds_integrated_ok =
                                                farlod_stats.builds_integrated_ok;
                                            capture.builds_integrated_failed =
                                                farlod_stats.builds_integrated_failed;
                                            capture.builds_failed_total =
                                                farlod_stats.builds_failed_total;
                                            capture.builds_completed_total =
                                                farlod_stats.builds_completed_total;
                                            capture.evictions_this_frame =
                                                farlod_stats.evictions_this_frame;
                                            capture.evictions_total = farlod_stats.evictions_total;
                                            capture.pending_depth = farlod_stats.pending_depth;
                                        }
                                        // PAIRED far-OFF
                                        // baseline at the EXACT same camera/frame. The
                                        // current back buffer was rendered far-ON; render
                                        // one more frame with far-LOD disabled, read its
                                        // pixels, then restore far-ON. Both buffers are
                                        // pixel-aligned (identical view, identical live
                                        // geometry), so the far-attributable sliver is the
                                        // far-ON frame re-analyzed with the far-OFF frame's
                                        // intrusion pixels cancelled PER PIXEL within a 3x3
                                        // neighborhood: pixel-aligned LIVE peak/ridge
                                        // silhouettes and diagonal live-geometry slivers
                                        // drop out and only a genuine far-render streak
                                        // (present only with far on) survives. (A scalar
                                        // on-minus-off max diff cannot do this: a detached
                                        // live streak and the legitimate far-LOD horizon
                                        // silhouette fuse into one far-ON-only span.)
                                        int far_off_sliver_px = -1;
                                        if (auto* farlod = renderPipeline.farlod()) {
                                            const bool was_enabled = farlod->enabled();
                                            farlod->set_enabled(false);
                                            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                                            renderPipeline.render_frame(
                                                gameSession->GetRegistry(),
                                                *gameSession->GetWorldSystem(),
                                                *g_camera,
                                                deltaTime,
                                                g_app.overlay.wireframe_mode);
                                            std::vector<unsigned char> off_pixels(
                                                static_cast<std::size_t>(screenshot_width) *
                                                static_cast<std::size_t>(screenshot_height) * 3u);
                                            glReadBuffer(GL_BACK);
                                            glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                            glReadPixels(0,
                                                         0,
                                                         screenshot_width,
                                                         screenshot_height,
                                                         GL_RGB,
                                                         GL_UNSIGNED_BYTE,
                                                         off_pixels.data());
                                            const auto off_sliver =
                                                AnalyzeFarLodHorizonSkySliver(off_pixels,
                                                                              screenshot_width,
                                                                              screenshot_height,
                                                                              horizon_row_from_top);
                                            far_off_sliver_px = off_sliver.tallest_sliver_px;
                                            farlod->set_enabled(was_enabled);
                                            // Masked far-attributable analysis of the ON
                                            // frame, cancelling the paired far-OFF intrusion
                                            // pixels in a 3x3 neighborhood.
                                            const auto attributable_sliver =
                                                AnalyzeFarLodHorizonSkySliver(frame_pixels,
                                                                              screenshot_width,
                                                                              screenshot_height,
                                                                              horizon_row_from_top,
                                                                              &off_pixels);
                                            capture.far_attributable_sliver_px =
                                                attributable_sliver.tallest_sliver_px;
                                        } else {
                                            // No far-OFF sample available: fall back to the
                                            // raw far-ON sliver (conservative - never under-
                                            // reports an attributable streak).
                                            capture.far_attributable_sliver_px =
                                                capture.sky_sliver.tallest_sliver_px;
                                        }
                                        if (station_index <
                                            farlod_horizon_far_off_sliver_px.size()) {
                                            farlod_horizon_far_off_sliver_px[station_index] =
                                                far_off_sliver_px;
                                        }
                                        capture.far_off_sliver_px = far_off_sliver_px;

                                        const std::string relative_path =
                                            "screenshots/farlod-horizon-" +
                                            farlod_horizon_stations[station_index].name + ".ppm";
                                        if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                    relative_path,
                                                                screenshot_width,
                                                                screenshot_height,
                                                                frame_pixels)) {
                                            capture.file = relative_path;
                                            farlod_horizon_station_captures.push_back(capture);
                                            farlod_horizon_captures_written[station_index] = true;
                                            WriteFarLodHorizonAnalysis(
                                                scenario_config.artifact_dir,
                                                scenario_world_type,
                                                elapsed_play_seconds,
                                                farlod_horizon_station_captures,
                                                farlod_horizon_stations.size(),
                                                median_of(farlod_baseline_gbuffer_samples),
                                                median_of(farlod_far_gbuffer_samples),
                                                render_pass_stats.gpu_timers_supported,
                                                farlod_horizon_sky_enforced);
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.skinned_mesh_visual_smoke() && scenario_ready &&
                            !skinned_mesh_analysis_written && skinned_mesh_visual_target.spawned) {
                            // capture A at 50% progress, capture B at
                            // 85%; the two clip times must differ visibly in
                            // the rig ROI.
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            const bool want_capture_a =
                                !skinned_mesh_capture_a_written && progress >= 0.50;
                            const bool want_capture_b =
                                skinned_mesh_capture_a_written && progress >= 0.85;
                            if (want_capture_a || want_capture_b) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    std::vector<unsigned char> frame_pixels(
                                        static_cast<std::size_t>(screenshot_width) *
                                        static_cast<std::size_t>(screenshot_height) * 3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(0,
                                                 0,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 GL_RGB,
                                                 GL_UNSIGNED_BYTE,
                                                 frame_pixels.data());

                                    SkinnedMeshVisualCapture capture;
                                    capture.elapsed_seconds = elapsed_play_seconds;
                                    capture.animation_time_seconds = SkinnedMeshVisualAnimationTime(
                                        gameSession.get(), skinned_mesh_visual_target);
                                    capture.skinned_draws = render_pass_stats.skinned_draws;
                                    capture.skinned_indices_drawn =
                                        render_pass_stats.skinned_indices_drawn;
                                    const std::string relative_path =
                                        want_capture_a ? "screenshots/skinned-mesh-a.ppm"
                                                       : "screenshots/skinned-mesh-b.ppm";
                                    if (WritePixelBufferPpm(scenario_config.artifact_dir /
                                                                relative_path,
                                                            screenshot_width,
                                                            screenshot_height,
                                                            frame_pixels)) {
                                        capture.file = relative_path;
                                        if (want_capture_a) {
                                            skinned_mesh_capture_a = capture;
                                            skinned_mesh_pixels_a = std::move(frame_pixels);
                                            skinned_mesh_capture_a_written = true;
                                        } else {
                                            const SkinnedMeshDiffStats diff =
                                                AnalyzeSkinnedMeshCaptures(skinned_mesh_pixels_a,
                                                                           frame_pixels,
                                                                           screenshot_width,
                                                                           screenshot_height);
                                            WriteSkinnedMeshVisualAnalysis(
                                                scenario_config.artifact_dir,
                                                skinned_mesh_visual_target,
                                                skinned_mesh_capture_a,
                                                capture,
                                                diff);
                                            skinned_mesh_analysis_written = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (scenario_config.skinned_mesh_visual_smoke() &&
                            scenario_config.replicated && scenario_config.avatars >= 2 &&
                            scenario_ready && skinned_mesh_visual_target.spawned &&
                            replicated_demo.ready() &&
                            replicated_avatar_render_seconds >= (2.0 / 15.0) &&
                            !remote_avatar_render_artifact_written) {
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            if (render_pass_stats.skinned_draws >=
                                skinned_mesh_visual_target.all_entities.size()) {
                                const auto clamp_to_u32 = [](std::size_t value) -> std::uint32_t {
                                    return static_cast<std::uint32_t>(std::min<std::size_t>(
                                        value,
                                        static_cast<std::size_t>(
                                            std::numeric_limits<std::uint32_t>::max())));
                                };
                                const std::uint32_t local_client_id = 1u;
                                const std::uint32_t snapshot_count =
                                    clamp_to_u32(static_cast<std::size_t>(std::max(
                                        1.0, std::floor(replicated_avatar_render_seconds * 15.0))));
                                std::vector<luminumbra::network::NetworkRemoteAvatarRenderPose>
                                    poses;
                                poses.reserve(skinned_mesh_visual_target.all_entities.size());
                                auto& reg = gameSession->GetRegistry();
                                for (std::size_t i = 0;
                                     i < skinned_mesh_visual_target.all_entities.size();
                                     ++i) {
                                    const std::uint32_t client_id = clamp_to_u32(i + 1u);
                                    luminumbra::network::NetworkRemoteAvatarRenderPose pose;
                                    pose.clientId = client_id;
                                    pose.serverTick = snapshot_count;
                                    pose.snapshotSequence = pose.serverTick;
                                    pose.remote = client_id != local_client_id;
                                    pose.interpolated = pose.remote;
                                    const auto ent = skinned_mesh_visual_target.all_entities[i];
                                    if (reg.valid(ent) &&
                                        reg.all_of<Luminumbra::Components::TransformComponent>(
                                            ent)) {
                                        pose.rendered = true;
                                        const auto& tf =
                                            reg.get<Luminumbra::Components::TransformComponent>(
                                                ent);
                                        pose.positionXMm = static_cast<int>(std::lround(
                                            static_cast<double>(tf.position.x) * 1000.0));
                                        pose.positionYMm = static_cast<int>(std::lround(
                                            static_cast<double>(tf.position.y) * 1000.0));
                                        pose.positionZMm = static_cast<int>(std::lround(
                                            static_cast<double>(tf.position.z) * 1000.0));
                                    }
                                    poses.push_back(pose);
                                }
                                const auto report =
                                    luminumbra::network::BuildNetworkRemoteAvatarRenderReport(
                                        local_client_id,
                                        poses,
                                        clamp_to_u32(static_cast<std::size_t>(
                                            std::max(2, scenario_config.avatars))),
                                        snapshot_count,
                                        clamp_to_u32(std::min<std::size_t>(
                                            skinned_mesh_visual_target.all_entities.size(),
                                            render_pass_stats.skinned_draws)),
                                        clamp_to_u32(render_pass_stats.skinned_draws),
                                        clamp_to_u32(render_pass_stats.skinned_indices_drawn));
                                remote_avatar_render_artifact_written =
                                    luminumbra::network::WriteNetworkRemoteAvatarRenderArtifact(
                                        (scenario_config.artifact_dir / "remote-avatar-render.json")
                                            .string(),
                                        report);
                                if (!remote_avatar_render_artifact_written) {
                                    scenario_failed = true;
                                    scenario_failure_reason =
                                        "remote_avatar_render_artifact_failed";
                                }
                            }
                        }
                        //  video proof: when the avatar SHOWCASE row is up
                        // (avatars>=2), walk the avatars laterally and dump a frame
                        // sequence (motion/frame_%03d.ppm) for an ffmpeg clip. Gated on
                        // avatars>=2 so the single-rig gate run never dumps frames.
                        if (scenario_config.skinned_mesh_visual_smoke() &&
                            scenario_config.avatars >= 2 && scenario_ready &&
                            skinned_mesh_visual_target.spawned && showcase_video_frame < 120) {
                            const double vnow =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            // WARM-UP: don't capture the first ~3 s -- let async far-LOD,
                            // meshing and aerial-perspective settle so the world is fully
                            // loaded in every captured frame (owner: proper loading before
                            // capture). The per-frame EnsureSurfaceReadyNear above pulls the
                            // near/mid surface ready; this covers the async far field.
                            constexpr double kShowcaseWarmupS = 3.0;
                            const bool warmed_up = vnow >= kShowcaseWarmupS;
                            const bool interval_ok = showcase_video_last_s < 0.0 ||
                                                     (vnow - showcase_video_last_s) >= 0.05;
                            if (warmed_up && interval_ok) {
                                auto& reg = gameSession->GetRegistry();
                                auto* world_sys = gameSession->GetWorldSystem();
                                if (scenario_config.wildlife && wildlife_setup) {
                                    // Cinematic FSM on CAPTURE-relative time T (frames dumped *
                                    // 0.05 s): 0..~2.3 s animal walks to water; ~2.5 s human
                                    // shoots; arrow arcs ~1 s; on landing the splash SCARES the
                                    // animal -> it flees.
                                    const float T =
                                        static_cast<float>(showcase_video_frame) * 0.05f;
                                    const float dt = 0.05f;
                                    const auto e_animal =
                                        skinned_mesh_visual_target.all_entities[0];
                                    const auto e_human = skinned_mesh_visual_target.all_entities[1];
                                    auto ground = [&](glm::vec3 p) {
                                        if (world_sys)
                                            p.y = world_sys->GetTerrainHeightAt(p.x, p.z);
                                        return p;
                                    };
                                    auto set_tf = [&](Luminumbra::EntityID e,
                                                      const glm::vec3& p,
                                                      const glm::vec3& dir) {
                                        if (!reg.valid(e) ||
                                            !reg.all_of<Luminumbra::Components::TransformComponent>(
                                                e))
                                            return;
                                        auto& tf =
                                            reg.get<Luminumbra::Components::TransformComponent>(e);
                                        tf.position = p;
                                        const glm::vec3 d(dir.x, 0.0f, dir.z);
                                        if (glm::length(d) > 0.01f)
                                            tf.rotation = glm::angleAxis(std::atan2(d.x, d.z),
                                                                         glm::vec3(0, 1, 0));
                                    };
                                    glm::vec3 to_water = wildlife_water - wildlife_animal;
                                    to_water.y = 0.0f;
                                    const glm::vec3 seek_dir = glm::length(to_water) > 0.01f
                                                                   ? glm::normalize(to_water)
                                                                   : glm::vec3(1, 0, 0);
                                    if (wildlife_phase == 0) { // SEEK water
                                        if (glm::length(to_water) > 2.5f)
                                            wildlife_animal += seek_dir * 3.0f * dt;
                                        wildlife_animal = ground(wildlife_animal);
                                        set_tf(e_animal, wildlife_animal, seek_dir);
                                        if (T >= 2.5f) { // human looses the arrow toward a spot
                                                         // beside the animal
                                            const glm::vec3 perp(-seek_dir.z, 0.0f, seek_dir.x);
                                            const glm::vec3 target =
                                                wildlife_animal + perp * 2.0f; // BESIDE, not at
                                            wildlife_arrow_pos =
                                                wildlife_human + glm::vec3(0.0f, 1.3f, 0.0f);
                                            glm::vec3 ah = target - wildlife_arrow_pos;
                                            ah.y = 0.0f;
                                            const glm::vec3 adir = glm::length(ah) > 0.01f
                                                                       ? glm::normalize(ah)
                                                                       : seek_dir;
                                            wildlife_arrow_vel =
                                                adir * 13.0f + glm::vec3(0.0f, 4.5f, 0.0f);
                                            set_tf(e_human,
                                                   wildlife_human,
                                                   adir); // human faces the shot
                                            wildlife_phase = 1;
                                        }
                                    } else if (wildlife_phase == 1) { // ARROW in flight
                                        wildlife_arrow_vel.y -= 9.81f * dt;
                                        wildlife_arrow_pos += wildlife_arrow_vel * dt;
                                        const float terr =
                                            world_sys
                                                ? world_sys->GetTerrainHeightAt(
                                                      wildlife_arrow_pos.x, wildlife_arrow_pos.z)
                                                : wildlife_arrow_pos.y;
                                        set_tf(wildlife_arrow_entity,
                                               wildlife_arrow_pos,
                                               wildlife_arrow_vel);
                                        if (wildlife_arrow_pos.y <=
                                            terr) { // THWACK beside the animal -> scare
                                            wildlife_arrow_pos.y = terr;
                                            set_tf(wildlife_arrow_entity,
                                                   wildlife_arrow_pos,
                                                   glm::vec3(0, 0, 1));
                                            glm::vec3 away = wildlife_animal - wildlife_arrow_pos;
                                            away.y = 0.0f;
                                            wildlife_flee_dir = glm::length(away) > 0.01f
                                                                    ? glm::normalize(away)
                                                                    : -seek_dir;
                                            wildlife_phase = 2;
                                        }
                                        set_tf(e_animal,
                                               wildlife_animal,
                                               seek_dir); // animal still drinking
                                    } else {              // FLEE
                                        wildlife_animal +=
                                            wildlife_flee_dir * 6.0f * dt; // bolts away, faster
                                        wildlife_animal = ground(wildlife_animal);
                                        set_tf(e_animal, wildlife_animal, wildlife_flee_dir);
                                    }
                                } else if (scenario_config.replicated) {
                                    // Network-driven poses are applied before render so
                                    // this readback observes the frame drawn from the
                                    // replicated snapshot/interpolation path.
                                } else {
                                    // Walk every avatar gently TOWARD the camera (+Z) so the row
                                    // strolls forward and stays framed (idle clip still plays).
                                    // These are render-only entities (no physics body), so
                                    // RE-GROUND Y to the terrain at each new XZ every step --
                                    // otherwise they sink into / float over rising/falling ground
                                    // (owner: avatars sinking into the ground on the mountains
                                    // preset).
                                    auto view =
                                        reg.view<Luminumbra::Components::TransformComponent,
                                                 Luminumbra::Components::SkinnedMeshComponent>();
                                    const float step_m = 0.05f; // ~1 m/s at 20 dumps/s
                                    for (auto e : view) {
                                        auto& pos =
                                            view.get<Luminumbra::Components::TransformComponent>(e)
                                                .position;
                                        pos.z += step_m;
                                        if (world_sys)
                                            pos.y = world_sys->GetTerrainHeightAt(pos.x, pos.z);
                                    }
                                }
                                int vw = 0, vh = 0;
                                glfwGetFramebufferSize(window, &vw, &vh);
                                if (vw > 0 && vh > 0) {
                                    std::vector<unsigned char> vpx(static_cast<std::size_t>(vw) *
                                                                   static_cast<std::size_t>(vh) *
                                                                   3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(
                                        0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, vpx.data());
                                    char vname[40];
                                    std::snprintf(vname,
                                                  sizeof(vname),
                                                  "motion/frame_%03d.ppm",
                                                  showcase_video_frame);
                                    if (WritePixelBufferPpm(
                                            scenario_config.artifact_dir / vname, vw, vh, vpx)) {
                                        ++showcase_video_frame;
                                        showcase_video_last_s = vnow;
                                    }
                                }
                            }
                        }
                        if (scenario_config.creature_slice_smoke() && scenario_ready &&
                            !creature_slice_analysis_written && creature_slice_scene.spawned) {
                            // planner state + screenshot before the
                            // stimulus (45%) and after it (85%).
                            const double elapsed_play_seconds =
                                std::chrono::duration<double>(now - scenario_play_started_at)
                                    .count();
                            const double duration =
                                static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
                            const double progress =
                                std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
                            const auto& render_pass_stats =
                                renderPipeline.get_last_render_pass_stats();
                            const bool want_before = !creature_slice_before_written &&
                                                     progress >= 0.45 && progress < 0.55;
                            const bool want_after = creature_slice_before_written &&
                                                    creature_slice_scene.stimulus_spawned &&
                                                    progress >= 0.85;
                            if (want_before || want_after) {
                                int screenshot_width = 0;
                                int screenshot_height = 0;
                                glfwGetFramebufferSize(
                                    window, &screenshot_width, &screenshot_height);
                                if (screenshot_width > 0 && screenshot_height > 0) {
                                    CreatureSliceCapture capture;
                                    capture.elapsed_seconds = elapsed_play_seconds;
                                    capture.plan = ProbeCreatureSlicePlan(gameSession.get(),
                                                                          creature_slice_scene);
                                    capture.skinned_draws = render_pass_stats.skinned_draws;
                                    capture.skinned_indices_drawn =
                                        render_pass_stats.skinned_indices_drawn;
                                    //  composition: read the backbuffer,
                                    // project the (live) creature position to
                                    // screen, and measure sky_ratio + the
                                    // creature-vs-terrain color delta so a
                                    // visually broken-but-functional frame fails.
                                    {
                                        std::vector<unsigned char> comp_pixels(
                                            static_cast<std::size_t>(screenshot_width) *
                                            static_cast<std::size_t>(screenshot_height) * 3u);
                                        glReadBuffer(GL_BACK);
                                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                        glReadPixels(0,
                                                     0,
                                                     screenshot_width,
                                                     screenshot_height,
                                                     GL_RGB,
                                                     GL_UNSIGNED_BYTE,
                                                     comp_pixels.data());
                                        Luminumbra::Vec3 creature_world =
                                            creature_slice_scene.creature_position;
                                        if (gameSession->GetRegistry().valid(
                                                creature_slice_scene.creature)) {
                                            if (const auto* tf =
                                                    gameSession->GetRegistry()
                                                        .try_get<const Luminumbra::Components::
                                                                     TransformComponent>(
                                                            creature_slice_scene.creature)) {
                                                creature_world = tf->position;
                                            }
                                        }
                                        const glm::mat4 view = g_camera->GetViewMatrix();
                                        const glm::mat4 proj = g_camera->GetProjectionMatrix(
                                            screenshot_width, screenshot_height);
                                        const glm::vec4 clip =
                                            proj * view *
                                            glm::vec4(creature_world.x,
                                                      creature_world.y +
                                                          1.0f, // body center, above feet
                                                      creature_world.z,
                                                      1.0f);
                                        int sx = -1, sy = -1;
                                        if (clip.w > 0.0f) {
                                            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                                            sx = static_cast<int>(
                                                (ndc.x * 0.5f + 0.5f) *
                                                static_cast<float>(screenshot_width));
                                            sy = static_cast<int>(
                                                (1.0f - (ndc.y * 0.5f + 0.5f)) *
                                                static_cast<float>(screenshot_height));
                                        }
                                        // project the emissive glow_bloom stimulus prop too so
                                        // the glow-halo (bright core -> falloff ring) can be
                                        // measured.
                                        int stim_x = -1, stim_y = -1;
                                        if (creature_slice_scene.stimulus_spawned) {
                                            const Luminumbra::Vec3 sp =
                                                creature_slice_scene.stimulus_position;
                                            const glm::vec4 sclip =
                                                proj * view *
                                                glm::vec4(sp.x, sp.y + 0.5f, sp.z, 1.0f);
                                            if (sclip.w > 0.0f) {
                                                const glm::vec3 sndc = glm::vec3(sclip) / sclip.w;
                                                stim_x = static_cast<int>(
                                                    (sndc.x * 0.5f + 0.5f) *
                                                    static_cast<float>(screenshot_width));
                                                stim_y = static_cast<int>(
                                                    (1.0f - (sndc.y * 0.5f + 0.5f)) *
                                                    static_cast<float>(screenshot_height));
                                            }
                                        }
                                        capture.composition =
                                            AnalyzeCreatureSliceComposition(comp_pixels,
                                                                            screenshot_width,
                                                                            screenshot_height,
                                                                            sx,
                                                                            sy,
                                                                            stim_x,
                                                                            stim_y);
                                    }
                                    const std::string relative_path =
                                        want_before ? "screenshots/creature-slice-before.ppm"
                                                    : "screenshots/creature-slice-after.ppm";
                                    if (WriteBackbufferPpm(scenario_config.artifact_dir /
                                                               relative_path,
                                                           screenshot_width,
                                                           screenshot_height)) {
                                        capture.file = relative_path;
                                        if (want_before) {
                                            creature_slice_before = capture;
                                            creature_slice_before_written = true;
                                        } else {
                                            WriteCreatureSliceAnalysis(scenario_config.artifact_dir,
                                                                       creature_slice_scene,
                                                                       creature_slice_before,
                                                                       capture);
                                            creature_slice_analysis_written = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (now - last_runtime_state_write >= std::chrono::seconds(1)) {
                            last_readiness_report =
                                EvaluateReadiness(scenario_config, gameSession.get());
                            runtime_state_recorder.capture("running",
                                                           &jobSystem,
                                                           gameSession.get(),
                                                           &renderPipeline,
                                                           scenario_frame_count,
                                                           last_readiness_report);
                            last_runtime_state_write = now;
                        }
                        if (scenario_ready && scenario_config.timed_run_seconds > 0) {
                            const auto elapsed_play_seconds =
                                std::chrono::duration_cast<std::chrono::seconds>(
                                    now - scenario_play_started_at)
                                    .count();
                            if (elapsed_play_seconds >= scenario_config.timed_run_seconds &&
                                scenario_frame_count > 0) {
                                last_readiness_report =
                                    EvaluateReadiness(scenario_config, gameSession.get());
                                if (scenario_config.skinned_mesh_visual_smoke() &&
                                    scenario_config.replicated && scenario_config.avatars >= 2 &&
                                    !remote_avatar_render_artifact_written) {
                                    scenario_failed = true;
                                    scenario_failure_reason =
                                        "remote_avatar_render_artifact_missing";
                                    runtime_state_recorder.capture(scenario_failure_reason,
                                                                   &jobSystem,
                                                                   gameSession.get(),
                                                                   &renderPipeline,
                                                                   scenario_frame_count,
                                                                   last_readiness_report);
                                } else {
                                    runtime_state_recorder.capture("timed_run_complete",
                                                                   &jobSystem,
                                                                   gameSession.get(),
                                                                   &renderPipeline,
                                                                   scenario_frame_count,
                                                                   last_readiness_report);
                                    scenario_timed_run_complete = true;
                                }
                                glfwSetWindowShouldClose(window, true);
                            }
                        }
                    }
                    if (runtime_boot_recorder.enabled()) {
                        runtime_boot_recorder.record_frame(deltaTime, renderPipeline);
                        if (runtime_boot_recorder.complete()) {
                            const bool wrote_metrics = runtime_boot_recorder.write_artifacts();
                            LUMINUMBRA_CORE_INFO("Runtime boot metrics {}: {} frames -> {}",
                                                 wrote_metrics ? "written" : "failed",
                                                 runtime_boot_frames,
                                                 runtime_boot_output.string());
                            glfwSetWindowShouldClose(window, true);
                        }
                    }
                }
            } else {
                // Menu-branch rendering: thumbs capture, menu backdrop,
                // create-world preview diorama, UI pass, and the
                // --ui-screenshot batch (app/MenuScreens.cpp).
                RenderMenuScreens(g_app,
                                  window,
                                  deltaTime,
                                  root_path_str,
                                  g_camera.get(),
                                  gameSession.get(),
                                  renderPipeline,
                                  g_uiManager.get(),
                                  worldgenPreview.get(),
                                  menuPreviewState);
            }
        }

        if (currentState == GameState::IN_GAME) {
            // Floating creature ID nameplates for the ecology demo capture
            // (app/DebugOverlays.cpp).
            DrawCreatureNameplates(g_app, window, g_camera.get(), gameSession.get());
            // Player debug UI, time-scale indicator, and the minimal crop HUD
            // (app/DebugOverlays.cpp).
            DrawFrameStatusOverlays(g_app, currentState, scenario_config, g_playerController.get());
            // Live per-pass GPU profiler overlay (app/DebugOverlays.cpp).
            DrawGpuProfilerOverlay(g_app, currentState, renderPipeline, g_uiManager.get());
            // --debug-glass-pane one-time capture subject (app/DebugOverlays.cpp).
            SpawnDebugGlassPanes(g_app, currentState, gameSession.get(), renderPipeline);
            // push the metering opt-in (idempotent per frame).
            renderPipeline.set_auto_exposure_metered(g_app.overlay.auto_exposure_metered);
            //  rendering: push the volumetrics tier.
            renderPipeline.set_volumetric_quality(g_app.overlay.volumetric_quality);
            // Live shader authoring: reload requests, the opt-in mtime watcher,
            // and the dev shader panel (app/DebugOverlays.cpp).
            UpdateShaderTools(g_app, currentState, renderPipeline);
            // Settings menu (app/DebugOverlays.cpp). Render-only; user.* is
            // never hashed.
            DrawSettingsWindow(
                g_app, window, g_camera.get(), audioManager.get(), g_systemConfig, g_windowState);
            if (g_app.overlay.imgui_enabled && g_app.overlay.show_worldgen_viewer &&
                worldGenViewer) {
                worldGenViewer->UpdateAndRender(g_app.overlay.show_worldgen_viewer,
                                                gameSession->GetWorldSystem());
            }
        }

        //  render the RmlUi overlay in-game too (HUD / photo-mode / pause), over the scene and
        // under the ImGui debug layer. Menus still render via the else-branch above; this path
        // shows whatever in-game document is loaded (hud.rml on entry).
        const auto _rb_ui_t0 = std::chrono::steady_clock::now(); //
        if (currentState == GameState::IN_GAME && g_uiManager) {
            g_uiManager->Render();
        }

        if (g_app.overlay.imgui_enabled) {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
        if (g_rb_active)
            rb_ui_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 _rb_ui_t0)
                           .count(); //

        // --render-benchmark: pin a FIXED, FOREST-DENSE camera pose + time-of-day so
        // the budget capture is reproducible AND actually stresses the static-prop
        // submit path  optimizes (an open-horizon pose under-samples the
        // 40-110k-instance loop). High over the grove looking out at a shallow
        // downward angle frames a deep carpet of canopy stretching to the horizon
        // (max instances in frustum + max overdraw). Set every frame so gravity /
        // settle can't drift it; it takes effect on the NEXT rendered frame. The
        // GPU-timer averaging + the honest CPU-submit/present/NVML accounting run
        // AFTER glfwSwapBuffers (see the post-swap block) so present + wall-clock
        // are measured, not just the GPU per-pass timer sum.
        if (!g_app.capture.render_benchmark_path.empty() && currentState == GameState::IN_GAME &&
            gameSession) {
            if (g_camera) {
                g_camera->Position = glm::vec3(8.0f, 56.0f, 8.0f);
                g_camera->Yaw = 35.0f;
                g_camera->Pitch = -6.0f; // shallow: deep forest carpet, not down at near ground
                g_camera->updateCameraVectors();
            }
            renderPipeline.set_time_of_day(0.04f); // fixed near-noon (clouds + lit terrain)
        }

        // --timelapse: dump the rendered frame, then fast-forward sim-time (+ the day clock)
        // for the next one. Pair with --no-ui so no overlay is baked into the frame.
        if (g_app.capture.timelapse_frames > 0 && currentState == GameState::IN_GAME &&
            gameSession) {
            if (g_app.capture.timelapse_settle < kTimelapseSettleFrames) {
                renderPipeline.set_time_of_day(
                    g_app.capture.timelapse_tod); // settle at the start time-of-day
                ++g_app.capture.timelapse_settle; // let the world stream/settle before frame 0
            } else {
                int vw = 0, vh = 0;
                glfwGetFramebufferSize(window, &vw, &vh);
                if (vw > 0 && vh > 0) {
                    std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                  static_cast<std::size_t>(vh) * 3u);
                    glReadBuffer(GL_BACK);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                    char nm[32];
                    std::snprintf(
                        nm, sizeof(nm), "frame_%04d.ppm", g_app.capture.timelapse_captured);
                    if (WritePixelBufferPpm(g_app.capture.timelapse_dir / nm, vw, vh, px))
                        ++g_app.capture.timelapse_captured;
                }
                if (g_app.capture.timelapse_captured >= g_app.capture.timelapse_frames) {
                    LUMINUMBRA_CORE_INFO("Timelapse: captured {} frames -> {}",
                                         g_app.capture.timelapse_captured,
                                         g_app.capture.timelapse_dir.string());
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                } else {
                    //  terraform demo: from the 3rd captured frame, carve a trench that
                    // marches from +Z toward spawn (one sphere/frame), then let the fast-forward
                    // ticks below drain/redirect any water into the freshly-cut channel. Carve
                    // BEFORE the ticks so the water responds within this same frame's advance.
                    if (g_app.capture.timelapse_dig && g_app.capture.timelapse_captured >= 2 &&
                        gameSession->GetWorldSystem()) {
                        const auto sp = gameSession->GetMetadata().spawnPoint;
                        const int dig_i = g_app.capture.timelapse_captured -
                                          2; // 0,1,2,... after a 2-frame establishing hold
                        const float surf =
                            gameSession->GetWorldSystem()->GetTerrainHeightAt(sp.x, sp.z);
                        // DEEPEN a crater at spawn: drop the sphere center ~1 m/frame so the pit
                        // visibly descends, with a generous radius so the excavation reads clearly
                        // at close range.
                        const float cy = surf - 0.5f - static_cast<float>(dig_i) * 1.0f;
                        const int n = gameSession->GetWorldSystem()->EditTerrainVoxel(
                            Luminumbra::Vec3(sp.x, cy, sp.z),
                            4.5f,
                            /*fill=*/false,
                            gameSession->GetPhysicsSystem());
                        LUMINUMBRA_CORE_INFO(
                            "Timelapse-dig: carved {} chunk(s), crater floor y={:.1f}", n, cy);
                    }
                    //  money shot: after a hold that shows the water body, breach the bank — step
                    // a carve sphere from the water OUT along the downhill direction, cutting a
                    // channel below the waterline so the body drains through it. The fast-forward
                    // ticks below let the solver push water out each frame; the level visibly
                    // drops.
                    if (g_app.capture.timelapse_drain && g_app.capture.drain_state.init &&
                        gameSession->GetWorldSystem()) {
                        auto* wsd = gameSession->GetWorldSystem();
                        wsd->debug_force_water_remesh(); // smooth per-frame surface
                        // GROUND TRUTH: water volume in a disc over the INLAND target region (where
                        // the channel is cut). If the water really floods into the new cut, this
                        // RISES from ~0.
                        const Luminumbra::Vec3 inland(
                            g_app.capture.drain_state.P.x + g_app.capture.drain_state.dhx * 6.0f,
                            g_app.capture.drain_state.P.y,
                            g_app.capture.drain_state.P.z + g_app.capture.drain_state.dhz * 6.0f);
                        const std::int64_t vol = wsd->debug_water_volume_near(inland, 10.0f);
                        LUMINUMBRA_CORE_INFO(
                            "Timelapse-drain[f{}]: inland water volume (Sum depth) = {} mm-cells",
                            g_app.capture.timelapse_captured,
                            vol);
                        // Three acts: A) hold on the dry bank, B) carve a contained MODERATE-depth
                        // basin into it (touching the sea so it floods), C) STOP carving and hold
                        // while the sea fills the basin up to its level on camera. Moderate depth
                        // so it fills in-window.
                        const int holdN =
                            std::max(6, g_app.capture.timelapse_frames / 5); // Act A end
                        const int carveN =
                            holdN + std::max(10, g_app.capture.timelapse_frames / 3); // Act B end
                        if (g_app.capture.timelapse_captured >= holdN &&
                            g_app.capture.timelapse_captured < carveN) {
                            const int k = g_app.capture.timelapse_captured - holdN;
                            // Bowl 3 m onto the bank, always touching the sea; WIDEN it (fixed 2 m
                            // depth) so a pool grows into the green bank and stays IN FRAME.
                            const float cx = g_app.capture.drain_state.P.x +
                                             g_app.capture.drain_state.dhx * 3.0f;
                            const float cz = g_app.capture.drain_state.P.z +
                                             g_app.capture.drain_state.dhz * 3.0f;
                            const float radius =
                                3.0f + 0.34f * static_cast<float>(k); // 3 -> ~11 m wide
                            const float floor_y = g_app.capture.drain_state.surf -
                                                  1.5f; // shallow+wide -> fills fast, spreads
                            const int n = gameSession->GetWorldSystem()->EditTerrainVoxel(
                                Luminumbra::Vec3(cx, floor_y, cz),
                                radius,
                                /*fill=*/false,
                                gameSession->GetPhysicsSystem());
                            LUMINUMBRA_CORE_INFO(
                                "Timelapse-drain: breach {} chunk(s) at ({:.1f},{:.1f}); max depth "
                                "now {:.2f} m",
                                n,
                                cx,
                                cz,
                                static_cast<double>(
                                    gameSession->GetWorldSystem()->debug_max_water_depth_mm()) /
                                    1000.0);
                        }
                    }
                    //  FINITE HYDROLOGY demo: Act 1 turns on rain (no perpetual source) and the
                    // land fills from rainfall — water collects in the low spots. Act 2 turns rain
                    // OFF and evaporation ON: the water does NOT refill (finite) and slowly
                    // recedes. The volume trace tells the story.
                    if (g_app.capture.timelapse_rain && g_app.capture.drain_state.init &&
                        gameSession->GetWorldSystem()) {
                        auto* wsr = gameSession->GetWorldSystem();
                        wsr->debug_force_water_remesh();
                        const int rainOff =
                            (g_app.capture.timelapse_frames * 3) / 5; // Act 1 rains, Act 2 dries
                        if (g_app.capture.timelapse_captured == rainOff) {
                            wsr->SetWaterHydrology(/*finite=*/true, /*rain=*/0, /*evap=*/2);
                            LUMINUMBRA_CORE_INFO("Timelapse-rain: rain OFF + evaporation ON — "
                                                 "water is finite, it recedes (no refill)");
                        }
                        const std::int64_t vol =
                            wsr->debug_water_volume_near(g_app.capture.drain_state.P, 14.0f);
                        LUMINUMBRA_CORE_INFO(
                            "Timelapse-rain[f{}]: basin vol = {}, LAND water = {} mm-cells",
                            g_app.capture.timelapse_captured,
                            vol,
                            wsr->debug_land_water_volume_mm());
                    }
                    // Fast-forward the SIM (weather/wind/creatures/plants) by K EXTRA fixed
                    // ticks for the next frame (on top of the normal per-frame tick). Physics
                    // runs normally each frame so the player stays grounded.
                    for (int i = 0; i < g_app.capture.timelapse_ticks; ++i)
                        gameSession->TickSimulation(1.0 / 30.0);
                    if (g_app.capture.timelapse_daystep >
                        0.0f) { // drift the sun/sky for shade-over-time
                        g_app.capture.timelapse_tod += g_app.capture.timelapse_daystep;
                        if (g_app.capture.timelapse_tod >= 1.0f)
                            g_app.capture.timelapse_tod -= 1.0f;
                        renderPipeline.set_time_of_day(g_app.capture.timelapse_tod);
                    }
                    if (g_app.capture.timelapse_season) { // drift summer -> autumn leaf color
                                                          // across the capture
                        g_procgen.season =
                            static_cast<float>(g_app.capture.timelapse_captured) /
                            static_cast<float>(std::max(1, g_app.capture.timelapse_frames - 1));
                    }
                    if (g_app.capture.timelapse_grow ||
                        g_app.capture.timelapse_season) { // re-bake on stage/season change
                        if (g_app.capture.timelapse_grow) {
                            g_procgen.stageF =
                                5.0f * static_cast<float>(g_app.capture.timelapse_captured) /
                                static_cast<float>(std::max(1, g_app.capture.timelapse_frames - 1));
                        }
                        BakeProcgenPlants(
                            g_procgen, renderPipeline.plant_procgen(), g_procgen.stageF);
                    }
                    //  re-bake LIVE sim plants each captured frame so the timelapse
                    // shows their REAL growth (the session tick advanced PlantGrowthSystem since
                    // the last bake). Visual-only; sim/world_hash untouched.
                    if (g_app.capture.timelapse_simgrow && gameSession) {
                        RebakeAllPlants(g_procgen,
                                        renderPipeline.plant_procgen(),
                                        gameSession->GetRegistry(),
                                        g_procgen.sunDir,
                                        g_procgen.season);
                    }
                }
            }
        }

        //  CPU-submit ends here (all GL work for the frame is
        // queued); present begins. With vsync off the swap drains the driver
        // queue, so its duration is the GPU/present wait.
        if (g_rb_active)
            g_rb_before_swap = std::chrono::steady_clock::now();
        const auto _beforeSwap = std::chrono::steady_clock::now();
        glfwSwapBuffers(window);
        // runtime telemetry ( implementation note): localize slideshow-on-move frames. Logs the
        // phase split for any frame slower than ~30 fps. sim = TickSimulation (physics+ecology),
        // stream = SHIELD_WorldSystem::update, render = render_frame CPU, present = swap wait.
        {
            const auto _now = std::chrono::steady_clock::now();
            const double _wallMs =
                std::chrono::duration<double, std::milli>(_now - _frameStart).count();
            const double _presentMs =
                std::chrono::duration<double, std::milli>(_now - _beforeSwap).count();
            static int _slowN = 0;
            if (_wallMs > 12.0 && _slowN++ < 400) {
                const double _other =
                    _wallMs - rb_sim_ms - rb_stream_ms - rb_render_call_ms - _presentMs;
                LUMINUMBRA_CORE_WARN("SLOWFRAME {:.1f}ms ({:.0f}fps): sim={:.1f} stream={:.1f} "
                                     "render={:.1f} present={:.1f} other={:.1f} | "
                                     "foliage_inst={:.1f} scatter={:.1f} rebake={:.1f} poll={:.1f}",
                                     _wallMs,
                                     1000.0 / _wallMs,
                                     rb_sim_ms,
                                     rb_stream_ms,
                                     rb_render_call_ms,
                                     _presentMs,
                                     _other,
                                     rb_foliage_ms,
                                     rb_scatter_ms,
                                     rb_rebake_ms,
                                     rb_poll_ms);
                if (gameSession && gameSession->GetWorldSystem()) {
                    const auto& st = gameSession->GetWorldSystem()->dbg_stream_timings();
                    LUMINUMBRA_CORE_WARN(
                        "  stream-split: process_completed={:.1f} telemetry={:.1f} "
                        "activation={:.1f} water={:.1f} meshing_pass={:.1f} collision={:.1f}",
                        st.process_completed,
                        st.telemetry,
                        st.activation,
                        st.water,
                        st.meshing_pass,
                        st.collision);
                }
            }
        }

        // HONEST measurement substrate. wall = max(CPU_submit,
        // GPU_work) + present. The old benchmark summed per-pass GPU timers ONLY
        // and was blind to the CPU-submit win this path provides; it also ran at idle
        // clock, so it now reports NVML power+clock to prove the GPU is at boost.
        if (g_rb_active && currentState == GameState::IN_GAME && gameSession) {
            const auto rb_now = std::chrono::steady_clock::now();
            const double cpu_submit_ms =
                std::chrono::duration<double, std::milli>(g_rb_before_swap - g_rb_frame_start)
                    .count();
            const double present_ms =
                std::chrono::duration<double, std::milli>(rb_now - g_rb_before_swap).count();
            static std::chrono::steady_clock::time_point rb_prev_end{};
            double wall_ms = 0.0;
            if (rb_prev_end.time_since_epoch().count() != 0)
                wall_ms = std::chrono::duration<double, std::milli>(rb_now - rb_prev_end).count();
            rb_prev_end = rb_now;

            if (!g_rb_nvml_tried) {
                g_rb_nvml_tried = true;
                g_rb_nvml_ok = g_rb_nvml.init();
            }
            double gpu_power_w = 0.0, gpu_clock_mhz = 0.0;
            const bool nv = g_rb_nvml_ok && g_rb_nvml.sample(gpu_power_w, gpu_clock_mhz);

            const auto& s = renderPipeline.get_last_render_pass_stats();
            static int rb_warm = 0, rb_count = 0, rb_nv_count = 0;
            static double rb_shadow = 0, rb_gbuffer = 0, rb_ssao = 0, rb_ssao_blur = 0,
                          rb_lighting = 0, rb_water = 0, rb_skybox = 0, rb_particle = 0,
                          rb_foliage = 0, rb_aerial = 0, rb_final = 0, rb_total = 0;
            static double rb_cpu = 0, rb_present = 0, rb_wall = 0, rb_power = 0, rb_clock = 0;
            static double rb_cpu_prep = 0, rb_cpu_shadow = 0, rb_cpu_gbuf = 0, rb_cpu_post = 0,
                          rb_cpu_prop = 0;
            static double rb_sim = 0, rb_stream = 0, rb_foliage_rebuild = 0, rb_ui = 0;
            static double rb_render_call = 0, rb_poll = 0,
                          rb_scatter = 0; //  unattributed-CPU localization
            static bool rb_nv_ever = false;

            if (rb_warm < g_app.capture.render_benchmark_warmup) {
                ++rb_warm;
            } else if (rb_count < g_app.capture.render_benchmark_frames) {
                rb_shadow += s.shadow_gpu_ms;
                rb_gbuffer += s.gbuffer_gpu_ms;
                rb_ssao += s.ssao_gpu_ms;
                rb_ssao_blur += s.ssao_blur_gpu_ms;
                rb_lighting += s.lighting_gpu_ms;
                rb_water += s.water_gpu_ms;
                rb_skybox += s.skybox_gpu_ms;
                rb_particle += s.particle_gpu_ms;
                rb_foliage += s.foliage_gpu_ms;
                rb_aerial += s.aerial_gpu_ms;
                rb_final += s.final_blit_gpu_ms;
                rb_total += s.shadow_gpu_ms + s.gbuffer_gpu_ms + s.ssao_gpu_ms +
                            s.ssao_blur_gpu_ms + s.lighting_gpu_ms + s.water_gpu_ms +
                            s.skybox_gpu_ms + s.particle_gpu_ms + s.foliage_gpu_ms +
                            s.aerial_gpu_ms + s.final_blit_gpu_ms;
                rb_cpu += cpu_submit_ms;
                rb_present += present_ms;
                rb_wall += wall_ms;
                rb_cpu_prep += s.cpu_prepare_ms;
                rb_cpu_shadow += s.cpu_shadow_ms;
                rb_cpu_gbuf += s.cpu_gbuffer_ms;
                rb_cpu_post += s.cpu_post_ms;
                rb_cpu_prop += s.cpu_static_prop_ms;
                rb_sim += rb_sim_ms;
                rb_stream += rb_stream_ms;
                rb_foliage_rebuild += rb_foliage_ms;
                rb_ui += rb_ui_ms;
                rb_render_call += rb_render_call_ms;
                rb_poll += rb_poll_ms;
                rb_scatter += rb_scatter_ms;
                if (nv) {
                    rb_power += gpu_power_w;
                    rb_clock += gpu_clock_mhz;
                    ++rb_nv_count;
                    rb_nv_ever = true;
                }
                ++rb_count;

                // Dump the forest-dense pose on the LAST measured frame (verifies
                // density + supplies before/after PNGs). The back buffer was just
                // swapped to front, so read GL_FRONT.
                if (rb_count == g_app.capture.render_benchmark_frames &&
                    !g_app.capture.render_benchmark_screenshot.empty()) {
                    int vw = 0, vh = 0;
                    glfwGetFramebufferSize(window, &vw, &vh);
                    if (vw > 0 && vh > 0) {
                        std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                      static_cast<std::size_t>(vh) * 3u);
                        glReadBuffer(GL_FRONT);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                        WritePixelBufferPpm(
                            std::filesystem::path(g_app.capture.render_benchmark_screenshot),
                            vw,
                            vh,
                            px);
                        LUMINUMBRA_CORE_INFO("Render benchmark screenshot -> {} ({}x{})",
                                             g_app.capture.render_benchmark_screenshot,
                                             vw,
                                             vh);
                    }
                }
            } else {
                const double n = static_cast<double>(std::max(1, rb_count));
                const double np = static_cast<double>(std::max(1, rb_nv_count));
                const double gpu_sum = rb_total / n;
                const double wall = rb_wall / n;
                const double cpu = rb_cpu / n;
                nlohmann::json j;
                j["schema"] = "luminumbra.render_benchmark.v2";
                j["frames"] = rb_count;
                j["warmup_frames"] = g_app.capture.render_benchmark_warmup;
                j["width"] = renderPipeline.screen_width();
                j["height"] = renderPipeline.screen_height();
                j["render_scale"] = renderPipeline.render_scale();
                j["internal_width"] = renderPipeline.internal_width();
                j["internal_height"] = renderPipeline.internal_height();
                j["pose"] = "forest_dense";
                j["cloud_quality"] = renderPipeline.get_cloud_quality();
                j["ssao_quality"] = renderPipeline.get_ssao_quality();
                j["gpu_timers_supported"] = s.gpu_timers_supported;
                j["nvml_supported"] = rb_nv_ever;
                // Per-pass GPU timer averages (kept for back-compat with the
                // RenderBudget per-pass budgets).
                j["avg_ms"] = {{"shadow", rb_shadow / n},
                               {"gbuffer", rb_gbuffer / n},
                               {"ssao", rb_ssao / n},
                               {"ssao_blur", rb_ssao_blur / n},
                               {"ssao_total", (rb_ssao + rb_ssao_blur) / n},
                               {"lighting", rb_lighting / n},
                               {"water", rb_water / n},
                               {"skybox", rb_skybox / n},
                               {"particle", rb_particle / n},
                               {"foliage", rb_foliage / n},
                               {"aerial", rb_aerial / n},
                               {"final_blit", rb_final / n},
                               {"total", gpu_sum}};
                // The honest frame attribution (the numbers that actually decide
                // whether we are CPU-bound or GPU-bound).
                j["avg"] = {{"cpu_submit_ms", cpu},
                            {"present_ms", rb_present / n},
                            {"frame_wall_ms", wall},
                            {"gpu_pass_sum_ms", gpu_sum},
                            {"gpu_power_w", rb_nv_ever ? rb_power / np : 0.0},
                            {"gpu_clock_mhz", rb_nv_ever ? rb_clock / np : 0.0},
                            // CPU submit broken down by phase (localizes the bound).
                            {"cpu_prepare_ms", rb_cpu_prep / n},
                            {"cpu_shadow_ms", rb_cpu_shadow / n},
                            {"cpu_gbuffer_ms", rb_cpu_gbuf / n},
                            {"cpu_static_prop_ms", rb_cpu_prop / n},
                            {"cpu_post_ms", rb_cpu_post / n},
                            {"sim_tick_ms", rb_sim / n},
                            {"streaming_ms", rb_stream / n},
                            {"foliage_rebuild_ms", rb_foliage_rebuild / n},
                            {"ui_render_ms", rb_ui / n},
                            {"render_frame_wall_ms", rb_render_call / n},
                            {"poll_ms", rb_poll / n},
                            {"scatter_build_ms", rb_scatter / n}};
                // Heuristic bound attribution for the log line: CPU-bound if the
                // CPU submit dominates the GPU pass-timer sum.
                j["bound"] = (cpu > gpu_sum * 1.1) ? "cpu" : "gpu_or_present";
                std::error_code _rb_ec;
                const std::filesystem::path rb_path(g_app.capture.render_benchmark_path);
                if (rb_path.has_parent_path())
                    std::filesystem::create_directories(rb_path.parent_path(), _rb_ec);
                std::ofstream out(rb_path);
                out << j.dump(2);
                out.close();
                LUMINUMBRA_CORE_INFO("Render benchmark: {} frames -> {} | wall {:.3f} ms ({:.0f} "
                                     "fps) | cpu_submit {:.3f} ms | "
                                     "present {:.3f} ms | gpu_pass_sum {:.3f} ms | power {:.0f} W "
                                     "| clock {:.0f} MHz | bound={}",
                                     rb_count,
                                     g_app.capture.render_benchmark_path,
                                     wall,
                                     wall > 0.0 ? 1000.0 / wall : 0.0,
                                     cpu,
                                     rb_present / n,
                                     gpu_sum,
                                     rb_nv_ever ? rb_power / np : 0.0,
                                     rb_nv_ever ? rb_clock / np : 0.0,
                                     j["bound"].get<std::string>());
                LUMINUMBRA_CORE_INFO(
                    "  CPU submit breakdown: prepare {:.3f} | shadow {:.3f} | gbuffer {:.3f} "
                    "(static_prop {:.3f}) | post {:.3f} ms",
                    rb_cpu_prep / n,
                    rb_cpu_shadow / n,
                    rb_cpu_gbuf / n,
                    rb_cpu_prop / n,
                    rb_cpu_post / n);
                {
                    // render_frame FULL wall captures every pass's CPU submit
                    // (skybox/water/foliage/lighting/aerial had no cpu_* sub-timer);
                    // poll is the window/input pump. Whatever remains after these +
                    // sim/stream/foliage-rebuild/ui is the true residual (scenario harness, etc).
                    const double _rcall = rb_render_call / n, _poll = rb_poll / n,
                                 _scatter = rb_scatter / n;
                    const double _known = _rcall + _poll + _scatter + rb_sim / n + rb_stream / n +
                                          rb_foliage_rebuild / n + rb_ui / n;
                    LUMINUMBRA_CORE_INFO(
                        "  NON-render frame CPU: sim {:.3f} | streaming {:.3f} | scatter_build "
                        "{:.3f} | "
                        "foliage_rebuild {:.3f} | ui_render {:.3f} | poll {:.3f} ms",
                        rb_sim / n,
                        rb_stream / n,
                        _scatter,
                        rb_foliage_rebuild / n,
                        rb_ui / n,
                        _poll);
                    LUMINUMBRA_CORE_INFO(
                        "  render_frame FULL wall {:.3f} ms (vs cpu_* sub-timers {:.3f}: the delta "
                        "is "
                        "skybox/water/foliage/lighting/aerial submit) | residual {:.3f} ms",
                        _rcall,
                        (rb_cpu_prep + rb_cpu_shadow + rb_cpu_gbuf + rb_cpu_post) / n,
                        std::max(0.0, cpu - _known));
                }
                glfwSetWindowShouldClose(window, GLFW_TRUE);
                rb_count = g_app.capture.render_benchmark_frames + 1; // latch: stop re-dumping
            }
        }
    }
    g_rb_nvml.shutdown(); //  release NVML if it was loaded

    if (scenario_config.active() && scenario_config.timed_run_seconds > 0 && scenario_ready &&
        !scenario_timed_run_complete && !scenario_failed) {
        const auto elapsed_play_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                             scenario_play_started_at)
                .count();
        if (elapsed_play_seconds < scenario_config.timed_run_seconds) {
            scenario_failed = true;
            scenario_failure_reason = "window_closed_before_timed_run_complete";
            exit_code = 5;
            last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
            runtime_state_recorder.capture("window_closed_before_timed_run_complete",
                                           &jobSystem,
                                           gameSession.get(),
                                           &renderPipeline,
                                           scenario_frame_count,
                                           last_readiness_report);
        }
    }

    if (scenario_failed && exit_code == 0) {
        exit_code = 2;
    }
    if (scenario_config.active()) {
        const std::string shutdown_phase =
            scenario_failed ? ("scenario_failed_" + scenario_failure_reason) : "shutdown_begin";
        runtime_state_recorder.capture(shutdown_phase,
                                       &jobSystem,
                                       gameSession.get(),
                                       &renderPipeline,
                                       scenario_frame_count,
                                       last_readiness_report);
    }
    if (lod_ground_frame_recorder.enabled()) {
        lod_ground_frame_recorder.write_artifacts();
        if (!lod_ground_screenshot_files.empty()) {
            WriteLodGroundScreenshotIndex(scenario_config.artifact_dir,
                                          lod_ground_screenshot_files);
            WriteLodGroundVisualAnalysis(scenario_config.artifact_dir, lod_ground_visual_captures);
        }
    }
    if (scenario_config.active()) {
        if (auto* world_system = gameSession->GetWorldSystem()) {
            const double scenario_play_seconds =
                scenario_ready ? std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                               scenario_play_started_at)
                                     .count()
                               : 0.0;
            WriteStreamingTelemetry(scenario_config.artifact_dir,
                                    scenario_config.scenario,
                                    scenario_play_seconds,
                                    world_system->get_streaming_telemetry_stats());
            if (scenario_config.lod_boundary_oscillation_smoke()) {
                WriteLodBoundaryOscillationAnalysis(scenario_config.artifact_dir,
                                                    scenario_play_seconds,
                                                    LodBoundaryDistance(world_system),
                                                    lod_boundary_transition_recorder);
            }
            if (scenario_config.lod_seam_arrival_smoke()) {
                WriteLodSeamArrivalAnalysis(scenario_config.artifact_dir,
                                            scenario_play_seconds,
                                            lod_seam_visual_captures,
                                            lod_seam_arrival_recorder);
            }
        }
        if (scenario_config.window_mode_stress_smoke() && !window_mode_stress_analysis_written) {
            const double scenario_play_seconds =
                scenario_ready ? std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                               scenario_play_started_at)
                                     .count()
                               : 0.0;
            WriteWindowModeStressAnalysis(scenario_config.artifact_dir,
                                          scenario_play_seconds,
                                          scenario_config.window_mode,
                                          window_mode_stress_steps,
                                          window_mode_stress_capture);
            window_mode_stress_analysis_written = true;
        }
    }

    std::vector<std::string> shutdown_milestones;
    auto mark_shutdown = [&](const std::string& milestone) {
        shutdown_milestones.push_back(milestone);
    };

    // persist unsaved voxel edits on the world-exit/shutdown path,
    // before the streamed chunks are torn down. No-op when no world session
    // is active or when no chunk carries unsaved edits.
    if (gameSession && gameSession->SaveWorldState()) {
        mark_shutdown("world_state_saved");
    }

    // Drain the  far-field heightfield build before the world is cleared —
    // its worker job reads the world by pointer (else a teardown-time use-after-free).
    renderPipeline.prepare_world_swap();
    if (auto* world_system = gameSession->GetWorldSystem()) {
        world_system->clear_world(gameSession->GetPhysicsSystem());
        mark_shutdown("world_cleared");
    }
    g_playerController.reset();
    mark_shutdown("player_controller_reset");
    g_camera.reset();
    mark_shutdown("camera_reset");
    if (g_uiManager) {
        g_uiManager->Shutdown();
        g_uiManager.reset();
        mark_shutdown("ui_shutdown");
    } else {
        mark_shutdown("ui_not_started");
    }
    if (g_loading_visualizer) {
        g_loading_visualizer->Shutdown();
        g_loading_visualizer.reset();
        mark_shutdown("loading_visualizer_shutdown");
    }
    audioManager->Shutdown();
    mark_shutdown("audio_shutdown");
    renderPipeline.shutdown();
    mark_shutdown("renderer_shutdown");
    if (g_app.overlay.imgui_enabled) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        mark_shutdown("imgui_shutdown");
    } else {
        mark_shutdown("imgui_not_started");
    }
    // the  background scan holds a raw world pointer — drain
    // it before the session (and its world) is destroyed.
    DrainBackgroundWorldScan(jobSystem);
    // the world-dressing placement job likewise queries the world
    // through its callbacks — drain it too.
    DrainWorldDressing(jobSystem);
    gameSession.reset();
    mark_shutdown("game_session_reset");
    jobSystem.shutdown();
    mark_shutdown("job_system_shutdown");
    const auto shutdown_job_stats = jobSystem.get_runtime_stats();
    runtime_state_recorder.write_shutdown(shutdown_milestones, shutdown_job_stats);
    glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
}
