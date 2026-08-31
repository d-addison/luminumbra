#pragma once

// gate-populated-world-replay: the id-ordered ECOLOGY sub-hash.
//
// This is the sim-truth checksum over the live creature roster that the
// PopulatedWorldReplay gate (and the canonical world_hash, approach a) fold in
// as the 6th term. It mirrors the gtest projection in
// test/sim/ecology_pipeline_test.cpp:84-106 (the `Snapshot` function) field-for-
// field and in the SAME order, so the binary/gate and the gtest agree on exactly
// what "creature state" means: per id-sorted creature -- position(3),
// wish-velocity(2), hunger, stamina, eaten flag, then (when present) genome
// (move_speed, generation, age_ticks), alarm level, and pack state (coord_x,
// coord_z, in_pack).
//
// NEUTRALITY (additivity guard, architecture contract): an EMPTY roster (no
// CreatureComponent) hashes to the empty string -- byte-identical to the scent
// sub-hash's "empty when no participant opted in" contract
// (GameSession::ComputeScentSubHash). This is what lets ComposeWorldHash append
// `|ecology:` and leave the five existing sub-terms byte-identical: at the gate
// default (empty roster) the appended value is empty, so the composite differs
// from the pre-fold composite ONLY by the literal `|ecology:` suffix.
//
// DETERMINISM: id-ordered traversal (entity ids sorted ascending), so
// the hash is independent of registry/view iteration order; no RNG, no wall
// clock, no libm transcendentals -- it only READS clamped sim floats and emits a
// canonical text the StableChecksum (fnv1a_64_stable_json) folds. Pure function
// of the registry state => byte-exact across re-runs.

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "components/AlarmComponents.h"
#include "components/CircadianComponents.h" //  v2: circadian activity
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/PackHunterComponents.h"
#include "components/ThirstComponents.h"           //  v2: thirst state
#include "persistence/WorldPersistenceRoundtrip.h" // Persistence::StableChecksum

namespace luminumbra::ai {

// Computes the id-ordered ecology sub-hash over every CreatureComponent-bearing
// entity in `registry`. Returns the EMPTY string when the roster carries no
// creatures (additivity-neutral; mirrors ComputeScentSubHash).
inline std::string ComputeEcologySubHash(const entt::registry& registry) {
    namespace Comp = ::Luminumbra::Components;

    // id-sorted creature set -> stable order independent of view iteration order
    // (exactly the gtest Snapshot's sort key: entt::to_integral ascending).
    auto view = registry.view<const Comp::CreatureComponent>();
    std::vector<entt::entity> es(view.begin(), view.end());
    if (es.empty()) {
        return {}; // additivity-neutral: empty roster == empty sub-hash
    }
    std::sort(es.begin(), es.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Canonical text in the same field set + order as the gtest Snapshot. Full
    // float precision (17 sig digits) so no projected bit is lost before the
    // StableChecksum folds it. Per-component presence is encoded so a creature
    // with a genome and one without are unambiguously distinguished.
    // sub-hash v2 — energy, species_id, the heritable
    // SENSORY genes, thirst, and circadian activity join the projection.
    // Divergence in any of these was previously invisible to the oracle until it
    // flipped an action; now it localizes under `ecology` directly. The version
    // literal flips v1 -> v2 (one deliberate populated-golden transition); the
    // empty roster still returns "" (additivity-neutral, canonical untouched).
    std::ostringstream bytes;
    bytes << "ecology:v2:" << es.size() << ':' << std::setprecision(17);
    for (auto e : es) {
        const auto& tf = registry.get<Comp::TransformComponent>(e);
        const auto& cr = registry.get<Comp::CreatureComponent>(e);
        bytes << tf.position.x << ',' << tf.position.y << ',' << tf.position.z << ',' << cr.wish_x
              << ',' << cr.wish_z << ',' << cr.hunger << ',' << cr.stamina << ','
              << static_cast<int>(cr.eaten) << ',' << cr.energy << ',' << cr.species_id;
        if (const auto* gn = registry.try_get<Comp::CreatureGenomeComponent>(e)) {
            bytes << ",g:" << gn->move_speed << ',' << gn->generation << ',' << gn->age_ticks << ','
                  << gn->vision_range << ',' << gn->hearing_range << ',' << gn->vision_cos_half_fov;
        }
        if (const auto* al = registry.try_get<Comp::AlarmComponent>(e)) {
            bytes << ",a:" << al->level;
        }
        if (const auto* pk = registry.try_get<Comp::PackHunterComponent>(e)) {
            bytes << ",p:" << pk->coord_x << ',' << pk->coord_z << ','
                  << static_cast<int>(pk->in_pack);
        }
        if (const auto* th = registry.try_get<Comp::ThirstComponent>(e)) {
            bytes << ",t:" << th->thirst << ',' << static_cast<int>(th->drinking);
        }
        if (const auto* cc = registry.try_get<Comp::CircadianComponent>(e)) {
            bytes << ",c:" << cc->activity << ',' << static_cast<int>(cc->nocturnal);
        }
        bytes << ';';
    }
    return ::Luminumbra::Persistence::StableChecksum(bytes.str());
}

} // namespace luminumbra::ai
