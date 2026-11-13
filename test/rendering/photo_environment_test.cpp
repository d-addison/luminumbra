#include <gtest/gtest.h>

#include "luminumbra_client/app/PhotoEnvironment.h"

#include <limits>

namespace {
using Luminumbra::Client::App::CyclePhotoWeather;
using Luminumbra::Client::App::PhotoEnvironmentOverride;
using Luminumbra::Client::App::PhotoEnvironmentValues;
using Luminumbra::Client::App::SelectPhotoEnvironment;
using Luminumbra::Rendering::WeatherType;

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
