#pragma once

#include "RenderFrameTypes.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

namespace Luminumbra::Rendering {

// One strike consumed this frame, projected to normalized device coordinates.
struct ProjectedLiveStrike {
    std::vector<ProjectedBoltPoint> main_channel;
    std::vector<std::vector<ProjectedBoltPoint>> branches;
    glm::vec2 strike_ndc = kLiveBoltPenUp;
    float pulse = 0.0f;
};

struct ComposedLiveStrikes {
    std::vector<ProjectedBoltPoint> points;
    LiveStrikeFlashList flashes;
    float pulse = 0.0f;
};

// Match coreHalf and glowRadius in lightning_overlay.frag. Their sum
// conservatively covers both; x is divided by the shader's u_aspect scale.
inline glm::vec2 LiveBoltViewMargin(float width, float glow, float aspect) {
    const double safe_glow = std::max(0.0, static_cast<double>(glow));
    const double core =
        std::clamp(std::min(std::max(0.0, static_cast<double>(width)) * 0.18, safe_glow * 0.10),
                   0.0010,
                   0.0024);
    const double radius = core + std::min(safe_glow, 0.030);
    const double x = aspect > 0.0f ? radius / aspect : 1.0;
    return {static_cast<float>(std::min(x, 1.0)), static_cast<float>(std::min(radius, 1.0))};
}

// Clips a-b to the expanded view (Liang-Barsky); refusal leaves it unchanged.
// Clamp each margin to [0, 1], so even extreme radii/aspects cannot reach (-3, -3).
inline bool ClipLiveSegmentToView(glm::vec2& a, glm::vec2& b, glm::vec2 margin = {}) {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
        return false;
    }
    // Double intermediates keep subtraction safe for all finite projected floats.
    const double bound_x = 1.0 + std::clamp(margin.x > 0.0f ? margin.x : 0.0f, 0.0f, 1.0f);
    const double bound_y = 1.0 + std::clamp(margin.y > 0.0f ? margin.y : 0.0f, 0.0f, 1.0f);
    const double dx = static_cast<double>(b.x) - a.x;
    const double dy = static_cast<double>(b.y) - a.y;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x + bound_x, bound_x - a.x, a.y + bound_y, bound_y - a.y};
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
    a = {static_cast<float>(std::clamp(origin.x + enter * dx, -bound_x, bound_x)),
         static_cast<float>(std::clamp(origin.y + enter * dy, -bound_y, bound_y))};
    b = {static_cast<float>(std::clamp(origin.x + leave * dx, -bound_x, bound_x)),
         static_cast<float>(std::clamp(origin.y + leave * dy, -bound_y, bound_y))};
    return true;
}

// True when the segment a-b touches the expanded view square.
inline bool LiveSegmentTouchesView(glm::vec2 a, glm::vec2 b, glm::vec2 margin = {}) {
    return ClipLiveSegmentToView(a, b, margin);
}

// True when any drawn segment of the stroke touches the view; segments touching a
// pen-up point are not drawn.
inline bool LiveStrokeMayBeVisible(std::span<const ProjectedBoltPoint> stroke,
                                   glm::vec2 margin = {}) {
    for (std::size_t i = 1; i < stroke.size(); ++i) {
        const glm::vec2 a = stroke[i - 1];
        const glm::vec2 b = stroke[i];
        if (IsLiveBoltPenUp(a) || IsLiveBoltPenUp(b)) {
            continue;
        }
        if (LiveSegmentTouchesView(a, b, margin)) {
            return true;
        }
    }
    return false;
}

// Indices, in ascending order, of the at most max_strikes strikes with the strongest
// pulse among visible candidates; ties keep the earlier strike. The visibility
// predicate runs before the cap, once per candidate (bounded by the live window).
template<typename Visible>
inline std::vector<std::size_t> SelectLiveStrikeGeometry(std::span<const float> pulses,
                                                         std::size_t max_strikes,
                                                         const Visible& visible) {
    std::vector<std::size_t> order;
    if (max_strikes == 0) {
        return order;
    }
    order.reserve(pulses.size());
    for (std::size_t i = 0; i < pulses.size(); ++i) {
        if (visible(i)) {
            order.push_back(i);
        }
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

inline std::vector<std::size_t> SelectLiveStrikeGeometry(std::span<const float> pulses,
                                                         std::size_t max_strikes) {
    return SelectLiveStrikeGeometry(pulses, max_strikes, [](std::size_t) { return true; });
}

inline bool LiveStrikeMayBeVisible(const ProjectedLiveStrike& strike, glm::vec2 margin) {
    if (LiveStrokeMayBeVisible(strike.main_channel, margin)) {
        return true;
    }
    return std::any_of(strike.branches.begin(), strike.branches.end(), [&](const auto& branch) {
        return LiveStrokeMayBeVisible(branch, margin);
    });
}

// Flash candidates use at most eight indices, even when the live window is full.
// Visit each pulse once; insertion scans and shifts at most kMaxLiveStrikeFlashes.
// Return input order so merging with geometry preserves the stable tie key.
inline std::vector<std::size_t> SelectLiveStrikeFlashCandidates(std::span<const float> pulses) {
    std::vector<std::size_t> selected;
    selected.reserve(kMaxLiveStrikeFlashes);
    for (std::size_t i = 0; i < pulses.size(); ++i) {
        const auto position = std::find_if(selected.begin(), selected.end(), [&](std::size_t key) {
            return pulses[i] > pulses[key];
        });
        const auto offset = position - selected.begin();
        if (selected.size() == kMaxLiveStrikeFlashes) {
            if (position == selected.end()) {
                continue;
            }
            selected.pop_back();
        }
        selected.insert(selected.begin() + offset, i);
    }
    std::sort(selected.begin(), selected.end());
    return selected;
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
// the flash list up to kMaxLiveStrikeFlashes, strongest pulse first with the input
// index as the stable tie key. Flash selection is independent of geometry capacity.
inline ComposedLiveStrikes ComposeLiveStrikes(std::span<const ProjectedLiveStrike> strikes,
                                              std::size_t max_points,
                                              glm::vec2 margin = {}) {
    ComposedLiveStrikes out;
    enum class Placement {
        Placed,
        Offscreen,
        NoRoom
    };
    const auto clipped_point = [](const ProjectedBoltPoint& a,
                                  const ProjectedBoltPoint& b,
                                  glm::vec2 ndc,
                                  double endpoint) {
        const double dx = static_cast<double>(b.x) - a.x;
        const double dy = static_cast<double>(b.y) - a.y;
        const double t = std::abs(dx) >= std::abs(dy)
                             ? (dx == 0.0 ? endpoint : (static_cast<double>(ndc.x) - a.x) / dx)
                             : (static_cast<double>(ndc.y) - a.y) / dy;
        if (!std::isfinite(a.inverse_depth) || !std::isfinite(b.inverse_depth) ||
            a.inverse_depth <= 0.0f || b.inverse_depth <= 0.0f) {
            return ProjectedBoltPoint(ndc);
        }
        return ProjectedBoltPoint(ndc,
                                  static_cast<float>(std::lerp(static_cast<double>(a.inverse_depth),
                                                               static_cast<double>(b.inverse_depth),
                                                               std::clamp(t, 0.0, 1.0))));
    };
    const auto append = [&](std::span<const ProjectedBoltPoint> stroke) {
        const std::size_t start = out.points.size();
        bool connected = false;
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            glm::vec2 a = stroke[i - 1];
            glm::vec2 b = stroke[i];
            if (IsLiveBoltPenUp(a) || IsLiveBoltPenUp(b) || !ClipLiveSegmentToView(a, b, margin)) {
                connected = false;
                continue;
            }
            const auto clipped_a = clipped_point(stroke[i - 1], stroke[i], a, 0.0);
            const auto clipped_b = clipped_point(stroke[i - 1], stroke[i], b, 1.0);
            const bool new_piece =
                out.points.size() == start || !connected || out.points.back() != clipped_a;
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
                out.points.push_back(clipped_a);
            }
            out.points.push_back(clipped_b);
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
        for (const std::vector<ProjectedBoltPoint>& branch : strikes[i].branches) {
            append(branch);
        }
    }
    out.flashes.count = std::min(order.size(), kMaxLiveStrikeFlashes);
    for (std::size_t i = 0; i < out.flashes.count; ++i) {
        const auto& strike = strikes[order[i]];
        const bool onscreen = !IsLiveBoltPenUp(strike.strike_ndc) &&
                              std::abs(strike.strike_ndc.x) <= 1.0f &&
                              std::abs(strike.strike_ndc.y) <= 1.0f;
        out.flashes.entries[i] = {strike.strike_ndc, strike.pulse, onscreen};
    }
    if (out.flashes.count > 0) {
        out.pulse = out.flashes.entries[0].pulse;
    }
    return out;
}

} // namespace Luminumbra::Rendering
