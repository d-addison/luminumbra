#pragma once

#include <luminumbra/core/Types.h>

#include <cstdint>
#include <vector>

namespace Luminumbra::Systems {

// PINNED bounded-state cap (regression review / documented design): at most this many
// storm cells are ever active. The container is fixed-capacity; a scheduled spawn
// is dropped when full (deterministically -- the schedule itself is bounded so
// this is belt-and-suspenders, asserted by Endurance300Storm).
inline constexpr int kMaxStormCells = 16;

// PINNED bounded-state cap: at most this many lightning strike events
// are retained in the live schedule window at once. A strike is a transient WORLD
// EVENT (replayable, identical on every client); the schedule is folded into the
// `weather` world_hash sub-hash. The window is small + flat-memory so the strike
// set never grows unbounded (Endurance300Storm asserts the bound).
inline constexpr int kMaxLiveStrikes = 32;

// One active storm cell. Position/velocity are world-space (XZ); the cell is
// advected every tick by the wind grid. spawn_tick/lifetime_ticks bound the
// intensity envelope (deterministic ramp-in/decay). seed_salt makes each cell's
// later strike-schedule an independent seeded stream.
struct StormCell {
    Vec2 center_world = Vec2(0.0f);
    Vec2 velocity = Vec2(0.0f); // m/s-ish, set from wind at spawn + re-advected
    float intensity = 0.0f;     // current [0, 1] envelope value
    std::uint64_t spawn_tick = 0;
    std::uint32_t lifetime_ticks = 0;
    std::uint32_t seed_salt = 0; // per-cell RNG salt ( strike stream)
};

// One scheduled lightning STRIKE event. A strike is a deterministic
// WORLD EVENT scheduled by storm state from the `seed+13` RNG stream -- replayable
// and bit-identical for every client. It folds into the `weather` world_hash
// sub-hash (it is sim-authoritative state, not a render effect). The RENDER side
// reads the schedule one-way  to draw the bolt / inject the light pulse / spawn
// scorch emitters / cue thunder; it writes NOTHING back.
//
// GameSession converts strikes due on the current tick into deterministic fire
// ignition sources. Worlds without combustible entities remain a no-op.
struct StrikeEvent {
    std::uint64_t strike_tick = 0; // absolute sim tick the bolt lands on
    float world_x = 0.0f;          // strike ground position X (world metres)
    float world_z = 0.0f;          // strike ground position Z (world metres)
    float magnitude = 0.0f;        // [0, 1] strike strength (drives pulse/thunder)
    std::uint32_t storm_salt = 0;  // originating storm cell's seed_salt (provenance)
};

struct AmbientWindPerturbation {
    Vec2 center_world = Vec2(0.0f);
    float radius_m = 0.0f;
    Vec2 velocity = Vec2(0.0f);
    float intensity = 0.0f;
};

[[nodiscard]] bool AmbientWindPerturbationLess(const AmbientWindPerturbation& left,
                                               const AmbientWindPerturbation& right);

// Owner envelope v2 only: at most 16 storms, 32 strikes and 16 wind inputs.
// Floats are encoded as u32 IEEE bits; no raw struct layout is serialized.
struct AmbientContinuation {
    std::uint64_t tick = 0;
    Vec3 anchor{};
    bool constructor_state = true;
    std::vector<StormCell> storms;
    std::vector<StrikeEvent> strikes;
    std::vector<AmbientWindPerturbation> wind;
};
[[nodiscard]] bool ValidAmbientContinuation(const AmbientContinuation& state);

} // namespace Luminumbra::Systems
