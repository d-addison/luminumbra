#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <vector>

namespace Luminumbra::Rendering {

// Capture-only evidence, in OpenGL's bottom-up pixel order. The central half
// of each axis is the intended target region; sky outside it cannot satisfy it.
struct WaterVisibilityMask {
    static constexpr std::size_t kMaxPixels = 4096u * 4096u;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;

    static bool valid_size(int w, int h) {
        return w >= 4 && h >= 4 && w <= 4096 && h <= 4096 &&
               static_cast<std::size_t>(w) <= kMaxPixels / static_cast<std::size_t>(h);
    }

    bool valid() const {
        return valid_size(width, height) &&
               pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    static WaterVisibilityMask FromRgba(int w, int h, const std::vector<unsigned char>& rgba) {
        if (!valid_size(w, h)) {
            return {};
        }
        const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        if (rgba.size() != count * 4u) {
            return {};
        }
        WaterVisibilityMask result{
            .width = w, .height = h, .pixels = std::vector<unsigned char>(count)};
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char alpha = rgba[i * 4u + 3u];
            result.pixels[i] = alpha != 0 ? 255 : 0;
        }
        return result;
    }

    std::uint64_t visible_pixels_in_target() const {
        if (!valid()) {
            return 0;
        }
        std::uint64_t count = 0;
        for (int y = height / 4; y < height - height / 4; ++y) {
            for (int x = width / 4; x < width - width / 4; ++x) {
                count += pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x)] != 0;
            }
        }
        return count;
    }
};

struct WaterVisibilityTarget {
    bool found = false;
    glm::vec3 eye{0.0f};
    glm::vec3 focus{0.0f};
};

// Once per sweep: at most 169 candidates, each with 25 patch samples and
// 9 sight lines of 33 samples. No world scan or type-erased per-sample call.
template<typename HeightAt>
WaterVisibilityTarget
FindWaterVisibilityTarget(const glm::vec3& anchor, float sea_level, const HeightAt& height_at) {
    WaterVisibilityTarget result;
    if (!std::isfinite(anchor.x) || !std::isfinite(anchor.z) || !std::isfinite(sea_level)) {
        return result;
    }
    float nearest = std::numeric_limits<float>::max();
    for (int dz = -96; dz <= 96; dz += 16) {
        for (int dx = -96; dx <= 96; dx += 16) {
            const float distance = static_cast<float>(dx * dx + dz * dz);
            if (distance >= nearest) {
                continue;
            }
            const glm::vec3 focus(anchor.x + static_cast<float>(dx),
                                  sea_level + 0.1f,
                                  anchor.z + static_cast<float>(dz));
            const glm::vec3 eye = focus + glm::vec3(0.0f, 12.0f, 16.0f);
            bool clear = true;
            for (int z = -16; z <= 16 && clear; z += 8) {
                for (int x = -16; x <= 16 && clear; x += 8) {
                    const float h =
                        height_at(focus.x + static_cast<float>(x), focus.z + static_cast<float>(z));
                    clear = std::isfinite(h) && h < sea_level - 0.5f;
                }
            }
            for (int z = -8; z <= 8 && clear; z += 8) {
                for (int x = -8; x <= 8 && clear; x += 8) {
                    const glm::vec3 end = focus + glm::vec3(x, 0, z);
                    for (int step = 0; step <= 32 && clear; ++step) {
                        const glm::vec3 point =
                            glm::mix(eye, end, static_cast<float>(step) / 32.0f);
                        const float h = height_at(point.x, point.z);
                        clear = std::isfinite(h) && h < point.y - 0.5f;
                    }
                }
            }
            if (clear) {
                nearest = distance;
                result = {.found = true, .eye = eye, .focus = focus};
            }
        }
    }
    return result;
}

} // namespace Luminumbra::Rendering
