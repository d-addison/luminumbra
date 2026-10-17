#pragma once

#include <cmath>

namespace luminumbra::core {
// Client-only geometry and open state, stored solely in user.developer.layouts.
struct DeveloperPanelLayout {
    float x = 0;
    float y = 0;
    float width = 480;
    float height = 520;
    bool open = false;
    bool operator==(const DeveloperPanelLayout&) const = default;
};
inline bool ValidDeveloperPanelLayout(const DeveloperPanelLayout& layout) {
    constexpr float limit = 1000000;
    return std::isfinite(layout.x) && std::isfinite(layout.y) && std::isfinite(layout.width) &&
           std::isfinite(layout.height) && std::abs(layout.x) <= limit &&
           std::abs(layout.y) <= limit && layout.width > 0 && layout.width <= limit &&
           layout.height > 0 && layout.height <= limit;
}
} // namespace luminumbra::core
