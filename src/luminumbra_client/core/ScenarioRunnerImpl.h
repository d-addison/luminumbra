#pragma once

// Concrete ScenarioRunner implementation, internal to luminumbra_client_qa.
// The per-scenario frame-loop state and hook bodies here moved VERBATIM out of
// main_client.cpp; main() only sees core/ScenarioRunner.h. The reference
// members and the state members deliberately keep the exact names the moved
// code used as main() locals/globals (g_camera, water_visual_target, ...) so
// every moved body compiles unchanged.
//
// TU layout (both share this class):
//   ScenarioRunnerDrive.cpp   - construction, the loop-top readiness watchdog,
//                               and the IN_GAME scenario driving chain.
//   ScenarioRunnerCapture.cpp - the pre-render pins, the world-visual-sweep
//                               matrix, the post-render capture/pixel-analysis
//                               blocks, and the shutdown result artifacts.

#include "core/ScenarioRunner.h"

#include "app/ClientAppContext.h"
#include "app/RuntimeStateRecorder.h"
#include "core/RuntimeScenarioHarness.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "rendering/RenderPipeline.h"
#include "rendering/passes/ParticlePass.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Luminumbra::Client::ScenarioHarness {

// farlod_horizon_smoke: phase A (far-LOD disabled) measures the
// honest in-run gbuffer GPU baseline, phase B enables far-LOD and sweeps
// the stations across the live/far boundary. (Shared by the drive-side camera
// sweep and the capture-side analysis, hence header scope.)
inline constexpr double kFarLodHorizonPhaseSplit = 0.30;

class ScenarioRunnerImpl final : public ScenarioRunner {
public:
    explicit ScenarioRunnerImpl(const ScenarioFrameContext& context);

    void onLoopTop() override;
    InGameDrive onGameStateInGame(float deltaTime) override;
    void onPreRenderWorldSweep() override;
    void onPreRenderCapturePins() override;
    void onPostRenderCapture(float deltaTime) override;
    void onShutdown() override;

private:
    // The IN_GAME driving chain, one method per scenario branch. Each returns
    // whether its scenario guard fired (the original else-if chain semantics:
    // the dispatcher stops at the first branch that fires).
    bool drivePersistenceRoundtrip();
    bool driveLodGround();
    bool driveWaterVisual();
    bool driveMaterialVisual();
    bool driveSkyboxVisual();
    bool driveWeatherVisual();
    bool driveCloudShadow();
    bool driveParticleDeterminism();
    bool driveFoliageVisual();
    bool drivePrecipitation();
    bool driveTimeOfDaySweep();
    bool driveLodBoundaryOscillation();
    bool driveLodSeamArrival();
    bool drivePlayerView();
    bool driveFarLodHorizon();
    bool driveSkinnedMeshVisual(float deltaTime);
    bool driveCreatureSlice(float deltaTime);
    bool driveNetworkedSession();

    // The post-render capture blocks, one method per scenario family, called
    // in the exact original order by onPostRenderCapture. Each method carries
    // its whole original scenario-guarded block (guard included) verbatim.
    void captureWindowModeStress();
    void captureLodSeamArrival(std::chrono::steady_clock::time_point now);
    void captureLodGround(std::chrono::steady_clock::time_point now);
    void captureWaterVisual(std::chrono::steady_clock::time_point now);
    void captureMaterialVisual(std::chrono::steady_clock::time_point now);
    void captureSkyboxVisual(std::chrono::steady_clock::time_point now);
    void captureWeatherVisual(std::chrono::steady_clock::time_point now);
    void captureLightningStrike(std::chrono::steady_clock::time_point now);
    void captureCloudShadow(std::chrono::steady_clock::time_point now);
    void captureParticleDeterminism(std::chrono::steady_clock::time_point now);
    void captureAtmosMotion(std::chrono::steady_clock::time_point now);
    void captureFoliageVisual(std::chrono::steady_clock::time_point now);
    void capturePrecipitation(std::chrono::steady_clock::time_point now);
    void captureTimeOfDaySweep(std::chrono::steady_clock::time_point now);
    void capturePlayerView(std::chrono::steady_clock::time_point now);
    void captureFarLodHorizon(std::chrono::steady_clock::time_point now, float deltaTime);
    void captureSkinnedMeshVisual(std::chrono::steady_clock::time_point now);
    void captureRemoteAvatarArtifact();
    void captureShowcaseVideo(std::chrono::steady_clock::time_point now);
    void captureCreatureSlice(std::chrono::steady_clock::time_point now);

    // --- main() frame-loop state the hooks read/write (see ScenarioFrameContext) ---
    GLFWwindow* const window;
    const std::filesystem::path& root_dir;
    const std::string& root_path_str;
    RuntimeScenarioConfig& scenario_config;
    App::RuntimeStateRecorder& runtime_state_recorder;
    App::RuntimeScenarioFrameRecorder& lod_ground_frame_recorder;
    Luminumbra::JobSystem& jobSystem;
    std::unique_ptr<Luminumbra::world::GameSession>& gameSession;
    Luminumbra::Rendering::RenderPipeline& renderPipeline;
    std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera;
    App::ClientAppContext& g_app;
    const std::string& scenario_world_type;
    bool& scenario_failed;
    std::string& scenario_failure_reason;
    bool& scenario_ready;
    std::uint64_t& scenario_frame_count;
    std::chrono::steady_clock::time_point& scenario_play_started_at;
    App::RuntimeReadinessReport& last_readiness_report;
    int& exit_code;

    // --- scenario-only state, moved verbatim from main()'s pre-loop locals ---
    bool scenario_timed_run_complete = false;
    //  the world_visual_sweep runs its whole capture matrix
    // synchronously in one pass once the world is ready, so it self-completes.
    bool world_visual_sweep_done = false;
    std::chrono::steady_clock::time_point scenario_started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_runtime_state_write =
        std::chrono::steady_clock::now();
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
    double atmos_motion_last_capture_s = -1.0;
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
};

} // namespace Luminumbra::Client::ScenarioHarness
