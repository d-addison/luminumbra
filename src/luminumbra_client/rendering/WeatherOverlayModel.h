#pragma once

#include "RenderFrameTypes.h"

#include <algorithm>

namespace Luminumbra::Rendering {

struct WeatherClassification {
    WeatherType type = WeatherType::None;
    float intensity = 0.0f;
};

// Classify the real weather before adding any visual rain overcast.
inline WeatherClassification ClassifyWeatherState(const WeatherRenderState& state) {
    if (state.storm_intensity > 0.05f && state.rain_intensity > 0.05f) {
        return {.type = WeatherType::Storm,
                .intensity =
                    std::max({state.rain_intensity, state.snow_intensity, state.storm_intensity})};
    }
    if (state.snow_intensity > 0.02f) {
        return {.type = WeatherType::Snow, .intensity = state.snow_intensity};
    }
    if (state.rain_intensity > 0.02f) {
        return {.type = WeatherType::Rain, .intensity = state.rain_intensity};
    }
    if (state.fog_density > 0.02f) {
        return {.type = WeatherType::Fog, .intensity = state.fog_density};
    }
    return {};
}

// Calm rain darkens both visual consumers without becoming a real storm.
inline float ResolveWeatherOvercast(const WeatherRenderState& state) {
    return std::max(state.storm_intensity, 0.25f * state.rain_intensity);
}

// Shared value-only inputs for the sky storm floor and the weather overlay's
// uniforms. Driven state has already been clamped by set_weather_state; direct
// controls use the legacy type/intensity mapping and never reuse stale fields.
inline WeatherRenderState
ResolveWeatherOverlayState(const WeatherRenderState& state, WeatherType type, float intensity) {
    if (state.driven) {
        return state;
    }
    WeatherRenderState resolved;
    switch (type) {
        case WeatherType::Rain:
            resolved.rain_intensity = intensity;
            resolved.fog_density = 0.1f * intensity;
            break;
        case WeatherType::Snow:
            resolved.snow_intensity = intensity;
            resolved.fog_density = 0.05f * intensity;
            break;
        case WeatherType::Fog:
            resolved.fog_density = intensity;
            break;
        case WeatherType::Storm:
            resolved.rain_intensity = intensity;
            resolved.storm_intensity = intensity;
            break;
        case WeatherType::None:
        default:
            break;
    }
    resolved.wetness = resolved.rain_intensity;
    resolved.wind_strength = 0.3f * intensity;
    return resolved;
}

} // namespace Luminumbra::Rendering
