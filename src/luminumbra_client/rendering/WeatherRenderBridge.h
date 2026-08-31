#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include "RenderFrameTypes.h"
#include "luminumbra_common/systems/WeatherSystem.h"

// Live weather bridge: the pure mapping from a
// replicated WeatherSystem sample to the render-side weather + cloud state PODs.
// This is the same sample->state shape the WeatherVisual scenario builds inline,
// WITHOUT the scenario's capture floors (max(precip,1), max(storm,0.4) — those
// exist to stabilize gate captures and stay scenario-local). It is one-way:
// reads sim state, writes render PODs, feeds nothing back, and never affects world_hash.
//
// Wired per frame from live play behind `render.live_weather`, enabled in the
// shipped config. Unit-tested as pure functions (WeatherRenderBridge CTests).
namespace Luminumbra::Rendering::WeatherBridge {

// Wind speed (m/s) that maps to full overlay wind strength — the scenario's
// verbatim scaling constant.
inline constexpr float kWindFullStrength = 13.0f;

inline WeatherRenderState BuildWeatherRenderState(const Systems::WeatherSample& s) {
    using Systems::WeatherCategory;
    WeatherRenderState w;
    w.driven = true;
    const float precip = std::clamp(s.precip_intensity, 0.0f, 1.0f);
    // Rain vs snow split on the category; Overcast/Fog can still drizzle (the
    // precip field says so), which reads as rain.
    w.rain_intensity = (s.category == WeatherCategory::Snow) ? 0.0f : precip;
    w.snow_intensity = (s.category == WeatherCategory::Snow) ? precip : 0.0f;
    // The scenario's fog constants: a thick veil under Fog, a light ambient haze
    // otherwise (the aerial pass owns true distance fog; this is the overlay's).
    w.fog_density = (s.category == WeatherCategory::Fog) ? 0.4f : 0.1f;
    w.storm_intensity = std::clamp(s.storm_intensity, 0.0f, 1.0f);
    w.wetness = precip;
    const float wlen = std::sqrt(s.wind.x * s.wind.x + s.wind.y * s.wind.y);
    if (wlen > 1e-4f) {
        w.wind_direction = glm::vec3(s.wind.x / wlen, 0.0f, s.wind.y / wlen);
        w.wind_strength = std::clamp(wlen / kWindFullStrength, 0.0f, 1.0f);
    }
    return w;
}

// Sky-cover fraction per category — the large-scale look of each weather state.
inline float CloudCoverageFor(Systems::WeatherCategory category, float storm_intensity) {
    using Systems::WeatherCategory;
    float base = 0.30f; // Clear: scattered fair-weather cover
    switch (category) {
        case WeatherCategory::Clear:
            base = 0.30f;
            break;
        case WeatherCategory::Overcast:
            base = 0.70f;
            break;
        case WeatherCategory::Rain:
            base = 0.80f;
            break;
        case WeatherCategory::Snow:
            base = 0.75f;
            break;
        case WeatherCategory::Fog:
            base = 0.60f;
            break;
    }
    // An active storm pushes toward full cover regardless of category.
    return std::clamp(base + 0.25f * std::clamp(storm_intensity, 0.0f, 1.0f), 0.0f, 1.0f);
}

// Wind-advected cloud scroll: direction * a tick-derived phase (deterministic
// drift; the dome and its cast shadow stay registered — the CloudRenderState
// contract). Metres of drift per tick at full wind.
inline constexpr float kCloudScrollMetresPerTick = 0.35f;

// sun_travel_dir defaults to overhead — the same POD default the WeatherVisual
// scenario ships (the passes register the live sun themselves where needed).
inline CloudRenderState
BuildCloudRenderState(const Systems::WeatherSample& s,
                      std::uint64_t simTick,
                      const glm::vec3& sun_travel_dir = glm::vec3(0.0f, -1.0f, 0.0f)) {
    CloudRenderState c;
    c.enabled = true;
    c.shadow_enabled = true;
    c.coverage_amount = CloudCoverageFor(s.category, s.storm_intensity);
    const float wlen = std::sqrt(s.wind.x * s.wind.x + s.wind.y * s.wind.y);
    const glm::vec2 wind_dir =
        (wlen > 1e-4f) ? glm::vec2(s.wind.x / wlen, s.wind.y / wlen) : glm::vec2(1.0f, 0.0f);
    const float wind01 = std::clamp(wlen / kWindFullStrength, 0.0f, 1.0f);
    const float phase = static_cast<float>(simTick % 4320000ull); // wraps ~40 h; float-safe
    c.scroll_offset = wind_dir * (phase * kCloudScrollMetresPerTick * (0.35f + 0.65f * wind01));
    // Denser cover casts stronger crawling shadows; clear skies barely any.
    c.shadow_strength = std::clamp((c.coverage_amount - 0.30f) * 0.9f, 0.0f, 0.6f);
    c.sun_travel_dir = sun_travel_dir;
    return c;
}

} // namespace Luminumbra::Rendering::WeatherBridge
