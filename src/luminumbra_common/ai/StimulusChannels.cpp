#include "StimulusChannels.h"

#include "../core/DeterministicMath.h"

namespace luminumbra::ai {
namespace {

// Call the transcendental wrappers via the fully-qualified DeterministicMath::
// name (NOT a short alias): the SimDeterminismLint exempts a line ONLY
// when the literal "DeterministicMath::" token is present, so the alias must keep
// that token. This is the same convention WeatherSystem/WindFieldSystem follow.
namespace DeterministicMath = Luminumbra::DeterministicMath;

float Clamp01(float value) noexcept {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

// Day fraction in [0, 1): 0 == cycle start (midnight), 0.5 == midday. Integer
// epoch math first (exact), then to float -- a pure function of the integer tick,
// no float accumulator (the  / critique- rule).
float DayFraction(std::uint64_t tick) noexcept {
    const std::uint64_t tick_in_day = tick % kTicksPerDayCycle;
    return static_cast<float>(static_cast<double>(tick_in_day) /
                              static_cast<double>(kTicksPerDayCycle));
}

// Season phase in [0, 1): 0 == spring equinox, 0.25 == summer solstice, 0.75 ==
// winter solstice. Mirrors the  render-side derivation so the engine Season
// channel and the render palette agree on the same tick.
float SeasonPhase(std::uint64_t tick) noexcept {
    const std::uint64_t tick_in_year = tick % kTicksPerSeasonCycle;
    return static_cast<float>(static_cast<double>(tick_in_year) /
                              static_cast<double>(kTicksPerSeasonCycle));
}

} // namespace

const char* StimulusChannelName(StimulusChannel channel) noexcept {
    switch (channel) {
        case StimulusChannel::Weather:
            return "weather";
        case StimulusChannel::Temperature:
            return "temperature";
        case StimulusChannel::TimeOfDay:
            return "time_of_day";
        case StimulusChannel::Season:
            return "season";
        case StimulusChannel::LightLevel:
            return "light_level";
        case StimulusChannel::Aether:
            return "aether";
    }
    return "unknown";
}

float StimulusChannelRegistry::SampleWeather() const noexcept {
    // Precipitation intensity [0, 1] at the sample position. A direct override
    // (a fixture / pre-sampled scalar) takes precedence; otherwise read one-way
    // from the replicated weather state. No weather/override -> deterministic dry.
    if (m_context.precip_override >= 0.0f) {
        return Clamp01(m_context.precip_override);
    }
    if (m_context.weather == nullptr) {
        return 0.0f;
    }
    return Clamp01(m_context.weather->PrecipitationAt(m_context.sample_position));
}

float StimulusChannelRegistry::SampleTemperature() const noexcept {
    // Local temperature normalized cold(0)->hot(1). Derived deterministically
    // from the season phase (summer warm, winter cold) and modulated DOWN by
    // precipitation (rain/snow cools) and by the night side of the day cycle.
    // Pure function of the tick + replicated weather; DeterministicMath trig.
    const float season_wave =
        DeterministicMath::Sin(SeasonPhase(m_context.tick) * DeterministicMath::kTwoPi); // [-1, 1]
    // Day warmth: warmest at midday, coolest at night. cos over the day phase.
    const float day_phase = DayFraction(m_context.tick) * DeterministicMath::kTwoPi;
    const float day_warmth =
        0.5f * (1.0f - DeterministicMath::Cos(day_phase)); // 0 at midnight, 1 at midday
    // Base around a temperate 0.5, +/- 0.25 for the season, +/- 0.12 for the day.
    float temperature = 0.5f + 0.25f * season_wave + 0.12f * (day_warmth - 0.5f);
    // Precipitation cools the local air a touch.
    temperature -= 0.15f * SampleWeather();
    return Clamp01(temperature);
}

float StimulusChannelRegistry::SampleTimeOfDay() const noexcept {
    // Day fraction stimulus: 0 at night (cycle start/end), 1 at midday. A raised
    // cosine over the day cycle so the curve is smooth and tick-deterministic.
    const float day_phase = DayFraction(m_context.tick) * DeterministicMath::kTwoPi;
    return Clamp01(0.5f * (1.0f - DeterministicMath::Cos(day_phase)));
}

float StimulusChannelRegistry::SampleSeason() const noexcept {
    // Season phase stimulus mapped to [0, 1]: 0 == deep winter, 1 == high summer,
    // ~0.5 at the equinoxes. (season_wave + 1) / 2 of the  season sine.
    const float season_wave =
        DeterministicMath::Sin(SeasonPhase(m_context.tick) * DeterministicMath::kTwoPi);
    return Clamp01(0.5f * (season_wave + 1.0f));
}

float StimulusChannelRegistry::SampleLightLevel() const noexcept {
    // Ambient light [0, 1]. Prefer the caller-supplied replicated light level;
    // otherwise derive a deterministic day/night curve from the tick (the same
    // raised-cosine the TimeOfDay channel uses), DIMMED by heavy precipitation
    // (overcast/storm darkens the scene).
    float light;
    if (m_context.ambient_light >= 0.0f) {
        light = m_context.ambient_light;
    } else {
        const float day_phase = DayFraction(m_context.tick) * DeterministicMath::kTwoPi;
        light = 0.5f * (1.0f - DeterministicMath::Cos(day_phase));
    }
    // Precipitation/overcast dims the light.
    light *= (1.0f - 0.4f * SampleWeather());
    return Clamp01(light);
}

float StimulusChannelRegistry::SampleAether() const noexcept {
    // Composite energy environment [0, 1]. The caller
    // supplies the already-sampled scalar (the stateful layer when
    // sim.aether_state is ON, else the re-derivable ambience); the registry
    // never reads a field system itself. Unset (< 0) is the deterministic
    // neutral 0.0 -- aether has NO tick-derived fallback (see the aether_level
    // contract in the header: no sample supplied means no energy). Clamped so
    // an over-unity sample (a saturated stateful cell) stays a bounded stimulus.
    if (m_context.aether_level < 0.0f) {
        return 0.0f;
    }
    return Clamp01(m_context.aether_level);
}

float StimulusChannelRegistry::Sample(StimulusChannel channel) const noexcept {
    switch (channel) {
        case StimulusChannel::Weather:
            return SampleWeather();
        case StimulusChannel::Temperature:
            return SampleTemperature();
        case StimulusChannel::TimeOfDay:
            return SampleTimeOfDay();
        case StimulusChannel::Season:
            return SampleSeason();
        case StimulusChannel::LightLevel:
            return SampleLightLevel();
        case StimulusChannel::Aether:
            return SampleAether();
    }
    return 0.0f;
}

} // namespace luminumbra::ai
