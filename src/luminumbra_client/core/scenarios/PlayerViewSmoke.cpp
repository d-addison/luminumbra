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

// --- player_view_smoke ---

namespace {

// player_view gate thresholds (luminumbra.player_view.v1).
constexpr std::size_t kPlayerViewMaxMissingFrustumChunks = 0;
constexpr double kPlayerViewMinRenderableFrustumRatio = 0.98;
constexpr double kPlayerViewMaxBelowHorizonSkyRatio = 0.005;
constexpr std::uint64_t kPlayerViewMaxNearBlackClusters = 0;
// Sim-side coverage range: the LOD0 near field (12 chunks).
constexpr float kPlayerViewCoverageDistance = 192.0f;
// Skybox signature at the pinned noon time of day (0.04): the horizon band
// measures ~(21-24, 60-63, 99-102) in captures - strongly blue-dominant.
// Terrain materials (grass/soil/stone/sand) are warm or grey and never
// satisfy b >= r + 35.
} // namespace

Luminumbra::Vec3 PlayerViewEyePosition(Luminumbra::world::GameSession* game_session) {
    if (!game_session || !game_session->GetWorldSystem()) {
        return Luminumbra::Vec3(0.0f);
    }
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    const float terrain_height =
        game_session->GetWorldSystem()->GetTerrainHeightAt(spawn.x, spawn.z);
    return Luminumbra::Vec3(spawn.x, terrain_height + 1.8f, spawn.z);
}

std::vector<PlayerViewStation> BuildPlayerViewStations(Luminumbra::world::GameSession* game_session,
                                                       const std::string& world_preset) {
    std::vector<PlayerViewStation> stations;
    stations.reserve(14);
    for (int i = 0; i < 12; ++i) {
        PlayerViewStation station;
        char name[16];
        std::snprintf(name, sizeof(name), "yaw_%03d", i * 30);
        station.name = name;
        station.yaw_degrees = static_cast<float>(i * 30);
        station.pitch_degrees = 0.0f;
        stations.push_back(station);
    }

    // Highest visible peak within the near field: aim a 13th station at it
    // with the appropriate pitch (tall summits sit above the eye-level
    // sweep's vertical FOV - exactly where the pre-span-fix streaming left
    // holes and churned summit chunks).
    if (game_session && game_session->GetWorldSystem()) {
        auto* world_system = game_session->GetWorldSystem();
        const Luminumbra::Vec3 eye = PlayerViewEyePosition(game_session);
        float best_height = -std::numeric_limits<float>::max();
        Luminumbra::Vec3 peak(0.0f);
        constexpr float kScanRange = 192.0f;
        constexpr float kScanStep = 8.0f;
        for (float dz = -kScanRange; dz <= kScanRange; dz += kScanStep) {
            for (float dx = -kScanRange; dx <= kScanRange; dx += kScanStep) {
                if (std::abs(dx) < 24.0f && std::abs(dz) < 24.0f) {
                    continue; // skip the camera's own column neighborhood
                }
                const float x = eye.x + dx;
                const float z = eye.z + dz;
                const float h = world_system->GetTerrainHeightAt(x, z);
                if (h > best_height) {
                    best_height = h;
                    peak = Luminumbra::Vec3(x, h, z);
                }
            }
        }
        PlayerViewStation peak_station;
        peak_station.name = "peak";
        peak_station.aim_at_target = true;
        peak_station.target = peak;
        stations.push_back(peak_station);
    }

    if (world_preset == "archipelago") {
        // Seed-424242 archipelago degenerate-geometry investigation region:
        // frame (-140, ~40, 205) and assert no void/black clusters there.
        PlayerViewStation degenerate_station;
        degenerate_station.name = "degenerate_region";
        degenerate_station.aim_at_target = true;
        degenerate_station.target = Luminumbra::Vec3(-140.0f, 40.0f, 205.0f);
        stations.push_back(degenerate_station);
    }

    return stations;
}

void ApplyPlayerViewCamera(Luminumbra::world::GameSession* game_session,
                           Luminumbra::Rendering::Camera* camera,
                           const PlayerViewStation& station) {
    if (!camera) {
        return;
    }

    camera->Position = PlayerViewEyePosition(game_session);
    camera->Zoom = 60.0f; // deterministic vertical FOV across stations
    if (station.aim_at_target) {
        AimCameraAt(camera, station.target);
    } else {
        camera->Yaw = station.yaw_degrees;
        camera->Pitch = station.pitch_degrees;
        camera->updateCameraVectors();
    }
}

std::array<Luminumbra::Vec4, 6>
ExtractCameraFrustumPlanes(const Luminumbra::Rendering::Camera& camera, int width, int height) {
    const glm::mat4 view_projection =
        camera.GetProjectionMatrix(std::max(1, width), std::max(1, height)) *
        camera.GetViewMatrix();

    // Gribb-Hartmann plane extraction (inward-facing, ax+by+cz+d >= 0 inside).
    const auto matrix_row = [&view_projection](int row) {
        return Luminumbra::Vec4(view_projection[0][row],
                                view_projection[1][row],
                                view_projection[2][row],
                                view_projection[3][row]);
    };
    const Luminumbra::Vec4 row0 = matrix_row(0);
    const Luminumbra::Vec4 row1 = matrix_row(1);
    const Luminumbra::Vec4 row2 = matrix_row(2);
    const Luminumbra::Vec4 row3 = matrix_row(3);

    std::array<Luminumbra::Vec4, 6> planes{
        row3 + row0, // left
        row3 - row0, // right
        row3 + row1, // bottom
        row3 - row1, // top
        row3 + row2, // near
        row3 - row2, // far
    };
    for (Luminumbra::Vec4& plane : planes) {
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length > 0.0f) {
            plane /= length;
        }
    }
    return planes;
}

PlayerViewPixelStats AnalyzePlayerViewPixels(const std::vector<unsigned char>& pixels,
                                             int width,
                                             int height,
                                             int horizon_row_from_top) {
    PlayerViewPixelStats stats;
    stats.width = width;
    stats.height = height;
    stats.horizon_row_from_top = std::clamp(horizon_row_from_top, 0, std::max(0, height - 1));
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    // Margin below the projected horizon row: distant terrain legitimately
    // converges on the horizon line, so the enforced ROI starts 2% of the
    // frame height below it. Same x-margins as the LOD hole analysis.
    const int margin = std::max(2, height / 50);
    const int roi_top = std::min(height, stats.horizon_row_from_top + margin);
    const int min_x = width / 64;
    const int max_x = width - min_x;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y; // glReadPixels rows are bottom-up
        if (y_from_top < roi_top) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            ++stats.below_horizon_pixels;
            if (IsBelowHorizonSkyPixel(
                    pixels[offset + 0u], pixels[offset + 1u], pixels[offset + 2u])) {
                ++stats.below_horizon_sky_pixels;
            }
        }
    }

    if (stats.below_horizon_pixels > 0) {
        stats.below_horizon_sky_ratio = static_cast<double>(stats.below_horizon_sky_pixels) /
                                        static_cast<double>(stats.below_horizon_pixels);
    }

    // Strict-void cluster pass (see PlayerViewPixelStats): connected
    // components of max(r,g,b) <= 2 pixels over the same below-horizon ROI
    // as the sky metric. The gate's premise - everything below the eye-level
    // horizon over loaded terrain must be rendered geometry - is what makes
    // a void cluster a defect there; above the horizon, point-blank cliff
    // faces in full shadow legitimately reach RGB 0-2 (lighting domain, not
    // coverage).
    std::vector<std::uint8_t> void_mask(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < roi_top) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            if (pixels[offset] <= 2u && pixels[offset + 1u] <= 2u && pixels[offset + 2u] <= 2u) {
                void_mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)] = 1u;
            }
        }
    }

    const std::uint64_t kMinVoidClusterPx =
        static_cast<std::uint64_t>(ScalePinnedArea(12, kCapturePinnedWidth, kCapturePinnedHeight));
    std::vector<std::size_t> flood_stack;
    for (std::size_t seed = 0; seed < void_mask.size(); ++seed) {
        if (void_mask[seed] != 1u) {
            continue;
        }
        std::uint64_t cluster_px = 0;
        flood_stack.clear();
        flood_stack.push_back(seed);
        void_mask[seed] = 2u;
        while (!flood_stack.empty()) {
            const std::size_t current = flood_stack.back();
            flood_stack.pop_back();
            ++cluster_px;
            const int cx = static_cast<int>(current % static_cast<std::size_t>(width));
            const int cy = static_cast<int>(current / static_cast<std::size_t>(width));
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = cx + dx;
                    const int ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const std::size_t neighbor =
                        static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) +
                        static_cast<std::size_t>(nx);
                    if (void_mask[neighbor] == 1u) {
                        void_mask[neighbor] = 2u;
                        flood_stack.push_back(neighbor);
                    }
                }
            }
        }
        stats.largest_void_cluster_px = std::max(stats.largest_void_cluster_px, cluster_px);
        if (cluster_px >= kMinVoidClusterPx) {
            ++stats.void_cluster_count;
        }
    }
    return stats;
}

bool PlayerViewSeaWaterInNearField(Luminumbra::world::GameSession* game_session) {
    if (!game_session || !game_session->GetWorldSystem()) {
        return false;
    }
    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 eye = PlayerViewEyePosition(game_session);
    constexpr float kScanStep = 8.0f;
    for (float dz = -kPlayerViewCoverageDistance; dz <= kPlayerViewCoverageDistance;
         dz += kScanStep) {
        for (float dx = -kPlayerViewCoverageDistance; dx <= kPlayerViewCoverageDistance;
             dx += kScanStep) {
            if (world_system->GetTerrainHeightAt(eye.x + dx, eye.z + dz) <
                Luminumbra::SEA_LEVEL - 0.25f) {
                return true;
            }
        }
    }
    return false;
}

void WritePlayerViewAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& world_preset,
    double duration_seconds,
    const std::vector<PlayerViewStationCapture>& captures,
    std::size_t expected_station_count,
    const Luminumbra::Systems::SHIELD_WorldSystem::RuntimeChunkStats& chunk_stats,
    bool enforce_sky_ratio) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    std::size_t max_missing = 0;
    double min_renderable_ratio = 1.0;
    double max_sky_ratio = 0.0;
    std::uint64_t max_clusters = 0;
    bool all_passed = captures.size() == expected_station_count;

    nlohmann::json station_rows = nlohmann::json::array();
    for (const PlayerViewStationCapture& capture : captures) {
        const bool station_passed =
            capture.coverage.missing_chunks <= kPlayerViewMaxMissingFrustumChunks &&
            capture.coverage.renderable_ratio >= kPlayerViewMinRenderableFrustumRatio &&
            (!enforce_sky_ratio ||
             capture.sky.below_horizon_sky_ratio < kPlayerViewMaxBelowHorizonSkyRatio) &&
            capture.sky.void_cluster_count <= kPlayerViewMaxNearBlackClusters;
        all_passed = all_passed && station_passed;

        max_missing = std::max(max_missing, capture.coverage.missing_chunks);
        min_renderable_ratio = std::min(min_renderable_ratio, capture.coverage.renderable_ratio);
        max_sky_ratio = std::max(max_sky_ratio, capture.sky.below_horizon_sky_ratio);
        max_clusters = std::max(max_clusters, capture.sky.void_cluster_count);

        station_rows.push_back({
            {"name", capture.station.name},
            {"yaw_degrees", capture.station.yaw_degrees},
            {"pitch_degrees", capture.station.pitch_degrees},
            {"aim_at_target", capture.station.aim_at_target},
            {"target", Vec3ToJson(capture.station.target)},
            {"file", capture.file},
            {"coverage",
             {
                 {"columns_considered", capture.coverage.columns_considered},
                 {"expected_chunks", capture.coverage.expected_chunks},
                 {"present_chunks", capture.coverage.present_chunks},
                 {"missing_frustum_surface_chunks", capture.coverage.missing_chunks},
                 {"renderable_chunks", capture.coverage.renderable_chunks},
                 {"renderable_frustum_ratio", capture.coverage.renderable_ratio},
             }},
            {"pixels",
             {
                 {"horizon_row_from_top", capture.sky.horizon_row_from_top},
                 {"below_horizon_pixels", capture.sky.below_horizon_pixels},
                 {"below_horizon_sky_pixels", capture.sky.below_horizon_sky_pixels},
                 {"below_horizon_sky_ratio", capture.sky.below_horizon_sky_ratio},
                 // Enforced void clusters: strict predicate max(r,g,b) <= 2
                 // (legit mountain shadow has a continuous dark tail at the
                 // LOD-seam gate's <= 10 bound; true voids stay at 0-2).
                 {"near_black_cluster_count", capture.sky.void_cluster_count},
                 {"largest_near_black_cluster_px", capture.sky.largest_void_cluster_px},
                 {"void_predicate", "max_rgb<=2"},
                 // Informative: the LOD-seam-tuned (<= 10) hole statistics.
                 {"holes", LodHolePixelStatsToJson(capture.holes)},
             }},
            {"passed", station_passed},
        });
    }

    const nlohmann::json artifact = {
        {"schema", "luminumbra.player_view.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"scenario", "player_view_smoke"},
        {"world_preset", world_preset},
        {"duration_seconds", duration_seconds},
        {"coverage_distance", kPlayerViewCoverageDistance},
        {"thresholds",
         {
             {"max_missing_frustum_surface_chunks", kPlayerViewMaxMissingFrustumChunks},
             {"min_renderable_frustum_ratio", kPlayerViewMinRenderableFrustumRatio},
             {"max_below_horizon_sky_ratio", kPlayerViewMaxBelowHorizonSkyRatio},
             {"max_near_black_cluster_count", kPlayerViewMaxNearBlackClusters},
             // The skybox and the water surface share hue at the pinned time
             // of day; with open sea in the near field the sky-leak ratio is
             // recorded but not enforced (coverage + void clusters still are).
             {"sky_ratio_enforced", enforce_sky_ratio},
         }},
        {"stations", station_rows},
        {"aggregates",
         {
             {"expected_stations", expected_station_count},
             {"captured_stations", captures.size()},
             {"max_missing_frustum_surface_chunks", max_missing},
             {"min_renderable_frustum_ratio", min_renderable_ratio},
             {"max_below_horizon_sky_ratio", max_sky_ratio},
             {"max_near_black_cluster_count", max_clusters},
         }},
        {"runtime_chunks",
         {
             {"total_chunks", chunk_stats.total_chunks},
             {"renderable_chunks", chunk_stats.renderable_chunks},
             {"sdf_skipped_chunks", chunk_stats.sdf_skipped_chunks},
             {"active_chunk_budget", 8192},
         }},
        {"gl_debug",
         {
             {"messages", gl_debug.messages},
             {"errors", gl_debug.errors},
             {"warnings", gl_debug.warnings},
             {"notifications", gl_debug.notifications},
         }},
        {"passed", all_passed && gl_debug.errors == 0},
    };

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "player-view-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
