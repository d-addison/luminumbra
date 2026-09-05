#pragma once

// The frame loop's non-scenario state clusters, grouped verbatim from the
// former main_client.cpp file-scope globals into one struct-of-structs
// context. main_client.cpp owns the single file-scope instance; the frame
// helpers extracted into app/ (FrameAudio, CaveFlourishes, DebugOverlays,
// MenuScreens, InputCallbacks) take it by reference instead of reaching for
// externs. Member initializers preserve each original global's initial value
// byte for byte, and none of the member types logs from a destructor (the
// teardown contract that moved the UI hot-reload watcher into main() scope).

#include "../../../include/luminumbra/core/Types.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/game/Objectives.h"
#include "luminumbra_common/game/PhotoMode.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "rendering/LightningBolt.h"
#include "rendering/ScentFieldRenderMirror.h"
#include "rendering/SnowCoverModel.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Client::App {

// ->: thunder cues queued by the live lightning
// consumer (strike sim tick + distance + magnitude); the audio tick drains them
// with the physical sound delay (distance / 343 m/s). Client-only, never sim.
struct PendingThunder {
    std::uint64_t strike_tick = 0;
    double fired_at_seconds = 0.0; // wall clock when the bolt rendered
    float distance_m = 0.0f;
    float magnitude = 0.0f;
};

struct TimelapseDrainState {
    bool init = false;
    Luminumbra::Vec3 P{0.0f, 0.0f, 0.0f};
    float dhx = 0.0f, dhz = 1.0f;
    float surf = 0.0f;
};

// let the world stream/settle before frame 0
inline constexpr int kTimelapseSettleFrames = 45;
// let chunks stream + atmosphere settle
inline constexpr int kFrameScanSettleFrames = 90;
// Watchdog: a headless auto-capture must NEVER hang. If the world hasn't
// reached IN_GAME and completed the scan within this many render-loop frames
// (boot + stream + settle is normally ~500-700), abort with a clear error
// instead of looping forever.
inline constexpr int kFrameScanWatchdogFrames = 4000;

// Photo mode / codex / objectives / farming HUD / settings-rebind state.
// Client-only presentation state — none of it participates in any baseline
// NetworkStateHash (PhotoCodex.h documents this), so photo mode is a pure
// read-mostly observer. The *Sig strings cache the last-rendered DOM text so
// the RML documents are only touched when the content actually changes.
struct HudState {
    luminumbra::game::PhotoModeState photoMode;
    bool photoModeUiShown = false; //  photo_mode.rml vs hud.rml is the active in-game overlay
    luminumbra::game::PhotoCodex photoCodex;
    // Data-driven creature species metadata (display names + rarity) the
    // codex/discovery HUD resolve a captured species_id against. Loaded once
    // from data/common/creatures/species after the runtime root is known;
    // client-only, never hashed.
    luminumbra::ai::CreatureSpeciesRegistry creatureSpecies;
    // Progression goals surfaced on the in-game HUD. Initialised lazily once
    // in-game with the default world's first creature so the starter chain is
    // always achievable. Client-only, never hashed.
    luminumbra::game::ObjectiveSet objectives;
    bool objectivesInit = false;
    std::string objHudSig;
    std::string farmHudSig;
    // Creature codex browse overlay (client-only). codexOpen toggles via the
    // ToggleCodex key; codexSig throttles re-population so rows are rebuilt
    // only when discovery changes.
    bool codexOpen = false;
    std::string codexSig;
    // Settings menu — frees the cursor so the ImGui panel is clickable. While
    // rebindCaptureAction >= 0 the next key press is captured as that action's
    // binding.
    bool show_settings = false;
    bool paused = false; //  in-game pause overlay active
    int rebindCaptureAction = -1;
    //  player farming state. The controller holds the seed/harvest inventory;
    // the species registry is loaded once for player seeding. Interactive-only
    // (never touched by gates).
    luminumbra::foliage::FarmingController farming;
    luminumbra::foliage::SpeciesRegistry farmSpecies;
    bool farmSpeciesLoaded = false;
    // Player-selected species to plant (index into farmSpecies.all; V cycles
    // it). So FarmPlant isn't hard-coded to wheat — the player picks
    // oak/wheat/etc. from the data-driven registry.
    int farmSelectedSpecies = 0;
};

// Debug/dev overlay toggles: GPU profiler, shader panel + watcher, G-buffer
// debug view, glass-pane capture subject, volumetrics tier, wireframe.
struct OverlayState {
    bool show_gpu_profiler = false; // Live per-pass GPU profiler overlay.
    //  (live shader authoring):  = reload-all next frame (GL-thread safe);
    //  = the dev shader panel; the watcher is the opt-in once/sec mtime poll.
    bool show_shader_panel = false;
    bool request_shader_reload = false;
    bool shader_auto_reload = false;
    double shader_watch_last_poll = 0.0;
    std::unordered_map<std::string, std::filesystem::file_time_type> shader_watch_mtimes;
    // --debug-glass-pane stages three stained-glass panes near spawn so the
    // colored-shadow AC captures have a subject. Render-only.
    bool debug_glass_panes = false;
    bool glass_panes_spawned = false;
    // --auto-exposure-metered opts the GPU metering servo in.
    bool auto_exposure_metered = false;
    //  rendering: --volumetric-quality N (0 analytic-only default).
    int volumetric_quality = 0;
    int debug_view_mode = 0; // render-only G-buffer debug overlay:
                             // 0=off,1=albedo,2=normal,3=depth,4=material,5=position ( cycles)
    bool show_worldgen_viewer = false;
    bool wireframe_mode = false;
    bool imgui_enabled = true;
};

// The write-once-at-CLI-parse, read-in-render capture-mode cluster:
// timelapse / scene-config / frame-scan / render-parity / ui-screenshot /
// render-benchmark / survey / bake-impostor / fixed-cam / play-paths /
// timeScale. See main_client.cpp's CLI parse for each flag's contract.
struct CaptureModes {
    // host_timescale-style engine time control (Source/GMod-like). 1.0 = real
    // time, 0 = paused, <1 slow-mo, >1 fast-forward. Render/client playback
    // rate: scales how many FIXED 30 Hz sim ticks run per real frame, NOT the
    // tick dt — so determinism + run==replay hold (tick sequence unchanged;
    // per-tick world_hash unchanged). Leveraged by the timelapse capture.
    float timeScale = 1.0f;
    // --timelapse capture: dump a frame sequence of the LIVE loaded world
    // while sim-time (and, optionally, the day clock) fast-forward, for
    // tools/timelapse.py. 0 = off.
    int timelapse_frames = 0;
    int timelapse_ticks = 60; // sim ticks advanced between captured frames (2 s at 30 Hz)
    float timelapse_daystep =
        0.0f; // time-of-day advance per frame [0,1] (shade/sky drift); 0 = leave
    int timelapse_captured = 0;
    int timelapse_settle = 0;
    bool timelapse_dig = false; // progressively carve a trench mid-capture (terraform demo)
    bool timelapse_drain =
        false; //  money shot: anchor on a real river/lake, breach the bank, drain it
    bool timelapse_rain = false; // finite hydrology — rain fills the land, then drain stays drained
    int timelapse_rain_mm = 18; // rain rate (mm/tick) for the rain demo
    TimelapseDrainState drain_state;
    float timelapse_tod = 0.0f; // starting time-of-day (0 = noon/brightest; drifts by daystep)
    std::filesystem::path timelapse_dir;
    bool timelapse_grow = false; // grow the procgen plants sapling->tree over the capture
    bool timelapse_simgrow =
        false; // seed SIM PlantTag plants + grow them via the real growth tick ( bridge)
    bool timelapse_season = false;    // drift summer->autumn leaf color over the capture
    bool timelapse_creatures = false; // spawn predators/prey + render markers (ecology demo)
    bool timelapse_living = false; // capture the LIVING WORLD (ambient creatures + forager colony)
                                   // during a timelapse, so the scent trail / rest poses show
    bool timelapse_calm = false;   // calm (no-predator) grazing herd -> reproduction/evolution demo
    bool timelapse_fire = false;   // ignite a patch of combustible foliage -> sim.fire spread demo
    // --render-benchmark <path>: averages the per-pass GPU timers over
    // render_benchmark_frames settled in-world frames and writes a JSON report
    // (per-pass avg ms + total + cloud_quality) before exiting. Empty = off.
    std::string render_benchmark_path;
    int render_benchmark_frames = 120; // measured frames (after warm-up)
    int render_benchmark_warmup = 60;  // frames discarded before measuring (stream/settle)
    //  optional PPM screenshot of the forest-DENSE budget pose, dumped on the
    // final measured frame. Empty = off.
    std::string render_benchmark_screenshot;
    // --play-paths runs a scenario's scripted camera but with the NORMAL-PLAY
    // render/streaming paths instead of the gate-mode paths a scenario
    // normally forces.
    bool play_paths = false;
    // --profile-fly <seconds> drives the player FORWARD at a constant noclip
    // speed in NORMAL-PLAY mode (no scenario, no gate-mode). 0 = off.
    double profile_fly_seconds = 0.0;
    // --debug-goto cave|doline|spawn: deterministically frame a feature for
    // capture.
    std::string debug_goto;
    // Fixed camera-pose override (--cam-pos x,y,z [--cam-yaw d]
    // [--cam-pitch d]): pins the camera every frame for
    // reproducible/controllable screenshots + benchmarks.
    bool fixed_cam = false;
    glm::vec3 fixed_cam_pos{0.0f};
    float fixed_cam_yaw = 0.0f;
    float fixed_cam_pitch = 0.0f;
    // --scene-config <json>: declaratively COMPOSE a scene (camera+fov,
    // time-of-day, weather, clouds, fog) to match a reference image, render a
    // settled frame, and screenshot it.
    bool scene_active = false;
    std::filesystem::path scene_dir;  // output dir for the scene screenshot
    std::filesystem::path scene_shot; // full screenshot path (.ppm)
    float scene_fov = 0.0f;           // 0 = leave camera default
    int scene_weather = 0;            // 0 none, 1 rain, 2 snow, 3 fog, 4 storm
    float scene_weather_intensity = 0.0f;
    bool scene_clouds = false; // push a cloud state
    float scene_cloud_coverage = 0.45f;
    float scene_cloud_biome = 0.0f;
    float scene_cloud_plane = 900.0f;
    bool scene_cloud_shadow = false;
    float scene_cloud_shadow_strength = 0.0f;
    //  rendering: scene-config "moon" -> lunar illumination [0,1] (1 full
    // moon, ~0 new moon). <0 = leave the automatic tick-derived lunar cycle.
    float scene_moon = -1.0f;
    // framescan: --frame-scan <out.json>. The scan issues no draws and never
    // touches sim/world_hash. Empty = off.
    std::string frame_scan_path;
    bool frame_scan_active = false;
    int frame_scan_settle = 0;
    // DIAGNOSTIC-only: LUMIN_FRAME_SCAN_SETTLE overrides the settle so a
    // headless capture can wait for the world to FULLY stream/bake.
    int frame_scan_settle_target = kFrameScanSettleFrames;
    int frame_scan_watchdog = 0;
    bool render_parity_active = false;
    std::filesystem::path render_parity_dir;
    std::string render_parity_pass = "frame";
    // --bake-tree-impostor <out.ppm>: far-field impostor atlas bake
    // (render-only; runs on the first render-loop frame).
    std::string bake_impostor_path;
    bool bake_impostor_done = false;
    // --survey <dir>: autonomous tour — discover POIs in the generated world,
    // write a screenshot + frame-scan per POI. Empty = off.
    std::string survey_dir;
    bool survey_active = false;
    int survey_settle = 0;
    // --ui-screenshot <screen>: the UI fidelity gate. Force the menu state,
    // load <screen>.rml, settle, capture the back buffer, and exit.
    std::string ui_screenshot_screen;    // "" = inactive; else the CURRENT screen being captured
    std::vector<std::string> ui_screens; // batch list (one window pop captures all in sequence)
    std::size_t ui_screen_index = 0;     // which screen in ui_screens is being captured
    std::filesystem::path ui_screenshot_dir; // output dir; each screen -> ui-<screen>.ppm
    bool ui_fixtures = false;                // seed deterministic UI fixture data
    int ui_screenshot_settle = 0;            // frames waited before capture of the current screen
    // HEADLESS PREVIEW-DIORAMA CAPTURE (--preview-live): for the
    // world_creation screen, wait (bounded) for the live diorama to build
    // before capturing. Render-only; server --smoke unaffected.
    bool ui_preview_live = false;   // wait for the live diorama before capturing world_creation
    std::string ui_preview_weather; // optional forced weather chip (e.g. "rain") so precip spawns
    int ui_preview_settle_after_ready =
        0; // post-world_ready settle frames accrued (far-LOD/foliage/particles)
    // thumbnail generation: capture N clean (no-UI) backdrop frames at varied
    // yaw/time-of-day in one window session. --ui-thumbs N.
    int ui_thumbs = 0;                   // 0 = off; else number of thumbnails to capture
    std::filesystem::path ui_thumbs_dir; // output dir; each -> thumb_<i>.ppm
    int ui_thumbs_index = 0;
    int ui_thumbs_settle = 0;
};

// live scenic menu backdrop: a golden-hour world rendered behind the menus.
// Stood up at boot while staying in MAIN_MENU; the menu render branch draws it
// under the transparent UI with a slow auto-orbit. Replaced cleanly when a
// real world loads.
struct MenuBackdrop {
    bool menu_backdrop_active = false;
    float menu_backdrop_yaw = 30.0f; // orbit accumulator (degrees)
    //  scroll-wheel accumulator for the create-world preview diorama. GLFW
    // scroll is event-driven (no poll API), so the menu scroll callback
    // accrues the wheel delta here and the preview block consumes it each
    // frame to drive WorldgenPreview::zoom.
    double menu_scroll_accum = 0.0;
};

// Initial world-generation loading-screen bookkeeping.
struct LoadingState {
    std::vector<Luminumbra::IVec3> initial_chunks_to_load;
    int generation_dispatch_index = 0;
    Luminumbra::JobHandle world_gen_handle;
    bool world_render_data_initialized = false;
};

// Mouse-look bookkeeping shared by the GLFW cursor callback and the
// game-state/pause transitions (firstMouse suppresses the camera jump when
// mouse-look resumes after the cursor was freed).
struct MouseLookState {
    float lastX = 1280 / 2.0f;
    float lastY = 720 / 2.0f;
    bool firstMouse = true;
};

// Frame-audio bookkeeping (the thunder cue queue).
struct FrameAudioState {
    std::vector<PendingThunder> pendingThunder;
};

// Live forager presentation caches and diagnostics. The environment-gated
// scent switch retains function-local-static lazy initialization semantics.
struct ForagerRenderState {
    std::unordered_map<std::uint32_t, float> cellHeight;
    float forageLog = 0.0f;
    std::uint32_t lastDeliveries = 0;
    bool scentDecalInitialized = false;
    bool scentDecal = false;
    Luminumbra::Rendering::ScentFieldRenderMirror scentMirror;
    int scentLog = 0;
};

// Normal-play profiling begins on the first frame that enters its branch, not
// when the process-wide context is constructed.
struct ProfileFlyState {
    bool initialized = false;
    double start = 0.0;
};

// One-time live-world dressing state. Skeleton and clip addresses remain
// stable because ClientAppContext has process lifetime.
struct WorldDressingState {
    luminumbra::animation::Skeleton wildlifeSkeleton;
    luminumbra::animation::AnimationClip wildlifeIdle;
    bool dispatched = false;
};

// Live weather presentation state: scheduled lightning and snow accumulation.
struct LiveWeatherState {
    std::uint64_t lastStrikeTickDone = 0;
    int boltFramesLeft = 0;
    Luminumbra::Rendering::LightningRenderState liveBolt;
    Luminumbra::Rendering::SnowCover::State snow;
};

struct FoliageScatterEntry {
    bool accepted = false;
    Luminumbra::Rendering::FoliagePass::ChunkScatter scatter;
};

// Live foliage setup and the normal-play per-chunk scatter cache.
struct FoliageRenderState {
    bool loaded = false;
    std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter> cachedScatter;
    std::uint64_t cachedScatterSig = ~0ull;
    std::unordered_map<Luminumbra::ChunkID, FoliageScatterEntry> scatterByChunk;
    std::uint32_t ambientMotes = Luminumbra::Rendering::ParticlePass::kInvalidEmitter;
};

// Edge-trigger bookkeeping for interactive farming and terraforming controls.
struct InteractionEdgeState {
    bool farmPlant = false;
    bool farmWater = false;
    bool farmFertilize = false;
    bool farmHarvest = false;
    bool farmCycleSpecies = false;
    bool terraformDig = false;
    bool terraformFill = false;
    float waterProbeRing[5][2] = {
        {0.0f, 0.0f}, {4.0f, 0.0f}, {-4.0f, 0.0f}, {0.0f, 4.0f}, {0.0f, -4.0f}};
};

struct SceneCaptureState {
    int settleFrames = 0;
};

struct RuntimeTelemetryState {
    int slowFrameCount = 0;
};

// Accumulators for the opt-in render benchmark report.
struct RenderBenchmarkState {
    std::chrono::steady_clock::time_point previousFrameEnd{};
    int warmupFrames = 0;
    int measuredFrames = 0;
    int nvmlSampleCount = 0;
    double shadow = 0.0;
    double gbuffer = 0.0;
    double ssao = 0.0;
    double ssaoBlur = 0.0;
    double lighting = 0.0;
    double water = 0.0;
    double skybox = 0.0;
    double particle = 0.0;
    double foliage = 0.0;
    double aerial = 0.0;
    double finalBlit = 0.0;
    double total = 0.0;
    double cpu = 0.0;
    double present = 0.0;
    double wall = 0.0;
    double power = 0.0;
    double clock = 0.0;
    double cpuPrepare = 0.0;
    double cpuShadow = 0.0;
    double cpuGbuffer = 0.0;
    double cpuPost = 0.0;
    double cpuStaticProp = 0.0;
    double sim = 0.0;
    double stream = 0.0;
    double foliageRebuild = 0.0;
    double ui = 0.0;
    double renderCall = 0.0;
    double poll = 0.0;
    double scatter = 0.0;
    bool nvmlEver = false;
};

struct ClientAppContext {
    HudState hud;
    OverlayState overlay;
    CaptureModes capture;
    MenuBackdrop menu;
    LoadingState loading;
    MouseLookState input;
    FrameAudioState audio;
    ForagerRenderState foragers;
    ProfileFlyState profileFly;
    WorldDressingState worldDressing;
    LiveWeatherState weather;
    FoliageRenderState foliage;
    InteractionEdgeState interactions;
    SceneCaptureState sceneCapture;
    RuntimeTelemetryState telemetry;
    RenderBenchmarkState benchmark;
};

} // namespace Luminumbra::Client::App
