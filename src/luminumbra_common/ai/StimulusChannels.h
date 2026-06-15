#pragma once

// T-I5b-2 (E1): ecology stimulus-channel REGISTRY feeding the InstinctSystem
// planner. Extends the T-I3-18 single-channel (light_stimulus) pattern to a
// registry of named environmental channels the planner samples per tick.
//
// DESIGN (design-decisions.md engine-iteration-5b §3, critique F1).
//   * The engine knows only "channel -> scalar stimulus in [0, 1]". Creature
//     REACTIONS (shelter in rain, dawn activity) are GAME DATA: an archetype
//     opts a creature in via a StimulusSubscriptionComponent listing
//     {channel, need, gain} mappings. The engine never names a creature behavior.
//   * The registry is INERT for non-subscribing creatures: a channel scalar is
//     computed LAZILY only when a subscription asks for it. The canonical world
//     (the HeadlessServerTick default roster) carries NO subscription component,
//     so the planner tick path is byte-unchanged and world_hash stays
//     `d950a6afc12a5cdc` (NO bump #4). Adding the registry must not perturb the
//     default entities sub-hash.
//   * DETERMINISM. Every channel is a pure function of the per-tick
//     StimulusContext (tick + replicated weather/light state). Transcendentals
//     route through core/DeterministicMath.h (NO libm trig on this sim path) so
//     the channels are bit-deterministic and pass SimDeterminismLint.
//
// ONE-WAY. The context is filled from the replicated weather/light state the
// SIM already owns; the registry READS it and writes NOTHING back into the sim
// or world_hash. Sampling a channel never mutates registry/context state.

#include <cstdint>

#include "../systems/WeatherSystem.h"
#include "../../../include/luminumbra/core/Types.h"

namespace luminumbra::ai {

// The environmental channels the registry exposes. The enum order is the
// canonical iteration order (stable for any hashing of subscription state).
// Append-only: never reorder/reuse a slot.
enum class StimulusChannel : std::uint8_t {
    Weather = 0,      // precipitation intensity at the sampled position [0, 1]
    Temperature = 1,  // local temperature, normalized cold->hot [0, 1]
    TimeOfDay = 2,    // day fraction stimulus: 0 at night, 1 at midday [0, 1]
    Season = 3,       // season phase stimulus: spring/autumn ~0.5, summer 1, winter 0
    LightLevel = 4,   // ambient light level [0, 1] (0 dark, 1 full daylight)
};
inline constexpr int kStimulusChannelCount = 5;
const char* StimulusChannelName(StimulusChannel channel) noexcept;

// PINNED day-cycle period (ticks). A day-of-the-engine is one cycle of the
// TimeOfDay / LightLevel channels. 30 Hz * 60 s * 4 min = 7200 ticks (a 4-minute
// engine day) -- a tick-pure cycle, no wall-clock, no float accumulator. The
// channel is a pure function of (tick % kTicksPerDayCycle).
inline constexpr std::uint64_t kTicksPerDayCycle = 7200ull;

// PINNED season-cycle period (ticks). Mirrors the C2 render-side season cycle
// (RenderPipeline kTicksPerSeasonCycle = 432000 = 4 h at 30 Hz) so the engine
// Season channel and the render season palette agree on the same tick.
inline constexpr std::uint64_t kTicksPerSeasonCycle = 432000ull;

// Per-tick inputs a channel may read. POD; carries no engine handles. Filled by
// the caller from the replicated weather/light state the sim already owns. The
// weather pointer is OPTIONAL: when null, the weather/temperature channels fall
// back to a deterministic neutral derived from the tick only (so the registry is
// usable in headless fixtures with no weather system, and never dereferences a
// null). The whole context is a pure function of (tick, sample_position, the
// replicated state) -- reproducible on every client/replay.
struct StimulusContext {
    std::uint64_t tick = 0;
    Luminumbra::Vec3 sample_position = Luminumbra::Vec3(0.0f);
    // Replicated weather state (read-only, one-way). May be null.
    const Luminumbra::Systems::WeatherSystem* weather = nullptr;
    // Direct precipitation override [0, 1]. When >= 0 the Weather channel uses
    // this scalar and ignores `weather` -- the deterministic path a fixture (rain
    // vs clear) or a caller with an already-sampled precip uses. When < 0 the
    // Weather channel reads `weather->PrecipitationAt(sample_position)` (or 0 if
    // `weather` is null too). Never a wall-clock/RNG input; a pure scalar.
    float precip_override = -1.0f;
    // Replicated ambient light level [0, 1] at the sample position. When the
    // caller has no light system it leaves this < 0 and the LightLevel channel
    // derives a deterministic day/night curve from the tick instead.
    float ambient_light = -1.0f;
};

// The stimulus-channel registry. Stateless beyond the bound context: it is a
// thin pure-function dispatcher. Construct it around a context, then Sample a
// channel on demand (lazily) for each subscribing creature. Non-subscribers
// never call Sample, so no channel work runs for them (the INERT property).
class StimulusChannelRegistry {
public:
    explicit StimulusChannelRegistry(const StimulusContext& context) noexcept
        : m_context(context) {}

    // Scalar stimulus in [0, 1] for `channel` under the bound context. Pure;
    // does not mutate the registry or the context. Lazy: only the requested
    // channel is computed.
    [[nodiscard]] float Sample(StimulusChannel channel) const noexcept;

    [[nodiscard]] const StimulusContext& context() const noexcept { return m_context; }

private:
    [[nodiscard]] float SampleWeather() const noexcept;
    [[nodiscard]] float SampleTemperature() const noexcept;
    [[nodiscard]] float SampleTimeOfDay() const noexcept;
    [[nodiscard]] float SampleSeason() const noexcept;
    [[nodiscard]] float SampleLightLevel() const noexcept;

    StimulusContext m_context;
};

} // namespace luminumbra::ai
