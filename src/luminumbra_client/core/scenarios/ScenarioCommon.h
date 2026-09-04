#pragma once

#include "nlohmann/json_fwd.hpp"
#include <cstdint>

namespace Luminumbra::Client::ScenarioHarness {

struct WaterVisualCameraTarget;

constexpr double kSkyRoiHeightFraction = 0.55;
extern const std::uint64_t kMinNearBlackClusterPx;

double PixelLuminance(unsigned char r, unsigned char g, unsigned char b);
bool IsWaterLikePixel(unsigned char r, unsigned char g, unsigned char b);
bool IsBelowHorizonSkyPixel(unsigned char r, unsigned char g, unsigned char b);
nlohmann::json WaterVisualTargetToJson(const WaterVisualCameraTarget& target);

} // namespace Luminumbra::Client::ScenarioHarness
