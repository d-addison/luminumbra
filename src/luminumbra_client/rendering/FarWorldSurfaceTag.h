#pragma once

namespace Luminumbra::Rendering {

// Mirrors g_buffer.frag / lighting_pass.frag in the RG16F material AO lane.
// Input AO is the existing nonnegative surface value (normally [0, 1]). The
// offset keeps tagged AO=0 distinct from both ordinary zero and negative zero.
constexpr float EncodeFarWorldSurfaceAO(float ao, bool far_surface) noexcept {
    return far_surface ? -(1.0f + ao) : ao;
}

constexpr bool IsFarWorldSurface(float value) noexcept {
    return value < 0.0f;
}

constexpr float DecodeFarWorldSurfaceAO(float value) noexcept {
    return IsFarWorldSurface(value) ? -value - 1.0f : value;
}

} // namespace Luminumbra::Rendering
