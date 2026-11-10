#pragma once

#include <algorithm>
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

// True when the segment a-b touches the [-1, 1] view square (Liang-Barsky clip).
inline bool LiveSegmentTouchesView(glm::vec2 a, glm::vec2 b) {
    const glm::vec2 d = b - a;
    const float p[4] = {-d.x, d.x, -d.y, d.y};
    const float q[4] = {a.x + 1.0f, 1.0f - a.x, a.y + 1.0f, 1.0f - a.y};
    float enter = 0.0f;
    float leave = 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) {
                return false;
            }
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0.0f) {
            enter = t > enter ? t : enter;
        } else {
            leave = t < leave ? t : leave;
        }
        if (enter > leave) {
            return false;
        }
    }
    return true;
}

// True when any drawn segment of the stroke touches the view; segments touching a
// pen-up point are not drawn.
inline bool LiveStrokeMayBeVisible(std::span<const glm::vec2> stroke) {
    for (std::size_t i = 1; i < stroke.size(); ++i) {
        const glm::vec2 a = stroke[i - 1];
        const glm::vec2 b = stroke[i];
        if (a == kLiveBoltPenUp || b == kLiveBoltPenUp) {
            continue;
        }
        if (LiveSegmentTouchesView(a, b)) {
            return true;
        }
    }
    return false;
}

// Indices, in ascending order, of the at most max_strikes strikes with the strongest
// pulse; ties keep the earlier strike. Only these strikes get bolt geometry, so the
// strongest flash always has its bolt.
inline std::vector<std::size_t> SelectLiveStrikeGeometry(std::span<const float> pulses,
                                                         std::size_t max_strikes) {
    std::vector<std::size_t> order(pulses.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    const std::size_t keep = max_strikes < order.size() ? max_strikes : order.size();
    std::partial_sort(order.begin(),
                      order.begin() + static_cast<std::ptrdiff_t>(keep),
                      order.end(),
                      [&](std::size_t a, std::size_t b) {
                          return pulses[a] != pulses[b] ? pulses[a] > pulses[b] : a < b;
                      });
    order.resize(keep);
    std::sort(order.begin(), order.end());
    return order;
}

// Combines every same-frame strike into one bolt polyline of at most max_points.
// Strokes outside the view are skipped and strokes that do not fit whole are
// dropped. Every visible main channel is placed before any branch, so heavily
// branched earlier strikes cannot displace a later strike's channel. Branches are
// drawn for strikes whose main channel was placed or lies outside the view, so a
// fork reaching into the view from an offscreen channel still shows; a visible
// channel that did not fit takes its branches with it. Every strike contributes to
// the flash: the strongest supplies the pulse and strike point, and ties keep the
// earlier strike.
inline ComposedLiveStrikes ComposeLiveStrikes(std::span<const ProjectedLiveStrike> strikes,
                                              std::size_t max_points) {
    ComposedLiveStrikes out;
    enum class Placement {
        Placed,
        Offscreen,
        NoRoom
    };
    const auto append = [&](std::span<const glm::vec2> stroke) {
        if (!LiveStrokeMayBeVisible(stroke)) {
            return Placement::Offscreen;
        }
        const std::size_t separator = out.points.empty() ? 0 : 1;
        if (stroke.size() + separator > max_points - out.points.size()) {
            return Placement::NoRoom;
        }
        if (separator != 0) {
            out.points.push_back(kLiveBoltPenUp);
        }
        out.points.insert(out.points.end(), stroke.begin(), stroke.end());
        return Placement::Placed;
    };
    std::vector<Placement> channels(strikes.size(), Placement::NoRoom);
    for (std::size_t i = 0; i < strikes.size(); ++i) {
        channels[i] = append(strikes[i].main_channel);
    }
    for (std::size_t i = 0; i < strikes.size(); ++i) {
        if (channels[i] == Placement::NoRoom) {
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
