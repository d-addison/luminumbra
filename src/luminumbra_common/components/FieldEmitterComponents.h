#pragma once

//  -5 — FIELD EMITTERS: the gameplay WRITE side of the
// stateful energy layer (fields/EnergyFieldState). An entity carrying this plus a
// TransformComponent deposits `rate_raw_per_tick` raw units into its 24 m world
// cell every tick; the crystal archetypes and energy-flavored flora ride this on
// the game-content track (the engine knows only a generic "energy field").
//
// CHANNEL AS DATA (the Factorio emissions_per_minute precedent): the channel is a
// plain data key, not an API surface — 's Lumin/Umbra polarity becomes
// channel 1 of the same component, no new component type, no new deposit path.
//
// TWO-PHASE SORTED DEPOSITS (/ ordering law): emitters never write
// the field directly. The gather helper (systems/FieldEmitterSystem.h) visits
// carriers in entity-id order and QUEUES (emitter_id, cell, amount) into the
// layer; EnergyFieldState re-sorts by (cell, channel, emitter_id) and applies
// with saturation, so registry iteration order can never reach the field bytes.
//
// GATING. Additive opt-in, the ThirstComponent idiom: zero-default POD, presence
// is the per-entity participation flag. No component — or rate 0 — means no
// deposits, no pages, no hash bytes, so the canonical roster (which carries
// none) stays byte-identical while sim.aether_state is OFF and even when ON.
//
// DETERMINISM. All fields are plain integers consumed by integer shift math
// only; the component owns no float, no RNG, no wall-clock state.

#include <cstdint>

namespace Luminumbra::Components {

// Per-entity energy emitter. Deposits into the world cell under the entity's
// transform (floor-quantized by the shared 24 m cell size) each tick; when
// radius_cells > 0 the surrounding cells within that Chebyshev radius also
// receive rate >> (Chebyshev distance) — an integer halving falloff per ring.
struct FieldEmitterComponent {
    // Data key into the layer's channel set (0 = energy;  polarity = 1).
    int channel = 0;
    // Raw units queued per tick at the centre cell (kEnergyRawPerUnit raw = 1
    // gameplay unit). 0 = inert: the gather helper skips the entity entirely.
    std::uint32_t rate_raw_per_tick = 0;
    // Chebyshev falloff radius in cells. 0 = centre cell only.
    int radius_cells = 0;
};

} // namespace Luminumbra::Components
