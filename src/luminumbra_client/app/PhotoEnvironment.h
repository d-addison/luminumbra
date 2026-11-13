#pragma once

#include "luminumbra_client/rendering/RenderFrameTypes.h"

#include <array>

namespace Luminumbra::Client::App {

struct PhotoEnvironmentValues {
    float time_of_day = 0.5f;
    Luminumbra::Rendering::WeatherType weather = Luminumbra::Rendering::WeatherType::None;
    float intensity = 0.0f;
};

// Client presentation state only. Inactive channels leave the live push untouched.
struct PhotoEnvironmentOverride {
    bool time_active = false;
    float time_of_day = 0.5f;
    bool weather_active = false;
    Luminumbra::Rendering::WeatherType weather = Luminumbra::Rendering::WeatherType::None;
    float intensity = 0.0f;
    int weather_index = 0; // Six UI choices: live, clear, fog, rain, snow, storm.
};

constexpr PhotoEnvironmentOverride CyclePhotoWeather(const PhotoEnvironmentOverride& photo,
                                                     int cycle) {
    auto next = photo;
    // Reduce before adding so even a full int-range nudge cannot overflow.
    next.weather_index = ((photo.weather_index % 6 + cycle % 6) % 6 + 6) % 6;
    constexpr std::array presets = {Luminumbra::Rendering::WeatherType::None,
                                    Luminumbra::Rendering::WeatherType::None,
                                    Luminumbra::Rendering::WeatherType::Fog,
                                    Luminumbra::Rendering::WeatherType::Rain,
                                    Luminumbra::Rendering::WeatherType::Snow,
                                    Luminumbra::Rendering::WeatherType::Storm};
    next.weather_active = next.weather_index != 0;
    next.weather = presets[static_cast<std::size_t>(next.weather_index)];
    next.intensity = next.weather_index <= 1 ? 0.0f : 0.7f;
    return next;
}

// Capture pins always retain authority, including when photo state is still engaged.
constexpr PhotoEnvironmentOverride SelectPhotoEnvironment(const PhotoEnvironmentValues& live,
                                                          const PhotoEnvironmentOverride& photo,
                                                          bool photo_mode_active,
                                                          bool capture_pinned) {
    auto selected = photo;
    if (!photo_mode_active || capture_pinned) {
        selected.time_active = false;
        selected.weather_active = false;
    }
    if (!selected.time_active) {
        selected.time_of_day = live.time_of_day;
    }
    if (!selected.weather_active) {
        selected.weather = live.weather;
        selected.intensity = live.intensity;
    }
    return selected;
}

} // namespace Luminumbra::Client::App
