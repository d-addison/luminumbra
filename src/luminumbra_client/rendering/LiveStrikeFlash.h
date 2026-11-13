#pragma once

#include <array>
#include <cstddef>

#include <glm/vec2.hpp>

namespace Luminumbra::Rendering {

// Marks a break between strokes and a point behind the camera.
// Expanded clip bounds are clamped to [-2, 2], strictly inside this marker.
inline constexpr glm::vec2 kLiveBoltPenUp{-3.0f, -3.0f};

// Keep the exact marker predicate in sync with lightning_overlay.frag.
inline bool IsLiveBoltPenUp(glm::vec2 point) {
    return point == kLiveBoltPenUp;
}

inline constexpr std::size_t kMaxLiveStrikeFlashes = 8;

struct LiveStrikeFlash {
    glm::vec2 strike_ndc = kLiveBoltPenUp;
    float pulse = 0.0f;
    bool onscreen = false;
};

struct LiveStrikeFlashList {
    // At most eight per-strike contributions, matching MAX_STRIKE_FLASHES in GLSL.
    std::array<LiveStrikeFlash, kMaxLiveStrikeFlashes> entries{};
    std::size_t count = 0;
};

} // namespace Luminumbra::Rendering
