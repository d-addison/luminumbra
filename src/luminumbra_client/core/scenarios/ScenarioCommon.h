#pragma once

#include "../../rendering/RenderFrameTypes.h"
#include "nlohmann/json_fwd.hpp"
#include <cstdint>
#include <filesystem>

namespace Luminumbra::Rendering {
class RenderPipeline;
}

namespace Luminumbra::Client::ScenarioHarness {

struct WaterVisualCameraTarget;

constexpr double kSkyRoiHeightFraction = 0.55;
extern const std::uint64_t kMinNearBlackClusterPx;

double PixelLuminance(unsigned char r, unsigned char g, unsigned char b);
bool IsWaterLikePixel(unsigned char r, unsigned char g, unsigned char b);
nlohmann::json WaterVisualTargetToJson(const WaterVisualCameraTarget& target);

// Capture-only inputs: live simulation weather must not influence comparisons.
// Keep the presentation phase identical across clear/rain and all six solar phases.
inline constexpr std::uint64_t kComparisonPresentationTick = 120;
inline constexpr std::uint32_t kComparisonPresentationRate = 30;

struct ComparisonAtmosphere {
    Luminumbra::Rendering::WeatherRenderState weather;
    Luminumbra::Rendering::CloudRenderState clouds;
};

inline ComparisonAtmosphere BuildComparisonAtmosphere(bool rain) {
    ComparisonAtmosphere state;
    state.weather.driven = true;
    state.weather.rain_intensity = rain ? 1.0f : 0.0f;
    state.weather.fog_density = rain ? 0.1f : 0.0f;
    state.weather.storm_intensity = rain ? 0.4f : 0.0f;
    state.weather.wetness = rain ? 1.0f : 0.0f;
    // Zero wind leaves vertical precipitation and a common cloud phase.
    state.weather.wind_direction = glm::vec3(1.0f, 0.0f, 0.0f);
    state.weather.wind_strength = 0.0f;
    // Enabled even at zero coverage so advance_cloud_phase replaces any stale
    // accumulated scroll offset using the pinned presentation clock.
    state.clouds.enabled = true;
    state.clouds.shadow_enabled = true;
    state.clouds.coverage_amount = rain ? 0.8f : 0.0f;
    state.clouds.shadow_strength = rain ? 0.45f : 0.0f;
    return state;
}

void PinComparisonAtmosphere(Luminumbra::Rendering::RenderPipeline& pipeline,
                             const std::filesystem::path& particle_data_root,
                             const glm::vec3& camera_position,
                             bool rain);
nlohmann::json ResolvedAtmosphereToJson(const Luminumbra::Rendering::RenderPipeline& pipeline);

} // namespace Luminumbra::Client::ScenarioHarness
