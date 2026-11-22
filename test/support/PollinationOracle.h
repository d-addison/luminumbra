#pragma once

#include "systems/PollinationSystem.h"

#include <cstdint>
#include <limits>

namespace Luminumbra::TestSupport::PollinationOracle {
namespace F = luminumbra::foliage;
namespace C = Luminumbra::Components;
using Gate = Luminumbra::world::RegionConsumerGate;

// Independent original pair arithmetic: normalization deliberately stays inside
// the pair loop, so the oracle detects rounding or hoisting mistakes.
inline float BruteEffectiveDistance(const Luminumbra::Vec3& donor,
                                    const Luminumbra::Vec3& receiver,
                                    const Luminumbra::Vec2& wind) {
    const float dx = receiver.x - donor.x;
    const float dz = receiver.z - donor.z;
    const float distance = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);
    const float length = Luminumbra::DeterministicMath::Sqrt(wind.x * wind.x + wind.y * wind.y);
    if (length <= 0.0f) {
        return distance;
    }
    const float wx = wind.x / length;
    const float wz = wind.y / length;
    const float along = dx * wx + dz * wz;
    const float effective = distance - F::kPollinationWindReach * along;
    return effective < 0.0f ? 0.0f : effective;
}

inline F::PollinationStats BrutePollination(entt::registry& registry,
                                            std::uint64_t tick,
                                            const Luminumbra::Vec2& wind,
                                            const Gate& gate) {
    F::PollinationStats stats;
    const auto participants = F::CollectPollinationParticipants(registry, gate);
    const auto identity = [&](entt::entity entity) {
        if (gate.UsesSimulationPlantIdentity()) {
            return F::RequirePlantSimulationId(registry, entity);
        }
        return gate.UsesPersistentPlantIdentity()
                   ? F::PlantSimulationId(registry, entity)
                   : static_cast<std::uint64_t>(entt::to_integral(entity));
    };
    for (const auto receiver : participants) {
        ++stats.considered;
        if (F::IsPollenDonor(registry.get<C::PlantGrowthComponent>(receiver))) {
            ++stats.donors;
        }
        entt::entity best = entt::null;
        float best_distance = F::kPollinationRadius;
        for (const auto donor : participants) {
            ++stats.candidate_tests;
            if (receiver == donor ||
                !F::IsPollenDonor(registry.get<C::PlantGrowthComponent>(donor))) {
                continue;
            }
            const float distance =
                BruteEffectiveDistance(registry.get<C::TransformComponent>(donor).position,
                                       registry.get<C::TransformComponent>(receiver).position,
                                       wind);
            if (distance > F::kPollinationRadius) {
                continue;
            }
            if (best == entt::null || distance < best_distance) {
                best = donor;
                best_distance = distance;
            }
        }
        if (best == entt::null) {
            continue;
        }
        auto& pollen = registry.get<C::PollinationComponent>(receiver);
        pollen.next_genome = F::PollinateCross(registry.get<C::PlantGenomeComponent>(receiver),
                                               registry.get<C::PlantGenomeComponent>(best),
                                               identity(receiver),
                                               identity(best),
                                               tick,
                                               99);
        pollen.pollinated = true;
        pollen.last_pollen_tick = tick;
        if (pollen.crosses < std::numeric_limits<std::uint32_t>::max()) {
            ++pollen.crosses;
        }
        ++stats.crossed;
    }
    return stats;
}

} // namespace Luminumbra::TestSupport::PollinationOracle
