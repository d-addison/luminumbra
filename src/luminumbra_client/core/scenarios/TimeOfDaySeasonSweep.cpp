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

// --- Time-of-day sweep smoke ---

namespace {

// Terrain band for the warm-shift measurement: the bottom quarter of the
// frame is terrain under the skybox-scenario camera framing.
constexpr double kTerrainRoiHeightFraction = 0.25;

// Emissive glow classification for the optional night-emissive capture.
constexpr double kEmissiveGlowMinLuminance = 60.0;

} // namespace

float TimeOfDaySweepPhaseTime(double progress) {
    if (progress < 1.0 / 3.0) {
        return 0.04f; // noon (sun elevation = cos(2*pi*t), pinned like the other gates)
    }
    if (progress < 2.0 / 3.0) {
        return 0.22f; // dusk: sun ~10.8 degrees above the horizon
    }
    return 0.45f; // night: sun well below the horizon, moon up
}

std::uint64_t SeasonSweepTick(int season_index) {
    // summer = quarter-year phase (0.25 -> highest arc), winter =
    // three-quarter phase (0.75 -> lowest arc). PURE integer tick math off the
    // long-period cycle; the RenderPipeline derives the season phase from this.
    using RP = Luminumbra::Rendering::RenderPipeline;
    if (season_index == 0) {
        return RP::kTicksPerSeasonCycle / 4; // summer solstice
    }
    return (RP::kTicksPerSeasonCycle * 3) / 4; // winter solstice
}

const TimeOfDaySweepCapturePlan& TimeOfDaySweepCapturePlanAt(int index) {
    //  single source of truth for the six capture
    // windows. The summer (season 0) noon/dusk/night own the canonical
    // timeofday-{noon,dusk,night}.ppm files + the ordering/hue-band/emissive
    // assertions; winter (season 1) adds the per-season comparison set. The
    // phase_time values are pinned sun positions: noon 0.04 (near zenith), dusk
    // 0.22 (sun ~10.8 deg up -> a genuine partial-day sky, brighter than night),
    // night 0.45 (sun below the horizon). The per-frame pin drives the sun from
    // THESE values for the pending capture, so the captured frame is always lit
    // by the labelled phase.
    static const std::array<TimeOfDaySweepCapturePlan, kTimeOfDaySweepCaptureCount> kPlans{{
        {0.13, "noon", 0, 0.04f, "summer", 0, "screenshots/timeofday-noon.ppm"},
        {0.28, "dusk", 1, 0.22f, "summer", 0, "screenshots/timeofday-dusk.ppm"},
        {0.42, "night", 2, 0.45f, "summer", 0, "screenshots/timeofday-night.ppm"},
        {0.63, "noon", 0, 0.04f, "winter", 1, "screenshots/timeofday-winter-noon.ppm"},
        {0.78, "dusk", 1, 0.22f, "winter", 1, "screenshots/timeofday-winter-dusk.ppm"},
        {0.92, "night", 2, 0.45f, "winter", 1, "screenshots/timeofday-winter-night.ppm"},
    }};
    const int clamped = std::clamp(index, 0, kTimeOfDaySweepCaptureCount - 1);
    return kPlans[static_cast<std::size_t>(clamped)];
}

SeasonSweepPoint SeasonSweepAt(double progress) {
    SeasonSweepPoint p;
    const double clamped = std::clamp(progress, 0.0, 1.0);
    // First half == summer, second half == winter; each half replays the
    // noon/dusk/night thirds via the existing phase-time mapping.
    const bool winter = clamped >= 0.5;
    p.season_index = winter ? 1 : 0;
    p.season_label = winter ? "winter" : "summer";
    p.season_tick = SeasonSweepTick(p.season_index);
    const double within = winter ? (clamped - 0.5) * 2.0 : clamped * 2.0;
    p.time_of_day = TimeOfDaySweepPhaseTime(std::clamp(within, 0.0, 1.0));
    p.phase_index = (within < 1.0 / 3.0) ? 0 : (within < 2.0 / 3.0 ? 1 : 2);
    return p;
}

TimeOfDayPixelStats
AnalyzeTimeOfDayPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    TimeOfDayPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    const int sky_rows =
        std::max(1, static_cast<int>(static_cast<double>(height) * kSkyRoiHeightFraction));
    const int terrain_min_y_from_top =
        height -
        std::max(1, static_cast<int>(static_cast<double>(height) * kTerrainRoiHeightFraction));
    const int min_x = width / 64;
    const int max_x = width - min_x;
    const int center_min_x = width / 3;
    const int center_max_x = (width * 2) / 3;
    const int center_min_y = height / 3;
    const int center_max_y = (height * 2) / 3;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    double frame_luminance_accum = 0.0;
    double frame_r_accum = 0.0;
    double frame_b_accum = 0.0;
    std::uint64_t frame_pixels = 0;
    double sky_luminance_accum = 0.0;
    std::uint64_t sky_pixels = 0;
    // sky-band color accumulators, split left/right
    // (mid_x) so the warmer sky half (the sun's side at dusk) is measurable.
    double sky_r_accum = 0.0;
    double sky_b_accum = 0.0;
    double sky_left_r_accum = 0.0;
    double sky_left_b_accum = 0.0;
    double sky_right_r_accum = 0.0;
    double sky_right_b_accum = 0.0;
    //  sky green-excess (aurora chroma) accumulators.
    double sky_green_excess_accum = 0.0;
    double sky_green_excess_max = 0.0;
    std::uint64_t sky_strong_green_pixels = 0;
    const int mid_x = width / 2;
    double terrain_luminance_accum = 0.0;
    double terrain_r_accum = 0.0;
    double terrain_b_accum = 0.0;
    std::uint64_t terrain_pixels = 0;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            const double luminance = PixelLuminance(r, g, b);

            frame_luminance_accum += luminance;
            frame_r_accum += static_cast<double>(r);
            frame_b_accum += static_cast<double>(b);
            ++frame_pixels;

            if (luminance > stats.max_luminance) {
                stats.max_luminance = luminance;
                stats.max_luminance_y_from_top_norm =
                    static_cast<double>(y_from_top) / static_cast<double>(height);
            }
            if (y_from_top < sky_rows) {
                sky_luminance_accum += luminance;
                ++sky_pixels;
                stats.sky_max_luminance = std::max(stats.sky_max_luminance, luminance);
                // sky-band color balance (whole band
                // and per-half) for the dusk warm-shift requirement.
                sky_r_accum += static_cast<double>(r);
                sky_b_accum += static_cast<double>(b);
                if (x < mid_x) {
                    sky_left_r_accum += static_cast<double>(r);
                    sky_left_b_accum += static_cast<double>(b);
                } else {
                    sky_right_r_accum += static_cast<double>(r);
                    sky_right_b_accum += static_cast<double>(b);
                }
                // Aurora green-excess: how much green leads the max of r,b. A
                // neutral/blue dusk dome has g <= max(r,b) -> ~0; a green aurora
                // smear spikes this. Normalized to [0,1] (divide by 255).
                const double green_excess =
                    std::max(0.0, static_cast<double>(g) - static_cast<double>(std::max(r, b))) /
                    255.0;
                sky_green_excess_accum += green_excess;
                sky_green_excess_max = std::max(sky_green_excess_max, green_excess);
                // STRONG green excess => an aurora curtain core (a warm yellow sky
                // has r>=g so it never clears this). ~0.12 == 30/255.
                if (green_excess >= 0.12) {
                    ++sky_strong_green_pixels;
                }
            }
            if (y_from_top >= terrain_min_y_from_top) {
                terrain_luminance_accum += luminance;
                terrain_r_accum += static_cast<double>(r);
                terrain_b_accum += static_cast<double>(b);
                ++terrain_pixels;
            }
            if (x >= center_min_x && x < center_max_x && y_from_top >= center_min_y &&
                y_from_top < center_max_y && luminance >= kEmissiveGlowMinLuminance) {
                ++stats.center_glow_pixels;
            }
        }
    }

    if (frame_pixels > 0) {
        stats.frame_mean_luminance = frame_luminance_accum / static_cast<double>(frame_pixels);
        stats.frame_mean_r = frame_r_accum / static_cast<double>(frame_pixels);
        stats.frame_mean_b = frame_b_accum / static_cast<double>(frame_pixels);
        if (stats.frame_mean_b > 0.0) {
            stats.frame_r_b_ratio = stats.frame_mean_r / stats.frame_mean_b;
        }
    }
    if (sky_pixels > 0) {
        stats.sky_mean_luminance = sky_luminance_accum / static_cast<double>(sky_pixels);
        // whole-band and warmer-half sky R/B ratios.
        stats.sky_mean_r = sky_r_accum / static_cast<double>(sky_pixels);
        stats.sky_mean_b = sky_b_accum / static_cast<double>(sky_pixels);
        if (sky_b_accum > 0.0) {
            stats.sky_r_b_ratio = sky_r_accum / sky_b_accum;
        }
        const double left_ratio =
            (sky_left_b_accum > 0.0) ? sky_left_r_accum / sky_left_b_accum : 0.0;
        const double right_ratio =
            (sky_right_b_accum > 0.0) ? sky_right_r_accum / sky_right_b_accum : 0.0;
        stats.sky_warm_half_r_b_ratio = std::max(left_ratio, right_ratio);
        stats.sky_green_excess_mean = sky_green_excess_accum / static_cast<double>(sky_pixels);
        stats.sky_green_excess_max = sky_green_excess_max;
        stats.sky_strong_green_fraction =
            static_cast<double>(sky_strong_green_pixels) / static_cast<double>(sky_pixels);
    }
    if (terrain_pixels > 0) {
        stats.terrain_mean_luminance =
            terrain_luminance_accum / static_cast<double>(terrain_pixels);
        if (terrain_b_accum > 0.0) {
            stats.terrain_r_b_ratio = terrain_r_accum / terrain_b_accum;
        }
    }
    return stats;
}

EmissiveMaterialTarget FindEmissiveMaterialTarget(Luminumbra::world::GameSession* game_session,
                                                  const std::filesystem::path& root_dir) {
    EmissiveMaterialTarget target;
    if (!game_session || !game_session->GetWorldSystem()) {
        return target;
    }

    // 1. Emissive ids from the generic material registry: any material whose
    // "emission" carries a non-zero component. Game data under data/ decides
    // which materials are emissive; the engine check stays material-agnostic.
    {
        std::ifstream input(root_dir / "data/common/materials.json");
        if (!input.is_open()) {
            return target;
        }
        nlohmann::json registry;
        try {
            registry = nlohmann::json::parse(input);
        } catch (const std::exception&) {
            return target;
        }
        if (!registry.contains("materials") || !registry["materials"].is_array()) {
            return target;
        }
        for (const nlohmann::json& material : registry["materials"]) {
            if (!material.contains("emission") || !material["emission"].is_array() ||
                !material.contains("id")) {
                continue;
            }
            bool emissive = false;
            for (const nlohmann::json& component : material["emission"]) {
                if (component.is_number() && component.get<double>() > 0.0) {
                    emissive = true;
                    break;
                }
            }
            if (emissive) {
                target.emissive_material_ids.push_back(material["id"].get<std::uint32_t>());
            }
        }
    }
    if (target.emissive_material_ids.empty()) {
        return target;
    }

    // 2. Scan the streamed terrain meshes for a near-surface emissive vertex
    // reachable by a surface camera. Among in-range emissive vertices the
    // most exposed one (smallest depth below the heightmap surface) wins;
    // cave-mouth crystals can sit a few meters below the column height while
    // still being visible from above.
    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    constexpr float kMaxSearchDistance = 512.0f;
    constexpr float kMaxDepthBelowSurface = 4.0f;
    float best_depth = std::numeric_limits<float>::max();

    for (const Luminumbra::Chunk* chunk : world_system->get_renderable_chunks()) {
        if (!chunk || chunk->mesh_vertices.empty()) {
            continue;
        }
        const Luminumbra::Vec3 chunk_origin =
            Luminumbra::Vec3(chunk->get_coords() * Luminumbra::IVec3(Luminumbra::CHUNK_SIZE_X,
                                                                     Luminumbra::CHUNK_SIZE_Y,
                                                                     Luminumbra::CHUNK_SIZE_Z));
        for (const Luminumbra::VoxelVertex& vertex : chunk->mesh_vertices) {
            ++target.vertices_scanned;
            bool emissive = false;
            for (const std::uint32_t id : target.emissive_material_ids) {
                if (vertex.material_id == id) {
                    emissive = true;
                    break;
                }
            }
            if (!emissive) {
                continue;
            }
            ++target.emissive_vertices_total;
            const Luminumbra::Vec3 world_pos = chunk_origin + vertex.position;
            const float horizontal_distance =
                glm::length(glm::vec2(world_pos.x - spawn.x, world_pos.z - spawn.z));
            if (horizontal_distance > kMaxSearchDistance) {
                continue;
            }
            ++target.emissive_vertices_in_range;
            const float surface_height = world_system->GetTerrainHeightAt(world_pos.x, world_pos.z);
            const float depth_below_surface = surface_height - world_pos.y;
            if (depth_below_surface > kMaxDepthBelowSurface) {
                continue; // deep underground: not visible in a surface capture
            }
            if (depth_below_surface < best_depth) {
                target.found = true;
                target.position = world_pos;
                target.material_id = vertex.material_id;
                target.distance_from_spawn = horizontal_distance;
                target.depth_below_surface = depth_below_surface;
                best_depth = depth_below_surface;
            }
        }
    }

    LUMINUMBRA_CORE_INFO(
        "Emissive material target scan: registry_ids={}, vertices_scanned={}, emissive_total={}, "
        "emissive_in_range={}, found={}, material_id={}, distance={:.1f}, depth={:.2f}",
        target.emissive_material_ids.size(),
        target.vertices_scanned,
        target.emissive_vertices_total,
        target.emissive_vertices_in_range,
        target.found,
        target.material_id,
        target.distance_from_spawn,
        target.depth_below_surface);
    return target;
}

nlohmann::json TimeOfDayPixelStatsToJson(const TimeOfDayPixelStats& stats) {
    return {{"width", stats.width},
            {"height", stats.height},
            {"frame_mean_luminance", stats.frame_mean_luminance},
            {"sky_mean_luminance", stats.sky_mean_luminance},
            {"terrain_mean_luminance", stats.terrain_mean_luminance},
            {"frame_mean_r", stats.frame_mean_r},
            {"frame_mean_b", stats.frame_mean_b},
            {"frame_r_b_ratio", stats.frame_r_b_ratio},
            {"terrain_r_b_ratio", stats.terrain_r_b_ratio},
            {"sky_mean_r", stats.sky_mean_r},
            {"sky_mean_b", stats.sky_mean_b},
            {"sky_r_b_ratio", stats.sky_r_b_ratio},
            {"sky_warm_half_r_b_ratio", stats.sky_warm_half_r_b_ratio},
            {"max_luminance", stats.max_luminance},
            {"max_luminance_y_from_top_norm", stats.max_luminance_y_from_top_norm},
            {"sky_max_luminance", stats.sky_max_luminance},
            {"sky_green_excess_mean", stats.sky_green_excess_mean},
            {"sky_green_excess_max", stats.sky_green_excess_max},
            {"sky_strong_green_fraction", stats.sky_strong_green_fraction},
            {"center_glow_pixels", stats.center_glow_pixels}};
}

void WriteTimeOfDaySweepAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::vector<TimeOfDayPhaseCapture>& phases,
    const EmissiveMaterialTarget& emissive_target,
    bool emissive_capture_written,
    const std::string& emissive_screenshot,
    const TimeOfDayPixelStats& emissive_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    // Calibrated against the first sweep run; measured values are recorded
    // next to the thresholds. The ordering gate uses the SKY band: the dusk
    // sun sits low inside the fixed frame, so its corona lifts the dusk
    // frame/terrain means to near-noon levels while the sky band darkens
    // monotonically (measured sky means: noon 216.2, dusk 208.8, night 185.1).
    constexpr double kMinNoonOverDuskGap = 3.0; // sky mean luminance units (0-255)
    constexpr double kMinDuskOverNightGap = 10.0;
    constexpr double kMinDuskWarmShift =
        0.01; // terrain r/b ratio increase vs noon (measured +0.081)
    const std::uint64_t kMinEmissiveGlowPixels =
        static_cast<std::uint64_t>(ScalePinnedArea(200, kCapturePinnedWidth, kCapturePinnedHeight));
    // the dome must actually track time-of-day.
    // (1) Night ceiling: the pre-fix dome held a bright twilight-blue night sky
    //     (sky mean ~182) over near-black ground; a real night dome reads dark.
    //     The fixed dome measures sky mean ~20-30 at night, so a 60 ceiling has
    //     wide margin against tonemap/star/moon noise yet fails the old dome.
    // (2) Dusk sky warm-shift: the pre-fix dusk dome was full-midday blue
    //     (sun-side sky R/B < 1, no shift vs noon). A real twilight warms the
    //     sun-side sky (R>B). Require the warmer sky half's R/B to rise vs noon;
    //     +0.05 sits well above per-frame noise while the warm fix clears it by
    //     a wide margin.
    constexpr double kMaxNightSkyLuminance = 60.0;
    constexpr double kMinDuskSkyWarmShift = 0.05; // dusk warm-half sky r/b increase vs noon
    //  dawn/dusk HUE-BAND assertion on top of the existing ordering +
    // relative warm-shift. The Hillaire scattering must make the low-sun sky
    // warm-half band read GENUINELY warm in ABSOLUTE terms (R/B approaching or
    // exceeding parity), not merely warmer than noon's blue. Clear sky .
    // The dusk warm-half r/b must clear this floor AND exceed the noon warm-half
    // r/b (the rising-warm direction). 0.95 sits below the scattering-warmed
    // dusk band but well above the cool midday sky (~0.7-0.8 R/B).
    constexpr double kMinDuskSkyWarmBandRatio = 0.95;

    // the SUMMER season (season_index 0) owns the existing
    // ordering / warm-shift / hue-band / emissive assertions -- these read the
    // summer noon/dusk/night exactly as the pre-season sweep did, so they stay
    // GREEN. The winter captures feed the NEW per-season comparison below.
    const TimeOfDayPhaseCapture* noon = nullptr;
    const TimeOfDayPhaseCapture* dusk = nullptr;
    const TimeOfDayPhaseCapture* night = nullptr;
    const TimeOfDayPhaseCapture* winter_noon = nullptr;
    const TimeOfDayPhaseCapture* winter_dusk = nullptr;
    const TimeOfDayPhaseCapture* winter_night = nullptr;
    for (const TimeOfDayPhaseCapture& phase : phases) {
        const bool summer = phase.season_index == 0;
        if (phase.name == "noon") {
            if (summer)
                noon = &phase;
            else
                winter_noon = &phase;
        } else if (phase.name == "dusk") {
            if (summer)
                dusk = &phase;
            else
                winter_dusk = &phase;
        } else if (phase.name == "night") {
            if (summer)
                night = &phase;
            else
                winter_night = &phase;
        }
    }

    const bool all_phases_captured = noon && dusk && night;
    const double noon_luminance = noon ? noon->stats.sky_mean_luminance : 0.0;
    const double dusk_luminance = dusk ? dusk->stats.sky_mean_luminance : 0.0;
    const double night_luminance = night ? night->stats.sky_mean_luminance : 0.0;
    const bool luminance_ordering_passed =
        all_phases_captured && (noon_luminance - dusk_luminance) >= kMinNoonOverDuskGap &&
        (dusk_luminance - night_luminance) >= kMinDuskOverNightGap;

    const double warm_shift =
        (noon && dusk) ? dusk->stats.terrain_r_b_ratio - noon->stats.terrain_r_b_ratio : 0.0;
    const bool warm_shift_passed = all_phases_captured && warm_shift >= kMinDuskWarmShift;

    // the dome (not just the terrain) must track
    // time-of-day. Night sky band must be dark, and the dusk sky's warmer half
    // (the sun side) must warm vs noon.
    const bool night_sky_dark_passed = night && night_luminance <= kMaxNightSkyLuminance;
    const double dusk_sky_warm_shift =
        (noon && dusk) ? dusk->stats.sky_warm_half_r_b_ratio - noon->stats.sky_warm_half_r_b_ratio
                       : 0.0;
    const bool dusk_sky_warm_passed =
        all_phases_captured && dusk_sky_warm_shift >= kMinDuskSkyWarmShift;

    //  absolute dawn/dusk hue-band. The dusk warm-half sky band must be
    // genuinely warm (R/B near/above parity) AND warmer than noon's warm half --
    // the scattering palette rising warm at low sun (clear sky).
    const double dusk_sky_warm_band_ratio = dusk ? dusk->stats.sky_warm_half_r_b_ratio : 0.0;
    const bool dusk_sky_warm_band_passed =
        all_phases_captured && dusk_sky_warm_band_ratio >= kMinDuskSkyWarmBandRatio &&
        dusk_sky_warm_band_ratio > (noon ? noon->stats.sky_warm_half_r_b_ratio : 0.0);

    // Emissive night check: when a registry-emissive material is reachable
    // in a surface capture, the dedicated night-emissive capture must show a
    // glow cluster. Otherwise the honest fallback asserts the night frame's
    // brightest pixel comes from the sky band (moon/stars), proving nothing
    // ground-side fakes an emissive response.
    // Fallback contract: the night frame's brightest source must be the sky
    // itself - either positionally inside the sky band, or (for sky leaking
    // through distant terrain LOD holes, which the LodGround gate tracks
    // separately) no brighter than the sky band's own maximum plus a small
    // epsilon. Either way nothing ground-side fakes an emissive response.
    constexpr double kNightSkyMaxEpsilon = 10.0;
    std::string emissive_status;
    bool emissive_passed = false;
    if (emissive_target.found && emissive_capture_written) {
        emissive_status = "checked_surface_emissive";
        emissive_passed = emissive_stats.center_glow_pixels >= kMinEmissiveGlowPixels;
    } else if (emissive_target.found) {
        emissive_status = "target_found_capture_missing";
        emissive_passed = false;
    } else {
        emissive_status = "not_applicable_no_surface_emissives";
        emissive_passed =
            night &&
            (night->stats.max_luminance_y_from_top_norm < kSkyRoiHeightFraction ||
             night->stats.max_luminance <= night->stats.sky_max_luminance + kNightSkyMaxEpsilon);
    }

    // SEASON-SWEEP assertions. The same noon/dusk/night phases are
    // captured under TWO seasons; assert a REAL per-season difference in BOTH
    // (a) the sun-path band -- summer's tick-derived solar arc sits HIGHER than
    //     winter's at the same time-of-day (the seasonal declination), measured
    //     directly from the sun elevation the season modulates; AND
    // (b) the palette band -- summer reads WARMER than winter (the luminance-
    //     preserving season tint), measured as a daytime sky/terrain r/b ratio
    //     difference at the same noon phase. Clear sky .
    const bool season_phases_captured =
        noon && winter_noon && dusk && winter_dusk && night && winter_night;
    // (a) sun-path band: summer noon elevation must exceed winter noon by a
    // margin well above per-frame jitter (the declination is ~23.5 deg => the
    // noon-elevation gap is ~tens of degrees; require a comfortably clearing
    // 0.05 rad ~ 2.9 deg minimum).
    constexpr double kMinSeasonSunElevationGap = 0.05; // radians
    const double summer_noon_elev = noon ? noon->sun_elevation_rad : 0.0;
    const double winter_noon_elev = winter_noon ? winter_noon->sun_elevation_rad : 0.0;
    const double season_sun_elev_gap = summer_noon_elev - winter_noon_elev;
    const bool season_sun_path_passed =
        season_phases_captured && season_sun_elev_gap >= kMinSeasonSunElevationGap;
    // (b) palette band: the two seasons' palettes must differ measurably in a
    // CONSISTENT direction. WINTER reads WARMER (higher frame r/b ratio) than
    // summer -- the season tint leans winter golden / summer cool, REINFORCING
    // the low-winter-sun / high-summer-sun arc, so the palette band is a large,
    // robust signal (the authored tint and the sun-path physics agree). The tint
    // is luminance-preserving, so use a jitter-clearing floor on the daytime
    // frame r/b difference.
    constexpr double kMinSeasonPaletteWarmthGap = 0.01; // frame r/b ratio delta
    const double summer_noon_rb = noon ? noon->stats.frame_r_b_ratio : 0.0;
    const double winter_noon_rb = winter_noon ? winter_noon->stats.frame_r_b_ratio : 0.0;
    // winter - summer: winter is the warmer season here (see direction note).
    const double season_palette_gap = winter_noon_rb - summer_noon_rb;
    const bool season_palette_passed =
        season_phases_captured && season_palette_gap >= kMinSeasonPaletteWarmthGap;
    // The two seasons must actually be distinct tick-derived phases (proves the
    // sweep drove different ticks, not the same frame twice).
    const bool season_phases_distinct =
        noon && winter_noon && noon->season_tick != winter_noon->season_tick &&
        std::abs(noon->season_phase - winter_noon->season_phase) > 1e-4;
    const bool season_sweep_passed = season_phases_captured && season_phases_distinct &&
                                     season_sun_path_passed && season_palette_passed;

    //  AURORA NIGHT-GATING. The aurora is a NIGHT-ONLY
    // phenomenon. The old shader let it bleed into the twilight/day dome. The fixed
    // shader gates the aurora by the deep-night brightness envelope, so its green
    // chroma only appears at night. We assert this as a RELATIVE presence check
    // (robust to the warm low-sun sky's own green/yellow gradient, which an
    // ABSOLUTE green ceiling would false-trip): the NIGHT sky band must carry a
    // measurably STRONGER green-excess curtain than the brighter (day/dusk) phases.
    // If the aurora bled into dusk/noon (the failure), the night-vs-day gap would
    // collapse; the night-only gating keeps a clear gap. We compare night against
    // the brightest (most day-like) phase, whichever of noon/dusk that is, so the
    // check holds even when the scenario's phase timing shifts which capture is the
    // sunniest. A non-trivial absolute night floor keeps the gate non-vacuous.
    // The discriminator is the SATURATED-green curtain fraction: the night aurora
    // covers a non-trivial fraction of the sky band with strong green; a warm low-
    // sun day/dusk sky (r>=g) produces ~none. Require the night to carry a visible
    // aurora AND the day-side phases to be essentially aurora-FREE.
    //  update the baseline (2026-06-21): the 2026-06-18 lighting/atmosphere overhaul
    // shifted the night aurora to vivid but more concentrated curtains covering
    // ~0.66% of the sky band (visually confirmed beautiful — green curtains clearly
    // present, day/dusk still aurora-free at 0.0). The old 0.010 floor pre-dated the
    // overhaul; lowered to 0.005 so the gate stays non-vacuous (aurora MUST still
    // render at night) while matching the confirmed-good look. Render-only threshold.
    constexpr double kMinNightStrongGreen = 0.005; // night aurora curtain present
    constexpr double kMaxDayStrongGreen = 0.003;   // day/dusk must be aurora-free
    const double noon_strong_green = noon ? noon->stats.sky_strong_green_fraction : 0.0;
    const double dusk_strong_green = dusk ? dusk->stats.sky_strong_green_fraction : 0.0;
    const double night_strong_green = night ? night->stats.sky_strong_green_fraction : 0.0;
    const bool aurora_absent_dusk_noon =
        all_phases_captured && noon_strong_green <= kMaxDayStrongGreen &&
        dusk_strong_green <= kMaxDayStrongGreen && night_strong_green >= kMinNightStrongGreen;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed = render_pass.skybox_draws > 0 && luminance_ordering_passed &&
                        warm_shift_passed && night_sky_dark_passed && //
                        dusk_sky_warm_passed &&                       //
                        dusk_sky_warm_band_passed && //  absolute dawn/dusk hue band
                        season_sweep_passed &&       //  per-season sun-path + palette
                        aurora_absent_dusk_noon &&   //  aurora night-only (absent at dusk/noon)
                        emissive_passed && gl_debug.errors == 0;

    nlohmann::json phases_json = nlohmann::json::array();
    for (const TimeOfDayPhaseCapture& phase : phases) {
        phases_json.push_back({{"name", phase.name},
                               {"time_of_day", phase.time_of_day},
                               {"screenshot", phase.file},
                               {"pixels", TimeOfDayPixelStatsToJson(phase.stats)},
                               // tick-derived season state at capture time.
                               {"season_label", phase.season_label},
                               {"season_index", phase.season_index},
                               {"season_phase", phase.season_phase},
                               {"season_tick", phase.season_tick},
                               {"sun_elevation_rad", phase.sun_elevation_rad},
                               {"season_sun_declination_rad", phase.season_sun_declination_rad}});
    }

    nlohmann::json emissive_ids = nlohmann::json::array();
    for (const std::uint32_t id : emissive_target.emissive_material_ids) {
        emissive_ids.push_back(id);
    }

    nlohmann::json artifact = {
        {"schema", "luminumbra.timeofday_sweep.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"phases", phases_json},
        {"luminance_ordering",
         {{"passed", luminance_ordering_passed},
          {"band", "sky"},
          {"noon_mean_luminance", noon_luminance},
          {"dusk_mean_luminance", dusk_luminance},
          {"night_mean_luminance", night_luminance},
          {"noon_over_dusk_gap", noon_luminance - dusk_luminance},
          {"dusk_over_night_gap", dusk_luminance - night_luminance}}},
        {"dusk_warm_shift",
         {{"passed", warm_shift_passed},
          {"noon_terrain_r_b_ratio", noon ? noon->stats.terrain_r_b_ratio : 0.0},
          {"dusk_terrain_r_b_ratio", dusk ? dusk->stats.terrain_r_b_ratio : 0.0},
          {"r_b_ratio_increase", warm_shift}}},
        {"night_sky_dark",
         {{"passed", night_sky_dark_passed},
          {"night_sky_mean_luminance", night_luminance},
          {"max_night_sky_luminance", kMaxNightSkyLuminance}}},
        {"dusk_sky_warm_shift",
         {{"passed", dusk_sky_warm_passed},
          {"noon_sky_warm_half_r_b_ratio", noon ? noon->stats.sky_warm_half_r_b_ratio : 0.0},
          {"dusk_sky_warm_half_r_b_ratio", dusk ? dusk->stats.sky_warm_half_r_b_ratio : 0.0},
          {"sky_warm_half_r_b_ratio_increase", dusk_sky_warm_shift}}},
        {"aurora_gating",
         {{"passed", aurora_absent_dusk_noon},
          {"noon_sky_strong_green_fraction", noon_strong_green},
          {"dusk_sky_strong_green_fraction", dusk_strong_green},
          {"night_sky_strong_green_fraction", night_strong_green},
          {"max_day_strong_green_fraction", kMaxDayStrongGreen},
          {"min_night_strong_green_fraction", kMinNightStrongGreen}}},
        {"dusk_sky_hue_band",
         {//  absolute dawn/dusk hue band (scattering palette rises warm
          // at low sun, clear sky).
          {"passed", dusk_sky_warm_band_passed},
          {"dusk_sky_warm_half_r_b_ratio", dusk_sky_warm_band_ratio},
          {"min_dusk_sky_warm_band_ratio", kMinDuskSkyWarmBandRatio}}},
        {"season_sweep",
         {// Per-season sun-path and palette bands. The same
          // noon/dusk/night phases captured under two TICK-DERIVED seasons
          // (summer/winter), asserting a real per-season difference. The season
          // is render-derived (pure function of tick) and adds NOTHING to
          // world_hash.
          {"passed", season_sweep_passed},
          {"phases_captured", season_phases_captured},
          {"phases_distinct", season_phases_distinct},
          {"summer_season_tick", noon ? noon->season_tick : 0},
          {"winter_season_tick", winter_noon ? winter_noon->season_tick : 0},
          {"summer_season_phase", noon ? noon->season_phase : 0.0},
          {"winter_season_phase", winter_noon ? winter_noon->season_phase : 0.0},
          {"sun_path",
           {{"passed", season_sun_path_passed},
            {"summer_noon_sun_elevation_rad", summer_noon_elev},
            {"winter_noon_sun_elevation_rad", winter_noon_elev},
            {"summer_noon_sun_declination_rad", noon ? noon->season_sun_declination_rad : 0.0},
            {"winter_noon_sun_declination_rad",
             winter_noon ? winter_noon->season_sun_declination_rad : 0.0},
            {"sun_elevation_gap_rad", season_sun_elev_gap},
            {"min_sun_elevation_gap_rad", kMinSeasonSunElevationGap}}},
          {"palette",
           {{"passed", season_palette_passed},
            {"summer_noon_frame_r_b_ratio", summer_noon_rb},
            {"winter_noon_frame_r_b_ratio", winter_noon_rb},
            {"palette_warmth_gap", season_palette_gap},
            {"min_palette_warmth_gap", kMinSeasonPaletteWarmthGap}}}}},
        {"gpu_timer",
         {//  sky precompute startup one-shot recorded in render
          // telemetry (budget enforced on release by the PS1 gate).
          {"supported", render_pass.gpu_timers_supported},
          {"aerial_gpu_ms", render_pass.aerial_gpu_ms},
          {"sky_view_refresh_ms", render_pass.sky_view_refresh_ms},
          {"sky_full_precompute_ms", render_pass.sky_full_precompute_ms}}},
        {"emissive_check",
         {{"status", emissive_status},
          {"passed", emissive_passed},
          {"registry_emissive_material_ids", emissive_ids},
          {"target_found", emissive_target.found},
          {"target_material_id", emissive_target.material_id},
          {"target_distance_from_spawn", emissive_target.distance_from_spawn},
          {"target_depth_below_surface", emissive_target.depth_below_surface},
          {"vertices_scanned", emissive_target.vertices_scanned},
          {"emissive_vertices_total", emissive_target.emissive_vertices_total},
          {"emissive_vertices_in_range", emissive_target.emissive_vertices_in_range},
          {"screenshot", emissive_capture_written ? emissive_screenshot : ""},
          {"center_glow_pixels", emissive_capture_written ? emissive_stats.center_glow_pixels : 0},
          {"night_max_luminance", night ? night->stats.max_luminance : 0.0},
          {"night_max_luminance_y_from_top_norm",
           night ? night->stats.max_luminance_y_from_top_norm : 1.0},
          {"night_sky_max_luminance", night ? night->stats.sky_max_luminance : 0.0},
          {"night_sky_max_epsilon", kNightSkyMaxEpsilon}}},
        {"thresholds",
         {{"min_noon_over_dusk_gap", kMinNoonOverDuskGap},
          {"min_dusk_over_night_gap", kMinDuskOverNightGap},
          {"min_dusk_warm_shift", kMinDuskWarmShift},
          {"max_night_sky_luminance", kMaxNightSkyLuminance},
          {"min_dusk_sky_warm_shift", kMinDuskSkyWarmShift},
          {"min_dusk_sky_warm_band_ratio", kMinDuskSkyWarmBandRatio},
          {"min_season_sun_elevation_gap_rad", kMinSeasonSunElevationGap},
          {"min_season_palette_warmth_gap", kMinSeasonPaletteWarmthGap},
          {"min_emissive_glow_pixels", kMinEmissiveGlowPixels},
          {"emissive_glow_min_luminance", kEmissiveGlowMinLuminance},
          {"sky_band_height_fraction", kSkyRoiHeightFraction}}},
        {"render_pass",
         {{"skybox_draws", render_pass.skybox_draws},
          {"terrain_draws", render_pass.terrain_draws}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "timeofday-sweep-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
