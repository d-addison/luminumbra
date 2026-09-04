#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

// --- world_visual_sweep ---------------------------------
namespace {

// One time-of-day pin in the sweep (normalized t, 0 = noon, 0.5 = midnight).
struct SweepTimeOfDay {
    const char* label;
    float time_of_day;
};

// One camera framing at the anchor. Eye-level vistas use yaw/pitch; the
// water-aimed cell aims at the resolved water focus instead.
struct SweepAngle {
    const char* label;
    float yaw_degrees;
    float pitch_degrees;
    bool aim_at_water;   // override yaw/pitch and aim at the water focus
    const char* feature; // intended feature the cell must SHOW
};

struct SweepCell {
    std::string file; // relative path under artifact_dir
    std::string tod;
    std::string angle;
    std::string weather;
    std::string season;
    std::string feature; // intended feature for this cell
    bool storm = false;
    bool pitched_up = false;   // up-pitched -> clouds should be visible
    bool pitched_down = false; // down-pitched daytime -> foliage should render
    bool water_aimed = false;
    bool daytime = false; // dawn/noon/dusk (not night)
    // Measured signals (filled at capture time).
    bool produced = false;
    bool non_black = false;
    double mean_luminance = 0.0;
    std::uint64_t foliage_draws = 0;
    std::uint64_t foliage_instances = 0;
    std::uint64_t particle_draws = 0;
    std::uint64_t particles_drawn = 0;
    bool lightning_active = false;
    double cloud_coverage = 0.0;
    std::uint64_t water_like_pixels = 0;
    double water_like_ratio = 0.0;
};

// Whole-frame mean luminance + a water-like pixel count (teal/blue, the same
// loose predicate AnalyzeScreenshotPixels uses) for the manifest.
void MeasureSweepFrame(const std::vector<unsigned char>& pixels,
                       int width,
                       int height,
                       double& out_mean_luminance,
                       bool& out_non_black,
                       std::uint64_t& out_water_pixels,
                       double& out_water_ratio) {
    out_mean_luminance = 0.0;
    out_non_black = false;
    out_water_pixels = 0;
    out_water_ratio = 0.0;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * height * 3u) {
        return;
    }
    const std::size_t total = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    double sum_lum = 0.0;
    std::uint64_t non_black_px = 0;
    std::uint64_t water_px = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const unsigned char r = pixels[i * 3u + 0u];
        const unsigned char g = pixels[i * 3u + 1u];
        const unsigned char b = pixels[i * 3u + 2u];
        const double lum = PixelLuminance(r, g, b);
        sum_lum += lum;
        if (r > 6 || g > 6 || b > 6) {
            ++non_black_px;
        }
        // Loose water-like: blue/teal dominant, not sky-bright-white.
        if (b > 40 && b >= r && g >= r && (g + b) > (2 * static_cast<int>(r) + 20)) {
            ++water_px;
        }
    }
    out_mean_luminance = sum_lum / static_cast<double>(total);
    // Non-black if at least 2% of pixels carry signal (guards a stuck black frame).
    out_non_black = (static_cast<double>(non_black_px) / static_cast<double>(total)) > 0.02;
    out_water_pixels = water_px;
    out_water_ratio = static_cast<double>(water_px) / static_cast<double>(total);
}

// Builds the per-chunk foliage scatter over the visible live ring (mirrors the
// foliage_visual_smoke build path) so the down-pitched daytime cells SHOW
// ground foliage. Returns the chunk-scatter set the FoliagePass rebuilds from.
std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter>
BuildSweepFoliageScatter(Luminumbra::Systems::SHIELD_WorldSystem* world_system) {
    std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter> chunk_scatter;
    if (world_system == nullptr) {
        return chunk_scatter;
    }
    const auto& renderable = world_system->get_renderable_chunks();
    chunk_scatter.reserve(renderable.size());
    for (const Luminumbra::Chunk* chunk : renderable) {
        if (chunk == nullptr) {
            continue;
        }
        const Luminumbra::IVec3 c = chunk->get_coords();
        const float origin_x = static_cast<float>(c.x * Luminumbra::CHUNK_SIZE_X);
        const float origin_z = static_cast<float>(c.z * Luminumbra::CHUNK_SIZE_Z);
        const float center_x = origin_x + Luminumbra::CHUNK_SIZE_X * 0.5f;
        const float center_z = origin_z + Luminumbra::CHUNK_SIZE_Z * 0.5f;
        const float surf_h = world_system->GetTerrainHeightAt(center_x, center_z);
        const float chunk_y0 = static_cast<float>(c.y * Luminumbra::CHUNK_SIZE_Y);
        if (surf_h < chunk_y0 || surf_h >= chunk_y0 + Luminumbra::CHUNK_SIZE_Y) {
            continue;
        }
        const Luminumbra::u8 biome_id = world_system->BiomeIdAt(center_x, center_z);
        const float density = world_system->biomes_enabled()
                                  ? world_system->biome_table().vegetation_for(biome_id).density
                                  : 0.3f;
        Luminumbra::Rendering::FoliagePass::ChunkScatter cs;
        cs.chunk_xz = glm::ivec2(c.x, c.z);
        cs.origin = glm::vec3(origin_x, 0.0f, origin_z);
        cs.extent_m = static_cast<float>(Luminumbra::CHUNK_SIZE_X);
        cs.biome_id = biome_id;
        cs.density = density;
        chunk_scatter.push_back(cs);
    }
    return chunk_scatter;
}

} // namespace

bool RunWorldVisualSweep(const WorldVisualSweepDeps& deps) {
    using namespace Luminumbra::Rendering;

    if (deps.game_session == nullptr || deps.pipeline == nullptr || deps.camera == nullptr ||
        !deps.render_and_read) {
        LUMINUMBRA_CORE_ERROR("world_visual_sweep: missing dependencies.");
        return false;
    }

    auto* game_session = deps.game_session;
    auto* pipeline = deps.pipeline;
    auto* camera = deps.camera;
    auto* world_system = game_session->GetWorldSystem();
    if (world_system == nullptr) {
        LUMINUMBRA_CORE_ERROR("world_visual_sweep: no world system.");
        return false;
    }

    const std::filesystem::path sweep_dir = deps.artifact_dir / "sweep";
    std::error_code ec;
    std::filesystem::create_directories(sweep_dir, ec);

    // Resolve the most open water near spawn for the water-aimed cell (also used
    // to bias the land anchor toward the shore so water stays in the vistas).
    const WaterVisualCameraTarget water_target = FindWaterVisualCameraTarget(game_session);

    // --- anchor + framing --------------------------------------------------
    // Pick a feature-rich LAND anchor: a column comfortably ABOVE sea level (so
    // the ground foliage actually renders in the down-pitched cells) and, when
    // possible, near the shore (so the horizon vistas + the water-aimed cell
    // still frame open water). We scan a grid around spawn and score columns by
    // (height above sea) + (vegetation density) - (distance from spawn) with a
    // shoreline bonus when open water sits within a short walk. Deterministic:
    // pure functions of the generated world (GetTerrainHeightAt / BiomeIdAt).
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    Luminumbra::Vec3 land(spawn.x, spawn.y, spawn.z);
    float land_terrain_h = world_system->GetTerrainHeightAt(spawn.x, spawn.z);
    {
        constexpr int kRadius = 384;
        constexpr int kStep = 16;
        float best_score = -1.0e30f;
        bool found_land = false;
        for (int dz = -kRadius; dz <= kRadius; dz += kStep) {
            for (int dx = -kRadius; dx <= kRadius; dx += kStep) {
                const float x = spawn.x + static_cast<float>(dx);
                const float z = spawn.z + static_cast<float>(dz);
                const float h = world_system->GetTerrainHeightAt(x, z);
                const float above_sea = h - Luminumbra::SEA_LEVEL;
                if (above_sea < 2.0f) {
                    continue;
                } // must be dry land
                const Luminumbra::u8 biome_id = world_system->BiomeIdAt(x, z);
                const float veg = world_system->biomes_enabled()
                                      ? world_system->biome_table().vegetation_for(biome_id).density
                                      : 0.3f;
                // Shore proximity bonus: open water within ~64 m keeps the sea in
                // frame for the vistas / water-aimed cell.
                float shore_bonus = 0.0f;
                for (int s = 32; s <= 96 && shore_bonus == 0.0f; s += 32) {
                    if (world_system->GetTerrainHeightAt(x + static_cast<float>(s), z) <
                            Luminumbra::SEA_LEVEL - 0.5f ||
                        world_system->GetTerrainHeightAt(x - static_cast<float>(s), z) <
                            Luminumbra::SEA_LEVEL - 0.5f ||
                        world_system->GetTerrainHeightAt(x, z + static_cast<float>(s)) <
                            Luminumbra::SEA_LEVEL - 0.5f ||
                        world_system->GetTerrainHeightAt(x, z - static_cast<float>(s)) <
                            Luminumbra::SEA_LEVEL - 0.5f) {
                        shore_bonus = 30.0f;
                    }
                }
                const float dist = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                const float score =
                    veg * 120.0f + std::min(above_sea, 30.0f) * 1.5f + shore_bonus - dist * 0.05f;
                if (score > best_score) {
                    best_score = score;
                    land = Luminumbra::Vec3(x, h, z);
                    land_terrain_h = h;
                    found_land = true;
                }
            }
        }
        if (!found_land) {
            // No dry land near spawn (open-ocean spawn): fall back to a lifted
            // spawn vista so the sweep still produces a full matrix.
            land = Luminumbra::Vec3(spawn.x, std::max(spawn.y, land_terrain_h), spawn.z);
        }
    }
    // Eye level a person-height above the ground so the down-pitch frames the
    // nearby foliage-covered ground rather than a distant sea horizon.
    const float eye_y = land_terrain_h + 2.5f;
    const Luminumbra::Vec3 anchor(land.x, eye_y, land.z);

    // --- foliage scatter (drives the down-daytime cells) -------------------
    bool foliage_ready = false;
    FoliagePass* foliage = pipeline->foliage();
    if (foliage != nullptr) {
        foliage->set_enabled(true);
        foliage_ready =
            foliage->load_scatter_set(deps.root_dir / "data/common/foliage/scatter_set.json");
        //  (defect 1): tighten the fade so foliage is a
        // NEAR-FIELD ground cover only. The old (60,120) ring let distant cards sit
        // out near the horizon, where a card's tip can poke ABOVE the terrain
        // silhouette and render as a green firefly against the sky/storm dome. A
        // (28,58) ring keeps the scatter dense underfoot (defect 2) while removing
        // the far floaters entirely -- combined with the denser candidate budget,
        // the near ground reads as real grass cover.
        foliage->set_fade_distances(28.0f, 58.0f);
        foliage->set_wind(glm::vec2(3.0f, 1.5f));
    }
    FoliageScatterContext foliage_ctx{world_system};
    const auto chunk_scatter = BuildSweepFoliageScatter(world_system);

    // --- rain emitter (lazily spawned for the storm cells) -----------------
    ParticlePass* particles = pipeline->particles();
    std::uint32_t rain_emitter = ParticlePass::kInvalidEmitter;

    // --- matrix dimensions -------------------------------------------------
    const std::array<SweepTimeOfDay, 4> tods = {{
        {"dawn", 0.17f},
        {"noon", 0.0f},
        {"dusk", 0.25f},
        {"night", 0.5f},
    }};
    const std::array<SweepAngle, 6> angles = {{
        {"yaw000", 0.0f, 0.0f, false, "horizon_vista"},
        {"yaw120", 120.0f, 0.0f, false, "horizon_vista"},
        {"yaw240", 240.0f, 0.0f, false, "horizon_vista"},
        {"down35", 45.0f, -35.0f, false, "ground_foliage"},
        {"up25", 200.0f, 25.0f, false, "sky_clouds"},
        {"water", 0.0f, 0.0f, true, "water_shore"},
    }};
    const std::array<const char*, 2> weathers = {{"clear", "storm"}};

    struct SeasonPin {
        const char* label;
        std::uint64_t tick;
    };
    std::vector<SeasonPin> seasons = {{"summer", SeasonSweepTick(0)}};
    if (deps.include_winter) {
        seasons.push_back({"winter", SeasonSweepTick(1)});
    }

    std::vector<SweepCell> cells;

    // Apply the camera framing for an angle at the anchor.
    const auto apply_angle = [&](const SweepAngle& a) {
        if (a.aim_at_water && water_target.found) {
            // Keep the shared anchor; aim down at the resolved water focus so the
            // water/shore cell frames the open water + shoreline.
            camera->Position = anchor;
            camera->Zoom = 70.0f;
            AimCameraAt(camera, water_target.focus);
        } else {
            camera->Position = anchor;
            camera->Yaw = a.yaw_degrees;
            camera->Pitch = a.pitch_degrees;
            camera->Zoom = 75.0f;
            camera->updateCameraVectors();
        }
    };

    // Settle + capture one cell.
    std::vector<unsigned char> pixels;
    int fb_w = 0, fb_h = 0;
    const auto render_settled = [&](int settle_frames) -> bool {
        bool ok = true;
        for (int i = 0; i < settle_frames; ++i) {
            // Rebuild foliage instances around the live camera each settle frame
            // so the scatter is fresh for the framing.
            //  (defect 1): foliage is GROUND cover -- it
            // must never appear in an UP-pitched (sky) shot. When the camera looks
            // up, rebuild from an EMPTY scatter set so the live instance set is
            // cleared (no stale cards from the previous down/horizon cell linger to
            // render as green specks against the sky/storm dome). The down + horizon
            // cells still get the full dense scatter.
            if (foliage != nullptr && foliage_ready) {
                static const std::vector<Luminumbra::Rendering::FoliagePass::ChunkScatter>
                    kEmptyScatter;
                const bool looking_up = camera->Pitch > 5.0f;
                const auto& scatter_for_cell = looking_up ? kEmptyScatter : chunk_scatter;
                foliage->rebuild_instances(
                    scatter_for_cell, &FoliageSurfaceQuery, &foliage_ctx, camera->Position);
            }
            ok = deps.render_and_read(pixels, fb_w, fb_h);
            if (!ok) {
                break;
            }
        }
        return ok;
    };

    // One-time WARMUP: stream the fixed-anchor world to readiness + warm the sky
    // LUT before the matrix so per-cell settle can stay small. The anchor position
    // never changes across the matrix, so this geometry is reused by every cell.
    pipeline->set_time_of_day(0.0f);
    camera->Position = anchor;
    camera->Yaw = 0.0f;
    camera->Pitch = 0.0f;
    camera->updateCameraVectors();
    for (int i = 0; i < 28; ++i) {
        if (foliage != nullptr && foliage_ready && !chunk_scatter.empty()) {
            foliage->rebuild_instances(
                chunk_scatter, &FoliageSurfaceQuery, &foliage_ctx, camera->Position);
        }
        if (!deps.render_and_read(pixels, fb_w, fb_h)) {
            break;
        }
    }

    // Drive the renderer state for a weather condition, returning whether
    // lightning is active + cloud coverage so the manifest can record them.
    const auto apply_weather = [&](bool storm,
                                   double elapsed_phase,
                                   bool& out_lightning,
                                   double& out_cloud_cov) {
        out_lightning = false;
        out_cloud_cov = 0.0;
        WeatherRenderState wstate;
        CloudRenderState cstate;
        LightningRenderState lstate; // default inactive
        if (storm) {
            wstate.driven = true;
            wstate.rain_intensity = 1.0f;
            wstate.storm_intensity = 0.9f;
            wstate.wetness = 1.0f;
            wstate.fog_density = 0.16f;
            wstate.wind_direction = glm::vec3(1.0f, 0.0f, 0.3f);
            wstate.wind_strength = 0.7f;
            cstate.enabled = true;
            cstate.shadow_enabled = true;
            cstate.coverage_amount = 0.85f;
            cstate.plane_height = 900.0f;
            cstate.shadow_strength = 0.6f;
            cstate.scroll_offset = glm::vec2(static_cast<float>(elapsed_phase) * 22.0f, 0.0f);
            out_cloud_cov = cstate.coverage_amount;

            // Rain particles at the camera. Re-add the emitter for each storm cell
            // (it was cleared on the previous clear cell) and re-center it on the
            // live camera so the rain falls past the viewer.
            if (particles != nullptr) {
                if (rain_emitter == ParticlePass::kInvalidEmitter) {
                    rain_emitter = particles->add_emitter(
                        deps.root_dir / "data/common/particles/precip_rain.json",
                        glm::vec3(camera->Position));
                } else {
                    particles->set_emitter_origin(rain_emitter, glm::vec3(camera->Position));
                }
                particles->set_wind(glm::vec3(4.0f, 0.0f, 1.0f));
            }
            // Periodic lightning: fire a deterministic bolt on the storm cells.
            //  (defect 3): a stronger pulse so the strike
            // clearly READS over the dark night-storm dome (the old 0.18 pulse was
            // nearly invisible once the storm dimmed the already-dark night frame).
            lstate.active = true;
            lstate.pulse_intensity = 0.42f;
            lstate.bolt_width_ndc = 0.010f;
            lstate.bolt_glow_ndc = 0.034f;
            lstate.cloud_darkness = 0.5f;
            const glm::vec3 fwd = glm::normalize(glm::vec3(camera->Front.x, 0.0f, camera->Front.z));
            // TOUCHDOWN. The strike terminus is a REAL
            // ground point ahead of the camera -- the terrain height at (x,z), not the
            // camera's eye level. The bolt's bottom is that ground point so the channel
            // spans cloud -> terrain and ends ON the surface rather than dangling in
            // mid-air at the horizon. The strike is placed near enough that the
            // touchdown projects inside the frame for the horizon/down framings.
            const glm::vec3 strike_xz = glm::vec3(camera->Position) + fwd * 150.0f;
            const float ground_y = world_system->GetTerrainHeightAt(strike_xz.x, strike_xz.z);
            const glm::vec3 strike_ground(strike_xz.x, ground_y, strike_xz.z);
            const LightningBoltGeometry bolt = BuildLightningBolt(strike_ground.x,
                                                                  strike_ground.y,
                                                                  strike_ground.z,
                                                                  /*magnitude=*/0.9f,
                                                                  /*strike_seed=*/0x5A5A1357ull);
            // PROJECT the bolt through the ACTUAL render camera (perspective-correct,
            // distance-aware) so the cloud-base top and the terrain terminus land at
            // their true screen positions. The old code projected each world point as
            // a DIRECTION to infinity (ProjectDirectionToScreen), which ignored range
            // and collapsed the descending channel onto the horizon line -- the bolt
            // appeared to stop in mid-air well above the ground. We project real world
            // positions and lay the seeded jagged channel along the screen line from
            // the cloud base down to the projected touchdown.
            const int proj_w = fb_w > 0 ? fb_w : 1280;
            const int proj_h = fb_h > 0 ? fb_h : 720;
            const glm::mat4 viewproj =
                camera->GetProjectionMatrix(proj_w, proj_h) * camera->GetViewMatrix();
            const glm::vec3 bolt_top = bolt.main_channel.front();
            const glm::vec3 bolt_bottom = bolt.main_channel.back();
            const float span_y = std::max(1e-3f, bolt_top.y - bolt_bottom.y);
            const auto project = [&](const glm::vec3& wp, bool& ok) -> glm::vec2 {
                const glm::vec4 clip = viewproj * glm::vec4(wp, 1.0f);
                ok = clip.w > 1e-4f;
                if (!ok)
                    return glm::vec2(0.0f);
                return glm::vec2(clip.x / clip.w, clip.y / clip.w);
            };
            bool top_ok = false, bot_ok = false;
            glm::vec2 top_ndc = project(bolt_top, top_ok);
            glm::vec2 bot_ndc = project(bolt_bottom, bot_ok);
            // Anchor the bolt TOP just below the top edge so the dark storm-cloud deck
            // is visible above the origin. The BOTTOM goes onto the projected ground
            // terminus, clamped just inside the frame so the touchdown stays visible
            // even when the camera pitch projects the ground point low or (looking up)
            // off the bottom edge.
            const float top_ndc_y = top_ok ? std::min(top_ndc.y, 0.74f) : 0.74f;
            // Whether the real ground terminus is genuinely on-screen (in front of the
            // camera and within the frame). This gates the ground-impact flash so it is
            // anchored at the actual touchdown -- never a hovering disc at frame centre.
            const bool ground_on_screen = bot_ok && bot_ndc.x >= -1.0f && bot_ndc.x <= 1.0f &&
                                          bot_ndc.y >= -1.0f && bot_ndc.y <= 1.0f;
            const float ground_ndc_y = bot_ok ? std::clamp(bot_ndc.y, -0.96f, 0.55f) : -0.92f;
            const float column_ndc_x = bot_ok ? std::clamp(bot_ndc.x, -0.85f, 0.85f) : 0.0f;
            const float kLateralToNdc = 1.0f / 260.0f; // modest sideways jag
            const auto map_point = [&](const glm::vec3& wp) -> glm::vec2 {
                const float hf = std::clamp((wp.y - bolt_bottom.y) / span_y, 0.0f, 1.0f);
                const float ndc_y = ground_ndc_y + (top_ndc_y - ground_ndc_y) * hf;
                const float base_x = bolt_bottom.x + (bolt_top.x - bolt_bottom.x) * hf;
                const float base_z = bolt_bottom.z + (bolt_top.z - bolt_bottom.z) * hf;
                const float lateral = (wp.x - base_x) + (wp.z - base_z);
                const float ndc_x =
                    column_ndc_x + std::clamp(lateral * kLateralToNdc, -0.14f, 0.14f);
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
            // Flash centre + dark-cloud anchor at the strike column.
            lstate.strike_ndc = glm::vec2(column_ndc_x, ground_ndc_y);
            lstate.cloud_anchor_ndc = glm::vec2(column_ndc_x, top_ndc_y);
            // GROUND-IMPACT bloom ONLY when the real
            // touchdown is on-screen, anchored AT the projected terminus (the bolt's
            // bottom). The previous code left u_groundNdc at its default (0, -1) while
            // forcing u_groundFlash > 0, which painted a hard bright disc at the bottom
            // centre of every storm frame -- the "floating UFO/saucer" artifact. By
            // anchoring the bloom to the actual touchdown and disabling it whenever the
            // strike point is off-screen (camera pitched up so the ground is below the
            // frame), the impact reads as ground illumination at the strike and the
            // floating disc is gone.
            lstate.ground_ndc = glm::vec2(column_ndc_x, ground_ndc_y);
            // tamer impact bloom. 0.55 over the bright storm
            // water clipped to a blown-out floating white smear. Drop it to a dim
            // contact glow; the shader bloom is also tightened + dimmed above.
            lstate.ground_flash = ground_on_screen ? 0.30f : 0.0f;
            out_lightning = true;
        } else {
            // Clear: overlay off, rain off, but a FAIR-WEATHER cloud deck that
            // still casts soft drifting shadows on the terrain (user: "make sure
            // clouds cast shadows" — clouds cast shadows in clear weather too, not
            // only storms). Lighter coverage + softer strength than the storm deck.
            cstate.enabled = true;
            cstate.shadow_enabled = true;
            cstate.coverage_amount = 0.50f;
            cstate.plane_height = 1100.0f;
            cstate.shadow_strength = 0.75f; // pronounced cast shadows (bolder look)
            cstate.scroll_offset = glm::vec2(static_cast<float>(elapsed_phase) * 14.0f, 0.0f);
            out_cloud_cov = cstate.coverage_amount;
            // Fully remove the rain emitter so no streaks bleed into the clear
            // cells (relocating it is not enough — it keeps emitting). It is
            // re-added on the next storm cell. set_wind(0) so any in-flight drops
            // stop being advected.
            if (particles != nullptr) {
                particles->clear_emitters();
                particles->set_wind(glm::vec3(0.0f));
                rain_emitter = ParticlePass::kInvalidEmitter;
            }
        }
        pipeline->set_weather_state(wstate);
        pipeline->set_cloud_state(cstate);
        pipeline->set_lightning_state(lstate);
    };

    // --- run the matrix ----------------------------------------------------
    double phase_clock = 0.0;
    for (const SeasonPin& season : seasons) {
        pipeline->set_season_tick(season.tick);
        for (const SweepTimeOfDay& tod : tods) {
            const bool daytime = (tod.time_of_day != 0.5f);
            for (std::size_t wi = 0; wi < weathers.size(); ++wi) {
                const bool storm = (std::string(weathers[wi]) == "storm");
                for (const SweepAngle& a : angles) {
                    pipeline->set_time_of_day(tod.time_of_day);
                    apply_angle(a);
                    phase_clock += 0.25;
                    bool lightning = false;
                    double cloud_cov = 0.0;
                    apply_weather(storm, phase_clock, lightning, cloud_cov);

                    // Settle so the lazy sky-view LUT catches up at the new sun pin
                    // and (storm) the rain particles accumulate, then capture. The
                    // world geometry is already streamed by the warmup, so this stays
                    // small. Storm cells settle a touch longer for the particle fill.
                    const int settle = storm ? 5 : 3;
                    const bool ok = render_settled(settle);

                    SweepCell cell;
                    cell.tod = tod.label;
                    cell.angle = a.label;
                    cell.weather = weathers[wi];
                    cell.season = season.label;
                    cell.feature = a.feature;
                    cell.storm = storm;
                    cell.pitched_up = (std::string(a.label) == "up25");
                    cell.pitched_down = (std::string(a.label) == "down35");
                    cell.water_aimed = a.aim_at_water;
                    cell.daytime = daytime;
                    cell.lightning_active = lightning;
                    cell.cloud_coverage = cloud_cov;

                    std::ostringstream fname;
                    fname << "sweep/" << season.label << "__" << tod.label << "__" << a.label
                          << "__" << weathers[wi] << ".ppm";
                    cell.file = fname.str();

                    if (ok && fb_w > 0 && fb_h > 0) {
                        const RenderPipeline::RenderPassFrameStats& stats =
                            pipeline->get_last_render_pass_stats();
                        cell.foliage_draws = stats.foliage_draws;
                        cell.foliage_instances = stats.foliage_instances_drawn;
                        cell.particle_draws = stats.particle_draws;
                        cell.particles_drawn = stats.particles_drawn;
                        MeasureSweepFrame(pixels,
                                          fb_w,
                                          fb_h,
                                          cell.mean_luminance,
                                          cell.non_black,
                                          cell.water_like_pixels,
                                          cell.water_like_ratio);
                        cell.produced =
                            WritePixelBufferPpm(deps.artifact_dir / cell.file, fb_w, fb_h, pixels);
                    }
                    cells.push_back(std::move(cell));
                }
            }
        }
    }

    // --- presence assertions + manifest ------------------------------------
    // The gate guards PRODUCTION + PRESENCE; real visual quality is judged by the
    // orchestrator from the montages. Required, per the matrix design:
    //   * every expected cell PPM produced + non-black,
    //   * foliage_draws > 0 in the down-pitched DAYTIME cells,
    //   * particle_draws > 0 AND lightning active in the STORM cells,
    //   * cloud coverage > 0 in the up-pitched STORM cells,
    //   * water-like pixels present in the water-aimed DAYTIME cells.
    std::vector<std::string> failures;
    std::size_t produced = 0, non_black = 0;
    for (const SweepCell& c : cells) {
        if (c.produced) {
            ++produced;
        } else {
            failures.push_back("missing:" + c.file);
        }
        if (c.non_black) {
            ++non_black;
        } else if (c.produced) {
            failures.push_back("black:" + c.file);
        }
        if (c.pitched_down && c.daytime && !c.storm && c.foliage_draws == 0) {
            failures.push_back("no_foliage:" + c.file);
        }
        if (c.storm && c.particle_draws == 0) {
            failures.push_back("no_rain:" + c.file);
        }
        if (c.storm && !c.lightning_active) {
            failures.push_back("no_lightning:" + c.file);
        }
        if (c.pitched_up && c.storm && c.cloud_coverage <= 0.0) {
            failures.push_back("no_clouds:" + c.file);
        }
        if (c.water_aimed && c.daytime && c.water_like_pixels == 0) {
            failures.push_back("no_water:" + c.file);
        }
    }

    nlohmann::json manifest;
    manifest["schema"] = "luminumbra.world_visual_sweep.v1";
    manifest["generated_by"] = "world_visual_sweep";
    manifest["world_preset"] = "archipelago";
    manifest["anchor"] = Vec3ToJson(anchor);
    manifest["water_target_found"] = water_target.found;
    manifest["foliage_scatter_loaded"] = foliage_ready;
    manifest["matrix"] = {
        {"seasons", static_cast<int>(seasons.size())},
        {"times_of_day", static_cast<int>(tods.size())},
        {"angles", static_cast<int>(angles.size())},
        {"weather", static_cast<int>(weathers.size())},
    };
    manifest["expected_cell_count"] = static_cast<int>(cells.size());
    manifest["produced_cell_count"] = static_cast<int>(produced);
    manifest["non_black_cell_count"] = static_cast<int>(non_black);
    nlohmann::json cells_json = nlohmann::json::array();
    for (const SweepCell& c : cells) {
        cells_json.push_back({
            {"file", c.file},
            {"season", c.season},
            {"tod", c.tod},
            {"angle", c.angle},
            {"weather", c.weather},
            {"feature", c.feature},
            {"produced", c.produced},
            {"non_black", c.non_black},
            {"mean_luminance", c.mean_luminance},
            {"foliage_draws", c.foliage_draws},
            {"foliage_instances", c.foliage_instances},
            {"particle_draws", c.particle_draws},
            {"particles_drawn", c.particles_drawn},
            {"lightning_active", c.lightning_active},
            {"cloud_coverage", c.cloud_coverage},
            {"water_like_pixels", c.water_like_pixels},
            {"water_like_ratio", c.water_like_ratio},
            {"pitched_down", c.pitched_down},
            {"pitched_up", c.pitched_up},
            {"water_aimed", c.water_aimed},
            {"daytime", c.daytime},
            {"storm", c.storm},
        });
    }
    manifest["cells"] = cells_json;
    manifest["failures"] = failures;
    const bool passed = failures.empty() && produced == cells.size();
    manifest["passed"] = passed;

    std::ofstream out(deps.artifact_dir / "world-visual-sweep-manifest.json");
    out << std::setw(2) << manifest << '\n';

    LUMINUMBRA_CORE_INFO(
        "world_visual_sweep: {} cells, {} produced, {} non-black, {} failures (passed={})",
        cells.size(),
        produced,
        non_black,
        failures.size(),
        passed);
    return passed;
}

} // namespace Luminumbra::Client::ScenarioHarness
