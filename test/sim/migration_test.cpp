// sim.migration: SEASONAL MIGRATION toward a MOVING target. Migratory creatures are
// pulled toward a seasonal target that travels a closed loop over the year; the urge ("drive")
// PEAKS at the season transitions and is ~0 mid-season; the per-creature wish points toward the
// current target. Deterministic (id-ordered, DeterministicMath only, NO rng), gated by the
// MigratoryComponent opt-in.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/MigrationSystem.h"
#include "components/CoreComponents.h"
#include "components/MigratoryComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::MigrationDriveAt;
using luminumbra::ai::MigrationStats;
using luminumbra::ai::MigrationTarget;
using luminumbra::ai::MigrationTargetAt;
using luminumbra::ai::RunMigrationOnTick;

// Spawn a creature carrying a MigratoryComponent (the opt-in) at world XZ (x,z).
entt::entity spawnMigrant(entt::registry& r, float x, float z) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    r.emplace<Comp::MigratoryComponent>(e);
    return e;
}

const Comp::MigratoryComponent& migOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::MigratoryComponent>(e);
}

// ---- gating / no-op ----

// Empty roster: the system reports nothing and changes nothing.
TEST(Migration, EmptyRosterNoOp) {
    entt::registry r;
    MigrationStats s = RunMigrationOnTick(r, 0.0f);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.driven, 0);
}

// Creatures WITHOUT a MigratoryComponent are invisible to the system (opt-in gate): a world of
// non-participants is a pure no-op even with a transform present.
TEST(Migration, NonParticipantsIgnored) {
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(10.0f, 0.0f, 10.0f);
    MigrationStats s = RunMigrationOnTick(r, 0.0f);
    EXPECT_EQ(s.participants, 0);
}

// ---- drive PEAKS at the season transitions, ~0 mid-season ----

// At the four season transitions (0, 0.25, 0.5, 0.75) the drive is strong; at the quarter
// midpoints (0.125, 0.375, 0.625, 0.875) it is floored to ~0.
TEST(Migration, DrivePeaksAtTransitionsLowMidSeason) {
    const float transitions[] = {0.0f, 0.25f, 0.5f, 0.75f};
    for (float s : transitions) {
        EXPECT_GT(MigrationDriveAt(s), 0.9f) << "transition season01=" << s;
    }
    const float midpoints[] = {0.125f, 0.375f, 0.625f, 0.875f};
    for (float s : midpoints) {
        EXPECT_LE(MigrationDriveAt(s), 0.05f) << "midpoint season01=" << s;
    }
    // And a transition drive strictly exceeds a midpoint drive.
    EXPECT_GT(MigrationDriveAt(0.25f), MigrationDriveAt(0.375f));
}

// The drive surfaced through a live tick matches the pure curve and lands on the component.
TEST(Migration, DriveWrittenToComponentMatchesCurve) {
    entt::registry r;
    auto e = spawnMigrant(r, 0.0f, 0.0f);

    MigrationStats sTrans = RunMigrationOnTick(r, 0.25f); // a transition
    EXPECT_FLOAT_EQ(migOf(r, e).drive, MigrationDriveAt(0.25f));
    EXPECT_GT(sTrans.drive, 0.9f);

    MigrationStats sMid = RunMigrationOnTick(r, 0.375f); // a midpoint
    EXPECT_FLOAT_EQ(migOf(r, e).drive, MigrationDriveAt(0.375f));
    EXPECT_EQ(sMid.drive, 0.0f); // floored to a clean zero mid-season
}

// Mid-season (drive 0): the wish is EXACTLY zero (settled, no migratory pull).
TEST(Migration, MidSeasonWishIsZero) {
    entt::registry r;
    auto e = spawnMigrant(r, 100.0f, -50.0f); // away from origin so a nonzero dir is possible
    RunMigrationOnTick(r, 0.125f);            // a quarter midpoint -> drive 0
    EXPECT_EQ(migOf(r, e).wish_x, 0.0f);
    EXPECT_EQ(migOf(r, e).wish_z, 0.0f);
}

// ---- the migration TARGET MOVES over the year ----

// Different season01 -> different target (the seasonal target travels a path), and a full year
// returns to the start (closed loop).
TEST(Migration, TargetMovesOverYear) {
    MigrationTarget t0 = MigrationTargetAt(0.0f);
    MigrationTarget tQ = MigrationTargetAt(0.25f);
    MigrationTarget tH = MigrationTargetAt(0.5f);
    MigrationTarget t3 = MigrationTargetAt(0.75f);

    auto differs = [](const MigrationTarget& a, const MigrationTarget& b) {
        return std::fabs(a.x - b.x) > 1.0f || std::fabs(a.z - b.z) > 1.0f;
    };
    EXPECT_TRUE(differs(t0, tQ));
    EXPECT_TRUE(differs(tQ, tH));
    EXPECT_TRUE(differs(tH, t3));
    EXPECT_TRUE(differs(t0, tH)); // opposite ends of the year-loop are far apart

    // A full year wraps back to the start.
    MigrationTarget tYear = MigrationTargetAt(1.0f);
    EXPECT_NEAR(tYear.x, t0.x, 0.5f);
    EXPECT_NEAR(tYear.z, t0.z, 0.5f);
}

// The live stats expose the moving target.
TEST(Migration, StatsTargetTracksSeason) {
    entt::registry r;
    spawnMigrant(r, 0.0f, 0.0f);
    MigrationStats sA = RunMigrationOnTick(r, 0.0f);
    MigrationStats sB = RunMigrationOnTick(r, 0.25f);
    const float moved = std::fabs(sA.target_x - sB.target_x) + std::fabs(sA.target_z - sB.target_z);
    EXPECT_GT(moved, 1.0f); // the target moved between the two seasons.
}

// ---- the wish POINTS TOWARD the current target ----

// When driven, the wish direction points from the creature toward the seasonal target.
TEST(Migration, WishPointsTowardTarget) {
    entt::registry r;
    // Place the creature well away from the origin so the direction is unambiguous.
    auto e = spawnMigrant(r, -300.0f, 200.0f);
    const float season = 0.25f; // a transition -> strong drive

    MigrationStats s = RunMigrationOnTick(r, season);
    ASSERT_GT(s.drive, 0.0f);

    const auto& tf = r.get<Comp::TransformComponent>(e);
    const float wantx = s.target_x - tf.position.x;
    const float wantz = s.target_z - tf.position.z;

    const auto& mig = migOf(r, e);
    // Wish must be non-zero and co-directional with (target - pos): dot product positive, and
    // the cross product ~0 (collinear).
    const float dot = mig.wish_x * wantx + mig.wish_z * wantz;
    const float cross = mig.wish_x * wantz - mig.wish_z * wantx;
    EXPECT_GT(dot, 0.0f);
    EXPECT_NEAR(cross, 0.0f, 1.0f);

    // The wish magnitude equals the drive (unit direction * drive).
    const float mag = std::sqrt(mig.wish_x * mig.wish_x + mig.wish_z * mig.wish_z);
    EXPECT_NEAR(mag, s.drive, 1.0e-3f);
    EXPECT_EQ(s.driven, 1);
}

// Two creatures on OPPOSITE sides of the target wish in OPPOSITE x-directions (each toward it).
TEST(Migration, WishConvergesFromBothSides) {
    entt::registry r;
    const MigrationTarget t = MigrationTargetAt(0.0f); // target near (+R, 0)
    auto west = spawnMigrant(r, t.x - 200.0f, t.z);    // to the -x side of the target
    auto east = spawnMigrant(r, t.x + 200.0f, t.z);    // to the +x side of the target

    RunMigrationOnTick(r, 0.0f);
    EXPECT_GT(migOf(r, west).wish_x, 0.0f); // west creature heads +x toward target
    EXPECT_LT(migOf(r, east).wish_x, 0.0f); // east creature heads -x toward target
}

// ---- determinism: run == replay (bit-for-bit over a full simulated year) ----

TEST(Migration, RunEqualsReplay) {
    auto simulate = []() {
        entt::registry r;
        std::vector<entt::entity> es;
        es.push_back(spawnMigrant(r, 0.0f, 0.0f));
        es.push_back(spawnMigrant(r, 120.0f, -40.0f));
        es.push_back(spawnMigrant(r, -250.0f, 310.0f));
        es.push_back(spawnMigrant(r, 75.5f, 75.5f));

        std::vector<float> trace;
        // Walk a full year in 120 steps (season01 advances by 1/120 each tick, wrapping).
        const int steps = 120;
        for (int t = 0; t < steps; ++t) {
            const float season01 = static_cast<float>(t) / static_cast<float>(steps);
            RunMigrationOnTick(r, season01);
            for (auto e : es) {
                const auto& mig = r.get<Comp::MigratoryComponent>(e);
                trace.push_back(mig.drive);
                trace.push_back(mig.wish_x);
                trace.push_back(mig.wish_z);
            }
        }
        return trace;
    };

    const std::vector<float> run = simulate();
    const std::vector<float> replay = simulate();
    ASSERT_EQ(run.size(), replay.size());
    for (std::size_t i = 0; i < run.size(); ++i) {
        EXPECT_EQ(run[i], replay[i]) << "divergence at sample " << i;
    }
}

// Order-independence: shuffled creation order yields the same per-creature wish (id-ordered
// traversal + a target/drive that depend only on the season, not on neighbours).
TEST(Migration, OrderIndependent) {
    struct Spec {
        float x, z;
    };
    std::vector<Spec> specs = {{0.0f, 0.0f}, {120.0f, -40.0f}, {-250.0f, 310.0f}, {75.5f, 75.5f}};

    auto build = [](const std::vector<Spec>& order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order)
            es.push_back(spawnMigrant(r, sp.x, sp.z));
        RunMigrationOnTick(r, 0.25f);
        std::vector<std::pair<std::pair<float, float>, std::pair<float, float>>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            const auto& mig = r.get<Comp::MigratoryComponent>(es[i]);
            out.push_back({{tf.position.x, tf.position.z}, {mig.wish_x, mig.wish_z}});
        }
        std::sort(out.begin(), out.end());
        return out;
    };

    auto forward = build(specs);
    std::reverse(specs.begin(), specs.end());
    auto reversed = build(specs);

    ASSERT_EQ(forward.size(), reversed.size());
    for (std::size_t i = 0; i < forward.size(); ++i) {
        EXPECT_EQ(forward[i].first, reversed[i].first);
        EXPECT_FLOAT_EQ(forward[i].second.first, reversed[i].second.first);
        EXPECT_FLOAT_EQ(forward[i].second.second, reversed[i].second.second);
    }
}

} // namespace
