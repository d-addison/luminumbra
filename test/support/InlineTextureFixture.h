#pragma once

#include <array>
#include <cstdint>

namespace Luminumbra::TestSupport::InlineTexture {

// A 2x2 RGBA8 source and its 1x1 mip bound each complete LTEX fixture to 37 bytes.
inline constexpr std::array<std::uint8_t, 16> kPixels = {
    255, 0, 0, 0, 255, 0, 0, 64, 255, 0, 0, 128, 255, 0, 0, 255};

struct CutoffCase {
    const char* argument;
    double cutoff;
    std::uint8_t mip_alpha;
};

// At 1x1, half coverage ties toward covered; quarter coverage stays uncovered.
inline constexpr std::array<CutoffCase, 5> kCutoffs = {
    {{"0", 0, 112}, {"0.25", .25, 64}, {"0.5", .5, 128}, {"0.75", .75, 191}, {"1", 1, 254}}};

constexpr std::array<std::uint8_t, 37> LtexBytes(std::uint8_t mip_alpha) {
    return {0x4c, 0x54, 0x45, 0x58, 1, 0,  2,   0, 2, 0,   0,   0, 2, 0,   0,   0, 4, 255,      0,
            0,    0,    255,  0,    0, 64, 255, 0, 0, 128, 255, 0, 0, 255, 255, 0, 0, mip_alpha};
}

} // namespace Luminumbra::TestSupport::InlineTexture
