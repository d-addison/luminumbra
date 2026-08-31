#pragma once

// sim.fire — deterministic emergent FIRE SPREAD across combustible entities.
//
// MODEL. Each tick, every Burning combustible is a heat SOURCE; every Unburnt
// combustible is a candidate. A candidate ignites when the total heat delivered to
// it (summed over all sources + any IgnitionSourceComponent) meets or exceeds its
// ignition resistance, which rises with moisture and falls with available fuel.
// Heat from a source FALLS OFF with distance (within the source's ignition radius)
// and is BIASED DOWNWIND: the dot of the unit source->candidate direction with the
// wind vector adds (downwind) or subtracts (upwind) heat, so fires run with the
// wind. A Burning cell burns down its burn_ticks_remaining and becomes Burnt
// (inert: stops spreading, can't re-ignite).
//
// DETERMINISM (this changes sim state -> world_hash):
//   * id-ordered traversal (entities sorted by entt::to_integral) so the snapshot
//     and the apply pass are order-independent.
//   * TWO-PHASE:  SNAPSHOTS the burning sources + ignition sources and tallies
//     heat per candidate WITHOUT mutating;  APPLIES ignition / burn-down. So
//     ignition this tick depends only on LAST tick's burning set — the result does
//     not depend on iteration order (a just-ignited cell cannot chain-ignite within
//     the same tick).
//   * Math uses only IEEE basic ops + Luminumbra::DeterministicMath (Sqrt) — NO
//     libm transcendentals, NO wall-clock, NO std::random. Ignition is a pure
//     THRESHOLD COMPARE, so no RNG is required (offset +17 is reserved for an
//     optional stochastic-ignition variant, intentionally NOT used here).
//   * run == replay holds: identical registry + tick + wind -> identical result.
//
// GATING: gated by the presence of CombustibleComponent (+ TransformComponent for
// position). A registry with NO combustibles does ZERO work and mutates nothing, so
// the canonical NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "../components/CombustionComponents.h"
#include "../components/CoreComponents.h"
#include "../core/DeterministicMath.h" // Sqrt (libm-free / IEEE-deterministic)

namespace luminumbra::sim {

namespace Comp = ::Luminumbra::Components;

// Distinct world-seed offset reserved for fire (registry note above). Only used if
// a stochastic-ignition variant is added; the default threshold path needs no RNG.
inline constexpr std::uint64_t kFireSeedOffset = 17ull;

// --- spread tuning (deterministic constants) ---
//   * kFireBaseHeat: heat delivered at zero distance by a fully-burning source.
//   * kFireWindGain: how strongly wind biases heat (added/subtracted via the
//     direction.wind dot product, scaled by source heat).
//   * a candidate's ignition RESISTANCE = kFireIgnitionBase + moisture*kFireMoistureResist,
//     reduced by fuel (dry, fuel-rich cells light easily). When delivered heat >=
//     resistance the cell ignites.
//   * kFireBurnTicks: default burn duration when a cell is ignited by spread.
inline constexpr float kFireBaseHeat = 1.0f;
inline constexpr float kFireWindGain = 0.6f;
inline constexpr float kFireIgnitionBase = 0.25f;
inline constexpr float kFireMoistureResist = 1.5f;  // moisture(0..1) * this -> resistance
inline constexpr float kFireFuelEase = 0.20f;       // fuel(0..1) * this -> resistance reduction
inline constexpr std::uint32_t kFireBurnTicks = 90; // ~3s @ 30Hz

struct FireSpreadStats {
    int considered = 0; // combustibles examined this tick
    int ignited = 0;    // Unburnt -> Burning this tick
    int burnt_out = 0;  // Burning -> Burnt this tick
};

struct FireIgnitionSource {
    glm::vec2 position{0.0f};
    float radius = 3.0f;
    float intensity = 1.0f;
};

// Heat one Burning source delivers to a candidate at separation `d` (m), with a
// downwind bias. `dir` is the UNIT source->candidate direction in the xz plane (or
// zero if coincident); `wind` is the wind velocity in xz. Within [0, radius] heat
// falls off linearly to zero at the radius; beyond the radius it is zero. The wind
// term adds heat when the candidate is downwind of the source (dir aligned with
// wind) and removes it when upwind. Clamped to >= 0. Pure / libm-free.
inline float FireHeatContribution(
    float d, float radius, const glm::vec2& dir, const glm::vec2& wind, float source_scale) {
    if (radius <= 0.0f || d > radius)
        return 0.0f;
    // Linear distance falloff in [0,1]: 1 at the source, 0 at the radius.
    const float falloff = 1.0f - (d / radius);
    float heat = kFireBaseHeat * falloff * source_scale;
    // Downwind bias: dir . wind (m/s). Positive => candidate is downwind => more heat.
    const float downwind = dir.x * wind.x + dir.y * wind.y;
    heat += kFireWindGain * downwind * falloff * source_scale;
    return heat > 0.0f ? heat : 0.0f;
}

// Advance fire spread by one fixed tick. Pure function of registry state + tick +
// wind (xz). Returns telemetry counts. `tick` is accepted for signature parity with
// the other sim ticks and for an optional stochastic variant; the deterministic
// threshold path does not consume it.
inline FireSpreadStats
RunFireSpreadOnTick(entt::registry& reg,
                    std::uint64_t tick,
                    glm::vec2 wind_xz = glm::vec2(0.0f),
                    std::span<const FireIgnitionSource> external_sources = {}) {
    (void)tick;
    FireSpreadStats stats;

    auto view = reg.view<Comp::CombustibleComponent, Comp::TransformComponent>();

    // id-ordered entity list so snapshot + apply are order-independent + deterministic.
    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // --- : snapshot heat SOURCES (this tick's Burning cells) ---
    struct Source {
        glm::vec2 pos; // xz position
        float radius;  // ignition radius (m)
        float scale;   // heat scale (1.0 for a burning cell)
    };
    std::vector<Source> sources;
    for (auto e : ents) {
        const auto& cb = view.get<Comp::CombustibleComponent>(e);
        if (cb.state() != Comp::BurnState::Burning)
            continue;
        const auto& tf = view.get<Comp::TransformComponent>(e);
        sources.push_back({glm::vec2(tf.position.x, tf.position.z), cb.ignition_radius, 1.0f});
    }
    // Explicit ignition sources (e.g. a torch/lightning) act as extra heat emitters.
    {
        auto ig = reg.view<Comp::IgnitionSourceComponent, Comp::TransformComponent>();
        std::vector<entt::entity> igs;
        for (auto e : ig)
            igs.push_back(e);
        std::sort(igs.begin(), igs.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (auto e : igs) {
            const auto& src = ig.get<Comp::IgnitionSourceComponent>(e);
            const auto& tf = ig.get<Comp::TransformComponent>(e);
            sources.push_back({glm::vec2(tf.position.x, tf.position.z),
                               src.radius,
                               src.intensity > 0.0f ? src.intensity : 0.0f});
        }
    }
    for (const FireIgnitionSource& source : external_sources) {
        sources.push_back(
            {source.position, source.radius, source.intensity > 0.0f ? source.intensity : 0.0f});
    }

    // --- : per Unburnt candidate, tally heat from all sources; ignite on threshold.
    // Burning cells burn down; at zero they become Burnt. We mutate here only (the
    // source snapshot above already captured last-tick's burning set).
    struct Apply {
        entt::entity e;
        bool ignite;    // Unburnt -> Burning
        bool burn_down; // Burning: decrement / transition
    };
    std::vector<Apply> applies;

    for (auto e : ents) {
        ++stats.considered;
        const auto& cb = view.get<Comp::CombustibleComponent>(e);
        const Comp::BurnState st = cb.state();

        if (st == Comp::BurnState::Burnt)
            continue; // inert

        if (st == Comp::BurnState::Burning) {
            applies.push_back({e, /*ignite*/ false, /*burn_down*/ true});
            continue;
        }

        // Unburnt candidate: sum delivered heat from every source.
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const glm::vec2 cpos(tf.position.x, tf.position.z);

        float heat = 0.0f;
        for (const Source& s : sources) {
            const glm::vec2 delta = cpos - s.pos;
            const float dx = delta.x, dz = delta.y;
            const float d = ::Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);
            // Unit direction source->candidate (zero if coincident -> no wind bias,
            // pure base heat at d==0).
            glm::vec2 dir(0.0f);
            if (d > 0.0f) {
                const float inv = 1.0f / d;
                dir = glm::vec2(dx * inv, dz * inv);
            }
            heat += FireHeatContribution(d, s.radius, dir, wind_xz, s.scale);
        }

        // Ignition resistance: rises with moisture, falls with fuel. Floor at 0.
        float resistance =
            kFireIgnitionBase + cb.moisture() * kFireMoistureResist - cb.fuel() * kFireFuelEase;
        if (resistance < 0.0f)
            resistance = 0.0f;

        // Need some fuel to actually catch (fuel-exhausted cells can't ignite).
        const bool has_fuel = cb.fuel_milli > 0;
        const bool ignite = has_fuel && (heat >= resistance) && (resistance > 0.0f || heat > 0.0f);
        if (ignite)
            applies.push_back({e, /*ignite*/ true, /*burn_down*/ false});
    }

    // Apply in id order (applies is already in id order since ents is).
    for (const Apply& a : applies) {
        auto& cb = reg.get<Comp::CombustibleComponent>(a.e);
        if (a.ignite) {
            cb.set_state(Comp::BurnState::Burning);
            cb.burn_ticks_remaining = kFireBurnTicks;
            ++stats.ignited;
        } else if (a.burn_down) {
            if (cb.burn_ticks_remaining > 0)
                --cb.burn_ticks_remaining;
            if (cb.burn_ticks_remaining == 0) {
                cb.set_state(Comp::BurnState::Burnt);
                cb.fuel_milli = 0; // consumed
                ++stats.burnt_out;
            }
        }
    }

    return stats;
}

} // namespace luminumbra::sim
