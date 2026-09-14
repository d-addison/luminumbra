#pragma once

#include <cstdint>
#include <limits>

namespace FarVolumeFoundationFixture {
// Deliberately independent analytic authority. It is shared by the pinned
// original exporter and the regression reader; neither supplies generation,
// span discovery, sparse ordering, or checksum behavior to the other.
inline float Height(float x, float z) {
    return 16.0f + x / 128.0f + z / 256.0f;
}
inline float Density(float y, float height, unsigned field) {
    switch (field) {
        case 1:
            return height - y + .125f;
        case 2:
            return 0.0f;
        case 3:
            return -std::numeric_limits<float>::denorm_min();
        default:
            return y - height + .001f;
    }
}
inline std::uint8_t
Material(std::int64_t x, std::int64_t y, std::int64_t z, std::uint32_t spacing) {
    return static_cast<std::uint8_t>(x / spacing * 17 + y / spacing * 31 + z / spacing * 7);
}
} // namespace FarVolumeFoundationFixture
