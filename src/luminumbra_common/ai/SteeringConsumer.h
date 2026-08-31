#pragma once

//  the STEERING CONSUMER — blends the  bias systems' outputs into the creature wish
// velocity before the physics bridge applies it. This is the INTEGRATION layer between the bias
// producers (PredatorPackSystem / MigrationSystem / TerritorySystem, which run in slot 7 and
// write their component each tick) and the locomotion (which reads CreatureComponent.wish_x/z).
//
// Extracted from GameSession::TickSimulation so it is UNIT-TESTABLE on its own (a producer's
// per-system test can't catch a consumer that mis-reads the producer's output -- e.g. treating a
// unit STEER DIRECTION as a world POINT). Each creature only writes its OWN wish, so the result
// is independent of traversal order; pure registry mutation, run==replay, gated by the optional
// components (no component -> untouched).

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/MigratoryComponents.h"
#include "../components/PackHunterComponents.h"
#include "../components/TerritoryComponents.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;

struct SteeringConsumerStats {
    int packed = 0;   // predators steered by a pack flank direction
    int migrated = 0; // creatures given a migration drift
    int homed = 0;    // creatures given a territory homing bias
};

inline SteeringConsumerStats RunSteeringConsumerOnTick(entt::registry& reg) {
    SteeringConsumerStats stats;
    auto sv = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();
    for (auto e : sv) {
        auto& cr = sv.get<Comp::CreatureComponent>(e);
        if (cr.eaten)
            continue; // a carcass is inert

        // Pack flanking OVERRIDES the predator's wish with its flank-steer DIRECTION.
        // PackHunterComponent.coord_x/z is a UNIT vector toward the flank approach point (NOT a
        // world position), so it is used directly as the heading.
        if (cr.is_predator) {
            if (auto* pk = reg.try_get<Comp::PackHunterComponent>(e);
                pk && pk->in_pack && (pk->coord_x != 0.0f || pk->coord_z != 0.0f)) {
                const float sp = cr.move_speed * 1.5f;
                cr.wish_x = pk->coord_x * sp;
                cr.wish_z = pk->coord_z * sp;
                ++stats.packed;
            }
        }
        // Migration + territory ADD a gentle drift on top (their wishes are velocity-ish vectors).
        if (auto* mig = reg.try_get<Comp::MigratoryComponent>(e)) {
            cr.wish_x += mig->wish_x;
            cr.wish_z += mig->wish_z;
            ++stats.migrated;
        }
        if (auto* tb = reg.try_get<Comp::TerritoryBiasComponent>(e)) {
            cr.wish_x += tb->wish_x;
            cr.wish_z += tb->wish_z;
            ++stats.homed;
        }
    }
    return stats;
}

} // namespace luminumbra::ai
