#pragma once

// PerceptionSystem — the live ECS read-side of the senses. For every
// perceiver (Transform + PerceptionComponent + AwarenessComponent) it fuses VISION
// (forward cone + range) and HEARING (audiogram) over the SensableComponent
// entities of OTHER factions, picks the strongest sensed target, and advances the
// per-agent awareness meter/state + last-known memory (see Awareness.h). Smell is
// handled field-wise by the ScentField/locomotion gradient path, not here.
//
// PURE-ENOUGH + DETERMINISTIC: perceivers and sensables are visited in entity-id
// order; float-only + DeterministicMath::Sqrt; no RNG/wall-clock. World_hash-
// neutral by construction — it only touches entities that carry these components,
// and the canonical headless determinism roster carries none (terrain/water only),
// so wiring it into the tick does not move world_hash.

#include "CreatureTickAdmission.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "entt/entt.hpp"

#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"
#include "../core/DeterministicMath.h"
#include "../simulation/SimulationIdentity.h"
#include "Awareness.h"
#include "Perception.h"

namespace luminumbra::ai {

// Candidate v2 ordering for perception, instinct and locomotion. Shipping sessions
// do not install this registry context until authority activation. Identity alone
// never selects it. Scratch belongs to the registry, neither saved nor hashed.
class PerceptionIdentityOrder {
public:
    // Validate and order only the admitted participants supplied by the caller.
    // Eight byte passes bound ordering to O(N); storage is reused across calls.
    // Invalid or duplicate IDs throw before any participant state is updated.
    template<typename Entry, typename EntityOf>
    void Sort(const entt::registry& registry, std::vector<Entry>& entries, EntityOf entity_of) {
        keys_.clear();
        visits_ = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            ++visits_;
            const auto* identity =
                registry.try_get<const Luminumbra::Components::SimulationIdentityComponent>(
                    entity_of(entries[index]));
            if (identity == nullptr || !Luminumbra::simulation::IsValidSimulationId(identity->id)) {
                throw std::logic_error(
                    "Perception participant requires a valid simulation identity");
            }
            keys_.push_back({identity->id, index});
        }
        scratch_.resize(keys_.size());
        for (unsigned shift = 0; shift < 64; shift += 8) {
            // Exactly 256 buckets for one identity byte, independent of roster size.
            std::array<std::size_t, 256> offsets{};
            for (const auto& key : keys_) {
                ++offsets[static_cast<std::size_t>((key.id >> shift) & 255u)];
            }
            std::size_t offset = 0;
            for (auto& bucket : offsets) {
                const auto count = bucket;
                bucket = offset;
                offset += count;
            }
            for (const auto& key : keys_) {
                scratch_[offsets[static_cast<std::size_t>((key.id >> shift) & 255u)]++] = key;
            }
            keys_.swap(scratch_);
        }
        for (std::size_t index = 1; index < keys_.size(); ++index) {
            if (keys_[index - 1].id == keys_[index].id) {
                throw std::logic_error("Duplicate perception participant simulation identity");
            }
        }
        // Invert the sorted permutation, then apply disjoint cycles in place.
        // Each swap places one entry; the inner loop performs at most N swaps total.
        for (std::size_t index = 0; index < keys_.size(); ++index) {
            scratch_[keys_[index].index].index = index;
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            while (scratch_[index].index != index) {
                const auto destination = scratch_[index].index;
                std::swap(entries[index], entries[destination]);
                std::swap(scratch_[index], scratch_[destination]);
            }
        }
    }

    // Const diagnostic: identity lookups in the latest participant gather.
    [[nodiscard]] std::size_t participant_visits() const noexcept {
        return visits_;
    }

private:
    struct Key {
        std::uint64_t id;
        std::size_t index;
    };
    std::vector<Key> keys_;
    std::vector<Key> scratch_;
    std::size_t visits_ = 0;
};

struct PerceptionTickStats {
    std::uint64_t perceivers = 0; // agents with perception+awareness considered
    std::uint64_t sensing = 0;    // produced a non-zero fused signal this tick
    std::uint64_t engaged = 0;    // reached the Engaged state
};

inline PerceptionTickStats RunPerceptionSystemOnTick(
    entt::registry& registry, float dt, const CreatureTickAdmission* admission = nullptr) {
    using Luminumbra::Components::AwarenessComponent;
    using Luminumbra::Components::PerceptionComponent;
    using Luminumbra::Components::SensableComponent;
    using Luminumbra::Components::TransformComponent;

    PerceptionTickStats stats;
    auto* identity_order = registry.ctx().find<PerceptionIdentityOrder>();

    // Snapshot sensables in id order (deterministic scan; independent of storage).
    struct Sensable {
        entt::entity id;
        float x;
        float z;
        float loudness;
        float pitch;
        std::uint32_t faction;
    };
    std::vector<Sensable> sensables;
    {
        auto view = registry.view<const TransformComponent, const SensableComponent>();
        for (auto e : view) {
            if (admission && !admission->Allows(e)) {
                continue;
            }
            const auto& tf = view.get<const TransformComponent>(e);
            const auto& s = view.get<const SensableComponent>(e);
            sensables.push_back(
                {e, tf.position.x, tf.position.z, s.noise_loudness, s.noise_pitch, s.faction});
        }
        if (identity_order != nullptr) {
            identity_order->Sort(
                registry, sensables, [](const Sensable& source) { return source.id; });
        } else {
            std::sort(sensables.begin(), sensables.end(), [](const Sensable& a, const Sensable& b) {
                return entt::to_integral(a.id) < entt::to_integral(b.id);
            });
        }
    }

    std::vector<entt::entity> perceivers;
    {
        auto view =
            registry
                .view<const TransformComponent, const PerceptionComponent, AwarenessComponent>();
        for (auto e : view) {
            if (!admission || admission->Allows(e)) {
                perceivers.push_back(e);
            }
        }
        if (identity_order != nullptr) {
            identity_order->Sort(registry, perceivers, [](entt::entity entity) { return entity; });
        } else {
            std::sort(perceivers.begin(), perceivers.end(), [](entt::entity a, entt::entity b) {
                return entt::to_integral(a) < entt::to_integral(b);
            });
        }
    }

    for (auto e : perceivers) {
        ++stats.perceivers;
        const auto& tf = registry.get<const TransformComponent>(e);
        const auto& perc = registry.get<const PerceptionComponent>(e);
        auto& aw = registry.get<AwarenessComponent>(e);

        float best_signal = 0.0f;
        float bx = 0.0f;
        float bz = 0.0f;
        for (const auto& s : sensables) {
            if (s.id == e || s.faction == perc.faction)
                continue; // self / same faction ignored
            const float dx = s.x - tf.position.x;
            const float dz = s.z - tf.position.z;
            const float dist = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

            // Vision: in-cone clarity falls off with range.
            float vis = 0.0f;
            if (InVisionCone(tf.position.x,
                             tf.position.z,
                             perc.facing_x,
                             perc.facing_z,
                             perc.vision_cos_half_fov,
                             perc.vision_range,
                             s.x,
                             s.z)) {
                vis = 1.0f - dist / perc.vision_range;
                if (vis < 0.0f)
                    vis = 0.0f;
            }
            // Hearing: audiogram response, clamped to a [0,1] signal contribution.
            float hear = HeardLoudness(dist, s.loudness, s.pitch, perc.ear);
            if (hear > 1.0f)
                hear = 1.0f;

            const float sig = vis > hear ? vis : hear; // strongest sense wins
            if (sig > best_signal) {
                best_signal = sig;
                bx = s.x;
                bz = s.z;
            }
        }

        if (best_signal > 0.0f)
            ++stats.sensing;
        aw.awareness = UpdateAwareness(aw.awareness, best_signal, bx, bz, dt, aw.params);
        if (aw.awareness.state == AwarenessState::Engaged)
            ++stats.engaged;
    }

    return stats;
}

} // namespace luminumbra::ai
