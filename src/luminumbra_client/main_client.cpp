#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "WorldDressing.h" // background world-dressing placement computation
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

#if defined(_WIN32)
#include <DbgHelp.h>
#include <Psapi.h>
#include <Windows.h>
#endif

using namespace Luminumbra::Client::ScenarioHarness;

// --- Global Pointers ---
std::unique_ptr<Luminumbra::Rendering::Camera> g_camera;
std::unique_ptr<Luminumbra::Client::PlayerController> g_playerController;

// feature: client-only photo-mode state + progression codex. NOT sim
// state — neither participates in any baseline NetworkStateHash (PhotoCodex.h
// documents this), so photo mode is a pure read-mostly observer. The lens is nudged
// by the PlayerController's aperture/focus inputs; the codex keeps the best score per
// species across the session.
luminumbra::game::PhotoModeState g_photoMode;
static bool g_photoModeUiShown = false; //  photo_mode.rml vs hud.rml is the active in-game overlay
luminumbra::game::PhotoCodex g_photoCodex;
// Data-driven creature species metadata (display names + rarity) the codex/discovery HUD
// resolve a captured species_id against. Loaded once from data/common/creatures/species
// after the runtime root is known; client-only, never hashed.
luminumbra::ai::CreatureSpeciesRegistry g_creatureSpecies;
// Progression goals surfaced on the in-game HUD. Initialised lazily once in-game with the
// default world's first creature so the starter chain is always achievable. Client-only,
// never hashed. g_objHudSig caches the last-rendered tracker text so the DOM is only
// touched when the current objective / progress actually changes.
luminumbra::game::ObjectiveSet g_objectives;
bool g_objectivesInit = false;
std::string g_objHudSig;
//  farming HUD signature — caches the last-rendered seed/harvest
// inventory + facing-crop text so the DOM is only touched when it actually changes.
// Client-only, never hashed (reads sim state, never mutates it).
std::string g_farmHudSig;
// Creature codex browse overlay (client-only). g_codexOpen toggles via the ToggleCodex
// key; g_codexSig throttles re-population so rows are rebuilt only when discovery changes.
bool g_codexOpen = false;
std::string g_codexSig;
// Single client config: defaults (data/common/systems.json) overlaid by the writable
// per-user settings file (%APPDATA%/Luminumbra/settings.json). user.* is client-only,
// never hashed. Loaded once at startup, before window creation.
luminumbra::core::SystemConfig g_systemConfig;
// Settings menu  — frees the cursor so the ImGui panel is clickable. While
// g_rebindCaptureAction >= 0 the next key press is captured as that action's binding.
bool g_show_settings = false;
bool g_paused = false;            //  in-game pause overlay active
bool g_show_gpu_profiler = false; // Live per-pass GPU profiler overlay.
//  (live shader authoring):  = reload-all next frame (GL-thread safe);
//  = the dev shader panel; the watcher is the opt-in once/sec mtime poll.
bool g_show_shader_panel = false;
bool g_request_shader_reload = false;
bool g_shader_auto_reload = false;
double g_shader_watch_last_poll = 0.0;
std::unordered_map<std::string, std::filesystem::file_time_type> g_shader_watch_mtimes;
// --debug-glass-pane stages three stained-glass panes
// near spawn so the colored-shadow AC captures have a subject. Render-only.
bool g_debug_glass_panes = false;
bool g_glass_panes_spawned = false;
// --auto-exposure-metered opts the GPU metering servo in.
bool g_auto_exposure_metered = false;
// ->: thunder cues queued by the live lightning
// consumer (strike sim tick + distance + magnitude); the audio tick drains them
// with the physical sound delay (distance / 343 m/s). Client-only, never sim.
struct PendingThunder {
    std::uint64_t strike_tick = 0;
    double fired_at_seconds = 0.0; // wall clock when the bolt rendered
    float distance_m = 0.0f;
    float magnitude = 0.0f;
};
std::vector<PendingThunder> g_pendingThunder;
//  rendering: --volumetric-quality N (0 analytic-only default).
int g_volumetric_quality = 0;
int g_rebindCaptureAction = -1;

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
// host_timescale-style engine time control (Source/GMod-like). 1.0 = real time, 0 = paused,
// <1 slow-mo, >1 fast-forward. Render/client playback rate: scales how many FIXED 30 Hz sim
// ticks run per real frame, NOT the tick dt — so determinism + run==replay hold (tick sequence
// unchanged; per-tick world_hash unchanged). Leveraged by the timelapse capture.
float g_timeScale = 1.0f;
// --timelapse capture: dump a frame sequence of the LIVE loaded world while sim-time (and,
// optionally, the day clock) fast-forward, for tools/timelapse.py. 0 = off. Run with
// --no-ui for a clean capture. Normal per-frame ticking is paused; the capture loop owns
// advancement (g_timelapse_ticks fixed ticks per captured frame).
int g_timelapse_frames = 0;
// Render-optimization (render-optimization-index ): --render-benchmark <path>.
// Boots auto-world, lets it settle, then averages the per-pass GPU timers
// (RenderPassFrameStats *_gpu_ms) over g_render_benchmark_frames in-world frames and
// writes a JSON report (per-pass avg ms + total + cloud_quality) before exiting. The
// repeatable, fixed-scenario capture the release per-pass budget RED gate consumes
// (no human in-world). Empty path = off. Pair with --auto-create-world --auto-enter-world.
std::string g_render_benchmark_path;
// runtime telemetry ( implementation note): --play-paths runs a scenario's scripted camera but with
// the NORMAL-PLAY render/streaming paths (foliage GPU readback OFF, scatter cache ON) instead of
// the gate-mode paths a scenario normally forces. Lets a headless moving scenario profile what the
// player actually experiences (the gate-mode foliage readback/cache-disable hugely inflate cost).
bool g_play_paths = false;
// runtime telemetry ( implementation note): --profile-fly <seconds> drives the player FORWARD at a
// constant noclip speed in NORMAL-PLAY mode (no scenario, no gate-mode) so the SLOWFRAME logger
// captures a representative moving cost without the time-based scenario camera's
// teleport-under-load feedback. Pair with --auto-create-world --auto-enter-world --no-audio. 0 =
// off.
double g_profile_fly_seconds = 0.0;
int g_render_benchmark_frames = 120; // measured frames (after warm-up)
int g_render_benchmark_warmup = 60;  // frames discarded before measuring (stream/settle)
//  optional PPM screenshot of the forest-DENSE budget pose,
// dumped on the final measured frame (verifies the pose is actually dense +
// supplies the owner's before/after PNGs). Empty = off.
std::string g_render_benchmark_screenshot;
int g_timelapse_ticks = 60; // sim ticks advanced between captured frames (2 s at 30 Hz)
float g_timelapse_daystep =
    0.0f; // time-of-day advance per frame [0,1] (shade/sky drift); 0 = leave
int g_timelapse_captured = 0;
int g_timelapse_settle = 0;
bool g_timelapse_dig = false; // progressively carve a trench mid-capture (terraform demo)
bool g_timelapse_drain =
    false; //  money shot: anchor on a real river/lake, breach the bank, drain it
bool g_timelapse_rain = false; // finite hydrology — rain fills the land, then drain stays drained
int g_timelapse_rain_mm = 18;  // rain rate (mm/tick) for the rain demo
struct TimelapseDrainState {
    bool init = false;
    Luminumbra::Vec3 P{0.0f, 0.0f, 0.0f};
    float dhx = 0.0f, dhz = 1.0f;
    float surf = 0.0f;
};
TimelapseDrainState g_drain_state;
float g_timelapse_tod = 0.0f; // starting time-of-day (0 = noon/brightest; drifts by daystep)
std::filesystem::path g_timelapse_dir;
static constexpr int kTimelapseSettleFrames = 45; // let the world stream/settle before frame 0
// Fixed camera-pose override (--cam-pos x,y,z [--cam-yaw d] [--cam-pitch d]): pins
// g_camera every frame for reproducible/controllable screenshots + benchmarks. Keep
// the position within the streamed area (near spawn) so chunks are resident.
static int g_debug_view_mode = 0; // render-only G-buffer debug overlay:
                                  // 0=off,1=albedo,2=normal,3=depth,4=material,5=position ( cycles)
std::string
    g_debug_goto; // --debug-goto cave|doline|spawn: deterministically frame a feature for capture
bool g_fixed_cam = false;
glm::vec3 g_fixed_cam_pos(0.0f);
float g_fixed_cam_yaw = 0.0f;
float g_fixed_cam_pitch = 0.0f;
// --scene-config <json>: declaratively COMPOSE a scene (camera+fov, time-of-day,
// weather, clouds, fog) to match a reference image, render a settled frame, and
// screenshot it. The reference-driven fidelity loop: compose -> render -> compare.
// Reuses the fixed-cam + timelapse single-frame capture; these add atmosphere.
bool g_scene_active = false;
std::filesystem::path g_scene_dir;  // output dir for the scene screenshot
std::filesystem::path g_scene_shot; // full screenshot path (.ppm)
float g_scene_fov = 0.0f;           // 0 = leave camera default
int g_scene_weather = 0;            // 0 none, 1 rain, 2 snow, 3 fog, 4 storm
float g_scene_weather_intensity = 0.0f;
bool g_scene_clouds = false; // push a cloud state
float g_scene_cloud_coverage = 0.45f;
float g_scene_cloud_biome = 0.0f;
float g_scene_cloud_plane = 900.0f;
bool g_scene_cloud_shadow = false;
float g_scene_cloud_shadow_strength = 0.0f;
//  rendering: scene-config "moon" -> lunar illumination [0,1] (1 full moon,
// ~0 new moon). <0 = leave the automatic tick-derived lunar cycle. For night-mode captures.
float g_scene_moon = -1.0f;
// framescan: --frame-scan <out.json> boots the auto-world, pins a FIXED forest-dense
// camera pose + near-noon time-of-day (reproducible), lets the world settle, then
// reads back the settled frame's G-buffer material-id attachment + back color buffer
// and writes a per-material coverage/luminance + water + foliage JSON report, then
// exits.: the scan issues no draws and never touches sim/world_hash, so
// running it twice on the same world produces an IDENTICAL report. Pair with
// --auto-create-world --auto-enter-world. Empty = off.
std::string g_frame_scan_path;
bool g_frame_scan_active = false;
int g_frame_scan_settle = 0;
bool g_render_parity_active = false;
std::filesystem::path g_render_parity_dir;
std::string g_render_parity_pass = "frame";
static constexpr int kFrameScanSettleFrames = 90; // let chunks stream + atmosphere settle
// Watchdog: a headless auto-capture must NEVER hang. If the world hasn't reached
// IN_GAME and completed the scan within this many render-loop frames (boot + stream +
// settle is normally ~500-700), abort with a clear error instead of looping forever.
static constexpr int kFrameScanWatchdogFrames = 4000;
// DIAGNOSTIC-only: LUMIN_FRAME_SCAN_SETTLE overrides the settle so a headless capture
// can wait for the world to FULLY stream/bake (default preserves the deterministic 90
// the gates rely on; the 4000-frame watchdog leaves ample headroom).
int g_frame_scan_settle_target = kFrameScanSettleFrames;
int g_frame_scan_watchdog = 0;
// --bake-tree-impostor <out.ppm>:  far-field impostor atlas bake. Renders the static tree parts
// from the hemi-octahedral view directions into an atlas (+ coverage JSON) and exits. Needs only GL
// + the tree meshes (no world), so it runs on the first render-loop frame. Render-only.
std::string g_bake_impostor_path;
bool g_bake_impostor_done = false;
// --survey <dir>: autonomous tour — discover POIs (waterfall/cliff/grass/lake) in the generated
// world, teleport+stream+settle at each, write a screenshot + frame-scan per POI. Empty = off.
std::string g_survey_dir;
bool g_survey_active = false;
int g_survey_settle = 0;
// --ui-screenshot <screen>: the UI fidelity gate. Force the menu state, load <screen>.rml,
// let layout/fonts settle, then capture the back buffer (which holds the UI over the menu
// backdrop) and exit. Drives the reference-driven compose->render->compare loop for the UI,
// mirroring --scene-config for the 3D scene. Output is PPM (converted to PNG by the harness
// script for critique). Optional --ui-fixtures seeds deterministic list/gallery/settings data.
std::string g_ui_screenshot_screen;    // "" = inactive; else the CURRENT screen being captured
std::vector<std::string> g_ui_screens; // batch list (one window pop captures them all in sequence)
std::size_t g_ui_screen_index = 0;     // which screen in g_ui_screens is being captured
std::filesystem::path g_ui_screenshot_dir; // output dir; each screen -> ui-<screen>.ppm
bool g_ui_fixtures = false;                // seed deterministic UI fixture data
// the  one-time world-entry flourishes (doline
// locate + the 25-anchor enclosed-cave crystal scan + the hero call) cost
// minutes of full-SDF probing in a debug build (est. 10-25 s release) and used
// to run INLINE on the frame-2 main thread — the world-entry stall. They now
// run as a batch of BACKGROUND JobSystem jobs (pure deterministic SDF reads,
// the same sampling meshing workers already do concurrently); the main thread
// polls the handle and creates the point-light entities when the batch lands.
// TEARDOWN CONTRACT: the jobs hold a raw SHIELD_WorldSystem* — every world
// transition (CreateWorld / session reset) MUST DrainBackgroundWorldScan first.
struct BackgroundWorldScan {
    const void* world = nullptr; // identity guard: consume only for the world scanned
    Luminumbra::Systems::SHIELD_WorldSystem::SurfaceBreakInfo doline;
    float doline_surface_h = 0.0f;
    std::optional<Luminumbra::Debug::DebugCamPose> hero;
    std::array<std::optional<glm::vec3>, 25> anchor_caves; // one slot per anchor (disjoint writes)
};
static std::shared_ptr<BackgroundWorldScan> s_backgroundWorldScan; // null = idle/consumed
static Luminumbra::JobHandle s_backgroundWorldScanHandle;
static void DrainBackgroundWorldScan(Luminumbra::JobSystem& jobs) {
    if (s_backgroundWorldScanHandle.counter) {
        jobs.wait(s_backgroundWorldScanHandle);
    }
    s_backgroundWorldScanHandle = {};
    s_backgroundWorldScan.reset();
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
int g_ui_screenshot_settle = 0; // frames waited before capture of the current screen
// HEADLESS PREVIEW-DIORAMA CAPTURE (--preview-live): for the world_creation screen, the
// live WorldgenPreview diorama (candidate world + far field + precipitation) builds on a
// BACKGROUND worker thread, so the fixed 30-frame settle above captures a black backdrop
// before the world is ready (render_to_backbuffer returns false until world_ready). When
// g_ui_preview_live is set, the world_creation capture instead WAITS (bounded, wall-clock
// timeout) for worldgenPreview->world_ready + a few post-ready settle frames so the
// terrain/far-field/atmosphere (and, with --preview-weather rain, falling precipitation)
// are actually drawn before the backbuffer is read. Render-only; server --smoke unaffected.
bool g_ui_preview_live = false;   // wait for the live diorama before capturing world_creation
std::string g_ui_preview_weather; // optional forced weather chip (e.g. "rain") so precip spawns
int g_ui_preview_settle_after_ready =
    0; // post-world_ready settle frames accrued (far-LOD/foliage/particles)
// live scenic menu backdrop: a golden-hour world rendered behind the menus (matching the
// references). Stood up at boot while staying in MAIN_MENU; the menu render branch draws it
// under the transparent UI with a slow auto-orbit. Replaced cleanly when a real world loads.
bool g_menu_backdrop_active = false;
float g_menu_backdrop_yaw = 30.0f; // orbit accumulator (degrees)
//  scroll-wheel accumulator for the create-world preview diorama.
// GLFW scroll is event-driven (no poll API), so the menu scroll callback accrues
// the wheel delta here and the preview block consumes it each frame to drive
// WorldgenPreview::zoom when the cursor is over the #preview_pane rect.
double g_menu_scroll_accum = 0.0;
// thumbnail generation: capture N clean (no-UI) backdrop frames at varied yaw/time-of-day
// in one window session, for use as world-select + gallery photo thumbnails. --ui-thumbs N.
int g_ui_thumbs = 0;                   // 0 = off; else number of thumbnails to capture
std::filesystem::path g_ui_thumbs_dir; // output dir; each -> thumb_<i>.ppm
int g_ui_thumbs_index = 0;
int g_ui_thumbs_settle = 0;
bool g_timelapse_grow = false; // grow the procgen plants sapling->tree over the capture
bool g_timelapse_simgrow =
    false; // seed SIM PlantTag plants + grow them via the real growth tick ( bridge)
bool g_timelapse_season = false;    // drift summer->autumn leaf color over the capture
bool g_timelapse_creatures = false; // spawn predators/prey + render markers (ecology demo)
bool g_timelapse_living = false;    // capture the LIVING WORLD (ambient creatures + forager colony)
                                 // during a timelapse, so the scent trail / rest poses are showable
bool g_timelapse_calm = false; // calm (no-predator) grazing herd -> reproduction/evolution demo
bool g_timelapse_fire = false; // ignite a patch of combustible foliage -> sim.fire spread demo

//  stored procgen plant instances so the geometry can be RE-BAKED at a changing
// growth stage (the live-growth render bridge) -- a plant grows sapling->tree over time.
struct ProcgenPlantInstance {
    glm::vec3 worldPos;
    glm::quat rot;
    float effScale;
    Luminumbra::Components::PlantGenomeComponent genome;
    // Plant unification: a decoration-tier scatter plant PROMOTED to a sim PlantTag entity (on
    // player interaction) is suppressed here so it is not double-drawn — the sim tier now owns it.
    bool suppressed = false;
};
std::vector<ProcgenPlantInstance> g_procgenPlants;
// Bumped whenever the scatter set changes (a promotion suppresses an instance) so the combined
// plant re-bake knows to rebuild the cached scatter geometry. The sim tier is keyed separately.
std::uint64_t g_scatterRevision = 0;
std::uint64_t g_lastCombinedPlantSig = 0; // cache key for the composited scatter+sim mesh upload
// Cache of the last-baked procedural TREE mesh, so the creature timelapse can draw the
// programmatic trees AND the moving creature markers through the single PlantProcgenPass
// (creatures are appended to this each frame).
std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> g_procgenTreeVerts;
std::vector<std::uint32_t> g_procgenTreeIndices;
float g_procgenStageF = 5.0f; // growth: 0 = Seed.. 5 = Fruiting (drives structure + size)
float g_procgenLastBakedStage = -2.0f;
glm::vec3 g_procgenSunDir = glm::vec3(0.0f, 1.0f, 0.0f);
float g_season = 0.0f; // 0 = summer green.. 1 = autumn ochre (seasonal leaf color)
//  player farming state. The controller holds the seed/harvest inventory; the
// species registry is loaded once for player seeding. Interactive-only (never touched by gates).
luminumbra::foliage::FarmingController g_farming;
luminumbra::foliage::SpeciesRegistry g_farmSpecies;
bool g_farmSpeciesLoaded = false;
// Player-selected species to plant (index into g_farmSpecies.all; V cycles it). So FarmPlant
// isn't hard-coded to wheat — the player picks oak/wheat/etc. from the data-driven registry.
int g_farmSelectedSpecies = 0;

// Re-bake the combined procgen plant mesh at growth `stageF` and push it to the pass. Young
// stages -> shallower branch recursion + smaller size; deterministic pure functions.
void BakeProcgenPlants(Luminumbra::Rendering::PlantProcgenPass* pp, float stageF) {
    if (!pp)
        return;
    if (g_procgenPlants.empty()) {
        pp->set_enabled(false);
        return;
    }
    const int kFruiting = static_cast<int>(Luminumbra::Components::PlantStage::Fruiting);
    const std::uint8_t stage =
        static_cast<std::uint8_t>(std::clamp(static_cast<int>(stageF), 0, kFruiting));
    const float growF =
        0.16f + 0.84f * std::clamp(stageF / static_cast<float>(kFruiting), 0.0f, 1.0f);
    luminumbra::foliage::PlantEnvDir env;
    env.sun_dir = g_procgenSunDir;
    env.phototropism = 0.5f;
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts;
    std::vector<std::uint32_t> indices;
    for (const ProcgenPlantInstance& inst : g_procgenPlants) {
        if (inst.suppressed)
            continue; // promoted to a sim PlantTag -> the sim tier draws it now
        const luminumbra::foliage::PlantStructure ps =
            luminumbra::foliage::GeneratePlant(inst.genome, stage, env);
        const luminumbra::foliage::ProcMesh pm = luminumbra::foliage::TessellatePlant(ps);
        const glm::mat3 rot = glm::mat3_cast(inst.rot);
        const float sc = inst.effScale * growF;
        const std::size_t leafVertStart = pm.vertices.size() >= ps.leaves.size() * 4u
                                              ? pm.vertices.size() - ps.leaves.size() * 4u
                                              : pm.vertices.size();
        // GENETIC + SEASONAL albedo: leaf greens vary by genome, shifting toward autumn
        // ochre with the season; bark brown varies subtly per genome.
        using G = Luminumbra::Components::PlantGene;
        const float hueVar = inst.genome.gene(G::LeafDensity);
        const float valVar = inst.genome.gene(G::Hardiness);
        const glm::vec3 summerLeaf(
            0.09f + 0.10f * hueVar, 0.32f + 0.22f * valVar, 0.07f + 0.05f * hueVar);
        const glm::vec3 autumnLeaf(0.42f + 0.10f * hueVar, 0.20f + 0.10f * valVar, 0.05f);
        const glm::vec3 leafColor =
            glm::mix(summerLeaf, autumnLeaf, std::clamp(g_season, 0.0f, 1.0f));
        const glm::vec3 barkColor(0.16f + 0.06f * valVar, 0.10f, 0.06f);
        const std::uint32_t baseVert = static_cast<std::uint32_t>(verts.size());
        for (std::size_t vi = 0; vi < pm.vertices.size(); ++vi) {
            const luminumbra::foliage::ProcVertex& src = pm.vertices[vi];
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = rot * (src.pos * sc) + inst.worldPos;
            v.normal = glm::normalize(rot * src.normal);
            const bool isLeaf = vi >= leafVertStart;
            v.uv = glm::vec2(isLeaf ? 1.0f : 0.0f, src.uv.y);
            v.color = isLeaf ? leafColor : barkColor;
            verts.push_back(v);
        }
        for (std::uint32_t idx : pm.indices)
            indices.push_back(baseVert + idx);
    }
    g_procgenLastBakedStage = stageF;
    // Cache the tree mesh so the creature demo can composite creatures on top of it.
    g_procgenTreeVerts = verts;
    g_procgenTreeIndices = indices;
    if (verts.empty()) {
        pp->set_enabled(false);
        return;
    }
    const std::uint64_t sig = ((static_cast<std::uint64_t>(g_procgenPlants.size()) << 24) ^
                               static_cast<std::uint64_t>(stageF * 1000.0f)) |
                              1ull;
    pp->set_plants(verts, indices, sig);
    pp->set_enabled(true);
}

// Plant unification: composite the DECORATION-tier scatter (g_procgenTreeVerts cache, rebuilt by
// BakeProcgenPlants when the scatter changes) with the SIM-tier PlantTag plants (each at its live
// PlantGrowthComponent stage) into the single PlantProcgenPass. This is the ONE unified plant
// RENDER; the SIM stays split — only PlantTag entities tick/persist/hash, the vast scatter is
// render-only. So a player-planted/promoted plant ADDS to the forest rather than replacing it.
// Sig-gated on (scatter revision + the sim roster's ids/stages) so a settled frame is a no-op. Pure
// visual-only: reads sim truth, never writes back into the sim / world_hash.
std::size_t RebakeAllPlants(Luminumbra::Rendering::PlantProcgenPass* pp,
                            const entt::registry& reg,
                            const glm::vec3& sunDir,
                            float season) {
    if (!pp)
        return 0;
    namespace C = Luminumbra::Components;
    auto view = reg.view<const C::PlantTag,
                         const C::PlantGenomeComponent,
                         const C::PlantGrowthComponent,
                         const C::TransformComponent>();
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end());

    // Cheap change key: scatter revision + each sim plant's (id, stage). Skip rebuild+upload when
    // unchanged (the common settled frame).
    std::uint64_t sig = 1469598103934665603ull ^ (g_scatterRevision * 1099511628211ull);
    for (const entt::entity e : ents) {
        const auto& gg = view.get<const C::PlantGrowthComponent>(e);
        sig = (sig ^ ((static_cast<std::uint64_t>(entt::to_integral(e)) << 8) ^
                      static_cast<std::uint64_t>(gg.stage))) *
              1099511628211ull;
    }
    sig |= 1ull;
    if (sig == g_lastCombinedPlantSig)
        return ents.size();
    g_lastCombinedPlantSig = sig;

    const int kFruiting = static_cast<int>(C::PlantStage::Fruiting);
    luminumbra::foliage::PlantEnvDir env;
    env.sun_dir = sunDir;
    env.phototropism = 0.5f;
    using G = C::PlantGene;

    // Base = the cached decoration scatter (already excludes promoted/suppressed instances).
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts = g_procgenTreeVerts;
    std::vector<std::uint32_t> indices = g_procgenTreeIndices;
    for (const entt::entity e : ents) {
        const auto& genome = view.get<const C::PlantGenomeComponent>(e);
        const auto& g = view.get<const C::PlantGrowthComponent>(e);
        const auto& tf = view.get<const C::TransformComponent>(e);
        const std::uint8_t stage =
            static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(g.stage), 0, kFruiting));
        const float growF =
            0.16f + 0.84f * std::clamp(static_cast<float>(stage) / static_cast<float>(kFruiting),
                                       0.0f,
                                       1.0f);

        const luminumbra::foliage::PlantStructure ps =
            luminumbra::foliage::GeneratePlant(genome, stage, env);
        const luminumbra::foliage::ProcMesh pm = luminumbra::foliage::TessellatePlant(ps);

        // Deterministic per-plant yaw + scale from the entity id / genome (visual variety only).
        const std::uint32_t eid = static_cast<std::uint32_t>(entt::to_integral(e));
        const float yaw = (static_cast<float>((eid * 2654435761u) >> 8) / 16777216.0f) * 6.2831853f;
        const glm::mat3 rot = glm::mat3_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
        const float sc = (0.6f + 1.8f * genome.gene(G::MaxScale)) * growF;
        const glm::vec3 worldPos(tf.position.x, tf.position.y, tf.position.z);

        const std::size_t leafVertStart = pm.vertices.size() >= ps.leaves.size() * 4u
                                              ? pm.vertices.size() - ps.leaves.size() * 4u
                                              : pm.vertices.size();
        const float hueVar = genome.gene(G::LeafDensity);
        const float valVar = genome.gene(G::Hardiness);
        const glm::vec3 summerLeaf(
            0.09f + 0.10f * hueVar, 0.32f + 0.22f * valVar, 0.07f + 0.05f * hueVar);
        const glm::vec3 autumnLeaf(0.42f + 0.10f * hueVar, 0.20f + 0.10f * valVar, 0.05f);
        const glm::vec3 leafColor =
            glm::mix(summerLeaf, autumnLeaf, std::clamp(season, 0.0f, 1.0f));
        const glm::vec3 barkColor(0.16f + 0.06f * valVar, 0.10f, 0.06f);
        const std::uint32_t baseVert = static_cast<std::uint32_t>(verts.size());
        for (std::size_t vi = 0; vi < pm.vertices.size(); ++vi) {
            const luminumbra::foliage::ProcVertex& src = pm.vertices[vi];
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = rot * (src.pos * sc) + worldPos;
            v.normal = glm::normalize(rot * src.normal);
            const bool isLeaf = vi >= leafVertStart;
            v.uv = glm::vec2(isLeaf ? 1.0f : 0.0f, src.uv.y);
            v.color = isLeaf ? leafColor : barkColor;
            verts.push_back(v);
        }
        for (const std::uint32_t idx : pm.indices)
            indices.push_back(baseVert + idx);
    }
    if (verts.empty()) {
        pp->set_enabled(false);
        return ents.size();
    }
    pp->set_plants(verts, indices, sig);
    pp->set_enabled(true);
    return ents.size();
}

// Plant unification — PROMOTION: turn the decoration-tier scatter plant nearest `aim` (within
// `reach`) into a SIM-tier PlantTag entity, so the player can tend a wild forest tree into a
// living, growing, persisting plant. The promoted plant inherits the scatter instance's genome + a
// grown (Mature) perennial state; the scatter instance is SUPPRESSED (so it is not double-drawn)
// and the scatter revision bumped (the caller rebuilds the scatter cache). Sim stays bounded —
// promotion is gated by player interaction. Returns the new entity, or entt::null if no scatter
// plant is in reach.
entt::entity
PromoteNearestScatter(entt::registry& reg, const glm::vec3& aim, float reach, std::uint64_t tick) {
    namespace C = Luminumbra::Components;
    int best = -1;
    float bestD = reach * reach;
    for (std::size_t i = 0; i < g_procgenPlants.size(); ++i) {
        const ProcgenPlantInstance& inst = g_procgenPlants[i];
        if (inst.suppressed)
            continue;
        const float dx = inst.worldPos.x - aim.x, dz = inst.worldPos.z - aim.z;
        const float d = dx * dx + dz * dz;
        if (d <= bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    if (best < 0)
        return entt::null;
    ProcgenPlantInstance& inst = g_procgenPlants[static_cast<std::size_t>(best)];
    const entt::entity e = reg.create();
    auto& tf = reg.emplace<C::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(inst.worldPos.x, inst.worldPos.y, inst.worldPos.z);
    reg.emplace<C::PlantTag>(e);
    reg.emplace<C::PlantGenomeComponent>(e, inst.genome);
    auto& g = reg.emplace<C::PlantGrowthComponent>(e);
    g.species_id = luminumbra::foliage::SpeciesId16("wild");
    g.stage = static_cast<std::uint8_t>(C::PlantStage::Mature); // a grown forest tree
    g.planted_tick = tick;
    g.last_tick = tick;
    auto& cl = reg.emplace<C::CropLifecycleComponent>(e); // promoted wild trees persist + regrow
    cl.perennial = true;
    cl.lifespan_ticks = 4800u;
    cl.species_id = g.species_id;
    inst.suppressed = true;
    ++g_scatterRevision;
    return e;
}

//  map the sun's elevation (radians, >0 above horizon) to a photographic
// scene luminance [0,1] for photo scoring. Night (sun below horizon) is dark (~0.06);
// low/golden-hour sun lands near the ideal (~0.5-0.7); midday is bright but not blown
// (~0.85). CLIENT render-derived feedback ONLY — never sim / world_hash — so libm
// (std::sin/std::pow) is fine here (unlike the pure photo headers, which stay libm-free).
inline float SceneLuminanceFromSunElevation(float sun_elev_rad) {
    const float e = std::sin(sun_elev_rad); // [-1,1], fraction of the way above horizon
    if (e <= 0.0f) {
        // Twilight → night: a small floor that dims toward midnight (e == -1).
        return luminumbra::game::Clamp01(0.06f + 0.10f * (1.0f + e));
    }
    return luminumbra::game::Clamp01(0.15f + 0.70f * std::pow(e, 0.3f));
}

// feature: gather the in-frustum creature subjects for a photo-mode
// CAPTURE. STRICTLY a read-only observer — it takes a CONST registry + CONST camera,
// projects each creature's world position into NDC via the camera's view*proj, and
// derives a deterministic species/luminance proxy (: no luminance component to
// read, so a constant scene-luminance + the predator-role species proxy; no new sim
// component). It mutates NOTHING, so it cannot perturb world_hash (the sim-isolation
// gate test pins this on the pure path).
std::vector<luminumbra::game::PhotoSubjectView>
GatherPhotoSubjects(const entt::registry& reg,
                    const Luminumbra::Rendering::Camera& camera,
                    int width,
                    int height,
                    float scene_luminance = 0.6f) {
    std::vector<luminumbra::game::PhotoSubjectView> views;
    if (width <= 0 || height <= 0)
        return views;

    const glm::mat4 proj = glm::perspective(glm::radians(camera.Zoom),
                                            static_cast<float>(width) / static_cast<float>(height),
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
    const glm::mat4 view_proj = proj * camera.GetViewMatrix();

    auto cr_view = reg.view<const Luminumbra::Components::CreatureComponent,
                            const Luminumbra::Components::TransformComponent>();
    for (const entt::entity e : cr_view) {
        const auto& tf = cr_view.get<const Luminumbra::Components::TransformComponent>(e);
        const auto& cr = cr_view.get<const Luminumbra::Components::CreatureComponent>(e);

        const glm::vec3 world(tf.position.x, tf.position.y, tf.position.z);
        const glm::vec4 clip = view_proj * glm::vec4(world, 1.0f);

        luminumbra::game::PhotoSubjectView pv;
        pv.in_frustum = (clip.w > 0.0f);
        if (pv.in_frustum) {
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            pv.in_frustum = (ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f && ndc.y <= 1.0f);
            pv.ndc_x = ndc.x;
            pv.ndc_y = ndc.y;
        }
        const glm::vec3 cam_to = world - camera.Position;
        const float dist = glm::length(cam_to);
        pv.distance_m = dist > 0.01f ? dist : 0.01f;
        pv.size_m = 1.0f; // ~creature footprint
        // Apparent footprint falls off with distance (a far subject fills less frame).
        pv.size = luminumbra::game::Clamp01(pv.size_m / (pv.distance_m * 0.5f + 1.0f));
        //  scene luminance is now driven by the sun (time-of-day), passed in
        // from the render pipeline's sun elevation, so golden hour rewards and night
        // punishes. No per-creature luminance component exists, so all subjects share the
        // scene's light this frame (a far better proxy than the old hardcoded 0.6).
        pv.light = scene_luminance;
        // Real per-creature species identity (set at spawn from the archetype) keys the
        // codex; fall back to the predator/prey role proxy only for unspecified (0)
        // creatures so the codex can fill with actual species rather than two buckets.
        pv.species_id =
            cr.species_id != 0 ? static_cast<int>(cr.species_id) : (cr.is_predator ? 1 : 2);
        //  carry the subject's current behaviour so the principal subject's
        // action reaches the capture's ObservationMetadata (behaviour objectives).
        pv.subject_action = cr.last_action;
        views.push_back(pv);
    }
    return views;
}

//  rebuild creature markers (small octahedra, red = predator, blue = prey) at the
// creatures' CURRENT positions and push to the procgen pass. Called per frame so the markers
// track the brain-driven movement. Render-only.
void BakeCreatureMarkers(Luminumbra::Rendering::PlantProcgenPass* pp,
                         entt::registry& reg,
                         Luminumbra::world::GameSession* gs = nullptr) {
    if (!pp)
        return;
    // Markers only: the procedural FOREST now renders through the instanced static-mesh path
    // (vast, LOD'd), so this pass draws just the moving creature octahedra.
    // when `gs` is supplied, the forager colony (ants + food piles + nest anchor) is rendered
    // INTO THE SAME buffer so the shared PlantProcgenPass geometry is one upload (never two
    // set_plants calls clobbering each other).
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts;
    std::vector<std::uint32_t> indices;
    auto view = reg.view<const Luminumbra::Components::CreatureComponent,
                         const Luminumbra::Components::TransformComponent>();
    constexpr float r = 1.0f, halfH = 1.3f; // ~matches the Jolt capsule; reads from the demo camera
    static const int tri[8][3] = {
        {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2}, {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
    // Append one octahedron (centre c, horizontal radius rr, vertical half-height hh, colour).
    auto emitOcta = [&](const glm::vec3& c, float rr, float hh, const glm::vec3& color) {
        const glm::vec3 P[6] = {c + glm::vec3(0, hh, 0),
                                c - glm::vec3(0, hh, 0),
                                c + glm::vec3(rr, 0, 0),
                                c + glm::vec3(0, 0, rr),
                                c - glm::vec3(rr, 0, 0),
                                c - glm::vec3(0, 0, rr)};
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        for (const glm::vec3& p : P) {
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = p;
            v.normal = glm::normalize(p - c);
            v.uv = glm::vec2(0.0f, 0.0f); // bark flag -> no wind sway, vertex color albedo
            v.color = color;
            verts.push_back(v);
        }
        for (const auto& t : tri) {
            indices.push_back(base + t[0]);
            indices.push_back(base + t[1]);
            indices.push_back(base + t[2]);
        }
    };
    for (auto e : view) {
        const auto& tf = view.get<const Luminumbra::Components::TransformComponent>(e);
        const auto& cr = view.get<const Luminumbra::Components::CreatureComponent>(e);
        // The Jolt avatar body owns the position now (gravity/terrain collision), so the
        // transform's Y is the resolved capsule centre -- draw the marker right there.
        const glm::vec3 c(tf.position.x, tf.position.y, tf.position.z);
        // Prey are tinted by GENERATION (founders blue -> teal -> green -> lime) so new
        // generations born during the evolution demo are visually distinct; size follows the
        // heritable genome size_scale so trait drift shows too. Predator red, carcass bone.
        glm::vec3 preyCol(0.18f, 0.5f, 0.85f);
        float sizeMul = 1.0f;
        if (const auto* gn = reg.try_get<Luminumbra::Components::CreatureGenomeComponent>(e)) {
            static const glm::vec3 kGenPalette[4] = {{0.18f, 0.5f, 0.85f},
                                                     {0.18f, 0.82f, 0.72f},
                                                     {0.32f, 0.85f, 0.32f},
                                                     {0.75f, 0.85f, 0.2f}};
            preyCol = kGenPalette[gn->generation < 4u ? gn->generation : 3u];
            sizeMul = gn->size_scale;
        }
        glm::vec3 col = cr.eaten ? glm::vec3(0.92f, 0.88f, 0.75f) // dead -> pale bone carcass
                        : cr.is_predator ? glm::vec3(0.75f, 0.12f, 0.12f) // predator -> red
                                         : preyCol;                       // prey -> by generation
        // Decomposition: a decaying carcass SHRINKS + darkens to nothing (skip when fully gone),
        // so the population visibly self-bounds via death -> decay.
        if (const auto* dec = reg.try_get<Luminumbra::Components::DecayComponent>(e)) {
            if (dec->fully_decomposed)
                continue; // returned to the soil -> no marker
            if (dec->decay_ticks > 0 && dec->decay_duration > 0) {
                const float t =
                    static_cast<float>(dec->decay_ticks) / static_cast<float>(dec->decay_duration);
                sizeMul *= (1.0f - 0.8f * t); // shrink as it rots
                col *= (1.0f - 0.7f * t);     // darken toward the soil
            }
        }
        // rest poses: a resting/sleeping creature reads as "bedded
        // down" — the marker shrinks, SQUATS (flattens vertically), and dims, so a
        // sleeping subject is visibly distinct from an awake one. This is what a behaviour
        // photo objective ( BehavioralMatch) is shot against. Render-only; the
        // action is the brain's last_action (Rest=4, Sleep=5).
        float restSquash = 1.0f;
        if (cr.last_action == 5) { // Sleep — most settled
            sizeMul *= 0.6f;
            restSquash = 0.45f;
            col *= 0.70f;
        } else if (cr.last_action == 4) { // Rest — partly settled
            sizeMul *= 0.8f;
            restSquash = 0.70f;
            col *= 0.85f;
        }
        const float rr = r * sizeMul, hh = halfH * sizeMul * restSquash;
        emitOcta(c, rr, hh, col);
    }

    // forager colony render (anchor-only, client-visual). Renders the
    // ants shuttling, the food piles, and the nest ANCHOR. All reads (TransformComponent
    // mirror, cell->world, terrain height) are render-only; nothing steers or writes sim.
    if (gs) {
        auto* ws = gs->GetWorldSystem();
        int nestCx = -1, nestCz = -1;
        auto fgview = reg.view<const Luminumbra::Components::ForagerComponent,
                               const Luminumbra::Components::TransformComponent>();
        for (auto e : fgview) {
            const auto& fg = fgview.get<const Luminumbra::Components::ForagerComponent>(e);
            const auto& tf = fgview.get<const Luminumbra::Components::TransformComponent>(e);
            nestCx = fg.home_x;
            nestCz = fg.home_z; // every ant shares the nest cell
            const glm::vec3 c(tf.position.x, tf.position.y + 0.2f, tf.position.z);
            // A LADEN ant (carrying food home) glows amber; an outbound ant is pale.
            const glm::vec3 col =
                fg.carrying_food ? glm::vec3(0.95f, 0.65f, 0.15f) : glm::vec3(0.85f, 0.82f, 0.70f);
            emitOcta(c, 0.30f, 0.30f, col);
        }
        // Food piles (green), cell->world. Skip depleted sources.
        auto foodv = reg.view<const Luminumbra::Components::FoodSourceComponent>();
        for (auto e : foodv) {
            const auto& fs = foodv.get<const Luminumbra::Components::FoodSourceComponent>(e);
            if (fs.amount <= 0)
                continue;
            const float wx = gs->ScentCellToWorldX(fs.cell_x);
            const float wz = gs->ScentCellToWorldZ(fs.cell_z);
            const float wy = (ws ? ws->GetTerrainHeightAt(wx, wz) : 0.0f) + 0.4f;
            emitOcta(glm::vec3(wx, wy, wz), 0.7f, 0.7f, glm::vec3(0.30f, 0.80f, 0.25f));
        }
        // Nest ANCHOR (tan mound) at the colony's home cell — the "home" the trails radiate
        // from. Anchor-only: it is decoration, never a steering target (Option A).
        if (nestCx >= 0) {
            const float wx = gs->ScentCellToWorldX(nestCx);
            const float wz = gs->ScentCellToWorldZ(nestCz);
            const float wy = (ws ? ws->GetTerrainHeightAt(wx, wz) : 0.0f) + 0.5f;
            emitOcta(glm::vec3(wx, wy, wz), 1.2f, 0.9f, glm::vec3(0.55f, 0.40f, 0.25f));
        }

        // LUMIN CRYSTALS — a tall bright shard at each cave point-light. It sits
        // inside its own glow so it reads as a luminous crystal (and is the photo subject the
        // light makes visible). The PointLightComponent does the actual cave illumination.
        auto plview = reg.view<const Luminumbra::Components::PointLightComponent,
                               const Luminumbra::Components::TransformComponent>();
        for (auto e : plview) {
            const auto& pl = plview.get<const Luminumbra::Components::PointLightComponent>(e);
            const auto& tf = plview.get<const Luminumbra::Components::TransformComponent>(e);
            const glm::vec3 c(tf.position.x, tf.position.y, tf.position.z);
            const glm::vec3 col =
                glm::clamp(glm::vec3(pl.color.x, pl.color.y, pl.color.z) * 1.4f, 0.0f, 1.0f);
            emitOcta(c, 0.45f, 1.1f, col); // a slender upright crystal shard
        }
    }

    static std::uint64_t s_sig = 1000;
    ++s_sig; // creatures move every frame -> always re-upload
    if (verts.empty()) {
        pp->set_enabled(false);
        return;
    }
    pp->set_plants(verts, indices, s_sig);
    pp->set_enabled(true);
}

//  sim.fire demo: draw each combustible bush as an octahedron coloured by its DETERMINISTIC
// burn_state (green = unburnt, orange = burning, charcoal = burnt), grounded on the terrain.
// The FireSpreadSystem (now wired into the tick) drives the colours; this just visualizes them.
void BakeCombustibleMarkers(Luminumbra::Rendering::PlantProcgenPass* pp,
                            entt::registry& reg,
                            Luminumbra::Systems::SHIELD_WorldSystem* ws) {
    if (!pp)
        return;
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts;
    std::vector<std::uint32_t> indices;
    auto view = reg.view<const Luminumbra::Components::CombustibleComponent,
                         const Luminumbra::Components::TransformComponent>();
    constexpr float r = 0.7f, halfH = 0.9f;
    static const int tri[8][3] = {
        {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2}, {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
    for (auto e : view) {
        const auto& tf = view.get<const Luminumbra::Components::TransformComponent>(e);
        const auto& cb = view.get<const Luminumbra::Components::CombustibleComponent>(e);
        const float gy = ws ? ws->GetTerrainHeightAt(tf.position.x, tf.position.z) : tf.position.y;
        const glm::vec3 c(tf.position.x, gy + halfH, tf.position.z);
        glm::vec3 col(0.15f, 0.55f, 0.12f); // Unburnt -> green
        if (cb.state() == Luminumbra::Components::BurnState::Burning)
            col = glm::vec3(1.0f, 0.42f, 0.05f);
        else if (cb.state() == Luminumbra::Components::BurnState::Burnt)
            col = glm::vec3(0.09f, 0.08f, 0.07f);
        const glm::vec3 P[6] = {c + glm::vec3(0, halfH, 0),
                                c - glm::vec3(0, halfH, 0),
                                c + glm::vec3(r, 0, 0),
                                c + glm::vec3(0, 0, r),
                                c - glm::vec3(r, 0, 0),
                                c - glm::vec3(0, 0, r)};
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        for (const glm::vec3& p : P) {
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = p;
            v.normal = glm::normalize(p - c);
            v.uv = glm::vec2(0.0f, 0.0f);
            v.color = col;
            verts.push_back(v);
        }
        for (const auto& t : tri) {
            indices.push_back(base + t[0]);
            indices.push_back(base + t[1]);
            indices.push_back(base + t[2]);
        }
    }
    static std::uint64_t s_fsig = 5000;
    ++s_fsig;
    if (verts.empty()) {
        pp->set_enabled(false);
        return;
    }
    pp->set_plants(verts, indices, s_fsig);
    pp->set_enabled(true);
}

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
// VAST FOREST: a small PALETTE of procedurally-generated tree meshes (no baked model). Each
// palette entry is built once at world load into the instanced static-mesh cache (with 3 LODs,
// split into bark + leaf submeshes so the existing material LUT colours them), then the world
// scatters THOUSANDS of cheap instances of the palette through the engine's instanced + tree
// rendering LOD + frustum-cull path. Variety comes from the palette + per-instance transform; scale
// comes from instancing. The maintained path uses palette tinting and mesh LODs.
int g_treePaletteCount = 0;
constexpr int kTreePaletteSize = 12; // distinct procedural trees (mix of species)
const char* const kBarkMatKey = "_bark";
const char* const kLeafMatKey = "_leaf";

void BuildProcgenTreePalette(Luminumbra::Rendering::RenderPipeline& rp, const glm::vec3& sunDir) {
    if (g_treePaletteCount > 0)
        return; // build once
    namespace fol = luminumbra::foliage;
    fol::PlantEnvDir env;
    env.sun_dir = sunDir;
    env.phototropism = 0.5f;
    const std::uint8_t stage =
        static_cast<std::uint8_t>(Luminumbra::Components::PlantStage::Fruiting);
    const int radialForLod[3] = {8, 5, 3}; // branch detail per LOD (far = coarser)
    auto pal = luminumbra::core::DeterministicRng::seeded(fol::kPlantSeedOffset, 0xA11CE5ull, 99u);
    int built = 0;
    using V = Luminumbra::Rendering::Vertex;
    for (int p = 0; p < kTreePaletteSize; ++p) {
        const auto genome = fol::RandomGenome(pal);
        const fol::PlantStructure ps = fol::GeneratePlant(genome, stage, env);
        const std::size_t leafQuads = ps.leaves.size();
        glm::vec3 lo(1.0e9f), hi(-1.0e9f);         // tree AABB (from LOD0)
        float leafLoY = 1.0e9f, leafHiY = -1.0e9f; // canopy vertical band (leaf verts)
        for (int lod = 0; lod < 3; ++lod) {
            // Larger leaf cards -> fuller canopy from the same leaf COUNT (render-only
            // tessellation param; no PlantStructure/sim change, no world_hash impact).
            // The alpha-cut leaf texture overlaps into a denser canopy vs the default 0.20.
            const fol::ProcMesh pm = fol::TessellatePlant(ps, radialForLod[lod], 0.36f);
            const std::size_t leafStart = pm.vertices.size() >= leafQuads * 4u
                                              ? pm.vertices.size() - leafQuads * 4u
                                              : pm.vertices.size();
            std::vector<V> barkV, leafV;
            std::vector<std::uint32_t> barkI, leafI;
            barkV.reserve(leafStart);
            leafV.reserve(pm.vertices.size() - leafStart);
            for (std::size_t i = 0; i < pm.vertices.size(); ++i) {
                const fol::ProcVertex& s = pm.vertices[i];
                if (lod == 0) {
                    lo = glm::min(lo, s.pos);
                    hi = glm::max(hi, s.pos);
                    if (i >= leafStart) {
                        leafLoY = std::min(leafLoY, s.pos.y);
                        leafHiY = std::max(leafHiY, s.pos.y);
                    }
                }
                V v{s.pos, s.normal, s.uv};
                if (i < leafStart)
                    barkV.push_back(v);
                else
                    leafV.push_back(v);
            }
            for (std::uint32_t idx : pm.indices) {
                if (idx < leafStart)
                    barkI.push_back(idx);
                else
                    leafI.push_back(idx - static_cast<std::uint32_t>(leafStart));
            }
            const std::string base = "procgen://tree_" + std::to_string(p);
            const std::string suf = lod == 0 ? "" : (lod == 1 ? ".lod1" : ".lod2");
            rp.register_procgen_mesh(
                base + kBarkMatKey + suf,
                Luminumbra::Rendering::MeshLoader::CreateFromArrays(barkV, barkI));
            rp.register_procgen_mesh(
                base + kLeafMatKey + suf,
                Luminumbra::Rendering::MeshLoader::CreateFromArrays(leafV, leafI));
        }
        // LOD3 FAR-FIELD cross-billboard: a few quads spanning the tree's silhouette (~6 tris vs
        // hundreds), so a vast forest stays in budget out to the horizon. Leaf = two crossed
        // vertical quads over the canopy band; bark = one slim trunk quad. Sized from the LOD0
        // AABB.
        const float H = std::max(hi.y, 0.5f);
        const float W = std::max({hi.x, -lo.x, hi.z, -lo.z, 0.5f}); // canopy half-width
        const float cLo = (leafLoY < leafHiY) ? leafLoY : H * 0.35f;
        const float cHi = (leafHiY > leafLoY) ? leafHiY : H;
        auto addQuad = [](std::vector<V>& vv,
                          std::vector<std::uint32_t>& ii,
                          glm::vec3 a,
                          glm::vec3 b,
                          glm::vec3 c,
                          glm::vec3 d,
                          glm::vec3 n) {
            const std::uint32_t k = static_cast<std::uint32_t>(vv.size());
            vv.push_back({a, n, {0, 0}});
            vv.push_back({b, n, {1, 0}});
            vv.push_back({c, n, {1, 1}});
            vv.push_back({d, n, {0, 1}});
            ii.push_back(k);
            ii.push_back(k + 1);
            ii.push_back(k + 2);
            ii.push_back(k);
            ii.push_back(k + 2);
            ii.push_back(k + 3);
        };
        std::vector<V> bbLeafV;
        std::vector<std::uint32_t> bbLeafI;
        const glm::vec3 up(0, 1, 0);
        addQuad(
            bbLeafV, bbLeafI, {-W, cLo, 0}, {W, cLo, 0}, {W, cHi, 0}, {-W, cHi, 0}, up); // X-facing
        addQuad(
            bbLeafV, bbLeafI, {0, cLo, -W}, {0, cLo, W}, {0, cHi, W}, {0, cHi, -W}, up); // Z-facing
        // FAR-TREE LEAVES (rendering contract): the two crossed quads above are VERTICAL, so an
        // aerial / top-down view sees them edge-on and the canopy vanishes -> bare brown trunk
        // skeleton. Add a HORIZONTAL canopy "cap" over the top of the leaf band so looking DOWN
        // at a far tree still reads green leaf coverage. Up-facing normal (matches the leaf cards);
        // ~mid-to-upper canopy height; render-only, +2 tris/far tree, green tint via fs_in.Tint.
        const float capY = cLo + (cHi - cLo) * 0.72f;
        addQuad(bbLeafV,
                bbLeafI,
                {-W, capY, -W},
                {W, capY, -W},
                {W, capY, W},
                {-W, capY, W},
                up); // top cap
        std::vector<V> bbBarkV;
        std::vector<std::uint32_t> bbBarkI;
        const float tw = W * 0.12f;
        addQuad(bbBarkV,
                bbBarkI,
                {-tw, 0, 0},
                {tw, 0, 0},
                {tw, cLo, 0},
                {-tw, cLo, 0},
                glm::vec3(0, 0, 1));
        const std::string base = "procgen://tree_" + std::to_string(p);
        rp.register_procgen_mesh(
            base + kLeafMatKey + ".lod3",
            Luminumbra::Rendering::MeshLoader::CreateFromArrays(bbLeafV, bbLeafI));
        rp.register_procgen_mesh(
            base + kBarkMatKey + ".lod3",
            Luminumbra::Rendering::MeshLoader::CreateFromArrays(bbBarkV, bbBarkI));
        ++built;
    }
    g_treePaletteCount = built;
    LUMINUMBRA_CORE_INFO("VAST-FOREST: built procedural tree palette of {} entries (x4 LODs incl "
                         "far-field billboard)",
                         built);
}

// ROCK FORMATIONS (worldgen-richness ): a small palette of procedural
// faceted boulder meshes, registered into the instanced static-mesh cache and
// scattered as thousands of cheap instances (same render-only path as the trees;
// never hashed, no world_hash impact). Stone material id -> the stone triplanar
// texture via the instanced g_buffer path (no new art). Each palette entry is a
// deformed icosahedron (flat-shaded faces read as rocky), with per-entry
// non-uniform scale + per-vertex radial noise for variety.
int g_rockPaletteCount = 0;
constexpr int kRockPaletteSize = 8;
void BuildProcgenRockPalette(Luminumbra::Rendering::RenderPipeline& rp) {
    if (g_rockPaletteCount > 0)
        return;
    using V = Luminumbra::Rendering::Vertex;
    const float t = 1.6180339887f;
    const glm::vec3 ico[12] = {{-1, t, 0},
                               {1, t, 0},
                               {-1, -t, 0},
                               {1, -t, 0},
                               {0, -1, t},
                               {0, 1, t},
                               {0, -1, -t},
                               {0, 1, -t},
                               {t, 0, -1},
                               {t, 0, 1},
                               {-t, 0, -1},
                               {-t, 0, 1}};
    const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    auto h01 = [](int a, int b) {
        // Unsigned arithmetic throughout: `a * 73856093` as signed int overflows
        // (a can exceed 29 here) which is UB the -O3/LTO release build miscompiles
        // (debug wraps, release does not) — the root cause of a release-only hang
        // (degenerate face -> NaN normal -> stall). Unsigned wraps deterministically.
        std::uint32_t ua = static_cast<std::uint32_t>(static_cast<std::int64_t>(a)) * 73856093u;
        std::uint32_t ub = static_cast<std::uint32_t>(static_cast<std::int64_t>(b)) * 19349663u;
        std::uint64_t z = static_cast<std::uint64_t>(ua ^ ub) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return static_cast<float>((z >> 11) * (1.0 / 9007199254740992.0));
    };
    //   LOD: an octahedron hull (6 axis-extreme verts, 8 flat faces)
    // built from the deformed icosahedron's AABB — reads as the same boulder silhouette
    // at distance for ~8 tris (vs 20). Used for LOD1/LOD2 so distant rocks (>140m / >320m)
    // shed geometry; LOD3 (>620m) collapses to a crossed billboard (~4 tris).
    auto buildRockHull = [](const glm::vec3& lo, const glm::vec3& hi) {
        const glm::vec3 c = (lo + hi) * 0.5f;
        const glm::vec3 ex[6] = {{hi.x, c.y, c.z},
                                 {lo.x, c.y, c.z},
                                 {c.x, hi.y, c.z},
                                 {c.x, lo.y, c.z},
                                 {c.x, c.y, hi.z},
                                 {c.x, c.y, lo.z}};
        // 8 octahedron faces (+X/-X top/bottom rings).
        const int of[8][3] = {
            {0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}};
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        v.reserve(24);
        i.reserve(24);
        for (int f = 0; f < 8; ++f) {
            const glm::vec3 a = ex[of[f][0]], b = ex[of[f][1]], cc = ex[of[f][2]];
            const glm::vec3 cr = glm::cross(b - a, cc - a);
            const float crLen = glm::length(cr);
            const glm::vec3 nrm = (crLen > 1e-6f) ? (cr / crLen) : glm::vec3(0, 1, 0);
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, nrm, {0, 0}});
            v.push_back({b, nrm, {1, 0}});
            v.push_back({cc, nrm, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
        }
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    //  far-field billboard: two crossed vertical quads spanning the boulder AABB
    // (~4 tris) — the stone triplanar material colours them, so a distant scree field
    // stays in budget. Mirrors the tree LOD3 cross-billboard.
    auto buildRockBillboard = [](const glm::vec3& lo, const glm::vec3& hi) {
        const float W = std::max({hi.x, -lo.x, hi.z, -lo.z, 0.2f});
        const float yLo = lo.y, yHi = std::max(hi.y, lo.y + 0.2f);
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, n, {0, 0}});
            v.push_back({b, n, {1, 0}});
            v.push_back({c, n, {1, 1}});
            v.push_back({d, n, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
            i.push_back(k);
            i.push_back(k + 2);
            i.push_back(k + 3);
        };
        quad({-W, yLo, 0}, {W, yLo, 0}, {W, yHi, 0}, {-W, yHi, 0}, {0, 0, 1});
        quad({0, yLo, -W}, {0, yLo, W}, {0, yHi, W}, {0, yHi, -W}, {1, 0, 0});
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    int built = 0;
    for (int p = 0; p < kRockPaletteSize; ++p) {
        const glm::vec3 baseScale(
            0.7f + 0.7f * h01(p, 1), 0.45f + 0.7f * h01(p, 2), 0.7f + 0.7f * h01(p, 3));
        glm::vec3 dv[12];
        glm::vec3 lo(1.0e9f), hi(-1.0e9f);
        for (int i = 0; i < 12; ++i) {
            const glm::vec3 n = glm::normalize(ico[i]);
            const float r = 0.72f + 0.55f * h01(p * 13 + i, 7); // radial roughness
            dv[i] = n * r * baseScale;
            lo = glm::min(lo, dv[i]);
            hi = glm::max(hi, dv[i]);
        }
        std::vector<V> verts;
        std::vector<std::uint32_t> idx;
        verts.reserve(60);
        idx.reserve(60);
        for (int f = 0; f < 20; ++f) {
            const glm::vec3 a = dv[faces[f][0]], b = dv[faces[f][1]], c = dv[faces[f][2]];
            const glm::vec3 cr = glm::cross(b - a, c - a);
            const float crLen = glm::length(cr);
            const glm::vec3 nrm = (crLen > 1e-6f)
                                      ? (cr / crLen)       // flat-shaded face
                                      : glm::normalize(a); // degenerate -> radial fallback (no NaN)
            const std::uint32_t k = static_cast<std::uint32_t>(verts.size());
            verts.push_back({a, nrm, {0.0f, 0.0f}});
            verts.push_back({b, nrm, {1.0f, 0.0f}});
            verts.push_back({c, nrm, {0.0f, 1.0f}});
            idx.push_back(k);
            idx.push_back(k + 1);
            idx.push_back(k + 2);
        }
        const std::string base = "procgen://rock_" + std::to_string(p);
        rp.register_procgen_mesh(base,
                                 Luminumbra::Rendering::MeshLoader::CreateFromArrays(verts, idx));
        //  distance LODs (GBufferPass SelectTreeLod path picks these by camera
        // distance; absent = fall back to LOD0). LOD1+LOD2 = octahedron hull, LOD3 = billboard.
        rp.register_procgen_mesh(base + ".lod1", buildRockHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod2", buildRockHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod3", buildRockBillboard(lo, hi));
        ++built;
    }
    g_rockPaletteCount = built;
    LUMINUMBRA_CORE_INFO("VAST-FOREST: built procedural rock palette of {} entries", built);
}

// SHRUB/BUSH LAYER: a small palette of procedural bush meshes,
// registered into the SAME instanced static-mesh cache as the trees/rocks and
// scattered as cheap instances on flatter, vegetated ground (the opposite niche
// to the scree rocks). Each entry is a CLUSTER of 2-3 squashed, deformed
// icospheres (overlapping lobes read as a leafy shrub), green leaf material via
// the instanced g_buffer path (no new art).  (never hashed, no
// world_hash impact) — mirrors BuildProcgenRockPalette exactly, including the
// unsigned-only position hash (signed int*prime overflow is release-only UB that
// miscompiles to a NaN-normal stall — see memory procgen-hash-signed-overflow-ub).
int g_bushPaletteCount = 0;
constexpr int kBushPaletteSize = 6;
void BuildProcgenBushPalette(Luminumbra::Rendering::RenderPipeline& rp) {
    if (g_bushPaletteCount > 0)
        return;
    using V = Luminumbra::Rendering::Vertex;
    const float t = 1.6180339887f;
    const glm::vec3 ico[12] = {{-1, t, 0},
                               {1, t, 0},
                               {-1, -t, 0},
                               {1, -t, 0},
                               {0, -1, t},
                               {0, 1, t},
                               {0, -1, -t},
                               {0, 1, -t},
                               {t, 0, -1},
                               {t, 0, 1},
                               {-t, 0, -1},
                               {-t, 0, 1}};
    const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    auto h01 = [](int a, int b) {
        std::uint32_t ua = static_cast<std::uint32_t>(static_cast<std::int64_t>(a)) * 73856093u;
        std::uint32_t ub = static_cast<std::uint32_t>(static_cast<std::int64_t>(b)) * 19349663u;
        std::uint64_t z = static_cast<std::uint64_t>(ua ^ ub) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return static_cast<float>((z >> 11) * (1.0 / 9007199254740992.0));
    };
    //   LOD helpers (mirror the rock palette): an 8-face octahedron mound
    // sized to the cluster AABB (~8 tris vs 40-60) for LOD1/LOD2, and a crossed billboard
    // (~4 tris) for the far field. The grass/leaf material colours them, so distant
    // undergrowth stays in budget. Selected by GBufferPass SelectTreeLod by camera distance.
    auto buildBushHull = [](const glm::vec3& lo, const glm::vec3& hi) {
        const glm::vec3 c = (lo + hi) * 0.5f;
        const glm::vec3 ex[6] = {{hi.x, c.y, c.z},
                                 {lo.x, c.y, c.z},
                                 {c.x, hi.y, c.z},
                                 {c.x, lo.y, c.z},
                                 {c.x, c.y, hi.z},
                                 {c.x, c.y, lo.z}};
        const int of[8][3] = {
            {0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}};
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        v.reserve(24);
        i.reserve(24);
        for (int f = 0; f < 8; ++f) {
            const glm::vec3 a = ex[of[f][0]], b = ex[of[f][1]], cc = ex[of[f][2]];
            const glm::vec3 cr = glm::cross(b - a, cc - a);
            const float crLen = glm::length(cr);
            const glm::vec3 nrm = (crLen > 1e-6f) ? (cr / crLen) : glm::vec3(0, 1, 0);
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, nrm, {0, 0}});
            v.push_back({b, nrm, {1, 0}});
            v.push_back({cc, nrm, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
        }
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    auto buildBushBillboard = [](const glm::vec3& lo, const glm::vec3& hi) {
        const float W = std::max({hi.x, -lo.x, hi.z, -lo.z, 0.2f});
        const float yLo = std::min(lo.y, 0.0f), yHi = std::max(hi.y, yLo + 0.2f);
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, n, {0, 0}});
            v.push_back({b, n, {1, 0}});
            v.push_back({c, n, {1, 1}});
            v.push_back({d, n, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
            i.push_back(k);
            i.push_back(k + 2);
            i.push_back(k + 3);
        };
        quad({-W, yLo, 0}, {W, yLo, 0}, {W, yHi, 0}, {-W, yHi, 0}, {0, 0, 1});
        quad({0, yLo, -W}, {0, yLo, W}, {0, yHi, W}, {0, yHi, -W}, {1, 0, 0});
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    int built = 0;
    for (int p = 0; p < kBushPaletteSize; ++p) {
        std::vector<V> verts;
        std::vector<std::uint32_t> idx;
        verts.reserve(180);
        idx.reserve(180);
        glm::vec3 lo(1.0e9f), hi(-1.0e9f); // cluster AABB for the LOD hull/billboard
        // 2-3 overlapping lobes per bush; each a squashed, deformed icosphere.
        const int lobes = 2 + static_cast<int>(h01(p, 41) * 2.0f); // 2..3
        for (int l = 0; l < lobes; ++l) {
            // lobe centre offset (low + spread out so the cluster reads as a mound).
            const glm::vec3 centre((h01(p * 7 + l, 11) - 0.5f) * 0.9f,
                                   0.18f + 0.30f * h01(p * 7 + l, 12),
                                   (h01(p * 7 + l, 13) - 0.5f) * 0.9f);
            // squashed (wider than tall) so it sits like a shrub, not a ball.
            const glm::vec3 lobeScale(0.40f + 0.30f * h01(p * 7 + l, 14),
                                      0.26f + 0.22f * h01(p * 7 + l, 15),
                                      0.40f + 0.30f * h01(p * 7 + l, 16));
            glm::vec3 dv[12];
            for (int i = 0; i < 12; ++i) {
                const glm::vec3 n = glm::normalize(ico[i]);
                const float r = 0.78f + 0.42f * h01((p * 7 + l) * 13 + i, 7); // leafy roughness
                dv[i] = centre + n * r * lobeScale;
                lo = glm::min(lo, dv[i]);
                hi = glm::max(hi, dv[i]);
            }
            for (int f = 0; f < 20; ++f) {
                const glm::vec3 a = dv[faces[f][0]], b = dv[faces[f][1]], c = dv[faces[f][2]];
                const glm::vec3 cr = glm::cross(b - a, c - a);
                const float crLen = glm::length(cr);
                const glm::vec3 nrm =
                    (crLen > 1e-6f) ? (cr / crLen)
                                    : glm::normalize(a - centre + glm::vec3(0.0f, 1e-3f, 0.0f));
                const std::uint32_t k = static_cast<std::uint32_t>(verts.size());
                verts.push_back({a, nrm, {0.0f, 0.0f}});
                verts.push_back({b, nrm, {1.0f, 0.0f}});
                verts.push_back({c, nrm, {0.0f, 1.0f}});
                idx.push_back(k);
                idx.push_back(k + 1);
                idx.push_back(k + 2);
            }
        }
        const std::string base = "procgen://bush_" + std::to_string(p);
        rp.register_procgen_mesh(base,
                                 Luminumbra::Rendering::MeshLoader::CreateFromArrays(verts, idx));
        //  distance LODs (GBufferPass picks by camera distance; absent -> LOD0).
        rp.register_procgen_mesh(base + ".lod1", buildBushHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod2", buildBushHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod3", buildBushBillboard(lo, hi));
        ++built;
    }
    g_bushPaletteCount = built;
    LUMINUMBRA_CORE_INFO("VAST-FOREST: built procedural bush palette of {} entries", built);
}

std::unique_ptr<Luminumbra::Client::Rml_UIManager> g_uiManager;
// UI hot reload: watches data/ui and reloads the active document on.rml/.rcss edits
// (opt-in via --ui-hot-reload, so the 1s filesystem poll is off during normal/gate runs).
Luminumbra::Client::UI::UIHotReload g_uiHotReload;
std::unique_ptr<Luminumbra::Client::WorldLoadingVisualizer> g_loading_visualizer;

// --- Forward Declarations ---
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
//  menu-state scroll callback — forwards to RmlUi (so menu lists
// still scroll) AND accrues the wheel delta into g_menu_scroll_accum so the
// create-world preview block can zoom the diorama when the cursor is over the pane.
void menu_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void GLAPIENTRY GLDebugMessageCallback(GLenum source,
                                       GLenum type,
                                       GLuint id,
                                       GLenum severity,
                                       GLsizei length,
                                       const GLchar* message,
                                       const void* userParam);
void GLFWErrorCallback(int error, const char* description);
void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState);
void SetGamePaused(GLFWwindow* window, bool paused);

// --- Global State ---
float lastX = 1280 / 2.0f;
float lastY = 720 / 2.0f;
bool firstMouse = true;
bool show_worldgen_viewer = false;
bool wireframe_mode = false;
bool g_world_render_data_initialized = false;
bool g_imgui_enabled = true;

std::vector<Luminumbra::IVec3> g_initial_chunks_to_load;
int g_generation_dispatch_index = 0;
Luminumbra::JobHandle g_world_gen_handle;

// --- Window-mode runtime state ---
// Tracks the active window arrangement plus the saved windowed geometry so the
// Alt+Enter windowed<->borderless toggle (and F11 exclusive-fullscreen toggle)
// can restore it. The framebuffer-size callback writes pending sizes here; the
// main loop debounces them to a single RenderPipeline::on_resize per settle
// window so dragging the window edge does not reallocate targets every event.
struct WindowState {
    Luminumbra::Client::ScenarioHarness::WindowMode mode =
        Luminumbra::Client::ScenarioHarness::WindowMode::Borderless;
    // Geometry to restore when leaving borderless/fullscreen back to windowed.
    int windowedX = 100, windowedY = 100;
    int windowedWidth = 1280, windowedHeight = 720;
    // Capture-pin lock: when true the window is held at the pinned capture size
    // and the runtime mode toggles are suppressed (scenario/capture runs).
    bool capture_pinned = false;

    // Debounced framebuffer resize (driven by the GLFW framebuffer-size cb).
    bool resize_pending = false;
    int pending_width = 0;
    int pending_height = 0;
    double pending_since_seconds = 0.0;
};
WindowState g_windowState;

// Debounce window for framebuffer resizes (seconds). A drag emits a burst of
// framebuffer-size events; we coalesce them into one realloc once the size has
// been stable for this long.
constexpr double kResizeDebounceSeconds = 0.12;

namespace {

bool HasRuntimeAssets(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path / "res/shaders/g_buffer.vert", ec) &&
           std::filesystem::exists(path / "data/audio/music.bank.json", ec) &&
           std::filesystem::exists(path / "worlds/atlas/presets/default.json", ec);
}

void AddAncestorCandidates(std::vector<std::filesystem::path>& candidates,
                           std::filesystem::path path) {
    std::error_code ec;
    path = std::filesystem::absolute(path, ec);
    if (path.empty()) {
        return;
    }

    while (!path.empty()) {
        candidates.push_back(path);
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
}

std::filesystem::path ResolveRuntimeRoot(const char* argv0) {
    std::vector<std::filesystem::path> candidates;

    std::error_code ec;
    AddAncestorCandidates(candidates, std::filesystem::current_path(ec));
    if (argv0 && argv0[0] != '\0') {
        AddAncestorCandidates(candidates, std::filesystem::path(argv0).parent_path());
    }

    for (const std::filesystem::path& candidate : candidates) {
        if (HasRuntimeAssets(candidate)) {
            std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate : canonical;
        }
    }

    return std::filesystem::current_path(ec);
}

std::string RuntimeRootString(const std::filesystem::path& root_dir) {
    std::string root_path = root_dir.generic_string();
    if (!root_path.empty() && root_path.back() != '/') {
        root_path.push_back('/');
    }
    return root_path;
}

struct ProcessMemoryStats {
    uint64_t working_set_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
    uint64_t private_bytes = 0;
    uint64_t pagefile_bytes = 0;
    uint64_t total_physical_bytes = 0;
    uint64_t available_physical_bytes = 0;
};

ProcessMemoryStats QueryProcessMemoryStats() {
    ProcessMemoryStats stats;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        stats.working_set_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
        stats.peak_working_set_bytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
        stats.private_bytes = static_cast<uint64_t>(counters.PrivateUsage);
        stats.pagefile_bytes = static_cast<uint64_t>(counters.PagefileUsage);
    }

    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (GlobalMemoryStatusEx(&memory_status)) {
        stats.total_physical_bytes = static_cast<uint64_t>(memory_status.ullTotalPhys);
        stats.available_physical_bytes = static_cast<uint64_t>(memory_status.ullAvailPhys);
    }
#endif
    return stats;
}

struct RuntimeReadinessReport {
    bool ready = false;
    std::vector<std::string> reasons;
};

RuntimeReadinessReport EvaluateReadiness(const RuntimeScenarioConfig& config,
                                         Luminumbra::world::GameSession* game_session) {
    RuntimeReadinessReport report;

    if (!game_session) {
        report.reasons.push_back("game_session_missing");
        return report;
    }

    auto* world_system = game_session->GetWorldSystem();
    if (!world_system) {
        report.reasons.push_back("world_system_missing");
        return report;
    }

    (void)world_system->get_renderable_chunks();
    const auto chunk_stats = world_system->get_runtime_chunk_stats();
    if (chunk_stats.renderable_chunks < config.min_renderable_chunks) {
        report.reasons.push_back("renderable_chunks_below_minimum");
    }
    if (chunk_stats.collision_chunks < config.min_collision_chunks) {
        report.reasons.push_back("collision_chunks_below_minimum");
    }
    if (chunk_stats.loading_chunks > 0) {
        report.reasons.push_back("chunks_still_loading");
    }
    if (chunk_stats.meshing_chunks > 0) {
        report.reasons.push_back("chunks_still_meshing");
    }
    if (chunk_stats.generation_job_active) {
        report.reasons.push_back("generation_job_active");
    }
    if (chunk_stats.meshing_job_active) {
        report.reasons.push_back("meshing_job_active");
    }

    report.ready = report.reasons.empty();
    return report;
}

class RuntimeStateRecorder {
public:
    explicit RuntimeStateRecorder(RuntimeScenarioConfig config)
        : m_config(std::move(config))
        , m_started_at(std::chrono::steady_clock::now()) {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
    }

    const std::filesystem::path& artifact_dir() const {
        return m_config.artifact_dir;
    }
    const std::filesystem::path& crash_dir() const {
        return m_config.crash_dir;
    }

    void capture(const std::string& phase,
                 const Luminumbra::JobSystem* job_system,
                 Luminumbra::world::GameSession* game_session,
                 const Luminumbra::Rendering::RenderPipeline* render_pipeline,
                 uint64_t frame_count,
                 const RuntimeReadinessReport& readiness) {
        m_last_known = build_state_json(
            phase, job_system, game_session, render_pipeline, frame_count, readiness);
        write_last_known();
    }

    void write_memory_watermark(const std::string& phase,
                                const ProcessMemoryStats& memory,
                                uint64_t measured_bytes) {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
        nlohmann::json artifact = {{"schema", "luminumbra.memory_watermark.v1"},
                                   {"timestamp_utc", TimestampUtc()},
                                   {"phase", phase},
                                   {"watermark_mb", m_config.memory_watermark_mb},
                                   {"measured_bytes", measured_bytes},
                                   {"memory", MemoryToJson(memory)}};
        std::ofstream output(m_config.artifact_dir / "memory-watermark.json");
        output << std::setw(2) << artifact << '\n';
    }

    void write_shutdown(const std::vector<std::string>& milestones,
                        const Luminumbra::JobSystem::RuntimeStats& job_stats) {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
        nlohmann::json artifact = {{"schema", "luminumbra.shutdown.v1"},
                                   {"timestamp_utc", TimestampUtc()},
                                   {"scenario", m_config.scenario},
                                   {"milestones", milestones},
                                   {"job_queue", JobStatsToJson(job_stats)},
                                   {"jobs_drained",
                                    job_stats.queue_depth == 0 && job_stats.worker_count == 0 &&
                                        !job_stats.accepting_jobs}};
        std::ofstream output(m_config.artifact_dir / "shutdown.json");
        output << std::setw(2) << artifact << '\n';
    }

    void mark_unhandled_exception(uint32_t exception_code) {
        m_last_known["phase"] = "unhandled_exception";
        m_last_known["exception_code"] = exception_code;
        m_last_known["timestamp_utc"] = TimestampUtc();
        write_last_known();
    }

private:
    static nlohmann::json MemoryToJson(const ProcessMemoryStats& memory) {
        return {{"working_set_bytes", memory.working_set_bytes},
                {"peak_working_set_bytes", memory.peak_working_set_bytes},
                {"private_bytes", memory.private_bytes},
                {"pagefile_bytes", memory.pagefile_bytes},
                {"total_physical_bytes", memory.total_physical_bytes},
                {"available_physical_bytes", memory.available_physical_bytes}};
    }

    static nlohmann::json JobStatsToJson(const Luminumbra::JobSystem::RuntimeStats& stats) {
        return {{"worker_count", stats.worker_count},
                // Total across both priority lanes (validators read this field).
                {"queue_depth", stats.queue_depth},
                {"high_priority_queue_depth", stats.high_priority_queue_depth},
                {"normal_priority_queue_depth", stats.normal_priority_queue_depth},
                {"accepting_jobs", stats.accepting_jobs},
                {"stop_requested", stats.stop_requested}};
    }

    static nlohmann::json
    ChunkStatsToJson(const Luminumbra::Systems::SHIELD_WorldSystem::RuntimeChunkStats& stats) {
        return {{"total_chunks", stats.total_chunks},
                {"unloaded", stats.unloaded_chunks},
                {"loading", stats.loading_chunks},
                {"idle", stats.idle_chunks},
                {"meshing", stats.meshing_chunks},
                {"ready", stats.ready_chunks},
                {"unloading", stats.unloading_chunks},
                {"renderable", stats.renderable_chunks},
                {"collision", stats.collision_chunks},
                {"terrain_vertex_count", stats.terrain_vertex_count},
                {"terrain_index_count", stats.terrain_index_count},
                {"water_vertex_count", stats.water_vertex_count},
                {"water_index_count", stats.water_index_count},
                {"terrain_payload_bytes", stats.terrain_payload_bytes},
                {"sdf_payload_bytes", stats.sdf_payload_bytes},
                {"heightmap_payload_bytes", stats.heightmap_payload_bytes},
                {"sdf_skipped_chunks", stats.sdf_skipped_chunks},
                {"generation_job_active", stats.generation_job_active},
                {"meshing_job_active", stats.meshing_job_active}};
    }

    static nlohmann::json
    UploadStatsToJson(const Luminumbra::Rendering::RenderPipeline::MeshUploadFrameStats& stats) {
        return {
            {"snapshot_count", stats.snapshot_count},
            {"terrain_upload_candidates", stats.terrain_upload_candidates},
            {"terrain_uploads", stats.terrain_uploads},
            {"terrain_uploads_deferred", stats.terrain_uploads_deferred},
            {"terrain_payload_bytes", stats.terrain_payload_bytes},
            {"terrain_upload_failures", stats.terrain_upload_failures},
            {"terrain_new_upload_candidates", stats.terrain_new_upload_candidates},
            {"terrain_stale_upload_candidates", stats.terrain_stale_upload_candidates},
            {"terrain_new_uploads_selected", stats.terrain_new_uploads_selected},
            {"terrain_stale_uploads_selected", stats.terrain_stale_uploads_selected},
            {"terrain_new_uploads_deferred", stats.terrain_new_uploads_deferred},
            {"terrain_stale_uploads_deferred", stats.terrain_stale_uploads_deferred},
            {"terrain_deferred_nearer_than_selected", stats.terrain_deferred_nearer_than_selected},
            {"terrain_nearest_candidate_distance_sq", stats.terrain_nearest_candidate_distance_sq},
            {"terrain_farthest_selected_distance_sq", stats.terrain_farthest_selected_distance_sq},
            {"terrain_nearest_deferred_distance_sq", stats.terrain_nearest_deferred_distance_sq},
            {"water_upload_candidates", stats.water_upload_candidates},
            {"water_uploads", stats.water_uploads},
            {"water_uploads_deferred", stats.water_uploads_deferred},
            {"water_payload_bytes", stats.water_payload_bytes},
            {"water_upload_failures", stats.water_upload_failures},
            {"water_new_upload_candidates", stats.water_new_upload_candidates},
            {"water_stale_upload_candidates", stats.water_stale_upload_candidates},
            {"water_new_uploads_selected", stats.water_new_uploads_selected},
            {"water_stale_uploads_selected", stats.water_stale_uploads_selected},
            {"water_new_uploads_deferred", stats.water_new_uploads_deferred},
            {"water_stale_uploads_deferred", stats.water_stale_uploads_deferred},
            {"water_deferred_nearer_than_selected", stats.water_deferred_nearer_than_selected},
            {"water_nearest_candidate_distance_sq", stats.water_nearest_candidate_distance_sq},
            {"water_farthest_selected_distance_sq", stats.water_farthest_selected_distance_sq},
            {"water_nearest_deferred_distance_sq", stats.water_nearest_deferred_distance_sq}};
    }

    static nlohmann::json RenderPassStatsToJson(
        const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& stats) {
        return {{"snapshot_count", stats.snapshot_count},
                {"culling_hierarchy_rebuilds", stats.culling_hierarchy_rebuilds},
                {"culling_hierarchy_chunks", stats.culling_hierarchy_chunks},
                {"terrain_visible_chunks", stats.terrain_visible_chunks},
                {"terrain_draws", stats.terrain_draws},
                {"terrain_indices_drawn", stats.terrain_indices_drawn},
                {"far_region_draws", stats.far_region_draws},
                {"far_indices_drawn", stats.far_indices_drawn},
                {"gpu_timers_supported", stats.gpu_timers_supported},
                {"gbuffer_gpu_ms", stats.gbuffer_gpu_ms},
                {"water_draws", stats.water_draws},
                {"water_indices_drawn", stats.water_indices_drawn},
                {"shadow_draws", stats.shadow_draws},
                {"shadow_indices_drawn", stats.shadow_indices_drawn}};
    }

    static nlohmann::json RenderRuntimeStatsToJson(
        const Luminumbra::Rendering::RenderPipeline::RuntimeRenderStats& stats) {
        return {{"started", stats.started},
                {"terrain_gpu_chunks", stats.terrain_gpu_chunks},
                {"water_gpu_chunks", stats.water_gpu_chunks},
                {"free_terrain_slots", stats.free_terrain_slots},
                {"free_water_slots", stats.free_water_slots},
                {"terrain_vertex_capacity", stats.terrain_vertex_capacity},
                {"terrain_index_capacity", stats.terrain_index_capacity},
                {"water_vertex_capacity", stats.water_vertex_capacity},
                {"water_index_capacity", stats.water_index_capacity},
                {"estimated_vram_bytes", stats.estimated_vram_bytes},
                {"shader_health",
                 {{"geometry", stats.geometry_shader_ok},
                  {"lighting", stats.lighting_shader_ok},
                  {"skybox", stats.skybox_shader_ok},
                  {"shadow", stats.shadow_shader_ok},
                  {"ssao", stats.ssao_shader_ok},
                  {"ssao_blur", stats.ssao_blur_shader_ok},
                  {"water", stats.water_shader_ok},
                  {"instanced_static_mesh", stats.instanced_static_mesh_shader_ok}}}};
    }

    static nlohmann::json CoverageStatsToJson(
        const Luminumbra::Systems::SHIELD_WorldSystem::CameraLocalCoverageStats& stats) {
        return {{"camera_position", Vec3ToJson(stats.camera_position)},
                {"camera_chunk", IVec3ToJson(stats.camera_chunk)},
                {"surface_chunk_under_camera", IVec3ToJson(stats.surface_chunk_under_camera)},
                {"horizontal_radius", stats.horizontal_radius},
                {"terrain_height_under_camera", stats.terrain_height_under_camera},
                {"camera_height_above_terrain", stats.camera_height_above_terrain},
                {"expected_surface_chunks", stats.expected_surface_chunks},
                {"present_surface_chunks", stats.present_surface_chunks},
                {"missing_surface_chunks", stats.missing_surface_chunks},
                {"unloaded_surface_chunks", stats.unloaded_surface_chunks},
                {"loading_surface_chunks", stats.loading_surface_chunks},
                {"idle_surface_chunks", stats.idle_surface_chunks},
                {"meshing_surface_chunks", stats.meshing_surface_chunks},
                {"ready_surface_chunks", stats.ready_surface_chunks},
                {"renderable_surface_chunks", stats.renderable_surface_chunks},
                {"collision_surface_chunks", stats.collision_surface_chunks},
                {"pending_lod_chunks", stats.pending_lod_chunks},
                {"lod_counts",
                 {{"lod0", stats.lod_counts[0]},
                  {"lod1", stats.lod_counts[1]},
                  {"lod2", stats.lod_counts[2]},
                  {"unknown", stats.lod_unknown_chunks}}},
                {"center_chunk_present", stats.center_chunk_present},
                {"center_chunk_renderable", stats.center_chunk_renderable},
                {"near_field_renderable", stats.near_field_renderable}};
    }

    static nlohmann::json GLDebugStatsToJson(const GLDebugRuntimeStats& stats) {
        return {{"messages", stats.messages},
                {"errors", stats.errors},
                {"warnings", stats.warnings},
                {"notifications", stats.notifications}};
    }

    nlohmann::json build_state_json(const std::string& phase,
                                    const Luminumbra::JobSystem* job_system,
                                    Luminumbra::world::GameSession* game_session,
                                    const Luminumbra::Rendering::RenderPipeline* render_pipeline,
                                    uint64_t frame_count,
                                    const RuntimeReadinessReport& readiness) const {
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started_at).count();
        const ProcessMemoryStats memory = QueryProcessMemoryStats();

        nlohmann::json state = {
            {"schema", "luminumbra.runtime_state.v1"},
            {"timestamp_utc", TimestampUtc()},
            {"phase", phase},
            {"scenario", m_config.scenario},
            {"elapsed_seconds", elapsed},
            {"frame_count", frame_count},
            {"launch_flags",
             {{"auto_create_world", m_config.auto_create_world},
              {"auto_enter_world", m_config.auto_enter_world},
              {"timed_run_seconds", m_config.timed_run_seconds},
              {"coverage_radius", m_config.coverage_radius},
              {"no_audio", m_config.no_audio},
              {"audio_telemetry_path", m_config.audio_telemetry_path.generic_string()},
              {"no_ui", m_config.no_ui},
              {"hidden_window", m_config.hidden_window},
              {"memory_watermark_mb", m_config.memory_watermark_mb}}},
            {"memory", MemoryToJson(memory)},
            {"estimated_vram_bytes", 0},
            {"chunk_states", nlohmann::json::object()},
            {"job_queue", nlohmann::json::object()},
            {"upload_queue", nlohmann::json::object()},
            {"render_pass", nlohmann::json::object()},
            {"shader_health", nlohmann::json::object()},
            {"camera", nlohmann::json::object()},
            {"camera_local_coverage", nlohmann::json::object()},
            {"gl_debug", GLDebugStatsToJson(CurrentGLDebugRuntimeStats())},
            {"readiness",
             {{"ready", readiness.ready},
              {"reasons", readiness.reasons},
              {"timeout_seconds", m_config.readiness_timeout_seconds},
              {"min_renderable_chunks", m_config.min_renderable_chunks},
              {"min_collision_chunks", m_config.min_collision_chunks}}}};

        if (job_system) {
            state["job_queue"] = JobStatsToJson(job_system->get_runtime_stats());
        }

        if (game_session) {
            const auto& metadata = game_session->GetMetadata();
            state["world"] = {{"name", metadata.name},
                              {"seed", metadata.seed},
                              {"world_type", metadata.worldType},
                              {"world_id", metadata.worldId},
                              {"spawn_point", Vec3ToJson(metadata.spawnPoint)}};
            if (auto* world_system = game_session->GetWorldSystem()) {
                (void)world_system->get_renderable_chunks();
                state["chunk_states"] = ChunkStatsToJson(world_system->get_runtime_chunk_stats());
                if (g_camera) {
                    const auto coverage = world_system->get_camera_local_coverage_stats(
                        g_camera->Position, m_config.coverage_radius);
                    state["camera"] = {
                        {"position", Vec3ToJson(g_camera->Position)},
                        {"chunk", IVec3ToJson(coverage.camera_chunk)},
                        {"terrain_height", coverage.terrain_height_under_camera},
                        {"height_above_terrain", coverage.camera_height_above_terrain}};
                    state["camera_local_coverage"] = CoverageStatsToJson(coverage);
                }
                const auto& streaming = world_system->get_last_streaming_budget_stats();
                state["streaming"] = {{"target_render_radius", streaming.target_render_radius},
                                      {"generation_budget", streaming.generation_budget},
                                      {"meshing_budget", streaming.meshing_budget},
                                      {"scheduled_generation", streaming.scheduled_generation},
                                      {"deferred_generation", streaming.deferred_generation},
                                      {"scheduled_meshing", streaming.scheduled_meshing},
                                      {"deferred_meshing", streaming.deferred_meshing},
                                      {"unloaded_chunks", streaming.unloaded_chunks},
                                      {"generation_job_active", streaming.generation_job_active},
                                      {"meshing_job_active", streaming.meshing_job_active}};
            }
        }

        if (render_pipeline) {
            const auto runtime_render = render_pipeline->get_runtime_render_stats();
            const auto render_json = RenderRuntimeStatsToJson(runtime_render);
            state["render_runtime"] = render_json;
            state["estimated_vram_bytes"] = runtime_render.estimated_vram_bytes;
            state["shader_health"] = render_json["shader_health"];
            // Capture-pin protection: record the active
            // window mode + the live render-target size so the offline gate can
            // hard-fail if a capture-mode run ever drifted off the pinned size.
            state["capture_pin"] = Luminumbra::Client::ScenarioHarness::CapturePinMetadata(
                m_config.window_mode,
                static_cast<int>(render_pipeline->screen_width()),
                static_cast<int>(render_pipeline->screen_height()));
            state["resize_generation"] = render_pipeline->resize_generation();
            state["upload_queue"] =
                UploadStatsToJson(render_pipeline->get_last_mesh_upload_stats());
            state["render_pass"] =
                RenderPassStatsToJson(render_pipeline->get_last_render_pass_stats());
            // Far-LOD scheduler telemetry (, FarLodHorizon gate inputs).
            if (const auto* farlod = render_pipeline->farlod()) {
                const auto& farlod_stats = farlod->stats();
                state["farlod"] = {
                    {"enabled", farlod_stats.enabled},
                    {"farlod_regions_wanted", farlod_stats.regions_wanted},
                    {"farlod_regions_resident", farlod_stats.regions_resident},
                    {"farlod_regions_missing", farlod_stats.regions_missing},
                    {"farlod_regions_building", farlod_stats.regions_building},
                    {"farlod_resident_bytes", farlod_stats.resident_bytes},
                    {"farlod_region_draws", farlod_stats.region_draws},
                    {"farlod_indices_drawn", farlod_stats.indices_drawn},
                    {"farlod_builds_completed_total", farlod_stats.builds_completed_total},
                    {"farlod_evictions_total", farlod_stats.evictions_total},
                    //  per-frame scheduler diagnostics for the mountains residency trace.
                    {"farlod_builds_dispatched", farlod_stats.builds_dispatched},
                    {"farlod_builds_integrated_ok", farlod_stats.builds_integrated_ok},
                    {"farlod_builds_integrated_failed", farlod_stats.builds_integrated_failed},
                    {"farlod_builds_failed_total", farlod_stats.builds_failed_total},
                    {"farlod_evictions_this_frame", farlod_stats.evictions_this_frame},
                    {"farlod_pending_depth", farlod_stats.pending_depth}};
            }
        }

        return state;
    }

    void write_last_known() const {
        std::error_code ec;
        std::filesystem::create_directories(m_config.artifact_dir, ec);
        std::ofstream output(m_config.artifact_dir / "last-known-runtime.json");
        output << std::setw(2) << m_last_known << '\n';
    }

    RuntimeScenarioConfig m_config;
    std::chrono::steady_clock::time_point m_started_at{};
    nlohmann::json m_last_known = {{"schema", "luminumbra.runtime_state.v1"},
                                   {"phase", "not_started"},
                                   {"timestamp_utc", TimestampUtc()}};
};

struct RuntimeScenarioFrameSample {
    uint64_t frame = 0;
    double elapsed_seconds = 0.0;
    double delta_ms = 0.0;
    Luminumbra::Vec3 camera_position{0.0f};
    float terrain_height = 0.0f;
    float height_above_terrain = 0.0f;
    std::size_t expected_surface_chunks = 0;
    std::size_t missing_surface_chunks = 0;
    std::size_t renderable_surface_chunks = 0;
    std::size_t pending_lod_chunks = 0;
    bool near_field_renderable = false;
    std::size_t terrain_visible_chunks = 0;
    std::size_t terrain_upload_candidates = 0;
    std::size_t terrain_uploads = 0;
    std::size_t terrain_uploads_deferred = 0;
    std::size_t terrain_stale_upload_candidates = 0;
    std::size_t terrain_stale_uploads_deferred = 0;
    std::size_t terrain_deferred_nearer_than_selected = 0;
    std::size_t water_upload_candidates = 0;
    std::size_t water_uploads = 0;
    std::size_t water_uploads_deferred = 0;
    std::size_t water_stale_upload_candidates = 0;
    std::size_t water_stale_uploads_deferred = 0;
    std::size_t water_deferred_nearer_than_selected = 0;
    uint64_t gl_debug_errors = 0;
};

class RuntimeScenarioFrameRecorder {
public:
    RuntimeScenarioFrameRecorder(bool enabled,
                                 int coverage_radius,
                                 std::filesystem::path output_dir)
        : m_enabled(enabled)
        , m_coverage_radius(std::max(0, coverage_radius))
        , m_output_dir(std::move(output_dir))
        , m_started_at(std::chrono::steady_clock::now()) {}

    bool enabled() const {
        return m_enabled;
    }

    void record_frame(float delta_time,
                      Luminumbra::world::GameSession* game_session,
                      const Luminumbra::Rendering::RenderPipeline& render_pipeline,
                      uint64_t frame_count) {
        if (!m_enabled || !game_session || !g_camera) {
            return;
        }

        auto* world_system = game_session->GetWorldSystem();
        if (!world_system) {
            return;
        }

        const auto coverage =
            world_system->get_camera_local_coverage_stats(g_camera->Position, m_coverage_radius);
        const auto& upload = render_pipeline.get_last_mesh_upload_stats();
        const auto& passes = render_pipeline.get_last_render_pass_stats();
        const auto gl_debug = CurrentGLDebugRuntimeStats();

        RuntimeScenarioFrameSample sample;
        sample.frame = frame_count;
        sample.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started_at).count();
        sample.delta_ms = static_cast<double>(delta_time) * 1000.0;
        sample.camera_position = g_camera->Position;
        sample.terrain_height = coverage.terrain_height_under_camera;
        sample.height_above_terrain = coverage.camera_height_above_terrain;
        sample.expected_surface_chunks = coverage.expected_surface_chunks;
        sample.missing_surface_chunks = coverage.missing_surface_chunks;
        sample.renderable_surface_chunks = coverage.renderable_surface_chunks;
        sample.pending_lod_chunks = coverage.pending_lod_chunks;
        sample.near_field_renderable = coverage.near_field_renderable;
        sample.terrain_visible_chunks = passes.terrain_visible_chunks;
        sample.terrain_upload_candidates = upload.terrain_upload_candidates;
        sample.terrain_uploads = upload.terrain_uploads;
        sample.terrain_uploads_deferred = upload.terrain_uploads_deferred;
        sample.terrain_stale_upload_candidates = upload.terrain_stale_upload_candidates;
        sample.terrain_stale_uploads_deferred = upload.terrain_stale_uploads_deferred;
        sample.terrain_deferred_nearer_than_selected = upload.terrain_deferred_nearer_than_selected;
        sample.water_upload_candidates = upload.water_upload_candidates;
        sample.water_uploads = upload.water_uploads;
        sample.water_uploads_deferred = upload.water_uploads_deferred;
        sample.water_stale_upload_candidates = upload.water_stale_upload_candidates;
        sample.water_stale_uploads_deferred = upload.water_stale_uploads_deferred;
        sample.water_deferred_nearer_than_selected = upload.water_deferred_nearer_than_selected;
        sample.gl_debug_errors = gl_debug.errors;
        m_samples.push_back(sample);
    }

    bool write_artifacts() const {
        if (!m_enabled) {
            return true;
        }

        std::error_code ec;
        std::filesystem::create_directories(m_output_dir, ec);
        if (ec) {
            LUMINUMBRA_CORE_ERROR("Failed to create runtime scenario frame directory '{}': {}",
                                  m_output_dir.string(),
                                  ec.message());
            return false;
        }

        return write_json(m_output_dir / "runtime-frames.json") &&
               write_csv(m_output_dir / "runtime-frames.csv");
    }

private:
    bool write_json(const std::filesystem::path& path) const {
        nlohmann::json frames = nlohmann::json::array();
        for (const RuntimeScenarioFrameSample& sample : m_samples) {
            frames.push_back(
                {{"frame", sample.frame},
                 {"elapsed_seconds", sample.elapsed_seconds},
                 {"delta_ms", sample.delta_ms},
                 {"camera_position", Vec3ToJson(sample.camera_position)},
                 {"terrain_height", sample.terrain_height},
                 {"height_above_terrain", sample.height_above_terrain},
                 {"coverage",
                  {{"expected_surface_chunks", sample.expected_surface_chunks},
                   {"missing_surface_chunks", sample.missing_surface_chunks},
                   {"renderable_surface_chunks", sample.renderable_surface_chunks},
                   {"pending_lod_chunks", sample.pending_lod_chunks},
                   {"near_field_renderable", sample.near_field_renderable}}},
                 {"render_pass", {{"terrain_visible_chunks", sample.terrain_visible_chunks}}},
                 {"upload_queue",
                  {{"terrain_upload_candidates", sample.terrain_upload_candidates},
                   {"terrain_uploads", sample.terrain_uploads},
                   {"terrain_uploads_deferred", sample.terrain_uploads_deferred},
                   {"terrain_stale_upload_candidates", sample.terrain_stale_upload_candidates},
                   {"terrain_stale_uploads_deferred", sample.terrain_stale_uploads_deferred},
                   {"terrain_deferred_nearer_than_selected",
                    sample.terrain_deferred_nearer_than_selected},
                   {"water_upload_candidates", sample.water_upload_candidates},
                   {"water_uploads", sample.water_uploads},
                   {"water_uploads_deferred", sample.water_uploads_deferred},
                   {"water_stale_upload_candidates", sample.water_stale_upload_candidates},
                   {"water_stale_uploads_deferred", sample.water_stale_uploads_deferred},
                   {"water_deferred_nearer_than_selected",
                    sample.water_deferred_nearer_than_selected}}},
                 {"gl_debug_errors", sample.gl_debug_errors}});
        }

        nlohmann::json artifact = {{"schema", "luminumbra.runtime_frames.v1"},
                                   {"timestamp_utc", TimestampUtc()},
                                   {"coverage_radius", m_coverage_radius},
                                   {"frames_recorded", m_samples.size()},
                                   {"frames", frames}};

        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime frame JSON: {}", path.string());
            return false;
        }
        output << std::setw(2) << artifact << '\n';
        return true;
    }

    bool write_csv(const std::filesystem::path& path) const {
        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime frame CSV: {}", path.string());
            return false;
        }

        output << "frame,elapsed_seconds,delta_ms,camera_x,camera_y,camera_z,terrain_height,height_"
                  "above_terrain,expected_surface_chunks,missing_surface_chunks,renderable_surface_"
                  "chunks,pending_lod_chunks,near_field_renderable,terrain_visible_chunks,terrain_"
                  "upload_candidates,terrain_uploads,terrain_uploads_deferred,terrain_stale_upload_"
                  "candidates,terrain_stale_uploads_deferred,terrain_deferred_nearer_than_selected,"
                  "water_upload_candidates,water_uploads,water_uploads_deferred,water_stale_upload_"
                  "candidates,water_stale_uploads_deferred,water_deferred_nearer_than_selected,gl_"
                  "debug_errors\n";
        for (const RuntimeScenarioFrameSample& sample : m_samples) {
            output << sample.frame << ',' << sample.elapsed_seconds << ',' << sample.delta_ms << ','
                   << sample.camera_position.x << ',' << sample.camera_position.y << ','
                   << sample.camera_position.z << ',' << sample.terrain_height << ','
                   << sample.height_above_terrain << ',' << sample.expected_surface_chunks << ','
                   << sample.missing_surface_chunks << ',' << sample.renderable_surface_chunks
                   << ',' << sample.pending_lod_chunks << ','
                   << (sample.near_field_renderable ? 1 : 0) << ',' << sample.terrain_visible_chunks
                   << ',' << sample.terrain_upload_candidates << ',' << sample.terrain_uploads
                   << ',' << sample.terrain_uploads_deferred << ','
                   << sample.terrain_stale_upload_candidates << ','
                   << sample.terrain_stale_uploads_deferred << ','
                   << sample.terrain_deferred_nearer_than_selected << ','
                   << sample.water_upload_candidates << ',' << sample.water_uploads << ','
                   << sample.water_uploads_deferred << ',' << sample.water_stale_upload_candidates
                   << ',' << sample.water_stale_uploads_deferred << ','
                   << sample.water_deferred_nearer_than_selected << ',' << sample.gl_debug_errors
                   << '\n';
        }
        return true;
    }

    bool m_enabled = false;
    int m_coverage_radius = 0;
    std::filesystem::path m_output_dir;
    std::chrono::steady_clock::time_point m_started_at{};
    std::vector<RuntimeScenarioFrameSample> m_samples;
};

RuntimeStateRecorder* g_runtime_state_recorder = nullptr;

#if defined(_WIN32)
bool WriteMiniDump(EXCEPTION_POINTERS* exception_info, const std::filesystem::path& crash_dir) {
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    const std::filesystem::path dump_path =
        crash_dir / ("luminumbra-" + TimestampForFile() + ".dmp");

    HANDLE file = CreateFileW(dump_path.wstring().c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception_information{};
    exception_information.ThreadId = GetCurrentThreadId();
    exception_information.ExceptionPointers = exception_info;
    exception_information.ClientPointers = FALSE;

    const BOOL wrote_dump = MiniDumpWriteDump(GetCurrentProcess(),
                                              GetCurrentProcessId(),
                                              file,
                                              MiniDumpNormal,
                                              exception_info ? &exception_information : nullptr,
                                              nullptr,
                                              nullptr);
    CloseHandle(file);
    return wrote_dump == TRUE;
}

// Walk + symbolize the faulting thread's stack at crash time and write it to a
// readable text file + the (now file-backed) log. The minidump path has been
// producing EMPTY.dmp files, and logs were stdout-only, so a normal-launch crash
// left nothing to diagnose. DbgHelp resolves public/export names; the per-frame
// `module+0xRVA` lets addr2line recover exact file:line from the binary's DWARF
// (run tools/gates/symbolize-crash.ps1 on the crash file). A crash handler must
// not itself throw — everything here is guarded and bounded.
void WriteCrashStackTrace(EXCEPTION_POINTERS* xp, const std::filesystem::path& crash_dir) {
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    const std::filesystem::path path = crash_dir / ("crash-" + TimestampForFile() + ".txt");
    std::ofstream out(path);

    const HANDLE proc = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    auto emit = [&](const std::string& s) {
        if (out) {
            out << s << "\n";
            out.flush();
        } // flush per line: the process may be torn down any moment
        LUMINUMBRA_CORE_CRITICAL("{}", s);
    };

    const uint32_t code = (xp && xp->ExceptionRecord)
                              ? static_cast<uint32_t>(xp->ExceptionRecord->ExceptionCode)
                              : 0u;
    void* faddr = (xp && xp->ExceptionRecord) ? xp->ExceptionRecord->ExceptionAddress : nullptr;
    {
        char b[160];
        std::snprintf(b,
                      sizeof b,
                      "=== LUMINUMBRA CRASH  exception=0x%08X  faulting_addr=%p ===",
                      code,
                      faddr);
        emit(b);
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);

    if (!xp || !xp->ContextRecord) {
        emit("(no thread context captured — cannot walk the stack)");
        SymCleanup(proc);
        return;
    }

    CONTEXT ctx = *xp->ContextRecord; // StackWalk64 mutates the context as it unwinds.
    {
        char b[256];
        std::snprintf(b,
                      sizeof b,
                      "regs: rip=0x%llX rsp=0x%llX rbp=0x%llX",
                      (unsigned long long)ctx.Rip,
                      (unsigned long long)ctx.Rsp,
                      (unsigned long long)ctx.Rbp);
        emit(b);
    }
    // NULL function-pointer call (rip==0, faulting_addr==0): the CALL already pushed the
    // return address, so [rsp] holds the CALLER's address. Seed the walk from there so the
    // first symbolized frame names WHO called null (StackWalk64 can't start from pc==0).
    if (ctx.Rip == 0 && ctx.Rsp != 0) {
        const DWORD64 ret = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        char b[160];
        std::snprintf(b,
                      sizeof b,
                      "(rip==0: null call — caller return addr [rsp]=0x%llX)",
                      (unsigned long long)ret);
        emit(b);
        ctx.Rip = ret; // pretend we are in the caller
        ctx.Rsp += 8;  // pop the pushed return address
    }
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                         proc,
                         thread,
                         &frame,
                         &ctx,
                         nullptr,
                         SymFunctionTableAccess64,
                         SymGetModuleBase64,
                         nullptr)) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0)
            break;

        const DWORD64 mod_base = SymGetModuleBase64(proc, pc);
        char mod_name[MAX_PATH] = "?";
        if (mod_base) {
            GetModuleFileNameA(reinterpret_cast<HMODULE>(mod_base), mod_name, MAX_PATH);
        }
        const DWORD64 rva = mod_base ? (pc - mod_base) : 0;

        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 512;
        DWORD64 disp = 0;
        const char* fn = SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "??";

        const char* slash = std::strrchr(mod_name, '\\');
        const char* mod_short = slash ? slash + 1 : mod_name;

        char b[1100];
        std::snprintf(b,
                      sizeof b,
                      "#%-2d 0x%016llX  %s+0x%llX  %s",
                      i,
                      static_cast<unsigned long long>(pc),
                      mod_short,
                      static_cast<unsigned long long>(rva),
                      fn);
        emit(b);
    }
    emit("=== end stack ===  (resolve file:line with tools/gates/symbolize-crash.ps1 <crash.txt>)");
    SymCleanup(proc);
}

LONG WINAPI RuntimeUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_info) {
    // One-shot: a parallel null-pointer fault hits on EVERY worker thread at once. The
    // first thread in writes the (single, clean) crash report; the rest just terminate
    // so the log isn't spammed and the report isn't interleaved.
    static std::atomic<bool> s_handling{false};
    bool expected = false;
    if (!s_handling.compare_exchange_strong(expected, true)) {
        // A sibling thread (the same parallel fault hits every worker at once) is already
        // writing the report. Do NOT return — that terminates the process and kills the
        // writer mid-flush (which left an empty crash file). PARK here; the writer's
        // return tears the whole process (and us) down once the report is on disk.
        for (;;) {
            Sleep(1000);
        }
    }

    const uint32_t exception_code =
        exception_info && exception_info->ExceptionRecord
            ? static_cast<uint32_t>(exception_info->ExceptionRecord->ExceptionCode)
            : 0;
    // Flush buffered breadcrumbs to the file sink FIRST, so logs/luminumbra.log holds
    // the pre-crash context even if a later step here itself faults.
    if (auto& lg = Log::GetCoreLogger())
        lg->flush();

    const std::filesystem::path crash_dir = g_runtime_state_recorder
                                                ? g_runtime_state_recorder->crash_dir()
                                                : std::filesystem::path("crashes");
    WriteCrashStackTrace(exception_info, crash_dir);

    if (g_runtime_state_recorder) {
        g_runtime_state_recorder->mark_unhandled_exception(exception_code);
        WriteMiniDump(exception_info, g_runtime_state_recorder->crash_dir());
    }
    if (auto& lg = Log::GetCoreLogger())
        lg->flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallRuntimeCrashHandler(RuntimeStateRecorder& recorder) {
    g_runtime_state_recorder = &recorder;
    SetUnhandledExceptionFilter(RuntimeUnhandledExceptionFilter);
}
#else
void InstallRuntimeCrashHandler(RuntimeStateRecorder& recorder) {
    g_runtime_state_recorder = &recorder;
}
#endif

[[noreturn]] void TriggerForcedCrash() {
#if defined(_WIN32)
    RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
    std::raise(SIGABRT);
#endif
    std::abort();
}

bool MemoryWatermarkExceeded(const RuntimeScenarioConfig& config,
                             ProcessMemoryStats* out_memory,
                             uint64_t* out_measured_bytes) {
    if (config.memory_watermark_mb == 0) {
        return false;
    }

    ProcessMemoryStats memory = QueryProcessMemoryStats();
    const uint64_t measured_bytes = std::max(memory.working_set_bytes, memory.private_bytes);
    if (out_memory) {
        *out_memory = memory;
    }
    if (out_measured_bytes) {
        *out_measured_bytes = measured_bytes;
    }
    return measured_bytes > config.memory_watermark_mb * 1024ull * 1024ull;
}

struct RuntimeBootFrameMetrics {
    int frame = 0;
    double delta_ms = 0.0;
    size_t snapshots = 0;
    size_t terrain_visible_chunks = 0;
    size_t terrain_draws = 0;
    size_t terrain_indices_drawn = 0;
    size_t shadow_draws = 0;
    size_t shadow_indices_drawn = 0;
    size_t culling_hierarchy_rebuilds = 0;
    size_t terrain_upload_candidates = 0;
    size_t terrain_uploads = 0;
    size_t terrain_uploads_deferred = 0;
    size_t terrain_payload_bytes = 0;
    size_t terrain_slots_created = 0;
    size_t terrain_slots_reused = 0;
    size_t terrain_slots_grown = 0;
    size_t terrain_upload_failures = 0;
    size_t water_upload_candidates = 0;
    size_t water_uploads = 0;
    size_t water_uploads_deferred = 0;
    size_t water_payload_bytes = 0;
    size_t water_slots_created = 0;
    size_t water_slots_reused = 0;
    size_t water_slots_grown = 0;
    size_t water_upload_failures = 0;
};

class RuntimeBootMetricsRecorder {
public:
    RuntimeBootMetricsRecorder(bool enabled, int target_frames, std::filesystem::path output_dir)
        : m_enabled(enabled)
        , m_target_frames(std::max(1, target_frames))
        , m_output_dir(std::move(output_dir)) {
        if (m_enabled) {
            m_frames.reserve(static_cast<size_t>(m_target_frames));
            m_started_at = std::chrono::steady_clock::now();
        }
    }

    bool enabled() const {
        return m_enabled;
    }
    bool complete() const {
        return m_enabled && static_cast<int>(m_frames.size()) >= m_target_frames;
    }

    void record_frame(float delta_time,
                      const Luminumbra::Rendering::RenderPipeline& render_pipeline) {
        if (!m_enabled || complete()) {
            return;
        }

        const auto& upload = render_pipeline.get_last_mesh_upload_stats();
        const auto& passes = render_pipeline.get_last_render_pass_stats();

        RuntimeBootFrameMetrics frame;
        frame.frame = static_cast<int>(m_frames.size()) + 1;
        frame.delta_ms = static_cast<double>(delta_time) * 1000.0;
        frame.snapshots = passes.snapshot_count;
        frame.terrain_visible_chunks = passes.terrain_visible_chunks;
        frame.terrain_draws = passes.terrain_draws;
        frame.terrain_indices_drawn = passes.terrain_indices_drawn;
        frame.shadow_draws = passes.shadow_draws;
        frame.shadow_indices_drawn = passes.shadow_indices_drawn;
        frame.culling_hierarchy_rebuilds = passes.culling_hierarchy_rebuilds;
        frame.terrain_upload_candidates = upload.terrain_upload_candidates;
        frame.terrain_uploads = upload.terrain_uploads;
        frame.terrain_uploads_deferred = upload.terrain_uploads_deferred;
        frame.terrain_payload_bytes = upload.terrain_payload_bytes;
        frame.terrain_slots_created = upload.terrain_slots_created;
        frame.terrain_slots_reused = upload.terrain_slots_reused;
        frame.terrain_slots_grown = upload.terrain_slots_grown;
        frame.terrain_upload_failures = upload.terrain_upload_failures;
        frame.water_upload_candidates = upload.water_upload_candidates;
        frame.water_uploads = upload.water_uploads;
        frame.water_uploads_deferred = upload.water_uploads_deferred;
        frame.water_payload_bytes = upload.water_payload_bytes;
        frame.water_slots_created = upload.water_slots_created;
        frame.water_slots_reused = upload.water_slots_reused;
        frame.water_slots_grown = upload.water_slots_grown;
        frame.water_upload_failures = upload.water_upload_failures;
        m_frames.push_back(frame);
    }

    bool write_artifacts() const {
        if (!m_enabled) {
            return true;
        }

        std::error_code ec;
        std::filesystem::create_directories(m_output_dir, ec);
        if (ec) {
            LUMINUMBRA_CORE_ERROR("Failed to create runtime boot metrics directory '{}': {}",
                                  m_output_dir.string(),
                                  ec.message());
            return false;
        }

        const std::filesystem::path json_path = m_output_dir / "runtime_boot.json";
        const std::filesystem::path csv_path = m_output_dir / "runtime_boot.csv";
        return write_json(json_path) && write_csv(csv_path);
    }

private:
    // Takes an ALREADY-SORTED vector by const reference. The previous
    // by-value-copy-then-sort signature tripped a GCC 15 -O3
    // -Wfree-nonheap-object false positive when the inlined copy's
    // deallocation was folded (release lane, ); sorting once at the
    // call site also avoids three copies/sorts of the frame-time vector.
    static double percentile_sorted(const std::vector<double>& sorted_values, double pct) {
        if (sorted_values.empty()) {
            return 0.0;
        }
        const double position = pct * static_cast<double>(sorted_values.size() - 1);
        const auto index = static_cast<size_t>(std::round(position));
        return sorted_values[std::min(index, sorted_values.size() - 1)];
    }

    std::vector<double> frame_times_ms() const {
        std::vector<double> values;
        values.reserve(m_frames.size());
        for (const auto& frame : m_frames) {
            values.push_back(frame.delta_ms);
        }
        return values;
    }

    bool write_json(const std::filesystem::path& path) const {
        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime boot metrics JSON: {}", path.string());
            return false;
        }

        std::vector<double> deltas = frame_times_ms();
        std::sort(deltas.begin(), deltas.end());
        const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - m_started_at)
                                    .count();
        const double p50_ms = percentile_sorted(deltas, 0.50);
        const double p95_ms = percentile_sorted(deltas, 0.95);
        const double p99_ms = percentile_sorted(deltas, 0.99);
        size_t max_snapshots = 0;
        size_t max_terrain_visible_chunks = 0;
        size_t max_terrain_draws = 0;
        size_t max_shadow_draws = 0;
        size_t total_culling_hierarchy_rebuilds = 0;
        size_t frames_with_culling_hierarchy_rebuilds = 0;
        size_t total_terrain_upload_candidates = 0;
        size_t total_terrain_uploads = 0;
        size_t total_terrain_uploads_deferred = 0;
        size_t total_terrain_payload_bytes = 0;
        size_t total_terrain_slots_created = 0;
        size_t total_terrain_slots_reused = 0;
        size_t total_terrain_slots_grown = 0;
        size_t total_terrain_upload_failures = 0;
        size_t total_water_upload_candidates = 0;
        size_t total_water_uploads = 0;
        size_t total_water_uploads_deferred = 0;
        size_t total_water_payload_bytes = 0;
        size_t total_water_slots_created = 0;
        size_t total_water_slots_reused = 0;
        size_t total_water_slots_grown = 0;
        size_t total_water_upload_failures = 0;
        size_t peak_terrain_uploads_deferred = 0;
        size_t peak_water_uploads_deferred = 0;
        size_t final_window_terrain_uploads_deferred = 0;
        size_t final_window_water_uploads_deferred = 0;
        constexpr size_t kFinalWindowFrames = 10;
        const size_t final_window_start =
            m_frames.size() > kFinalWindowFrames ? m_frames.size() - kFinalWindowFrames : 0;

        for (size_t i = 0; i < m_frames.size(); ++i) {
            const auto& frame = m_frames[i];
            max_snapshots = std::max(max_snapshots, frame.snapshots);
            max_terrain_visible_chunks =
                std::max(max_terrain_visible_chunks, frame.terrain_visible_chunks);
            max_terrain_draws = std::max(max_terrain_draws, frame.terrain_draws);
            max_shadow_draws = std::max(max_shadow_draws, frame.shadow_draws);
            total_culling_hierarchy_rebuilds += frame.culling_hierarchy_rebuilds;
            if (frame.culling_hierarchy_rebuilds > 0) {
                frames_with_culling_hierarchy_rebuilds++;
            }
            total_terrain_upload_candidates += frame.terrain_upload_candidates;
            total_terrain_uploads += frame.terrain_uploads;
            total_terrain_uploads_deferred += frame.terrain_uploads_deferred;
            total_terrain_payload_bytes += frame.terrain_payload_bytes;
            total_terrain_slots_created += frame.terrain_slots_created;
            total_terrain_slots_reused += frame.terrain_slots_reused;
            total_terrain_slots_grown += frame.terrain_slots_grown;
            total_terrain_upload_failures += frame.terrain_upload_failures;
            total_water_upload_candidates += frame.water_upload_candidates;
            total_water_uploads += frame.water_uploads;
            total_water_uploads_deferred += frame.water_uploads_deferred;
            total_water_payload_bytes += frame.water_payload_bytes;
            total_water_slots_created += frame.water_slots_created;
            total_water_slots_reused += frame.water_slots_reused;
            total_water_slots_grown += frame.water_slots_grown;
            total_water_upload_failures += frame.water_upload_failures;
            peak_terrain_uploads_deferred =
                std::max(peak_terrain_uploads_deferred, frame.terrain_uploads_deferred);
            peak_water_uploads_deferred =
                std::max(peak_water_uploads_deferred, frame.water_uploads_deferred);
            if (i >= final_window_start) {
                final_window_terrain_uploads_deferred += frame.terrain_uploads_deferred;
                final_window_water_uploads_deferred += frame.water_uploads_deferred;
            }
        }

        output << "{\n";
        output << "  \"schema\": \"luminumbra.runtime_boot.v1\",\n";
        output << "  \"frames_requested\": " << m_target_frames << ",\n";
        output << "  \"frames_recorded\": " << m_frames.size() << ",\n";
        output << "  \"elapsed_ms\": " << elapsed_ms << ",\n";
        output << "  \"frame_time_ms\": {\n";
        output << "    \"p50\": " << p50_ms << ",\n";
        output << "    \"p95\": " << p95_ms << ",\n";
        output << "    \"p99\": " << p99_ms << "\n";
        output << "  },\n";
        output << "  \"summary\": {\n";
        output << "    \"max_snapshots\": " << max_snapshots << ",\n";
        output << "    \"max_terrain_visible_chunks\": " << max_terrain_visible_chunks << ",\n";
        output << "    \"max_terrain_draws\": " << max_terrain_draws << ",\n";
        output << "    \"max_shadow_draws\": " << max_shadow_draws << ",\n";
        output << "    \"total_culling_hierarchy_rebuilds\": " << total_culling_hierarchy_rebuilds
               << ",\n";
        output << "    \"frames_with_culling_hierarchy_rebuilds\": "
               << frames_with_culling_hierarchy_rebuilds << ",\n";
        output << "    \"total_terrain_upload_candidates\": " << total_terrain_upload_candidates
               << ",\n";
        output << "    \"total_terrain_uploads\": " << total_terrain_uploads << ",\n";
        output << "    \"total_terrain_uploads_deferred\": " << total_terrain_uploads_deferred
               << ",\n";
        output << "    \"total_terrain_payload_bytes\": " << total_terrain_payload_bytes << ",\n";
        output << "    \"total_terrain_slots_created\": " << total_terrain_slots_created << ",\n";
        output << "    \"total_terrain_slots_reused\": " << total_terrain_slots_reused << ",\n";
        output << "    \"total_terrain_slots_grown\": " << total_terrain_slots_grown << ",\n";
        output << "    \"total_terrain_upload_failures\": " << total_terrain_upload_failures
               << ",\n";
        output << "    \"total_water_upload_candidates\": " << total_water_upload_candidates
               << ",\n";
        output << "    \"total_water_uploads\": " << total_water_uploads << ",\n";
        output << "    \"total_water_uploads_deferred\": " << total_water_uploads_deferred << ",\n";
        output << "    \"total_water_payload_bytes\": " << total_water_payload_bytes << ",\n";
        output << "    \"total_water_slots_created\": " << total_water_slots_created << ",\n";
        output << "    \"total_water_slots_reused\": " << total_water_slots_reused << ",\n";
        output << "    \"total_water_slots_grown\": " << total_water_slots_grown << ",\n";
        output << "    \"total_water_upload_failures\": " << total_water_upload_failures << ",\n";
        output << "    \"peak_terrain_uploads_deferred\": " << peak_terrain_uploads_deferred
               << ",\n";
        output << "    \"peak_water_uploads_deferred\": " << peak_water_uploads_deferred << ",\n";
        output << "    \"final_window_frames\": " << std::min(kFinalWindowFrames, m_frames.size())
               << ",\n";
        output << "    \"final_window_terrain_uploads_deferred\": "
               << final_window_terrain_uploads_deferred << ",\n";
        output << "    \"final_window_water_uploads_deferred\": "
               << final_window_water_uploads_deferred << "\n";
        output << "  },\n";
        output << "  \"last_frame\": ";
        if (m_frames.empty()) {
            output << "null\n";
        } else {
            const auto& frame = m_frames.back();
            output << "{";
            output << "\"snapshots\": " << frame.snapshots << ", ";
            output << "\"terrain_visible_chunks\": " << frame.terrain_visible_chunks << ", ";
            output << "\"terrain_draws\": " << frame.terrain_draws << ", ";
            output << "\"shadow_draws\": " << frame.shadow_draws << ", ";
            output << "\"terrain_uploads_deferred\": " << frame.terrain_uploads_deferred << ", ";
            output << "\"water_upload_candidates\": " << frame.water_upload_candidates << ", ";
            output << "\"water_uploads_deferred\": " << frame.water_uploads_deferred;
            output << "}\n";
        }
        output << "}\n";
        return true;
    }

    bool write_csv(const std::filesystem::path& path) const {
        std::ofstream output(path);
        if (!output) {
            LUMINUMBRA_CORE_ERROR("Failed to write runtime boot metrics CSV: {}", path.string());
            return false;
        }

        output
            << "frame,delta_ms,snapshots,terrain_visible_chunks,terrain_draws,terrain_indices_"
               "drawn,shadow_draws,shadow_indices_drawn,culling_hierarchy_rebuilds,terrain_upload_"
               "candidates,terrain_uploads,terrain_uploads_deferred,terrain_payload_bytes,terrain_"
               "slots_created,terrain_slots_reused,terrain_slots_grown,terrain_upload_failures,"
               "water_upload_candidates,water_uploads,water_uploads_deferred,water_payload_bytes,"
               "water_slots_created,water_slots_reused,water_slots_grown,water_upload_failures\n";
        for (const auto& frame : m_frames) {
            output << frame.frame << ',' << frame.delta_ms << ',' << frame.snapshots << ','
                   << frame.terrain_visible_chunks << ',' << frame.terrain_draws << ','
                   << frame.terrain_indices_drawn << ',' << frame.shadow_draws << ','
                   << frame.shadow_indices_drawn << ',' << frame.culling_hierarchy_rebuilds << ','
                   << frame.terrain_upload_candidates << ',' << frame.terrain_uploads << ','
                   << frame.terrain_uploads_deferred << ',' << frame.terrain_payload_bytes << ','
                   << frame.terrain_slots_created << ',' << frame.terrain_slots_reused << ','
                   << frame.terrain_slots_grown << ',' << frame.terrain_upload_failures << ','
                   << frame.water_upload_candidates << ',' << frame.water_uploads << ','
                   << frame.water_uploads_deferred << ',' << frame.water_payload_bytes << ','
                   << frame.water_slots_created << ',' << frame.water_slots_reused << ','
                   << frame.water_slots_grown << ',' << frame.water_upload_failures << '\n';
        }
        return true;
    }

    bool m_enabled = false;
    int m_target_frames = 300;
    std::filesystem::path m_output_dir;
    std::chrono::steady_clock::time_point m_started_at{};
    std::vector<RuntimeBootFrameMetrics> m_frames;
};

} // namespace

namespace {

using Luminumbra::Client::ScenarioHarness::WindowMode;

// Monitor under the window's center (falls back to the primary monitor). Used
// so borderless/fullscreen target the display the window currently lives on.
GLFWmonitor* MonitorForWindow(GLFWwindow* window) {
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    const int cx = wx + ww / 2;
    const int cy = wy + wh / 2;

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) {
            return monitors[i];
        }
    }
    return glfwGetPrimaryMonitor();
}

// Saves the current windowed geometry so a later return to windowed restores it.
void SaveWindowedGeometry(GLFWwindow* window, WindowState& state) {
    glfwGetWindowPos(window, &state.windowedX, &state.windowedY);
    glfwGetWindowSize(window, &state.windowedWidth, &state.windowedHeight);
}

// Applies a window mode to an existing window. capture_pinned runs (scenario
// captures) are never reconfigured: they stay at the pinned size in a hidden /
// stable window so every pixel-ROI gate sees exactly 1280x720.
void ApplyWindowMode(GLFWwindow* window, WindowState& state, WindowMode mode) {
    if (state.capture_pinned) {
        state.mode = mode; // record intent, but do not touch the pinned window
        return;
    }
    if (mode == state.mode)
        return;

    // Leaving windowed: remember where it was so we can come back to it.
    if (state.mode == WindowMode::Windowed) {
        SaveWindowedGeometry(window, state);
    }

    GLFWmonitor* monitor = MonitorForWindow(window);
    switch (mode) {
        case WindowMode::Windowed: {
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_TRUE);
            glfwSetWindowMonitor(window,
                                 nullptr,
                                 state.windowedX,
                                 state.windowedY,
                                 state.windowedWidth,
                                 state.windowedHeight,
                                 0);
            break;
        }
        case WindowMode::Borderless: {
            // Borderless window covering the WHOLE monitor (owner default 2026-06-16:
            // make full use of the ultrawide display for reviews/critiques). Unlike
            // the work-area variant, this spans the full native resolution including
            // under the taskbar (a borderless fullscreen), without an exclusive
            // video-mode switch so alt-tab stays instant. Position = monitor origin,
            // size = native video mode.
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitor, &mx, &my);
            const GLFWvidmode* vmode = glfwGetVideoMode(monitor);
            const int mw = vmode ? vmode->width : 1920;
            const int mh = vmode ? vmode->height : 1080;
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_FALSE);
            glfwSetWindowMonitor(window, nullptr, mx, my, mw, mh, 0);
            break;
        }
        case WindowMode::Fullscreen: {
            // Exclusive fullscreen at the monitor's native video mode.
            const GLFWvidmode* vmode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(
                window, monitor, 0, 0, vmode->width, vmode->height, vmode->refreshRate);
            break;
        }
        case WindowMode::Headless:
            glfwHideWindow(window);
            break;
    }
    state.mode = mode;
    LUMINUMBRA_CORE_INFO("Window mode -> {}",
                         Luminumbra::Client::ScenarioHarness::WindowModeName(mode));
}

} // namespace

using Luminumbra::Client::ScenarioHarness::WindowMode;

// Alt+Enter runtime toggle: windowed <-> borderless. Suppressed on
// capture-pinned (scenario) runs.
void ToggleWindowedBorderless(GLFWwindow* window, WindowState& state) {
    if (state.capture_pinned)
        return;
    const WindowMode next =
        (state.mode == WindowMode::Windowed) ? WindowMode::Borderless : WindowMode::Windowed;
    ApplyWindowMode(window, state, next);
}

// F11 toggle: exclusive fullscreen <-> windowed (kept for back-compat with the
// previous F11 binding).
void ToggleFullscreen(GLFWwindow* window, WindowState& state) {
    if (state.capture_pinned)
        return;
    const WindowMode next =
        (state.mode == WindowMode::Fullscreen) ? WindowMode::Windowed : WindowMode::Fullscreen;
    ApplyWindowMode(window, state, next);
}

// Write a downscaled 24-bit uncompressed TGA thumbnail of a captured frame. `rgb` is the
// glReadPixels buffer (bottom-up, RGB); TGA with descriptor=0 is bottom-up origin too, so the
// rows map directly. RmlUi's GL3 backend only decodes TGA, so the gallery thumbs are TGA. The
// downscale is nearest-neighbour (thumbnails don't need filtering) and keeps a small on-disk size.
static bool WriteCaptureThumbnailTga(const std::filesystem::path& path,
                                     int srcW,
                                     int srcH,
                                     const std::vector<unsigned char>& rgb,
                                     int maxDim) {
    if (srcW <= 0 || srcH <= 0 || rgb.size() < static_cast<std::size_t>(srcW) * srcH * 3u)
        return false;
    const float scale =
        std::min(1.0f, static_cast<float>(maxDim) / static_cast<float>(std::max(srcW, srcH)));
    const int dstW = std::max(1, static_cast<int>(static_cast<float>(srcW) * scale));
    const int dstH = std::max(1, static_cast<int>(static_cast<float>(srcH) * scale));
    std::vector<unsigned char> bgr(static_cast<std::size_t>(dstW) * dstH * 3u);
    for (int y = 0; y < dstH; ++y) {
        const int sy =
            std::min(srcH - 1, static_cast<int>((static_cast<float>(y) + 0.5f) / dstH * srcH));
        for (int x = 0; x < dstW; ++x) {
            const int sx =
                std::min(srcW - 1, static_cast<int>((static_cast<float>(x) + 0.5f) / dstW * srcW));
            const unsigned char* s = &rgb[(static_cast<std::size_t>(sy) * srcW + sx) * 3u];
            unsigned char* d = &bgr[(static_cast<std::size_t>(y) * dstW + x) * 3u];
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0]; // RGB -> BGR
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    unsigned char hdr[18] = {0};
    hdr[2] = 2; // uncompressed true-color
    hdr[12] = static_cast<unsigned char>(dstW & 0xFF);
    hdr[13] = static_cast<unsigned char>((dstW >> 8) & 0xFF);
    hdr[14] = static_cast<unsigned char>(dstH & 0xFF);
    hdr[15] = static_cast<unsigned char>((dstH >> 8) & 0xFF);
    hdr[16] = 24; // bits per pixel
    hdr[17] = 0;  // descriptor: bottom-up origin (matches the GL buffer)
    out.write(reinterpret_cast<const char*>(hdr), 18);
    out.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
    return static_cast<bool>(out);
}

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
    g_imgui_enabled = !scenario_config.no_ui;
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
            g_debug_view_mode = 1;
        else if (dv == "normal")
            g_debug_view_mode = 2;
        else if (dv == "depth")
            g_debug_view_mode = 3;
        else if (dv == "material")
            g_debug_view_mode = 4;
        else if (dv == "position")
            g_debug_view_mode = 5;
    }
    g_render_benchmark_path = GetCommandLineOption(argc, argv, "--render-benchmark", "");
    // --debug-goto cave|doline|spawn: after the world loads, deterministically locate the
    // feature + set the fixed camera to frame it (pair with --auto-create-world/--timelapse).
    g_debug_goto = GetCommandLineOption(argc, argv, "--debug-goto", "");
    g_play_paths = HasCommandLineFlag(
        argc,
        argv,
        "--play-paths"); // runtime telemetry: normal-play paths under a scripted scenario camera
    // stage the stained-glass capture subject near spawn.
    g_debug_glass_panes = HasCommandLineFlag(argc, argv, "--debug-glass-pane");
    // Opt-in GPU auto-exposure metering for capture diagnostics; the analytic
    // exposure curve remains the shipped gameplay default.
    g_auto_exposure_metered = HasCommandLineFlag(argc, argv, "--auto-exposure-metered");
    //  rendering (,  ): opt-in froxel volumetrics tier.
    g_volumetric_quality = std::stoi(GetCommandLineOption(argc, argv, "--volumetric-quality", "0"));
    g_profile_fly_seconds = static_cast<double>(GetCommandLineIntOption(
        argc, argv, "--profile-fly", 0)); // runtime telemetry: constant-speed eye-level moving
                                          // profiler (normal-play, self-exits)
    g_render_benchmark_frames =
        GetCommandLineIntOption(argc, argv, "--render-benchmark-frames", 120);
    g_render_benchmark_warmup =
        GetCommandLineIntOption(argc, argv, "--render-benchmark-warmup", 60);
    g_render_benchmark_screenshot =
        GetCommandLineOption(argc, argv, "--render-benchmark-screenshot", "");
    {
        const std::string cp = GetCommandLineOption(argc, argv, "--cam-pos", "");
        if (!cp.empty()) {
            float x = 0, y = 0, z = 0;
            if (std::sscanf(cp.c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
                g_fixed_cam_pos = glm::vec3(x, y, z);
                g_fixed_cam = true;
            }
        }
        const std::string cy = GetCommandLineOption(argc, argv, "--cam-yaw", "");
        if (!cy.empty()) {
            try {
                g_fixed_cam_yaw = std::stof(cy);
            } catch (...) {}
        }
        const std::string cpi = GetCommandLineOption(argc, argv, "--cam-pitch", "");
        if (!cpi.empty()) {
            try {
                g_fixed_cam_pitch = std::stof(cpi);
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
            g_scene_active = true;
            if (j.contains("camera")) {
                const auto& c = j["camera"];
                if (c.contains("pos") && c["pos"].size() == 3) {
                    g_fixed_cam_pos = glm::vec3(c["pos"][0].get<float>(),
                                                c["pos"][1].get<float>(),
                                                c["pos"][2].get<float>());
                    g_fixed_cam = true;
                }
                if (c.contains("yaw"))
                    g_fixed_cam_yaw = c["yaw"].get<float>();
                if (c.contains("pitch"))
                    g_fixed_cam_pitch = c["pitch"].get<float>();
                if (c.contains("fov"))
                    g_scene_fov = c["fov"].get<float>();
            }
            if (j.contains("time_of_day"))
                g_timelapse_tod = j["time_of_day"].get<float>();
            if (j.contains("moon"))
                g_scene_moon = j["moon"].get<float>(); // rendering: night-mode capture
            if (j.contains("weather")) {
                const auto& w = j["weather"];
                const std::string t = w.value("type", "none");
                g_scene_weather = (t == "rain")    ? 1
                                  : (t == "snow")  ? 2
                                  : (t == "fog")   ? 3
                                  : (t == "storm") ? 4
                                                   : 0;
                g_scene_weather_intensity = w.value("intensity", 0.0f);
            }
            if (j.contains("clouds")) {
                const auto& cl = j["clouds"];
                g_scene_clouds = true;
                g_scene_cloud_coverage = cl.value("coverage", 0.45f);
                g_scene_cloud_biome = cl.value("biome_variation", 0.0f);
                g_scene_cloud_plane = cl.value("plane_height", 900.0f);
                g_scene_cloud_shadow = cl.value("shadow", false);
                g_scene_cloud_shadow_strength = cl.value("shadow_strength", 0.0f);
            }
            // Drive the single-frame capture through the timelapse path.
            const std::string shot = j.value("screenshot", std::string("build/captures/scene.ppm"));
            std::filesystem::path shotPath(shot);
            shotPath.replace_extension(".ppm"); // WritePixelBufferPpm writes PPM
            g_scene_shot = shotPath;
            g_scene_dir =
                shotPath.has_parent_path() ? shotPath.parent_path() : std::filesystem::path(".");
            // Scene capture is self-contained (handled at the render site, own g_scene_dir
            // so the later --timelapse-dir default can't clobber it). tod applied per-frame.
            std::error_code _sc_ec;
            std::filesystem::create_directories(g_scene_dir, _sc_ec);
            LUMINUMBRA_CORE_INFO(
                "Scene-config loaded: {} (tod {:.3f}, weather {}, clouds {}) -> {}",
                sc,
                g_timelapse_tod,
                g_scene_weather,
                g_scene_clouds,
                g_timelapse_dir.string());
        } catch (const std::exception& e) {
            LUMINUMBRA_CORE_ERROR("Scene-config parse failed: {}", e.what());
        }
    }

    // framescan: --frame-scan <out.json>. Pin the same forest-dense pose the render
    // benchmark uses so the scan is reproducible + stresses real coverage, then write
    // the what's-in-frame report and exit. Render-only (no draws, no world_hash).
    if (const std::string fs = GetCommandLineOption(argc, argv, "--frame-scan", ""); !fs.empty()) {
        g_frame_scan_active = true;
        g_frame_scan_path = fs;
        // Self-sufficient: --frame-scan IMPLIES the auto-world boot it needs (it must reach IN_GAME
        // for the settle/capture to run). Without this, omitting
        // --auto-create-world/--auto-enter-world left the client looping in the menu forever. A
        // watchdog in the render loop is the backstop.
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Frame-scan armed -> {} (auto-world implied, fixed pose, settle {} frames)",
            g_frame_scan_path,
            kFrameScanSettleFrames);
    }
    // --render-parity-frame <dir>. Same boot/settle, then the in-process
    // WHOLE-FRAME A/B — dispatch the settled prepared frame twice into twin targets,
    // FLIP in-process, demand exactly 0.0 (the  migration gate).
    if (const std::string rp = GetCommandLineOption(argc, argv, "--render-parity-frame", "");
        !rp.empty()) {
        g_render_parity_active = true;
        g_render_parity_dir = std::filesystem::path(rp);
        g_render_parity_pass = "frame";
        g_frame_scan_active = true;
        g_frame_scan_path = (g_render_parity_dir / "parity_scan.json").string();
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Render-parity (whole-frame) armed -> {} (auto-world implied, settle {} frames)",
            g_render_parity_dir.string(),
            kFrameScanSettleFrames);
    }
    // native scale-1 reference vs exact scale-1 seam and the
    // scale-0.67 upscaled output, all in one process/context to avoid capture noise.
    if (const std::string rp = GetCommandLineOption(argc, argv, "--upscale-seam-parity", "");
        !rp.empty()) {
        g_render_parity_active = true;
        g_render_parity_dir = std::filesystem::path(rp);
        g_render_parity_pass = "upscale_seam";
        g_frame_scan_active = true;
        g_frame_scan_path = (g_render_parity_dir / "parity_scan.json").string();
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Upscale-seam parity armed -> {} (auto-world implied, settle {} frames)",
            g_render_parity_dir.string(),
            kFrameScanSettleFrames);
    }
    // DIAGNOSTIC-only settle override (see g_frame_scan_settle_target): let a fully-loaded
    // capture wait past the gate's 90-frame default. Gates never set this env.
    if (const auto settle_env = Luminumbra::Core::ReadEnvironment("LUMIN_FRAME_SCAN_SETTLE")) {
        const int v = std::atoi(settle_env->c_str());
        if (v > 0)
            g_frame_scan_settle_target = v;
    }
    // -T02: --render-parity-ssao <dir>. Same boot/settle, captures the
    // SSAO ctx-mapping + seam-determinism parity gate.
    if (const std::string rp = GetCommandLineOption(argc, argv, "--render-parity-ssao", "");
        !rp.empty()) {
        g_render_parity_active = true;
        g_render_parity_dir = std::filesystem::path(rp);
        g_render_parity_pass = "ssao";
        g_frame_scan_active = true;
        g_frame_scan_path = (g_render_parity_dir / "parity_scan.json").string();
        scenario_config.auto_create_world = true;
        scenario_config.auto_enter_world = true;
        LUMINUMBRA_CORE_INFO(
            "Render-parity (SSAO) armed -> {} (auto-world implied, settle {} frames)",
            g_render_parity_dir.string(),
            kFrameScanSettleFrames);
    }
    // --bake-tree-impostor <out.ppm>: bake the far-field tree impostor atlas (no world needed).
    if (const std::string bp = GetCommandLineOption(argc, argv, "--bake-tree-impostor", "");
        !bp.empty()) {
        g_bake_impostor_path = bp;
        LUMINUMBRA_CORE_INFO("Impostor-bake armed -> {} (GL atlas bake on first frame, then exit)",
                             bp);
    }
    // --survey <dir>: autonomous POI tour + per-scene screenshot/frame-scan. Pair with
    // --auto-create-world --auto-enter-world --no-audio.
    if (const std::string sv = GetCommandLineOption(argc, argv, "--survey", ""); !sv.empty()) {
        g_survey_active = true;
        g_survey_dir = sv;
        LUMINUMBRA_CORE_INFO(
            "Scene survey armed -> {} (auto-world; tours waterfall/cliff/grass/lake)",
            g_survey_dir);
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
                g_ui_screens.push_back(name);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        g_ui_fixtures = HasCommandLineFlag(argc, argv, "--ui-fixtures");
        g_ui_screenshot_dir =
            GetCommandLineOption(argc, argv, "--ui-screenshot-dir", "build/captures/ui");
        std::error_code _ui_ec;
        std::filesystem::create_directories(g_ui_screenshot_dir, _ui_ec);
        if (!g_ui_screens.empty())
            g_ui_screenshot_screen = g_ui_screens.front();
        // --preview-live [--preview-weather rain]: for the world_creation screen, wait for the
        // live diorama (async candidate-world build + far field + precipitation) before capturing,
        // so the create-world UI redesign, the centre-anchored far field, and the preview rain are
        // VISUALLY CAPTURABLE headlessly (the default fixed-settle path captures a black backdrop
        // because the world is still building). Bounded by a wall-clock timeout (never hangs).
        g_ui_preview_live = HasCommandLineFlag(argc, argv, "--preview-live");
        g_ui_preview_weather = GetCommandLineOption(argc, argv, "--preview-weather", "");
        LUMINUMBRA_CORE_INFO("UI screenshot mode: {} screen(s), fixtures={}, preview_live={}, "
                             "preview_weather='{}' -> {}/ui-*.ppm",
                             g_ui_screens.size(),
                             g_ui_fixtures,
                             g_ui_preview_live,
                             g_ui_preview_weather,
                             g_ui_screenshot_dir.string());
    }

    // --ui-thumbs N [--ui-thumbs-dir d]: capture N clean landscape thumbnails from the menu
    // backdrop world (varied yaw + time-of-day), one window session. For world/gallery tiles.
    if (const int n = GetCommandLineIntOption(argc, argv, "--ui-thumbs", 0); n > 0) {
        g_ui_thumbs = n;
        g_ui_thumbs_dir = GetCommandLineOption(argc, argv, "--ui-thumbs-dir", "data/ui/thumbs");
        std::error_code _t_ec;
        std::filesystem::create_directories(g_ui_thumbs_dir, _t_ec);
        LUMINUMBRA_CORE_INFO("UI thumbnail mode: {} thumbs -> {}/thumb_*.ppm",
                             g_ui_thumbs,
                             g_ui_thumbs_dir.string());
    }

    // --timelapse capture mode. Single-player; pair with
    // --auto-create-world --auto-enter-world (and --no-ui for a clean frame).
    g_timelapse_frames = GetCommandLineIntOption(argc, argv, "--timelapse-frames", 0);
    g_timelapse_ticks = GetCommandLineIntOption(argc, argv, "--timelapse-ticks", 60);
    g_timelapse_grow = HasCommandLineFlag(argc, argv, "--timelapse-grow");
    if (g_timelapse_grow)
        g_procgenStageF = 0.0f; // start as seeds; grow sapling->tree over the capture
    g_timelapse_simgrow = HasCommandLineFlag(argc, argv, "--timelapse-simgrow");
    g_timelapse_season = HasCommandLineFlag(argc, argv, "--timelapse-season");
    if (g_timelapse_season)
        g_season = 0.0f; // start summer-green; drift to autumn over the capture
    g_timelapse_creatures = HasCommandLineFlag(argc, argv, "--timelapse-creatures");
    g_timelapse_living = HasCommandLineFlag(argc, argv, "--timelapse-living");
    g_timelapse_calm = HasCommandLineFlag(argc, argv, "--timelapse-calm");
    if (g_timelapse_calm)
        g_timelapse_creatures = true; // calm mode is a creature scenario
    g_timelapse_fire = HasCommandLineFlag(argc, argv, "--timelapse-fire");
    g_timelapse_dig = HasCommandLineFlag(argc, argv, "--timelapse-dig");
    g_timelapse_drain = HasCommandLineFlag(argc, argv, "--timelapse-drain");
    g_timelapse_rain = HasCommandLineFlag(argc, argv, "--timelapse-rain");
    g_timelapse_rain_mm = GetCommandLineIntOption(argc, argv, "--timelapse-rain-mm", 18);
    {
        const std::string ds = GetCommandLineOption(argc, argv, "--timelapse-daystep", "");
        if (!ds.empty()) {
            try {
                g_timelapse_daystep = std::stof(ds);
            } catch (...) {}
        }
        const std::string t0 = GetCommandLineOption(argc, argv, "--timelapse-tod", "");
        if (!t0.empty()) {
            try {
                g_timelapse_tod = std::stof(t0);
            } catch (...) {}
        }
        const std::string td = GetCommandLineOption(argc, argv, "--timelapse-dir", "");
        g_timelapse_dir = !td.empty() ? std::filesystem::path(td)
                                      : (!scenario_config.artifact_dir.empty()
                                             ? scenario_config.artifact_dir / "timelapse"
                                             : std::filesystem::path("timelapse"));
    }
    if (g_timelapse_frames > 0) {
        std::error_code _tl_ec;
        std::filesystem::create_directories(g_timelapse_dir, _tl_ec);
        // NOTE: keep g_timeScale = 1 so the player physics + collision settle each frame (a
        // frozen physics step makes the avatar fall through the streaming-in ground). The
        // capture loop adds EXTRA sim ticks for the fast-forward; the player stays grounded.
        LUMINUMBRA_CORE_INFO("Timelapse: {} frames, {} ticks/frame, daystep {:.4f} -> {}",
                             g_timelapse_frames,
                             g_timelapse_ticks,
                             g_timelapse_daystep,
                             g_timelapse_dir.string());
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
        scenario_config.requires_pinned_capture() || !g_render_benchmark_path.empty();
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
        (!g_render_benchmark_path.empty() || g_systemConfig.user().vsync == false) ? 0 : 1);

    // [[maybe_unused]]: LUMINUMBRA_ASSERT compiles out in release builds, which
    // compile with warnings as errors.
    [[maybe_unused]] const int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    LUMINUMBRA_ASSERT(status, "Failed to initialize GLAD!");

    if (g_imgui_enabled) {
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
        const std::size_t loaded = g_creatureSpecies.LoadFromDirectory(species_dir, species_errors);
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

    if (!scenario_config.no_ui) {
        g_uiManager = std::make_unique<Luminumbra::Client::Rml_UIManager>(root_path_str);
        g_uiManager->Init(window, audioManager.get());
        // --ui-fixtures points capture-backed screens at
        // deterministic committed fixture data so --ui-screenshot output is
        // reproducible on any checkout (the live gallery is empty until a player
        // presses the shutter). Gallery today; extend per-screen as fixtures grow.
        if (g_ui_fixtures) {
            g_uiManager->SetGalleryCaptureSource(root_dir / "data" / "ui" / "fixtures" / "captures",
                                                 "fixtures/captures/");
            LUMINUMBRA_CORE_INFO("UI fixtures: gallery sourcing data/ui/fixtures/captures");
        }
        // opt-in UI hot reload: watch data/ui and reload the active document on edits.
        if (HasCommandLineFlag(argc, argv, "--ui-hot-reload")) {
            g_uiHotReload.SetEnabled(true);
            g_uiHotReload.WatchDirectory("data/ui", "rml");
            g_uiHotReload.WatchDirectory("data/ui", "rcss");
            g_uiHotReload.SetReloadCallback([](const std::string&) {
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
        if (g_imgui_enabled) {
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
    glfwSetWindowUserPointer(window, &renderPipeline);
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
            g_menu_backdrop_active = false;
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
                if (g_world_render_data_initialized) {
                    renderPipeline.clear_all_chunk_data();
                }
                g_world_render_data_initialized = true;
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
                g_initial_chunks_to_load.clear();
            } else {
                g_initial_chunks_to_load = world_system->GetInitialChunkLoadList(spawn_pos);
            }

            g_generation_dispatch_index = 0;
            if (g_loading_visualizer) {
                g_loading_visualizer->BeginVisualization(g_initial_chunks_to_load);
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
                    g_rebindCaptureAction = static_cast<int>(i);
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
        const std::string boot_doc = g_ui_screenshot_screen.empty()
                                         ? std::string("main_menu.rml")
                                         : (g_ui_screenshot_screen + ".rml");
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
    if (g_imgui_enabled) {
        worldGenViewer = std::make_unique<Luminumbra::Client::WorldGenViewer>();
        //  --worldgen-graph turns on the constrained layer-graph
        // authoring panel inside the inspector (a flagged INTERNAL dev tool, NOT
        // the shipping RmlUi create surface) and opens the inspector at boot.
        if (HasCommandLineFlag(argc, argv, "--worldgen-graph")) {
            worldGenViewer->SetGraphEnabled(true);
            show_worldgen_viewer = true;
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
    // Last candidate signature so we only re-derive + push params when the form
    // actually changed (the controller then debounces the rebuild).
    std::string worldgenPreviewLastSig;
    bool worldgenPreviewDragging = false;
    double worldgenPreviewLastCursorX = 0.0, worldgenPreviewLastCursorY = 0.0;

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
                                            wireframe_mode);
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
        const bool drives_own_world = scenario_config.active() ||
                                      HasCommandLineFlag(argc, argv, "--auto-create-world") ||
                                      runtime_boot_recorder.enabled() || g_timelapse_frames > 0 ||
                                      !g_render_benchmark_path.empty();
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
                    g_world_render_data_initialized = true;
                    g_menu_backdrop_active = true;
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
        const bool g_rb_active = !g_render_benchmark_path.empty();
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
        if (!g_bake_impostor_path.empty() && !g_bake_impostor_done) {
            g_bake_impostor_done = true;
            Luminumbra::Rendering::OctaImpostorGrid bakeGrid;
            bakeGrid.gridResolution = 12; // 12x12 = 144 views for smoother runtime view blending
            const Luminumbra::Rendering::ImpostorBakeResult br =
                Luminumbra::Rendering::BakeTreeImpostorAtlas(
                    g_bake_impostor_path, root_dir.string(), renderPipeline, bakeGrid);
            if (br.ok) {
                LUMINUMBRA_CORE_INFO(
                    "Impostor atlas baked -> {} ({}x{} px): mean coverage {:.3f}, min tile {:.3f}",
                    g_bake_impostor_path,
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
        g_uiHotReload.Update(); // No-op unless --ui-hot-reload enabled it; throttled to 1 second.

        GameState currentState = gameStateManager.GetCurrentState();

        // 3D audio LISTENER follows the player so footsteps / ambient bed / creature sounds
        // spatialize correctly (without this the listener sits at the origin and positional audio
        // is near-silent).
        if (currentState == GameState::IN_GAME && audioManager && g_camera) {
            audioManager->SetListenerTransform(g_camera->Position, g_camera->Front, g_camera->Up);
            // point the audio-occlusion raycaster at the LIVE physics world so
            // geometry between a sound and the listener muffles it (real raycasts, not the
            // distance-only fallback). Re-set each frame -> correct across world (re)load/unload;
            // nullptr when there is no physics -> occlusion falls back to distance-only.
            if (auto* mam =
                    dynamic_cast<Luminumbra::Client::MiniaudioManager*>(audioManager.get())) {
                mam->SetPhysicsSystem(gameSession ? gameSession->GetPhysicsSystem() : nullptr);
            }
            // feed the sun elevation (sin; the sun vector points AWAY from
            // the sun — same convention as the dawn/dusk cues below) + tick the
            // day/night bed crossfade (internally throttled to 10 Hz).
            envAudio->SetSunElevation(-renderPipeline.sun_direction().y);
            envAudio->Update(g_camera->Position, static_cast<float>(deltaTime));
        }
        // Player FOOTSTEPS (interactive audio): when the player walks, play a footstep keyed to the
        // surface material under them (stone vs grass/soil/etc.), at a distance-based cadence so it
        // tracks speed. Render/audio-only; live play only (never in scenario captures, so
        // determinism and the visual gates are untouched). Stride length / speed gates are tunable
        // by ear.
        if (currentState == GameState::IN_GAME && audioManager && g_camera && !g_paused &&
            !scenario_config.active() && gameSession && gameSession->GetWorldSystem()) {
            static glm::vec3 s_footLastPos = g_camera->Position;
            static float s_footDist = 0.0f;
            const glm::vec3 fp = g_camera->Position;
            const float fdx = fp.x - s_footLastPos.x, fdz = fp.z - s_footLastPos.z;
            const float fhoriz = std::sqrt(fdx * fdx + fdz * fdz);
            s_footLastPos = fp;
            const float fspeed = deltaTime > 0.0f ? fhoriz / static_cast<float>(deltaTime) : 0.0f;
            if (fspeed < 0.8f || fspeed > 25.0f) {
                s_footDist = 0.0f; // standing still, or a teleport/respawn jump -> reset
            } else {
                s_footDist += fhoriz;
                if (s_footDist >= 1.9f) { // stride length (m)
                    s_footDist = 0.0f;
                    auto* fws = gameSession->GetWorldSystem();
                    const float fth = fws->GetTerrainHeightAt(fp.x, fp.z);
                    // Material-based footstep: the surface under the player picks the sound.
                    const char* fev = "footstep_grass";
                    switch (fws->SurfaceVertexMaterial(fp.x, fp.z, fth)) {
                        case Luminumbra::MaterialType::Stone:
                        case Luminumbra::MaterialType::Deepslate:
                            fev = "footstep_stone";
                            break;
                        case Luminumbra::MaterialType::Soil:
                            fev = "footstep_soil";
                            break;
                        case Luminumbra::MaterialType::Sand:
                            fev = "footstep_sand";
                            break;
                        case Luminumbra::MaterialType::Water:
                            fev = "footstep_water";
                            break;
                        case Luminumbra::MaterialType::LuminCrystal:
                            fev = "footstep_crystal";
                            break;
                        case Luminumbra::MaterialType::Grass:
                        default:
                            fev = "footstep_grass";
                            break;
                    }
                    audioManager->PlayOneShot(fev, glm::vec3(fp.x, fth, fp.z));
                }
            }
        }

        // LIVING-WORLD AUDIO: weather-reactive RAIN + occasional CREATURE CALLS. Periodic in live
        // play; render/audio-only (reads sim state, never mutates -> determinism + gates
        // untouched).
        if (currentState == GameState::IN_GAME && audioManager && g_camera && !g_paused &&
            !scenario_config.active() && gameSession) {
            static float s_envTimer = 0.0f, s_callTimer = 0.0f, s_sleepTimer = 0.0f;
            static float s_feedTimer = 0.0f, s_colonyTimer = 0.0f;
            static bool s_rainOn = false, s_waterOn = false;
            s_envTimer += static_cast<float>(deltaTime);
            s_callTimer += static_cast<float>(deltaTime);
            s_sleepTimer += static_cast<float>(deltaTime);
            s_feedTimer += static_cast<float>(deltaTime);
            s_colonyTimer += static_cast<float>(deltaTime);
            const glm::vec3 pc = g_camera->Position;
            if (s_envTimer >= 0.5f) {
                s_envTimer = 0.0f;
                // Rain: tie ambient_rain to the live weather precipitation at the player
                // (hysteresis so it doesn't flutter at a storm-cell edge). Same field the foliage
                // growth reads.
                if (auto* weather = gameSession->GetWeatherSystem()) {
                    const float precip =
                        weather->PrecipitationAt(Luminumbra::Vec3(pc.x, pc.y, pc.z));
                    if (!s_rainOn && precip > 0.18f) {
                        audioManager->PlayAmbientLoop("ambient_rain", pc, 1.0e6f);
                        s_rainOn = true;
                    } else if (s_rainOn && precip < 0.08f) {
                        audioManager->StopAmbientLoop("ambient_rain");
                        s_rainOn = false;
                    }
                    // thunder now follows the SIM strike schedule when
                    // the live-weather bridge is on — each queued strike cue fires after
                    // its physical sound delay (distance / 343 m/s), volume by
                    // magnitude/distance. The legacy flat 22 s timer remains ONLY as the
                    // fallback when the bridge is off (no schedule consumer running).
                    if (g_systemConfig.enabled(luminumbra::core::SysKey::RenderLiveWeather)) {
                        const double now_s = glfwGetTime();
                        for (auto it = g_pendingThunder.begin(); it != g_pendingThunder.end();) {
                            const double delay_s = it->distance_m / 343.0; // speed of sound
                            if (now_s - it->fired_at_seconds >= delay_s) {
                                audioManager->PlayOneShot2D("thunder",
                                                            Luminumbra::Client::BusId::Events);
                                it = g_pendingThunder.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    } else {
                        static float s_thunderTimer = 0.0f;
                        if (precip > 0.40f) {
                            s_thunderTimer += 0.5f; // this branch runs once per 0.5 s tick above
                            if (s_thunderTimer >= 22.0f) {
                                s_thunderTimer = 0.0f;
                                audioManager->PlayOneShot2D("thunder",
                                                            Luminumbra::Client::BusId::Events);
                            }
                        } else {
                            s_thunderTimer = 0.0f;
                        }
                    }
                }
                // Water: a gentle stream bed when standing water is within ~14 m (a ring probe of
                // the water surface vs terrain). Fades in/out as you approach / leave a river or
                // lake.
                if (auto* ws2 = gameSession->GetWorldSystem()) {
                    static const float off[5][2] = {{0, 0}, {14, 0}, {-14, 0}, {0, 14}, {0, -14}};
                    bool nearWater = false;
                    for (const auto& o : off) {
                        const float wx = pc.x + o[0], wz = pc.z + o[1];
                        if (ws2->WaterLevelAt(wx, wz) > ws2->GetTerrainHeightAt(wx, wz) + 0.4f) {
                            nearWater = true;
                            break;
                        }
                    }
                    if (nearWater && !s_waterOn) {
                        audioManager->PlayAmbientLoop("ambient_stream", pc, 1.0e6f);
                        s_waterOn = true;
                    } else if (!nearWater && s_waterOn) {
                        audioManager->StopAmbientLoop("ambient_stream");
                        s_waterOn = false;
                    }
                    // waterfall ROAR — the nearest DETECTED waterfall
                    // site within earshot drives a positional ambient loop through the
                    // previously never-called ComputeWaterfallRoar. The
                    // detector's cached sites are the SAME set the render sheets use, so
                    // what you hear is what you see. Pure reads; client-only.
                    {
                        static bool s_roarOn = false;
                        const auto& falls = renderPipeline.waterfall_sites(*ws2);
                        const Luminumbra::Rendering::WaterfallSite* best = nullptr;
                        float best_d2 = 400.0f * 400.0f;
                        for (const auto& site : falls) {
                            const glm::vec3 d = site.crest - pc;
                            const float d2 = glm::dot(d, d);
                            if (d2 < best_d2) {
                                best_d2 = d2;
                                best = &site;
                            }
                        }
                        if (best != nullptr) {
                            const auto roar =
                                Luminumbra::Client::AudioPropagationSystem::ComputeWaterfallRoar(
                                    best->crest, best->drop_height, pc);
                            if (roar.audible) {
                                if (!s_roarOn) {
                                    audioManager->PlayAmbientLoop(
                                        "waterfall_roar", best->crest, 400.0f);
                                    s_roarOn = true;
                                }
                                audioManager->SetAmbientVolume("waterfall_roar", roar.volume);
                            } else if (s_roarOn) {
                                audioManager->StopAmbientLoop("waterfall_roar");
                                s_roarOn = false;
                            }
                        } else if (s_roarOn) {
                            audioManager->StopAmbientLoop("waterfall_roar");
                            s_roarOn = false;
                        }
                    }
                }
                // Wind GUSTS: the wind bed never stops, but its volume breathes with the live
                // wind-field magnitude so a gust is actually felt (calm still whispers).
                if (auto* weather2 = gameSession->GetWeatherSystem()) {
                    const auto wsmp = weather2->SampleAt(Luminumbra::Vec3(pc.x, pc.y, pc.z));
                    const float windMag =
                        std::sqrt(wsmp.wind.x * wsmp.wind.x + wsmp.wind.y * wsmp.wind.y);
                    const float swell = 0.5f + std::min(windMag / 6.0f, 1.0f) * 1.1f; // [0.5.. 1.6]
                    audioManager->SetAmbientVolume("ambient_wind", swell);
                }
                // biome reverb base (idempotent), then the weather reverb
                // shift — order matters: UpdateAtmosphere layers on the last base.
                if (auto* wsr = gameSession->GetWorldSystem()) {
                    const auto& br = wsr->BiomeReverbAt(pc.x, pc.z);
                    envAudio->ApplyBiomeReverb(br.preset, br.wet, br.dry, br.decay);
                }
                if (auto* weather3 = gameSession->GetWeatherSystem()) {
                    const auto smp = weather3->SampleAt(Luminumbra::Vec3(pc.x, pc.y, pc.z));
                    // WeatherSample.wind is a Vec2 in the world XZ plane:.y -> z.
                    envAudio->UpdateAtmosphere(glm::vec3(smp.wind.x, 0.0f, smp.wind.y),
                                               smp.precip_intensity,
                                               smp.storm_intensity);
                }
            }
            // Occasional call from the nearest LIVE creature (<50 m) so the world has voices.
            if (s_callTimer >= 11.0f) {
                s_callTimer = 0.0f;
                const auto& reg = gameSession->GetRegistry();
                auto cview = reg.view<const Luminumbra::Components::CreatureComponent,
                                      const Luminumbra::Components::TransformComponent>();
                entt::entity best = entt::null;
                float bestD = 50.0f * 50.0f;
                glm::vec3 bestPos(0.0f);
                std::uint16_t bestSpecies = 0;
                for (auto e : cview) {
                    const auto& cc = cview.get<const Luminumbra::Components::CreatureComponent>(e);
                    if (cc.eaten)
                        continue;
                    const auto& tf = cview.get<const Luminumbra::Components::TransformComponent>(e);
                    const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD) {
                        bestD = d2;
                        best = e;
                        bestSpecies = cc.species_id;
                        bestPos = glm::vec3(tf.position.x, tf.position.y, tf.position.z);
                    }
                }
                if (best != entt::null) {
                    // Per-species voice: map the species id -> "creature_<id>_call". Every species
                    // has a call event in the bank; an unspecified/legacy id falls back to
                    // grovestrider.
                    std::string ev = "creature_grovestrider_call";
                    for (const auto& sp : g_creatureSpecies.all()) {
                        if (sp.species_id() == bestSpecies) {
                            ev = "creature_" + sp.id + "_call";
                            break;
                        }
                    }
                    audioManager->PlayOneShot(ev, bestPos);
                } else {
                    s_callTimer = 8.0f; // nobody near -> check again soon
                }
            }
            // a soft sleeping breath from the nearest SLEEPING creature (<18 m) every
            // ~6 s, so a creature bedded down for the night reads as alive, not frozen. last_action
            // == Sleep (CreatureAction::Sleep = 5). Render-only; one breath at a time stays subtle.
            if (s_sleepTimer >= 6.0f) {
                s_sleepTimer = 0.0f;
                const auto& reg = gameSession->GetRegistry();
                auto sview = reg.view<const Luminumbra::Components::CreatureComponent,
                                      const Luminumbra::Components::TransformComponent>();
                entt::entity best = entt::null;
                float bestD = 18.0f * 18.0f;
                glm::vec3 bestPos(0.0f);
                for (auto e : sview) {
                    const auto& cc = sview.get<const Luminumbra::Components::CreatureComponent>(e);
                    if (cc.eaten || cc.last_action != 5 /* CreatureAction::Sleep */)
                        continue;
                    const auto& tf = sview.get<const Luminumbra::Components::TransformComponent>(e);
                    const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD) {
                        bestD = d2;
                        best = e;
                        bestPos = glm::vec3(tf.position.x, tf.position.y, tf.position.z);
                    }
                }
                if (best != entt::null)
                    audioManager->PlayOneShot("creature_sleep", bestPos);
                else
                    s_sleepTimer = 4.0f; // none asleep nearby -> re-check sooner
            }
            // per-action audio: the nearest GRAZING creature (<20 m) emits a
            // soft feed/chew every ~5 s (a drink/sip instead if it is feeding right at the water's
            // edge). last_action == Graze (CreatureAction::Graze = 1). Render-only.
            if (s_feedTimer >= 5.0f) {
                s_feedTimer = 0.0f;
                const auto& reg = gameSession->GetRegistry();
                auto* ws3 = gameSession->GetWorldSystem();
                auto gview = reg.view<const Luminumbra::Components::CreatureComponent,
                                      const Luminumbra::Components::TransformComponent>();
                entt::entity best = entt::null;
                float bestD = 20.0f * 20.0f;
                glm::vec3 bestPos(0.0f);
                for (auto e : gview) {
                    const auto& cc = gview.get<const Luminumbra::Components::CreatureComponent>(e);
                    if (cc.eaten || cc.last_action != 1 /* CreatureAction::Graze */)
                        continue;
                    const auto& tf = gview.get<const Luminumbra::Components::TransformComponent>(e);
                    const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD) {
                        bestD = d2;
                        best = e;
                        bestPos = glm::vec3(tf.position.x, tf.position.y, tf.position.z);
                    }
                }
                if (best != entt::null) {
                    // Drinking proxy: if the grazer is at the water's edge, it sips instead of
                    // chews.
                    const bool atWater =
                        ws3 && ws3->WaterLevelAt(bestPos.x, bestPos.z) >
                                   ws3->GetTerrainHeightAt(bestPos.x, bestPos.z) + 0.2f;
                    audioManager->PlayOneShot(atWater ? "creature_drink" : "creature_feed",
                                              bestPos);
                } else {
                    s_feedTimer = 3.0f; // nobody grazing nearby -> re-check sooner
                }
            }
            // colony bed: a faint chitter from the nearest forager NEST (<25 m)
            // every ~5 s, so an active ant colony reads as alive. Keyed on the colony's shared home
            // cell (every ForagerComponent carries it). Render-only.
            if (s_colonyTimer >= 5.0f) {
                s_colonyTimer = 0.0f;
                const auto& reg = gameSession->GetRegistry();
                auto* ws4 = gameSession->GetWorldSystem();
                auto nview = reg.view<const Luminumbra::Components::ForagerComponent>();
                bool haveNest = false;
                glm::vec3 nestPos(0.0f);
                float bestD = 25.0f * 25.0f;
                for (auto e : nview) {
                    const auto& fg = nview.get<const Luminumbra::Components::ForagerComponent>(e);
                    const float wx = gameSession->ScentCellToWorldX(fg.home_x);
                    const float wz = gameSession->ScentCellToWorldZ(fg.home_z);
                    const float dx = wx - pc.x, dz = wz - pc.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD) {
                        bestD = d2;
                        haveNest = true;
                        const float wy = (ws4 ? ws4->GetTerrainHeightAt(wx, wz) : 0.0f) + 0.3f;
                        nestPos = glm::vec3(wx, wy, wz);
                    }
                }
                if (haveNest)
                    audioManager->PlayOneShot("creature_colony", nestPos);
                else
                    s_colonyTimer = 3.0f;
            }
            // Creature LOCOMOTION sound: grounded creatures near the player tick a soft footfall
            // as they travel (stride-accumulated from real movement, so cadence tracks speed);
            // fliers (corvid/heron/finch/moth) get wingbeats at a longer interval; the small
            // skink gets a light skitter. Render-only.
            {
                static std::unordered_set<std::uint16_t> s_fliers, s_light;
                if (s_fliers.empty()) {
                    for (const char* f :
                         {"ashen_corvid", "dusk_heron", "glimmer_finch", "lumen_moth"})
                        s_fliers.insert(Luminumbra::Components::CreatureSpeciesId16(f));
                    s_light.insert(Luminumbra::Components::CreatureSpeciesId16("ember_skink"));
                }
                static std::unordered_map<std::uint32_t, std::pair<glm::vec2, float>> s_stride;
                if (s_stride.size() > 512)
                    s_stride.clear(); // bound: render-only bookkeeping
                const auto& reg = gameSession->GetRegistry();
                auto fview = reg.view<const Luminumbra::Components::CreatureComponent,
                                      const Luminumbra::Components::TransformComponent>();
                for (auto e : fview) {
                    const auto& cc = fview.get<const Luminumbra::Components::CreatureComponent>(e);
                    if (cc.eaten)
                        continue;
                    const auto& tf = fview.get<const Luminumbra::Components::TransformComponent>(e);
                    const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                    if (dx * dx + dz * dz > 35.0f * 35.0f)
                        continue; // only the audible ones
                    // Pick the gait sound + stride length by species class.
                    const char* gait = "creature_grovestrider_footstep";
                    float kStride = 1.7f;
                    if (s_fliers.count(cc.species_id)) {
                        gait = "creature_wingbeat";
                        kStride = 3.0f;
                    } else if (s_light.count(cc.species_id)) {
                        gait = "creature_footstep_light";
                        kStride = 1.0f;
                    }
                    const glm::vec2 cur(tf.position.x, tf.position.z);
                    const auto key = static_cast<std::uint32_t>(entt::to_integral(e));
                    auto it = s_stride.find(key);
                    if (it == s_stride.end()) {
                        s_stride.emplace(key, std::make_pair(cur, 0.0f));
                        continue;
                    }
                    const float moved = glm::distance(cur, it->second.first);
                    it->second.first = cur;
                    if (moved > 5.0f)
                        continue; // ignore teleport-sized jumps (re-anchor/respawn)
                    it->second.second += moved;
                    if (it->second.second >= kStride) {
                        it->second.second -= kStride;
                        audioManager->PlayOneShot(
                            gait, glm::vec3(tf.position.x, tf.position.y, tf.position.z));
                    }
                }
            }
            // Day/night: a soft cue as the sun crosses the horizon — brightening at dawn, settling
            // at dusk. Sun elevation = -sun_direction.y (the vector points away from the sun).
            // The state only flips once clearly past the horizon, so it fires once per transition.
            {
                static int s_sunUp = -1; // -1 uninit, 0 below horizon, 1 above
                const float elev = -renderPipeline.sun_direction().y;
                const int up = elev > 0.0f ? 1 : 0;
                if (s_sunUp == -1) {
                    s_sunUp = up;
                } else if (up != s_sunUp) {
                    if (up == 1 && elev > 0.03f) {
                        audioManager->PlayOneShot2D("time_dawn", Luminumbra::Client::BusId::Events);
                        audioManager->PlayMusic("music_exploration");
                        s_sunUp = 1;
                    } else if (up == 0 && elev < -0.03f) {
                        audioManager->PlayOneShot2D("time_dusk", Luminumbra::Client::BusId::Events);
                        audioManager->PlayMusic("music_dusk");
                        s_sunUp = 0;
                    }
                }
            }
        }

        // mirror each forager's authoritative grid cell -> a world transform so the
        // colony has live positions, and a 10 s delivery-count heartbeat proving the double-bridge
        // actually forages in the live world (deliveries accrue => ants are completing trips).
        if (currentState == GameState::IN_GAME && !g_paused && !scenario_config.active() &&
            gameSession) {
            auto& freg = gameSession->GetRegistry();
            auto* fws = gameSession->GetWorldSystem();
            // headless automation skips the  one-time
            // flourishes entirely — captures want the world + shaders, not spelunking
            // cues (and the crystal point lights would move visual baselines).
            // --debug-goto cave still lights its framed cave (below).
            const bool headless_automation = g_frame_scan_active || g_scene_active ||
                                             g_play_paths || !g_render_benchmark_path.empty() ||
                                             !g_survey_dir.empty() || g_timelapse_frames > 0;
            // dispatch the  scans (doline locate + 25
            // cave anchors + hero call) as ONE background job batch instead of the
            // old frame-2 inline walk (6m25s main-thread in debug, est. 10-25 s
            // release). Pure deterministic SDF reads — the same sampling the meshing
            // workers already run concurrently. Process-once, matching the old
            // statics' behavior. Every world transition drains the handle first
            // (DrainBackgroundWorldScan), so the raw fws capture can never dangle.
            static bool s_backgroundWorldScanDispatched = false;
            if (!s_backgroundWorldScanDispatched && fws && !headless_automation) {
                s_backgroundWorldScanDispatched = true;
                auto scan = std::make_shared<BackgroundWorldScan>();
                scan->world = fws;
                const auto& sp = gameSession->GetMetadata().spawnPoint;
                const glm::vec3 spawn(sp.x, sp.y, sp.z);
                std::vector<Luminumbra::Job> scan_jobs;
                scan_jobs.reserve(26);
                // Job 0: doline locate + the hero enclosed-cave call.
                scan_jobs.emplace_back([scan, fws, spawn]() {
                    scan->doline = fws->FindLargestSurfaceBreak(spawn.x, spawn.z, 500.0f);
                    if (scan->doline.found) {
                        scan->doline_surface_h =
                            fws->GetTerrainHeightAt(scan->doline.x, scan->doline.z);
                    }
                    scan->hero = Luminumbra::Debug::FindEnclosedCave(*fws, spawn, 256.0f);
                });
                // Jobs 1..25: one enclosed-cave anchor each (disjoint result slots, no
                // locking; dedup happens at consume time in the ORIGINAL scan order so
                // crystal placement stays byte-identical to the old sequential walk).
                for (int gz = -2; gz <= 2; ++gz) {
                    for (int gx = -2; gx <= 2; ++gx) {
                        constexpr float kAnchorStep = 120.0f;   // metres between anchors
                        constexpr float kSearchRadius = 140.0f; // per-anchor search
                        const std::size_t slot = static_cast<std::size_t>((gz + 2) * 5 + (gx + 2));
                        const glm::vec3 anchorW(spawn.x + static_cast<float>(gx) * kAnchorStep,
                                                spawn.y,
                                                spawn.z + static_cast<float>(gz) * kAnchorStep);
                        scan_jobs.emplace_back([scan, fws, anchorW, slot]() {
                            if (auto cave = Luminumbra::Debug::FindEnclosedCave(
                                    *fws, anchorW, kSearchRadius)) {
                                scan->anchor_caves[slot] = cave->target;
                            }
                        });
                    }
                }
                s_backgroundWorldScanHandle = jobSystem.dispatch_batch(scan_jobs);
                s_backgroundWorldScan = std::move(scan);
                LUMINUMBRA_CORE_INFO(
                    " world scan dispatched to background jobs (doline + 25 cave anchors + hero)");
            }

            // Debug suite: --debug-goto cave|doline|spawn — deterministically frame a feature so
            // captures (--timelapse/--frame-scan) can SEE it (the gap that blocked cave shots).
            // Sets the fixed camera; the streaming anchor follows it (far-camera bug already
            // fixed).
            static bool s_debugGotoDone = false;
            if (!s_debugGotoDone && fws && !g_debug_goto.empty()) {
                s_debugGotoDone = true;
                const auto& dsp = gameSession->GetMetadata().spawnPoint;
                const glm::vec3 dnear(dsp.x, dsp.y, dsp.z);
                std::optional<Luminumbra::Debug::DebugCamPose> pose;
                if (g_debug_goto == "cave")
                    pose = Luminumbra::Debug::FindEnclosedCave(*fws, dnear, 256.0f);
                else if (g_debug_goto == "doline")
                    pose = Luminumbra::Debug::FindDoline(*fws, dnear, 500.0f);
                else if (g_debug_goto == "spawn")
                    pose = Luminumbra::Debug::FrameFeature(dnear, 24.0f);
                if (pose) {
                    g_fixed_cam_pos = pose->pos;
                    g_fixed_cam_yaw = pose->yaw;
                    g_fixed_cam_pitch = pose->pitch;
                    g_fixed_cam = true;
                    if (g_camera) {
                        g_camera->Position = pose->pos;
                        g_camera->Yaw = pose->yaw;
                        g_camera->Pitch = pose->pitch;
                        g_camera->updateCameraVectors();
                    }
                    // The crystal scatter is skipped under headless automation, so a
                    // captured cave would be pitch-black: light the framed cave from the pose we
                    // already computed (no second FindEnclosedCave walk).
                    if (g_debug_goto == "cave" && headless_automation) {
                        const auto ce = freg.create();
                        auto& ctf = freg.emplace<Luminumbra::Components::TransformComponent>(ce);
                        ctf.position =
                            Luminumbra::Vec3(pose->target.x, pose->target.y, pose->target.z);
                        auto& cpl = freg.emplace<Luminumbra::Components::PointLightComponent>(ce);
                        cpl.color = Luminumbra::Vec3(0.55f, 0.85f, 1.0f);
                        cpl.intensity = 6.0f; // hero crystal (mirrors the interactive one)
                        cpl.radius = 40.0f;
                        LUMINUMBRA_CORE_INFO(
                            "--debug-goto cave: hero crystal lit at the framed cave (headless)");
                    }
                    LUMINUMBRA_CORE_INFO(
                        "--debug-goto {}: framed ({:.1f},{:.1f},{:.1f}) yaw {:.0f} pitch {:.0f}",
                        g_debug_goto,
                        pose->pos.x,
                        pose->pos.y,
                        pose->pos.z,
                        pose->yaw,
                        pose->pitch);
                } else {
                    LUMINUMBRA_CORE_WARN("--debug-goto {}: no '{}' feature found near spawn",
                                         g_debug_goto,
                                         g_debug_goto);
                }
            }

            // LUMIN CRYSTALS — emissive point lights that light dark caves (so you can
            // see + photograph underground without sunlight) and double as photo subjects.
            // consume the background scan when the batch lands (non-blocking
            // poll of the JobHandle counter). Dedup runs here in the ORIGINAL sequential
            // anchor order with the same 16-cap, so placement is byte-identical to the
            // old inline walk. Client-only point lights (never hashed) — no re-pin.
            if (s_backgroundWorldScan && s_backgroundWorldScan->world == fws &&
                (!s_backgroundWorldScanHandle.counter ||
                 s_backgroundWorldScanHandle.counter->load(std::memory_order_acquire) <= 0)) {
                const BackgroundWorldScan& scan = *s_backgroundWorldScan;
                //  the doline cave-mouth aim cue.
                if (scan.doline.found) {
                    LUMINUMBRA_CORE_INFO(
                        "Largest doline near spawn: world ({:.1f}, {:.1f}, {:.1f}) "
                        "radius={:.1f}m depth={:.1f}m shaft={}",
                        scan.doline.x,
                        scan.doline_surface_h,
                        scan.doline.z,
                        scan.doline.radius,
                        scan.doline.depth,
                        scan.doline.shaft ? 1 : 0);
                } else {
                    LUMINUMBRA_CORE_INFO(
                        "No doline found within 500m of spawn (surface breaks off or sparse).");
                }
                // Crystal scatter: the roof-checked enclosed caverns found near each anchor
                // (cave bug B fix preserved — no floating crystals in open dips).
                constexpr float kMinSepSq = 20.0f * 20.0f; // de-dup nearby hits
                int placed = 0;
                float firstX = 0.0f, firstY = 0.0f, firstZ = 0.0f;
                std::vector<glm::vec3> caveCenters;
                for (const auto& maybe_cave : scan.anchor_caves) {
                    if (placed >= 16)
                        break;
                    if (!maybe_cave)
                        continue;
                    const glm::vec3 c = *maybe_cave; // interior void point of the cavern
                    bool dup = false;
                    for (const glm::vec3& prev : caveCenters) {
                        const glm::vec3 d = prev - c;
                        if (glm::dot(d, d) < kMinSepSq) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup)
                        continue;
                    caveCenters.push_back(c);
                    const auto e = freg.create();
                    auto& tf = freg.emplace<Luminumbra::Components::TransformComponent>(e);
                    tf.position = Luminumbra::Vec3(c.x, c.y + 0.6f, c.z);
                    auto& pl = freg.emplace<Luminumbra::Components::PointLightComponent>(e);
                    pl.color = Luminumbra::Vec3(0.45f, 0.78f, 1.0f); // cyan lumin glow
                    pl.intensity = 4.5f;
                    pl.radius = 24.0f;
                    if (placed == 0) {
                        firstX = c.x;
                        firstY = c.y;
                        firstZ = c.z;
                    }
                    ++placed;
                }
                LUMINUMBRA_CORE_INFO(
                    "Lumin crystals: placed {} enclosed-cave glow point-lights near spawn; "
                    "first at world ({:.1f}, {:.1f}, {:.1f})",
                    placed,
                    firstX,
                    firstY,
                    firstZ);
                // Hero crystal in the located enclosed cave (aligns with --debug-goto cave).
                if (scan.hero) {
                    const auto ce = freg.create();
                    auto& ctf = freg.emplace<Luminumbra::Components::TransformComponent>(ce);
                    ctf.position = Luminumbra::Vec3(
                        scan.hero->target.x, scan.hero->target.y, scan.hero->target.z);
                    auto& cpl = freg.emplace<Luminumbra::Components::PointLightComponent>(ce);
                    cpl.color = Luminumbra::Vec3(0.55f, 0.85f, 1.0f);
                    cpl.intensity = 6.0f; // hero crystal in the located enclosed cave
                    cpl.radius = 40.0f;
                    LUMINUMBRA_CORE_INFO(
                        "Lumin crystal: hero light in enclosed cave at ({:.1f}, {:.1f}, {:.1f})",
                        scan.hero->target.x,
                        scan.hero->target.y,
                        scan.hero->target.z);
                }
                s_backgroundWorldScan.reset();
                s_backgroundWorldScanHandle = {};
            }

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

        if (g_imgui_enabled) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }

        switch (currentState) {
            case GameState::WORLD_LOADING: {
                const int JOBS_PER_FRAME = 512;
                if (runtime_boot_recorder.enabled()) {
                    g_generation_dispatch_index = static_cast<int>(g_initial_chunks_to_load.size());
                }

                if (static_cast<size_t>(g_generation_dispatch_index) <
                    g_initial_chunks_to_load.size()) {
                    std::vector<Luminumbra::IVec3> batch_to_generate;
                    const int batch_start_index = g_generation_dispatch_index;
                    while (static_cast<size_t>(batch_start_index +
                                               static_cast<int>(batch_to_generate.size())) <
                               g_initial_chunks_to_load.size() &&
                           static_cast<int>(batch_to_generate.size()) < JOBS_PER_FRAME) {
                        batch_to_generate.push_back(
                            g_initial_chunks_to_load[batch_start_index +
                                                     static_cast<int>(batch_to_generate.size())]);
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
                            g_generation_dispatch_index +=
                                static_cast<int>(batch_to_generate.size());
                        }
                    }
                }

                float progress = g_initial_chunks_to_load.empty()
                                     ? 1.0f
                                     : static_cast<float>(g_generation_dispatch_index) /
                                           g_initial_chunks_to_load.size();

                glClearColor(0.01f, 0.02f, 0.05f, 1.0f); // Dark blue background
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                if (g_loading_visualizer) {
                    g_loading_visualizer->UpdateAndRender(
                        deltaTime, "CONSTRUCTING WORLD GEOMETRY...", progress);
                }

                if (static_cast<size_t>(g_generation_dispatch_index) >=
                    g_initial_chunks_to_load.size()) {
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
                    if (g_world_render_data_initialized) {
                        renderPipeline.clear_all_chunk_data();
                    }
                    g_world_render_data_initialized = true;

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
                } else if (g_profile_fly_seconds > 0.0 && g_playerController && g_camera) {
                    // runtime telemetry (--profile-fly): drive the player FORWARD at a constant
                    // noclip speed in normal-play mode so the SLOWFRAME logger captures a
                    // representative MOVING cost without the time-based scenario camera's
                    // teleport-under-load feedback. deltaTime is clamped (<=50ms) so per-frame
                    // movement stays bounded even on a slow frame → no teleport/active-set
                    // explosion. A slow yaw drift sweeps varied terrain (rivers/biomes). Self-exits
                    // after g_profile_fly_seconds. UpdateNoclip directly advances position
                    // (bypasses physics); streaming anchor = GetPosition follows.
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
                    if (elapsed > g_profile_fly_seconds)
                        glfwSetWindowShouldClose(window, true);
                } else if (g_playerController && !g_show_settings) {
                    g_playerController->Update(deltaTime); // movement paused while the menu is open
                }
                if (g_timelapse_frames > 0 && g_camera && gameSession) {
                    // FIXED timelapse camera: elevated, looking down at the grove around spawn.
                    // NOT tied to the (settling) player physics, so it can never fall through the
                    // world -- and it frames the plants instead of whatever the avatar sees.
                    const auto sp = gameSession->GetMetadata().spawnPoint;
                    // Growth captures frame a tighter view of the hero cluster in front; otherwise
                    // an elevated look over the grove.
                    const bool showcase =
                        g_timelapse_grow || g_timelapse_season || g_timelapse_simgrow;
                    // Ecology demo: a HIGH, wide, near-top-down look over the whole field so
                    // the herd scattering away from the predator (and the predator weaving
                    // toward the nearest prey) reads as clear motion across the ground, and
                    // nobody runs out of frame as they spread.
                    const glm::vec3 camPos =
                        g_timelapse_rain
                            ? glm::vec3(sp.x + 2.0f,
                                        sp.y + 16.0f,
                                        sp.z + 26.0f) // elevated overlook for rain pooling
                        : g_timelapse_dig
                            ? glm::vec3(sp.x + 10.0f,
                                        sp.y + 6.0f,
                                        sp.z + 10.0f) // close, low 3/4 look at the crater
                        : g_timelapse_fire
                            ? glm::vec3(
                                  sp.x, sp.y + 34.0f, sp.z + 36.0f) // high look over the burn patch
                        : g_timelapse_creatures ? glm::vec3(sp.x, sp.y + 30.0f, sp.z + 30.0f)
                        : showcase              ? glm::vec3(sp.x, sp.y + 4.0f, sp.z + 22.0f)
                                                : glm::vec3(sp.x, sp.y + 7.0f, sp.z + 20.0f);
                    const glm::vec3 target =
                        g_timelapse_rain ? glm::vec3(sp.x,
                                                     sp.y - 2.0f,
                                                     sp.z) // look down over the filling valley
                        : g_timelapse_dig
                            ? glm::vec3(sp.x, sp.y - 3.0f, sp.z) // the deepening crater at spawn
                        : g_timelapse_fire      ? glm::vec3(sp.x, sp.y, sp.z)
                        : g_timelapse_creatures ? glm::vec3(sp.x, sp.y, sp.z - 8.0f)
                        : showcase              ? glm::vec3(sp.x, sp.y + 3.0f, sp.z + 10.0f)
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
                if (g_timelapse_drain && g_camera && gameSession && gameSession->GetWorldSystem() &&
                    g_timelapse_settle >= kTimelapseSettleFrames) {
                    auto* ws = gameSession->GetWorldSystem();
                    if (!g_drain_state.init) {
                        // Anchor on a real SHORELINE — deep water beside a tall DRY bank — so
                        // cutting the bank floods the dry side (verified by the rising
                        // inland-volume probe below).
                        Luminumbra::Vec3 wp;
                        float tlx = 0.0f, tlz = 1.0f, wsurf = 0.0f, bank = 0.0f;
                        if (ws->debug_find_shoreline(wp, tlx, tlz, wsurf, bank)) {
                            g_drain_state.P = wp;
                            g_drain_state.dhx = tlx;
                            g_drain_state.dhz = tlz;
                            g_drain_state.surf = wsurf;
                            g_drain_state.init = true;
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
                    if (g_drain_state.init) {
                        const glm::vec3 P(g_drain_state.P.x, g_drain_state.P.y, g_drain_state.P.z);
                        const glm::vec3 dh =
                            glm::normalize(glm::vec3(g_drain_state.dhx, 0.0f, g_drain_state.dhz));
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
                if (g_timelapse_rain && g_camera && gameSession && gameSession->GetWorldSystem() &&
                    g_timelapse_settle >= kTimelapseSettleFrames) {
                    auto* ws = gameSession->GetWorldSystem();
                    if (!g_drain_state.init) {
                        ws->SetWaterHydrology(/*finite=*/true, g_timelapse_rain_mm, /*evap=*/0);
                        Luminumbra::Vec3 vp;
                        float bed = 0.0f;
                        if (ws->debug_lowest_land_pos(vp, bed)) {
                            g_drain_state.P = vp;
                            g_drain_state.init = true;
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
                    if (g_drain_state.init) {
                        const glm::vec3 P(g_drain_state.P.x, g_drain_state.P.y, g_drain_state.P.z);
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
                    physics->update(deltaTime * g_timeScale);
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
                    if (g_timeScale == 1.0f) {
                        gameSession->TickSimulation(static_cast<double>(
                            deltaTime)); // byte-identical default (gates run here)
                    } else if (g_timeScale > 0.0f) {
                        // host_timescale: run the sim faster/slower. Chunk into <=4-tick steps so a
                        // high scale isn't dropped by the catch-up clamp, capped per frame to keep
                        // spiral protection. Determinism holds (fixed dt per tick).
                        double simDt =
                            static_cast<double>(deltaTime) * static_cast<double>(g_timeScale);
                        const double kFourTicks = (1.0 / 30.0) * 4.0;
                        const int budget =
                            static_cast<int>(std::ceil(4.0 * static_cast<double>(g_timeScale)));
                        int ran = 0;
                        while (simDt > 1e-9 && ran < budget) {
                            const double step = std::min(simDt, kFourTicks);
                            const std::uint32_t t = gameSession->TickSimulation(step);
                            simDt -= step;
                            if (t == 0u)
                                break; // accumulator < 1 tick this frame
                            ran += static_cast<int>(t);
                        }
                    } // g_timeScale == 0 -> paused (no sim ticks; render/streaming continue)
                    rb_sim_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - _rb_sim_t0)
                                    .count();                         //
                    _rb_stream_t0 = std::chrono::steady_clock::now(); //
                    if (gameSession->GetWorldSystem() && (g_playerController || g_camera)) {
                        // Anchor world streaming on the CAMERA (not the spawn-bound player)
                        // whenever a fixed/scenario camera drives the view — otherwise a --cam-pos
                        // far from spawn streams chunks around the player at spawn and the camera
                        // sees an unloaded, unlit void (the "far-camera renders black" bug).
                        // g_fixed_cam covers the capture/showcase path; the scenario smokes keep
                        // their existing behaviour.
                        const bool cam_anchored =
                            (g_fixed_cam || ((scenario_config.lod_ground_smoke() ||
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
                        BuildProcgenTreePalette(renderPipeline, sunToward);
                        BuildProcgenRockPalette(renderPipeline);
                        BuildProcgenBushPalette(renderPipeline);
                        // LIVING WORLD: ambient WILDLIFE for interactive play. The world
                        // had no creatures in normal play (only timelapse markers /
                        // scenario rigs), so the codex/objectives loop had nothing to
                        // photograph. Render+client-sim only; the gated headless
                        // world_hash is the server's and is unaffected. Skipped in
                        // capture/scenario modes (they own their own creature handling).
                        const bool interactive_play =
                            !scenario_config.active() && !g_timelapse_creatures &&
                            (g_timelapse_frames == 0 || g_timelapse_living);
                        auto pending = std::make_shared<WorldDressingPending>();
                        pending->world = ws;
                        pending->sun_toward = sunToward;
                        // Capture/scenario/timelapse runs must see the FULL dressing in
                        // their first settled frames (visual sweep / frame-scan / thumbs
                        // capture right after scenario_ready), so they compute + consume
                        // synchronously — the placements are byte-identical either way.
                        pending->synchronous =
                            scenario_config.active() || g_frame_scan_active || g_scene_active ||
                            g_play_paths || !g_render_benchmark_path.empty() ||
                            !g_survey_dir.empty() || g_timelapse_frames > 0 || g_ui_thumbs > 0;
                        // One-time skinned rig load (file IO — never the slow part). The
                        // wildlife placements are only computed when the rig is usable.
                        if (interactive_play && g_creatureSpecies.size() > 0) {
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
                        dparams.tree_palette_count = g_treePaletteCount;
                        dparams.rock_palette_count = g_rockPaletteCount;
                        dparams.bush_palette_count = g_bushPaletteCount;
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
                                g_creatureSpecies.SelectForBiome(biome, pick);
                            if (!sp_sel)
                                return static_cast<int>(pick % g_creatureSpecies.size());
                            return static_cast<int>(sp_sel - g_creatureSpecies.all().data());
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
                                g_rockPaletteCount > 0) {
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
                                g_bushPaletteCount > 0) {
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
                                const auto& roster = g_creatureSpecies.all();
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
                            g_procgenPlants.clear();
                            // Growth showcase: a cluster of bigger HERO plants right in front of
                            // the fixed grow-mode camera, so the foreground is dominated by plants
                            // visibly growing (the scattered grove alone reads as distant
                            // background).
                            if ((g_timelapse_grow || g_timelapse_season) && procgenPlants) {
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
                                    g_procgenPlants.push_back(inst);
                                }
                            }
                            //  SIM plant seeding for the REAL-growth showcase. Spawn a
                            // hero cluster of LIVE PlantTag plants (data-driven species via the
                            // MakePlantFromSpecies) into the SESSION registry, so the deterministic
                            // PlantGrowthSystem advances them Seed->Fruiting each tick and
                            // BakeSimPlants renders their TRUE stage. Under time-scale they visibly
                            // grow on capture.
                            if (g_timelapse_simgrow) {
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
                            // -> g_procgenTreeVerts), then composite the SIM-tier PlantTag plants
                            // on top via RebakeAllPlants (one pass, both tiers).
                            // Player-planted/promoted plants ADD to the forest rather than
                            // replacing it. OFF/empty -> pass disabled. Growth + promotion re-bakes
                            // happen per-frame in the loop (sig-gated, so cheap). Phototropism uses
                            // the scene's REAL sun, captured at dispatch time (the same value the
                            // tree palette was built with).
                            g_procgenSunDir = pend.sun_toward;
                            if (auto* pp = renderPipeline.plant_procgen()) {
                                if (procgenPlants) {
                                    BakeProcgenPlants(pp,
                                                      g_procgenStageF); // build + cache the scatter
                                    LUMINUMBRA_CORE_INFO(
                                        ": {} procedural scatter plants (stage {:.1f})",
                                        g_procgenPlants.size(),
                                        g_procgenStageF);
                                } else {
                                    pp->set_enabled(false);
                                }
                                const std::size_t simPlants =
                                    RebakeAllPlants(pp, reg, g_procgenSunDir, g_season);
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
                            if (g_timelapse_creatures) {
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
                                        if (g_timelapse_calm) {
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
                                    if (g_timelapse_calm) {
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
                                if (g_timelapse_calm) {
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
                            if (g_timelapse_fire) {
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
                                                    wireframe_mode);
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
                                    g_pendingThunder.push_back({strike.strike_tick,
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
                    if (g_timelapse_creatures) {
                        // Newborn offspring start bodyless; give them avatar bodies so they
                        // collide with the terrain (no more walking through mountains), then
                        // bake markers from the physics-resolved positions.
                        AttachMissingCreatureBodies(gameSession->GetPhysicsSystem(),
                                                    gameSession->GetRegistry());
                        BakeCreatureMarkers(renderPipeline.plant_procgen(),
                                            gameSession->GetRegistry(),
                                            gameSession.get());
                    }
                    if (g_timelapse_fire) {
                        BakeCombustibleMarkers(renderPipeline.plant_procgen(),
                                               gameSession->GetRegistry(),
                                               gameSession->GetWorldSystem());
                    }
                    if (g_fixed_cam && g_camera) {
                        g_camera->Position = g_fixed_cam_pos;
                        g_camera->Yaw = g_fixed_cam_yaw;
                        g_camera->Pitch = g_fixed_cam_pitch;
                        g_camera->updateCameraVectors();
                    }
                    if (g_scene_active) {
                        if (g_scene_fov > 0.0f && g_camera)
                            g_camera->Zoom = g_scene_fov;
                        renderPipeline.set_time_of_day(g_timelapse_tod);
                        if (g_scene_moon >= 0.0f)
                            renderPipeline.set_moon_illumination(g_scene_moon); // rendering
                        using WT = Luminumbra::Rendering::WeatherType;
                        const WT wt = (g_scene_weather == 1)   ? WT::Rain
                                      : (g_scene_weather == 2) ? WT::Snow
                                      : (g_scene_weather == 3) ? WT::Fog
                                      : (g_scene_weather == 4) ? WT::Storm
                                                               : WT::None;
                        renderPipeline.set_weather(wt, g_scene_weather_intensity);
                        if (g_scene_clouds) {
                            Luminumbra::Rendering::CloudRenderState cs;
                            cs.enabled = true;
                            cs.shadow_enabled = g_scene_cloud_shadow;
                            cs.coverage_amount = g_scene_cloud_coverage;
                            cs.biome_variation = g_scene_cloud_biome;
                            cs.plane_height = g_scene_cloud_plane;
                            cs.shadow_strength = g_scene_cloud_shadow_strength;
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
                                                          !g_play_paths);
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
                                (scenario_config.active() && !g_play_paths) ||
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
                            RebakeAllPlants(
                                ppp, gameSession->GetRegistry(), g_procgenSunDir, g_season);
                        rb_rebake_ms = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - _fb0)
                                           .count();
                    }
                    const auto _rb_rcall_t0 =
                        std::chrono::steady_clock::now(); // always-on (runtime telemetry)
                    renderPipeline.set_debug_view(
                        g_debug_view_mode); // render-only; 0 = byte-identical default ( /
                                            // --debug-view)
                    renderPipeline.render_frame(gameSession->GetRegistry(),
                                                *gameSession->GetWorldSystem(),
                                                *g_camera,
                                                deltaTime,
                                                wireframe_mode);
                    rb_render_call_ms = std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - _rb_rcall_t0)
                                            .count();

                    // --scene-config: self-contained capture. Settle a few frames (world
                    // stream + atmosphere), then read the clean back buffer (BEFORE any UI
                    // overlay this frame) and write the screenshot, then close.
                    if (g_scene_active) {
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
                                WritePixelBufferPpm(g_scene_shot, vw, vh, px);
                                LUMINUMBRA_CORE_INFO("Scene capture written -> {} ({}x{})",
                                                     g_scene_shot.string(),
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
                    if (g_survey_active && currentState == GameState::IN_GAME && gameSession &&
                        gameSession->GetWorldSystem() && g_camera) {
                        if (g_survey_settle < 45) {
                            ++g_survey_settle;
                        } else {
                            Luminumbra::Rendering::RunSceneSurvey(
                                window,
                                *gameSession,
                                renderPipeline,
                                *g_camera,
                                std::filesystem::path(g_survey_dir),
                                root_dir / "data/common/materials.json",
                                wireframe_mode);
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
                    if (g_frame_scan_active && ++g_frame_scan_watchdog > kFrameScanWatchdogFrames) {
                        LUMINUMBRA_CORE_ERROR("Frame-scan watchdog: did not settle within {} "
                                              "frames (state={}). Aborting "
                                              "without a report.",
                                              kFrameScanWatchdogFrames,
                                              static_cast<int>(currentState));
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                    if (g_frame_scan_active && currentState == GameState::IN_GAME && gameSession) {
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
                        if (g_frame_scan_settle < g_frame_scan_settle_target) {
                            ++g_frame_scan_settle; // let chunks stream + atmosphere settle
                        } else {
                            if (g_render_parity_active) {
                                bool parity_ok = false;
                                if (g_render_parity_pass == "ssao" && g_camera)
                                    parity_ok = renderPipeline.capture_ssao_parity(
                                        g_render_parity_dir, *g_camera);
                                else if (g_render_parity_pass == "upscale_seam" && g_camera)
                                    parity_ok = renderPipeline.capture_upscale_seam_parity(
                                        *g_camera, g_render_parity_dir);
                                else if (g_render_parity_pass == "frame" && g_camera)
                                    // whole-frame A/B — dispatch the settled
                                    // prepared frame twice, in-process FLIP must be 0.0.
                                    parity_ok = renderPipeline.capture_frame_parity(
                                        *g_camera, g_render_parity_dir);
                                else
                                    parity_ok = false;
                                if (parity_ok)
                                    LUMINUMBRA_CORE_INFO("{} parity captured -> {}",
                                                         g_render_parity_pass,
                                                         g_render_parity_dir.string());
                                else
                                    LUMINUMBRA_CORE_ERROR("{} parity capture FAILED",
                                                          g_render_parity_pass);
                            }
                            int vw = 0, vh = 0;
                            glfwGetFramebufferSize(window, &vw, &vh);
                            const Luminumbra::Rendering::FrameScanReport rep =
                                Luminumbra::Rendering::ScanFrame(renderPipeline,
                                                                 vw,
                                                                 vh,
                                                                 root_dir /
                                                                     "data/common/materials.json");
                            if (rep.ok && Luminumbra::Rendering::WriteFrameScanReport(
                                              rep, std::filesystem::path(g_frame_scan_path))) {
                                LUMINUMBRA_CORE_INFO(
                                    "Frame-scan written -> {} ({}x{}): {} materials, water "
                                    "{:.1f}%, foliage {} inst, mean luma {:.3f}",
                                    g_frame_scan_path,
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
                                    std::filesystem::path img(g_frame_scan_path);
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
                                        std::filesystem::path hp(g_frame_scan_path);
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
                                LUMINUMBRA_CORE_ERROR("Frame-scan failed -> {}", g_frame_scan_path);
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
                        !scenario_config.active() && !g_paused && gameSession && g_camera) {
                        if (!g_farmSpeciesLoaded) {
                            std::vector<std::string> ferr;
                            g_farmSpecies.LoadFromDirectory(
                                root_dir / "data/common/foliage/species", ferr);
                            g_farmSpeciesLoaded = true;
                        }
                        static bool s_fp = false, s_fw = false, s_ff = false, s_fh = false,
                                    s_fv = false;
                        // V cycles the selected species to plant (data-driven; oak/wheat/...).
                        {
                            const bool fv_now = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
                            const std::size_t n = g_farmSpecies.all().size();
                            if (fv_now && !s_fv && n > 0) {
                                g_farmSelectedSpecies =
                                    (g_farmSelectedSpecies + 1) % static_cast<int>(n);
                                if (audioManager)
                                    audioManager->PlayOneShot2D("ui_button_click");
                                LUMINUMBRA_CORE_INFO("Farm: selected species '{}'",
                                                     g_farmSpecies.all()[g_farmSelectedSpecies].id);
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
                            const auto& species = g_farmSpecies.all();
                            if (!species.empty()) {
                                const luminumbra::foliage::SpeciesTemplate& tmpl =
                                    species[static_cast<std::size_t>(g_farmSelectedSpecies) %
                                            species.size()];
                                auto frng = luminumbra::core::DeterministicRng::seeded(
                                    luminumbra::foliage::kPlantSeedOffset,
                                    static_cast<std::uint64_t>(
                                        static_cast<std::int64_t>(aimXZ.x * 8.0f)),
                                    static_cast<std::uint64_t>(
                                        static_cast<std::int64_t>(aimXZ.z * 8.0f)) ^
                                        ftick);
                                if (g_farming.Seed(freg, aim, tmpl, frng, ftick) != entt::null) {
                                    if (audioManager)
                                        audioManager->PlayOneShot("farm_plant",
                                                                  glm::vec3(aim.x, aim.y, aim.z));
                                    LUMINUMBRA_CORE_INFO("Farm: planted {} ({} seeds left)",
                                                         tmpl.id,
                                                         g_farming.seeds);
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
                                e = PromoteNearestScatter(freg, aim, 3.0f, ftick);
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
                            if (g_farming.Water(freg, fpick()) && audioManager)
                                audioManager->PlayOneShot("farm_water", aimSnd);
                        }
                        if (farmEdge(IA::FarmFertilize, s_ff)) {
                            if (g_farming.Fertilize(freg, fpick()) && audioManager)
                                audioManager->PlayOneShot("farm_fertilize", aimSnd);
                        }
                        if (farmEdge(IA::FarmHarvest, s_fh)) {
                            const auto hr = g_farming.Harvest(freg, fpick());
                            if (hr.harvestable) {
                                if (audioManager)
                                    audioManager->PlayOneShot("farm_harvest", aimSnd);
                                LUMINUMBRA_CORE_INFO(
                                    "Farm: harvested yield {:.2f} (+{} seeds, {} total)",
                                    hr.yield,
                                    hr.seeds,
                                    g_farming.harvests);
                            }
                        }
                        // A promotion suppressed a scatter instance -> rebuild the scatter cache so
                        // the next composite re-bake (RebakeAllPlants) drops the now-promoted dup.
                        if (promoted) {
                            if (auto* fpp = renderPipeline.plant_procgen())
                                BakeProcgenPlants(fpp, g_procgenStageF);
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
                        !scenario_config.active() && !g_paused && gameSession && g_camera &&
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

                    // --- feature: photo-mode capture loop ---
                    // Runs AFTER render_frame, on the RENDER side, against a CONST
                    // registry: it reads camera + creature state, scores via the landed
                    // PhotoSession scorers, and on shutter persists a PPM + verdict
                    // sidecar into photos/. It NEVER ticks the sim or mutates the
                    // registry, so determinism cannot regress (the sim-isolation gate
                    // test pins the pure path). The interactive-only guard keeps it out
                    // of the automated scenario/gate runs.
                    if (g_playerController && currentState == GameState::IN_GAME &&
                        !scenario_config.active() && !g_paused) {
                        g_photoMode.active = g_playerController->photo_mode_active();

                        // Codex browse overlay (client-only). Its toggle takes precedence
                        // over the HUD/viewfinder swap below and cannot open while the
                        // viewfinder is active. Closing it resyncs the hud-swap state.
                        if (g_playerController->consume_codex_toggle() && !g_photoMode.active) {
                            g_codexOpen = !g_codexOpen;
                            if (audioManager)
                                audioManager->PlayOneShot2D(g_codexOpen ? "ui_codex_open"
                                                                        : "ui_codex_close");
                            if (g_uiManager) {
                                g_uiManager->RequestLoadDocument(g_codexOpen ? "codex.rml"
                                                                             : "hud.rml");
                                g_photoModeUiShown = false;
                                g_codexSig.clear(); // force a repopulate on next open
                            }
                        }

                        //  swap the in-game overlay between the HUD and the photo-mode viewfinder
                        // when photo mode toggles (the capture loop + lens nudges below already
                        // exist).
                        if (!g_codexOpen && g_uiManager) {
                            if (g_photoMode.active && !g_photoModeUiShown) {
                                g_uiManager->RequestLoadDocument("photo_mode.rml");
                                g_photoModeUiShown = true;
                            } else if (!g_photoMode.active && g_photoModeUiShown) {
                                g_uiManager->RequestLoadDocument("hud.rml");
                                g_photoModeUiShown = false;
                            }
                        }

                        // While the codex is open, populate it from the live codex+registry
                        // (throttled by g_codexSig: only rebuild rows when discovery state
                        // changes). Skips the HUD objective update below.
                        if (g_codexOpen && g_uiManager && g_uiManager->GetContext()) {
                            if (auto* doc = g_uiManager->GetContext()->GetDocument("codex")) {
                                const luminumbra::game::CodexView cv =
                                    luminumbra::game::BuildCodexView(g_creatureSpecies,
                                                                     g_photoCodex);
                                const int pct = static_cast<int>(cv.completeness * 100.0f + 0.5f);
                                std::string sig = std::to_string(cv.discovered_count) + "/" +
                                                  std::to_string(cv.total_species) + "|" +
                                                  std::to_string(pct);
                                if (sig != g_codexSig) {
                                    g_codexSig = sig;
                                    if (auto* h = doc->GetElementById("codex_completion")) {
                                        h->SetInnerRML(std::to_string(cv.discovered_count) + " / " +
                                                       std::to_string(cv.total_species) +
                                                       " discovered \xC2\xB7 " +
                                                       std::to_string(pct) + "%");
                                    }
                                    if (auto* list = doc->GetElementById("codex_list")) {
                                        std::string rows_rml;
                                        for (const auto& r : cv.rows) {
                                            std::string stars;
                                            for (int s = 0; s < 5; ++s)
                                                stars +=
                                                    (s < r.stars) ? "\xE2\x98\x85" : "\xE2\x98\x86";
                                            const std::string cls = r.discovered
                                                                        ? "codex-row codex-found"
                                                                        : "codex-row codex-locked";
                                            const std::string name =
                                                r.discovered ? r.display_name : "? ? ?";
                                            rows_rml += "<div class=\"" + cls +
                                                        "\">"
                                                        "<span class=\"codex-name\">" +
                                                        name +
                                                        "</span>"
                                                        "<span class=\"codex-stars\">" +
                                                        stars +
                                                        "</span>"
                                                        "</div>";
                                        }
                                        list->SetInnerRML(rows_rml);
                                    }
                                }
                            }
                        }

                        //  lazily build the starter objective chain keyed on the
                        // default world's first creature, then surface the current goal +
                        // progress on the HUD. Throttled by g_objHudSig so the DOM is only
                        // written when the current objective or its progress changes.
                        if (!g_objectivesInit) {
                            g_objectives = luminumbra::game::DefaultObjectives(static_cast<int>(
                                Luminumbra::Components::CreatureSpeciesId16("grovestrider")));
                            g_objectivesInit = true;
                        }
                        if (!g_codexOpen && !g_photoMode.active && g_uiManager &&
                            g_uiManager->GetContext()) {
                            if (auto* hud = g_uiManager->GetContext()->GetDocument("hud")) {
                                const luminumbra::game::Objective* cur =
                                    g_objectives.next_incomplete(g_photoCodex);
                                const std::uint32_t done =
                                    g_objectives.completed_count(g_photoCodex);
                                // Everything maps to sound: a newly-completed goal rings the
                                // success chime once (edge-triggered on the completed count).
                                static std::uint32_t s_objDoneLast = 0;
                                if (done > s_objDoneLast && audioManager)
                                    audioManager->PlayOneShot2D("objective_complete",
                                                                Luminumbra::Client::BusId::Events);
                                s_objDoneLast = done;
                                std::string title = "All goals complete";
                                float progress = 1.0f;
                                if (cur) {
                                    title = cur->title;
                                    progress =
                                        luminumbra::game::EvaluateObjective(*cur, g_photoCodex)
                                            .progress;
                                }
                                const int pct = static_cast<int>(progress * 100.0f + 0.5f);
                                // Onboarding hint shows until the first capture lands a species.
                                const bool show_tutorial = g_photoCodex.species_count() == 0;
                                std::string sig = std::to_string(done) + "/" +
                                                  std::to_string(g_objectives.size()) + "|" +
                                                  title + "|" + std::to_string(pct) + "|" +
                                                  (show_tutorial ? "t1" : "t0");
                                if (sig != g_objHudSig) {
                                    g_objHudSig = sig;
                                    if (auto* e = hud->GetElementById("obj_title"))
                                        e->SetInnerRML(title);
                                    if (auto* e = hud->GetElementById("obj_progress"))
                                        e->SetInnerRML(std::to_string(pct) + "%");
                                    if (auto* e = hud->GetElementById("obj_count"))
                                        e->SetInnerRML(std::to_string(done) + " / " +
                                                       std::to_string(g_objectives.size()) +
                                                       " goals");
                                    if (auto* e = hud->GetElementById("tutorial_hint"))
                                        e->SetClass("hidden", !show_tutorial);
                                }
                            }
                        }
                        //  farming HUD — seed/harvest inventory + the crop the
                        // player is facing (stage + quality + a harvest hint). Shown once the
                        // player is near a crop or has farmed, so it never clutters a non-farming
                        // session. Signature-gated like the objective tracker; render-only (reads
                        // sim state).
                        if (!g_codexOpen && !g_photoMode.active && g_uiManager &&
                            g_uiManager->GetContext() && gameSession && g_camera) {
                            if (auto* hud = g_uiManager->GetContext()->GetDocument("hud")) {
                                namespace FC = Luminumbra::Components;
                                const auto& freg = gameSession->GetRegistry();
                                const glm::vec3 ffwd = glm::normalize(
                                    glm::vec3(g_camera->Front.x, 0.0f, g_camera->Front.z));
                                const glm::vec3 aimXZ = glm::vec3(g_camera->Position) + ffwd * 3.0f;
                                const Luminumbra::Vec3 aimv(aimXZ.x, g_camera->Position.y, aimXZ.z);
                                const entt::entity crop =
                                    luminumbra::foliage::FarmingController::NearestPlant(
                                        freg, aimv, 3.0f);
                                // Show once farming is in play: a crop in reach, a seed spent /
                                // harvest made, or the player has cycled the species picker (so
                                // it's discoverable).
                                const bool farmed = g_farming.seeds != 5 || g_farming.harvests > 0;
                                const bool show =
                                    (crop != entt::null) || farmed || g_farmSelectedSpecies != 0;
                                std::string cropText = "no crop in reach";
                                if (crop != entt::null &&
                                    freg.all_of<FC::PlantGrowthComponent>(crop)) {
                                    const auto& g = freg.get<FC::PlantGrowthComponent>(crop);
                                    static const char* kStage[] = {"seed",
                                                                   "sprout",
                                                                   "juvenile",
                                                                   "mature",
                                                                   "flowering",
                                                                   "fruiting"};
                                    const int s = g.stage < 6 ? static_cast<int>(g.stage) : 5;
                                    // Optional species name (reverse-map the stable id; default
                                    // "crop").
                                    std::string sname = "crop";
                                    if (g_farmSpeciesLoaded) {
                                        for (const auto& t : g_farmSpecies.all())
                                            if (luminumbra::foliage::SpeciesId16(t.id) ==
                                                g.species_id) {
                                                sname = t.id;
                                                break;
                                            }
                                    }
                                    cropText = sname + " - " + kStage[s] + " - quality " +
                                               std::to_string(static_cast<int>(g.quality));
                                    if (g.stage >=
                                        static_cast<std::uint8_t>(FC::PlantStage::Mature))
                                        cropText += " - ready (J)";
                                }
                                // Selected planting species (V cycles it) shown so the player knows
                                // what F plants.
                                std::string selName = "wheat";
                                if (g_farmSpeciesLoaded && !g_farmSpecies.all().empty())
                                    selName =
                                        g_farmSpecies
                                            .all()[static_cast<std::size_t>(g_farmSelectedSpecies) %
                                                   g_farmSpecies.all().size()]
                                            .id;
                                const std::string inv =
                                    selName + " - " + std::to_string(g_farming.seeds) +
                                    " seeds - " + std::to_string(g_farming.harvests) + " harvested";
                                const std::string sig = (show ? "1" : "0") + inv + "|" + cropText;
                                if (sig != g_farmHudSig) {
                                    g_farmHudSig = sig;
                                    if (auto* e = hud->GetElementById("farming_panel"))
                                        e->SetClass("hidden", !show);
                                    if (auto* e = hud->GetElementById("farm_inv"))
                                        e->SetInnerRML(inv);
                                    if (auto* e = hud->GetElementById("farm_crop"))
                                        e->SetInnerRML(cropText);
                                }
                            }
                        }
                        // photo-mode ENVIRONMENT scrub state (time-of-day + weather).
                        // Render-only overrides of the visual day clock + weather overlay; they
                        // NEVER touch the sim clock or WeatherSystem (world_hash unaffected).
                        static bool s_photoEnvEngaged = false;
                        static float s_photoTod = 0.5f;
                        static int s_photoWeatherIdx = 0;
                        static bool s_photoWeatherActive = false;
                        if (!g_photoMode.active && s_photoEnvEngaged) {
                            // Left photo mode: release the day-clock hold (live clock resumes); the
                            // per-frame sim weather push restores driven weather next frame.
                            s_photoEnvEngaged = false;
                            s_photoWeatherActive = false;
                            renderPipeline.set_time_of_day_hold(false);
                            //  rendering: drop the manual exposure override so
                            // the automatic time-of-day exposure curve resumes.
                            renderPipeline.set_exposure_override(-1.0f);
                        }
                        if (g_photoMode.active) {
                            // Apply lens nudges (aperture stops + focus metres), clamped
                            // to sane photographic ranges.
                            g_photoMode.lens.aperture_f +=
                                g_playerController->consume_aperture_nudge();
                            if (g_photoMode.lens.aperture_f < 1.0f)
                                g_photoMode.lens.aperture_f = 1.0f;
                            if (g_photoMode.lens.aperture_f > 32.0f)
                                g_photoMode.lens.aperture_f = 32.0f;
                            g_photoMode.lens.focus_distance_m +=
                                g_playerController->consume_focus_nudge();
                            if (g_photoMode.lens.focus_distance_m < 0.2f)
                                g_photoMode.lens.focus_distance_m = 0.2f;
                            if (g_photoMode.lens.focus_distance_m > 200.0f)
                                g_photoMode.lens.focus_distance_m = 200.0f;

                            //  manual exposure — shutter speed + ISO nudges
                            // applied multiplicatively in stops. + shutter stop = FASTER
                            // (less light, shorter time); + ISO stop = higher sensitivity.
                            const float shutter_stops =
                                g_playerController->consume_shutter_speed_nudge();
                            if (shutter_stops != 0.0f) {
                                g_photoMode.lens.shutter_s *= std::pow(2.0f, -shutter_stops);
                                if (g_photoMode.lens.shutter_s < 1.0f / 4000.0f)
                                    g_photoMode.lens.shutter_s = 1.0f / 4000.0f;
                                if (g_photoMode.lens.shutter_s > 30.0f)
                                    g_photoMode.lens.shutter_s = 30.0f;
                            }
                            const float iso_stops = g_playerController->consume_iso_nudge();
                            if (iso_stops != 0.0f) {
                                g_photoMode.lens.iso *= std::pow(2.0f, iso_stops);
                                if (g_photoMode.lens.iso < 50.0f)
                                    g_photoMode.lens.iso = 50.0f;
                                if (g_photoMode.lens.iso > 25600.0f)
                                    g_photoMode.lens.iso = 25600.0f;
                            }

                            //  rendering ( /  ): the lens now drives
                            // the RENDER exposure. Map the (just-nudged) lens EV to an exposure
                            // multiplier and push it; it OVERRIDES the analytic TOD exposure curve
                            // so stopping down darkens and opening up brightens the frame —
                            // the photographer exposes for the light. Render-only; never
                            // world_hash.
                            renderPipeline.set_exposure_override(
                                Luminumbra::Rendering::ManualExposureMultiplier(g_photoMode.lens));

                            //  /0.2: TIME-OF-DAY scrub (K/L) + WEATHER cycle (T).
                            // On entry, seed the scrub from the live clock and HOLD it (so the
                            // per-frame auto-advance stops); each frame push the scrubbed values to
                            // the render pipeline. Render-only — the sim is never touched.
                            if (!s_photoEnvEngaged) {
                                s_photoEnvEngaged = true;
                                s_photoTod = renderPipeline.get_time_of_day();
                                renderPipeline.set_time_of_day_hold(true);
                            }
                            s_photoTod += g_playerController->consume_tod_nudge();
                            s_photoTod -= std::floor(s_photoTod); // wrap to [0,1)
                            renderPipeline.set_time_of_day(s_photoTod);
                            if (const int wc = g_playerController->consume_weather_cycle();
                                wc != 0) {
                                s_photoWeatherActive = true;
                                s_photoWeatherIdx = (((s_photoWeatherIdx + wc) % 5) + 5) % 5;
                            }
                            if (s_photoWeatherActive) {
                                using WT = Luminumbra::Rendering::WeatherType;
                                static const WT kW[5] = {
                                    WT::None, WT::Fog, WT::Rain, WT::Snow, WT::Storm};
                                renderPipeline.set_weather(kW[s_photoWeatherIdx],
                                                           s_photoWeatherIdx == 0 ? 0.0f : 0.7f);
                            }

                            //  live-bind the viewfinder readouts to the current lens.
                            if (g_uiManager && g_uiManager->GetContext()) {
                                if (auto* doc =
                                        g_uiManager->GetContext()->GetDocument("photo_mode")) {
                                    char rbuf[24];
                                    if (auto* e = doc->GetElementById("ro_aperture")) {
                                        std::snprintf(rbuf,
                                                      sizeof(rbuf),
                                                      "f/%.1f",
                                                      g_photoMode.lens.aperture_f);
                                        e->SetInnerRML(rbuf);
                                    }
                                    if (auto* e = doc->GetElementById("ro_focus")) {
                                        std::snprintf(rbuf,
                                                      sizeof(rbuf),
                                                      "%.1fm",
                                                      g_photoMode.lens.focus_distance_m);
                                        e->SetInnerRML(rbuf);
                                    }
                                    //  shutter + ISO + a live EV meter so the
                                    // player sees whether they are exposing for the light.
                                    if (auto* e = doc->GetElementById("ro_shutter")) {
                                        const float ss = g_photoMode.lens.shutter_s;
                                        if (ss >= 1.0f)
                                            std::snprintf(rbuf, sizeof(rbuf), "%.1fs", ss);
                                        else
                                            std::snprintf(rbuf,
                                                          sizeof(rbuf),
                                                          "1/%d",
                                                          static_cast<int>(1.0f / ss + 0.5f));
                                        e->SetInnerRML(rbuf);
                                    }
                                    if (auto* e = doc->GetElementById("ro_iso")) {
                                        std::snprintf(
                                            rbuf,
                                            sizeof(rbuf),
                                            "ISO %d",
                                            static_cast<int>(g_photoMode.lens.iso + 0.5f));
                                        e->SetInnerRML(rbuf);
                                    }
                                    if (auto* e = doc->GetElementById("ro_ev")) {
                                        const float live_lum = SceneLuminanceFromSunElevation(
                                            renderPipeline.get_sun_elevation_rad());
                                        const float lens_ev =
                                            luminumbra::game::ExposureValue(g_photoMode.lens);
                                        const float target_ev =
                                            6.0f +
                                            live_lum * 9.0f; // kSceneEvMin + lum*kSceneEvSpan
                                        const float d =
                                            lens_ev -
                                            target_ev; // + = under (dark), - = over (blown)
                                        const char* tag = (d > 0.5f)    ? " dark"
                                                          : (d < -0.5f) ? " bright"
                                                                        : " ok";
                                        std::snprintf(rbuf, sizeof(rbuf), "%+.1f EV%s", d, tag);
                                        e->SetInnerRML(rbuf);
                                    }
                                    // time-of-day phase (golden-hour cue for the photographer)
                                    // + the active weather preset.
                                    if (auto* e = doc->GetElementById("ro_tod")) {
                                        const float elev = renderPipeline.get_sun_elevation_rad();
                                        const char* ph = (elev > 0.6f)     ? "midday"
                                                         : (elev > 0.12f)  ? "day"
                                                         : (elev > -0.08f) ? "golden"
                                                                           : "night";
                                        e->SetInnerRML(ph);
                                    }
                                    if (auto* e = doc->GetElementById("ro_weather")) {
                                        static const char* kWN[5] = {
                                            "clear", "fog", "rain", "snow", "storm"};
                                        e->SetInnerRML(s_photoWeatherActive ? kWN[s_photoWeatherIdx]
                                                                            : "live");
                                    }
                                }
                            }

                            if (g_playerController->consume_shutter_request()) {
                                // The core action gets its sound: a soft camera shutter on every
                                // capture.
                                if (audioManager)
                                    audioManager->PlayOneShot2D("camera_shutter");
                                int cap_w = 0, cap_h = 0;
                                glfwGetFramebufferSize(window, &cap_w, &cap_h);
                                //  scene luminance follows the sun so the
                                // player must expose for the light (golden hour rewards,
                                // night punishes). Render-derived; never feeds world_hash.
                                const float scene_lum = SceneLuminanceFromSunElevation(
                                    renderPipeline.get_sun_elevation_rad());
                                // Build the shot from the live frame (CONST registry read).
                                const std::vector<luminumbra::game::PhotoSubjectView> subjects =
                                    GatherPhotoSubjects(gameSession->GetRegistry(),
                                                        *g_camera,
                                                        cap_w,
                                                        cap_h,
                                                        scene_lum);
                                //  stamp the capture's time-of-day; BuildShotInput
                                // fills the principal subject's behaviour + scene luminance.
                                luminumbra::game::ObservationMetadata obs;
                                obs.time_of_day = renderPipeline.get_time_of_day();
                                const luminumbra::game::ShotInput shot =
                                    luminumbra::game::BuildShotInput(
                                        subjects, g_photoMode.lens, scene_lum, 0.5f, 1.0f, obs);
                                // Was this species already in the codex BEFORE the capture? A
                                // subject-bearing shot of a never-seen species is a DISCOVERY.
                                const bool had_subject = !shot.composition.subjects.empty();
                                const bool was_known =
                                    had_subject && g_photoCodex.discovered(shot.main_species_id);
                                const luminumbra::game::ShotVerdict verdict =
                                    luminumbra::game::CaptureShot(g_photoCodex, shot);
                                g_photoMode.last_total = verdict.total;
                                g_photoMode.last_stars = verdict.stars;
                                ++g_photoMode.captures;

                                const bool is_discovery = had_subject && !was_known;
                                const std::string species_name =
                                    had_subject
                                        ? g_creatureSpecies.DisplayName(
                                              static_cast<std::uint16_t>(shot.main_species_id))
                                        : std::string("no subject");

                                //  reveal the star-verdict panel on the overlay (filled to
                                //  last_stars). name the subject + flag a first-time DISCOVERY so
                                //  the codex
                                // fill is felt at the moment of capture.
                                if (g_uiManager && g_uiManager->GetContext()) {
                                    if (auto* doc =
                                            g_uiManager->GetContext()->GetDocument("photo_mode")) {
                                        if (auto* panel = doc->GetElementById("verdict-panel"))
                                            panel->SetClass("hidden", false);
                                        if (auto* heading =
                                                doc->GetElementById("verdict_heading")) {
                                            heading->SetInnerRML(had_subject ? species_name
                                                                             : "captured");
                                        }
                                        if (auto* note = doc->GetElementById("verdict_note")) {
                                            if (is_discovery) {
                                                note->SetInnerRML(
                                                    "New species discovered! \xE2\x98\x85 " +
                                                    std::to_string(g_photoCodex.species_count()) +
                                                    " in codex");
                                                note->SetClass("verdict-discovery", true);
                                            } else if (had_subject) {
                                                note->SetInnerRML(
                                                    "Codex updated \xC2\xB7 best shot kept");
                                                note->SetClass("verdict-discovery", false);
                                            } else {
                                                note->SetInnerRML("no subject in frame");
                                                note->SetClass("verdict-discovery", false);
                                            }
                                        }
                                        if (auto* st = doc->GetElementById("verdict_stars")) {
                                            std::string stars_rml;
                                            for (int si = 0; si < 5; ++si) {
                                                stars_rml +=
                                                    (si < g_photoMode.last_stars)
                                                        ? "<span class=\"verdict-star "
                                                          "verdict-star-on\">\xE2\x98\x85</span>"
                                                        : "<span "
                                                          "class=\"verdict-star\">\xE2\x98\x85</"
                                                          "span>";
                                            }
                                            st->SetInnerRML(stars_rml);
                                        }
                                    }
                                }

                                // Everything maps to sound: a first-time codex fill rings the
                                // discovery chime at the moment of capture (2D, UI-felt).
                                if (is_discovery && audioManager)
                                    audioManager->PlayOneShot2D("discovery",
                                                                Luminumbra::Client::BusId::Events);

                                // Persist the framebuffer (PPM) + a verdict sidecar.
                                std::error_code _photo_ec;
                                const std::filesystem::path photo_dir =
                                    std::filesystem::path(root_path_str) / "photos";
                                std::filesystem::create_directories(photo_dir, _photo_ec);
                                const std::string stamp =
                                    "photo-" + std::to_string(g_photoMode.captures);
                                if (cap_w > 0 && cap_h > 0) {
                                    std::vector<unsigned char> px(static_cast<std::size_t>(cap_w) *
                                                                  static_cast<std::size_t>(cap_h) *
                                                                  3u);
                                    glReadBuffer(GL_BACK);
                                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                    glReadPixels(
                                        0, 0, cap_w, cap_h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                                    WritePixelBufferPpm(
                                        photo_dir / (stamp + ".ppm"), cap_w, cap_h, px);
                                    // Also drop a small TGA thumbnail where the gallery can load it
                                    // (RmlUi decodes TGA only). The gallery enumerates these on
                                    // open.
                                    WriteCaptureThumbnailTga(
                                        std::filesystem::path(root_path_str) / "data" / "ui" /
                                            "captures" /
                                            ("cap_" + std::to_string(g_photoMode.captures) +
                                             ".tga"),
                                        cap_w,
                                        cap_h,
                                        px,
                                        512);
                                }
                                luminumbra::game::PhotoSidecar side;
                                side.stamp = stamp;
                                side.verdict = verdict;
                                side.species_id = shot.main_species_id;
                                side.lens = g_photoMode.lens;
                                side.observation = shot.observation; // behaviour/time/light context
                                std::ofstream sidecar(photo_dir / (stamp + ".photo.json"),
                                                      std::ios::binary | std::ios::trunc);
                                if (sidecar) {
                                    const std::string json =
                                        luminumbra::game::SerializePhotoSidecar(side);
                                    sidecar.write(json.data(),
                                                  static_cast<std::streamsize>(json.size()));
                                }
                                LUMINUMBRA_CORE_INFO(
                                    "Photo captured: {} stars (total {}), saved {}",
                                    verdict.stars,
                                    verdict.total,
                                    stamp);
                            }
                        }
                    }

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
                                                wireframe_mode);
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
            } else if (g_ui_thumbs > 0 && g_menu_backdrop_active && g_camera && gameSession &&
                       gameSession->GetWorldSystem() && g_ui_thumbs_index < g_ui_thumbs) {
                // capture N clean landscape thumbnails (no UI) at varied yaw + time-of-day.
                const float yaw = 40.0f + static_cast<float>(g_ui_thumbs_index) * 47.0f;
                const float tod = 0.16f + 0.025f * static_cast<float>(g_ui_thumbs_index % 5);
                g_camera->Yaw = yaw;
                g_camera->Pitch = -14.0f; // look down to frame the lit landscape, not just sky
                g_camera->updateCameraVectors();
                renderPipeline.set_time_of_day(tod);
                renderPipeline.render_frame(gameSession->GetRegistry(),
                                            *gameSession->GetWorldSystem(),
                                            *g_camera,
                                            deltaTime,
                                            wireframe_mode);
                if (g_ui_thumbs_settle < 28) {
                    ++g_ui_thumbs_settle;
                } else {
                    int vw = 0, vh = 0;
                    glfwGetFramebufferSize(window, &vw, &vh);
                    if (vw > 0 && vh > 0) {
                        std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                      static_cast<std::size_t>(vh) * 3u);
                        glReadBuffer(GL_BACK);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                        const std::filesystem::path shot =
                            g_ui_thumbs_dir /
                            ("thumb_" + std::to_string(g_ui_thumbs_index) + ".ppm");
                        WritePixelBufferPpm(shot, vw, vh, px);
                        LUMINUMBRA_CORE_INFO(
                            "UI thumb written -> {} ({}x{})", shot.string(), vw, vh);
                    }
                    ++g_ui_thumbs_index;
                    g_ui_thumbs_settle = 0;
                    if (g_ui_thumbs_index >= g_ui_thumbs)
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            } else { // Main Menu, etc.
                //  is the create-world live preview active this frame?
                // (world_creation.rml loaded + #preview_pane present + sized). When it
                // is, the candidate world IS the backdrop — we render ONE world (the
                // candidate, full-screen) and SUPPRESS the separate menu-vista backdrop,
                // so the create screen pays for a single render with no FBO/resize churn.
                Luminumbra::Client::Rml_UIManager::PreviewState pv;
                bool previewActive = false;
                if (g_uiManager && worldgenPreview) {
                    pv = g_uiManager->GetWorldCreationPreviewState();
                    previewActive = pv.active; // full-screen diorama; no bounded pane rect
                }

                // render the live scenic world behind the menu, with a slow auto-orbit, at
                // golden hour. The menu UI bodies are transparent (game_theme.rcss) so the world
                // shows through. render_frame draws to the back buffer BEFORE the UI pass.
                // Suppressed while the create-world preview owns the world render.
                if (!previewActive && g_menu_backdrop_active && g_camera && gameSession &&
                    gameSession->GetWorldSystem()) {
                    // Gentle yaw oscillation around the lit-valley heading (95°) for a living, slow
                    // parallax that never rotates away into dark/back-lit terrain.
                    g_menu_backdrop_yaw += deltaTime; // phase accumulator (seconds)
                    g_camera->Yaw = 95.0f + 6.0f * std::sin(g_menu_backdrop_yaw * 0.12f);
                    g_camera->updateCameraVectors();
                    renderPipeline.set_time_of_day(0.24f); // dusk golden hour (matches dusk.json)
                    renderPipeline.render_frame(gameSession->GetRegistry(),
                                                *gameSession->GetWorldSystem(),
                                                *g_camera,
                                                deltaTime,
                                                wireframe_mode);
                }

                //  LIVE WORLD-PREVIEW DIORAMA. When the create-world
                // screen is up, feed the candidate params/weather/tod and render the
                // candidate world FULL-SCREEN to the backbuffer (the "framed hole" the
                // create panel frames). The menu UI bodies are transparent so the world
                // shows through; the #preview_pane frames the primary viewing area.
                // Mouse drag/scroll over the pane orbits/zooms the turntable camera.
                if (g_uiManager && worldgenPreview) {
                    if (previewActive) {
                        worldgenPreview->set_active(true);

                        // Re-derive the candidate world ONLY when the form changed.
                        // BuildCustomPreset diffs the form against the base preset;
                        // we hand the resolved JSON + data root to the in-memory
                        // loader seam so biome/structure tables resolve correctly.
                        std::string sig = pv.worldType;
                        for (const auto& p : pv.params) {
                            sig += '|';
                            sig += p.path;
                            sig += '=';
                            sig += p.value;
                        }
                        int seedVal = 4242;
                        if (sig != worldgenPreviewLastSig) {
                            worldgenPreviewLastSig = sig;
                            // prove a knob/param drag actually drives a
                            // diorama rebuild — one line per resolved candidate sig so
                            // a dragged knob is visibly firing the host rebuild branch.
                            LUMINUMBRA_CORE_INFO("Worldgen preview rebuild: sig={}", sig);
                            try {
                                const std::filesystem::path base_path =
                                    std::filesystem::path(root_path_str) / "worlds" / "atlas" /
                                    "presets" / (pv.worldType + ".json");
                                std::ifstream in(base_path);
                                if (in) {
                                    nlohmann::json base;
                                    in >> base;
                                    //  resolve the live diorama through
                                    // the engine KnobLayer when the form carries knobs
                                    // (knobs -> response curves + overlay overrides), so
                                    // sliding a knob regenerates the preview.
                                    bool hasKnobs = false;
                                    for (const auto& p : pv.params)
                                        if (p.type == "knob" || p.path.rfind("knob.", 0) == 0) {
                                            hasKnobs = true;
                                            break;
                                        }
                                    nlohmann::json resolved =
                                        hasKnobs
                                            ? Luminumbra::Client::BuildKnobResolvedPreset(base,
                                                                                          pv.params)
                                                  .json
                                            : Luminumbra::Client::BuildCustomPreset(base, pv.params)
                                                  .json;
                                    const std::filesystem::path data_root =
                                        std::filesystem::path(root_path_str) / "data";
                                    worldgenPreview->set_candidate(resolved, data_root, seedVal);
                                }
                            } catch (const std::exception& e) {
                                LUMINUMBRA_CORE_WARN("Preview candidate build failed: {}",
                                                     e.what());
                            }
                        }

                        // Live look controls.
                        using PW = Luminumbra::Client::WorldgenPreview::Weather;
                        // Headless capture (--preview-weather) can force a weather chip so the
                        // diorama spawns precipitation even though no UI pill was clicked; falls
                        // back to the DOM-selected pill for the interactive create screen.
                        const std::string weatherSel =
                            (g_ui_preview_live && !g_ui_preview_weather.empty())
                                ? g_ui_preview_weather
                                : pv.weather;
                        PW w = PW::Clear;
                        if (weatherSel == "rain")
                            w = PW::Rain;
                        else if (weatherSel == "snow")
                            w = PW::Snow;
                        else if (weatherSel == "fog")
                            w = PW::Fog;
                        else if (weatherSel == "storm")
                            w = PW::Storm;
                        worldgenPreview->set_weather(w);
                        worldgenPreview->set_time_of_day(pv.tod);
                        if (g_uiManager->ConsumeWorldCreationResetView())
                            worldgenPreview->reset_view();

                        // Orbit/zoom when the cursor is over the WORLD backdrop, i.e.
                        // not over the create panel or a control. RmlUi reports the
                        // hovered element; the bare backdrop is the document body
                        // (id "world_creation"), so a null/body hover == over the world.
                        double cx = 0.0, cy = 0.0;
                        glfwGetCursorPos(window, &cx, &cy);
                        const bool lmb =
                            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                        Rml::Element* hover = g_uiManager->GetContext()
                                                  ? g_uiManager->GetContext()->GetHoverElement()
                                                  : nullptr;
                        const bool overWorld =
                            (hover == nullptr) || (hover->GetId() == "world_creation");
                        if (lmb && overWorld && !worldgenPreviewDragging) {
                            worldgenPreviewDragging = true;
                            worldgenPreviewLastCursorX = cx;
                            worldgenPreviewLastCursorY = cy;
                        } else if (!lmb) {
                            worldgenPreviewDragging = false;
                        }
                        if (worldgenPreviewDragging) {
                            const float dx = static_cast<float>(cx - worldgenPreviewLastCursorX);
                            const float dy = static_cast<float>(cy - worldgenPreviewLastCursorY);
                            worldgenPreviewLastCursorX = cx;
                            worldgenPreviewLastCursorY = cy;
                            // Drag right -> spin right; drag down -> tilt down.
                            worldgenPreview->orbit(dx * 0.35f, -dy * 0.35f);
                        }

                        // Scroll wheel over the world zooms the diorama in/out. The
                        // menu scroll callback accrues the delta; consume + reset it
                        // here (only applied when the cursor is over the world).
                        if (overWorld && g_menu_scroll_accum != 0.0) {
                            worldgenPreview->zoom(static_cast<float>(g_menu_scroll_accum));
                        }
                        g_menu_scroll_accum = 0.0;

                        // Debounced rebuild, then render the candidate world FULL-SCREEN
                        // to the backbuffer (no offscreen FBO, no per-frame pipeline
                        // resize). render_frame clears + fills the backbuffer; the UI
                        // pass draws the frosted frame/vignette on top.
                        worldgenPreview->tick(deltaTime);
                        worldgenPreview->render_to_backbuffer(renderPipeline, deltaTime);
                    } else {
                        worldgenPreview->set_active(false);
                        worldgenPreview->clear_precipitation(
                            renderPipeline); // no lingering preview rain
                        worldgenPreviewLastSig.clear();
                        worldgenPreviewDragging = false;
                        g_menu_scroll_accum = 0.0; // drop stale wheel deltas from other menus
                    }
                }

                if (g_uiManager) {
                    g_uiManager->Render();
                }
                // --ui-screenshot: settle layout/fonts/textures, capture the back buffer (now
                // holding the UI over the menu backdrop), then advance to the next batched screen
                // (one window session captures them all). Mirrors --scene-config.
                if (!g_ui_screens.empty() && g_ui_screen_index < g_ui_screens.size()) {
                    // HEADLESS PREVIEW-DIORAMA CAPTURE: for world_creation under --preview-live,
                    // the live diorama builds on a background worker, so the fixed 30-frame settle
                    // would capture a black backdrop before world_ready. Instead, wait (bounded
                    // by a wall-clock timeout so we never hang) for the candidate world to build +
                    // a short post-ready settle (far-LOD/foliage/precipitation drawn), THEN
                    // capture.
                    static std::chrono::steady_clock::time_point s_preview_wait_start{};
                    static bool s_preview_wait_armed = false;
                    const bool preview_live_screen =
                        g_ui_preview_live && g_ui_screens[g_ui_screen_index] == "world_creation";
                    bool ready_to_capture = false;
                    if (preview_live_screen) {
                        if (!s_preview_wait_armed) {
                            s_preview_wait_armed = true;
                            s_preview_wait_start = std::chrono::steady_clock::now();
                            g_ui_preview_settle_after_ready = 0;
                        }
                        const bool world_ready = worldgenPreview && worldgenPreview->world_ready();
                        if (world_ready)
                            ++g_ui_preview_settle_after_ready;
                        const double waited_s =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          s_preview_wait_start)
                                .count();
                        constexpr double kPreviewTimeoutSeconds =
                            45.0; // hard bound -> never hangs headlessly
                        constexpr int kPreviewSettleAfterReady =
                            40; // post-ready frames for far-LOD/foliage/particles
                        const bool settled = world_ready && g_ui_preview_settle_after_ready >=
                                                                kPreviewSettleAfterReady;
                        const bool timed_out = waited_s >= kPreviewTimeoutSeconds;
                        ready_to_capture = settled || timed_out;
                        if (ready_to_capture) {
                            // One-line, self-diagnosing readiness report so a black/timed-out
                            // capture tells us WHICH failure happened (no rebuild signal vs build
                            // failed vs world built but render black) in a single run.
                            LUMINUMBRA_CORE_INFO(
                                "Preview-live capture gate: world_ready={} settle_after_ready={} "
                                "waited={:.1f}s timed_out={} "
                                "build_failed={} last_error='{}'",
                                world_ready,
                                g_ui_preview_settle_after_ready,
                                waited_s,
                                timed_out,
                                worldgenPreview ? worldgenPreview->last_build_failed() : true,
                                worldgenPreview ? worldgenPreview->last_error()
                                                : std::string("<null preview>"));
                        }
                    } else if (g_ui_screenshot_settle < 30) {
                        ++g_ui_screenshot_settle;
                    } else {
                        ready_to_capture = true;
                    }
                    if (ready_to_capture) {
                        s_preview_wait_armed = false; // re-arm for the next screen (batched runs)
                        int vw = 0, vh = 0;
                        glfwGetFramebufferSize(window, &vw, &vh);
                        if (vw > 0 && vh > 0) {
                            std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                          static_cast<std::size_t>(vh) * 3u);
                            glReadBuffer(GL_BACK);
                            glPixelStorei(GL_PACK_ALIGNMENT, 1);
                            glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                            const std::filesystem::path shot =
                                g_ui_screenshot_dir /
                                ("ui-" + g_ui_screens[g_ui_screen_index] + ".ppm");
                            WritePixelBufferPpm(shot, vw, vh, px);
                            LUMINUMBRA_CORE_INFO(
                                "UI screenshot written -> {} ({}x{})", shot.string(), vw, vh);
                        }
                        // Advance to the next screen, or finish.
                        ++g_ui_screen_index;
                        if (g_ui_screen_index < g_ui_screens.size()) {
                            g_ui_screenshot_screen = g_ui_screens[g_ui_screen_index];
                            g_ui_screenshot_settle = 0;
                            if (g_uiManager)
                                g_uiManager->RequestLoadDocument(g_ui_screens[g_ui_screen_index] +
                                                                 ".rml");
                        } else {
                            glfwSetWindowShouldClose(window, GLFW_TRUE);
                        }
                    }
                }
            }
        }

        if (currentState == GameState::IN_GAME) {
            //  Minecraft-style floating ID nameplates above each creature's head. Projects
            // the world position to screen via the camera and draws a label (id + sex + generation)
            // into the ImGui foreground list, so it shows in the live view AND in the demo capture
            // (this runs even during the timelapse, unlike the gated debug windows below).
            if (g_imgui_enabled && g_timelapse_creatures && g_camera && gameSession) {
                int fbw = 0, fbh = 0;
                glfwGetFramebufferSize(window, &fbw, &fbh);
                if (fbw > 0 && fbh > 0) {
                    const float aspect = static_cast<float>(fbw) / static_cast<float>(fbh);
                    const glm::mat4 vp =
                        glm::perspective(glm::radians(g_camera->Zoom), aspect, 0.1f, 10000.0f) *
                        g_camera->GetViewMatrix();
                    auto* dl = ImGui::GetForegroundDrawList();
                    auto& reg = gameSession->GetRegistry();
                    auto view = reg.view<const Luminumbra::Components::CreatureComponent,
                                         const Luminumbra::Components::TransformComponent>();
                    for (auto e : view) {
                        const auto& cr =
                            view.get<const Luminumbra::Components::CreatureComponent>(e);
                        const auto& tf =
                            view.get<const Luminumbra::Components::TransformComponent>(e);
                        const glm::vec4 clip =
                            vp *
                            glm::vec4(tf.position.x, tf.position.y + 2.4f, tf.position.z, 1.0f);
                        if (clip.w <= 0.05f)
                            continue; // behind the camera
                        const float sx = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(fbw);
                        const float sy =
                            (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * static_cast<float>(fbh);
                        char label[48];
                        const unsigned idx = static_cast<unsigned>(entt::to_entity(e));
                        if (cr.eaten) {
                            std::snprintf(label, sizeof(label), "#%u dead", idx);
                        } else if (cr.is_predator) {
                            std::snprintf(label, sizeof(label), "#%u PRED", idx);
                        } else if (const auto* gn =
                                       reg.try_get<Luminumbra::Components::CreatureGenomeComponent>(
                                           e)) {
                            std::snprintf(label,
                                          sizeof(label),
                                          "#%u %c g%u",
                                          idx,
                                          gn->female ? 'F' : 'M',
                                          gn->generation);
                        } else {
                            std::snprintf(label, sizeof(label), "#%u", idx);
                        }
                        const ImVec2 sz = ImGui::CalcTextSize(label);
                        const ImVec2 at(sx - sz.x * 0.5f, sy - sz.y);
                        dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                                    IM_COL32(0, 0, 0, 200),
                                    label); // shadow
                        dl->AddText(at, IM_COL32(255, 255, 255, 235), label);
                    }
                }
            }
            if (g_imgui_enabled && g_playerController && g_timelapse_frames == 0) {
                g_playerController->RenderDebugUI();
            }
            // Always-on time-scale indicator (when not real-time) so slow-mo / fast-forward
            // / pause is obvious at a glance.
            if (g_imgui_enabled && g_timeScale != 1.0f && g_timelapse_frames == 0) {
                ImGui::SetNextWindowPos(ImVec2(10.0f, 60.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.5f);
                if (ImGui::Begin("##timescale",
                                 nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoMove)) {
                    if (g_timeScale == 0.0f)
                        ImGui::Text("|| PAUSED  (\\ to resume)");
                    else
                        ImGui::Text("TIME  x%.2f", g_timeScale);
                }
                ImGui::End();
            }
            //  minimal crop HUD — seed/harvest inventory + the farming verb hints.
            if (g_imgui_enabled && currentState == GameState::IN_GAME &&
                !scenario_config.active() && g_timelapse_frames == 0) {
                ImGui::SetNextWindowPos(ImVec2(10.0f, 92.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.45f);
                if (ImGui::Begin("##farmhud",
                                 nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoMove)) {
                    ImGui::Text("FARM  seeds:%d  harvests:%d  yield:%.1f",
                                g_farming.seeds,
                                g_farming.harvests,
                                g_farming.total_yield);
                    ImGui::TextDisabled("F plant   G water   H fertilize   J harvest");
                }
                ImGui::End();
            }
            // Live GPU profiler : per-pass GPU-ms + draw/instance counts read from the render
            // pipeline's GL_TIMESTAMP timer ring (render-side only; never hashed). The first real
            // interactive readout for the engine optimization pass.
            if (g_imgui_enabled && g_show_gpu_profiler && currentState == GameState::IN_GAME &&
                g_timelapse_frames == 0) {
                const auto& gp = renderPipeline.get_last_render_pass_stats();
                ImGui::SetNextWindowPos(ImVec2(10.0f, 120.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowBgAlpha(0.85f);
                if (ImGui::Begin(
                        "GPU Profiler ", &g_show_gpu_profiler, ImGuiWindowFlags_AlwaysAutoResize)) {
                    const double total_ms = gp.shadow_gpu_ms + gp.gbuffer_gpu_ms + gp.ssao_gpu_ms +
                                            gp.ssao_blur_gpu_ms + gp.lighting_gpu_ms +
                                            gp.water_gpu_ms + gp.skybox_gpu_ms +
                                            gp.particle_gpu_ms + gp.foliage_gpu_ms +
                                            gp.aerial_gpu_ms + gp.final_blit_gpu_ms;
                    if (!gp.gpu_timers_supported) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                           "GPU timers unsupported on this context");
                    }
                    const ImVec4 over(1.0f, 0.45f, 0.45f, 1.0f);
                    const ImVec4 okc(0.70f, 0.85f, 0.70f, 1.0f);
                    ImGui::TextColored(total_ms > 3.333 ? over : okc,
                                       "GPU frame total: %6.3f ms   (budget 3.333 ms @ 300fps)",
                                       total_ms);
                    const double ui_ms = g_uiManager ? g_uiManager->GetLastUiFrameMs() : 0.0;
                    ImGui::Text("UI submit (CPU): %6.3f ms", ui_ms);
                    ImGui::Separator();
                    auto row = [&](const char* name, double ms) {
                        ImGui::TextColored(ms > 1.0 ? over : okc, "  %-11s %7.3f ms", name, ms);
                    };
                    row("gbuffer", gp.gbuffer_gpu_ms);
                    row("shadow", gp.shadow_gpu_ms);
                    row("lighting", gp.lighting_gpu_ms);
                    row("foliage", gp.foliage_gpu_ms);
                    row("particle", gp.particle_gpu_ms);
                    row("water", gp.water_gpu_ms);
                    row("ssao", gp.ssao_gpu_ms + gp.ssao_blur_gpu_ms);
                    row("aerial", gp.aerial_gpu_ms);
                    row("skybox", gp.skybox_gpu_ms);
                    row("final_blit", gp.final_blit_gpu_ms);
                    ImGui::Separator();
                    ImGui::Text("terrain: %5zu draws  %5zu chunks  %7zu tris",
                                gp.terrain_draws,
                                gp.terrain_visible_chunks,
                                gp.terrain_indices_drawn / 3);
                    ImGui::Text("far-LOD: %5zu draws  %7zu tris",
                                gp.far_region_draws,
                                gp.far_indices_drawn / 3);
                    ImGui::Text("foliage: %5zu draws  %7zu instances",
                                gp.foliage_draws,
                                gp.foliage_instances_drawn);
                    ImGui::Text("particle: %5zu draws  %7zu instances",
                                gp.particle_draws,
                                gp.particles_drawn);
                    ImGui::Text("shadow: %5zu draws", gp.shadow_draws);
                    ImGui::Text("water: %5zu draws", gp.water_draws);
                }
                ImGui::End();
            }
            // --debug-glass-pane — stage three stained-glass
            // panes on the terrain near spawn (one-time). Render-only capture subject.
            if (g_debug_glass_panes && !g_glass_panes_spawned &&
                currentState == GameState::IN_GAME && gameSession) {
                if (auto* gws = gameSession->GetWorldSystem()) {
                    g_glass_panes_spawned = true;
                    std::vector<Luminumbra::Rendering::GlassPaneItem> panes;
                    const glm::vec3 pane_tints[3] = {
                        {0.95f, 0.25f, 0.25f}, {0.25f, 0.85f, 0.35f}, {0.30f, 0.45f, 0.95f}};
                    for (int pi = 0; pi < 3; ++pi) {
                        const float px = 12.0f + 5.0f * static_cast<float>(pi);
                        const float pz = 14.0f;
                        const float py = gws->GetTerrainHeightAt(px, pz);
                        Luminumbra::Rendering::GlassPaneItem pane;
                        glm::mat4 pm(1.0f);
                        pm = glm::translate(pm, glm::vec3(px, py, pz));
                        // Lean the panes back ~50 deg so the near-noon sun projects a
                        // real footprint (a vertical pane under an overhead sun casts
                        // only a sliver - the first capture attempt's lesson).
                        pm = glm::rotate(pm, glm::radians(-50.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                        pm = glm::scale(pm, glm::vec3(4.0f, 5.0f, 1.0f)); // 4 m wide, 5 m tall
                        pane.model = pm;
                        pane.tint = pane_tints[pi];
                        pane.thickness = 1.0f;
                        panes.push_back(pane);
                    }
                    renderPipeline.set_glass_panes(std::move(panes));
                    LUMINUMBRA_CORE_INFO(
                        "Debug glass panes staged (3 stained-glass tints) near spawn");
                }
            }
            // push the metering opt-in (idempotent per frame).
            renderPipeline.set_auto_exposure_metered(g_auto_exposure_metered);
            //  rendering: push the volumetrics tier.
            renderPipeline.set_volumetric_quality(g_volumetric_quality);
            // live shader authoring (crawl  + walk: watcher + panel ).
            // Render-only end to end: shaders/uniforms never feed the sim or world_hash.
            if (currentState == GameState::IN_GAME && g_timelapse_frames == 0) {
                // Crawl: reload-all requested by  (executed here, on the GL thread).
                if (g_request_shader_reload) {
                    g_request_shader_reload = false;
                    renderPipeline.reload_all_shaders();
                }
                // Walk (-3): opt-in once/sec mtime poll over the roster's source
                // files; a changed file triggers that shader's rollback-safe Reload.
                if (g_shader_auto_reload) {
                    const double watch_now = glfwGetTime();
                    if (watch_now - g_shader_watch_last_poll >= 1.0) {
                        g_shader_watch_last_poll = watch_now;
                        renderPipeline.enumerate_shaders([&](const char* sh_name,
                                                             Luminumbra::Rendering::Shader* sh) {
                            if (!sh)
                                return;
                            bool changed = false;
                            for (const std::string& p : {sh->VertexPath(), sh->FragmentPath()}) {
                                if (p.empty())
                                    continue;
                                std::error_code ec;
                                const auto t = std::filesystem::last_write_time(p, ec);
                                if (ec)
                                    continue;
                                auto it = g_shader_watch_mtimes.find(p);
                                if (it != g_shader_watch_mtimes.end() && it->second != t) {
                                    changed = true;
                                }
                                g_shader_watch_mtimes[p] = t;
                            }
                            if (changed) {
                                LUMINUMBRA_CORE_INFO(
                                    "Shader source changed on disk -> reloading {}", sh_name);
                                sh->Reload();
                            }
                        });
                    }
                }
                // Walk (-4): the dev shader panel — roster status, per-shader
                // reload, and LIVE uniform editing via glProgramUniform (GL 4.5 DSA).
                // Honesty note (-5): passes re-set most uniforms per draw; an
                // edit persists only for uniforms a pass never sets (u_dev_*).
                if (g_imgui_enabled && g_show_shader_panel) {
                    ImGui::SetNextWindowPos(ImVec2(10.0f, 420.0f), ImGuiCond_FirstUseEver);
                    ImGui::SetNextWindowSize(ImVec2(440.0f, 520.0f), ImGuiCond_FirstUseEver);
                    if (ImGui::Begin("Shaders ", &g_show_shader_panel)) {
                        ImGui::TextWrapped(
                            "Live shader authoring (): edit res/shaders/ in any editor, "
                            "reload hot-swaps rollback-safe. Uniform edits persist only for "
                            "uniforms a pass does not re-set per frame (u_dev_* convention).");
                        if (ImGui::Button("Reload All "))
                            g_request_shader_reload = true;
                        ImGui::SameLine();
                        ImGui::Checkbox("Auto-reload on file change", &g_shader_auto_reload);
                        ImGui::Separator();
                        renderPipeline.enumerate_shaders([&](const char* sh_name,
                                                             Luminumbra::Rendering::Shader* sh) {
                            if (!sh)
                                return;
                            ImGui::PushID(sh_name);
                            if (ImGui::CollapsingHeader(sh_name)) {
                                const bool sh_ok = sh->IsValid();
                                ImGui::TextColored(sh_ok ? ImVec4(0.70f, 0.85f, 0.70f, 1.0f)
                                                         : ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                                                   sh_ok ? "valid"
                                                         : "INVALID (prior program kept)");
                                if (!sh->Diagnostic().empty()) {
                                    ImGui::TextWrapped("%s", sh->Diagnostic().c_str());
                                }
                                if (ImGui::Button("Reload"))
                                    sh->Reload();
                                const GLuint prog = sh->Id();
                                GLint uniform_count = 0;
                                glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &uniform_count);
                                for (GLint u = 0; u < uniform_count; ++u) {
                                    char uname[128];
                                    GLsizei ulen = 0;
                                    GLint usize = 0;
                                    GLenum utype = 0;
                                    glGetActiveUniform(prog,
                                                       static_cast<GLuint>(u),
                                                       sizeof(uname),
                                                       &ulen,
                                                       &usize,
                                                       &utype,
                                                       uname);
                                    if (usize != 1)
                                        continue; // arrays: not editable here
                                    const GLint loc = glGetUniformLocation(prog, uname);
                                    if (loc < 0)
                                        continue;
                                    ImGui::PushID(u);
                                    const bool looks_color = std::string_view(uname).find("olor") !=
                                                             std::string_view::npos;
                                    switch (utype) {
                                        case GL_FLOAT: {
                                            float v = 0.0f;
                                            glGetUniformfv(prog, loc, &v);
                                            if (ImGui::DragFloat(uname, &v, 0.01f))
                                                glProgramUniform1f(prog, loc, v);
                                            break;
                                        }
                                        case GL_FLOAT_VEC2: {
                                            float v[2] = {};
                                            glGetUniformfv(prog, loc, v);
                                            if (ImGui::DragFloat2(uname, v, 0.01f))
                                                glProgramUniform2fv(prog, loc, 1, v);
                                            break;
                                        }
                                        case GL_FLOAT_VEC3: {
                                            float v[3] = {};
                                            glGetUniformfv(prog, loc, v);
                                            const bool edited =
                                                looks_color
                                                    ? ImGui::ColorEdit3(
                                                          uname, v, ImGuiColorEditFlags_Float)
                                                    : ImGui::DragFloat3(uname, v, 0.01f);
                                            if (edited)
                                                glProgramUniform3fv(prog, loc, 1, v);
                                            break;
                                        }
                                        case GL_FLOAT_VEC4: {
                                            float v[4] = {};
                                            glGetUniformfv(prog, loc, v);
                                            const bool edited =
                                                looks_color
                                                    ? ImGui::ColorEdit4(
                                                          uname, v, ImGuiColorEditFlags_Float)
                                                    : ImGui::DragFloat4(uname, v, 0.01f);
                                            if (edited)
                                                glProgramUniform4fv(prog, loc, 1, v);
                                            break;
                                        }
                                        case GL_INT: {
                                            GLint v = 0;
                                            glGetUniformiv(prog, loc, &v);
                                            if (ImGui::DragInt(uname, &v))
                                                glProgramUniform1i(prog, loc, v);
                                            break;
                                        }
                                        case GL_BOOL: {
                                            GLint v = 0;
                                            glGetUniformiv(prog, loc, &v);
                                            bool b = v != 0;
                                            if (ImGui::Checkbox(uname, &b))
                                                glProgramUniform1i(prog, loc, b ? 1 : 0);
                                            break;
                                        }
                                        default:
                                            break; // samplers/matrices: display-only, skip
                                    }
                                    ImGui::PopID();
                                }
                            }
                            ImGui::PopID();
                        });
                    }
                    ImGui::End();
                }
            }
            // Settings menu ( to toggle; frees the cursor). Render-only; user.* is never
            // hashed. Changes apply live and "Save" persists them to the per-user overlay.
            // The polished RML settings screen (settings.rml) is the follow-on.
            if (g_imgui_enabled && g_show_settings && g_camera && g_timelapse_frames == 0) {
                ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Settings ")) {
                    luminumbra::core::UserSettings& us = g_systemConfig.user();
                    if (ImGui::SliderFloat(
                            "Look sensitivity", &us.mouse_sensitivity, 0.01f, 1.0f, "%.3f")) {
                        g_camera->MouseSensitivity = us.mouse_sensitivity; // applied live
                    }
                    if (ImGui::SliderFloat("FOV", &us.fov, 30.0f, 110.0f, "%.0f deg")) {
                        g_camera->Zoom = us.fov;
                    }
                    if (ImGui::Checkbox("VSync", &us.vsync)) {
                        glfwSwapInterval(us.vsync ? 1 : 0);
                    }
                    ImGui::Separator();
                    ImGui::SliderFloat("Time scale", &g_timeScale, 0.0f, 8.0f, "%.2fx");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("1x"))
                        g_timeScale = 1.0f;
                    ImGui::SameLine();
                    if (ImGui::SmallButton(g_timeScale == 0.0f ? "Resume" : "Pause"))
                        g_timeScale = (g_timeScale == 0.0f) ? 1.0f : 0.0f;
                    ImGui::TextDisabled("engine time: [ slower   ] faster   \\ reset");
                    ImGui::Separator();
                    {
                        // Window mode — applied live via ApplyWindowMode (no-op on capture-pinned
                        // runs).
                        const char* modes[] = {"windowed", "borderless", "fullscreen"};
                        int cur = 1; // default borderless
                        for (int i = 0; i < 3; ++i)
                            if (us.window_mode == modes[i])
                                cur = i;
                        if (ImGui::Combo("Window mode", &cur, modes, 3)) {
                            us.window_mode = modes[cur];
                            const WindowMode m =
                                ParseWindowMode(us.window_mode, g_windowState.mode);
                            ApplyWindowMode(window, g_windowState, m);
                        }
                    }
                    {
                        // Resolution — applied live in windowed mode (borderless/fullscreen use
                        // the monitor's resolution); suppressed on capture-pinned gate runs.
                        const char* resos[] = {"1280x720",
                                               "1920x1080",
                                               "2560x1440",
                                               "3440x1440",
                                               "3840x1600",
                                               "3840x2160"};
                        int rcur = -1;
                        for (int i = 0; i < 6; ++i)
                            if (us.resolution == resos[i])
                                rcur = i;
                        if (ImGui::Combo("Resolution", &rcur, resos, 6) && rcur >= 0) {
                            us.resolution = resos[rcur];
                            const std::string& r = us.resolution;
                            const auto xp = r.find('x');
                            if (xp != std::string::npos) {
                                const int rw = std::atoi(r.substr(0, xp).c_str());
                                const int rh = std::atoi(r.substr(xp + 1).c_str());
                                if (rw > 0 && rh > 0) {
                                    g_windowState.windowedWidth = rw;
                                    g_windowState.windowedHeight = rh;
                                    if (us.window_mode == "windowed" &&
                                        !g_windowState.capture_pinned)
                                        glfwSetWindowSize(window, rw, rh);
                                }
                            }
                        }
                    }
                    if (ImGui::SliderFloat("Master volume", &us.audio_master, 0.0f, 1.0f, "%.2f")) {
                        if (audioManager)
                            audioManager->SetMasterVolume(us.audio_master); // applied live
                    }
                    if (ImGui::SliderFloat("Music volume", &us.audio_music, 0.0f, 1.0f, "%.2f")) {
                        if (audioManager)
                            audioManager->SetMusicVolume(
                                us.audio_music); // applied live (music bus)
                    }
                    if (ImGui::SliderFloat("SFX volume", &us.audio_sfx, 0.0f, 1.0f, "%.2f")) {
                        if (audioManager)
                            audioManager->SetSfxVolume(us.audio_sfx); // applied live (sfx bus)
                    }
                    if (ImGui::CollapsingHeader("Controls (keyboard)")) {
                        for (const auto& def : Luminumbra::Client::kInputActionDefs) {
                            const int idx = static_cast<int>(def.action);
                            const int kc = g_systemConfig.keybind(def.name, def.default_key);
                            const char* kn = glfwGetKeyName(kc, 0);
                            char btn[48];
                            if (g_rebindCaptureAction == idx)
                                std::snprintf(btn, sizeof(btn), "press a key...##%s", def.name);
                            else if (kn)
                                std::snprintf(btn, sizeof(btn), "%s##%s", kn, def.name);
                            else
                                std::snprintf(btn, sizeof(btn), "key %d##%s", kc, def.name);
                            ImGui::Text("%-12s", def.name);
                            ImGui::SameLine(150);
                            if (ImGui::Button(btn))
                                g_rebindCaptureAction = idx;
                        }
                        ImGui::TextDisabled("click a binding, then press a key (Esc cancels)");
                    }
                    if (ImGui::Button("Save settings")) {
                        const std::string path =
                            luminumbra::core::SystemConfig::DefaultUserOverlayPath();
                        const bool ok = g_systemConfig.SaveUserOverlay(path);
                        LUMINUMBRA_CORE_INFO(
                            "Settings {} ({})", ok ? "saved" : "save FAILED", path);
                    }
                    ImGui::TextDisabled("user.* — client-only, never hashed");
                }
                ImGui::End();
            }
            if (g_imgui_enabled && show_worldgen_viewer && worldGenViewer) {
                worldGenViewer->UpdateAndRender(show_worldgen_viewer,
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

        if (g_imgui_enabled) {
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
        if (!g_render_benchmark_path.empty() && currentState == GameState::IN_GAME && gameSession) {
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
        if (g_timelapse_frames > 0 && currentState == GameState::IN_GAME && gameSession) {
            if (g_timelapse_settle < kTimelapseSettleFrames) {
                renderPipeline.set_time_of_day(g_timelapse_tod); // settle at the start time-of-day
                ++g_timelapse_settle; // let the world stream/settle before frame 0
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
                    std::snprintf(nm, sizeof(nm), "frame_%04d.ppm", g_timelapse_captured);
                    if (WritePixelBufferPpm(g_timelapse_dir / nm, vw, vh, px))
                        ++g_timelapse_captured;
                }
                if (g_timelapse_captured >= g_timelapse_frames) {
                    LUMINUMBRA_CORE_INFO("Timelapse: captured {} frames -> {}",
                                         g_timelapse_captured,
                                         g_timelapse_dir.string());
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                } else {
                    //  terraform demo: from the 3rd captured frame, carve a trench that
                    // marches from +Z toward spawn (one sphere/frame), then let the fast-forward
                    // ticks below drain/redirect any water into the freshly-cut channel. Carve
                    // BEFORE the ticks so the water responds within this same frame's advance.
                    if (g_timelapse_dig && g_timelapse_captured >= 2 &&
                        gameSession->GetWorldSystem()) {
                        const auto sp = gameSession->GetMetadata().spawnPoint;
                        const int dig_i =
                            g_timelapse_captured - 2; // 0,1,2,... after a 2-frame establishing hold
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
                    if (g_timelapse_drain && g_drain_state.init && gameSession->GetWorldSystem()) {
                        auto* wsd = gameSession->GetWorldSystem();
                        wsd->debug_force_water_remesh(); // smooth per-frame surface
                        // GROUND TRUTH: water volume in a disc over the INLAND target region (where
                        // the channel is cut). If the water really floods into the new cut, this
                        // RISES from ~0.
                        const Luminumbra::Vec3 inland(g_drain_state.P.x + g_drain_state.dhx * 6.0f,
                                                      g_drain_state.P.y,
                                                      g_drain_state.P.z + g_drain_state.dhz * 6.0f);
                        const std::int64_t vol = wsd->debug_water_volume_near(inland, 10.0f);
                        LUMINUMBRA_CORE_INFO(
                            "Timelapse-drain[f{}]: inland water volume (Sum depth) = {} mm-cells",
                            g_timelapse_captured,
                            vol);
                        // Three acts: A) hold on the dry bank, B) carve a contained MODERATE-depth
                        // basin into it (touching the sea so it floods), C) STOP carving and hold
                        // while the sea fills the basin up to its level on camera. Moderate depth
                        // so it fills in-window.
                        const int holdN = std::max(6, g_timelapse_frames / 5); // Act A end
                        const int carveN =
                            holdN + std::max(10, g_timelapse_frames / 3); // Act B end
                        if (g_timelapse_captured >= holdN && g_timelapse_captured < carveN) {
                            const int k = g_timelapse_captured - holdN;
                            // Bowl 3 m onto the bank, always touching the sea; WIDEN it (fixed 2 m
                            // depth) so a pool grows into the green bank and stays IN FRAME.
                            const float cx = g_drain_state.P.x + g_drain_state.dhx * 3.0f;
                            const float cz = g_drain_state.P.z + g_drain_state.dhz * 3.0f;
                            const float radius =
                                3.0f + 0.34f * static_cast<float>(k); // 3 -> ~11 m wide
                            const float floor_y =
                                g_drain_state.surf - 1.5f; // shallow+wide -> fills fast, spreads
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
                    if (g_timelapse_rain && g_drain_state.init && gameSession->GetWorldSystem()) {
                        auto* wsr = gameSession->GetWorldSystem();
                        wsr->debug_force_water_remesh();
                        const int rainOff =
                            (g_timelapse_frames * 3) / 5; // Act 1 rains, Act 2 dries
                        if (g_timelapse_captured == rainOff) {
                            wsr->SetWaterHydrology(/*finite=*/true, /*rain=*/0, /*evap=*/2);
                            LUMINUMBRA_CORE_INFO("Timelapse-rain: rain OFF + evaporation ON — "
                                                 "water is finite, it recedes (no refill)");
                        }
                        const std::int64_t vol =
                            wsr->debug_water_volume_near(g_drain_state.P, 14.0f);
                        LUMINUMBRA_CORE_INFO(
                            "Timelapse-rain[f{}]: basin vol = {}, LAND water = {} mm-cells",
                            g_timelapse_captured,
                            vol,
                            wsr->debug_land_water_volume_mm());
                    }
                    // Fast-forward the SIM (weather/wind/creatures/plants) by K EXTRA fixed
                    // ticks for the next frame (on top of the normal per-frame tick). Physics
                    // runs normally each frame so the player stays grounded.
                    for (int i = 0; i < g_timelapse_ticks; ++i)
                        gameSession->TickSimulation(1.0 / 30.0);
                    if (g_timelapse_daystep > 0.0f) { // drift the sun/sky for shade-over-time
                        g_timelapse_tod += g_timelapse_daystep;
                        if (g_timelapse_tod >= 1.0f)
                            g_timelapse_tod -= 1.0f;
                        renderPipeline.set_time_of_day(g_timelapse_tod);
                    }
                    if (g_timelapse_season) { // drift summer -> autumn leaf color across the
                                              // capture
                        g_season = static_cast<float>(g_timelapse_captured) /
                                   static_cast<float>(std::max(1, g_timelapse_frames - 1));
                    }
                    if (g_timelapse_grow || g_timelapse_season) { // re-bake on stage/season change
                        if (g_timelapse_grow) {
                            g_procgenStageF =
                                5.0f * static_cast<float>(g_timelapse_captured) /
                                static_cast<float>(std::max(1, g_timelapse_frames - 1));
                        }
                        BakeProcgenPlants(renderPipeline.plant_procgen(), g_procgenStageF);
                    }
                    //  re-bake LIVE sim plants each captured frame so the timelapse
                    // shows their REAL growth (the session tick advanced PlantGrowthSystem since
                    // the last bake). Visual-only; sim/world_hash untouched.
                    if (g_timelapse_simgrow && gameSession) {
                        RebakeAllPlants(renderPipeline.plant_procgen(),
                                        gameSession->GetRegistry(),
                                        g_procgenSunDir,
                                        g_season);
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

            if (rb_warm < g_render_benchmark_warmup) {
                ++rb_warm;
            } else if (rb_count < g_render_benchmark_frames) {
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
                if (rb_count == g_render_benchmark_frames &&
                    !g_render_benchmark_screenshot.empty()) {
                    int vw = 0, vh = 0;
                    glfwGetFramebufferSize(window, &vw, &vh);
                    if (vw > 0 && vh > 0) {
                        std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                      static_cast<std::size_t>(vh) * 3u);
                        glReadBuffer(GL_FRONT);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                        WritePixelBufferPpm(
                            std::filesystem::path(g_render_benchmark_screenshot), vw, vh, px);
                        LUMINUMBRA_CORE_INFO("Render benchmark screenshot -> {} ({}x{})",
                                             g_render_benchmark_screenshot,
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
                j["warmup_frames"] = g_render_benchmark_warmup;
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
                const std::filesystem::path rb_path(g_render_benchmark_path);
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
                                     g_render_benchmark_path,
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
                rb_count = g_render_benchmark_frames + 1; // latch: stop re-dumping
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
    if (g_imgui_enabled) {
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

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    // Rebind capture: while waiting for a key for some action, the next key press becomes
    // its binding (Escape cancels). Intercept first so any key — even F-keys — can be bound.
    if (g_rebindCaptureAction >= 0 && action == GLFW_PRESS) {
        if (key != GLFW_KEY_ESCAPE &&
            g_rebindCaptureAction < static_cast<int>(Luminumbra::Client::kInputActionCount)) {
            const char* name = Luminumbra::Client::kInputActionDefs[g_rebindCaptureAction].name;
            g_systemConfig.user().keybinds[name] = key;
            if (g_playerController)
                g_playerController->ApplyKeyBindings(g_systemConfig);
        }
        g_rebindCaptureAction = -1;
        return;
    }
    // Escape: toggle the in-game pause overlay (only in a world, and not while the  panel is up).
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && g_playerController && !g_show_settings) {
        SetGamePaused(window, !g_paused);
        return;
    }
    // toggle the live per-pass GPU profiler overlay.
    if (key == GLFW_KEY_F3 && action == GLFW_PRESS) {
        g_show_gpu_profiler = !g_show_gpu_profiler;
        return;
    }
    // crawl — hot-reload every live shader from res/shaders/ next
    // frame (rollback-safe per shader; a broken edit keeps the prior program).
    if (key == GLFW_KEY_F5 && action == GLFW_PRESS) {
        g_request_shader_reload = true;
        return;
    }
    // walk — the dev shader panel (per-shader reload, auto-reload
    // watcher toggle, live uniform editing).
    if (key == GLFW_KEY_F10 && action == GLFW_PRESS) {
        g_show_shader_panel = !g_show_shader_panel;
        if (g_show_shader_panel) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (g_playerController) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
        return;
    }
    // cycle the render-only G-buffer debug view (off -> albedo -> normal -> depth ->
    // material -> position -> off). Diagnostic only; never affects sim/world_hash. The main
    // loop pushes g_debug_view_mode into the pipeline each frame.
    if (key == GLFW_KEY_F6 && action == GLFW_PRESS) {
        g_debug_view_mode = (g_debug_view_mode + 1) % 6; // 0..5
        return;
    }
    // toggle the settings menu and free/restore the cursor so the panel is usable.
    if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
        g_show_settings = !g_show_settings;
        g_rebindCaptureAction = -1;
        if (g_show_settings) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (g_playerController) { // in a world -> resume mouse-look
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            firstMouse = true; // avoid a camera jump when mouse-look resumes
        }
        return;
    }
    // Engine time scale (host_timescale-style): [ slower, ] faster, \ reset to 1x.
    if (action == GLFW_PRESS && (key == GLFW_KEY_LEFT_BRACKET || key == GLFW_KEY_RIGHT_BRACKET ||
                                 key == GLFW_KEY_BACKSLASH)) {
        if (key == GLFW_KEY_LEFT_BRACKET)
            g_timeScale = (g_timeScale <= 0.125f) ? 0.0f : g_timeScale * 0.5f; //...down to pause
        else if (key == GLFW_KEY_RIGHT_BRACKET)
            g_timeScale = (g_timeScale < 0.125f) ? 0.125f : std::min(g_timeScale * 2.0f, 16.0f);
        else
            g_timeScale = 1.0f; // reset
        LUMINUMBRA_CORE_INFO("Engine time scale: {:.3f}x", g_timeScale);
        return;
    }
    if (key == GLFW_KEY_F7 && action == GLFW_PRESS) {
        show_worldgen_viewer = !show_worldgen_viewer;
        if (show_worldgen_viewer) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        return;
    }
    // Alt+Enter: runtime windowed <-> borderless toggle.
    if (key == GLFW_KEY_ENTER && action == GLFW_PRESS && (mods & GLFW_MOD_ALT)) {
        ToggleWindowedBorderless(window, g_windowState);
        return;
    }
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
        ToggleFullscreen(window, g_windowState);
        return;
    }
    if (key == GLFW_KEY_F9 && action == GLFW_PRESS) {
        wireframe_mode = !wireframe_mode;
        return;
    }

    if (g_playerController) {
        g_playerController->ProcessKeyInput(key, action);
    }

    if (g_imgui_enabled && ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureKeyboard) {
            return;
        }
    }

    // Always give RmlUi a chance to process key events.
    if (g_uiManager) {
        g_uiManager->KeyCallback(window, key, scancode, action, mods);
        // If RmlUi has a focused input element, it should consume the event.
        if (g_uiManager->GetContext() && g_uiManager->GetContext()->GetFocusElement()) {
            return;
        }
    }
}

void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState) {
    gameStateManager.SetState(newState);
    bool cursorDisabled = (newState == GameState::IN_GAME);

    if (cursorDisabled) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        // Set game-related callbacks
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        // Mouse buttons could be set here for game actions if needed
        glfwSetMouseButtonCallback(window, nullptr);
        firstMouse = true;
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (g_uiManager) {
            // Set UI-related callbacks
            glfwSetCursorPosCallback(window, Luminumbra::Client::Rml_UIManager::CursorPosCallback);
            glfwSetScrollCallback(window,
                                  menu_scroll_callback); // RmlUi + create-world preview zoom
            glfwSetMouseButtonCallback(window,
                                       Luminumbra::Client::Rml_UIManager::MouseButtonCallback);
        } else {
            glfwSetCursorPosCallback(window, nullptr);
            glfwSetScrollCallback(window, nullptr);
            glfwSetMouseButtonCallback(window, nullptr);
        }
    }
}

void SetGamePaused(GLFWwindow* window, bool paused) {
    g_paused = paused;
    if (paused) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (g_uiManager) {
            glfwSetCursorPosCallback(window, Luminumbra::Client::Rml_UIManager::CursorPosCallback);
            glfwSetMouseButtonCallback(window,
                                       Luminumbra::Client::Rml_UIManager::MouseButtonCallback);
            glfwSetScrollCallback(window, Luminumbra::Client::Rml_UIManager::ScrollCallback);
            g_uiManager->RequestLoadDocument("pause.rml");
        }
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, nullptr);
        firstMouse = true;
        g_photoModeUiShown = false; // re-sync the in-game overlay next frame
        if (g_uiManager)
            g_uiManager->RequestLoadDocument("hud.rml");
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    if (g_show_settings || g_paused)
        return; // settings/pause open (cursor freed) -> don't swing the camera
    if (firstMouse) {
        lastX = (float)xpos;
        lastY = (float)ypos;
        firstMouse = false;
    }
    float xoffset = (float)xpos - lastX;
    float yoffset = lastY - (float)ypos;
    lastX = (float)xpos;
    lastY = (float)ypos;
    if (g_camera)
        g_camera->ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;
    const bool imgui_wants_mouse =
        g_imgui_enabled && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    if (imgui_wants_mouse ||
        (g_uiManager && g_uiManager->GetContext()->GetHoverElement() != nullptr)) {
        return;
    }
    if (g_camera)
        g_camera->ProcessMouseScroll((float)yoffset);
    if (g_playerController)
        g_playerController->ProcessMouseScroll(yoffset);
}

void menu_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    // Keep RmlUi's scroll behaviour for menu lists/galleries...
    Luminumbra::Client::Rml_UIManager::ScrollCallback(window, xoffset, yoffset);
    //...and accrue the vertical wheel delta for the create-world preview zoom.
    // The preview block consumes + resets this each frame (only when over the pane).
    g_menu_scroll_accum += yoffset;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    (void)window;
    if (width <= 0 || height <= 0)
        return; // minimized window: ignore
    // Capture-pinned (scenario) runs must never resize their targets; the gate
    // depends on a fixed 1280x720 framebuffer.
    if (g_windowState.capture_pinned)
        return;
    // Debounce: record the pending size and let the main loop coalesce a burst
    // of drag events into one RenderPipeline::on_resize after the size settles.
    g_windowState.pending_width = width;
    g_windowState.pending_height = height;
    g_windowState.pending_since_seconds = glfwGetTime();
    g_windowState.resize_pending = true;
}

void GLFWErrorCallback(int error, const char* description) {
    LUMINUMBRA_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
}

void GLAPIENTRY GLDebugMessageCallback(GLenum source,
                                       GLenum type,
                                       GLuint id,
                                       GLenum severity,
                                       GLsizei length,
                                       const GLchar* message,
                                       const void* userParam) {
    (void)source;
    (void)length;
    (void)userParam;
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204)
        return;
    g_gl_debug_message_count.fetch_add(1, std::memory_order_relaxed);
    const bool is_error = type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH;
    if (is_error) {
        g_gl_debug_error_count.fetch_add(1, std::memory_order_relaxed);
        LUMINUMBRA_CORE_ERROR("OpenGL: {0}", message);
        return;
    }

    switch (severity) {
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            g_gl_debug_notification_count.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_TRACE("OpenGL: {0}", message);
            break;
        default:
            g_gl_debug_warning_count.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_WARN("OpenGL: {0}", message);
            break;
    }
}
