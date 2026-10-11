#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Luminumbra::Client::ScenarioHarness {

// Versioned diagnostic workload. The old driver requested 60/96 and density 1,
// but the normal updater replaced those values with 48/92 and 1.6 before draw.
// Preserve that effective workload while making its owner and camera explicit.
struct FoliageVisualProfile {
    static constexpr const char* id = "luminumbra.foliage_control.v2";
    static constexpr float fade_start_m = 48.0f;
    static constexpr float fade_end_m = 92.0f;
    static constexpr float density_scale = 1.6f;
    static constexpr float eye_height_m = 2.4f;
    static constexpr float yaw_degrees = 35.0f;
    static constexpr float pitch_degrees = -18.0f;
    static constexpr float fov_degrees = 60.0f;
    static constexpr float time_of_day = 0.04f;
    static constexpr float windy_input = 6.0f; // Wind-field units, not displacement.
    static constexpr std::size_t minimum_instances = 100000;
    static constexpr double gpu_budget_ms = 0.6;
};

inline bool FoliageScenarioOwnsFrame(bool foliage_scenario, bool play_paths) {
    return foliage_scenario && !play_paths;
}

struct FoliagePhaseEvidence {
    bool sampled = false;
    bool from_gpu_readback = false;
    std::uint32_t phase = 0; // 1 calm, 2 windy
    std::uint64_t capture_frame = 0;
    std::uint64_t simulation_tick = 0;
    std::uint64_t build_generation = 0;
    std::uint64_t instance_generation = 0;
    std::uint64_t build_frame = 0;
    std::uint64_t available_frame = 0;
    std::uint64_t instance_hash = 0; // One snapshot, never an independent rebuild proof.
    double shader_time_seconds = 0.0;
    std::array<float, 3> camera{};
    float terrain_height_m = 0.0f;
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;
    float fov_degrees = 0.0f;
    float time_of_day = 0.0f;
    float fade_start_m = 0.0f;
    float fade_end_m = 0.0f;
    float density_scale = 0.0f;
    std::array<float, 2> wind_input{};
    double maximum_instance_wind = 0.0;
    int width = 0;
    int height = 0;
    std::string screenshot;
};

// GL-free reconstruction of Camera.h's finite RH reversed-Z projection. This
// joins the actual captured matrix to the reported camera, rather than accepting
// a self-consistent matrix/vertex pair from a different view.
inline std::array<double, 16> FoliageExpectedViewProjection(const FoliagePhaseEvidence& sample) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double yaw = sample.yaw_degrees * radians, pitch = sample.pitch_degrees * radians;
    const std::array<double, 3> front{
        std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)};
    const std::array<double, 3> right{-std::sin(yaw), 0, std::cos(yaw)};
    const std::array<double, 3> up{
        -std::cos(yaw) * std::sin(pitch), std::cos(pitch), -std::sin(yaw) * std::sin(pitch)};
    const auto dot = [&](const auto& vector) {
        return vector[0] * sample.camera[0] + vector[1] * sample.camera[1] +
               vector[2] * sample.camera[2];
    };
    const double view[4][4] = {{right[0], right[1], right[2], -dot(right)},
                               {up[0], up[1], up[2], -dot(up)},
                               {-front[0], -front[1], -front[2], dot(front)},
                               {0, 0, 0, 1}};
    const double scale = 1 / std::tan(sample.fov_degrees * radians / 2);
    constexpr double near_plane = .1, far_plane = 3200;
    const double projection[4][4] = {{scale / (3840.0 / 1600.0), 0, 0, 0},
                                     {0, scale, 0, 0},
                                     {0,
                                      0,
                                      near_plane / (far_plane - near_plane),
                                      far_plane * near_plane / (far_plane - near_plane)},
                                     {0, 0, -1, 0}};
    std::array<double, 16> result{};
    for (std::size_t col = 0; col < 4; ++col)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                result[col * 4 + row] += projection[row][k] * view[k][col];
    return result;
}

inline bool FoliageInstanceMatches(std::uint64_t build_generation,
                                   std::uint64_t instance_generation,
                                   std::uint64_t build_frame,
                                   std::uint64_t available_frame,
                                   std::uint64_t capture_frame) {
    return build_generation != 0 && build_generation == instance_generation && build_frame != 0 &&
           build_frame <= available_frame && available_frame <= capture_frame;
}

inline bool FoliagePhaseMatchesProfile(const FoliagePhaseEvidence& sample,
                                       std::uint32_t phase,
                                       float density_multiplier) {
    const auto near = [](float a, float b) {
        return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 0.0001f;
    };
    const float expected_wind = phase == 2 ? FoliageVisualProfile::windy_input : 0.0f;
    return sample.sampled && sample.phase == phase && (phase == 1 || phase == 2) &&
           FoliageInstanceMatches(sample.build_generation,
                                  sample.instance_generation,
                                  sample.build_frame,
                                  sample.available_frame,
                                  sample.capture_frame) &&
           std::isfinite(sample.camera[0]) && std::isfinite(sample.camera[2]) &&
           near(sample.camera[1], sample.terrain_height_m + FoliageVisualProfile::eye_height_m) &&
           near(sample.yaw_degrees, FoliageVisualProfile::yaw_degrees) &&
           near(sample.pitch_degrees, FoliageVisualProfile::pitch_degrees) &&
           near(sample.fov_degrees, FoliageVisualProfile::fov_degrees) &&
           near(sample.time_of_day, FoliageVisualProfile::time_of_day) &&
           near(sample.fade_start_m, FoliageVisualProfile::fade_start_m) &&
           near(sample.fade_end_m, FoliageVisualProfile::fade_end_m) &&
           near(sample.density_scale, FoliageVisualProfile::density_scale * density_multiplier) &&
           near(sample.wind_input[0], expected_wind) && near(sample.wind_input[1], 0.0f) &&
           std::isfinite(sample.maximum_instance_wind) &&
           std::abs(sample.maximum_instance_wind - expected_wind) <= 0.0001 &&
           std::isfinite(sample.shader_time_seconds) && sample.shader_time_seconds >= 0.0 &&
           sample.width > 0 && sample.height > 0 && !sample.screenshot.empty();
}

inline bool FoliageControlsMatch(const FoliagePhaseEvidence& calm,
                                 const FoliagePhaseEvidence& windy,
                                 float density_multiplier) {
    return FoliagePhaseMatchesProfile(calm, 1, density_multiplier) &&
           FoliagePhaseMatchesProfile(windy, 2, density_multiplier) &&
           calm.capture_frame < windy.build_frame &&
           calm.build_generation < windy.build_generation && calm.camera == windy.camera &&
           calm.width == windy.width && calm.height == windy.height;
}

inline bool FoliageCoveragePasses(std::size_t within_ring,
                                  double calibrated_count_ratio,
                                  double biome_density,
                                  double band) {
    return within_ring >= FoliageVisualProfile::minimum_instances &&
           std::isfinite(calibrated_count_ratio) && std::isfinite(biome_density) &&
           std::isfinite(band) && band >= 0.0 &&
           std::abs(calibrated_count_ratio - biome_density) <= band;
}

} // namespace Luminumbra::Client::ScenarioHarness
