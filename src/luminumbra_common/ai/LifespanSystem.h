#pragma once

// sim.lifespan: creatures (and any opted-in entity) AGE and DIE. This is
// the closing half of the ecology life-cycle: CreatureReproductionSystem makes
// BIRTHS, this makes DEATHS, so the population is bounded rather than exploding.
//
// MODEL: every entity carrying a MortalComponent (components/MortalComponents.h)
// is the per-entity opt-in. Each tick the system, in ASCENDING entity-id order:
//   * ages the entity (age_ticks++ , saturating; never ages a dead entity), and
//   * MARKS it dead when EITHER
//       - it has reached its lifespan  (age_ticks >= lifespan_ticks)  [old age], OR
//       - it ALSO carries a CreatureComponent whose hunger has maxed out
//         (hunger >= 1.0)                                             [starvation].
//
// Marking dead sets MortalComponent.dead = 1 and, when a CreatureComponent is
// present, ALSO sets CreatureComponent.eaten = true so the existing brain /
// floating-id marker treat the corpse as an inert carcass (it stops deciding /
// moving and predators ignore it) — reusing the carcass seam rather than adding
// a new one. The system does NOT reg.destroy anything: the orchestrator owns
// removal/cleanup later (destroying here could desync the avatar pool).
//
// DETERMINISM (this feeds world_hash once wired): id-ordered traversal; all
// state is integer (age/lifespan are uint32 ticks, dead is a flag); the only
// float read is CreatureComponent.hunger against a constant threshold (a pure
// compare, no accumulation in mortal sim truth). NO rng (death here is a
// DETERMINISTIC threshold, so the reserved seed offset +22 is recorded for
// collision-avoidance but intentionally never consumed), NO wall-clock, NO libm
// transcendentals. run==replay holds.
//
// GATING: a registry with NO MortalComponent => RunLifespanOnTick performs zero
// work and touches nothing, so the canonical NetworkStateHash baseline stays
// byte-identical until mortal entities are deliberately spawned.
//
// SEED OFFSET REGISTRY: wind+11, weather+12/13, aether+14, plant+15,
// creature-repro+16, fire+17, soil+18, pollination+19, disease+20 taken;
// LIFESPAN CLAIMS +22 (reserved; unused — death is a deterministic threshold).

#include "CreatureTickAdmission.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CreatureComponents.h" // CreatureComponent (starvation + carcass seam)
#include "../components/MortalComponents.h"   // MortalComponent (the opt-in)

#include "../simulation/SimulationIdentity.h"
#include "../systems/RegionConsumerGate.h"

namespace luminumbra::ai {

// Components live in the capital-L namespace (engine convention).
namespace Comp = ::Luminumbra::Components;

// Explicit candidate-path selection. Shipping registries do not install this context until
// the authority transition. Creature IDs and these tick-local records are never persisted.
// Kept with the lifecycle consumer so its ordered publications share the traversal contract.
class SimulationTraversalContext {
public:
    struct DecayEvent {
        std::uint64_t id;
        std::uint32_t released_milli;
        bool finished;
    };

    // Reuse storage across the six traversal consumers. Only their participating view is
    // staged; unrelated residents are never scanned. The returned buffer is tick-local.
    std::vector<entt::entity>& BeginParticipants() {
        m_entities.clear();
        return m_entities;
    }

    // Validate the entire candidate roster before any simulation writes or publication.
    // Eight byte passes bound ordering to O(participants + 256), independent of ID magnitude.
    // Vector max_size bounds each allocation; counts/prefixes never exceed the roster size.
    void OrderParticipants(const entt::registry& registry) {
        m_participantVisits = 0;
        m_records.clear();
        m_records.reserve(m_entities.size());
        for (const auto entity : m_entities) {
            ++m_participantVisits;
            const auto* identity = registry.try_get<Comp::SimulationIdentityComponent>(entity);
            if (!identity || !::Luminumbra::simulation::IsValidSimulationId(identity->id))
                throw std::logic_error("Invalid simulation traversal identity");
            m_records.push_back(Record{.id = identity->id, .entity = entity});
        }
        if (m_records.size() < 2)
            return;
        m_radix.resize(m_records.size());
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::array<std::size_t, 256> offsets{};
            for (const auto& record : m_records)
                ++offsets[static_cast<std::size_t>((record.id >> shift) & 255u)];
            std::size_t prefix = 0;
            for (auto& offset : offsets) {
                const auto count = offset;
                offset = prefix;
                prefix += count;
            }
            for (const auto& record : m_records)
                m_radix[offsets[static_cast<std::size_t>((record.id >> shift) & 255u)]++] = record;
            m_records.swap(m_radix);
        }
        for (std::size_t index = 1; index < m_records.size(); ++index) {
            if (m_records[index - 1].id == m_records[index].id)
                throw std::logic_error("Duplicate simulation traversal identity");
        }
        for (std::size_t index = 0; index < m_records.size(); ++index)
            m_entities[index] = m_records[index].entity;
    }

    void BeginDeaths(std::size_t participants) {
        m_deaths.reserve(participants);
        m_deaths.clear();
    }
    void BeginDecay(std::size_t participants) {
        m_decay.reserve(participants);
        m_decay.clear();
    }
    // BeginDeaths/BeginDecay reserve at most one event per participant before state changes.
    void PublishDeath(std::uint64_t id) {
        m_deaths.push_back(id);
    }
    void PublishDecay(const DecayEvent& event) {
        m_decay.push_back(event);
    }
    [[nodiscard]] std::span<const std::uint64_t> deaths() const {
        return m_deaths;
    }
    [[nodiscard]] std::span<const DecayEvent> decay() const {
        return m_decay;
    }
    // Const diagnostic for the participant-only traversal cost contract.
    [[nodiscard]] std::size_t participant_visits() const {
        return m_participantVisits;
    }

private:
    struct Record {
        std::uint64_t id;
        entt::entity entity;
    };
    std::vector<entt::entity> m_entities;
    std::vector<Record> m_records;
    std::vector<Record> m_radix;
    std::vector<std::uint64_t> m_deaths;
    std::vector<DecayEvent> m_decay;
    std::size_t m_participantVisits = 0;
};

// Reserved seed-stream offset for the lifespan track. Death here is a pure
// DETERMINISTIC threshold (no stochastic culling), so this is recorded for
// collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kLifespanSeedOffset = 22ull;

// Hunger at/above this is fatal starvation (hunger is 0 sated .. 1 starving).
inline constexpr float kStarvationHunger = 1.0f;

// Telemetry / sub-hash for the lifespan tick.
struct LifespanStats {
    int aged = 0; // mortal entities advanced this tick (alive before the step)
    int died = 0; // entities newly MARKED dead this tick (old age OR starvation)
};

// Advance the lifespan of every opted-in entity by ONE fixed tick. Pure function
// of registry state. id-ordered so the death-marking order (and thus any derived
// telemetry) is deterministic. No creation/destruction -> no two-phase needed,
// no view invalidation. Safe to call every tick; a no-op when no entity opts in.
inline LifespanStats RunLifespanOnTick(entt::registry& reg,
                                       std::uint64_t /*tick*/ = 0,
                                       const ::Luminumbra::world::RegionConsumerGate& gate = {},
                                       const CreatureTickAdmission* admission = nullptr) {
    LifespanStats stats;

    auto view = reg.view<Comp::MortalComponent>();

    // id-ordered traversal so death-marking is deterministic / order-stable.
    auto* traversal = reg.ctx().find<SimulationTraversalContext>();
    std::vector<entt::entity> legacy_ents;
    auto& ents = traversal ? traversal->BeginParticipants() : legacy_ents;
    for (auto e : view) {
        // Creature decisions survive movement across an owner boundary within this tick.
        const bool allowed = admission && reg.all_of<Comp::CreatureComponent>(e)
                                 ? admission->Allows(e)
                                 : gate.Allows(e);
        if (allowed) {
            ents.push_back(e);
        }
    }
    if (traversal) {
        traversal->OrderParticipants(reg);
        traversal->BeginDeaths(ents.size());
    } else {
        std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
    }

    for (auto e : ents) {
        auto& mortal = view.get<Comp::MortalComponent>(e);

        // A latched-dead entity is inert: it neither ages nor dies again, so its
        // age and death flag stay stable across run==replay.
        if (mortal.dead != 0) {
            continue;
        }

        ++stats.aged;

        // Age (saturating so a long-lived entity can't wrap its clock).
        if (mortal.age_ticks < 0xFFFFFFFFu) {
            ++mortal.age_ticks;
        }

        // Old-age death: reaching lifespan is fatal.
        bool dies = mortal.age_ticks >= mortal.lifespan_ticks;

        // Starvation death: only if this entity is also a creature (the brain's
        // hunger model lives there). Reading hunger >= 1.0 as "maxed out".
        if (!dies) {
            if (auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
                if (cr->hunger >= kStarvationHunger) {
                    dies = true;
                }
            }
        }

        if (dies) {
            mortal.dead = 1;
            // Reuse the existing carcass seam so the brain / markers treat the
            // corpse as inert (stops deciding/moving; predators ignore it).
            if (auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
                cr->eaten = true;
            }
            ++stats.died;
            if (traversal)
                traversal->PublishDeath(reg.get<Comp::SimulationIdentityComponent>(e).id);
        }
    }

    return stats;
}

} // namespace luminumbra::ai
