#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/RuntimeScenarioHarness.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

std::atomic<uint64_t> g_gl_debug_message_count{0};
std::atomic<uint64_t> g_gl_debug_error_count{0};
std::atomic<uint64_t> g_gl_debug_warning_count{0};
std::atomic<uint64_t> g_gl_debug_notification_count{0};

bool HasCommandLineFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) {
            return true;
        }
    }
    return false;
}

std::string GetCommandLineOption(int argc, char* argv[], const std::string& flag, const std::string& fallback) {
    const std::string assignment_prefix = flag + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == flag && i + 1 < argc) {
            return argv[i + 1];
        }
        if (arg.rfind(assignment_prefix, 0) == 0) {
            return arg.substr(assignment_prefix.size());
        }
    }
    return fallback;
}

int GetCommandLineIntOption(int argc, char* argv[], const std::string& flag, int fallback) {
    const std::string value = GetCommandLineOption(argc, argv, flag, {});
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::max(1, std::stoi(value));
    } catch (...) {
        LUMINUMBRA_CORE_WARN("Invalid integer value '{}' for {}; using {}", value, flag, fallback);
        return fallback;
    }
}

uint64_t GetCommandLineUInt64Option(int argc, char* argv[], const std::string& flag, uint64_t fallback) {
    const std::string value = GetCommandLineOption(argc, argv, flag, {});
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stoull(value);
    } catch (...) {
        LUMINUMBRA_CORE_WARN("Invalid unsigned integer value '{}' for {}; using {}", value, flag, fallback);
        return fallback;
    }
}

RuntimeScenarioConfig ParseRuntimeScenarioConfig(int argc, char* argv[], const std::filesystem::path& root_dir) {
    RuntimeScenarioConfig config;
    config.scenario = GetCommandLineOption(argc, argv, "--scenario", "");
    config.auto_create_world = HasCommandLineFlag(argc, argv, "--auto-create-world");
    config.auto_enter_world = HasCommandLineFlag(argc, argv, "--auto-enter-world");
    config.no_audio = HasCommandLineFlag(argc, argv, "--no-audio");
    config.no_ui = HasCommandLineFlag(argc, argv, "--no-ui");
    config.hidden_window = HasCommandLineFlag(argc, argv, "--hidden-window");
    config.enable_gpu_sdf_runtime = HasCommandLineFlag(argc, argv, "--enable-gpu-sdf-runtime");
    config.readiness_timeout_seconds = GetCommandLineIntOption(argc, argv, "--readiness-timeout", config.readiness_timeout_seconds);
    config.horizon_radius = GetCommandLineIntOption(argc, argv, "--horizon-radius", config.horizon_radius);
    config.collision_radius = GetCommandLineIntOption(argc, argv, "--collision-radius", config.collision_radius);
    config.coverage_radius = GetCommandLineIntOption(argc, argv, "--coverage-radius", config.coverage_radius);
    config.min_renderable_chunks = static_cast<size_t>(GetCommandLineUInt64Option(argc, argv, "--min-renderable-chunks", config.min_renderable_chunks));
    config.min_collision_chunks = static_cast<size_t>(GetCommandLineUInt64Option(argc, argv, "--min-collision-chunks", config.min_collision_chunks));
    config.memory_watermark_mb = GetCommandLineUInt64Option(argc, argv, "--memory-watermark-mb", 0);

    const int default_timed_run = config.auto_world_smoke() ? 300 : ((config.lod_ground_smoke() || config.water_visual_smoke() || config.material_visual_smoke() || config.lod_boundary_oscillation_smoke()) ? 60 : 0);
    config.timed_run_seconds = GetCommandLineIntOption(argc, argv, "--timed-run", default_timed_run);

    if (config.auto_world_smoke() || config.lod_ground_smoke() || config.water_visual_smoke() || config.material_visual_smoke() || config.lod_boundary_oscillation_smoke()) {
        config.auto_create_world = true;
        config.auto_enter_world = true;
    }

    const std::filesystem::path default_artifact_dir = root_dir / "build/debug/test-artifacts/runtime";
    const std::filesystem::path default_audio_telemetry_path = root_dir / "build/debug/test-artifacts/audio/audio-telemetry.json";
    const std::filesystem::path default_crash_dir = root_dir / "build/debug/crashes";
    config.artifact_dir = GetCommandLineOption(argc, argv, "--runtime-artifact-dir", default_artifact_dir.string());
    config.audio_telemetry_path = GetCommandLineOption(argc, argv, "--audio-telemetry-path", default_audio_telemetry_path.string());
    config.crash_dir = GetCommandLineOption(argc, argv, "--crash-dir", default_crash_dir.string());
    return config;
}

std::tm UtcTime(std::time_t value) {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif
    return tm;
}

std::string TimestampUtc() {
    const std::time_t now = std::time(nullptr);
    const std::tm tm = UtcTime(now);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string TimestampForFile() {
    const std::time_t now = std::time(nullptr);
    const std::tm tm = UtcTime(now);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return output.str();
}

nlohmann::json Vec3ToJson(const Luminumbra::Vec3& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z}
    };
}

nlohmann::json IVec3ToJson(const Luminumbra::IVec3& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z}
    };
}

GLDebugRuntimeStats CurrentGLDebugRuntimeStats() {
    return {
        g_gl_debug_message_count.load(std::memory_order_relaxed),
        g_gl_debug_error_count.load(std::memory_order_relaxed),
        g_gl_debug_warning_count.load(std::memory_order_relaxed),
        g_gl_debug_notification_count.load(std::memory_order_relaxed)
    };
}

double SmoothStep01(double value) {
    const double t = std::clamp(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Luminumbra::Vec3 CalculateLodGroundCameraPosition(
    const RuntimeScenarioConfig& config,
    Luminumbra::world::GameSession* game_session,
    double elapsed_seconds)
{
    const Luminumbra::Vec3 spawn = game_session ? game_session->GetMetadata().spawnPoint : Luminumbra::Vec3(0.0f);
    auto* world_system = game_session ? game_session->GetWorldSystem() : nullptr;
    const double duration = static_cast<double>(std::max(1, config.timed_run_seconds));
    const double normalized = std::clamp(elapsed_seconds / duration, 0.0, 1.0);
    const double travel = SmoothStep01((normalized - 0.12) / 0.58);

    constexpr double kPi = 3.14159265358979323846;
    const float x = spawn.x + static_cast<float>(travel * 240.0);
    const float z = spawn.z + static_cast<float>(std::sin(travel * kPi * 1.5) * 64.0);
    const float terrain_height = world_system ? world_system->GetTerrainHeightAt(x, z) : spawn.y;
    const float y = terrain_height + 180.0f;
    return Luminumbra::Vec3(x, y, z);
}

Luminumbra::Vec3 CalculateLodGroundFocusPosition(
    Luminumbra::world::GameSession* game_session,
    const Luminumbra::Vec3& camera_position,
    double elapsed_seconds)
{
    auto* world_system = game_session ? game_session->GetWorldSystem() : nullptr;
    const float yaw_degrees = -72.0f + static_cast<float>(std::sin(elapsed_seconds * 0.35) * 10.0);
    const float yaw_radians = glm::radians(yaw_degrees);
    const Luminumbra::Vec3 forward(
        std::cos(yaw_radians),
        0.0f,
        std::sin(yaw_radians)
    );
    const Luminumbra::Vec3 focus_xz = camera_position + forward * 120.0f;
    const float terrain_height = world_system
        ? world_system->GetTerrainHeightAt(focus_xz.x, focus_xz.z)
        : camera_position.y - 180.0f;
    return Luminumbra::Vec3(focus_xz.x, terrain_height, focus_xz.z);
}

void ApplyLodGroundCameraPath(
    const RuntimeScenarioConfig& config,
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    double elapsed_seconds)
{
    if (!camera || !game_session) {
        return;
    }

    camera->Position = CalculateLodGroundCameraPosition(config, game_session, elapsed_seconds);
    const Luminumbra::Vec3 focus = CalculateLodGroundFocusPosition(game_session, camera->Position, elapsed_seconds);
    const glm::vec3 direction = glm::normalize(focus - camera->Position);
    camera->Yaw = glm::degrees(std::atan2(direction.z, direction.x));
    camera->Pitch = glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    camera->updateCameraVectors();
}

WaterVisualCameraTarget FindWaterVisualCameraTarget(Luminumbra::world::GameSession* game_session) {
    WaterVisualCameraTarget target;
    if (!game_session || !game_session->GetWorldSystem()) {
        return target;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;

    const auto renderable_chunks = world_system->get_renderable_chunks();
    float best_mesh_score = 0.0f;
    for (const Luminumbra::Chunk* chunk : renderable_chunks) {
        if (!chunk || chunk->water_mesh_vertices.empty() || chunk->water_mesh_indices.empty()) {
            continue;
        }

        const Luminumbra::Vec3 chunk_origin = Luminumbra::Vec3(chunk->get_coords() * Luminumbra::IVec3(
            Luminumbra::CHUNK_SIZE_X,
            Luminumbra::CHUNK_SIZE_Y,
            Luminumbra::CHUNK_SIZE_Z
        ));
        Luminumbra::Vec3 accumulated_position(0.0f);
        const std::size_t sample_count = std::min<std::size_t>(chunk->water_mesh_vertices.size(), 256u);
        const std::size_t stride = std::max<std::size_t>(1u, chunk->water_mesh_vertices.size() / sample_count);
        std::size_t collected = 0;
        for (std::size_t i = 0; i < chunk->water_mesh_vertices.size() && collected < sample_count; i += stride) {
            accumulated_position += chunk_origin + chunk->water_mesh_vertices[i].position;
            ++collected;
        }
        if (collected == 0) {
            continue;
        }

        const Luminumbra::Vec3 focus = accumulated_position / static_cast<float>(collected);
        const float distance = glm::length(Luminumbra::Vec3(focus.x, spawn.y, focus.z) - Luminumbra::Vec3(spawn.x, spawn.y, spawn.z));
        int open_water_samples = 0;
        float min_water_depth = std::numeric_limits<float>::max();
        for (int oz = -2; oz <= 2; ++oz) {
            for (int ox = -2; ox <= 2; ++ox) {
                const float sample_x = focus.x + static_cast<float>(ox * 16);
                const float sample_z = focus.z + static_cast<float>(oz * 16);
                const float water_depth = Luminumbra::SEA_LEVEL - world_system->GetTerrainHeightAt(sample_x, sample_z);
                min_water_depth = std::min(min_water_depth, water_depth);
                if (water_depth > 1.0f) {
                    ++open_water_samples;
                }
            }
        }
        if (open_water_samples < 20) {
            continue;
        }
        const float score =
            static_cast<float>(std::min<std::size_t>(chunk->water_mesh_indices.size(), 20000u)) * 0.01f +
            static_cast<float>(open_water_samples) * 250.0f +
            std::max(0.0f, min_water_depth) * 5.0f -
            distance * 0.02f;
        if (!target.found || score > best_mesh_score) {
            target.found = true;
            target.focus = focus + Luminumbra::Vec3(0.0f, 0.1f, 0.0f);
            target.terrain_height = world_system->GetTerrainHeightAt(focus.x, focus.z);
            target.supporting_water_samples = open_water_samples;
            best_mesh_score = score;
        }
    }

    if (target.found) {
        const Luminumbra::Vec3 camera_offset(0.0f, 80.0f, 24.0f);
        target.camera_position = target.focus + camera_offset;
        target.camera_terrain_height = world_system->GetTerrainHeightAt(target.camera_position.x, target.camera_position.z);
        target.camera_position.y = std::max(target.camera_position.y, target.camera_terrain_height + 32.0f);
        return target;
    }

    constexpr int kSearchRadius = 512;
    constexpr int kSearchStep = 16;
    constexpr float kMinWaterDepth = 0.5f;
    float best_score = 0.0f;

    for (int dz = -kSearchRadius; dz <= kSearchRadius; dz += kSearchStep) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; dx += kSearchStep) {
            const float x = spawn.x + static_cast<float>(dx);
            const float z = spawn.z + static_cast<float>(dz);
            const float terrain_height = world_system->GetTerrainHeightAt(x, z);
            const float water_depth = Luminumbra::SEA_LEVEL - terrain_height;
            if (water_depth < kMinWaterDepth) {
                continue;
            }

            int supporting_water_samples = 0;
            for (int oz = -2; oz <= 2; ++oz) {
                for (int ox = -2; ox <= 2; ++ox) {
                    const float sx = x + static_cast<float>(ox * kSearchStep);
                    const float sz = z + static_cast<float>(oz * kSearchStep);
                    if (world_system->GetTerrainHeightAt(sx, sz) < Luminumbra::SEA_LEVEL - 0.25f) {
                        ++supporting_water_samples;
                    }
                }
            }
            if (supporting_water_samples < 20) {
                continue;
            }

            const float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
            const float score =
                static_cast<float>(supporting_water_samples) * 100.0f +
                water_depth * 10.0f -
                distance * 0.02f;
            if (!target.found || score > best_score) {
                target.found = true;
                target.focus = Luminumbra::Vec3(x, Luminumbra::SEA_LEVEL + 0.1f, z);
                target.terrain_height = terrain_height;
                target.supporting_water_samples = supporting_water_samples;
                best_score = score;
            }
        }
    }

    if (!target.found) {
        return target;
    }

    const Luminumbra::Vec3 camera_offset(0.0f, 80.0f, 24.0f);
    target.camera_position = target.focus + camera_offset;
    target.camera_terrain_height = world_system->GetTerrainHeightAt(target.camera_position.x, target.camera_position.z);
    target.camera_position.y = std::max(target.camera_position.y, target.camera_terrain_height + 32.0f);
    return target;
}

void AimCameraAt(Luminumbra::Rendering::Camera* camera, const Luminumbra::Vec3& focus) {
    if (!camera) {
        return;
    }

    const glm::vec3 direction = glm::normalize(focus - camera->Position);
    camera->Yaw = glm::degrees(std::atan2(direction.z, direction.x));
    camera->Pitch = glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    camera->updateCameraVectors();
}

void ApplyWaterVisualCamera(
    Luminumbra::Rendering::Camera* camera,
    const WaterVisualCameraTarget& target)
{
    if (!camera || !target.found) {
        return;
    }

    camera->Position = target.camera_position;
    AimCameraAt(camera, target.focus);
}

// Material visual targets reuse the water target shape: a focus point, a
// raised camera position, and a count of supporting samples that prove the
// surrounding area really is the expected material.
WaterVisualCameraTarget FindMaterialVisualCameraTarget(Luminumbra::world::GameSession* game_session) {
    WaterVisualCameraTarget target;
    if (!game_session || !game_session->GetWorldSystem()) {
        return target;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;

    // Sand band per GetTerrainMaterialAt: dry terrain near sea level. Look for
    // the broadest beach reachable; fall back to the best candidate seen so a
    // narrow fringe beach still produces an honest capture for the pixel gate.
    constexpr float kBeachMinHeight = 0.25f;  // above SEA_LEVEL offset
    constexpr float kBeachMaxHeight = 12.0f;  // comfortably inside the <36 beach band
    constexpr int kSearchRadius = 512;
    constexpr int kSearchStep = 8;
    float best_score = -std::numeric_limits<float>::max();
    std::size_t in_band_candidates = 0;

    for (int dz = -kSearchRadius; dz <= kSearchRadius; dz += kSearchStep) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; dx += kSearchStep) {
            const float x = spawn.x + static_cast<float>(dx);
            const float z = spawn.z + static_cast<float>(dz);
            const float terrain_height = world_system->GetTerrainHeightAt(x, z);
            const float height_above_sea = terrain_height - Luminumbra::SEA_LEVEL;
            if (height_above_sea < kBeachMinHeight || height_above_sea > kBeachMaxHeight) {
                continue;
            }
            ++in_band_candidates;

            int supporting_sand_samples = 0;
            for (int oz = -2; oz <= 2; ++oz) {
                for (int ox = -2; ox <= 2; ++ox) {
                    const float sx = x + static_cast<float>(ox * 6);
                    const float sz = z + static_cast<float>(oz * 6);
                    const float sample_height = world_system->GetTerrainHeightAt(sx, sz) - Luminumbra::SEA_LEVEL;
                    if (sample_height >= kBeachMinHeight && sample_height <= kBeachMaxHeight) {
                        ++supporting_sand_samples;
                    }
                }
            }

            const float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
            const float score =
                static_cast<float>(supporting_sand_samples) * 100.0f -
                distance * 0.05f;
            if (!target.found || score > best_score) {
                target.found = true;
                target.focus = Luminumbra::Vec3(x, terrain_height, z);
                target.terrain_height = terrain_height;
                target.supporting_water_samples = supporting_sand_samples;
                best_score = score;
            }
        }
    }

    LUMINUMBRA_CORE_INFO(
        "Material visual target scan: in_band_candidates={}, found={}, focus=({:.1f},{:.1f},{:.1f}), supporting={}",
        in_band_candidates,
        target.found,
        target.focus.x, target.focus.y, target.focus.z,
        target.supporting_water_samples);

    if (!target.found) {
        return target;
    }

    const Luminumbra::Vec3 camera_offset(0.0f, 26.0f, 12.0f);
    target.camera_position = target.focus + camera_offset;
    target.camera_terrain_height = world_system->GetTerrainHeightAt(target.camera_position.x, target.camera_position.z);
    target.camera_position.y = std::max(target.camera_position.y, target.camera_terrain_height + 14.0f);
    return target;
}

bool IsWaterLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    const bool blue_green_dominant = b >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 10))
        && g >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 4));
    const bool plausible_water_luma = b >= 45 && g >= 42 && r <= 135;
    const bool plausible_dark_water_luma = b >= 24 && g >= 18 && r <= 70;
    const int gb_delta = std::abs(static_cast<int>(g) - static_cast<int>(b));
    return blue_green_dominant && (plausible_water_luma || plausible_dark_water_luma) && gb_delta <= 95;
}

bool IsDarkVoidPixel(unsigned char r, unsigned char g, unsigned char b) {
    return r < 24 &&
        g < 34 &&
        b < 54 &&
        b >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 8)) &&
        b >= static_cast<unsigned char>(std::min(255, static_cast<int>(g) + 4));
}

bool IsNearBlackPixel(unsigned char r, unsigned char g, unsigned char b) {
    return r < 24 && g < 24 && b < 24;
}

bool IsBackgroundBluePixel(unsigned char r, unsigned char g, unsigned char b) {
    return r >= 38 &&
        r <= 110 &&
        g >= 50 &&
        g <= 130 &&
        b >= 70 &&
        b <= 150 &&
        b >= static_cast<unsigned char>(std::min(255, static_cast<int>(r) + 8)) &&
        std::abs(static_cast<int>(b) - static_cast<int>(g)) <= 55;
}

ScreenshotPixelStats AnalyzeScreenshotPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    ScreenshotPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 || pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    const int min_x = width / 5;
    const int max_x = width - min_x;
    const int min_top_y = height / 4;
    const int max_top_y = (height * 9) / 10;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= max_top_y) {
            continue;
        }

        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            ++stats.roi_pixels;
            if (IsWaterLikePixel(r, g, b)) {
                ++stats.water_like_pixels;
            }
            if (r < 16 && g < 20 && b < 28) {
                ++stats.dark_pixels;
            }
            if (r > 80 && g > 100 && b > 120 && std::abs(static_cast<int>(b) - static_cast<int>(g)) < 40) {
                ++stats.bright_sky_like_pixels;
            }
        }
    }

    if (stats.roi_pixels > 0) {
        stats.water_like_ratio = static_cast<double>(stats.water_like_pixels) / static_cast<double>(stats.roi_pixels);
    }
    return stats;
}

LodHolePixelStats AnalyzeLodHolePixels(const std::vector<unsigned char>& pixels, int width, int height) {
    LodHolePixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 || pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    const int min_x = width / 64;
    const int max_x = width - min_x;
    const int min_top_y = height / 4;
    const int max_top_y = height;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= max_top_y) {
            continue;
        }

        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            ++stats.roi_pixels;
            if (IsDarkVoidPixel(r, g, b)) {
                ++stats.dark_void_pixels;
            }
            if (IsNearBlackPixel(r, g, b)) {
                ++stats.near_black_pixels;
            }
            if (IsBackgroundBluePixel(r, g, b)) {
                ++stats.background_blue_pixels;
            }
        }
    }

    if (stats.roi_pixels > 0) {
        stats.dark_void_ratio = static_cast<double>(stats.dark_void_pixels) / static_cast<double>(stats.roi_pixels);
        stats.near_black_ratio = static_cast<double>(stats.near_black_pixels) / static_cast<double>(stats.roi_pixels);
        stats.background_blue_ratio = static_cast<double>(stats.background_blue_pixels) / static_cast<double>(stats.roi_pixels);
    }
    return stats;
}

bool WriteBackbufferPpm(
    const std::filesystem::path& path,
    int width,
    int height,
    ScreenshotPixelStats* out_stats,
    LodHolePixelStats* out_lod_hole_stats)
{
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LUMINUMBRA_CORE_ERROR("Failed to create screenshot directory '{}': {}", path.parent_path().string(), ec.message());
        return false;
    }

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    if (out_stats) {
        *out_stats = AnalyzeScreenshotPixels(pixels, width, height);
    }
    if (out_lod_hole_stats) {
        *out_lod_hole_stats = AnalyzeLodHolePixels(pixels, width, height);
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write screenshot artifact: {}", path.string());
        return false;
    }

    output << "P6\n" << width << ' ' << height << "\n255\n";
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    for (int row = height - 1; row >= 0; --row) {
        const std::size_t offset = static_cast<std::size_t>(row) * row_stride;
        output.write(reinterpret_cast<const char*>(pixels.data() + offset), static_cast<std::streamsize>(row_stride));
    }
    return true;
}

nlohmann::json LodHolePixelStatsToJson(const LodHolePixelStats& stats) {
    return {
        {"width", stats.width},
        {"height", stats.height},
        {"roi_pixels", stats.roi_pixels},
        {"dark_void_pixels", stats.dark_void_pixels},
        {"near_black_pixels", stats.near_black_pixels},
        {"background_blue_pixels", stats.background_blue_pixels},
        {"dark_void_ratio", stats.dark_void_ratio},
        {"near_black_ratio", stats.near_black_ratio},
        {"background_blue_ratio", stats.background_blue_ratio}
    };
}

nlohmann::json ScreenshotPixelStatsToJson(const ScreenshotPixelStats& stats) {
    return {
        {"width", stats.width},
        {"height", stats.height},
        {"roi_pixels", stats.roi_pixels},
        {"water_like_pixels", stats.water_like_pixels},
        {"dark_pixels", stats.dark_pixels},
        {"bright_sky_like_pixels", stats.bright_sky_like_pixels},
        {"water_like_ratio", stats.water_like_ratio}
    };
}

nlohmann::json WaterVisualTargetToJson(const WaterVisualCameraTarget& target) {
    return {
        {"found", target.found},
        {"focus", Vec3ToJson(target.focus)},
        {"camera_position", Vec3ToJson(target.camera_position)},
        {"terrain_height", target.terrain_height},
        {"camera_terrain_height", target.camera_terrain_height},
        {"supporting_water_samples", target.supporting_water_samples}
    };
}

void WriteWaterVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& screenshot,
    const WaterVisualCameraTarget& target,
    const ScreenshotPixelStats& pixel_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass,
    const Luminumbra::Rendering::RenderPipeline::MeshUploadFrameStats& upload_queue)
{
    constexpr std::uint64_t kMinWaterLikePixels = 2500;
    constexpr double kMinWaterLikeRatio = 0.02;
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed =
        target.found &&
        render_pass.water_draws > 0 &&
        render_pass.water_indices_drawn > 0 &&
        pixel_stats.water_like_pixels >= kMinWaterLikePixels &&
        pixel_stats.water_like_ratio >= kMinWaterLikeRatio &&
        gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.water_visual_analysis.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"screenshot", screenshot},
        {"target", WaterVisualTargetToJson(target)},
        {"pixels", ScreenshotPixelStatsToJson(pixel_stats)},
        {"thresholds", {
            {"min_water_like_pixels", kMinWaterLikePixels},
            {"min_water_like_ratio", kMinWaterLikeRatio}
        }},
        {"render_pass", {
            {"water_draws", render_pass.water_draws},
            {"water_indices_drawn", render_pass.water_indices_drawn},
            {"terrain_draws", render_pass.terrain_draws},
            {"terrain_indices_drawn", render_pass.terrain_indices_drawn}
        }},
        {"upload_queue", {
            {"water_upload_candidates", upload_queue.water_upload_candidates},
            {"water_uploads_deferred", upload_queue.water_uploads_deferred},
            {"terrain_upload_candidates", upload_queue.terrain_upload_candidates},
            {"terrain_uploads_deferred", upload_queue.terrain_uploads_deferred}
        }},
        {"gl_debug", {
            {"messages", gl_debug.messages},
            {"errors", gl_debug.errors},
            {"warnings", gl_debug.warnings},
            {"notifications", gl_debug.notifications}
        }}
    };

    std::ofstream output(artifact_dir / "water-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

// Calibrated against noon captures: lit sand measures around RGB(67,67,39) -
// red and green track together while blue trails by a wide margin. Grass is
// green-led (g far above r), soil and shadows fall below the brightness
// floor, so neither aliases into this bucket.
bool IsSandLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    return r >= 45 &&
           static_cast<int>(r) + 5 >= static_cast<int>(g) &&
           static_cast<int>(g) - static_cast<int>(b) >= 12 &&
           static_cast<int>(r) - static_cast<int>(b) >= 18;
}

// The grey fallback failure renders as a flat grey: all channels within a
// narrow spread, above shadow black and below sky white.
bool IsGreyFallbackPixel(unsigned char r, unsigned char g, unsigned char b) {
    const int max_channel = std::max({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    const int min_channel = std::min({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    return (max_channel - min_channel) <= 12 && max_channel >= 30 && max_channel <= 215;
}

void MaterialRoiBounds(int width, int height, int& min_x, int& max_x, int& min_top_y, int& max_top_y) {
    min_x = width / 6;
    max_x = width - width / 6;
    min_top_y = (height * 2) / 5;
    max_top_y = height;
}

MaterialPixelStats AnalyzeMaterialPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    MaterialPixelStats stats;
    stats.width = width;
    stats.height = height;
    if (width <= 0 || height <= 0 || pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return stats;
    }

    int min_x = 0;
    int max_x = 0;
    int min_top_y = 0;
    int max_top_y = 0;
    MaterialRoiBounds(width, height, min_x, max_x, min_top_y, max_top_y);
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < min_top_y || y_from_top >= max_top_y) {
            continue;
        }
        for (int x = min_x; x < max_x; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            ++stats.roi_pixels;
            if (IsSandLikePixel(r, g, b)) {
                ++stats.sand_pixels;
            } else if (IsWaterLikePixel(r, g, b)) {
                ++stats.water_like_pixels;
            } else if (IsGreyFallbackPixel(r, g, b)) {
                ++stats.grey_fallback_pixels;
            } else {
                ++stats.other_pixels;
            }
        }
    }

    if (stats.roi_pixels > 0) {
        stats.sand_ratio = static_cast<double>(stats.sand_pixels) / static_cast<double>(stats.roi_pixels);
        stats.grey_fallback_ratio = static_cast<double>(stats.grey_fallback_pixels) / static_cast<double>(stats.roi_pixels);
    }
    return stats;
}

bool WritePixelBufferPpm(
    const std::filesystem::path& path,
    int width,
    int height,
    const std::vector<unsigned char>& pixels)
{
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LUMINUMBRA_CORE_ERROR("Failed to create screenshot directory '{}': {}", path.parent_path().string(), ec.message());
        return false;
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write screenshot artifact: {}", path.string());
        return false;
    }

    output << "P6\n" << width << ' ' << height << "\n255\n";
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    for (int row = height - 1; row >= 0; --row) {
        const std::size_t offset = static_cast<std::size_t>(row) * row_stride;
        output.write(reinterpret_cast<const char*>(pixels.data() + offset), static_cast<std::streamsize>(row_stride));
    }
    return true;
}

// Heatmap legend: sand -> gold, grey fallback -> magenta (the failure being
// gated must be unmissable), water -> blue, other ROI -> dimmed luminance,
// outside ROI -> heavily dimmed luminance.
std::vector<unsigned char> BuildMaterialHeatmap(const std::vector<unsigned char>& pixels, int width, int height) {
    std::vector<unsigned char> heatmap(pixels.size());
    int min_x = 0;
    int max_x = 0;
    int min_top_y = 0;
    int max_top_y = 0;
    MaterialRoiBounds(width, height, min_x, max_x, min_top_y, max_top_y);
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;

    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        const bool row_in_roi = y_from_top >= min_top_y && y_from_top < max_top_y;
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char r = pixels[offset + 0u];
            const unsigned char g = pixels[offset + 1u];
            const unsigned char b = pixels[offset + 2u];
            const unsigned char luminance = static_cast<unsigned char>((static_cast<int>(r) + g + b) / 3);
            const bool in_roi = row_in_roi && x >= min_x && x < max_x;

            unsigned char out_r = static_cast<unsigned char>(luminance / 4);
            unsigned char out_g = out_r;
            unsigned char out_b = out_r;
            if (in_roi) {
                if (IsSandLikePixel(r, g, b)) {
                    out_r = 240; out_g = 200; out_b = 40;
                } else if (IsWaterLikePixel(r, g, b)) {
                    out_r = 40; out_g = 80; out_b = 220;
                } else if (IsGreyFallbackPixel(r, g, b)) {
                    out_r = 255; out_g = 0; out_b = 255;
                } else {
                    out_r = static_cast<unsigned char>(luminance / 2);
                    out_g = out_r;
                    out_b = out_r;
                }
            }
            heatmap[offset + 0u] = out_r;
            heatmap[offset + 1u] = out_g;
            heatmap[offset + 2u] = out_b;
        }
    }
    return heatmap;
}

void WriteMaterialVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& screenshot,
    const std::string& heatmap_screenshot,
    const WaterVisualCameraTarget& target,
    const MaterialPixelStats& pixel_stats,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass)
{
    constexpr std::uint64_t kMinSandPixels = 2000;
    constexpr double kMinSandRatio = 0.02;
    constexpr double kMaxGreyFallbackRatio = 0.125;
    const std::uint64_t max_grey_fallback_pixels = static_cast<std::uint64_t>(
        static_cast<double>(pixel_stats.roi_pixels) * kMaxGreyFallbackRatio);
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed =
        target.found &&
        render_pass.terrain_draws > 0 &&
        render_pass.terrain_indices_drawn > 0 &&
        pixel_stats.sand_pixels >= kMinSandPixels &&
        pixel_stats.sand_ratio >= kMinSandRatio &&
        pixel_stats.grey_fallback_pixels <= max_grey_fallback_pixels &&
        gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.material_visual_analysis.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"screenshot", screenshot},
        {"heatmap_screenshot", heatmap_screenshot},
        {"target", WaterVisualTargetToJson(target)},
        {"roi", {
            {"width", pixel_stats.width},
            {"height", pixel_stats.height},
            {"roi_pixels", pixel_stats.roi_pixels},
            {"water_like_pixels", pixel_stats.water_like_pixels},
            {"other_pixels", pixel_stats.other_pixels}
        }},
        {"materials", nlohmann::json::array({
            {
                {"material_id", 4},
                {"name", "Sand"},
                {"pixels", {
                    {"classified_pixels", pixel_stats.sand_pixels},
                    {"classified_ratio", pixel_stats.sand_ratio},
                    {"grey_fallback_pixels", pixel_stats.grey_fallback_pixels},
                    {"grey_fallback_ratio", pixel_stats.grey_fallback_ratio}
                }},
                {"thresholds", {
                    {"min_classified_pixels", kMinSandPixels},
                    {"min_classified_ratio", kMinSandRatio},
                    {"max_grey_fallback_pixels", max_grey_fallback_pixels},
                    {"max_grey_fallback_ratio", kMaxGreyFallbackRatio}
                }}
            }
        })},
        {"render_pass", {
            {"terrain_draws", render_pass.terrain_draws},
            {"terrain_indices_drawn", render_pass.terrain_indices_drawn},
            {"water_draws", render_pass.water_draws}
        }},
        {"gl_debug", {
            {"messages", gl_debug.messages},
            {"errors", gl_debug.errors},
            {"warnings", gl_debug.warnings},
            {"notifications", gl_debug.notifications}
        }}
    };

    std::ofstream output(artifact_dir / "material-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

void WriteLodGroundScreenshotIndex(
    const std::filesystem::path& artifact_dir,
    const std::vector<std::string>& screenshots)
{
    nlohmann::json captures = nlohmann::json::array();
    for (const std::string& screenshot : screenshots) {
        captures.push_back({{"file", screenshot}});
    }

    nlohmann::json artifact = {
        {"schema", "luminumbra.lod_ground_screenshots.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"captures", captures}
    };
    std::ofstream output(artifact_dir / "lod-ground-screenshots.json");
    output << std::setw(2) << artifact << '\n';
}

void WriteLodGroundVisualAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::vector<LodGroundVisualCapture>& captures)
{
    constexpr std::uint64_t kMaxDarkVoidPixels = 18000;
    constexpr std::uint64_t kMaxNearBlackPixels = 4000;
    constexpr std::uint64_t kMaxBackgroundBluePixels = 22000;
    constexpr double kMaxDarkVoidRatio = 0.020;
    constexpr double kMaxNearBlackRatio = 0.0065;
    constexpr double kMaxBackgroundBlueRatio = 0.025;

    bool passed = captures.size() >= 3;
    nlohmann::json captures_json = nlohmann::json::array();
    for (const LodGroundVisualCapture& capture : captures) {
        const bool enforced = capture.role == "mid" || capture.role == "end";
        const bool capture_passed =
            !enforced ||
            (capture.pixels.dark_void_pixels <= kMaxDarkVoidPixels &&
             capture.pixels.dark_void_ratio <= kMaxDarkVoidRatio &&
             capture.pixels.near_black_pixels <= kMaxNearBlackPixels &&
             capture.pixels.near_black_ratio <= kMaxNearBlackRatio &&
             capture.pixels.background_blue_pixels <= kMaxBackgroundBluePixels &&
             capture.pixels.background_blue_ratio <= kMaxBackgroundBlueRatio);
        if (!capture_passed) {
            passed = false;
        }
        captures_json.push_back({
            {"role", capture.role},
            {"file", capture.file},
            {"enforced", enforced},
            {"passed", capture_passed},
            {"pixels", LodHolePixelStatsToJson(capture.pixels)}
        });
    }

    nlohmann::json artifact = {
        {"schema", "luminumbra.lod_ground_visual_analysis.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"thresholds", {
            {"max_dark_void_pixels", kMaxDarkVoidPixels},
            {"max_dark_void_ratio", kMaxDarkVoidRatio},
            {"max_near_black_pixels", kMaxNearBlackPixels},
            {"max_near_black_ratio", kMaxNearBlackRatio},
            {"max_background_blue_pixels", kMaxBackgroundBluePixels},
            {"max_background_blue_ratio", kMaxBackgroundBlueRatio}
        }},
        {"captures", captures_json}
    };

    std::ofstream output(artifact_dir / "lod-ground-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

void WriteStreamingTelemetry(
    const std::filesystem::path& artifact_dir,
    const std::string& scenario,
    double duration_seconds,
    const Luminumbra::Systems::SHIELD_WorldSystem::StreamingTelemetryStats& stats)
{
    const double drain_rate_per_s = duration_seconds > 0.0
        ? static_cast<double>(stats.cumulative_scheduled_meshing) / duration_seconds
        : 0.0;
    const bool deferred_age_bounded =
        stats.frames_observed == 0 || stats.max_deferred_age_frames < stats.frames_observed;
    const bool backlog_bounded = stats.last_queue_depth == 0 && deferred_age_bounded;

    nlohmann::json artifact = {
        {"schema", "luminumbra.streaming_telemetry.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"scenario", scenario},
        {"duration_seconds", duration_seconds},
        {"frames_observed", stats.frames_observed},
        {"peak_queue_depth", stats.peak_queue_depth},
        {"peak_meshing_candidates", stats.peak_meshing_candidates},
        {"cumulative_scheduled_meshing", stats.cumulative_scheduled_meshing},
        {"cumulative_deferred_meshing", stats.cumulative_deferred_meshing},
        {"max_deferred_age_frames", stats.max_deferred_age_frames},
        {"drain_rate_per_s", drain_rate_per_s},
        {"backlog_bounded", backlog_bounded},
        {"final_queue_depth", stats.last_queue_depth}
    };

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "streaming-telemetry.json");
    output << std::setw(2) << artifact << '\n';
}

float LodBoundaryDistance(Luminumbra::Systems::SHIELD_WorldSystem* world_system) {
    if (world_system && !world_system->get_lod_levels().empty()) {
        return world_system->get_lod_levels().front().distance;
    }
    return 192.0f;
}

void ApplyLodBoundaryOscillationCamera(
    Luminumbra::world::GameSession* game_session,
    Luminumbra::Rendering::Camera* camera,
    double elapsed_seconds)
{
    if (!camera || !game_session) {
        return;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kDriftAmplitudeMeters = 6.0;
    constexpr double kDriftPeriodSeconds = 4.0;
    const float x = spawn.x + static_cast<float>(kDriftAmplitudeMeters * std::sin(elapsed_seconds * 2.0 * kPi / kDriftPeriodSeconds));
    const float z = spawn.z;
    const float terrain_height = world_system ? world_system->GetTerrainHeightAt(x, z) : spawn.y;
    camera->Position = Luminumbra::Vec3(x, terrain_height + 80.0f, z);

    const float boundary_distance = LodBoundaryDistance(world_system);
    const float focus_x = x + boundary_distance;
    const float focus_height = world_system ? world_system->GetTerrainHeightAt(focus_x, z) : terrain_height;
    AimCameraAt(camera, Luminumbra::Vec3(focus_x, focus_height, z));
}

void LodBoundaryTransitionRecorder::record_frame(Luminumbra::Systems::SHIELD_WorldSystem* world_system) {
    if (!world_system) {
        return;
    }

    ++m_frames_observed;
    for (const Luminumbra::Chunk* chunk : world_system->get_renderable_chunks()) {
        if (!chunk) {
            continue;
        }
        const int lod = chunk->current_lod.load(std::memory_order_acquire);
        const Luminumbra::ChunkID id = chunk->get_id();
        const auto [it, inserted] = m_last_lod.try_emplace(id, lod);
        if (!inserted && it->second != lod) {
            ++m_transitions[id];
            it->second = lod;
        }
    }
}

void WriteLodBoundaryOscillationAnalysis(
    const std::filesystem::path& artifact_dir,
    double duration_seconds,
    float boundary_distance,
    const LodBoundaryTransitionRecorder& recorder)
{
    // Baseline thresholds: current behavior plus margin. There is no LOD
    // hysteresis yet, so chunks dwelling on the boundary remesh on every
    // drift crossing (observed: 0.5 transitions/s per boundary chunk, 116
    // oscillating chunks and 75 total transitions/s over a 30s run). The
    // per-second rates keep the gate stable across run lengths; the gate
    // locks in no-worse-than-today so a future hysteresis fix can tighten
    // these numbers.
    constexpr std::uint64_t kOscillatingTransitionThreshold = 4;
    constexpr double kBaselineMaxTransitionsPerChunkPerSecond = 0.75;
    constexpr std::uint64_t kBaselineMaxOscillatingChunks = 240;
    constexpr double kBaselineMaxTotalTransitionsPerSecond = 115.0;

    std::uint64_t max_transitions_per_chunk = 0;
    std::uint64_t total_transitions = 0;
    std::uint64_t oscillating_chunk_count = 0;
    for (const auto& [id, transitions] : recorder.transitions()) {
        (void)id;
        max_transitions_per_chunk = std::max(max_transitions_per_chunk, transitions);
        total_transitions += transitions;
        if (transitions > kOscillatingTransitionThreshold) {
            ++oscillating_chunk_count;
        }
    }

    const double max_transitions_per_chunk_per_s = duration_seconds > 0.0
        ? static_cast<double>(max_transitions_per_chunk) / duration_seconds
        : 0.0;
    const double total_transitions_per_s = duration_seconds > 0.0
        ? static_cast<double>(total_transitions) / duration_seconds
        : 0.0;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed =
        gl_debug.errors == 0 &&
        max_transitions_per_chunk_per_s <= kBaselineMaxTransitionsPerChunkPerSecond &&
        oscillating_chunk_count <= kBaselineMaxOscillatingChunks &&
        total_transitions_per_s <= kBaselineMaxTotalTransitionsPerSecond;

    nlohmann::json artifact = {
        {"schema", "luminumbra.lod_boundary_oscillation.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"duration_seconds", duration_seconds},
        {"boundary_distance", boundary_distance},
        {"frames_observed", recorder.frames_observed()},
        {"chunks_observed", recorder.chunks_observed()},
        {"max_transitions_per_chunk", max_transitions_per_chunk},
        {"max_transitions_per_chunk_per_s", max_transitions_per_chunk_per_s},
        {"oscillating_transition_threshold", kOscillatingTransitionThreshold},
        {"oscillating_chunk_count", oscillating_chunk_count},
        {"total_transitions", total_transitions},
        {"total_transitions_per_s", total_transitions_per_s},
        {"known_oscillation_baseline", {
            {"max_transitions_per_chunk_per_s", kBaselineMaxTransitionsPerChunkPerSecond},
            {"max_oscillating_chunk_count", kBaselineMaxOscillatingChunks},
            {"max_total_transitions_per_s", kBaselineMaxTotalTransitionsPerSecond}
        }},
        {"gl_debug", {
            {"messages", gl_debug.messages},
            {"errors", gl_debug.errors},
            {"warnings", gl_debug.warnings},
            {"notifications", gl_debug.notifications}
        }}
    };

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "lod-boundary-oscillation.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
