#pragma once

// --timelapse-vista / --timelapse-pan / --timelapse-settle policy for the capture
// harness. Header-only and GL-free so the bounds and framing are unit-tested.

#include <glm/glm.hpp>

#include "StrictNumeric.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace Luminumbra::TimelapseVista {

inline constexpr int kMaxSettleFrames = 3600;
inline constexpr float kMaxPanDegreesPerFrame = 2.0f;

// Settle frames never drop below the harness default (streaming needs them) and
// never exceed one minute at 60 fps.
inline int ClampSettleFrames(int requested, int default_frames) {
    return std::clamp(requested, default_frames, std::max(default_frames, kMaxSettleFrames));
}

// Empty, malformed or non-finite input keeps the default (nullopt).
inline std::optional<float> ParsePanDegrees(const std::string& text) {
    float parsed = 0.0f;
    if (!Core::ParseNumber(text, parsed)) {
        return std::nullopt;
    }
    return std::clamp(parsed, -kMaxPanDegreesPerFrame, kMaxPanDegreesPerFrame);
}

struct Framing {
    glm::vec3 position{};
    glm::vec3 target{};
};

// A low camera near the horizon looking far toward the sunset side (the sun
// sets toward -x), aimed a few degrees below the horizon so ground, sky and sun
// path share the frame.
inline Framing VistaFraming(const glm::vec3& spawn) {
    return {glm::vec3(spawn.x + 6.0f, spawn.y + 10.0f, spawn.z + 8.0f),
            glm::vec3(spawn.x - 120.0f, spawn.y + 2.0f, spawn.z - 40.0f)};
}

// Yaw added on top of the framing's base yaw for captured frame `captured`.
inline float PanYawOffset(float pan_degrees_per_frame, int captured) {
    return pan_degrees_per_frame * static_cast<float>(captured);
}

// Vista owns the camera: scenario framings (rain, drain) still run their setup,
// but only move the camera when vista is off.
inline bool ScenarioMayFrameCamera(bool vista) {
    return !vista;
}

} // namespace Luminumbra::TimelapseVista
