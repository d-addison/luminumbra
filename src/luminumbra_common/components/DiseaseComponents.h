#pragma once

// sim.disease — the SIMULATION-SIDE plant PEST/BLIGHT state .
//
// A configurable blight that spreads by PROXIMITY between plants and is gated by
// each plant's QUALITY/RESISTANCE: a low-resistance neighbour catches it; a tended
// (high-resistance) plant shrugs it off; an infected plant eventually recovers
// (raising resistance) or dies — deterministically.
//
// LOAD-BEARING RULE (mirrors PlantComponents.h): this is integer/fixed-point,
// replay-exact SIM TRUTH; nothing here touches the renderer. The disease STATE
// lives in THIS new component so no existing component is edited. A plant only
// participates in disease if it carries a PlantHealthComponent (the opt-in gate),
// AND is itself a plant (the system additionally requires PlantTag). A registry
// with NO PlantHealthComponent => the disease system is a NO-OP and the canonical
// NetworkStateHash baseline stays byte-identical.
//
// DETERMINISM: infection / resistance are FIXED-POINT milli-units (0..1000 == 0..1),
// integer tick counters drive duration, and the system threshold dynamics are pure
// integer arithmetic (no RNG needed for the default deterministic transmission).
//
// SEED OFFSET REGISTRY: wind+11, weather+12/13, aether+14, plant+15,
// creature-reproduction+16 are taken; PLANT DISEASE CLAIMS +20.

#include <cstdint>

namespace Luminumbra::Components {

// Disease lifecycle. Stored as an integer so it hashes/replays trivially.
enum class PlantDiseaseState : std::uint8_t {
    Healthy = 0,   // no active infection
    Infected,      // carrying + spreading the blight
    Recovered,     // beat the infection (raised resistance, immune-ish)
    Dead,          // succumbed (terminal)
    Count
};
inline constexpr std::uint8_t kPlantDiseaseStateCount =
    static_cast<std::uint8_t>(PlantDiseaseState::Count);

// Fixed-point scale: a value of 1000 represents 1.0 (full infection / resistance).
inline constexpr std::uint16_t kDiseaseUnitScale = 1000u;

// The plant's DETERMINISTIC disease STATE. Presence is the per-entity opt-in gate
// (like PlantTag / CreatureGenomeComponent). All quantities are fixed-point
// milli-units so no float accumulation enters the sim-truth state.
struct PlantHealthComponent {
    // Current infection load, fixed-point 0..1000 (== 0.0 .. 1.0). Rises from
    // infected neighbours, falls while recovering.
    std::uint16_t infection = 0;

    // Disease resistance, fixed-point 0..1000 (== 0.0 .. 1.0). Raised by tending /
    // high plant quality (seeded by the caller / derived each tick) and by having
    // recovered. Scales down incoming transmission: a fully-resistant plant (1000)
    // takes ZERO infection from a neighbour.
    std::uint16_t resistance = 0;

    // PlantDiseaseState index (stored as int per the determinism convention).
    std::uint8_t state = static_cast<std::uint8_t>(PlantDiseaseState::Healthy);

    // Integer counter: how many ticks the plant has been continuously Infected.
    // Drives the deterministic recover-vs-die resolution.
    std::uint32_t infected_ticks = 0;
};

} // namespace Luminumbra::Components
