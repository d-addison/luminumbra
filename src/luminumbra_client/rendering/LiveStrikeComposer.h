#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

namespace Luminumbra::Rendering {

// Marks a break between strokes and a point behind the camera.
inline constexpr glm::vec2 kLiveBoltPenUp{-3.0f, -3.0f};

// One strike consumed this frame, projected to normalized device coordinates.
struct ProjectedLiveStrike {
    std::vector<glm::vec2> main_channel;
    std::vector<std::vector<glm::vec2>> branches;
    glm::vec2 strike_ndc{0.0f};
    float pulse = 0.0f;
};

struct ComposedLiveStrikes {
    std::vector<glm::vec2> points;
    glm::vec2 strike_ndc{0.0f};
    float pulse = 0.0f;
};

// True when a segment's bounding box overlaps the view; segments touching a
// pen-up point are not drawn.
inline bool LiveStrokeMayBeVisible(std::span<const glm::vec2> stroke) {
    for (std::size_t i = 1; i < stroke.size(); ++i) {
        const glm::vec2 a = stroke[i - 1];
        const glm::vec2 b = stroke[i];
        if (a == kLiveBoltPenUp || b == kLiveBoltPenUp) {
            continue;
        }
        const bool overlaps_x = (a.x < b.x ? b.x : a.x) >= -1.0f && (a.x < b.x ? a.x : b.x) <= 1.0f;
        const bool overlaps_y = (a.y < b.y ? b.y : a.y) >= -1.0f && (a.y < b.y ? a.y : b.y) <= 1.0f;
        if (overlaps_x && overlaps_y) {
            return true;
        }
    }
    return false;
}

// Combines every same-frame strike into one bolt polyline of at most max_points.
// Strokes outside the view are skipped and strokes that do not fit whole are
// dropped. Every visible main channel is placed before any branch, so heavily
// branched earlier strikes cannot displace a later strike's channel, and branches
// are drawn only for strikes whose main channel was placed. Every strike
// contributes to the flash: the strongest supplies the pulse and strike point, and
// ties keep the earlier strike.
inline ComposedLiveStrikes ComposeLiveStrikes(std::span<const ProjectedLiveStrike> strikes,
                                              std::size_t max_points) {
    ComposedLiveStrikes out;
    const auto append = [&](std::span<const glm::vec2> stroke) {
        if (!LiveStrokeMayBeVisible(stroke)) {
            return false;
        }
        const std::size_t separator = out.points.empty() ? 0 : 1;
        if (stroke.size() + separator > max_points - out.points.size()) {
            return false;
        }
        if (separator != 0) {
            out.points.push_back(kLiveBoltPenUp);
        }
        out.points.insert(out.points.end(), stroke.begin(), stroke.end());
        return true;
    };
    std::vector<bool> placed(strikes.size(), false);
    for (std::size_t i = 0; i < strikes.size(); ++i) {
        placed[i] = append(strikes[i].main_channel);
    }
    for (std::size_t i = 0; i < strikes.size(); ++i) {
        if (!placed[i]) {
            continue;
        }
        for (const std::vector<glm::vec2>& branch : strikes[i].branches) {
            append(branch);
        }
    }
    bool have_flash = false;
    for (const ProjectedLiveStrike& strike : strikes) {
        if (!have_flash || strike.pulse > out.pulse) {
            out.pulse = strike.pulse;
            out.strike_ndc = strike.strike_ndc;
            have_flash = true;
        }
    }
    return out;
}

} // namespace Luminumbra::Rendering
