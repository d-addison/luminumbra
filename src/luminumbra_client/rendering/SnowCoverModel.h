#pragma once

#include <algorithm>

// the  snow-cover model. A single [0,1] ground
// cover scalar accumulated from the live Snow weather intensity and melted by sun
// elevation (plus a slow ambient thaw), advanced with render dt — never sim state,
// never world_hash. The lighting pass blends up-facing terrain albedo toward snow
// white (+ roughness up) by this cover; 0.0 (the default) is byte-identical.
// Controlled by render.snow_cover, which is enabled in the shipped config.
namespace Luminumbra::Rendering::SnowCover {

// Full cover after ~120 s of heavy snowfall; bare again after ~240 s of full sun.
inline constexpr float kAccumulatePerSecond = 1.0f / 120.0f;
inline constexpr float kSunMeltPerSecond = 1.0f / 240.0f;
inline constexpr float kAmbientThawPerSecond = 1.0f / 3600.0f; // slow off-sun thaw

struct State {
    float cover01 = 0.0f;
};

// snow_intensity01: the Snow-category precipitation at the camera [0,1].
// sun_up01: max(sun up-factor, 0) — melts only while the sun is actually up.
inline void Advance(State& s, float snow_intensity01, float sun_up01, float dt_seconds) {
    const float snow = std::clamp(snow_intensity01, 0.0f, 1.0f);
    const float sun = std::clamp(sun_up01, 0.0f, 1.0f);
    const float dt = std::max(dt_seconds, 0.0f);
    s.cover01 += snow * kAccumulatePerSecond * dt;
    s.cover01 -= (sun * kSunMeltPerSecond + kAmbientThawPerSecond) * dt;
    s.cover01 = std::clamp(s.cover01, 0.0f, 1.0f);
}

} // namespace Luminumbra::Rendering::SnowCover
