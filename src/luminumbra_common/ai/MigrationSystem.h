#pragma once

// sim.migration: SEASONAL MIGRATION toward a MOVING target (a deterministic seasonal
// drive). Each migratory creature is pulled toward a migration target that travels a closed
// path around the world over the course of a year, and the urge to follow it ("drive") PEAKS at
// the season transitions (the spring + autumn migrations) and is ~0 mid-season. The result is
// written per-creature as a wish DIRECTION scaled by the drive; the orchestrator blends that
// wish into the creature's actual movement (this system never moves an entity itself).
//
// SEASONAL MODEL. season01 in [0,1] is a plain INPUT (0 = spring.. 1 = winter, wrapping): the
// year is a circle. The caller advances it deterministically (e.g. tick / ticks-per-year); we
// treat it purely as a phase so run==replay is the caller's to guarantee for the phase and ours
// for everything derived from it.
//
//   * TARGET PATH: a large ellipse in world XZ parameterized by the year angle
//     theta = 2*pi*season01, target = (cx + Rx*Cos(theta), cz + Rz*Sin(theta)). Because Cos/Sin
//     are the libm-free DeterministicMath wrappers, the path is bit-stable; because it is a
//     closed loop, the target MOVES continuously over the year and a full year returns to start.
//
//   * DRIVE (transition-peaking): migrations happen WHEN the seasons turn, not at the solstices.
//     The four transition phases are season01 = 0, 0.25, 0.5, 0.75 (the quarter boundaries); the
//     drive peaks there and falls to ~0 at the quarter MIDPOINTS (0.125, 0.375,...). We build
//     this from |cos(4*pi*season01)| — a libm-free  is 1 at every transition (season01 =
//     0, 0.25, 0.5, 0.75) and 0 at every quarter midpoint (0.125, 0.375,...) — giving two strong
//     (spring/autumn) and two shoulder pushes per year, all
//     bounded to [0,1]. A small threshold floors the tiny mid-season residue to a clean 0 so a
//     settled creature emits an exactly-zero wish (and the gate stays crisp).
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral) so the write order is stable.
// season01 is a plain float input; the target + drive are pure functions of it. Math is
// DeterministicMath only (Cos/Sin for the path + the drive wave, Sqrt to normalise the wish
// direction); NO rng is consumed (the drive is a deterministic seasonal function, not stochastic
// seed offset +32 is reserved for collision-avoidance only and never drawn), NO wall-clock, NO
// libm transcendentals (no exp/log/pow). run==replay holds.
//
// GATING. A creature participates only if it carries a MigratoryComponent (the opt-in). A world
// whose creatures carry none — and any world with no creatures — does nothing, so the canonical
// NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/MigratoryComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry:... herd-alarm+26, migration+32). This system is
// deterministic and consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kMigrationSeedOffset = 32ull;

// Migration target path: a large ellipse centred on the world origin. The half-extents (world
// units) set how far the herds range over a year. Authored generously so the seasonal target
// clearly MOVES (different season01 -> visibly different target).
inline constexpr float kMigrationCenterX = 0.0f;
inline constexpr float kMigrationCenterZ = 0.0f;
inline constexpr float kMigrationRadiusX = 600.0f;
inline constexpr float kMigrationRadiusZ = 600.0f;

// Below this the seasonal drive is floored to exactly 0 (mid-season settle band) so a settled
// creature emits a clean zero wish and stays effectively gated.
inline constexpr float kMigrationDriveFloor = 0.05f;

struct MigrationTarget {
    float x = 0.0f;
    float z = 0.0f;
};

struct MigrationStats {
    int participants = 0;  // creatures carrying a MigratoryComponent that took part
    int driven = 0;        // creatures with a non-zero drive this tick (actively migrating)
    float target_x = 0.0f; // the seasonal target this tick (telemetry / test hook)
    float target_z = 0.0f;
    float drive = 0.0f; // the seasonal drive this tick (same for all participants)
};

[[nodiscard]] inline float MigrationClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// Wrap an arbitrary season phase into [0,1) deterministically (float floor via int truncation).
// Keeps the year a circle for callers that pass an unwrapped phase.
[[nodiscard]] inline float MigrationWrap01(float season01) {
    float s = season01 - static_cast<float>(static_cast<std::int64_t>(season01));
    if (s < 0.0f)
        s += 1.0f; // map negative phase into [0,1)
    return s;
}

// The seasonal migration TARGET for a given (wrapped) season phase. A point on the year-ellipse;
// theta sweeps a full turn over a year, so the target travels a closed loop and a full year
// returns to the start. Pure function of season01 (libm-free Cos/Sin).
[[nodiscard]] inline MigrationTarget MigrationTargetAt(float season01) {
    const float s = MigrationWrap01(season01);
    const float theta = dm::kTwoPi * s;
    MigrationTarget t;
    t.x = kMigrationCenterX + kMigrationRadiusX * dm::Cos(theta);
    t.z = kMigrationCenterZ + kMigrationRadiusZ * dm::Sin(theta);
    return t;
}

// The seasonal DRIVE for a given (wrapped) season phase: peaks at the four season transitions
// (season01 = 0, 0.25, 0.5, 0.75) and falls to ~0 at the quarter midpoints. Built from
// |cos(4*pi*season01)| (libm-free), then floored so the mid-season residue is a clean 0. In
// [0,1]. Pure function of season01.
[[nodiscard]] inline float MigrationDriveAt(float season01) {
    const float s = MigrationWrap01(season01);
    // |cos(4*pi*s)|: cos(4*pi*s) is +-1 at s = 0,0.25,0.5,0.75 (the four season TRANSITIONS) and
    // 0 at the quarter midpoints (0.125,0.375,...). Taking |.| gives a  PEAKS (==1) at
    // every transition and dips to 0 at every midpoint — exactly the spring/autumn-style pushes.
    float w = dm::Cos(dm::kTwoPi * 2.0f * s); // cos(4*pi*s): +-1 at transitions, 0 at midpoints
    if (w < 0.0f)
        w = -w; // |cos| -> [0,1], peaks (==1) AT the transitions
    w = MigrationClamp01(w);
    if (w < kMigrationDriveFloor)
        w = 0.0f; // floor the mid-season residue to a clean 0
    return w;
}

// RunMigrationOnTick: for each migratory creature, write a wish toward the current seasonal
// target scaled by the seasonal drive. id-ordered, deterministic. season01 is a plain input.
inline MigrationStats RunMigrationOnTick(entt::registry& reg, float season01) {
    MigrationStats stats;

    auto view = reg.view<Comp::MigratoryComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic write order).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // Seasonal target + drive are shared by all participants this tick (pure functions of the
    // season phase). Compute once.
    const MigrationTarget target = MigrationTargetAt(season01);
    const float drive = MigrationDriveAt(season01);

    stats.participants = static_cast<int>(ents.size());
    stats.target_x = target.x;
    stats.target_z = target.z;
    stats.drive = drive;

    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        auto& mig = view.get<Comp::MigratoryComponent>(e);

        mig.drive = drive;

        if (drive <= 0.0f) {
            // Settled mid-season: emit an exactly-zero wish (no migratory pull).
            mig.wish_x = 0.0f;
            mig.wish_z = 0.0f;
            continue;
        }

        // Direction toward the seasonal target, normalised, then scaled by the drive.
        const float dx = target.x - tf.position.x;
        const float dz = target.z - tf.position.z;
        const float d = dm::Sqrt(dx * dx + dz * dz);
        if (d > 1.0e-5f) {
            const float inv = drive / d; // unit direction * drive
            mig.wish_x = dx * inv;
            mig.wish_z = dz * inv;
            ++stats.driven;
        } else {
            // Already at the target: nowhere to migrate, so no wish (but still "driven" season).
            mig.wish_x = 0.0f;
            mig.wish_z = 0.0f;
        }
    }

    return stats;
}

} // namespace luminumbra::ai
