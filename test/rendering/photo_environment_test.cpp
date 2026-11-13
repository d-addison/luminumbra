#include <gtest/gtest.h>

#include "luminumbra_client/app/PhotoEnvironment.h"
#include "luminumbra_client/rendering/WeatherOverlayModel.h"

#include <algorithm>
#include <limits>

namespace {
using Luminumbra::Client::App::CyclePhotoWeather;
using Luminumbra::Client::App::PhotoEnvironmentOverride;
using Luminumbra::Client::App::PhotoEnvironmentValues;
using Luminumbra::Client::App::PhotoWeatherOverride;
using Luminumbra::Client::App::SelectPhotoEnvironment;
using Luminumbra::Rendering::WeatherRenderState;
using Luminumbra::Rendering::WeatherType;

// CPU-only renderer interface with the same driven/debug setter distinction.
struct WeatherPipeline {
    WeatherType weather = WeatherType::None;
    float intensity = 0.0f;
    WeatherRenderState state;
    int writes = 0;

    WeatherType get_weather_type() const {
        return weather;
    }
    float get_weather_intensity() const {
        return intensity;
    }
    const WeatherRenderState& get_weather_state() const {
        return state;
    }
    void set_weather(WeatherType type, float amount) {
        weather = type;
        intensity = std::clamp(amount, 0.0f, 1.0f);
        state.driven = false;
        ++writes;
    }
    void set_weather_state(const WeatherRenderState& next) {
        state = next;
        state.driven = true;
        const auto classification = Luminumbra::Rendering::ClassifyWeatherState(state);
        weather = classification.type;
        intensity = classification.intensity;
        ++writes;
    }
};

constexpr PhotoEnvironmentOverride kPhoto{.time_active = true,
                                          .time_of_day = 0.24f,
                                          .weather_active = true,
                                          .weather = WeatherType::Rain,
                                          .intensity = 0.7f,
                                          .weather_index = 3};
constexpr PhotoEnvironmentValues kLive{
    .time_of_day = 0.0f, .weather = WeatherType::Snow, .intensity = 0.25f};

static_assert(SelectPhotoEnvironment(kLive, kPhoto, true, false).weather == WeatherType::Rain);
static_assert(SelectPhotoEnvironment(kLive, {}, true, false).weather == kLive.weather);
static_assert(SelectPhotoEnvironment(kLive, kPhoto, false, false).time_of_day == kLive.time_of_day);
static_assert(SelectPhotoEnvironment(kLive, kPhoto, true, true).weather == kLive.weather);
} // namespace

TEST(PhotoEnvironment, OverrideRemainsSelectedAfterEveryLivePush) {
    // The frame writes live defaults before asking which photo channels to apply.
    for (int frame = 0; frame < 4; ++frame) {
        auto live = kLive;
        live.time_of_day += static_cast<float>(frame) * 0.1f;
        live.intensity += static_cast<float>(frame) * 0.1f;
        const auto selected = SelectPhotoEnvironment(live, kPhoto, true, false);
        EXPECT_TRUE(selected.time_active);
        EXPECT_FLOAT_EQ(selected.time_of_day, 0.24f);
        EXPECT_TRUE(selected.weather_active);
        EXPECT_EQ(selected.weather, WeatherType::Rain);
        EXPECT_FLOAT_EQ(selected.intensity, 0.7f);
    }
}

TEST(PhotoEnvironment, ClearedChannelsLeaveLiveValuesUntouched) {
    const auto selected = SelectPhotoEnvironment(kLive, {}, true, false);
    EXPECT_FALSE(selected.time_active);
    EXPECT_FALSE(selected.weather_active);
    EXPECT_FLOAT_EQ(selected.time_of_day, kLive.time_of_day);
    EXPECT_EQ(selected.weather, kLive.weather);
    EXPECT_FLOAT_EQ(selected.intensity, kLive.intensity);
    const auto weather_live = CyclePhotoWeather(kPhoto, 3);
    const auto time_only = SelectPhotoEnvironment(kLive, weather_live, true, false);
    EXPECT_TRUE(time_only.time_active);
    EXPECT_FLOAT_EQ(time_only.time_of_day, kPhoto.time_of_day);
    EXPECT_FALSE(time_only.weather_active);
    EXPECT_EQ(time_only.weather_index, 0);
    EXPECT_EQ(time_only.weather, kLive.weather);
    EXPECT_FLOAT_EQ(time_only.intensity, kLive.intensity);
    auto weather_only_override = kPhoto;
    weather_only_override.time_active = false;
    const auto weather_only = SelectPhotoEnvironment(kLive, weather_only_override, true, false);
    EXPECT_FALSE(weather_only.time_active);
    EXPECT_FLOAT_EQ(weather_only.time_of_day, kLive.time_of_day);
    EXPECT_TRUE(weather_only.weather_active);
    EXPECT_EQ(weather_only.weather, kPhoto.weather);
}

TEST(PhotoEnvironment, ExitReleasesBothChannelsEvenWithStaleOverrides) {
    const auto selected = SelectPhotoEnvironment(kLive, kPhoto, false, false);
    EXPECT_FALSE(selected.time_active);
    EXPECT_FALSE(selected.weather_active);
    EXPECT_FLOAT_EQ(selected.time_of_day, kLive.time_of_day);
    EXPECT_EQ(selected.weather, kLive.weather);
    EXPECT_FLOAT_EQ(selected.intensity, kLive.intensity);
}

TEST(PhotoEnvironment, ScenarioPinsSuppressBothPhotoChannels) {
    for (const bool active : {false, true}) {
        const auto selected = SelectPhotoEnvironment(kLive, kPhoto, active, true);
        EXPECT_FALSE(selected.time_active);
        EXPECT_FALSE(selected.weather_active);
        EXPECT_FLOAT_EQ(selected.time_of_day, kLive.time_of_day);
        EXPECT_EQ(selected.weather, kLive.weather);
        EXPECT_FLOAT_EQ(selected.intensity, kLive.intensity);
    }
}

TEST(PhotoEnvironment, ClearIsAnActiveZeroIntensityPresetAndCycleReturnsToLive) {
    auto photo = CyclePhotoWeather({}, 1);
    EXPECT_TRUE(photo.weather_active);
    EXPECT_EQ(photo.weather, WeatherType::None);
    EXPECT_FLOAT_EQ(photo.intensity, 0.0f);
    const auto clear = SelectPhotoEnvironment(kLive, photo, true, false);
    EXPECT_TRUE(clear.weather_active);
    EXPECT_EQ(clear.weather, WeatherType::None);
    EXPECT_FLOAT_EQ(clear.intensity, 0.0f);
    for (const auto weather :
         {WeatherType::Fog, WeatherType::Rain, WeatherType::Snow, WeatherType::Storm}) {
        photo = CyclePhotoWeather(photo, 1);
        EXPECT_TRUE(photo.weather_active);
        EXPECT_EQ(photo.weather, weather);
        EXPECT_FLOAT_EQ(photo.intensity, 0.7f);
    }
    photo = CyclePhotoWeather(photo, 1);
    EXPECT_FALSE(photo.weather_active);
    EXPECT_EQ(photo.weather_index, 0);
}

TEST(PhotoEnvironment, SignedAndFullRangeCyclesStayWithinTheSixChoices) {
    for (const int index :
         {0, 3, 5, std::numeric_limits<int>::max(), std::numeric_limits<int>::min()}) {
        for (const int cycle :
             {0, 1, -1, 6, -6, std::numeric_limits<int>::max(), std::numeric_limits<int>::min()}) {
            auto initial = kPhoto;
            initial.weather_index = index;
            const auto selected = CyclePhotoWeather(initial, cycle);
            const auto expected = (static_cast<long long>(index) + cycle) % 6;
            EXPECT_EQ(selected.weather_index, (expected + 6) % 6);
            EXPECT_GE(selected.weather_index, 0);
            EXPECT_LT(selected.weather_index, 6);
        }
    }
    EXPECT_EQ(CyclePhotoWeather({}, -1).weather, WeatherType::Storm);
}

TEST(PhotoEnvironment, PresetToLiveWithoutWeatherWriterRestoresPreOverrideWeather) {
    for (const auto underlying : {WeatherType::None, WeatherType::Fog, WeatherType::Snow}) {
        for (const float intensity : {0.0f, 1.0f}) {
            WeatherPipeline pipeline;
            pipeline.set_weather(underlying, intensity);
            PhotoWeatherOverride weather_override;
            auto photo = CyclePhotoWeather({}, 1);
            // Clear, fog, rain, snow, storm, then live. No live writer runs.
            for (int frame = 0; frame < 7; ++frame) {
                weather_override.Restore(pipeline);
                if (frame != 0) {
                    photo = CyclePhotoWeather(photo, 1);
                }
                const PhotoEnvironmentValues live{.time_of_day = 0.5f,
                                                  .weather = pipeline.get_weather_type(),
                                                  .intensity = pipeline.get_weather_intensity()};
                const auto selected = SelectPhotoEnvironment(live, photo, true, false);
                if (selected.weather_active) {
                    weather_override.Apply(pipeline, selected.weather, selected.intensity);
                    EXPECT_EQ(pipeline.weather, photo.weather);
                    EXPECT_FLOAT_EQ(pipeline.intensity, photo.intensity);
                } else {
                    EXPECT_EQ(pipeline.weather, underlying);
                    EXPECT_FLOAT_EQ(pipeline.intensity, intensity);
                    EXPECT_FALSE(pipeline.state.driven);
                }
            }
        }
    }
}

TEST(PhotoEnvironment, RestorePreservesFullDrivenWeatherAndRepeatedApplyKeepsOriginal) {
    WeatherPipeline pipeline;
    WeatherRenderState original;
    original.driven = true;
    original.rain_intensity = 0.2f;
    original.snow_intensity = 0.3f;
    original.fog_density = 0.4f;
    original.storm_intensity = 0.5f;
    original.wetness = 0.6f;
    original.wind_direction = glm::vec3(0.0f, 0.0f, -1.0f);
    original.wind_strength = 0.8f;
    pipeline.set_weather_state(original);
    const auto original_type = pipeline.weather;
    const float original_intensity = pipeline.intensity;
    PhotoWeatherOverride weather_override;
    weather_override.Apply(pipeline, WeatherType::Rain, 0.7f);
    weather_override.Apply(pipeline, WeatherType::None, 0.0f);
    weather_override.Restore(pipeline);
    EXPECT_EQ(pipeline.weather, original_type);
    EXPECT_FLOAT_EQ(pipeline.intensity, original_intensity);
    EXPECT_TRUE(pipeline.state.driven);
    EXPECT_FLOAT_EQ(pipeline.state.rain_intensity, original.rain_intensity);
    EXPECT_FLOAT_EQ(pipeline.state.snow_intensity, original.snow_intensity);
    EXPECT_FLOAT_EQ(pipeline.state.fog_density, original.fog_density);
    EXPECT_FLOAT_EQ(pipeline.state.storm_intensity, original.storm_intensity);
    EXPECT_FLOAT_EQ(pipeline.state.wetness, original.wetness);
    EXPECT_EQ(pipeline.state.wind_direction, original.wind_direction);
    EXPECT_FLOAT_EQ(pipeline.state.wind_strength, original.wind_strength);
    const int writes = pipeline.writes;
    weather_override.Restore(pipeline);
    EXPECT_EQ(pipeline.writes, writes);
}

TEST(PhotoEnvironment, FreshLiveWeatherReplacesRestoredSnapshotBeforeNextOverride) {
    for (const bool driven : {false, true}) {
        WeatherPipeline pipeline;
        PhotoWeatherOverride weather_override;
        // Each frame supplies a different live sample after restoring the last one.
        for (int frame = 1; frame <= 4; ++frame) {
            weather_override.Restore(pipeline);
            const float intensity = static_cast<float>(frame) * 0.2f;
            if (driven) {
                WeatherRenderState state;
                state.snow_intensity = intensity;
                state.wetness = intensity;
                pipeline.set_weather_state(state);
            } else {
                pipeline.set_weather(WeatherType::Snow, intensity);
            }
            weather_override.Apply(pipeline, WeatherType::Rain, 0.7f);
        }
        weather_override.Restore(pipeline);
        EXPECT_EQ(pipeline.weather, WeatherType::Snow);
        EXPECT_FLOAT_EQ(pipeline.intensity, 0.8f);
        EXPECT_EQ(pipeline.state.driven, driven);
        if (driven) {
            EXPECT_FLOAT_EQ(pipeline.state.wetness, 0.8f);
        }
    }
}

TEST(PhotoEnvironment, DrivenClearAndFullIntensityWeatherRemainDrivenAfterRelease) {
    for (const float intensity : {0.0f, 1.0f}) {
        WeatherPipeline pipeline;
        WeatherRenderState state;
        state.snow_intensity = intensity;
        pipeline.set_weather_state(state);
        PhotoWeatherOverride weather_override;
        weather_override.Apply(pipeline, WeatherType::Storm, 0.7f);
        weather_override.Restore(pipeline);
        EXPECT_TRUE(pipeline.state.driven);
        EXPECT_EQ(pipeline.weather, intensity == 0.0f ? WeatherType::None : WeatherType::Snow);
        EXPECT_FLOAT_EQ(pipeline.intensity, intensity);
        EXPECT_FLOAT_EQ(pipeline.state.snow_intensity, intensity);
    }
}

TEST(PhotoEnvironment, InactiveExitAndCapturePinsReleaseWeatherWithoutLiveWriter) {
    for (const bool active : {false, true}) {
        for (const bool pinned : {false, true}) {
            WeatherPipeline pipeline;
            PhotoWeatherOverride weather_override;
            weather_override.Restore(pipeline);
            EXPECT_EQ(pipeline.writes, 0);
            pipeline.set_weather(WeatherType::Snow, 0.25f);
            weather_override.Apply(pipeline, kPhoto.weather, kPhoto.intensity);
            weather_override.Restore(pipeline);
            const auto selected = SelectPhotoEnvironment(kLive, kPhoto, active, pinned);
            if (selected.weather_active) {
                weather_override.Apply(pipeline, selected.weather, selected.intensity);
                EXPECT_EQ(pipeline.weather, kPhoto.weather);
            } else {
                EXPECT_EQ(pipeline.weather, kLive.weather);
                EXPECT_FLOAT_EQ(pipeline.intensity, kLive.intensity);
            }
        }
    }
}

TEST(PhotoEnvironment, FrameWithoutPhotoUpdateReleasesBeforeMenuPreviewWeather) {
    WeatherPipeline pipeline;
    PhotoWeatherOverride weather_override;
    pipeline.set_weather(WeatherType::Snow, 0.25f);
    weather_override.Apply(pipeline, WeatherType::Rain, 0.7f);
    // The menu frame restores weather without running the in-game photo update.
    weather_override.Restore(pipeline);
    EXPECT_EQ(pipeline.weather, WeatherType::Snow);
    EXPECT_FLOAT_EQ(pipeline.intensity, 0.25f);
    pipeline.set_weather(WeatherType::Fog, 0.4f);
    // A later gameplay frame must retain the preview's newer weather.
    const int writes = pipeline.writes;
    weather_override.Restore(pipeline);
    EXPECT_EQ(pipeline.weather, WeatherType::Fog);
    EXPECT_FLOAT_EQ(pipeline.intensity, 0.4f);
    EXPECT_EQ(pipeline.writes, writes);
}
