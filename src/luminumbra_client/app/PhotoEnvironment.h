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

// Exposure can engage while capture pins suppress the time/weather channels.
// Keep its ownership separate so an inactive photo mode releases it exactly once.
class PhotoExposureOverride {
public:
    template<typename Pipeline>
    void Apply(Pipeline& pipeline, float multiplier) {
        pipeline.set_exposure_override(multiplier);
        applied_ = true;
    }

    template<typename Pipeline>
    void ReleaseIfInactive(Pipeline& pipeline, bool photo_mode_active) {
        if (photo_mode_active || !applied_) {
            return;
        }
        pipeline.set_exposure_override(-1.0f);
        applied_ = false;
    }

private:
    bool applied_ = false;
};

// Restore before the next frame's live writers, even when no weather writer runs.
// Retain the full driven state: type/intensity alone lose wetness and wind.
// One fixed-size snapshot; no allocation or per-frame scan.
class PhotoWeatherOverride {
public:
    template<typename Pipeline>
    void Apply(Pipeline& pipeline, Luminumbra::Rendering::WeatherType weather, float intensity) {
        if (!applied_) {
            weather_ = pipeline.get_weather_type();
            intensity_ = pipeline.get_weather_intensity();
            state_ = pipeline.get_weather_state();
            applied_ = true;
        }
        pipeline.set_weather(weather, intensity);
    }

    template<typename Pipeline>
    void Restore(Pipeline& pipeline) {
        if (!applied_) {
            return;
        }
        if (state_.driven) {
            pipeline.set_weather_state(state_);
        } else {
            pipeline.set_weather(weather_, intensity_);
        }
        applied_ = false;
    }

private:
    bool applied_ = false;
    Luminumbra::Rendering::WeatherType weather_ = Luminumbra::Rendering::WeatherType::None;
    float intensity_ = 0.0f;
    Luminumbra::Rendering::WeatherRenderState state_;
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
