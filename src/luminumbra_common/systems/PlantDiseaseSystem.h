#pragma once

// sim.disease — the deterministic PLANT PEST/BLIGHT tick .
//
// A configurable blight that spreads by PROXIMITY and is resisted by tending /
// quality. Each fixed 30 Hz tick:
//   1. (snapshot) collect every INFECTED plant's position + infectious strength.
//   2. (expose)   for every plant, sum the infection pressure from infected
//      neighbours within kDiseaseRadius, scaled by (1 - this plant's resistance).
//      A Healthy plant whose infection crosses kInfectThreshold tips to Infected.
//      An Infected plant accrues infected_ticks and, once past kSickTicks, resolves
//      to Recovered (if resistant enough) or Dead (otherwise); recovery raises
//      resistance (acquired immunity).
//
// DETERMINISM (this changes sim state -> world_hash):
//   * id-ordered traversal (sort by entt::to_integral) so accumulation order is
//     stable across machines.
//   * TWO-PHASE: we snapshot the infected set FIRST, then mutate health, so a
//     plant infected THIS tick does not retroactively infect its neighbours within
//     the same tick (order-independent spread).
//   * All sim quantities are FIXED-POINT integers (milli-units, 0..1000); distance
//     uses DeterministicMath::Sqrt (the one IEEE-deterministic transcendental).
//   * NO RNG needed: transmission is a deterministic distance-falloff threshold.
//     (The +20 seed offset is reserved for an optional stochastic variant.)
//
// GATING: a plant only participates if it carries BOTH PlantTag (it IS a plant) and
// PlantHealthComponent (the disease opt-in). A registry with no PlantHealthComponent
// and in particular the canonical headless roster with no plants — does ZERO work
// and creates/mutates nothing, so the NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/PlantComponents.h"
#include "../core/DeterministicMath.h" // Sqrt (IEEE-deterministic)
#include "../core/DeterministicRng.h"  // reserved (offset +20) for a stochastic variant

namespace luminumbra::foliage {

namespace Comp = ::Luminumbra::Components;

// Distinct world-seed offset for the disease stream (see registry note above).
// Reserved for an optional stochastic-transmission variant; the default dynamics
// are a pure deterministic threshold and consume no RNG.
inline constexpr std::uint64_t kDiseaseSeedOffset = 20ull;

// --- Tuning (deterministic integer/float constants) ---

// Plants within this many world units of an infected plant are exposed.
inline constexpr float kDiseaseRadius = 4.0f;

// Per-tick infection pressure (fixed-point milli-units) emitted by one infected
// plant at zero distance; it falls off linearly to zero at kDiseaseRadius.
inline constexpr std::uint16_t kDiseaseBasePressure = 220u;

// Infection (milli-units) at/above which a Healthy plant tips to Infected.
inline constexpr std::uint16_t kInfectThreshold = 500u;

// An Infected plant resolves (recover-or-die) once it has been sick this long.
inline constexpr std::uint32_t kSickTicks = 90u; // ~3s @ 30Hz

// At resolution, a plant with resistance at/above this RECOVERS; otherwise it DIES.
inline constexpr std::uint16_t kRecoverResistance = 400u;

// Recovery raises resistance by this much (acquired immunity), saturating at 1000.
inline constexpr std::uint16_t kRecoveryResistanceGain = 250u;

// While Infected and below resolution, infection naturally decays a little each
// tick (the plant fights back); the net direction depends on neighbour pressure.
inline constexpr std::uint16_t kInfectionDecayPerTick = 8u;

struct PlantDiseaseStats {
    int considered = 0;     // plants with a health component examined this tick
    int infected = 0;       // plants currently in the Infected state after the tick
    int new_infections = 0; // Healthy -> Infected transitions this tick
    int recovered = 0;      // Infected -> Recovered transitions this tick
    int died = 0;           // Infected -> Dead transitions this tick
};

// Saturating fixed-point add/sub helpers (clamp to [0, kDiseaseUnitScale]).
inline std::uint16_t SatAdd(std::uint16_t a, std::uint32_t b) {
    const std::uint32_t s = static_cast<std::uint32_t>(a) + b;
    return static_cast<std::uint16_t>(s > Comp::kDiseaseUnitScale ? Comp::kDiseaseUnitScale : s);
}
inline std::uint16_t SatSub(std::uint16_t a, std::uint32_t b) {
    return static_cast<std::uint16_t>(b >= a ? 0u : (static_cast<std::uint32_t>(a) - b));
}

// Advance the plant blight by ONE fixed tick. Pure function of registry state +
// tick id. Returns telemetry (also useful for the world sub-hash). Deterministic,
// id-ordered, two-phase. `world_seed` lets distinct worlds diverge (reserved for a
// future stochastic variant); the default deterministic path ignores it.
inline PlantDiseaseStats
RunPlantDiseaseOnTick(entt::registry& reg, std::uint64_t tick, std::uint64_t world_seed = 0) {
    (void)tick;       // unused by the deterministic threshold path
    (void)world_seed; // reserved for the +20 stochastic variant
    PlantDiseaseStats stats;

    auto view =
        reg.view<Comp::PlantTag, Comp::PlantHealthComponent, const Comp::TransformComponent>();

    // id-ordered participant ids so accumulation + transition order is deterministic.
    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    if (ents.empty())
        return stats; // explicit no-op for the empty roster

    //  (snapshot): record every INFECTED plant's position. Reading the
    // infected set BEFORE any mutation makes spread order-independent within a tick.
    struct Source {
        float x, y, z;
    };
    std::vector<Source> sources;
    sources.reserve(ents.size());
    for (auto e : ents) {
        const auto& h = view.get<Comp::PlantHealthComponent>(e);
        if (h.state == static_cast<std::uint8_t>(Comp::PlantDiseaseState::Infected)) {
            const auto& tf = view.get<const Comp::TransformComponent>(e);
            sources.push_back(Source{tf.position.x, tf.position.y, tf.position.z});
        }
    }

    const float radius2 = kDiseaseRadius * kDiseaseRadius;

    //  (expose + resolve): mutate each plant's health from the snapshot.
    for (auto e : ents) {
        ++stats.considered;
        auto& h = view.get<Comp::PlantHealthComponent>(e);
        const auto& tf = view.get<const Comp::TransformComponent>(e);

        // Terminal plants do nothing further (a carcass neither spreads nor heals).
        if (h.state == static_cast<std::uint8_t>(Comp::PlantDiseaseState::Dead)) {
            continue;
        }

        // Sum incoming infection pressure from the snapshot of infected sources,
        // skipping a source coincident with this plant (its own position) so a
        // plant never re-infects itself. Linear distance falloff -> 0 at the radius.
        std::uint32_t pressure = 0;
        for (const Source& s : sources) {
            const float dx = tf.position.x - s.x;
            const float dy = tf.position.y - s.y;
            const float dz = tf.position.z - s.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 >= radius2)
                continue; // out of range
            if (d2 <= 0.0000001f)
                continue; // self / coincident source
            const float dist = ::Luminumbra::DeterministicMath::Sqrt(d2);
            // falloff in [0,1): (radius - dist) / radius.
            const float falloff = (kDiseaseRadius - dist) * (1.0f / kDiseaseRadius);
            pressure +=
                static_cast<std::uint32_t>(static_cast<float>(kDiseaseBasePressure) * falloff);
        }

        // Resistance scales DOWN the effective pressure: a plant with resistance r
        // (milli-units) admits pressure * (1000 - r) / 1000.
        const std::uint32_t admit =
            static_cast<std::uint32_t>(Comp::kDiseaseUnitScale) - h.resistance;
        const std::uint32_t effective = (pressure * admit) / Comp::kDiseaseUnitScale;

        if (h.state == static_cast<std::uint8_t>(Comp::PlantDiseaseState::Infected)) {
            // Already sick: incoming pressure tops up infection; meanwhile the plant
            // fights back a little. Net fixed-point integer step.
            h.infection = SatAdd(h.infection, effective);
            h.infection = SatSub(h.infection, kInfectionDecayPerTick);
            ++h.infected_ticks;

            if (h.infected_ticks >= kSickTicks) {
                if (h.resistance >= kRecoverResistance) {
                    h.state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Recovered);
                    h.resistance = SatAdd(h.resistance, kRecoveryResistanceGain);
                    h.infection = 0;
                    h.infected_ticks = 0;
                    ++stats.recovered;
                } else {
                    h.state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Dead);
                    h.infection = Comp::kDiseaseUnitScale;
                    ++stats.died;
                }
            }
        } else {
            // Healthy or Recovered: accrue infection from pressure. Recovered plants
            // resist re-infection through their (raised) resistance, not immunity, so
            // a still-high resistance keeps `effective` low. With no pressure, a
            // sub-threshold infection decays back toward zero.
            if (effective > 0) {
                h.infection = SatAdd(h.infection, effective);
            } else {
                h.infection = SatSub(h.infection, kInfectionDecayPerTick);
            }

            if (h.infection >= kInfectThreshold &&
                h.state == static_cast<std::uint8_t>(Comp::PlantDiseaseState::Healthy)) {
                h.state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Infected);
                h.infected_ticks = 0;
                ++stats.new_infections;
            }
        }

        if (h.state == static_cast<std::uint8_t>(Comp::PlantDiseaseState::Infected)) {
            ++stats.infected;
        }
    }

    return stats;
}

} // namespace luminumbra::foliage
