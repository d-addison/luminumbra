#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

namespace Luminumbra::Rendering {

// Marks a break between strokes and a point behind the camera.
// Uploaded real points are clipped to [-1, 1], outside this marker's range.
inline constexpr glm::vec2 kLiveBoltPenUp{-3.0f, -3.0f};

// Keep the exact marker predicate in sync with lightning_overlay.frag.
inline bool IsLiveBoltPenUp(glm::vec2 point) {
    return point == kLiveBoltPenUp;
}

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

// Clips a-b to the [-1, 1] view square (Liang-Barsky); refusal leaves it unchanged.
inline bool ClipLiveSegmentToView(glm::vec2& a, glm::vec2& b) {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
        return false;
    }
    // Double intermediates keep subtraction safe for all finite projected floats.
    const double dx = static_cast<double>(b.x) - a.x;
    const double dy = static_cast<double>(b.y) - a.y;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x + 1.0, 1.0 - a.x, a.y + 1.0, 1.0 - a.y};
    double enter = 0.0;
    double leave = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) {
                return false;
            }
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0.0f) {
            enter = t > enter ? t : enter;
        } else {
            leave = t < leave ? t : leave;
        }
        if (enter > leave) {
            return false;
        }
    }
    const glm::vec2 origin = a;
    a = {static_cast<float>(std::clamp(origin.x + enter * dx, -1.0, 1.0)),
         static_cast<float>(std::clamp(origin.y + enter * dy, -1.0, 1.0))};
    b = {static_cast<float>(std::clamp(origin.x + leave * dx, -1.0, 1.0)),
         static_cast<float>(std::clamp(origin.y + leave * dy, -1.0, 1.0))};
    return true;
}

// True when the segment a-b touches the [-1, 1] view square.
inline bool LiveSegmentTouchesView(glm::vec2 a, glm::vec2 b) {
    return ClipLiveSegmentToView(a, b);
}

// True when any drawn segment of the stroke touches the view; segments touching a
// pen-up point are not drawn.
inline bool LiveStrokeMayBeVisible(std::span<const glm::vec2> stroke) {
    for (std::size_t i = 1; i < stroke.size(); ++i) {
        const glm::vec2 a = stroke[i - 1];
        const glm::vec2 b = stroke[i];
        if (IsLiveBoltPenUp(a) || IsLiveBoltPenUp(b)) {
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
// Segments are clipped to the view before upload, with pen-ups between disjoint
// pieces. Strokes outside the view are skipped and strokes that do not fit whole are
// dropped. Strikes are placed strongest pulse first, ties keeping the earlier
// strike, so weaker strikes cannot crowd out a stronger strike's bolt. Every
// visible main channel is placed before any branch, so heavily branched strikes
// cannot displace another strike's channel. Branches are
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
        const std::size_t start = out.points.size();
        bool connected = false;
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            glm::vec2 a = stroke[i - 1];
            glm::vec2 b = stroke[i];
            if (IsLiveBoltPenUp(a) || IsLiveBoltPenUp(b) || !ClipLiveSegmentToView(a, b)) {
                connected = false;
                continue;
            }
            const bool new_piece =
                out.points.size() == start || !connected || out.points.back() != a;
            const std::size_t needed = new_piece ? (out.points.empty() ? 2u : 3u) : 1u;
            // The output holds at most max_points, including every pen-up marker.
            if (needed > max_points - out.points.size()) {
                out.points.resize(start);
                return Placement::NoRoom;
            }
            if (new_piece) {
                if (!out.points.empty()) {
                    out.points.push_back(kLiveBoltPenUp);
                }
                out.points.push_back(a);
            }
            out.points.push_back(b);
            connected = true;
        }
        return out.points.size() == start ? Placement::Offscreen : Placement::Placed;
    };
    std::vector<std::size_t> order(strikes.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return strikes[a].pulse > strikes[b].pulse;
    });
    std::vector<Placement> channels(strikes.size(), Placement::NoRoom);
    for (const std::size_t i : order) {
        channels[i] = append(strikes[i].main_channel);
    }
    for (const std::size_t i : order) {
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
