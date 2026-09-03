// Scenario driving hooks: the loop-top readiness watchdog and the IN_GAME
// per-scenario camera/scene driving chain, moved VERBATIM out of
// main_client.cpp's frame loop behind the ScenarioRunner seam. See
// core/ScenarioRunnerImpl.h for the TU layout.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "core/ScenarioRunnerImpl.h"

#include "core/Log.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "rendering/Camera.h"
#include "rendering/FarLodSystem.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The moved code names the scenario-harness and app vocabulary unqualified,
// exactly as it did inside main_client.cpp (file-level using-directives).
using namespace Luminumbra::Client::ScenarioHarness;
using namespace Luminumbra::Client::App;

namespace Luminumbra::Client::ScenarioHarness {

ScenarioRunnerImpl::ScenarioRunnerImpl(const ScenarioFrameContext& context)
    : window(context.window)
    , root_dir(context.root_dir)
    , root_path_str(context.root_path_str)
    , scenario_config(context.scenario_config)
    , runtime_state_recorder(context.runtime_state_recorder)
    , lod_ground_frame_recorder(context.lod_ground_frame_recorder)
    , jobSystem(context.jobSystem)
    , gameSession(context.gameSession)
    , renderPipeline(context.renderPipeline)
    , g_camera(context.g_camera)
    , g_app(context.g_app)
    , scenario_world_type(context.scenario_world_type)
    , scenario_failed(context.scenario_failed)
    , scenario_failure_reason(context.scenario_failure_reason)
    , scenario_ready(context.scenario_ready)
    , scenario_frame_count(context.scenario_frame_count)
    , scenario_play_started_at(context.scenario_play_started_at)
    , last_readiness_report(context.last_readiness_report)
    , exit_code(context.exit_code) {}

std::unique_ptr<ScenarioRunner> CreateScenarioRunner(const ScenarioFrameContext& context) {
    return std::make_unique<ScenarioRunnerImpl>(context);
}

void ScenarioRunnerImpl::onLoopTop() {
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
}

ScenarioRunner::InGameDrive ScenarioRunnerImpl::onGameStateInGame(float deltaTime) {
    // The original IN_GAME scenario chain, verbatim: the persistence phase ran
    // first (and broke out of the case), then one single if/else-if chain
    // picked the first matching scenario driver. Each drive* method carries
    // its original guard and body; the first that fires wins, exactly like
    // the else-if chain did.
    if (drivePersistenceRoundtrip()) {
        return InGameDrive::kBreakCase;
    }
    if (driveLodGround() || driveWaterVisual() || driveMaterialVisual() || driveSkyboxVisual() ||
        driveWeatherVisual() || driveCloudShadow() || driveParticleDeterminism() ||
        driveFoliageVisual() || drivePrecipitation() || driveTimeOfDaySweep() ||
        driveLodBoundaryOscillation() || driveLodSeamArrival() || drivePlayerView() ||
        driveFarLodHorizon() || driveSkinnedMeshVisual(deltaTime) ||
        driveCreatureSlice(deltaTime) || driveNetworkedSession()) {
        return InGameDrive::kHandled;
    }
    return InGameDrive::kFallThrough;
}

bool ScenarioRunnerImpl::drivePersistenceRoundtrip() {
    // persistence runtime roundtrip phases run once on
    // the first ready frame and exit cleanly; the streaming
    // update is skipped so the hashed/saved chunk set is exactly
    // the deterministic post-readiness world.
    if (scenario_config.persistence_roundtrip_smoke() && scenario_ready &&
        !persistence_phase_attempted) {
        persistence_phase_attempted = true;
        PersistenceRoundtripPhaseResult phase_result;
        if (scenario_config.persistence_phase == "load") {
            phase_result = RunPersistenceRoundtripLoadPhase(scenario_config, gameSession.get());
        } else {
            phase_result = RunPersistenceRoundtripSavePhase(scenario_config, gameSession.get());
        }
        if (!phase_result.passed) {
            scenario_failed = true;
            scenario_failure_reason = "persistence_phase_" + phase_result.failure_reason;
            runtime_state_recorder.capture(scenario_failure_reason,
                                           &jobSystem,
                                           gameSession.get(),
                                           &renderPipeline,
                                           scenario_frame_count,
                                           last_readiness_report);
        }
        glfwSetWindowShouldClose(window, true);
        // Originally `break;` out of the IN_GAME case: the dispatcher
        // returns InGameDrive::kBreakCase for it.
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveLodGround() {
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
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveWaterVisual() {
    if (scenario_config.water_visual_smoke() && scenario_ready && g_camera) {
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
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveMaterialVisual() {
    if (scenario_config.material_visual_smoke() && scenario_ready && g_camera) {
        if (!material_visual_target_initialized || !material_visual_target.found) {
            material_visual_target = FindMaterialVisualCameraTarget(gameSession.get());
            material_visual_target_initialized = material_visual_target.found;
        }
        ApplyWaterVisualCamera(g_camera.get(), material_visual_target);
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveSkyboxVisual() {
    if (scenario_config.skybox_visual_smoke() && scenario_ready && g_camera) {
        ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.04f);
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveWeatherVisual() {
    if (scenario_config.weather_visual_smoke() && scenario_ready && g_camera) {
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
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
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
            wstate.snow_intensity = (sample.category == Luminumbra::Systems::WeatherCategory::Snow)
                                        ? sample.precip_intensity
                                        : 0.0f;
            wstate.fog_density =
                (sample.category == Luminumbra::Systems::WeatherCategory::Fog) ? 0.4f : 0.1f;
            wstate.storm_intensity = std::max(sample.storm_intensity, 0.4f);
            wstate.wetness = precip;
            const float wlen =
                std::sqrt(sample.wind.x * sample.wind.x + sample.wind.y * sample.wind.y);
            if (wlen > 1e-4f) {
                wstate.wind_direction = glm::vec3(sample.wind.x / wlen, 0.0f, sample.wind.y / wlen);
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
        const bool strike_window = weather_phase && (elapsed_play_seconds / duration) >= 0.80 &&
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
                Luminumbra::Rendering::BuildLightningBolt(strike_ground.x,
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
                const float ndc_x =
                    kBoltColumnNdcX + std::clamp(lateral * kLateralToNdc, -0.22f, 0.22f);
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
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveCloudShadow() {
    if (scenario_config.cloud_shadow_smoke() && scenario_ready && g_camera) {
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
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
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
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveParticleDeterminism() {
    if (scenario_config.particle_emitter_determinism_smoke() && scenario_ready && g_camera) {
        //  fixed skybox-style camera; spawn the fixture
        // emitter ONCE in front of the camera so particles render.
        ApplySkyboxVisualCamera(gameSession.get(), g_camera.get(), 0.30f);
        if (!particle_emitter_spawned) {
            if (auto* particles = renderPipeline.particles()) {
                const glm::vec3 spawn_origin = g_camera->Position + g_camera->Front * 8.0f;
                particles->add_emitter(root_dir / "data/common/particles/fixture_sparkle.json",
                                       spawn_origin);
                particle_emitter_spawned = true;
            }
        }
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveFoliageVisual() {
    if (scenario_config.foliage_visual_smoke() && scenario_ready && g_camera) {
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
                foliage->load_scatter_set(root_dir / "data/common/foliage/scatter_set.json");
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
            const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
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
            Luminumbra::Client::ScenarioHarness::FoliageScatterContext ctx{world_system};
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
                const float origin_x = static_cast<float>(c.x * Luminumbra::CHUNK_SIZE_X);
                const float origin_z = static_cast<float>(c.z * Luminumbra::CHUNK_SIZE_Z);
                const float center_x = origin_x + Luminumbra::CHUNK_SIZE_X * 0.5f;
                const float center_z = origin_z + Luminumbra::CHUNK_SIZE_Z * 0.5f;
                const float surf_h = world_system->GetTerrainHeightAt(center_x, center_z);
                // The chunk that straddles the surface column.
                const float chunk_y0 = static_cast<float>(c.y * Luminumbra::CHUNK_SIZE_Y);
                if (surf_h < chunk_y0 || surf_h >= chunk_y0 + Luminumbra::CHUNK_SIZE_Y) {
                    continue;
                }
                const Luminumbra::u8 biome_id = world_system->BiomeIdAt(center_x, center_z);
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
            foliage->rebuild_instances(chunk_scatter,
                                       &Luminumbra::Client::ScenarioHarness::FoliageSurfaceQuery,
                                       &ctx,
                                       g_camera->Position);
        }
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::drivePrecipitation() {
    if (scenario_config.precipitation_smoke() && scenario_ready && g_camera) {
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
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
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
            particles->add_splash_emitter(root_dir / "data/common/particles/precip_splash.json");
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
            precip_rain_emitter_id != Luminumbra::Rendering::ParticlePass::kInvalidEmitter) {
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
            sampled_wind_len =
                std::sqrt(sample.wind.x * sample.wind.x + sample.wind.y * sample.wind.y);
            if (sampled_wind_len > 1e-4f) {
                sampled_wind_dir = glm::vec3(
                    sample.wind.x / sampled_wind_len, 0.0f, sample.wind.y / sampled_wind_len);
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
            cstate.scroll_offset = glm::vec2(static_cast<float>(elapsed_play_seconds) * 22.0f,
                                             static_cast<float>(elapsed_play_seconds) * 6.0f);
            renderPipeline.set_cloud_state(cstate);

            // Periodic lightning: fire a deterministic forked bolt in a
            // short window roughly every ~1.6 s of wall-clock so the clip
            // contains a few strikes. The bolt + full-scene flash use the
            // same screen-anchored projection as the WeatherVisual gate.
            const double strike_cycle = std::fmod(elapsed_play_seconds, 1.6);
            const bool strike_now = strike_cycle < 0.16; // ~10% duty -> a few-frame flash
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
                const glm::vec3 fwd =
                    glm::normalize(glm::vec3(g_camera->Front.x, 0.0f, g_camera->Front.z));
                //  TOUCHDOWN. Strike a real ground
                // point ahead of the camera: terrain height at (x,z) is the
                // bolt's true bottom, so the channel spans cloud->terrain and
                // ends ON the ground (no floating mid-air bolt).
                const glm::vec3 strike_xz = g_camera->Position + fwd * 160.0f;
                const float ground_y =
                    gameSession->GetWorldSystem()->GetTerrainHeightAt(strike_xz.x, strike_xz.z);
                const glm::vec3 strike_ground(strike_xz.x, ground_y, strike_xz.z);
                // Vary the strike seed per cycle so successive bolts differ.
                const uint64_t cycle_index = static_cast<uint64_t>(elapsed_play_seconds / 1.6);
                const Luminumbra::Rendering::LightningBoltGeometry bolt =
                    Luminumbra::Rendering::BuildLightningBolt(strike_ground.x,
                                                              strike_ground.y,
                                                              strike_ground.z,
                                                              /*magnitude=*/0.9f,
                                                              /*strike_seed=*/0x5A5A1357ull +
                                                                  cycle_index * 0x9E3779B1ull);
                // PROJECT the real bolt through the actual render camera so
                // the bolt spans the frame from the cloud base down to the
                // projected terrain terminus -- it visibly TOUCHES DOWN.
                int mvw = 0, mvh = 0;
                glfwGetFramebufferSize(window, &mvw, &mvh);
                const glm::mat4 proj = glm::perspective(glm::radians(g_camera->Zoom),
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
                const float kGroundNdcY = bot_ok ? std::clamp(bot_ndc.y, -0.96f, -0.55f) : -0.92f;
                const float kColumnNdcX = bot_ok ? std::clamp(bot_ndc.x, -0.6f, 0.6f) : 0.0f;
                const float kLateralToNdc = 1.0f / 260.0f; // modest jag
                const auto map_point = [&](const glm::vec3& wp) -> glm::vec2 {
                    const float hf = std::clamp((wp.y - bottom.y) / span_y, 0.0f, 1.0f);
                    const float ndc_y = kGroundNdcY + (top_ndc.y - kGroundNdcY) * hf;
                    const float base_x = bottom.x + (top.x - bottom.x) * hf;
                    const float base_z = bottom.z + (top.z - bottom.z) * hf;
                    const float lateral = (wp.x - base_x) + (wp.z - base_z);
                    const float ndc_x =
                        kColumnNdcX + std::clamp(lateral * kLateralToNdc, -0.14f, 0.14f);
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
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveTimeOfDaySweep() {
    if (scenario_config.timeofday_sweep_smoke() && scenario_ready && g_camera) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          scenario_play_started_at)
                .count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        // Discovery reruns while meshes stream in (the first
        // frames only carry a fraction of the surface meshes);
        // it freezes once found or once the SUMMER night phase nears
        // so the emissive camera target stays stable. : the
        // summer half ends at progress 0.5, so freeze discovery before
        // its night/emissive window (~0.45-0.5).
        if (!timeofday_emissive_target.found && progress < 0.43 &&
            (!timeofday_emissive_target_initialized || (scenario_frame_count % 120) == 0)) {
            timeofday_emissive_target_initialized = true;
            timeofday_emissive_target = FindEmissiveMaterialTarget(gameSession.get(), root_dir);
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
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveLodBoundaryOscillation() {
    if (scenario_config.lod_boundary_oscillation_smoke() && scenario_ready && g_camera) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          scenario_play_started_at)
                .count();
        ApplyLodBoundaryOscillationCamera(gameSession.get(), g_camera.get(), elapsed_play_seconds);
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveLodSeamArrival() {
    if (scenario_config.lod_seam_arrival_smoke() && scenario_ready && g_camera) {
        const double elapsed_play_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          scenario_play_started_at)
                .count();
        ApplyLodSeamArrivalCamera(
            scenario_config, gameSession.get(), g_camera.get(), elapsed_play_seconds);
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::drivePlayerView() {
    if (scenario_config.player_view_smoke() && scenario_ready && g_camera) {
        // eye-level 360-degree sweep. Stations are built
        // once after readiness (the peak station scans the loaded
        // span field); each station holds its window so streaming
        // and uploads settle before the capture at 70% progress.
        if (player_view_stations.empty()) {
            player_view_stations = BuildPlayerViewStations(gameSession.get(), scenario_world_type);
            player_view_captures_written.assign(player_view_stations.size(), false);
            player_view_sky_enforced = !PlayerViewSeaWaterInNearField(gameSession.get());
        }
        const double elapsed_play_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          scenario_play_started_at)
                .count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        // Warmup lead before station 0: the post-readiness LOD0
        // promotion of the near ring is still draining during the
        // first seconds; stations divide the remaining time.
        const double warmup_seconds = std::min(8.0, duration * 0.2);
        const double effective_seconds = std::max(0.0, elapsed_play_seconds - warmup_seconds);
        const double progress =
            std::clamp(effective_seconds / std::max(1.0, duration - warmup_seconds), 0.0, 0.999);
        const std::size_t wall_clock_index = std::min(
            player_view_stations.size() - 1u,
            static_cast<std::size_t>(progress * static_cast<double>(player_view_stations.size())));
        // Hitch tolerance (mirrors the capture branch): hold the
        // camera on the first un-captured station so a passed-over
        // station is framed when its catch-up capture fires.
        std::size_t first_unwritten = 0;
        while (first_unwritten < player_view_captures_written.size() &&
               player_view_captures_written[first_unwritten]) {
            ++first_unwritten;
        }
        const std::size_t station_index = first_unwritten >= player_view_stations.size()
                                              ? wall_clock_index
                                              : std::min(wall_clock_index, first_unwritten);
        ApplyPlayerViewCamera(
            gameSession.get(), g_camera.get(), player_view_stations[station_index]);
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveFarLodHorizon() {
    if (scenario_config.farlod_horizon_smoke() && scenario_ready && g_camera) {
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
            farlod_horizon_captures_written.assign(farlod_horizon_stations.size(), false);
            farlod_horizon_far_off_sliver_px.assign(farlod_horizon_stations.size(), -1);
            farlod_horizon_sky_enforced = !PlayerViewSeaWaterInNearField(gameSession.get());
        }
        const double elapsed_play_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          scenario_play_started_at)
                .count();
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 0.999);
        if (auto* farlod = renderPipeline.farlod()) {
            farlod->set_enabled(progress >= kFarLodHorizonPhaseSplit);
        }
        const std::size_t station_count = farlod_horizon_stations.size();
        std::size_t station_index = 0;
        if (progress >= kFarLodHorizonPhaseSplit) {
            const double sweep =
                (progress - kFarLodHorizonPhaseSplit) / (1.0 - kFarLodHorizonPhaseSplit);
            const std::size_t time_based_index =
                std::min(station_count - 1u,
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
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveSkinnedMeshVisual(float deltaTime) {
    if (scenario_config.skinned_mesh_visual_smoke() && scenario_ready && g_camera) {
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
        if (scenario_config.wildlife && !wildlife_setup && skinned_mesh_visual_target.spawned &&
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
                constexpr float kWaterDepth = 0.5f; // neighbour must be this far below sea
                const float step = 16.0f;
                const float reach = 1600.0f;
                const float probe = 16.0f;
                float best_d2 = 1e18f;
                const glm::vec2 dirs[4] = {{probe, 0}, {-probe, 0}, {0, probe}, {0, -probe}};
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
                            if (terr(x + d.x, z + d.y) < Luminumbra::SEA_LEVEL - kWaterDepth) {
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
                            toLand =
                                -glm::normalize(wdir); // water->land = away from water neighbour
                            found = true;
                        }
                    }
                }
            }
            const float shore_dist = std::sqrt((shore.x - spawn.x) * (shore.x - spawn.x) +
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
            wildlife_water = glm::vec3(shore.x, Luminumbra::SEA_LEVEL, shore.z) - toLand * 2.0f;
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
            reg.emplace<Luminumbra::Components::TransformComponent>(wildlife_arrow_entity)
                .position = glm::vec3(0.0f, -1000.0f, 0.0f);
            auto& am =
                reg.emplace<Luminumbra::Components::StaticMeshComponent>(wildlife_arrow_entity);
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
                deltaTime > 0.0f ? static_cast<double>(std::min(deltaTime, 1.0f / 20.0f))
                                 : (1.0 / 60.0);
            const auto positions = replicated_demo.Update(replicated_dt, world_sys);
            auto& reg = gameSession->GetRegistry();
            bool applied_remote_pose = false;
            for (std::size_t i = 0;
                 i < skinned_mesh_visual_target.all_entities.size() && i < positions.size();
                 ++i) {
                const auto ent = skinned_mesh_visual_target.all_entities[i];
                if (reg.valid(ent) && reg.all_of<Luminumbra::Components::TransformComponent>(ent)) {
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
            const bool skip = scenario_config.wildlife && (wildlife_stream_tick++ % 30 != 0);
            if (!skip) {
                gameSession->GetWorldSystem()->EnsureSurfaceReadyNear(
                    g_camera->Position,
                    gameSession->GetPhysicsSystem(),
                    scenario_config.horizon_radius,
                    scenario_config.collision_radius);
            }
        }
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveCreatureSlice(float deltaTime) {
    if (scenario_config.creature_slice_smoke() && scenario_ready && g_camera) {
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
        const double duration = static_cast<double>(std::max(1, scenario_config.timed_run_seconds));
        const double progress = std::clamp(elapsed_play_seconds / duration, 0.0, 1.0);
        if (progress >= 0.55 && creature_slice_scene.spawned &&
            !creature_slice_scene.stimulus_spawned) {
            SpawnCreatureSliceStimulus(gameSession.get(), creature_slice_scene);
        }
        UpdateCreatureSliceScene(
            gameSession.get(), creature_slice_scene, static_cast<double>(deltaTime));
        ApplyCreatureSliceCamera(gameSession.get(), g_camera.get(), creature_slice_scene);
        return true;
    }
    return false;
}

bool ScenarioRunnerImpl::driveNetworkedSession() {
    if (scenario_config.networked_session_smoke() && scenario_ready && g_camera) {
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
                scenario_failure_reason =
                    "networked_session_begin_failed_" + networked_session_driver.failure_reason();
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
                            : ("networked_session_" + networked_session_driver.failure_reason());
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
            const Luminumbra::Vec3 anchor = networked_session_driver.ClientStreamingAnchor();
            g_camera->Position = glm::vec3(anchor.x, anchor.y + 1.8f, anchor.z);
            g_camera->Yaw = 0.0f;
            g_camera->Pitch = 0.0f;
            g_camera->updateCameraVectors();
        }
        return true;
    }
    return false;
}

} // namespace Luminumbra::Client::ScenarioHarness
