#pragma once

// sim.weather_events: a deterministic WEATHER-EVENT scheduler that sits ON
// TOP of the existing sim-authoritative WeatherSystem (systems/WeatherSystem.h),
// WITHOUT editing it. Where WeatherSystem produces the spatial weather field
// (category map + storm cells + strikes), THIS layer produces a single world-level
// EVENT timeline: over fixed-length tick WINDOWS the world is in one of
// {Clear, Storm, Drought, Snow} at a given intensity in [0, 1]. The orchestrator
// queries it each tick and feeds the intensity into downstream sim (moisture /
// fire-dryness / snow cover) later.
//
// SCOPE. NO render, NO GL, NO entt entities — it is a world-level SCHEDULE, a pure
// function of (tick, seed). There is no per-entity state, so there is nothing to
// gate by a participant component: a query is a pure read. (An optional tiny
// stateful driver below merely caches the current window so a caller can advance
// per tick instead of recomputing; it changes no observable result.)
//
// MODEL. The timeline is partitioned into windows of kWindowTicks. Each window has
// a deterministic event drawn by a small seeded MARKOV CHAIN: window 0 starts in
// Clear, and each subsequent window's event is chosen from a per-from-state
// transition row using a uniform draw from a DeterministicRng seeded ONLY from
// integers (offset 25 ^ seed, window_index, 0). The chain favours dwelling in the
// current event and forbids the most jarring jumps (e.g. Drought <-> Snow never
// occur back-to-back), so transitions are BOUNDED. Intensity is a separate seeded
// draw per window, shaped per event (Clear is calm/low; Storm/Drought/Snow ramp
// higher), always clamped to [0, 1].
//
// DETERMINISM CONTRACT. Uses ONLY luminumbra::core::DeterministicRng (splitmix64,
// seeded from integers) — NO wall-clock, NO std::random, NO libm transcendentals
// (no exp/log/pow; we never even need Sqrt here). The same (tick, seed) ALWAYS
// yields the same WeatherEventState (run==replay); different seeds diverge because
// the seed perturbs every window's stream. Because each window's event depends on
// the previous window's event (the Markov step), WeatherEventAt walks the chain
// from window 0 up to the target window — bounded, integer-only, allocation-free.

#include <cstdint>

#include "../core/DeterministicRng.h"

namespace luminumbra::sim {

namespace Rng = ::luminumbra::core;

// Reserved seed-stream offset for the weather-events subsystem (registry: wind+11,
// weather+12/13, aether+14, plant+15, creature-repro+16, fire+17, soil+18,
// pollination+19, disease+20, irrigation+21, lifespan+22, wildlife+23, photo+24,
// weather-events+25). Mixed with the caller seed via XOR before splitmix64 stream
// composition so the +25 stream never collides with another track's stream.
inline constexpr std::uint64_t kWeatherEventSeedOffset = 25ull;

// The world-level weather event. The enum ORDER is the canonical hash/serialization
// order; Clear is the calm default (window 0 always starts Clear).
enum class WeatherEvent : std::uint8_t {
    Clear   = 0,
    Storm   = 1,
    Drought = 2,
    Snow    = 3,
};
inline constexpr int kWeatherEventCount = 4;

// Human-readable name (diagnostics / overlays). Pure, no allocation.
inline const char* WeatherEventName(WeatherEvent e) noexcept {
    switch (e) {
        case WeatherEvent::Clear:   return "clear";
        case WeatherEvent::Storm:   return "storm";
        case WeatherEvent::Drought: return "drought";
        case WeatherEvent::Snow:    return "snow";
    }
    return "clear";
}

// The scheduled state for a tick: the current event, its intensity in [0, 1], and
// the index of the window the tick falls in. POD; carries no engine handles.
struct WeatherEventState {
    WeatherEvent  event = WeatherEvent::Clear;
    float         intensity = 0.0f;
    std::uint64_t window_index = 0;
};

// ---------------------------------------------------------------------------
// Tuning constants (PINNED — changing any of these changes the schedule, which is
// a determinism contract surface for any caller that folds intensity into a hash).
// ---------------------------------------------------------------------------

// Length of one event window in ticks. At the canonical 30 Hz tick this is ~30 s
// per window — long enough to read as a weather "phase", short enough that events
// turn over visibly across a session.
inline constexpr std::uint64_t kWindowTicks = 900ull;

// ---------------------------------------------------------------------------
// Markov transition rows. kTransition[from][to] is the (non-normalized) weight of
// moving from event `from` to event `to` for the NEXT window. Rows favour dwelling
// (the diagonal dominates) and forbid the two most jarring direct jumps by giving
// them weight 0: Drought->Snow and Snow->Drought never occur back-to-back. Every
// row has a positive total so a draw always lands. Authored as floats; the draw
// uses only +-*/ and comparisons (no libm).
// ---------------------------------------------------------------------------
inline constexpr float kTransition[kWeatherEventCount][kWeatherEventCount] = {
    // from Clear   -> Clear Storm Drought Snow
    {                  0.55f, 0.20f, 0.15f, 0.10f },
    // from Storm   -> Clear Storm Drought Snow
    {                  0.40f, 0.45f, 0.05f, 0.10f },
    // from Drought -> Clear Storm Drought Snow  (Drought->Snow forbidden)
    {                  0.35f, 0.10f, 0.55f, 0.00f },
    // from Snow    -> Clear Storm Drought Snow  (Snow->Drought forbidden)
    {                  0.35f, 0.10f, 0.00f, 0.55f },
};

// Per-event intensity band [lo, hi] for the seeded per-window intensity draw. Clear
// is calm/low; Storm and Snow ramp high; Drought sits mid-high (persistent dryness).
// Both ends are within [0, 1]; the draw is clamped regardless.
inline constexpr float kIntensityLo[kWeatherEventCount] = { 0.00f, 0.45f, 0.30f, 0.40f };
inline constexpr float kIntensityHi[kWeatherEventCount] = { 0.20f, 1.00f, 0.85f, 0.95f };

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------
inline float ClampUnit(float v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// Compose the per-window seed: offset 25 XOR caller seed, mixed with the window
// index, third slot 0 (reserved). Seeded ONLY from integers, per the contract.
inline Rng::DeterministicRng WindowRng(std::uint64_t window_index, std::uint64_t seed) {
    return Rng::DeterministicRng::seeded(kWeatherEventSeedOffset ^ seed, window_index, 0ull);
}

// Pick the event for a window GIVEN the previous window's event, using a uniform
// draw against that from-state's transition row. Deterministic: a single
// next_unit draw scaled by the row total, walked term by term. Pure integer/float.
inline WeatherEvent NextEvent(WeatherEvent from, std::uint64_t window_index, std::uint64_t seed) {
    const int fi = static_cast<int>(from);

    float total = 0.0f;
    for (int j = 0; j < kWeatherEventCount; ++j) total += kTransition[fi][j];

    // total is a PINNED positive constant per row; guard belt-and-suspenders.
    if (total <= 0.0f) return WeatherEvent::Clear;

    Rng::DeterministicRng rng = WindowRng(window_index, seed);
    const float target = rng.next_unit() * total; // [0, total)

    float acc = 0.0f;
    for (int j = 0; j < kWeatherEventCount; ++j) {
        acc += kTransition[fi][j];
        if (target < acc) return static_cast<WeatherEvent>(j);
    }
    // FP edge (target == total - epsilon rounding): fall to the last positive entry.
    for (int j = kWeatherEventCount - 1; j >= 0; --j) {
        if (kTransition[fi][j] > 0.0f) return static_cast<WeatherEvent>(j);
    }
    return WeatherEvent::Clear;
}

// Seeded per-window intensity for a chosen event, in that event's band, clamped to
// [0, 1]. Uses a SEPARATE draw from the same per-window stream (after the event
// draw) so event and intensity are decorrelated yet fully reproducible.
inline float WindowIntensity(WeatherEvent event, std::uint64_t window_index, std::uint64_t seed) {
    const int ei = static_cast<int>(event);
    Rng::DeterministicRng rng = WindowRng(window_index, seed);
    rng.next_u64();                 // advance past the slot the event draw consumes
    const float u = rng.next_unit();
    const float lo = kIntensityLo[ei];
    const float hi = kIntensityHi[ei];
    return ClampUnit(lo + (hi - lo) * u);
}

// Resolve the event for a window by walking the Markov chain from window 0 (which is
// always Clear, the calm premise) up to `window_index`. Bounded, integer-only,
// allocation-free. Pure function of (window_index, seed).
inline WeatherEvent EventForWindow(std::uint64_t window_index, std::uint64_t seed) {
    WeatherEvent ev = WeatherEvent::Clear; // window 0 is always Clear.
    for (std::uint64_t w = 1; w <= window_index; ++w) {
        ev = NextEvent(ev, w, seed);
    }
    return ev;
}

// ---------------------------------------------------------------------------
// WeatherEventAt — the public pure entry point. Same (tick, seed) -> same state.
// ---------------------------------------------------------------------------
inline WeatherEventState WeatherEventAt(std::uint64_t tick, std::uint64_t seed = 0) {
    WeatherEventState out;
    out.window_index = tick / kWindowTicks;
    out.event        = EventForWindow(out.window_index, seed);
    out.intensity    = WindowIntensity(out.event, out.window_index, seed);
    return out;
}

// ---------------------------------------------------------------------------
// WeatherEventDriver — an OPTIONAL tiny stateful convenience. It advances per tick
// and caches the current window so repeated current reads inside a window don't
// re-walk the chain. It changes NOTHING observable vs WeatherEventAt: at any tick,
// current == WeatherEventAt(tick, seed). No entt, no allocation, no wall-clock.
// ---------------------------------------------------------------------------
class WeatherEventDriver {
public:
    explicit WeatherEventDriver(std::uint64_t seed = 0) : m_seed(seed) {
        m_state = WeatherEventAt(0ull, m_seed);
        m_tick = 0ull;
        m_cached_window = 0ull;
    }

    // Advance the driver to an absolute tick (idempotent for the same tick). When the
    // tick stays inside the cached window, only the window_index is reused; the event
    // is identical and intensity is recomputed cheaply (same window -> same result).
    void AdvanceTo(std::uint64_t tick) {
        m_tick = tick;
        const std::uint64_t window = tick / kWindowTicks;
        if (window != m_cached_window || (m_state.event == WeatherEvent::Clear && tick == 0ull)) {
            m_cached_window = window;
            m_state = WeatherEventAt(tick, m_seed);
        } else {
            // Same window: event + intensity are window-keyed, so the cached state
            // already matches WeatherEventAt(tick) — only the (unchanged) window holds.
            m_state.window_index = window;
        }
    }

    // Advance exactly one tick forward.
    void Step() { AdvanceTo(m_tick + 1ull); }

    [[nodiscard]] const WeatherEventState& current() const noexcept { return m_state; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return m_tick; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return m_seed; }

private:
    std::uint64_t      m_seed = 0;
    std::uint64_t      m_tick = 0;
    std::uint64_t      m_cached_window = 0;
    WeatherEventState  m_state;
};

} // namespace luminumbra::sim
