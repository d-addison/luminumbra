#pragma once

#include "RenderFrameTypes.h"

namespace Luminumbra::Rendering {

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
            resolved.storm_intensity = 0.25f * intensity;
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
