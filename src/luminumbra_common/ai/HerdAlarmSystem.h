#pragma once

// sim.herd_alarm: ALARM SIGNALLING / COLLECTIVE VIGILANCE.
//
// When a prey senses a threat it raises an alarm; this system PROPAGATES that alarm to
// nearby SAME-ROLE neighbours so the herd flees together. Concretely it maintains a
// scalar "alarm field" over every creature carrying an AlarmComponent:
//   * a creature is an alarm SOURCE this tick if it is alarmed==1 (effective strength >= 1)
//     or its current level is above kAlarmSourceThreshold,
//   * a source RAISES the level of same-role neighbours (matched on
//     CreatureComponent.is_predator) within kAlarmRadius, scaled by CLOSENESS (full at the
//     source, falling linearly to 0 at the radius edge) and by the source's strength,
//   * a carcass (CreatureComponent.eaten) neither emits nor receives (the dead don't warn),
//   * every creature's level DECAYS multiplicatively toward 0 each tick; the propagated
//     contribution is then folded in, so a level only persists while a source is in range.
//
// CreatureBrainSystem consumes the propagated level when evaluating flee
// behaviour; this system maintains that field.
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral). TWO-PHASE so the result is
// order-INDEPENDENT:  SNAPSHOTS every participant's pre-tick level/role/position;
//  computes each creature's new level from that frozen snapshot (decay of its own
// snapshot level + the max-closeness contribution from snapshot sources) and writes it.
// Because every read is from the snapshot, no creature's update can see another creature's
// in-this-tick update, so iteration order cannot change the outcome. Math is
// DeterministicMath only (Sqrt for distance; +-*/, clamped to [0,1]); NO rng is consumed
// (propagation is fully deterministic), NO wall-clock, NO libm transcendentals. run==replay
// holds. Seed offset +26 is reserved for collision-avoidance only and never drawn.
//
// GATING. A creature only participates if it carries an AlarmComponent (the opt-in). A world
// whose creatures carry none — and any world with no creatures — does nothing, so the
// canonical NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <entt/entt.hpp>

#include "../components/AlarmComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../core/DeterministicMath.h"
#include "../simulation/DeterministicUniformGrid.h"
#include "CreatureTickAdmission.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry:... pollination+19, disease+20, irrigation+21,
// lifespan+22, wildlife+23, photo+24, herd-alarm+26). This system is deterministic and
// consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kHerdAlarmSeedOffset = 26ull;

// Distance (world units) within which an alarmed creature warns same-role neighbours.
inline constexpr float kAlarmRadius = 12.0f;

// A creature whose accumulated level is at/above this acts as a propagating SOURCE (so the
// alarm relays outward in  through a chain of creatures, not just from the originator).
inline constexpr float kAlarmSourceThreshold = 0.25f;

// Per-tick retention of a creature's own level when no source is nearby: new_self = level *
// kAlarmDecay. ~0.85 fades a full alarm to near-zero over ~1s @30Hz once the threat passes.
inline constexpr float kAlarmDecay = 0.85f;

// Fraction of a source's strength that transfers to a neighbour AT zero distance (it falls
// linearly to 0 at kAlarmRadius). Keeps a relayed creature slightly below its source so the
// chain forms a decreasing gradient outward rather than a flat plateau.
inline constexpr float kAlarmTransfer = 0.9f;

struct HerdAlarmStats {
    int participants = 0; // creatures carrying an AlarmComponent that took part
    int sources = 0;      // creatures acting as an alarm source this tick
    int raised = 0;       // creatures whose level INCREASED this tick (propagation reached)
};

[[nodiscard]] inline float AlarmClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

namespace herd_alarm_detail {

// Transient registry scratch, neither saved nor hashed. Retain index capacities
// across regional calls once the admitted roster fits.
struct SpatialScratch {
    ::luminumbra::simulation::DeterministicUniformGrid grid;
    std::vector<::luminumbra::simulation::DeterministicUniformGrid::Point> points;
};

} // namespace herd_alarm_detail

// RunHerdAlarmOnTick: maintain the propagating alarm field. id-ordered, two-phase snapshot.
// `dt` scales the decay/propagation rate so the field is frame-rate independent (the sim
// runs a fixed dt; the parameter keeps the signature honest and lets tests exercise decay).
// Elapsed opt-in samples geometry/role/alarmed at due time and repeats this exact float step,
// never dt * elapsed. Windows end at the same due tick: shorter windows (including newborns)
// join only for their suffix; zero/denied windows neither emit nor receive. As in foliage
// replay, reject windows above 65,536 before any writes. Stats count distinct participants,
// initial sources and participants raised at least once. Legacy callers retain one step.
inline HerdAlarmStats RunHerdAlarmOnTick(entt::registry& reg,
                                         float dt,
                                         const CreatureTickAdmission* admission = nullptr,
                                         bool use_elapsed = false) {
    HerdAlarmStats stats;

    auto view = reg.view<Comp::AlarmComponent, Comp::CreatureComponent, Comp::TransformComponent>();

    // Only admitted alarm participants are staged; storage is bounded by the EnTT entity
    // mask, independent of nonparticipants and completed decomposition history.
    struct Snap {
        entt::entity e;
        float x, z;
        float level;
        bool predator;
        bool dead;
        bool alarmed;
        std::uint32_t ticks;
        float next;
        float src_strength;
        bool raised;
    };
    const auto elapsedFor = [&](entt::entity entity) {
        if (admission && !admission->Allows(entity)) {
            return 0u;
        }
        return use_elapsed && admission ? admission->ElapsedTicks(entity) : 1u;
    };
    std::size_t participant_count = 0;
    std::uint32_t steps = 0;
    for (auto entity : view) {
        const auto ticks = elapsedFor(entity);
        if (use_elapsed && ticks > 65536u) {
            throw std::invalid_argument("Unbounded alarm window");
        }
        if (ticks != 0) {
            ++participant_count;
            steps = std::max(steps, ticks);
        }
    }
    if (participant_count == 0) {
        return stats;
    }
    // Validate first, then allocate exactly the admitted count. reserve checks max_size;
    // no byte-size multiplication or storage proportional to denied owners is needed.
    std::vector<Snap> snap;
    snap.reserve(participant_count);
    for (auto e : view) {
        const auto ticks = elapsedFor(e);
        if (ticks == 0) {
            continue;
        }
        const auto& al = view.get<Comp::AlarmComponent>(e);
        const auto& cr = view.get<Comp::CreatureComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const float level = AlarmClamp01(al.level);
        const bool dead = cr.eaten;
        const float src =
            dead ? 0.0f
                 : (al.alarmed != 0 ? 1.0f : (level >= kAlarmSourceThreshold ? level : 0.0f));
        snap.push_back({e,
                        tf.position.x,
                        tf.position.z,
                        level,
                        cr.is_predator,
                        dead,
                        al.alarmed != 0,
                        ticks,
                        level,
                        src,
                        false});
    }
    std::sort(snap.begin(), snap.end(), [](const Snap& a, const Snap& b) {
        return entt::to_integral(a.e) < entt::to_integral(b.e);
    });

    stats.participants = static_cast<int>(snap.size());
    for (const Snap& s : snap) {
        if (s.src_strength > 0.0f)
            ++stats.sources;
    }

    // dt-scaled rates. The sim's fixed dt (~1/30s) gives the tuned constants; we keep the
    // behaviour bounded for any dt by clamping the scaled fractions to [0,1].
    const float rate = dt * static_cast<float>(::Luminumbra::TICKS_PER_SECOND); // 1.0 at 30Hz
    // Decay applied this tick: level retains (kAlarmDecay) per 1/30s. Approximate the
    // per-dt retention by scaling the *loss* (1-kAlarmDecay) with rate, clamped so a large
    // dt can drain but never invert.
    float decay_loss = (1.0f - kAlarmDecay) * rate;
    decay_loss = AlarmClamp01(decay_loss);
    const float keep = 1.0f - decay_loss;

    // Due-time geometry is fixed for the replay. The integer broad phase preserves
    // identity order and exact pair math; unsupported coordinates use the original scan.
    // Storage is O(admitted participants), never O(pairs), and queries allocate nothing.
    herd_alarm_detail::SpatialScratch local_scratch;
    auto* scratch = &local_scratch;
    if (use_elapsed) {
        scratch = reg.ctx().find<herd_alarm_detail::SpatialScratch>();
        if (scratch == nullptr)
            scratch = &reg.ctx().emplace<herd_alarm_detail::SpatialScratch>();
    }
    auto& grid = scratch->grid;
    auto& points = scratch->points;
    points.clear();
    bool indexed = use_elapsed;
    if (indexed) {
        points.reserve(snap.size()); // At most one point per admitted participant.
        for (const auto& self : snap) {
            constexpr float coordinate_limit = 536870912.0f;
            if (!std::isfinite(self.x) || !std::isfinite(self.z) ||
                std::abs(self.x) > coordinate_limit || std::abs(self.z) > coordinate_limit) {
                indexed = false;
                break;
            }
            points.push_back({static_cast<std::int64_t>(std::floor(self.x)),
                              static_cast<std::int64_t>(std::floor(self.z))});
        }
        if (indexed)
            grid.Build(points, 12);
    }

    // At most 65,536 substeps, each visiting only the admitted roster and spatial
    // candidates (at worst N*N). Bit-identical fixed points skip to the next join.
    const float invR = kAlarmRadius > 0.0f ? (1.0f / kAlarmRadius) : 0.0f;
    for (std::uint32_t remaining = steps; remaining != 0; --remaining) {
        std::uint32_t next_join = 0;
        for (auto& self : snap) {
            if (self.ticks < remaining)
                next_join = std::max(next_join, self.ticks);
            self.src_strength =
                self.ticks < remaining || self.dead
                    ? 0.0f
                    : (self.alarmed ? 1.0f
                                    : (self.level >= kAlarmSourceThreshold ? self.level : 0.0f));
        }
        for (std::size_t i = 0; i < snap.size(); ++i) {
            auto& self = snap[i];
            if (self.ticks < remaining) {
                continue;
            }
            if (self.dead) {
                self.next = 0.0f;
                continue;
            }
            float next = self.level * keep;
            if (self.alarmed) {
                next = 1.0f;
            }
            float best_in = 0.0f;
            std::span<const std::size_t> candidates;
            if (indexed) {
                // One metre covers projection and float rounding at the radius.
                static_assert(kAlarmRadius == 12.0f, "Update alarm query extents with radius");
                const auto center = points[i];
                candidates =
                    grid.Query({center.x - 13, center.z - 13}, {center.x + 13, center.z + 13});
            }
            const auto count = indexed ? candidates.size() : snap.size();
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                const auto j = indexed ? candidates[candidate] : candidate;
                if (j == i)
                    continue;
                const Snap& other = snap[j];
                if (other.dead || other.src_strength <= 0.0f || other.predator != self.predator)
                    continue;
                const float dx = other.x - self.x;
                const float dz = other.z - self.z;
                const float distance = dm::Sqrt(dx * dx + dz * dz);
                if (distance >= kAlarmRadius)
                    continue;
                const float closeness = 1.0f - distance * invR;
                const float contrib = other.src_strength * kAlarmTransfer * closeness * rate;
                if (contrib > best_in)
                    best_in = contrib;
            }
            if (best_in > next)
                next = best_in;
            self.next = AlarmClamp01(next);
            self.raised = self.raised || self.next > self.level + 1.0e-6f;
        }
        // Publish only into the scratch snapshot until every listener has read the same
        // pre-step field. No partial registry writes, even for invalid elapsed input.
        bool unchanged = true;
        for (auto& self : snap) {
            if (self.ticks >= remaining) {
                unchanged = unchanged && std::bit_cast<std::uint32_t>(self.level) ==
                                             std::bit_cast<std::uint32_t>(self.next);
                self.level = self.next;
            }
        }
        if (use_elapsed && unchanged) {
            // No state, source strength or raised flag can change until another
            // suffix window joins. Compare bits so signed zero is preserved too.
            if (next_join == 0)
                break;
            remaining = next_join + 1u;
        }
    }
    for (const auto& self : snap) {
        view.get<Comp::AlarmComponent>(self.e).level = self.level;
        if (self.raised) {
            ++stats.raised;
        }
    }

    return stats;
}

} // namespace luminumbra::ai
