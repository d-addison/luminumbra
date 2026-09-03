// Scenario capture hooks: the pre-render capture pins, the world-visual-sweep
// matrix, the per-scenario post-render capture/pixel-analysis blocks, and the
// shutdown result artifacts, moved VERBATIM out of main_client.cpp's frame
// loop behind the ScenarioRunner seam. See core/ScenarioRunnerImpl.h for the
// TU layout.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "core/ScenarioRunnerImpl.h"

#include "core/Log.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/network/NetworkLoopbackAuthority.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "nlohmann/json.hpp"
#include "rendering/Camera.h"
#include "rendering/FarLodSystem.h"
#include "rendering/passes/WaterPass.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// The moved code names the scenario-harness and app vocabulary unqualified,
// exactly as it did inside main_client.cpp (file-level using-directives).
using namespace Luminumbra::Client::ScenarioHarness;
using namespace Luminumbra::Client::App;

namespace {

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
    const int y_from_bottom = static_cast<int>((ndc.y * 0.5f + 0.5f) * static_cast<float>(height));
    const int y_from_top = height - 1 - y_from_bottom;
    if (x < margin || x >= width - margin || y_from_top < margin || y_from_top >= height - margin) {
        return false;
    }
    out_x = x;
    out_y_from_top = y_from_top;
    return true;
};

const auto median_of = [](std::vector<double> samples) -> double {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() - 1) / 2];
};

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
constexpr double kAtmosMotionFrameIntervalS = 1.0 / 60.0;

} // namespace

namespace Luminumbra::Client::ScenarioHarness {

void ScenarioRunnerImpl::onPreRenderWorldSweep() {
    //  run the entire deterministic capture matrix
    // (times-of-day x angles x weather x season) in ONE synchronous pass
    // once the world is ready, then self-complete. The render_and_read
    // hook owns render_frame + present + glReadPixels so the harness stays
    // GL-context-free.: drives the existing one-way bridges,
    // never writes world_hash.
    if (scenario_config.world_visual_sweep() && scenario_ready && !world_visual_sweep_done &&
        gameSession->GetWorldSystem() && g_camera) {
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
            const auto winter = Luminumbra::Core::ReadEnvironment("LUMINUMBRA_VISUAL_SWEEP_WINTER");
            deps.include_winter = winter && !winter->empty() && winter->front() != '0';
        }
        // The anchor position is FIXED across the whole matrix (only the
        // camera orientation changes per cell), so the world only needs to
        // stream ONCE. We stream for a bounded warmup, then skip the
        // expensive per-frame world update and just re-render — the same
        // settled geometry is reused for every subsequent cell.
        int sweep_stream_frames = 0;
        deps.render_and_read = [&, sweep_stream_frames](std::vector<unsigned char>& out_pixels,
                                                        int& w,
                                                        int& h) mutable -> bool {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (sweep_stream_frames < 24) {
                gameSession->GetWorldSystem()->update(gameSession->GetRegistry(),
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
            out_pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u, 0u);
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            // glReadPixels itself synchronizes; no explicit glFinish needed.
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, out_pixels.data());
            glfwSwapBuffers(window);
            glfwPollEvents();
            return true;
        };
        const bool sweep_passed = Luminumbra::Client::ScenarioHarness::RunWorldVisualSweep(deps);
        world_visual_sweep_done = true;
        if (!sweep_passed) {
            scenario_failed = true;
            scenario_failure_reason = "world_visual_sweep_presence_failed";
        }
        scenario_timed_run_complete = true;
        glfwSetWindowShouldClose(window, true);
    }
}

void ScenarioRunnerImpl::onPreRenderCapturePins() {
    if (scenario_config.lod_ground_smoke() || scenario_config.water_visual_smoke() ||
        scenario_config.material_visual_smoke() || scenario_config.skybox_visual_smoke() ||
        scenario_config.weather_visual_smoke() || scenario_config.cloud_shadow_smoke() ||
        scenario_config.precipitation_smoke() || scenario_config.lod_seam_arrival_smoke() ||
        scenario_config.player_view_smoke() || scenario_config.farlod_horizon_smoke() ||
        scenario_config.skinned_mesh_visual_smoke() || scenario_config.creature_slice_smoke()) {
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
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double sweep_progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
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
            const TimeOfDaySweepCapturePlan& plan = TimeOfDaySweepCapturePlanAt(pending_capture);
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
}

void ScenarioRunnerImpl::onPostRenderCapture(float deltaTime) {
    // Body of the original `if (scenario_config.active() && currentState ==
    // GameState::IN_GAME)` post-render region: the runner only exists for
    // active scenario runs and main() calls this hook inside the IN_GAME
    // render branch, so the guard is met by construction. The capture blocks
    // run in their exact original order.
    ++scenario_frame_count;
    const auto now = std::chrono::steady_clock::now();
    captureWindowModeStress();
    if (lod_ground_frame_recorder.enabled()) {
        lod_ground_frame_recorder.record_frame(
            deltaTime, gameSession.get(), renderPipeline, scenario_frame_count);
    }
    if (scenario_config.lod_boundary_oscillation_smoke() && scenario_ready) {
        lod_boundary_transition_recorder.record_frame(gameSession->GetWorldSystem());
    }
    captureLodSeamArrival(now);
    captureLodGround(now);
    captureWaterVisual(now);
    captureMaterialVisual(now);
    captureSkyboxVisual(now);
    captureWeatherVisual(now);
    captureLightningStrike(now);
    captureCloudShadow(now);
    captureParticleDeterminism(now);
    captureAtmosMotion(now);
    captureFoliageVisual(now);
    capturePrecipitation(now);
    captureTimeOfDaySweep(now);
    capturePlayerView(now);
    captureFarLodHorizon(now, deltaTime);
    captureSkinnedMeshVisual(now);
    captureRemoteAvatarArtifact();
    captureShowcaseVideo(now);
    captureCreatureSlice(now);
    if (now - last_runtime_state_write >= std::chrono::seconds(1)) {
        last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
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
            std::chrono::duration_cast<std::chrono::seconds>(now - scenario_play_started_at)
                .count();
        if (elapsed_play_seconds >= scenario_config.timed_run_seconds && scenario_frame_count > 0) {
            last_readiness_report = EvaluateReadiness(scenario_config, gameSession.get());
            if (scenario_config.skinned_mesh_visual_smoke() && scenario_config.replicated &&
                scenario_config.avatars >= 2 && !remote_avatar_render_artifact_written) {
                scenario_failed = true;
                scenario_failure_reason = "remote_avatar_render_artifact_missing";
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

void ScenarioRunnerImpl::captureWindowModeStress() {
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
            done.targets_width_after = static_cast<int>(renderPipeline.screen_width());
            done.targets_height_after = static_cast<int>(renderPipeline.screen_height());
        }
        if (window_mode_stress_step_index < window_mode_stress_steps.size()) {
            WindowModeStressStep& step = window_mode_stress_steps[window_mode_stress_step_index];
            step.resize_generation_before = renderPipeline.resize_generation();
            step.size_changed = static_cast<int>(renderPipeline.screen_width()) != step.width ||
                                static_cast<int>(renderPipeline.screen_height()) != step.height;
            renderPipeline.on_resize(static_cast<unsigned int>(step.width),
                                     static_cast<unsigned int>(step.height));
            ++window_mode_stress_step_index;
        } else {
            // All steps applied + finalized: capture the
            // restored pinned-size frame (Smoke-equivalent).
            // Targets are 1280x720; render one clean frame
            // into the default framebuffer for the readback.
            window_mode_stress_capture.width = static_cast<int>(renderPipeline.screen_width());
            window_mode_stress_capture.height = static_cast<int>(renderPipeline.screen_height());
            window_mode_stress_capture.file = "window-mode-stress-final.ppm";
            ScreenshotPixelStats final_stats;
            if (WriteBackbufferPpm(scenario_config.artifact_dir / window_mode_stress_capture.file,
                                   window_mode_stress_capture.width,
                                   window_mode_stress_capture.height,
                                   &final_stats)) {
                window_mode_stress_capture.pixels = final_stats;
            }
            window_mode_stress_complete = true;
        }
    }
}

void ScenarioRunnerImpl::captureLodSeamArrival(std::chrono::steady_clock::time_point now) {
    if (scenario_config.lod_seam_arrival_smoke() && scenario_ready) {
        lod_seam_arrival_recorder.record_frame(gameSession->GetWorldSystem());
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const std::array<double, 4> thresholds{0.25, 0.50, 0.75, 0.95};
        const std::array<const char*, 4> names{"p25", "p50", "p75", "p95"};
        for (std::size_t i = 0; i < thresholds.size(); ++i) {
            if (lod_seam_screenshots_written[i] || progress < thresholds[i]) {
                continue;
            }
            lod_seam_screenshots_written[i] = true;
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
            const std::string relative_path =
                std::string("screenshots/lod-seam-") + names[i] + ".ppm";
            LodHolePixelStats hole_stats;
            if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path,
                                   screenshot_width,
                                   screenshot_height,
                                   nullptr,
                                   &hole_stats)) {
                lod_seam_visual_captures.push_back({names[i], relative_path, hole_stats});
                WriteLodSeamArrivalAnalysis(scenario_config.artifact_dir,
                                            elapsed_play_seconds,
                                            lod_seam_visual_captures,
                                            lod_seam_arrival_recorder);
            }
        }
    }
}

void ScenarioRunnerImpl::captureLodGround(std::chrono::steady_clock::time_point now) {
    if (scenario_config.lod_ground_smoke() && scenario_ready) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const std::array<double, 3> thresholds{0.15, 0.55, 0.95};
        const std::array<const char*, 3> names{"start", "mid", "end"};
        for (std::size_t i = 0; i < thresholds.size(); ++i) {
            if (lod_ground_screenshots_written[i] || progress < thresholds[i]) {
                continue;
            }
            lod_ground_screenshots_written[i] = true;
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
            const std::string relative_path =
                std::string("screenshots/lod-ground-") + names[i] + ".ppm";
            LodHolePixelStats hole_stats;
            if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path,
                                   screenshot_width,
                                   screenshot_height,
                                   nullptr,
                                   &hole_stats)) {
                lod_ground_screenshot_files.push_back(relative_path);
                lod_ground_visual_captures.push_back({names[i], relative_path, hole_stats});
                WriteLodGroundScreenshotIndex(scenario_config.artifact_dir,
                                              lod_ground_screenshot_files);
                WriteLodGroundVisualAnalysis(scenario_config.artifact_dir,
                                             lod_ground_visual_captures);
            }
        }
    }
}

void ScenarioRunnerImpl::captureWaterVisual(std::chrono::steady_clock::time_point now) {
    if (scenario_config.water_visual_smoke() && scenario_ready &&
        !water_reflection_capture_written) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        // sample the ROI water luminance roughly once a
        // second while the top-down framing is active; the analysis
        // requires temporal variance across these samples (animated
        // caustics, not a static tint).
        if (!water_visual_capture_written && render_pass_stats.water_draws > 0 &&
            water_visual_target.found &&
            elapsed_play_seconds >= water_caustics_next_sample_seconds) {
            int sample_width = 0;
            int sample_height = 0;
            glfwGetFramebufferSize(window, &sample_width, &sample_height);
            WaterCausticsSample caustics_sample =
                SampleBackbufferWaterLuminance(sample_width, sample_height, elapsed_play_seconds);
            caustics_sample.texture_mean_abs_delta = SampleCausticsTextureDelta(
                renderPipeline.water_caustics_texture(), water_caustics_previous_texels);
            water_caustics_samples.push_back(caustics_sample);
            water_caustics_next_sample_seconds = elapsed_play_seconds + 1.0;
        }
        // Main capture (top-down framing) at 50% progress.
        if (!water_visual_capture_written && progress >= 0.50 && progress < 0.60 &&
            render_pass_stats.water_draws > 0 && water_visual_target.found &&
            water_caustics_samples.size() >= 2) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                    water_visual_pixel_stats =
                        AnalyzeScreenshotPixels(frame_pixels, screenshot_width, screenshot_height);

                    // depth tint gradient + shoreline foam
                    // probes around the projected shallow/deep points.
                    constexpr int kWaterPatchRadius = 10;
                    int patch_x = 0;
                    int patch_y = 0;
                    if (water_visual_target.shallow_point_found && g_camera &&
                        project_world_to_capture(*g_camera,
                                                 water_visual_target.shallow_point,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 kWaterPatchRadius,
                                                 patch_x,
                                                 patch_y)) {
                        water_shallow_patch = AnalyzeWaterRegionPatch(frame_pixels,
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
                        water_deep_patch = AnalyzeWaterRegionPatch(frame_pixels,
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
                        water_foam_patch = AnalyzeWaterRegionPatch(frame_pixels,
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
        if (water_visual_capture_written && progress >= 0.85 && render_pass_stats.water_draws > 0 &&
            water_visual_target.found) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                const WaterReflectionStats reflection_stats = AnalyzeWaterReflection(
                    frame_pixels,
                    screenshot_width,
                    screenshot_height,
                    Luminumbra::Rendering::WaterPass::approximate_sky_reflection_color(1.0f));
                const std::string reflection_path = "screenshots/water-reflection.ppm";
                if (WritePixelBufferPpm(scenario_config.artifact_dir / reflection_path,
                                        screenshot_width,
                                        screenshot_height,
                                        frame_pixels)) {
                    water_reflection_capture_written = true;
                    WriteWaterVisualAnalysis(scenario_config.artifact_dir,
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
}

void ScenarioRunnerImpl::captureMaterialVisual(std::chrono::steady_clock::time_point now) {
    if (scenario_config.material_visual_smoke() && scenario_ready &&
        !material_visual_capture_written) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        if (progress >= 0.50 && render_pass_stats.terrain_draws > 0 &&
            material_visual_target.found) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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

                const std::string screenshot_path = "screenshots/material-visual.ppm";
                const std::string heatmap_path = "screenshots/material-id-heatmap.ppm";
                const MaterialPixelStats material_stats =
                    AnalyzeMaterialPixels(frame_pixels, screenshot_width, screenshot_height);
                const std::vector<unsigned char> heatmap_pixels =
                    BuildMaterialHeatmap(frame_pixels, screenshot_width, screenshot_height);
                const bool wrote_screenshot =
                    WritePixelBufferPpm(scenario_config.artifact_dir / screenshot_path,
                                        screenshot_width,
                                        screenshot_height,
                                        frame_pixels);
                const bool wrote_heatmap =
                    WritePixelBufferPpm(scenario_config.artifact_dir / heatmap_path,
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
}

void ScenarioRunnerImpl::captureSkyboxVisual(std::chrono::steady_clock::time_point now) {
    if (scenario_config.skybox_visual_smoke() && scenario_ready && !skybox_visual_capture_written) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        if (progress >= 0.50 && render_pass_stats.skybox_draws > 0) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                const bool sun_on_screen = ProjectDirectionToScreen(*g_camera,
                                                                    screenshot_width,
                                                                    screenshot_height,
                                                                    TowardSunDirection(0.04f),
                                                                    sun_screen_x,
                                                                    sun_screen_y);
                const SkyboxPixelStats skybox_stats = AnalyzeSkyboxPixels(frame_pixels,
                                                                          screenshot_width,
                                                                          screenshot_height,
                                                                          sun_screen_x,
                                                                          sun_screen_y,
                                                                          sun_on_screen);
                const std::string screenshot_path = "screenshots/skybox-visual.ppm";
                if (WritePixelBufferPpm(scenario_config.artifact_dir / screenshot_path,
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
}

void ScenarioRunnerImpl::captureWeatherVisual(std::chrono::steady_clock::time_point now) {
    if (scenario_config.weather_visual_smoke() && scenario_ready &&
        !weather_visual_capture_written) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        const bool capture_baseline =
            !weather_baseline_capture_written && progress >= 0.35 && progress < 0.5;
        const bool capture_weather = weather_baseline_capture_written && progress >= 0.85;
        if ((capture_baseline || capture_weather) && render_pass_stats.skybox_draws > 0) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                const WeatherPixelStats stats =
                    AnalyzeWeatherPixels(frame_pixels, screenshot_width, screenshot_height);
                if (capture_baseline) {
                    const std::string baseline_path = "screenshots/weather-baseline.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / baseline_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        weather_baseline_capture_written = true;
                        weather_baseline_stats = stats;
                    }
                } else {
                    const std::string weather_path = "screenshots/weather-visual.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / weather_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        weather_visual_capture_written = true;
                        WriteWeatherVisualAnalysis(scenario_config.artifact_dir,
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
}

void ScenarioRunnerImpl::captureLightningStrike(std::chrono::steady_clock::time_point now) {
    // lightning strike-frame capture. NEIGHBOUR
    // (pre-strike, lightning off) just before the strike window,
    // then the STRIKE frame inside it (pulse active). The gate
    // asserts the frame-mean luminance PULSE delta + BOLT pixels.
    if (scenario_config.weather_visual_smoke() && scenario_ready &&
        !lightning_strike_capture_written) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        const auto& lit_state = renderPipeline.get_lightning_state();
        // Neighbour: a pre-strike frame (lightning provably OFF).
        const bool capture_neighbor = !lightning_neighbor_captured && progress >= 0.76 &&
                                      progress < 0.80 && !lit_state.active;
        // Strike: a frame inside the window where the pulse is ON.
        const bool capture_strike = lightning_neighbor_captured && lit_state.active &&
                                    lit_state.pulse_intensity > 0.0f && progress >= 0.80 &&
                                    progress < 0.84;
        if ((capture_neighbor || capture_strike) && render_pass_stats.lighting_draws > 0) {
            int sw = 0, sh = 0;
            glfwGetFramebufferSize(window, &sw, &sh);
            if (sw > 0 && sh > 0) {
                std::vector<unsigned char> frame_pixels(static_cast<std::size_t>(sw) *
                                                        static_cast<std::size_t>(sh) * 3u);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, frame_pixels.data());
                const auto stats =
                    Luminumbra::Client::ScenarioHarness::AnalyzeStrikePixels(frame_pixels, sw, sh);
                if (capture_neighbor) {
                    const std::string neighbor_path = "screenshots/lightning-neighbor.ppm";
                    if (WritePixelBufferPpm(
                            scenario_config.artifact_dir / neighbor_path, sw, sh, frame_pixels)) {
                        lightning_neighbor_captured = true;
                        lightning_neighbor_stats = stats;
                    }
                } else {
                    const std::string strike_path = "screenshots/lightning-strike.ppm";
                    if (WritePixelBufferPpm(
                            scenario_config.artifact_dir / strike_path, sw, sh, frame_pixels)) {
                        lightning_strike_capture_written = true;
                        Luminumbra::Client::ScenarioHarness::WriteStrikeVisualAnalysis(
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
}

void ScenarioRunnerImpl::captureCloudShadow(std::chrono::steady_clock::time_point now) {
    if (scenario_config.cloud_shadow_smoke() && scenario_ready && !cloud_shadow_done) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        const auto& cloud_state = renderPipeline.get_cloud_state();
        // Sample the clouds-OFF lighting GPU baseline during the
        // shadow-off warmup window (the camera branch keeps the
        // cast shadow off until progress >= 0.15).
        if (!cloud_shadow_lighting_off_sampled && progress >= 0.10 && progress < 0.15 &&
            render_pass_stats.lighting_draws > 0 && render_pass_stats.lighting_gpu_ms > 0.0) {
            cloud_shadow_lighting_ms_off = render_pass_stats.lighting_gpu_ms;
            cloud_shadow_lighting_off_sampled = true;
        }
        // Two terrain-ROI captures with the cast shadow ON, far
        // enough apart that the wind has drifted a shadow edge
        // across the fixed ROI (t0 ~45%, t1 ~92%).
        const bool capture_t0 = !cloud_shadow_t0_written && progress >= 0.45 && progress < 0.55;
        const bool capture_t1 = cloud_shadow_t0_written && progress >= 0.90;
        if ((capture_t0 || capture_t1) && render_pass_stats.skybox_draws > 0) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                const Luminumbra::Client::ScenarioHarness::CloudShadowPixelStats stats =
                    Luminumbra::Client::ScenarioHarness::AnalyzeCloudShadowPixels(
                        frame_pixels, screenshot_width, screenshot_height);
                if (capture_t0) {
                    const std::string t0_path = "screenshots/cloud-shadow-t0.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / t0_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        cloud_shadow_t0_written = true;
                        cloud_shadow_terrain_luma_t0 = stats.terrain_roi_mean_luminance;
                        cloud_shadow_scroll_t0 = static_cast<double>(cloud_state.scroll_offset.x);
                    }
                } else {
                    const std::string t1_path = "screenshots/cloud-shadow-t1.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / t1_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        Luminumbra::Client::ScenarioHarness::CloudShadowResult result;
                        result.terrain_roi_luminance_t0 = cloud_shadow_terrain_luma_t0;
                        result.terrain_roi_luminance_t1 = stats.terrain_roi_mean_luminance;
                        result.terrain_roi_luminance_delta = std::abs(
                            result.terrain_roi_luminance_t1 - result.terrain_roi_luminance_t0);
                        result.sky_mean_luminance = stats.sky_mean_luminance;
                        result.sky_horizontal_gradient_mean = stats.sky_horizontal_gradient_mean;
                        result.cloud_layer_present = stats.sky_horizontal_gradient_mean > 0.0;
                        result.lighting_gpu_ms_clouds_off = cloud_shadow_lighting_ms_off;
                        result.lighting_gpu_ms_clouds_on = render_pass_stats.cloud_shadow_gpu_ms;
                        result.cloud_shadow_added_ms = std::max(
                            0.0,
                            result.lighting_gpu_ms_clouds_on - result.lighting_gpu_ms_clouds_off);
                        result.gpu_timers_supported = render_pass_stats.gpu_timers_supported &&
                                                      cloud_shadow_lighting_off_sampled &&
                                                      render_pass_stats.cloud_shadow_gpu_ms > 0.0;
                        result.coverage_amount = cloud_state.coverage_amount;
                        result.shadow_strength = cloud_state.shadow_strength;
                        result.scroll_offset_t0 = cloud_shadow_scroll_t0;
                        result.scroll_offset_t1 = static_cast<double>(cloud_state.scroll_offset.x);
                        Luminumbra::Client::ScenarioHarness::WriteCloudShadowAnalysis(
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
}

void ScenarioRunnerImpl::captureParticleDeterminism(std::chrono::steady_clock::time_point now) {
    if (scenario_config.particle_emitter_determinism_smoke() && scenario_ready &&
        !particle_determinism_capture_written) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
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
                    world_seed =
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(ws->get_seed()));
                }
                const std::uint64_t world_tick = gameSession->GetSimulationTickCount();

                particles->rebuild_emitter_descriptors(world_seed, world_tick);
                const auto descriptors_a = particles->emitter_descriptors();
                const std::uint64_t hash_a = particles->emitter_descriptor_hash();

                particles->rebuild_emitter_descriptors(world_seed, world_tick);
                const auto descriptors_b = particles->emitter_descriptors();
                const std::uint64_t hash_b = particles->emitter_descriptor_hash();

                const bool byte_equal =
                    descriptors_a.size() == descriptors_b.size() &&
                    std::memcmp(
                        descriptors_a.data(),
                        descriptors_b.data(),
                        descriptors_a.size() *
                            sizeof(Luminumbra::Rendering::ParticlePass::EmitterDescriptor)) == 0;

                Luminumbra::Client::ScenarioHarness::ParticleDeterminismResult result;
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
                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                    const std::string screenshot_path = "screenshots/particle-determinism.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / screenshot_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        particle_determinism_capture_written = true;
                        WriteParticleEmitterDeterminismAnalysis(scenario_config.artifact_dir,
                                                                screenshot_path,
                                                                result,
                                                                render_pass_stats);
                    }
                }
            }
        }
    }
}

void ScenarioRunnerImpl::captureAtmosMotion(std::chrono::steady_clock::time_point now) {
    //  dump consecutive STORM
    // frames for the motion clip. Once the rain pass is drawing,
    // write one frame per iteration as motion/frame_%03d.ppm until
    // kAtmosMotionFrameCount frames are captured.
    if (atmos_motion_capture && scenario_config.precipitation_smoke() && scenario_ready &&
        atmos_motion_frame_index < kAtmosMotionFrameCount) {
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        const double motion_now_s =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const bool motion_interval_elapsed =
            atmos_motion_last_capture_s < 0.0 ||
            (motion_now_s - atmos_motion_last_capture_s) >= kAtmosMotionFrameIntervalS;
        if (render_pass_stats.particle_draws > 0 && motion_interval_elapsed) {
            int mw = 0;
            int mh = 0;
            glfwGetFramebufferSize(window, &mw, &mh);
            if (mw > 0 && mh > 0) {
                std::vector<unsigned char> mpx(static_cast<std::size_t>(mw) *
                                               static_cast<std::size_t>(mh) * 3u);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, mw, mh, GL_RGB, GL_UNSIGNED_BYTE, mpx.data());
                char name[32];
                std::snprintf(
                    name, sizeof(name), "motion/frame_%03d.ppm", atmos_motion_frame_index);
                if (WritePixelBufferPpm(scenario_config.artifact_dir / name, mw, mh, mpx)) {
                    ++atmos_motion_frame_index;
                    atmos_motion_last_capture_s = motion_now_s;
                }
            }
        }
    }
}

void ScenarioRunnerImpl::captureFoliageVisual(std::chrono::steady_clock::time_point now) {
    if (scenario_config.foliage_visual_smoke() && scenario_ready && !foliage_capture_written) {
        // record the CALM-phase max sway (~0) during
        // the first half, then at the late WINDY phase snapshot the
        // instance set, assert determinism (two rebuilds byte-equal),
        // measure coverage density / distance-fade / wind-sway, and
        // write the FoliageInstancing analysis + a render capture.
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        auto* foliage = renderPipeline.foliage();
        // Sample the calm-phase max sway (zero wind) once mid-first-half.
        if (foliage != nullptr && !foliage_calm_sampled && progress >= 0.30 && progress < 0.45 &&
            render_pass_stats.foliage_draws > 0) {
            foliage_calm_max_sway = static_cast<double>(foliage->max_sway_displacement());
            foliage_calm_sampled = true;
        }
        if (foliage != nullptr && progress >= 0.85 && foliage->readback_enabled() &&
            render_pass_stats.foliage_draws > 0 && render_pass_stats.foliage_instances_drawn > 0) {
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
                world_seed = static_cast<std::uint64_t>(static_cast<std::uint32_t>(ws->get_seed()));
                // Biome density at the camera column (the scatter's
                // dominant local biome) for the coverage band check.
                const Luminumbra::Vec3 cam(
                    g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
                probe_biome = ws->BiomeIdAt(cam.x, cam.z);
                biome_density = ws->biomes_enabled()
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
            result.measured_density = std::clamp(
                static_cast<double>(result.instances_within_ring) / nominal_full, 0.0, 1.0);
            result.biome_density = biome_density;
            result.biome_density_band = 0.6; // loose band (footprint-dependent)
            result.calm_max_sway = foliage_calm_max_sway;
            result.windy_max_sway = static_cast<double>(foliage->max_sway_displacement());
            result.sway_responds = result.windy_max_sway > result.calm_max_sway;
            result.foliage_gpu_ms = render_pass_stats.foliage_gpu_ms;
            result.foliage_budget_ms = 0.6;
            result.gpu_timers_supported =
                render_pass_stats.gpu_timers_supported && render_pass_stats.foliage_gpu_ms > 0.0;
            result.foliage_draws = render_pass_stats.foliage_draws;
            result.foliage_instances_drawn = render_pass_stats.foliage_instances_drawn;

            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                const std::string screenshot_path = "screenshots/foliage-instancing.ppm";
                if (WritePixelBufferPpm(scenario_config.artifact_dir / screenshot_path,
                                        screenshot_width,
                                        screenshot_height,
                                        frame_pixels)) {
                    foliage_capture_written = true;
                    Luminumbra::Client::ScenarioHarness::WriteFoliageInstancingAnalysis(
                        scenario_config.artifact_dir, screenshot_path, result, render_pass_stats);
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
                refusal_reason = "readback-disabled: instance probes are vacuous (play-mode "
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
                  {"foliage_instances_drawn", render_pass_stats.foliage_instances_drawn}}}};
            std::ofstream refusal_out(scenario_config.artifact_dir /
                                      "foliage-instancing-analysis.json");
            refusal_out << std::setw(2) << refusal << '\n';
            LUMINUMBRA_CORE_ERROR("FoliageInstancing: REFUSAL analysis written ({})",
                                  refusal_reason);
        }
    }
}

void ScenarioRunnerImpl::capturePrecipitation(std::chrono::steady_clock::time_point now) {
    if (scenario_config.precipitation_smoke() && scenario_ready && !precip_windy_capture_written) {
        // capture a CALM rain frame (first half,
        // zero wind -> vertical fall) and a WINDY rain frame
        // (second half, wind-advected slant). The analysis on
        // the windy capture asserts precip particles are present
        // in both AND that the streaks slant with wind.
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        // Capture once rain is visibly rendering (the pass
        // reports draws). Calm: late in the first half so the
        // field has filled and settled to a steady vertical
        // fall. Windy: late in the second half so the slant has
        // fully developed after the wind switch at progress 0.5.
        const bool capture_calm = !precip_calm_capture_written && progress >= 0.35 &&
                                  progress < 0.5 && render_pass_stats.particle_draws > 0;
        const bool capture_windy =
            precip_calm_capture_written && progress >= 0.9 && render_pass_stats.particle_draws > 0;
        if (capture_calm || capture_windy) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                const PrecipPixelStats stats =
                    AnalyzePrecipPixels(frame_pixels, screenshot_width, screenshot_height);
                if (capture_calm) {
                    const std::string calm_path = "screenshots/precip-calm.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / calm_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        precip_calm_capture_written = true;
                        precip_calm_stats = stats;
                        precip_calm_render_pass = render_pass_stats;
                    }
                } else {
                    const std::string windy_path = "screenshots/precip-windy.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / windy_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        precip_windy_capture_written = true;
                        WritePrecipitationAnalysis(scenario_config.artifact_dir,
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
}

void ScenarioRunnerImpl::captureTimeOfDaySweep(std::chrono::steady_clock::time_point now) {
    if (scenario_config.timeofday_sweep_smoke() && scenario_ready && !timeofday_analysis_final) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
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
            if (!timeofday_season_captures_written[static_cast<std::size_t>(i)] &&
                progress >= TimeOfDaySweepCapturePlanAt(i).threshold) {
                capture_index = i;
                break;
            }
        }
        // The emissive capture follows the SUMMER night (season 0,
        // index 2) so the existing emissive night check is unchanged;
        // it fires once that night is in and the camera has settled.
        const bool capture_emissive =
            timeofday_season_captures_written[2] && timeofday_emissive_target.found &&
            !timeofday_emissive_capture_written && progress >= 0.45 && progress < 0.5;
        //  the pinned-phase capture must
        // wait for the sky dome to settle at the new sun pin (the
        // sun-view LUT refreshes lazily, so the first frame after a
        // long sun jump still carries the prior phase's dome). Require
        // a handful of consecutive settle frames at this exact pin.
        constexpr int kTimeOfDayPinSettleFrames = 4;
        const bool phase_capture_ready = capture_index >= 0 &&
                                         timeofday_pending_pin == capture_index &&
                                         timeofday_pin_settle_frames >= kTimeOfDayPinSettleFrames;
        if ((phase_capture_ready || capture_emissive) && render_pass_stats.skybox_draws > 0) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                const TimeOfDayPixelStats stats =
                    AnalyzeTimeOfDayPixels(frame_pixels, screenshot_width, screenshot_height);
                if (phase_capture_ready) {
                    const SeasonCapturePlan& plan = TimeOfDaySweepCapturePlanAt(capture_index);
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / plan.file,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        timeofday_season_captures_written[static_cast<std::size_t>(capture_index)] =
                            true;
                        TimeOfDayPhaseCapture cap;
                        cap.name = plan.phase_name;
                        cap.time_of_day = plan.phase_time;
                        cap.file = plan.file;
                        cap.stats = stats;
                        cap.season_label = plan.season_label;
                        cap.season_index = plan.season_index;
                        cap.season_phase = renderPipeline.get_season_phase();
                        cap.sun_elevation_rad = renderPipeline.get_sun_elevation_rad();
                        cap.season_sun_declination_rad =
                            renderPipeline.get_season_sun_declination();
                        cap.season_tick = renderPipeline.get_season_tick();
                        timeofday_phase_captures.push_back(cap);
                    }
                } else {
                    const std::string emissive_path = "screenshots/timeofday-night-emissive.ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / emissive_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        timeofday_emissive_capture_written = true;
                        timeofday_emissive_stats = stats;
                    }
                }
                const bool all_six =
                    timeofday_season_captures_written[0] && timeofday_season_captures_written[1] &&
                    timeofday_season_captures_written[2] && timeofday_season_captures_written[3] &&
                    timeofday_season_captures_written[4] && timeofday_season_captures_written[5];
                if (all_six) {
                    // Final once the optional emissive capture is in
                    // (or no surface emissive target exists).
                    timeofday_analysis_final =
                        !timeofday_emissive_target.found || timeofday_emissive_capture_written;
                    WriteTimeOfDaySweepAnalysis(scenario_config.artifact_dir,
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
}

void ScenarioRunnerImpl::capturePlayerView(std::chrono::steady_clock::time_point now) {
    if (scenario_config.player_view_smoke() && scenario_ready && !player_view_stations.empty() &&
        g_camera) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        // Mirror the camera branch's warmup-adjusted schedule.
        const double warmup_seconds = std::min(8.0, duration * 0.2);
        const double effective_seconds = std::max(0.0, elapsed_play_seconds - warmup_seconds);
        const double progress =
            std::clamp(effective_seconds / std::max(1.0, duration - warmup_seconds), 0.0, 0.999);
        const std::size_t station_count = player_view_stations.size();
        const std::size_t wall_clock_index =
            std::min(station_count - 1u,
                     static_cast<std::size_t>(progress * static_cast<double>(station_count)));
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
        const std::size_t station_index = first_unwritten >= station_count
                                              ? wall_clock_index
                                              : std::min(wall_clock_index, first_unwritten);
        const double station_progress = wall_clock_index > station_index
                                            ? 1.0
                                            : progress * static_cast<double>(station_count) -
                                                  static_cast<double>(station_index);
        if (!player_view_captures_written[station_index] && station_progress >= 0.7) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                if (glm::dot(horizontal_forward, horizontal_forward) > 1.0e-6f) {
                    horizontal_forward = glm::normalize(horizontal_forward);
                    double horizon_x_norm = 0.0;
                    double horizon_y_norm = 0.0;
                    if (ProjectDirectionToScreen(*g_camera,
                                                 screenshot_width,
                                                 screenshot_height,
                                                 horizontal_forward,
                                                 horizon_x_norm,
                                                 horizon_y_norm)) {
                        horizon_row_from_top = static_cast<int>(horizon_y_norm * screenshot_height);
                    } else {
                        // Horizon outside the frame: pitched far up
                        // (all sky legitimate, ROI empty) or far down
                        // (all terrain, full-frame ROI).
                        horizon_row_from_top = g_camera->Pitch > 0.0f ? screenshot_height : 0;
                    }
                }

                PlayerViewStationCapture capture;
                capture.station = player_view_stations[station_index];
                capture.station.yaw_degrees = g_camera->Yaw;
                capture.station.pitch_degrees = g_camera->Pitch;
                capture.sky = AnalyzePlayerViewPixels(
                    frame_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
                capture.holes =
                    AnalyzeLodHolePixels(frame_pixels, screenshot_width, screenshot_height);
                capture.coverage =
                    gameSession->GetWorldSystem()->get_frustum_surface_coverage_stats(
                        g_camera->Position,
                        ExtractCameraFrustumPlanes(*g_camera, screenshot_width, screenshot_height),
                        192.0f);

                const std::string relative_path =
                    "screenshots/player-view-" + player_view_stations[station_index].name + ".ppm";
                if (WritePixelBufferPpm(scenario_config.artifact_dir / relative_path,
                                        screenshot_width,
                                        screenshot_height,
                                        frame_pixels)) {
                    capture.file = relative_path;
                    player_view_station_captures.push_back(capture);
                    player_view_captures_written[station_index] = true;
                    WritePlayerViewAnalysis(
                        scenario_config.artifact_dir,
                        scenario_world_type,
                        elapsed_play_seconds,
                        player_view_station_captures,
                        station_count,
                        gameSession->GetWorldSystem()->get_runtime_chunk_stats(),
                        player_view_sky_enforced);
                }
            }
        }
    }
}

void ScenarioRunnerImpl::captureFarLodHorizon(std::chrono::steady_clock::time_point now,
                                              float deltaTime) {
    if (scenario_config.farlod_horizon_smoke() && scenario_ready &&
        !farlod_horizon_stations.empty() && g_camera) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 0.999);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();

        // gbuffer GPU samples: phase-A tail = far-disabled
        // baseline; run tail = far-enabled comparison.
        if (render_pass_stats.gpu_timers_supported && render_pass_stats.gbuffer_gpu_ms > 0.0) {
            if (progress >= 0.15 && progress < kFarLodHorizonPhaseSplit) {
                farlod_baseline_gbuffer_samples.push_back(render_pass_stats.gbuffer_gpu_ms);
            } else if (progress >= 0.85) {
                farlod_far_gbuffer_samples.push_back(render_pass_stats.gbuffer_gpu_ms);
            }
        }

        if (progress >= kFarLodHorizonPhaseSplit) {
            const double sweep =
                (progress - kFarLodHorizonPhaseSplit) / (1.0 - kFarLodHorizonPhaseSplit);
            const std::size_t station_count = farlod_horizon_stations.size();
            const std::size_t time_station_index =
                std::min(station_count - 1u,
                         static_cast<std::size_t>(sweep * static_cast<double>(station_count)));
            // capture the station the
            // rendered back buffer actually shows (the camera-apply
            // clamp), not the bare time index. station_progress is
            // only meaningful when station_index == time_station_index.
            const std::size_t station_index = farlod_horizon_applied_station;
            const double station_progress =
                sweep * static_cast<double>(station_count) - static_cast<double>(station_index);
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
                glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                    if (glm::dot(horizontal_forward, horizontal_forward) > 1.0e-6f) {
                        horizontal_forward = glm::normalize(horizontal_forward);
                        double horizon_x_norm = 0.0;
                        double horizon_y_norm = 0.0;
                        if (ProjectDirectionToScreen(*g_camera,
                                                     screenshot_width,
                                                     screenshot_height,
                                                     horizontal_forward,
                                                     horizon_x_norm,
                                                     horizon_y_norm)) {
                            horizon_row_from_top =
                                static_cast<int>(horizon_y_norm * screenshot_height);
                        } else {
                            horizon_row_from_top = g_camera->Pitch > 0.0f ? screenshot_height : 0;
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
                    capture.sky = AnalyzePlayerViewPixels(
                        frame_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
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
                        capture.boundary = AnalyzeFarLodBoundaryBand(frame_pixels,
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
                            band_total_px > 0 ? static_cast<double>(band_water_px) /
                                                    static_cast<double>(band_total_px)
                                              : 0.0;
                        capture.boundary_band_sand_flat_pixels = band_sand_flat_px;
                        capture.boundary_band_sand_flat_ratio =
                            band_total_px > 0 ? static_cast<double>(band_sand_flat_px) /
                                                    static_cast<double>(band_total_px)
                                              : 0.0;
                    }
                    // above-horizon thin-sliver scan (far ON).
                    capture.sky_sliver = AnalyzeFarLodHorizonSkySliver(
                        frame_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
                    // snapshot the far-LOD
                    // scheduler stats for the ON frame BEFORE the paired
                    // far-OFF render below - that render updates
                    // farlod->stats to the disabled frame (no wanted/
                    // resident regions, zero draws), which is not the
                    // state this capture asserts on.
                    if (const auto* farlod = renderPipeline.farlod()) {
                        const auto& farlod_stats = farlod->stats();
                        capture.regions_wanted = farlod_stats.regions_wanted;
                        capture.regions_resident = farlod_stats.regions_resident;
                        capture.regions_missing = farlod_stats.regions_missing;
                        capture.resident_bytes = farlod_stats.resident_bytes;
                        capture.region_draws = farlod_stats.region_draws;
                        capture.far_indices_drawn = farlod_stats.indices_drawn;
                        // far water sheet draw counts.
                        capture.water_sheet_draws = farlod_stats.water_sheet_draws;
                        capture.water_sheet_indices = farlod_stats.water_sheet_indices;
                        //  far-LOD scheduler diagnostics at capture time.
                        capture.builds_dispatched = farlod_stats.builds_dispatched;
                        capture.builds_integrated_ok = farlod_stats.builds_integrated_ok;
                        capture.builds_integrated_failed = farlod_stats.builds_integrated_failed;
                        capture.builds_failed_total = farlod_stats.builds_failed_total;
                        capture.builds_completed_total = farlod_stats.builds_completed_total;
                        capture.evictions_this_frame = farlod_stats.evictions_this_frame;
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
                        renderPipeline.render_frame(gameSession->GetRegistry(),
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
                        const auto off_sliver = AnalyzeFarLodHorizonSkySliver(
                            off_pixels, screenshot_width, screenshot_height, horizon_row_from_top);
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
                        capture.far_attributable_sliver_px = attributable_sliver.tallest_sliver_px;
                    } else {
                        // No far-OFF sample available: fall back to the
                        // raw far-ON sliver (conservative - never under-
                        // reports an attributable streak).
                        capture.far_attributable_sliver_px = capture.sky_sliver.tallest_sliver_px;
                    }
                    if (station_index < farlod_horizon_far_off_sliver_px.size()) {
                        farlod_horizon_far_off_sliver_px[station_index] = far_off_sliver_px;
                    }
                    capture.far_off_sliver_px = far_off_sliver_px;

                    const std::string relative_path = "screenshots/farlod-horizon-" +
                                                      farlod_horizon_stations[station_index].name +
                                                      ".ppm";
                    if (WritePixelBufferPpm(scenario_config.artifact_dir / relative_path,
                                            screenshot_width,
                                            screenshot_height,
                                            frame_pixels)) {
                        capture.file = relative_path;
                        farlod_horizon_station_captures.push_back(capture);
                        farlod_horizon_captures_written[station_index] = true;
                        WriteFarLodHorizonAnalysis(scenario_config.artifact_dir,
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
}

void ScenarioRunnerImpl::captureSkinnedMeshVisual(std::chrono::steady_clock::time_point now) {
    if (scenario_config.skinned_mesh_visual_smoke() && scenario_ready &&
        !skinned_mesh_analysis_written && skinned_mesh_visual_target.spawned) {
        // capture A at 50% progress, capture B at
        // 85%; the two clip times must differ visibly in
        // the rig ROI.
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        const bool want_capture_a = !skinned_mesh_capture_a_written && progress >= 0.50;
        const bool want_capture_b = skinned_mesh_capture_a_written && progress >= 0.85;
        if (want_capture_a || want_capture_b) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
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
                capture.animation_time_seconds =
                    SkinnedMeshVisualAnimationTime(gameSession.get(), skinned_mesh_visual_target);
                capture.skinned_draws = render_pass_stats.skinned_draws;
                capture.skinned_indices_drawn = render_pass_stats.skinned_indices_drawn;
                const std::string relative_path = want_capture_a ? "screenshots/skinned-mesh-a.ppm"
                                                                 : "screenshots/skinned-mesh-b.ppm";
                if (WritePixelBufferPpm(scenario_config.artifact_dir / relative_path,
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
                        WriteSkinnedMeshVisualAnalysis(scenario_config.artifact_dir,
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
}

void ScenarioRunnerImpl::captureRemoteAvatarArtifact() {
    if (scenario_config.skinned_mesh_visual_smoke() && scenario_config.replicated &&
        scenario_config.avatars >= 2 && scenario_ready && skinned_mesh_visual_target.spawned &&
        replicated_demo.ready() && replicated_avatar_render_seconds >= (2.0 / 15.0) &&
        !remote_avatar_render_artifact_written) {
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        if (render_pass_stats.skinned_draws >= skinned_mesh_visual_target.all_entities.size()) {
            const auto clamp_to_u32 = [](std::size_t value) -> std::uint32_t {
                return static_cast<std::uint32_t>(std::min<std::size_t>(
                    value, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
            };
            const std::uint32_t local_client_id = 1u;
            const std::uint32_t snapshot_count = clamp_to_u32(static_cast<std::size_t>(
                std::max(1.0, std::floor(replicated_avatar_render_seconds * 15.0))));
            std::vector<luminumbra::network::NetworkRemoteAvatarRenderPose> poses;
            poses.reserve(skinned_mesh_visual_target.all_entities.size());
            auto& reg = gameSession->GetRegistry();
            for (std::size_t i = 0; i < skinned_mesh_visual_target.all_entities.size(); ++i) {
                const std::uint32_t client_id = clamp_to_u32(i + 1u);
                luminumbra::network::NetworkRemoteAvatarRenderPose pose;
                pose.clientId = client_id;
                pose.serverTick = snapshot_count;
                pose.snapshotSequence = pose.serverTick;
                pose.remote = client_id != local_client_id;
                pose.interpolated = pose.remote;
                const auto ent = skinned_mesh_visual_target.all_entities[i];
                if (reg.valid(ent) && reg.all_of<Luminumbra::Components::TransformComponent>(ent)) {
                    pose.rendered = true;
                    const auto& tf = reg.get<Luminumbra::Components::TransformComponent>(ent);
                    pose.positionXMm =
                        static_cast<int>(std::lround(static_cast<double>(tf.position.x) * 1000.0));
                    pose.positionYMm =
                        static_cast<int>(std::lround(static_cast<double>(tf.position.y) * 1000.0));
                    pose.positionZMm =
                        static_cast<int>(std::lround(static_cast<double>(tf.position.z) * 1000.0));
                }
                poses.push_back(pose);
            }
            const auto report = luminumbra::network::BuildNetworkRemoteAvatarRenderReport(
                local_client_id,
                poses,
                clamp_to_u32(static_cast<std::size_t>(std::max(2, scenario_config.avatars))),
                snapshot_count,
                clamp_to_u32(std::min<std::size_t>(skinned_mesh_visual_target.all_entities.size(),
                                                   render_pass_stats.skinned_draws)),
                clamp_to_u32(render_pass_stats.skinned_draws),
                clamp_to_u32(render_pass_stats.skinned_indices_drawn));
            remote_avatar_render_artifact_written =
                luminumbra::network::WriteNetworkRemoteAvatarRenderArtifact(
                    (scenario_config.artifact_dir / "remote-avatar-render.json").string(), report);
            if (!remote_avatar_render_artifact_written) {
                scenario_failed = true;
                scenario_failure_reason = "remote_avatar_render_artifact_failed";
            }
        }
    }
}

void ScenarioRunnerImpl::captureShowcaseVideo(std::chrono::steady_clock::time_point now) {
    //  video proof: when the avatar SHOWCASE row is up
    // (avatars>=2), walk the avatars laterally and dump a frame
    // sequence (motion/frame_%03d.ppm) for an ffmpeg clip. Gated on
    // avatars>=2 so the single-rig gate run never dumps frames.
    if (scenario_config.skinned_mesh_visual_smoke() && scenario_config.avatars >= 2 &&
        scenario_ready && skinned_mesh_visual_target.spawned && showcase_video_frame < 120) {
        const double vnow = std::chrono::duration<double>(now - scenario_play_started_at).count();
        // WARM-UP: don't capture the first ~3 s -- let async far-LOD,
        // meshing and aerial-perspective settle so the world is fully
        // loaded in every captured frame (owner: proper loading before
        // capture). The per-frame EnsureSurfaceReadyNear above pulls the
        // near/mid surface ready; this covers the async far field.
        constexpr double kShowcaseWarmupS = 3.0;
        const bool warmed_up = vnow >= kShowcaseWarmupS;
        const bool interval_ok =
            showcase_video_last_s < 0.0 || (vnow - showcase_video_last_s) >= 0.05;
        if (warmed_up && interval_ok) {
            auto& reg = gameSession->GetRegistry();
            auto* world_sys = gameSession->GetWorldSystem();
            if (scenario_config.wildlife && wildlife_setup) {
                // Cinematic FSM on CAPTURE-relative time T (frames dumped *
                // 0.05 s): 0..~2.3 s animal walks to water; ~2.5 s human
                // shoots; arrow arcs ~1 s; on landing the splash SCARES the
                // animal -> it flees.
                const float T = static_cast<float>(showcase_video_frame) * 0.05f;
                const float dt = 0.05f;
                const auto e_animal = skinned_mesh_visual_target.all_entities[0];
                const auto e_human = skinned_mesh_visual_target.all_entities[1];
                auto ground = [&](glm::vec3 p) {
                    if (world_sys)
                        p.y = world_sys->GetTerrainHeightAt(p.x, p.z);
                    return p;
                };
                auto set_tf = [&](Luminumbra::EntityID e,
                                  const glm::vec3& p,
                                  const glm::vec3& dir) {
                    if (!reg.valid(e) || !reg.all_of<Luminumbra::Components::TransformComponent>(e))
                        return;
                    auto& tf = reg.get<Luminumbra::Components::TransformComponent>(e);
                    tf.position = p;
                    const glm::vec3 d(dir.x, 0.0f, dir.z);
                    if (glm::length(d) > 0.01f)
                        tf.rotation = glm::angleAxis(std::atan2(d.x, d.z), glm::vec3(0, 1, 0));
                };
                glm::vec3 to_water = wildlife_water - wildlife_animal;
                to_water.y = 0.0f;
                const glm::vec3 seek_dir =
                    glm::length(to_water) > 0.01f ? glm::normalize(to_water) : glm::vec3(1, 0, 0);
                if (wildlife_phase == 0) { // SEEK water
                    if (glm::length(to_water) > 2.5f)
                        wildlife_animal += seek_dir * 3.0f * dt;
                    wildlife_animal = ground(wildlife_animal);
                    set_tf(e_animal, wildlife_animal, seek_dir);
                    if (T >= 2.5f) { // human looses the arrow toward a spot
                                     // beside the animal
                        const glm::vec3 perp(-seek_dir.z, 0.0f, seek_dir.x);
                        const glm::vec3 target = wildlife_animal + perp * 2.0f; // BESIDE, not at
                        wildlife_arrow_pos = wildlife_human + glm::vec3(0.0f, 1.3f, 0.0f);
                        glm::vec3 ah = target - wildlife_arrow_pos;
                        ah.y = 0.0f;
                        const glm::vec3 adir =
                            glm::length(ah) > 0.01f ? glm::normalize(ah) : seek_dir;
                        wildlife_arrow_vel = adir * 13.0f + glm::vec3(0.0f, 4.5f, 0.0f);
                        set_tf(e_human, wildlife_human,
                               adir); // human faces the shot
                        wildlife_phase = 1;
                    }
                } else if (wildlife_phase == 1) { // ARROW in flight
                    wildlife_arrow_vel.y -= 9.81f * dt;
                    wildlife_arrow_pos += wildlife_arrow_vel * dt;
                    const float terr = world_sys ? world_sys->GetTerrainHeightAt(
                                                       wildlife_arrow_pos.x, wildlife_arrow_pos.z)
                                                 : wildlife_arrow_pos.y;
                    set_tf(wildlife_arrow_entity, wildlife_arrow_pos, wildlife_arrow_vel);
                    if (wildlife_arrow_pos.y <= terr) { // THWACK beside the animal -> scare
                        wildlife_arrow_pos.y = terr;
                        set_tf(wildlife_arrow_entity, wildlife_arrow_pos, glm::vec3(0, 0, 1));
                        glm::vec3 away = wildlife_animal - wildlife_arrow_pos;
                        away.y = 0.0f;
                        wildlife_flee_dir =
                            glm::length(away) > 0.01f ? glm::normalize(away) : -seek_dir;
                        wildlife_phase = 2;
                    }
                    set_tf(e_animal, wildlife_animal,
                           seek_dir);                                 // animal still drinking
                } else {                                              // FLEE
                    wildlife_animal += wildlife_flee_dir * 6.0f * dt; // bolts away, faster
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
                auto view = reg.view<Luminumbra::Components::TransformComponent,
                                     Luminumbra::Components::SkinnedMeshComponent>();
                const float step_m = 0.05f; // ~1 m/s at 20 dumps/s
                for (auto e : view) {
                    auto& pos = view.get<Luminumbra::Components::TransformComponent>(e).position;
                    pos.z += step_m;
                    if (world_sys)
                        pos.y = world_sys->GetTerrainHeightAt(pos.x, pos.z);
                }
            }
            int vw = 0, vh = 0;
            glfwGetFramebufferSize(window, &vw, &vh);
            if (vw > 0 && vh > 0) {
                std::vector<unsigned char> vpx(static_cast<std::size_t>(vw) *
                                               static_cast<std::size_t>(vh) * 3u);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, vpx.data());
                char vname[40];
                std::snprintf(vname, sizeof(vname), "motion/frame_%03d.ppm", showcase_video_frame);
                if (WritePixelBufferPpm(scenario_config.artifact_dir / vname, vw, vh, vpx)) {
                    ++showcase_video_frame;
                    showcase_video_last_s = vnow;
                }
            }
        }
    }
}

void ScenarioRunnerImpl::captureCreatureSlice(std::chrono::steady_clock::time_point now) {
    if (scenario_config.creature_slice_smoke() && scenario_ready &&
        !creature_slice_analysis_written && creature_slice_scene.spawned) {
        // planner state + screenshot before the
        // stimulus (45%) and after it (85%).
        const double elapsed_play_seconds =
            std::chrono::duration<double>(now - scenario_play_started_at).count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        const auto& render_pass_stats = renderPipeline.get_last_render_pass_stats();
        const bool want_before =
            !creature_slice_before_written && progress >= 0.45 && progress < 0.55;
        const bool want_after = creature_slice_before_written &&
                                creature_slice_scene.stimulus_spawned && progress >= 0.85;
        if (want_before || want_after) {
            int screenshot_width = 0;
            int screenshot_height = 0;
            glfwGetFramebufferSize(window, &screenshot_width, &screenshot_height);
            if (screenshot_width > 0 && screenshot_height > 0) {
                CreatureSliceCapture capture;
                capture.elapsed_seconds = elapsed_play_seconds;
                capture.plan = ProbeCreatureSlicePlan(gameSession.get(), creature_slice_scene);
                capture.skinned_draws = render_pass_stats.skinned_draws;
                capture.skinned_indices_drawn = render_pass_stats.skinned_indices_drawn;
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
                    Luminumbra::Vec3 creature_world = creature_slice_scene.creature_position;
                    if (gameSession->GetRegistry().valid(creature_slice_scene.creature)) {
                        if (const auto* tf =
                                gameSession->GetRegistry()
                                    .try_get<const Luminumbra::Components::TransformComponent>(
                                        creature_slice_scene.creature)) {
                            creature_world = tf->position;
                        }
                    }
                    const glm::mat4 view = g_camera->GetViewMatrix();
                    const glm::mat4 proj =
                        g_camera->GetProjectionMatrix(screenshot_width, screenshot_height);
                    const glm::vec4 clip =
                        proj * view *
                        glm::vec4(creature_world.x,
                                  creature_world.y + 1.0f, // body center, above feet
                                  creature_world.z,
                                  1.0f);
                    int sx = -1, sy = -1;
                    if (clip.w > 0.0f) {
                        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                        sx = static_cast<int>((ndc.x * 0.5f + 0.5f) *
                                              static_cast<float>(screenshot_width));
                        sy = static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) *
                                              static_cast<float>(screenshot_height));
                    }
                    // project the emissive glow_bloom stimulus prop too so
                    // the glow-halo (bright core -> falloff ring) can be
                    // measured.
                    int stim_x = -1, stim_y = -1;
                    if (creature_slice_scene.stimulus_spawned) {
                        const Luminumbra::Vec3 sp = creature_slice_scene.stimulus_position;
                        const glm::vec4 sclip =
                            proj * view * glm::vec4(sp.x, sp.y + 0.5f, sp.z, 1.0f);
                        if (sclip.w > 0.0f) {
                            const glm::vec3 sndc = glm::vec3(sclip) / sclip.w;
                            stim_x = static_cast<int>((sndc.x * 0.5f + 0.5f) *
                                                      static_cast<float>(screenshot_width));
                            stim_y = static_cast<int>((1.0f - (sndc.y * 0.5f + 0.5f)) *
                                                      static_cast<float>(screenshot_height));
                        }
                    }
                    capture.composition = AnalyzeCreatureSliceComposition(
                        comp_pixels, screenshot_width, screenshot_height, sx, sy, stim_x, stim_y);
                }
                const std::string relative_path = want_before
                                                      ? "screenshots/creature-slice-before.ppm"
                                                      : "screenshots/creature-slice-after.ppm";
                if (WriteBackbufferPpm(scenario_config.artifact_dir / relative_path,
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
}

void ScenarioRunnerImpl::onShutdown() {
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
}

} // namespace Luminumbra::Client::ScenarioHarness
