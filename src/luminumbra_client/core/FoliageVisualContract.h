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
