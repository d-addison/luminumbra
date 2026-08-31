#pragma once

// sim.fire — emergent FIRE SPREAD: the SIMULATION-SIDE combustion state.
//
// LOAD-BEARING RULE (determinism contract): this is small, fixed-point / integer
// sim truth that feeds world_hash. The renderer may read burn_state for a
// scorched material or flame VFX, but nothing here touches the renderer and
// nothing here uses wall-clock / std::random / libm transcendentals. Fire spread
// advances on the fixed 30 Hz tick (systems/FireSpreadSystem.h).
//
// GATING: CombustibleComponent is the OPT-IN marker — only entities that carry it
// participate in the fire tick. A registry with NO CombustibleComponent performs
// ZERO work and creates/mutates nothing, so the canonical headless roster (which
// has no combustibles) stays byte-identical and the NetworkStateHash baseline is
// unperturbed until combustibles are deliberately spawned.
//
// SEED OFFSET REGISTRY: wind+11, weather+12/13, aether+14, plant+15,
// creature-reproduction+16 are taken; FIRE CLAIMS +17 (used only if stochastic
// ignition is enabled — the default spread path is a pure deterministic threshold).

#include <cstdint>

namespace Luminumbra::Components {

// Discrete combustion lifecycle. Stored as an integer (uint8) in the component so
// the sim only ever hashes the index; the renderer picks scorch material / VFX.
enum class BurnState : std::uint8_t {
    Unburnt = 0,  // intact fuel; can be ignited by a nearby Burning neighbor
    Burning = 1,  // actively on fire; spreads to neighbors, counts down burn_ticks
    Burnt   = 2,  // consumed; inert — neither spreads nor re-ignites
    Count
};

// The DETERMINISTIC per-entity combustion STATE — replay-exact and cheap to hash.
//
// fuel / moisture are FIXED-POINT milli-units (integer, 0..1000 == 0.0..1.0) so no
// float accumulation enters sim-truth state. burn_state is the BurnState index.
// burn_ticks_remaining counts down once Burning and, at zero, transitions to Burnt.
struct CombustibleComponent {
    std::uint16_t fuel_milli = 1000;       // 0..1000 -> 0.0..1.0 available fuel
    std::uint16_t moisture_milli = 0;      // 0..1000 -> 0.0..1.0; resists ignition
    std::uint8_t  burn_state = 0;          // BurnState index (Unburnt/Burning/Burnt)
    std::uint32_t burn_ticks_remaining = 0;// ticks left while Burning before Burnt
    float         ignition_radius = 2.0f;  // m; a Burning cell ignites within this

    // --- convenience accessors (do not change the hashed representation) ---
    BurnState state() const { return static_cast<BurnState>(burn_state); }
    void set_state(BurnState s) { burn_state = static_cast<std::uint8_t>(s); }
    float fuel() const { return static_cast<float>(fuel_milli) * 0.001f; }
    float moisture() const { return static_cast<float>(moisture_milli) * 0.001f; }
};

// Optional explicit ignition source — lets content kick off a fire without first
// hand-setting a CombustibleComponent to Burning. Presence on an entity that also
// has a TransformComponent means: any Unburnt combustible within `radius` (subject
// to the same moisture gate as normal spread) is ignited on the tick. Carrying this
// is still opt-in; absence costs nothing.
struct IgnitionSourceComponent {
    float radius = 3.0f;     // m; ignition reach of this source
    float intensity = 1.0f;  // 0..1 heat scale (raises the heat delivered to targets)
};

} // namespace Luminumbra::Components
