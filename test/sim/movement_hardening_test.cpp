// MOVEMENT & STEERING SEAMS — adversarial hardening.
//
// This file does NOT re-test the happy paths (those live in predator_pack_test.cpp /
// migration_test.cpp / territory_test.cpp / thirst_test.cpp / scavenging_test.cpp /
// steering_consumer_test.cpp). It hunts for what those MISS across the movement/steering
// cluster: producer->consumer field-meaning/magnitude contracts (unit-direction vs world
// point, scaled-by-what), coincident-entity (zero distance) safety, threshold EDGES,
// run==replay, order-independence, gating no-ops, and NaN/degenerate-input survival.
//
// Systems covered:
//   ai/PredatorPackSystem.h   (PackHunterComponent.coord_x/z = UNIT direction)
//   ai/MigrationSystem.h      (MigratoryComponent.wish = unit dir * drive; drive in [0,1])
//   ai/TerritorySystem.h      (TerritoryBiasComponent.wish = unit-toward-home * capped mag)
//   ai/ThirstSystem.h         (ThirstComponent.wish = unit-toward-hole * thirst)
//   ai/ScavengingSystem.h     (ScavengerComponent.wish = UNIT toward carcass)
//   ai/Flocking.h             (pure ComputeFlockSteer)
//   ai/SteeringConsumer.h     (blend that READS pack/migration/territory)
//
// Determinism: only the systems' own seeded/integer paths; NO wall-clock/std::random.
// For run==replay assert EXACT float equality (the systems are -ffp-contract=off bit-exact).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <vector>

#include <entt/entt.hpp>

#include "ai/Flocking.h"
#include "ai/MigrationSystem.h"
#include "ai/PredatorPackSystem.h"
#include "ai/ScavengingSystem.h"
#include "ai/SteeringConsumer.h"
#include "ai/TerritorySystem.h"
#include "ai/ThirstSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/MigratoryComponents.h"
#include "components/MortalComponents.h"
#include "components/PackHunterComponents.h"
#include "components/ScavengerComponent.h"
#include "components/TerritoryComponents.h"
#include "components/ThirstComponents.h"

#include "../support/SeededShuffle.h"

namespace {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;
using luminumbra::ai::RunPredatorPackOnTick;

// ---------------------------------------------------------------------------
// shared spawn helpers
// ---------------------------------------------------------------------------

entt::entity
spawnPredator(entt::registry& r, float x, float z, bool predator = true, bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = predator;
    cr.eaten = eaten;
    r.emplace<Comp::PackHunterComponent>(e);
    return e;
}

entt::entity spawnPrey(entt::registry& r, float x, float z, bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    cr.eaten = eaten;
    return e;
}

bool isFiniteF(float v) {
    return !std::isnan(v) && !std::isinf(v);
}

// ===========================================================================
// PredatorPackSystem
// ===========================================================================

// CONTRACT: coord_x/z is a UNIT direction. Whenever a wish is emitted its magnitude must be
// ~1 (the consumer multiplies it by move_speed*1.5; a non-unit producer would scale speed).
TEST(PackHarden, EmittedWishIsUnitLength) {
    entt::registry r;
    auto p0 = spawnPredator(r, -2.0f, 0.0f);
    auto p1 = spawnPredator(r, 2.0f, 0.0f);
    spawnPrey(r, 0.0f, 37.0f);
    RunPredatorPackOnTick(r, 0);
    for (auto e : {p0, p1}) {
        const auto& pk = r.get<Comp::PackHunterComponent>(e);
        const float len = std::sqrt(pk.coord_x * pk.coord_x + pk.coord_z * pk.coord_z);
        EXPECT_NEAR(len, 1.0f, 1.0e-3f) << "pack coord must be a UNIT steer direction";
    }
}

// EDGE: a predator sitting EXACTLY on top of its lone prey (zero distance) must not emit a
// NaN/inf wish; the d>1e-5 guard zeroes the wish instead.
TEST(PackHarden, CoincidentPredatorPreyNoNaN) {
    entt::registry r;
    auto p = spawnPredator(r, 5.0f, 5.0f);
    spawnPrey(r, 5.0f, 5.0f); // exactly coincident
    RunPredatorPackOnTick(r, 0);
    const auto& pk = r.get<Comp::PackHunterComponent>(p);
    EXPECT_TRUE(isFiniteF(pk.coord_x));
    EXPECT_TRUE(isFiniteF(pk.coord_z));
    EXPECT_FLOAT_EQ(pk.coord_x, 0.0f);
    EXPECT_FLOAT_EQ(pk.coord_z, 0.0f);
}

// EDGE: two predators at the SAME position with a prey. Pack of 2 -> flank standoff ring; the
// wish must still be finite + unit (the centroid==each position, baseAngle from centroid->prey).
TEST(PackHarden, CoincidentPackMatesFinite) {
    entt::registry r;
    auto p0 = spawnPredator(r, 0.0f, 0.0f);
    auto p1 = spawnPredator(r, 0.0f, 0.0f); // coincident ally -> pack of 2
    spawnPrey(r, 0.0f, 20.0f);
    RunPredatorPackOnTick(r, 0);
    for (auto e : {p0, p1}) {
        const auto& pk = r.get<Comp::PackHunterComponent>(e);
        EXPECT_EQ(pk.in_pack, 1u);
        EXPECT_TRUE(isFiniteF(pk.coord_x));
        EXPECT_TRUE(isFiniteF(pk.coord_z));
        const float len = std::sqrt(pk.coord_x * pk.coord_x + pk.coord_z * pk.coord_z);
        // standoff ring (4u) around a prey 20u away -> still well away from the target point.
        EXPECT_NEAR(len, 1.0f, 1.0e-3f);
    }
}

// THRESHOLD EDGE: two predators EXACTLY kPackRadius apart count as pack-mates (the test is
// d <= kPackRadius, inclusive). Just-beyond does NOT.
TEST(PackHarden, PackRadiusBoundaryInclusive) {
    using luminumbra::ai::kPackRadius;
    {
        entt::registry r;
        auto a = spawnPredator(r, 0.0f, 0.0f);
        auto b = spawnPredator(r, kPackRadius, 0.0f); // exactly on the boundary
        spawnPrey(r, 0.0f, 50.0f);
        RunPredatorPackOnTick(r, 0);
        EXPECT_EQ(r.get<Comp::PackHunterComponent>(a).in_pack, 1u)
            << "exactly kPackRadius apart must be a pack (<= is inclusive)";
        EXPECT_EQ(r.get<Comp::PackHunterComponent>(b).in_pack, 1u);
    }
    {
        entt::registry r;
        auto a = spawnPredator(r, 0.0f, 0.0f);
        auto b = spawnPredator(r, kPackRadius + 0.5f, 0.0f); // just beyond
        spawnPrey(r, 0.0f, 50.0f);
        RunPredatorPackOnTick(r, 0);
        EXPECT_EQ(r.get<Comp::PackHunterComponent>(a).in_pack, 0u)
            << "beyond kPackRadius must hunt solo";
        EXPECT_EQ(r.get<Comp::PackHunterComponent>(b).in_pack, 0u);
    }
}

// A non-predator carrying the PackHunterComponent tag is INERT (no coordination), but it still
// counts as a participant. Its wish must be exactly zero.
TEST(PackHarden, NonPredatorTagHolderInert) {
    entt::registry r;
    auto tagOnly = spawnPredator(r, 1.0f, 1.0f, /*predator=*/false);
    spawnPrey(r, 1.0f, 30.0f);
    auto s = RunPredatorPackOnTick(r, 0);
    EXPECT_EQ(s.participants, 1);
    const auto& pk = r.get<Comp::PackHunterComponent>(tagOnly);
    EXPECT_EQ(pk.in_pack, 0u);
    EXPECT_FLOAT_EQ(pk.coord_x, 0.0f);
    EXPECT_FLOAT_EQ(pk.coord_z, 0.0f);
}

// ORDER-INDEPENDENCE under many ticks with a mixed roster, keyed by geometry. (Stronger than
// the existing single-tick reverse test: 8 ticks, 3 build orderings.)
TEST(PackHarden, OrderIndependentMultiTick) {
    struct Spec {
        float x, z;
        bool predator;
        bool prey;
    };
    std::vector<Spec> base = {
        {-4.0f, 0.0f, true, false},
        {0.0f, 1.0f, true, false},
        {4.0f, 0.0f, true, false},
        {0.0f, 30.0f, false, true},
        {12.0f, 28.0f, false, true},
    };
    auto build = [](std::vector<Spec> order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order) {
            es.push_back(sp.prey ? spawnPrey(r, sp.x, sp.z)
                                 : spawnPredator(r, sp.x, sp.z, sp.predator));
        }
        for (std::uint64_t t = 0; t < 8; ++t)
            RunPredatorPackOnTick(r, t);
        std::vector<std::tuple<float, float, float, float, int>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i].prey)
                continue;
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            const auto& pk = r.get<Comp::PackHunterComponent>(es[i]);
            out.push_back({tf.position.x,
                           tf.position.z,
                           pk.coord_x,
                           pk.coord_z,
                           static_cast<int>(pk.in_pack)});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto a = build(base);
    std::reverse(base.begin(), base.end());
    auto b = build(base);
    std::rotate(base.begin(), base.begin() + 2, base.end());
    auto c = build(base);
    ASSERT_EQ(a.size(), b.size());
    ASSERT_EQ(a.size(), c.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i], b[i]);
        EXPECT_EQ(a[i], c[i]) << "flank result must depend on geometry, not spawn order";
    }
}

// ORDER-INDEPENDENCE via a DETERMINISTIC SEEDED SHUFFLE (test-rigor  / ).
// OrderIndependentMultiTick above uses ad-hoc std::reverse/std::rotate; this adds the
// integer-SEEDED permutation  mandates so the reorder is itself reproducible.
TEST(PackHarden, OrderIndependentSeededShuffle) {
    struct Spec {
        float x, z;
        bool predator;
        bool prey;
    };
    std::vector<Spec> base = {
        {-4.0f, 0.0f, true, false},
        {0.0f, 1.0f, true, false},
        {4.0f, 0.0f, true, false},
        {-2.0f, 2.0f, true, false},
        {0.0f, 30.0f, false, true},
        {12.0f, 28.0f, false, true},
        {-8.0f, 33.0f, false, true},
    };
    auto build = [](const std::vector<Spec>& order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order)
            es.push_back(sp.prey ? spawnPrey(r, sp.x, sp.z)
                                 : spawnPredator(r, sp.x, sp.z, sp.predator));
        for (std::uint64_t t = 0; t < 8; ++t)
            RunPredatorPackOnTick(r, t);
        std::vector<std::tuple<float, float, float, float, int>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i].prey)
                continue;
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            const auto& pk = r.get<Comp::PackHunterComponent>(es[i]);
            out.push_back({tf.position.x,
                           tf.position.z,
                           pk.coord_x,
                           pk.coord_z,
                           static_cast<int>(pk.in_pack)});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto baseline = build(base);

    auto shuffled = Luminumbra::TestSupport::SeededShuffled(base, /*seed=*/0xBADC0DEu);
    bool reordered = false; // guard : the seed must actually permute.
    for (std::size_t i = 0; i < base.size(); ++i)
        if (shuffled[i].x != base[i].x || shuffled[i].z != base[i].z) {
            reordered = true;
            break;
        }
    ASSERT_TRUE(reordered) << "seed must actually permute the spawn order";

    const auto fromShuffle = build(shuffled);
    ASSERT_EQ(baseline.size(), fromShuffle.size());
    for (std::size_t i = 0; i < baseline.size(); ++i)
        EXPECT_EQ(baseline[i], fromShuffle[i])
            << "flank result must depend on geometry, not (seeded-shuffled) spawn order";
}

// ORACLE (test-rigor ): the FLANK RING POINT geometry, recomputed independently +
// pinned to golden literals. The flank target is
//   baseAngle = Atan2(preyZ-cz, preyX-cx); angle = baseAngle + (2*pi/packSize)*rank;
//   target = prey + kFlankStandoff*(Cos angle, Sin angle)   (PredatorPackSystem.h:210-215)
// and the emitted coord is the UNIT vector self->target. We use a pack of THREE and place
// the second member exactly on the prey, so its target-self vector is the standoff ring
// vector and its unit coord reduces to (Cos angle, Sin angle) — recomputed here with the
// SAME dm:: functions. packSize=3 is deliberate: a pack of 2 has step=pi, so a
// +step*ordinal -> -step*ordinal sign flip maps the second member to the same point and cannot be
// caught; with step=2*pi/3 the flip moves that angle from baseAngle+2pi/3 to baseAngle-2pi/3,
// flipping coord_x's sign -> RED (proven in mutation-pass-results.md).
TEST(PackHarden, FlankPointGeometryOracle) {
    using luminumbra::ai::kFlankStandoff;
    entt::registry r;
    // Prey to the NORTH; three predators south of it, all pairwise within kPackRadius=20.
    // Positional flank rank = sort by x then z: B(x=4)->0, A(x=10)->1, C(x=16)->2.
    const float preyX = 10.0f, preyZ = 20.0f;
    const float ax = 10.0f, az = 20.0f;  // A: , sits EXACTLY on the prey
    const float bx = 4.0f, bz = 8.0f;    // B:
    const float cxp = 16.0f, czp = 8.0f; // C:
    auto pa = spawnPredator(r, ax, az);
    auto pb = spawnPredator(r, bx, bz);
    auto pc = spawnPredator(r, cxp, czp);
    spawnPrey(r, preyX, preyZ);
    RunPredatorPackOnTick(r, 0);

    const auto& pkA = r.get<Comp::PackHunterComponent>(pa);
    ASSERT_EQ(pkA.in_pack, 1u);
    ASSERT_EQ(r.get<Comp::PackHunterComponent>(pb).in_pack, 1u);
    ASSERT_EQ(r.get<Comp::PackHunterComponent>(pc).in_pack, 1u);

    // INDEPENDENT recompute of the geometry, mirroring the source EXACTLY.
    const float cx = (ax + bx + cxp) / 3.0f;
    const float cz = (az + bz + czp) / 3.0f;
    const float baseAngle = dm::Atan2(preyZ - cz, preyX - cx);
    const float step = dm::kTwoPi / 3.0f;
    const float angleA = baseAngle + step * 1.0f; // A is
    // A is ON the prey, so its unit coord toward (prey + standoff*(Cos,Sin)) is exactly
    // (Cos angleA, Sin angleA).
    const float expX = dm::Cos(angleA);
    const float expZ = dm::Sin(angleA);
    EXPECT_NEAR(pkA.coord_x, expX, 1e-5f);
    EXPECT_NEAR(pkA.coord_z, expZ, 1e-5f);

    // Emitted coord is unit length (the consumer relies on it).
    const float lenA = dm::Sqrt(pkA.coord_x * pkA.coord_x + pkA.coord_z * pkA.coord_z);
    EXPECT_NEAR(lenA, 1.0f, 1e-3f);
    (void)kFlankStandoff;

    // GOLDEN LITERAL (frozen on THIS toolchain): centroid is due-south of the prey so
    // baseAngle = +pi/2; A is  so angleA = pi/2 + 2pi/3 = 7pi/6 -> coord ~ (-0.866, -0.5).
    // A rank-term sign flip sends angleA to pi/2 - 2pi/3 = -pi/6 -> coord ~ (+0.866, -0.5),
    // flipping coord_x's SIGN — so this golden (and the recompute) go RED. See
    // mutation-pass-results.md.
    EXPECT_NEAR(pkA.coord_x, -0.8660254f, 1e-3f);
    EXPECT_NEAR(pkA.coord_z, -0.5000000f, 1e-3f);
}

// ===========================================================================
// MigrationSystem
// ===========================================================================

// CONTRACT: the emitted wish is a unit direction SCALED BY DRIVE -> its magnitude must equal
// the drive exactly (within fp). This is the field-meaning the consumer relies on (it ADDs the
// wish as a velocity-ish drift). A unit-only or drive-squared producer would fail here.
TEST(MigrationHarden, WishMagnitudeEqualsDrive) {
    using luminumbra::ai::RunMigrationOnTick;
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(100.0f, 0.0f, 100.0f); // away from the ellipse target
    r.emplace<Comp::MigratoryComponent>(e);
    // season01 = 0 -> a transition -> drive == 1 (peak).
    auto s = RunMigrationOnTick(r, 0.0f);
    const auto& mig = r.get<Comp::MigratoryComponent>(e);
    const float mag = std::sqrt(mig.wish_x * mig.wish_x + mig.wish_z * mig.wish_z);
    EXPECT_NEAR(mag, s.drive, 1.0e-4f) << "wish magnitude must equal the seasonal drive";
    EXPECT_NEAR(s.drive, 1.0f, 1.0e-4f) << "season01=0 is a transition -> peak drive";
    EXPECT_FLOAT_EQ(mig.drive, s.drive);
}

// BOUNDS: the drive is bounded to [0,1] for ANY season phase (including unwrapped/negative).
TEST(MigrationHarden, DriveBoundedZeroToOne) {
    using luminumbra::ai::MigrationDriveAt;
    for (int i = -50; i <= 350; ++i) {
        const float s = static_cast<float>(i) * 0.01f; // -0.5.. 3.5, sweeps wrapping
        const float d = MigrationDriveAt(s);
        EXPECT_TRUE(isFiniteF(d));
        EXPECT_GE(d, 0.0f);
        EXPECT_LE(d, 1.0f) << "drive out of [0,1] at season " << s;
    }
}

// THRESHOLD EDGE: at a quarter MIDPOINT (0.125) the |cos(4*pi*s)|  0 and the floor
// clamps it to a CLEAN zero -> a settled creature emits an EXACTLY-zero wish (no residue).
TEST(MigrationHarden, MidSeasonExactlyZeroWish) {
    using luminumbra::ai::MigrationDriveAt;
    using luminumbra::ai::RunMigrationOnTick;
    EXPECT_FLOAT_EQ(MigrationDriveAt(0.125f), 0.0f);
    EXPECT_FLOAT_EQ(MigrationDriveAt(0.375f), 0.0f);
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(50.0f, 0.0f, -50.0f);
    r.emplace<Comp::MigratoryComponent>(e);
    auto s = RunMigrationOnTick(r, 0.125f);
    EXPECT_EQ(s.driven, 0);
    const auto& mig = r.get<Comp::MigratoryComponent>(e);
    EXPECT_FLOAT_EQ(mig.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(mig.wish_z, 0.0f);
    EXPECT_FLOAT_EQ(mig.drive, 0.0f);
}

// DRIVE PEAKS AT TRANSITIONS, not solstices: the four quarter boundaries are ~1, the four
// midpoints are 0. (Guards the documented seasonal-curve shape against a sign/phase flip.)
TEST(MigrationHarden, DrivePeaksAtTransitions) {
    using luminumbra::ai::MigrationDriveAt;
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f}) {
        EXPECT_NEAR(MigrationDriveAt(t), 1.0f, 1.0e-3f) << "transition " << t << " must peak";
    }
    for (float m : {0.125f, 0.375f, 0.625f, 0.875f}) {
        EXPECT_FLOAT_EQ(MigrationDriveAt(m), 0.0f) << "midpoint " << m << " must settle to 0";
    }
}

// The seasonal target is a CLOSED loop: a full year returns to the start point (Cos/Sin path).
TEST(MigrationHarden, TargetClosesOverAYear) {
    using luminumbra::ai::MigrationTargetAt;
    auto t0 = MigrationTargetAt(0.0f);
    auto t1 = MigrationTargetAt(1.0f); // one full year later
    EXPECT_NEAR(t0.x, t1.x, 1.0e-2f);
    EXPECT_NEAR(t0.z, t1.z, 1.0e-2f);
    // And the target genuinely MOVES across the year (quarter turn is far away).
    auto tq = MigrationTargetAt(0.25f);
    const float moved = std::sqrt((t0.x - tq.x) * (t0.x - tq.x) + (t0.z - tq.z) * (t0.z - tq.z));
    EXPECT_GT(moved, 100.0f) << "the seasonal target must travel over the year";
}

// EDGE: a creature sitting EXACTLY on the seasonal target during a driven season emits a zero
// wish (no division by ~0), still finite.
TEST(MigrationHarden, AtTargetZeroWishNoNaN) {
    using luminumbra::ai::MigrationTargetAt;
    using luminumbra::ai::RunMigrationOnTick;
    entt::registry r;
    auto e = r.create();
    auto target = MigrationTargetAt(0.0f); // season 0 -> peak drive
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(target.x, 0.0f, target.z);
    r.emplace<Comp::MigratoryComponent>(e);
    RunMigrationOnTick(r, 0.0f);
    const auto& mig = r.get<Comp::MigratoryComponent>(e);
    EXPECT_TRUE(isFiniteF(mig.wish_x));
    EXPECT_TRUE(isFiniteF(mig.wish_z));
    EXPECT_FLOAT_EQ(mig.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(mig.wish_z, 0.0f);
}

// GATING: empty roster -> pure no-op (no participants, target/drive stats untouched defaults).
TEST(MigrationHarden, EmptyRosterNoOp) {
    using luminumbra::ai::RunMigrationOnTick;
    entt::registry r;
    auto s = RunMigrationOnTick(r, 0.0f);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.driven, 0);
}

// NEGATIVE / UNWRAPPED phase wraps deterministically: drive(-0.25) == drive(0.75).
TEST(MigrationHarden, NegativePhaseWraps) {
    using luminumbra::ai::MigrationDriveAt;
    using luminumbra::ai::MigrationTargetAt;
    EXPECT_FLOAT_EQ(MigrationDriveAt(-0.25f), MigrationDriveAt(0.75f));
    auto a = MigrationTargetAt(-0.25f);
    auto b = MigrationTargetAt(0.75f);
    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_FLOAT_EQ(a.z, b.z);
}

// ===========================================================================
// TerritorySystem
// ===========================================================================

// CONTRACT: the homing bias points TOWARD home and its MAGNITUDE == overshoot*gain (capped at
// kTerritoryMaxBias). The consumer ADDs this as a velocity-ish steer, so magnitude meaning is
// load-bearing. Assert both direction and exact magnitude.
TEST(TerritoryHarden, HomingBiasMagnitudeAndDirection) {
    using luminumbra::ai::kTerritoryHomingGain;
    using luminumbra::ai::kTerritoryMaxBias;
    using luminumbra::ai::RunTerritoryOnTick;
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
    auto& terr = r.emplace<Comp::TerritoryComponent>(e);
    terr.radius = 20.0f;
    RunTerritoryOnTick(r, 0); // claims home at origin, bias 0
    // Now teleport it OUT past the radius along +x and re-run.
    tf.position = Luminumbra::Vec3(30.0f, 0.0f, 0.0f); // dist 30, overshoot 10
    RunTerritoryOnTick(r, 1);
    const auto& bias = r.get<Comp::TerritoryBiasComponent>(e);
    const float expectedMag = std::min(10.0f * kTerritoryHomingGain, kTerritoryMaxBias);
    const float mag = std::sqrt(bias.wish_x * bias.wish_x + bias.wish_z * bias.wish_z);
    EXPECT_NEAR(mag, expectedMag, 1.0e-4f);
    // Direction TOWARD home (origin) -> pure -x.
    EXPECT_LT(bias.wish_x, 0.0f);
    EXPECT_NEAR(bias.wish_z, 0.0f, 1.0e-4f);
}

// BOUNDS: a creature stranded ARBITRARILY far from home produces a bias capped at
// kTerritoryMaxBias (no unbounded steer).
TEST(TerritoryHarden, BiasCappedFarFromHome) {
    using luminumbra::ai::kTerritoryMaxBias;
    using luminumbra::ai::RunTerritoryOnTick;
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
    r.emplace<Comp::TerritoryComponent>(e).radius = 20.0f;
    RunTerritoryOnTick(r, 0);
    tf.position = Luminumbra::Vec3(100000.0f, 0.0f, 0.0f); // absurdly far
    RunTerritoryOnTick(r, 1);
    const auto& bias = r.get<Comp::TerritoryBiasComponent>(e);
    const float mag = std::sqrt(bias.wish_x * bias.wish_x + bias.wish_z * bias.wish_z);
    EXPECT_TRUE(isFiniteF(mag));
    EXPECT_NEAR(mag, kTerritoryMaxBias, 1.0e-3f);
}

// THRESHOLD EDGE: a creature EXACTLY at the radius boundary (distance == radius) is INSIDE
// (dist <= radius) -> zero bias. Just beyond -> non-zero.
TEST(TerritoryHarden, RadiusBoundaryIsInside) {
    using luminumbra::ai::RunTerritoryOnTick;
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
    r.emplace<Comp::TerritoryComponent>(e).radius = 20.0f;
    RunTerritoryOnTick(r, 0);                          // home = origin
    tf.position = Luminumbra::Vec3(20.0f, 0.0f, 0.0f); // dist == radius exactly
    RunTerritoryOnTick(r, 1);
    const auto& bias = r.get<Comp::TerritoryBiasComponent>(e);
    EXPECT_FLOAT_EQ(bias.wish_x, 0.0f) << "exactly at radius is INSIDE -> no homing";
    EXPECT_FLOAT_EQ(bias.wish_z, 0.0f);
}

// CLAIM contract: on the FIRST tick the creature claims its CURRENT position as home, so it is
// at home and the bias is exactly zero (even if spawned far from origin). A second tick at the
// same spot stays zero (home is sticky, not re-claimed).
TEST(TerritoryHarden, ClaimAtCurrentPositionZeroBias) {
    using luminumbra::ai::RunTerritoryOnTick;
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(777.0f, 0.0f, -333.0f);
    r.emplace<Comp::TerritoryComponent>(e).radius = 5.0f;
    auto s = RunTerritoryOnTick(r, 0);
    EXPECT_EQ(s.claimed, 1);
    const auto& terr = r.get<Comp::TerritoryComponent>(e);
    EXPECT_EQ(terr.established, 1u);
    EXPECT_FLOAT_EQ(terr.home_x, 777.0f);
    EXPECT_FLOAT_EQ(terr.home_z, -333.0f);
    const auto& bias = r.get<Comp::TerritoryBiasComponent>(e);
    EXPECT_FLOAT_EQ(bias.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(bias.wish_z, 0.0f);
    // Re-run: no second claim.
    auto s2 = RunTerritoryOnTick(r, 1);
    EXPECT_EQ(s2.claimed, 0);
}

// GATING: a world with no TerritoryComponent creates NO TerritoryBiasComponent (no mutation).
TEST(TerritoryHarden, GatingCreatesNoBias) {
    using luminumbra::ai::RunTerritoryOnTick;
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    r.emplace<Comp::CreatureComponent>(e);
    auto s = RunTerritoryOnTick(r, 0);
    EXPECT_EQ(s.participants, 0);
    EXPECT_FALSE(r.all_of<Comp::TerritoryBiasComponent>(e))
        << "non-participant must not get a bias component";
}

// run==replay over many ticks of straying + returning.
TEST(TerritoryHarden, RunEqualsReplay) {
    using luminumbra::ai::RunTerritoryOnTick;
    auto sim = [] {
        entt::registry r;
        std::vector<entt::entity> es;
        for (int i = 0; i < 4; ++i) {
            auto e = r.create();
            auto& tf = r.emplace<Comp::TransformComponent>(e);
            tf.position =
                Luminumbra::Vec3(static_cast<float>(i) * 7.0f, 0.0f, static_cast<float>(i) * -3.0f);
            r.emplace<Comp::TerritoryComponent>(e).radius = 10.0f;
            es.push_back(e);
        }
        std::vector<float> trace;
        for (std::uint64_t t = 0; t < 20; ++t) {
            // drift each creature outward deterministically
            for (std::size_t i = 0; i < es.size(); ++i) {
                auto& tf = r.get<Comp::TransformComponent>(es[i]);
                tf.position.x += 1.5f;
                tf.position.z -= 0.5f;
            }
            RunTerritoryOnTick(r, t);
            for (auto e : es) {
                const auto& b = r.get<Comp::TerritoryBiasComponent>(e);
                trace.push_back(b.wish_x);
                trace.push_back(b.wish_z);
            }
        }
        return trace;
    };
    auto a = sim();
    auto b = sim();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        EXPECT_EQ(a[i], b[i]) << "divergence " << i;
}

// ===========================================================================
// ThirstSystem
// ===========================================================================

entt::entity spawnThirsty(entt::registry& r, float x, float z, float thirst, bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.eaten = eaten;
    auto& th = r.emplace<Comp::ThirstComponent>(e);
    th.thirst = thirst;
    return e;
}

entt::entity spawnHole(entt::registry& r, float x, float z, float radius) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    r.emplace<Comp::WaterHoleComponent>(e).radius = radius;
    return e;
}

// CONTRACT: the seeking wish is a UNIT direction toward the hole SCALED BY THIRST. Magnitude
// must equal (post-rise) thirst exactly; direction toward the hole. (The dt rise is added
// before the wish is computed, so use the post-rise thirst.)
TEST(ThirstHarden, SeekWishMagnitudeEqualsThirst) {
    using luminumbra::ai::kThirstRise;
    using luminumbra::ai::RunThirstOnTick;
    entt::registry r;
    auto e = spawnThirsty(r, 0.0f, 0.0f, /*thirst=*/0.6f);
    spawnHole(r, 0.0f, 100.0f, /*radius=*/4.0f); // far north, out of range
    const float dt = 1.0f;
    RunThirstOnTick(r, dt);
    const auto& th = r.get<Comp::ThirstComponent>(e);
    const float postThirst = 0.6f + kThirstRise * dt;
    const float mag = std::sqrt(th.wish_x * th.wish_x + th.wish_z * th.wish_z);
    EXPECT_NEAR(mag, postThirst, 1.0e-4f) << "seek wish must be unit-dir * (post-rise) thirst";
    // Direction toward the hole (+z).
    EXPECT_NEAR(th.wish_x, 0.0f, 1.0e-4f);
    EXPECT_GT(th.wish_z, 0.0f);
    EXPECT_EQ(th.drinking, 0);
}

// THRESHOLD EDGE: just below the seek threshold -> no wish; at/above -> wish. The rise pushes a
// just-under creature over, so set thirst so post-rise still < threshold to isolate the gate.
TEST(ThirstHarden, SeekThresholdGate) {
    using luminumbra::ai::kThirstRise;
    using luminumbra::ai::kThirstSeekThreshold;
    using luminumbra::ai::RunThirstOnTick;
    // BELOW: post-rise thirst stays under the threshold -> zero wish.
    {
        entt::registry r;
        const float dt = 1.0f;
        const float start = kThirstSeekThreshold - 2.0f * kThirstRise * dt;
        auto e = spawnThirsty(r, 0.0f, 0.0f, start);
        spawnHole(r, 50.0f, 0.0f, 4.0f);
        RunThirstOnTick(r, dt);
        const auto& th = r.get<Comp::ThirstComponent>(e);
        EXPECT_LT(th.thirst, kThirstSeekThreshold);
        EXPECT_FLOAT_EQ(th.wish_x, 0.0f);
        EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
    }
    // ABOVE: post-rise clearly over -> non-zero wish.
    {
        entt::registry r;
        auto e = spawnThirsty(r, 0.0f, 0.0f, kThirstSeekThreshold + 0.2f);
        spawnHole(r, 50.0f, 0.0f, 4.0f);
        RunThirstOnTick(r, 1.0f);
        const auto& th = r.get<Comp::ThirstComponent>(e);
        EXPECT_GT(std::abs(th.wish_x) + std::abs(th.wish_z), 0.0f);
    }
}

// DRINK contract: a creature INSIDE the hole radius sets drinking=1 and thirst FALLS (and is
// clamped >= 0). The drink path must zero the wish (it has arrived).
TEST(ThirstHarden, DrinkInsideRadiusLowersThirst) {
    using luminumbra::ai::kThirstDrink;
    using luminumbra::ai::kThirstRise;
    using luminumbra::ai::RunThirstOnTick;
    entt::registry r;
    auto e = spawnThirsty(r, 1.0f, 1.0f, /*thirst=*/0.9f);
    spawnHole(r, 1.0f, 1.0f, /*radius=*/4.0f); // creature sits inside
    const float dt = 1.0f;
    RunThirstOnTick(r, dt);
    const auto& th = r.get<Comp::ThirstComponent>(e);
    EXPECT_EQ(th.drinking, 1);
    // thirst = clamp(0.9 + rise) then clamp(- drink). drink(0.5) > rise(0.04) -> net fall.
    const float expected =
        std::max(0.0f, std::min(1.0f, 0.9f + kThirstRise * dt) - kThirstDrink * dt);
    EXPECT_NEAR(th.thirst, expected, 1.0e-5f);
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
}

// BOUNDS: thirst is clamped to [0,1] — many rise ticks never exceed 1.
TEST(ThirstHarden, ThirstClampedToOne) {
    using luminumbra::ai::RunThirstOnTick;
    entt::registry r;
    auto e = spawnThirsty(r, 0.0f, 0.0f, 0.99f);
    // no holes -> just rises
    for (int i = 0; i < 100; ++i)
        RunThirstOnTick(r, 1.0f);
    const auto& th = r.get<Comp::ThirstComponent>(e);
    EXPECT_LE(th.thirst, 1.0f);
    EXPECT_GE(th.thirst, 0.0f);
    EXPECT_FLOAT_EQ(th.thirst, 1.0f);
}

// EDGE: coincident creature + hole with radius 0. dist==0 <= radius==0 -> drinks; no NaN wish.
TEST(ThirstHarden, CoincidentZeroRadiusHoleNoNaN) {
    using luminumbra::ai::RunThirstOnTick;
    entt::registry r;
    auto e = spawnThirsty(r, 2.0f, 2.0f, 0.8f);
    spawnHole(r, 2.0f, 2.0f, /*radius=*/0.0f);
    RunThirstOnTick(r, 1.0f);
    const auto& th = r.get<Comp::ThirstComponent>(e);
    EXPECT_EQ(th.drinking, 1);
    EXPECT_TRUE(isFiniteF(th.wish_x));
    EXPECT_TRUE(isFiniteF(th.wish_z));
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
}

// A carcass (eaten) neither thirsts nor drinks: thirst unchanged, drinking 0, wish 0.
TEST(ThirstHarden, CarcassInert) {
    using luminumbra::ai::RunThirstOnTick;
    entt::registry r;
    auto e = spawnThirsty(r, 0.0f, 0.0f, /*thirst=*/0.7f, /*eaten=*/true);
    spawnHole(r, 0.0f, 5.0f, 4.0f);
    RunThirstOnTick(r, 1.0f);
    const auto& th = r.get<Comp::ThirstComponent>(e);
    EXPECT_FLOAT_EQ(th.thirst, 0.7f) << "a carcass must not get thirsty";
    EXPECT_EQ(th.drinking, 0);
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
}

// NO water in the world: the creature still gets thirsty but emits a zero wish.
TEST(ThirstHarden, NoWaterStillThirstsZeroWish) {
    using luminumbra::ai::kThirstRise;
    using luminumbra::ai::RunThirstOnTick;
    entt::registry r;
    auto e = spawnThirsty(r, 0.0f, 0.0f, 0.5f);
    RunThirstOnTick(r, 1.0f);
    const auto& th = r.get<Comp::ThirstComponent>(e);
    EXPECT_NEAR(th.thirst, 0.5f + kThirstRise, 1.0e-5f);
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
}

// ORDER-INDEPENDENCE: nearest-hole + steer keyed by geometry, invariant to spawn order.
TEST(ThirstHarden, OrderIndependent) {
    using luminumbra::ai::RunThirstOnTick;
    struct Spec {
        float x, z;
        float thirst;
        bool hole;
        float radius;
    };
    std::vector<Spec> base = {
        {0.0f, 0.0f, 0.8f, false, 0.0f},
        {10.0f, 0.0f, 0.8f, false, 0.0f},
        {3.0f, 0.0f, 0.0f, true, 2.0f},
        {40.0f, 0.0f, 0.0f, true, 2.0f},
    };
    auto build = [](std::vector<Spec> order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order) {
            es.push_back(sp.hole ? spawnHole(r, sp.x, sp.z, sp.radius)
                                 : spawnThirsty(r, sp.x, sp.z, sp.thirst));
        }
        RunThirstOnTick(r, 1.0f);
        std::vector<std::tuple<float, float, float, float>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i].hole)
                continue;
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            const auto& th = r.get<Comp::ThirstComponent>(es[i]);
            out.push_back({tf.position.x, tf.position.z, th.wish_x, th.wish_z});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto a = build(base);
    std::reverse(base.begin(), base.end());
    auto b = build(base);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(std::get<2>(a[i]), std::get<2>(b[i]));
        EXPECT_FLOAT_EQ(std::get<3>(a[i]), std::get<3>(b[i]));
    }
}

// ===========================================================================
// ScavengingSystem
// ===========================================================================

entt::entity spawnScavenger(entt::registry& r, float x, float z, float hunger, bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.hunger = hunger;
    cr.eaten = eaten;
    r.emplace<Comp::ScavengerComponent>(e);
    return e;
}

entt::entity spawnCarcass(entt::registry& r, float x, float z) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.eaten = true; // dead body
    return e;
}

// CONTRACT: the scavenger steer is a PURE UNIT direction toward the carcass (NOT scaled by
// hunger/distance). Magnitude must be ~1.
TEST(ScavengeHarden, SteerIsUnitDirection) {
    using luminumbra::ai::RunScavengingOnTick;
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.9f);
    spawnCarcass(r, 30.0f, 40.0f); // 50 units away (3-4-5)
    RunScavengingOnTick(r, 0);
    const auto& sc = r.get<Comp::ScavengerComponent>(s);
    const float mag = std::sqrt(sc.wish_x * sc.wish_x + sc.wish_z * sc.wish_z);
    EXPECT_NEAR(mag, 1.0f, 1.0e-4f) << "scavenger wish must be a UNIT steer (not hunger-scaled)";
    // direction toward (30,40): (0.6, 0.8)
    EXPECT_NEAR(sc.wish_x, 0.6f, 1.0e-3f);
    EXPECT_NEAR(sc.wish_z, 0.8f, 1.0e-3f);
    EXPECT_EQ(sc.feeding, 0);
}

// THRESHOLD EDGE: hunger EXACTLY at the threshold does NOT scavenge (the gate is hunger <=
// threshold -> skip). Just above -> seeks.
TEST(ScavengeHarden, HungerThresholdGate) {
    using luminumbra::ai::kScavengeHungerThreshold;
    using luminumbra::ai::RunScavengingOnTick;
    {
        entt::registry r;
        auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/kScavengeHungerThreshold); // exactly at
        spawnCarcass(r, 20.0f, 0.0f);
        auto st = RunScavengingOnTick(r, 0);
        EXPECT_EQ(st.seeking, 0) << "hunger == threshold must NOT seek (gate is <=)";
        const auto& sc = r.get<Comp::ScavengerComponent>(s);
        EXPECT_FLOAT_EQ(sc.wish_x, 0.0f);
        EXPECT_FLOAT_EQ(sc.wish_z, 0.0f);
    }
    {
        entt::registry r;
        spawnScavenger(r, 0.0f, 0.0f, kScavengeHungerThreshold + 0.01f);
        spawnCarcass(r, 20.0f, 0.0f);
        auto st = RunScavengingOnTick(r, 0);
        EXPECT_EQ(st.seeking, 1) << "just above threshold must seek";
    }
}

// FEED contract: within feed radius, feeding=1 and hunger drops by kScavengeFeedRate (clamped
// >=0), wish held at zero (settle on the food).
TEST(ScavengeHarden, FeedInsideRadiusLowersHunger) {
    using luminumbra::ai::kScavengeFeedRadius;
    using luminumbra::ai::kScavengeFeedRate;
    using luminumbra::ai::RunScavengingOnTick;
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.8f);
    spawnCarcass(r, kScavengeFeedRadius * 0.5f, 0.0f); // well inside feed radius
    RunScavengingOnTick(r, 0);
    const auto& sc = r.get<Comp::ScavengerComponent>(s);
    const auto& cr = r.get<Comp::CreatureComponent>(s);
    EXPECT_EQ(sc.feeding, 1);
    EXPECT_NEAR(cr.hunger, 0.8f - kScavengeFeedRate, 1.0e-5f);
    EXPECT_FLOAT_EQ(sc.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(sc.wish_z, 0.0f);
}

// THRESHOLD EDGE: a carcass EXACTLY at the feed radius distance is "in range" (bestD <=
// kScavengeFeedRadius is inclusive) -> feeds, not steers.
TEST(ScavengeHarden, FeedRadiusBoundaryInclusive) {
    using luminumbra::ai::kScavengeFeedRadius;
    using luminumbra::ai::RunScavengingOnTick;
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, 0.9f);
    spawnCarcass(r, kScavengeFeedRadius, 0.0f); // exactly at the feed boundary
    RunScavengingOnTick(r, 0);
    const auto& sc = r.get<Comp::ScavengerComponent>(s);
    EXPECT_EQ(sc.feeding, 1) << "exactly at feed radius is in range (<= inclusive)";
}

// A MortalComponent.dead==1 creature is ALSO a carcass (not just eaten==1). A scavenger seeks it.
TEST(ScavengeHarden, MortalDeadCountsAsCarcass) {
    using luminumbra::ai::RunScavengingOnTick;
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, 0.9f);
    // a creature dead via mortality (NOT eaten)
    auto dead = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(dead);
    tf.position = Luminumbra::Vec3(15.0f, 0.0f, 0.0f);
    r.emplace<Comp::CreatureComponent>(dead); // eaten=false
    r.emplace<Comp::MortalComponent>(dead).dead = 1;
    auto st = RunScavengingOnTick(r, 0);
    EXPECT_EQ(st.carcasses, 1);
    EXPECT_EQ(st.seeking, 1);
    const auto& sc = r.get<Comp::ScavengerComponent>(s);
    EXPECT_GT(std::abs(sc.wish_x) + std::abs(sc.wish_z), 0.0f);
}

// A DEAD scavenger (eaten) does not scavenge: zero wish, no feeding, even with a carcass next to
// it.
TEST(ScavengeHarden, DeadScavengerInert) {
    using luminumbra::ai::RunScavengingOnTick;
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.9f, /*eaten=*/true);
    spawnCarcass(r, 0.5f, 0.0f);
    RunScavengingOnTick(r, 0);
    const auto& sc = r.get<Comp::ScavengerComponent>(s);
    EXPECT_EQ(sc.feeding, 0);
    EXPECT_FLOAT_EQ(sc.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(sc.wish_z, 0.0f);
}

// A well-fed scavenger or one with no carcass writes a zero wish; the prior wish must be RESET
// (no stale steer persists across ticks).
TEST(ScavengeHarden, StaleWishResetWhenSated) {
    using luminumbra::ai::RunScavengingOnTick;
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.9f);
    spawnCarcass(r, 30.0f, 0.0f);
    RunScavengingOnTick(r, 0); // writes a non-zero wish
    ASSERT_GT(std::abs(r.get<Comp::ScavengerComponent>(s).wish_x), 0.0f);
    // Now sate it: hunger drops below threshold, re-run -> wish must reset to zero.
    r.get<Comp::CreatureComponent>(s).hunger = 0.0f;
    RunScavengingOnTick(r, 1);
    const auto& sc = r.get<Comp::ScavengerComponent>(s);
    EXPECT_FLOAT_EQ(sc.wish_x, 0.0f) << "a sated scavenger must reset its stale wish";
    EXPECT_FLOAT_EQ(sc.wish_z, 0.0f);
}

// BOUNDS: feeding never drives hunger below 0 (clamped).
TEST(ScavengeHarden, HungerClampedAtZeroWhileFeeding) {
    using luminumbra::ai::RunScavengingOnTick;
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.31f); // just above threshold
    spawnCarcass(r, 0.2f, 0.0f);                              // inside feed radius
    for (int i = 0; i < 50; ++i)
        RunScavengingOnTick(r, static_cast<std::uint64_t>(i));
    const auto& cr = r.get<Comp::CreatureComponent>(s);
    EXPECT_GE(cr.hunger, 0.0f);
}

// ORDER-INDEPENDENCE: nearest carcass + steer keyed by geometry, invariant to spawn order.
TEST(ScavengeHarden, OrderIndependent) {
    using luminumbra::ai::RunScavengingOnTick;
    struct Spec {
        float x, z;
        bool carcass;
        float hunger;
    };
    std::vector<Spec> base = {
        {0.0f, 0.0f, false, 0.9f},
        {20.0f, 0.0f, false, 0.9f},
        {5.0f, 0.0f, true, 0.0f},
        {25.0f, 3.0f, true, 0.0f},
    };
    auto build = [](std::vector<Spec> order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order) {
            es.push_back(sp.carcass ? spawnCarcass(r, sp.x, sp.z)
                                    : spawnScavenger(r, sp.x, sp.z, sp.hunger));
        }
        RunScavengingOnTick(r, 0);
        std::vector<std::tuple<float, float, float, float>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i].carcass)
                continue;
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            const auto& sc = r.get<Comp::ScavengerComponent>(es[i]);
            out.push_back({tf.position.x, tf.position.z, sc.wish_x, sc.wish_z});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto a = build(base);
    std::reverse(base.begin(), base.end());
    auto b = build(base);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(std::get<2>(a[i]), std::get<2>(b[i]));
        EXPECT_FLOAT_EQ(std::get<3>(a[i]), std::get<3>(b[i]));
    }
}

// ===========================================================================
// Flocking (pure ComputeFlockSteer)
// ===========================================================================

using luminumbra::ai::ComputeFlockSteer;
using luminumbra::ai::FlockParams;
using luminumbra::ai::FlockSteer;

// GATING: no neighbours -> zero steer (and finite).
TEST(FlockHarden, NoNeighborsZeroSteer) {
    FlockSteer s = ComputeFlockSteer(5.0f, -3.0f, {});
    EXPECT_FLOAT_EQ(s.x, 0.0f);
    EXPECT_FLOAT_EQ(s.z, 0.0f);
}

// EDGE: a neighbour at EXACTLY the self position (zero distance). The separation term is guarded
// (dist > 1e-5) so it contributes nothing; cohesion centroid == self -> toward ~0 -> guarded.
// Result must be finite (no 1/0).
TEST(FlockHarden, CoincidentNeighborNoNaN) {
    FlockSteer s = ComputeFlockSteer(2.0f, 2.0f, {{2.0f, 2.0f}});
    EXPECT_TRUE(isFiniteF(s.x));
    EXPECT_TRUE(isFiniteF(s.z));
}

// SEPARATION sign: a neighbour very close (inside separation_radius, off to +x) pushes self in
// the -x direction (AWAY). Guards a sign error in the separation accumulation.
TEST(FlockHarden, SeparationPushesAway) {
    FlockParams p; // separation_radius 3, neighbor_radius 12
    // self at origin, neighbour at +1 on x (inside separation radius)
    FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, {{1.0f, 0.0f}}, p);
    // Cohesion pulls toward +x (single neighbour centroid), separation pushes -x and dominates
    // up close (separation_weight 1.4 > cohesion_weight 0.6, crowd term large near touching).
    EXPECT_LT(s.x, 0.0f) << "a crowding neighbour must net-push self AWAY (separation dominates)";
}

// COHESION sign: a neighbour OUTSIDE the separation radius but inside the cohesion radius pulls
// self TOWARD it. (Only cohesion active.)
TEST(FlockHarden, CohesionPullsToward) {
    FlockParams p;
    // neighbour at +8 on x: beyond separation_radius (3), inside neighbor_radius (12).
    FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, {{8.0f, 0.0f}}, p);
    EXPECT_GT(s.x, 0.0f) << "cohesion must pull toward a far-but-in-range neighbour";
    EXPECT_NEAR(s.z, 0.0f, 1.0e-4f);
    // Cohesion-only magnitude == cohesion_weight (toward vector renormalised then * weight).
    const float mag = std::sqrt(s.x * s.x + s.z * s.z);
    EXPECT_NEAR(mag, p.cohesion_weight, 1.0e-4f);
}

// ORDER-INDEPENDENCE: the header claims the accumulation is "commutative -> independent of
// neighbour order". Float addition is commutative but NOT associative, so a reversed list can
// differ by an ULP; we assert agreement to a tight fp tolerance (the documented determinism is
// "same caller-supplied list -> same steer"; the brain supplies neighbours in a fixed order).
// If this fails by MORE than fp noise, the steer is genuinely order-sensitive (a real defect).
TEST(FlockHarden, NeighborOrderStable) {
    std::vector<std::pair<float, float>> ns = {
        {1.0f, 0.5f}, {-2.0f, 1.0f}, {0.5f, -3.0f}, {2.5f, 2.5f}, {-1.0f, -1.0f}};
    FlockSteer a = ComputeFlockSteer(0.0f, 0.0f, ns);
    std::reverse(ns.begin(), ns.end());
    FlockSteer b = ComputeFlockSteer(0.0f, 0.0f, ns);
    EXPECT_NEAR(a.x, b.x, 1.0e-5f) << "ComputeFlockSteer must be ~order-independent (fp tolerance)";
    EXPECT_NEAR(a.z, b.z, 1.0e-5f);
}

// run==replay: the SAME caller-supplied neighbour list always yields a bit-identical steer.
TEST(FlockHarden, RunEqualsReplay) {
    std::vector<std::pair<float, float>> ns = {
        {1.0f, 0.5f}, {-2.0f, 1.0f}, {0.5f, -3.0f}, {2.5f, 2.5f}};
    FlockSteer a = ComputeFlockSteer(1.0f, -1.0f, ns);
    FlockSteer b = ComputeFlockSteer(1.0f, -1.0f, ns);
    EXPECT_EQ(a.x, b.x);
    EXPECT_EQ(a.z, b.z);
}

// BOUNDS: degenerate params (zero radii) must not produce NaN/inf.
TEST(FlockHarden, ZeroRadiiNoNaN) {
    FlockParams p;
    p.neighbor_radius = 0.0f;
    p.separation_radius = 0.0f;
    FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, {{1.0f, 1.0f}, {-1.0f, 2.0f}}, p);
    EXPECT_TRUE(isFiniteF(s.x));
    EXPECT_TRUE(isFiniteF(s.z));
}

// ===========================================================================
// SteeringConsumer (PRODUCER->CONSUMER blend)
// ===========================================================================

// CONTRACT (the bug class this seam exists to catch): the pack coord is read as a UNIT STEER
// DIRECTION and multiplied by move_speed*1.5 — NOT treated as a world point (coord - position).
// Spawn FAR from origin: a point-misread would yield a wildly different, origin-ward wish.
TEST(ConsumerHarden, PackCoordIsDirectionNotPoint) {
    using luminumbra::ai::RunSteeringConsumerOnTick;
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(900.0f, 0.0f, -450.0f);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = true;
    cr.move_speed = 5.0f;
    auto& pk = r.emplace<Comp::PackHunterComponent>(e);
    pk.in_pack = 1;
    pk.coord_x = 0.6f;
    pk.coord_z = 0.8f; // unit direction
    RunSteeringConsumerOnTick(r);
    EXPECT_FLOAT_EQ(cr.wish_x, 0.6f * 5.0f * 1.5f);
    EXPECT_FLOAT_EQ(cr.wish_z, 0.8f * 5.0f * 1.5f);
}

// A pack predator with a ZERO coord (no target this tick) is NOT pack-steered (the guard is
// in_pack && (coord_x!=0 || coord_z!=0)) — the brain's wish is left intact.
TEST(ConsumerHarden, ZeroCoordPackNotSteered) {
    using luminumbra::ai::RunSteeringConsumerOnTick;
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = true;
    cr.wish_x = 9.0f; // brain value
    auto& pk = r.emplace<Comp::PackHunterComponent>(e);
    pk.in_pack = 1;
    pk.coord_x = 0.0f;
    pk.coord_z = 0.0f;
    auto s = RunSteeringConsumerOnTick(r);
    EXPECT_EQ(s.packed, 0);
    EXPECT_FLOAT_EQ(cr.wish_x, 9.0f) << "a zero-coord pack must not clobber the brain wish";
}

// Migration + territory ADD on top of the pack-overridden wish (they are additive drifts). The
// pack term OVERRIDES (sets) the wish, then migration/territory ADD. Assert the exact compose.
TEST(ConsumerHarden, PackOverrideThenMigrationTerritoryAdd) {
    using luminumbra::ai::RunSteeringConsumerOnTick;
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = true;
    cr.move_speed = 4.0f;
    cr.wish_x = 100.0f; // brain value — must be OVERWRITTEN by the pack term (not added to)
    cr.wish_z = 100.0f;
    auto& pk = r.emplace<Comp::PackHunterComponent>(e);
    pk.in_pack = 1;
    pk.coord_x = 1.0f;
    pk.coord_z = 0.0f;
    auto& mig = r.emplace<Comp::MigratoryComponent>(e);
    mig.wish_x = 0.5f;
    mig.wish_z = -0.25f;
    auto& tb = r.emplace<Comp::TerritoryBiasComponent>(e);
    tb.wish_x = 0.1f;
    tb.wish_z = 0.2f;
    RunSteeringConsumerOnTick(r);
    const float packX = 1.0f * 4.0f * 1.5f; // = 6
    EXPECT_FLOAT_EQ(cr.wish_x, packX + 0.5f + 0.1f);
    EXPECT_FLOAT_EQ(cr.wish_z, 0.0f - 0.25f + 0.2f);
}

// Migration/territory ADD to the brain wish for a NON-pack creature (no override). Prey path.
TEST(ConsumerHarden, PreyAdditiveBias) {
    using luminumbra::ai::RunSteeringConsumerOnTick;
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    cr.wish_x = 2.0f;
    cr.wish_z = -1.0f;
    r.emplace<Comp::MigratoryComponent>(e).wish_x = 0.3f;
    r.emplace<Comp::TerritoryBiasComponent>(e).wish_z = 0.4f;
    RunSteeringConsumerOnTick(r);
    EXPECT_FLOAT_EQ(cr.wish_x, 2.0f + 0.3f);
    EXPECT_FLOAT_EQ(cr.wish_z, -1.0f + 0.4f);
}

// A carcass (eaten) is wholly inert: NONE of pack/migration/territory touch its wish.
TEST(ConsumerHarden, CarcassFullyInert) {
    using luminumbra::ai::RunSteeringConsumerOnTick;
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = true;
    cr.eaten = true;
    cr.wish_x = 3.0f;
    cr.wish_z = 4.0f;
    auto& pk = r.emplace<Comp::PackHunterComponent>(e);
    pk.in_pack = 1;
    pk.coord_x = 1.0f;
    r.emplace<Comp::MigratoryComponent>(e).wish_x = 5.0f;
    r.emplace<Comp::TerritoryBiasComponent>(e).wish_x = 5.0f;
    auto s = RunSteeringConsumerOnTick(r);
    EXPECT_EQ(s.packed, 0);
    EXPECT_EQ(s.migrated, 0);
    EXPECT_EQ(s.homed, 0);
    EXPECT_FLOAT_EQ(cr.wish_x, 3.0f);
    EXPECT_FLOAT_EQ(cr.wish_z, 4.0f);
}

// ORDER-INDEPENDENCE: each creature writes only its OWN wish, so shuffling creation order is
// irrelevant — result keyed by identity.
TEST(ConsumerHarden, OrderIndependent) {
    using luminumbra::ai::RunSteeringConsumerOnTick;
    auto build = [](bool reversed) {
        entt::registry r;
        struct E {
            float mx;
            float tx;
            bool pred;
            float coordx;
        };
        std::vector<E> specs = {
            {0.1f, 0.2f, false, 0.0f}, {0.3f, -0.1f, true, 0.5f}, {-0.2f, 0.05f, false, 0.0f}};
        if (reversed)
            std::reverse(specs.begin(), specs.end());
        std::vector<std::pair<float, std::pair<float, float>>> key;
        for (const auto& sp : specs) {
            auto e = r.create();
            r.emplace<Comp::TransformComponent>(e);
            auto& cr = r.emplace<Comp::CreatureComponent>(e);
            cr.is_predator = sp.pred;
            cr.move_speed = 4.0f;
            r.emplace<Comp::MigratoryComponent>(e).wish_x = sp.mx;
            r.emplace<Comp::TerritoryBiasComponent>(e).wish_x = sp.tx;
            if (sp.pred) {
                auto& pk = r.emplace<Comp::PackHunterComponent>(e);
                pk.in_pack = 1;
                pk.coord_x = sp.coordx;
            }
        }
        RunSteeringConsumerOnTick(r);
        std::vector<std::pair<float, float>> out; // key by (mx) -> wish_x
        auto v = r.view<Comp::CreatureComponent, Comp::MigratoryComponent>();
        for (auto e : v) {
            out.push_back({v.get<Comp::MigratoryComponent>(e).wish_x,
                           v.get<Comp::CreatureComponent>(e).wish_x});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto a = build(false);
    auto b = build(true);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i].first, b[i].first);
        EXPECT_FLOAT_EQ(a[i].second, b[i].second);
    }
}

// ===========================================================================
// CROSS-SYSTEM: full producer->consumer chain (pack/migration/territory -> consumer)
// ===========================================================================

// Run the actual producers, then the consumer, and assert the consumer's wish is consistent
// with the producers' UNIT/scaled outputs (no double-scaling, no point-misread end to end).
TEST(CrossHarden, ProducersThenConsumerConsistent) {
    using luminumbra::ai::RunMigrationOnTick;
    using luminumbra::ai::RunPredatorPackOnTick;
    using luminumbra::ai::RunSteeringConsumerOnTick;
    using luminumbra::ai::RunTerritoryOnTick;
    entt::registry r;
    // Two predators forming a pack, far from origin, with prey ahead.
    auto p0 = spawnPredator(r, 500.0f, 500.0f);
    auto p1 = spawnPredator(r, 503.0f, 500.0f);
    r.get<Comp::CreatureComponent>(p0).move_speed = 4.0f;
    r.get<Comp::CreatureComponent>(p1).move_speed = 4.0f;
    spawnPrey(r, 500.0f, 540.0f);
    // Give p0 a migration + territory bias too.
    r.emplace<Comp::MigratoryComponent>(p0);
    auto& terr = r.emplace<Comp::TerritoryComponent>(p0);
    terr.radius = 20.0f;

    // Producers (slot 7), then consumer.
    RunPredatorPackOnTick(r, 0);
    RunMigrationOnTick(r, 0.0f); // peak drive
    RunTerritoryOnTick(r, 0);    // claims home at current pos -> bias 0
    RunSteeringConsumerOnTick(r);

    const auto& pk = r.get<Comp::PackHunterComponent>(p0);
    const auto& mig = r.get<Comp::MigratoryComponent>(p0);
    const auto& tb = r.get<Comp::TerritoryBiasComponent>(p0);
    const auto& cr = r.get<Comp::CreatureComponent>(p0);
    // Expected: pack override (coord*speed*1.5) + migration wish + territory bias (0 just-claimed).
    const float sp = cr.move_speed * 1.5f;
    EXPECT_NEAR(cr.wish_x, pk.coord_x * sp + mig.wish_x + tb.wish_x, 1.0e-3f);
    EXPECT_NEAR(cr.wish_z, pk.coord_z * sp + mig.wish_z + tb.wish_z, 1.0e-3f);
    // Territory bias is zero on the claiming tick.
    EXPECT_FLOAT_EQ(tb.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(tb.wish_z, 0.0f);
    // The pack term is unit-scaled: its contribution magnitude == sp (coord is unit).
    const float packMag = std::sqrt(pk.coord_x * pk.coord_x + pk.coord_z * pk.coord_z) * sp;
    EXPECT_NEAR(packMag, sp, 1.0e-2f);
}

} // namespace
