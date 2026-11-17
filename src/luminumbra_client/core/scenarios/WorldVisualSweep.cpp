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
#include <chrono>
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
    std::uint64_t water_draws = 0;
    std::uint64_t water_indices_drawn = 0;
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
        static_cast<std::size_t>(width) > pixels.size() / static_cast<std::size_t>(height) / 3u) {
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
        !deps.render_and_read || deps.readiness_timeout_seconds <= 0) {
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
    const float eye_y = std::max(land_terrain_h, Luminumbra::SEA_LEVEL) + 2.5f;
    const Luminumbra::Vec3 anchor(land.x, eye_y, land.z);

    // Keep the water focus inside the anchor's live ring. The spawn-centred
    // water search can select a distant ocean hidden behind intervening land.
    // At most 169 height queries, once per sweep.
    WaterVisualCameraTarget water_target;
    float nearest_water_distance_sq = std::numeric_limits<float>::max();
    for (int dz = -96; dz <= 96; dz += 16) {
        for (int dx = -96; dx <= 96; dx += 16) {
            const float x = anchor.x + static_cast<float>(dx);
            const float z = anchor.z + static_cast<float>(dz);
            const float height = world_system->GetTerrainHeightAt(x, z);
            const float distance_sq = static_cast<float>(dx * dx + dz * dz);
            if (height < Luminumbra::SEA_LEVEL - 0.5f && distance_sq < nearest_water_distance_sq) {
                nearest_water_distance_sq = distance_sq;
                water_target.found = true;
                water_target.focus = Luminumbra::Vec3(x, Luminumbra::SEA_LEVEL + 0.1f, z);
            }
        }
    }
    if (!water_target.found) {
        LUMINUMBRA_CORE_ERROR("world_visual_sweep: no water target in the anchor's live ring.");
        return false;
    }

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
    std::vector<FoliagePass::ChunkScatter> chunk_scatter;

    // --- rain emitter -----------------------------------------------------
    ParticlePass* particles = pipeline->particles();

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
                    scatter_for_cell,
                    &FoliageSurfaceQuery,
                    &foliage_ctx,
                    camera->Position,
                    Luminumbra::Rendering::FoliagePass::Preparation::Complete);
            }
            ok = deps.render_and_read(pixels, fb_w, fb_h, false);
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
    AimCameraAt(camera, water_target.focus);
    const auto readiness_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(deps.readiness_timeout_seconds);
    int warmup_frames = 0;
    bool streaming_ready = false;
    std::size_t water_indices_drawn = 0;
    while (!streaming_ready && std::chrono::steady_clock::now() < readiness_deadline) {
        if (!deps.render_and_read(pixels, fb_w, fb_h, true)) {
            LUMINUMBRA_CORE_ERROR("world_visual_sweep: warmup render failed.");
            return false;
        }
        ++warmup_frames;
        // 625 columns cover the 176 m legacy far-terrain inner clip in every
        // direction. This diagnostic runs only in the deadline-bounded warmup.
        const auto coverage = world_system->get_camera_local_coverage_stats(anchor, 12);
        const auto near_coverage = world_system->get_camera_local_coverage_stats(anchor, 3);
        const bool terrain_ready =
            warmup_frames >= 28 && coverage.near_field_renderable &&
            coverage.ready_surface_chunks == coverage.expected_surface_chunks &&
            coverage.pending_lod_chunks == 0 && near_coverage.expected_surface_chunks > 0 &&
            near_coverage.lod_counts[0] == near_coverage.expected_surface_chunks;
        if (terrain_ready) {
            // The warmup faces the local water target. Require submitted water,
            // rather than counting unrelated water still resident near spawn.
            const auto& stats = pipeline->get_last_render_pass_stats();
            water_indices_drawn = stats.water_indices_drawn;
            streaming_ready = stats.water_draws > 0 && water_indices_drawn > 0;
        }
    }
    if (!streaming_ready) {
        LUMINUMBRA_CORE_ERROR("world_visual_sweep: anchor residency timed out (water indices={}).",
                              water_indices_drawn);
        return false;
    }
    // Residency may have changed completely since spawn. Build scatter from the
    // settled anchor's chunks, then reuse it throughout the fixed-anchor matrix.
    chunk_scatter = BuildSweepFoliageScatter(world_system);

    // Drive the renderer state for a weather condition, returning whether
    // lightning is active + cloud coverage so the manifest can record them.
    const auto apply_weather = [&](bool storm, double phase, bool& lightning, double& cloud_cov) {
        lightning = false;
        cloud_cov = 0.0;
        WeatherRenderState wstate;
        CloudRenderState cstate;
        LightningRenderState lstate; // default inactive
        // Each cell starts with an empty particle pool, so its frame statistics
        // describe this cell's rain rather than surviving drops from earlier views.
        if (particles != nullptr) {
            particles->clear_emitters();
        }
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
            cstate.scroll_offset = glm::vec2(static_cast<float>(phase) * 22.0f, 0.0f);
            cloud_cov = cstate.coverage_amount;

            // Rain particles at the camera, with the same fill time in every cell.
            if (particles != nullptr) {
                particles->add_emitter(deps.root_dir / "data/common/particles/precip_rain.json",
                                       glm::vec3(camera->Position));
                particles->set_wind(glm::vec3(4.0f, 0.0f, 1.0f));
            }
            // Keep one strike fixed in the world across every yaw and pitch.
            // Only its true projection may enter or leave the viewport.
            lstate.active = true;
            lstate.pulse_intensity = 0.42f;
            lstate.bolt_width_ndc = 0.010f;
            lstate.bolt_glow_ndc = 0.034f;
            const glm::vec3 strike_xz = glm::vec3(anchor) + glm::vec3(150.0f, 0.0f, 0.0f);
            const float ground_y = world_system->GetTerrainHeightAt(strike_xz.x, strike_xz.z);
            const auto bolt =
                BuildLightningBolt(strike_xz.x, ground_y, strike_xz.z, 0.9f, 0x5A5A1357ull);
            const int proj_w = std::max(1, fb_w);
            const int proj_h = std::max(1, fb_h);
            const glm::mat4 viewproj =
                camera->GetProjectionMatrix(proj_w, proj_h) * camera->GetViewMatrix();
            const auto projected = ProjectLightningBolt(bolt, viewproj, lstate.pulse_intensity);
            const auto margin = LiveBoltViewMargin(
                lstate.bolt_width_ndc, lstate.bolt_glow_ndc, static_cast<float>(proj_w) / proj_h);
            auto composed =
                ComposeLiveStrikes(std::span(&projected, 1), kMaxBoltSegmentPoints, margin);
            lstate.bolt_points_ndc = std::move(composed.points);
            lstate.flashes = composed.flashes;
            lstate.ground_ndc = projected.strike_ndc;
            const bool ground_on_screen = !IsLiveBoltPenUp(projected.strike_ndc) &&
                                          std::abs(projected.strike_ndc.x) <= 1.0f &&
                                          std::abs(projected.strike_ndc.y) <= 1.0f;
            lstate.ground_flash = ground_on_screen ? 0.30f : 0.0f;
            lightning = true;
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
            cstate.scroll_offset = glm::vec2(static_cast<float>(phase) * 14.0f, 0.0f);
            cloud_cov = cstate.coverage_amount;
            // Clear cells also reset the rain wind.
            if (particles != nullptr) {
                particles->set_wind(glm::vec3(0.0f));
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
                        cell.water_draws = stats.water_draws;
                        cell.water_indices_drawn = stats.water_indices_drawn;
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
        if (c.water_aimed && (c.water_draws == 0 || c.water_indices_drawn == 0)) {
            failures.push_back("no_water_geometry:" + c.file);
        }
    }

    nlohmann::json manifest;
    manifest["schema"] = "luminumbra.world_visual_sweep.v1";
    manifest["generated_by"] = "world_visual_sweep";
    manifest["world_preset"] = "archipelago";
    manifest["anchor"] = Vec3ToJson(anchor);
    manifest["water_target_found"] = water_target.found;
    manifest["warmup_water_indices_drawn"] = water_indices_drawn;
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
            {"water_draws", c.water_draws},
            {"water_indices_drawn", c.water_indices_drawn},
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
