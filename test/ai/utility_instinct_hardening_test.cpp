// ADVERSARIAL determinism / contract-hardening coverage for the DECISION ARBITER
// cluster: the IAUS UtilityAI scorer + SelectAction tiebreak, the CreatureBrain
// (senses -> action) mapping, the live CreatureBrainSystem tick (senses snapshot,
// flee/hunt direction, wish-VELOCITY scale, gating no-op, run==replay), the
// InstinctLocomotionSystem seek/arrival/flee executor, and the Awareness fusion
// state machine. These tests hunt for producer->consumer unit/meaning mismatches,
// threshold edges, coincident/zero-distance degeneracies, order-independence,
// gating no-ops, NaN/inf survival, and EXACT run==replay.
//
// Style mirrors the sibling happy-path tests in this directory (ai/ include root,
// `namespace Comp =::Luminumbra::Components` inside the anon namespace).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <entt/entt.hpp>

#include "ai/Awareness.h"
#include "ai/CreatureBrain.h"
#include "ai/CreatureBrainSystem.h"
#include "ai/InstinctLocomotionSystem.h"
#include "ai/UtilityAI.h"
#include "components/AlarmComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/InstinctComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;

using luminumbra::ai::Awareness;
using luminumbra::ai::AwarenessParams;
using luminumbra::ai::AwarenessState;
using luminumbra::ai::Consideration;
using luminumbra::ai::CreatureAction;
using luminumbra::ai::CreatureSenses;
using luminumbra::ai::CurveType;
using luminumbra::ai::DecideCreatureAction;
using luminumbra::ai::RunCreatureBrainSystemOnTick;
using luminumbra::ai::RunInstinctLocomotionOnTick;
using luminumbra::ai::SelectAction;
using luminumbra::ai::UpdateAwareness;
using luminumbra::ai::utility_clamp01;
using luminumbra::ai::UtilityAction;

constexpr float kDt = 1.0f / 30.0f;

Consideration con(float input, CurveType curve, float m = 1.0f, float b = 0.0f, float c = 0.0f) {
    Consideration k;
    k.input = input;
    k.curve = curve;
    k.m = m;
    k.b = b;
    k.c = c;
    return k;
}

UtilityAction act(int id, float weight, std::vector<Consideration> cons = {}) {
    UtilityAction a;
    a.id = id;
    a.weight = weight;
    a.considerations = std::move(cons);
    return a;
}

CreatureSenses prey(float hunger, float threat, float food, float stamina) {
    CreatureSenses s;
    s.is_predator = false;
    s.hunger = hunger;
    s.threat_proximity = threat;
    s.food_proximity = food;
    s.stamina = stamina;
    return s;
}

entt::entity spawn(
    entt::registry& r, float x, float z, bool predator, float hunger = 0.0f, float stamina = 1.0f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = predator;
    cr.hunger = hunger;
    cr.stamina = stamina;
    return e;
}

float xOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::TransformComponent>(e).position.x;
}
float zOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::TransformComponent>(e).position.z;
}

float Len2(const Luminumbra::Vec2& v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

// ===========================================================================
// 1. UtilityAI scorer: curve bounds, clamp invariants, compensation, NaN/inf.
// ===========================================================================

// Every curve output MUST be clamped to [0,1] even for hostile shape params
// (large m, negative b, c outside [0,1]) so one consideration can never make a
// score exceed 1 or go negative. (consumer: Score/SelectAction assume [0,1].)
TEST(UtilityHardening, CurvesAlwaysClampedForHostileParams) {
    const CurveType curves[] = {
        CurveType::Linear, CurveType::InvLinear, CurveType::Quadratic, CurveType::Logistic};
    const float ms[] = {-50.0f, -1.0f, 0.0f, 1.0f, 100.0f};
    const float bs[] = {-5.0f, 0.0f, 5.0f};
    const float cs[] = {-2.0f, 0.0f, 0.5f, 2.0f};
    for (CurveType cv : curves)
        for (float m : ms)
            for (float b : bs)
                for (float c : cs)
                    for (int i = 0; i <= 8; ++i) {
                        const float x = static_cast<float>(i) / 8.0f;
                        const float v = con(x, cv, m, b, c).Evaluate();
                        EXPECT_GE(v, 0.0f) << "curve below 0";
                        EXPECT_LE(v, 1.0f) << "curve above 1";
                        EXPECT_FALSE(std::isnan(v)) << "curve produced NaN";
                    }
}

// Out-of-range INPUT (negative / >1) is clamped before the curve sees it, so a
// bad sense value can't blow up scoring.
TEST(UtilityHardening, OutOfRangeInputClampedInsideCurve) {
    EXPECT_FLOAT_EQ(con(-3.0f, CurveType::Linear).Evaluate(),
                    con(0.0f, CurveType::Linear).Evaluate());
    EXPECT_FLOAT_EQ(con(7.0f, CurveType::Linear).Evaluate(),
                    con(1.0f, CurveType::Linear).Evaluate());
}

// A NaN input must not propagate to a NaN score (would poison SelectAction's
// comparisons). utility_clamp01 of NaN: NaN<0 is false, NaN>1 is false -> NaN.
// This documents the contract that scores stay finite; if it fails the clamp
// needs a NaN guard.
TEST(UtilityHardening, NanInputDoesNotProduceNanScore) {
    UtilityAction a =
        act(1, 1.0f, {con(std::numeric_limits<float>::quiet_NaN(), CurveType::Linear)});
    const float s = a.Score();
    EXPECT_FALSE(std::isnan(s)) << "NaN sense leaked into the utility score";
}

// Score is clamped to [0,1] for any weight (weight > 1 must not yield > 1).
TEST(UtilityHardening, ScoreClampedForOversizedWeight) {
    UtilityAction a = act(1, 9.0f, {con(1.0f, CurveType::Linear)});
    EXPECT_LE(a.Score(), 1.0f);
    EXPECT_GE(a.Score(), 0.0f);
}

// A zero-consideration action scores exactly clamp01(weight) (the baseline path).
TEST(UtilityHardening, EmptyConsiderationScoreIsClampedWeight) {
    EXPECT_FLOAT_EQ(act(1, 0.2f, {}).Score(), 0.2f);
    EXPECT_FLOAT_EQ(act(1, 1.5f, {}).Score(), 1.0f);
}

// A single zero-valued consideration collapses the action to 0 (no compensation
// rescue with n==1: modFactor = 1 - 1/1 = 0).
TEST(UtilityHardening, SingleZeroConsiderationCollapsesToZero) {
    EXPECT_FLOAT_EQ(act(1, 1.0f, {con(0.0f, CurveType::Linear)}).Score(), 0.0f);
}

// Scoring is a pure function: same action scored twice is bit-identical.
TEST(UtilityHardening, ScoreIsBitDeterministic) {
    UtilityAction a = act(3,
                          0.77f,
                          {con(0.31f, CurveType::Logistic, 2.0f, 0.0f, 0.45f),
                           con(0.62f, CurveType::Quadratic, 1.3f, 0.1f, 0.5f)});
    EXPECT_EQ(a.Score(), a.Score());
}

// ===========================================================================
// 2. SelectAction arbiter: ties, order-independence, higher-utility wins, edges.
// ===========================================================================

// Ties resolve to the LOWEST id regardless of input order (canonical tiebreak ->
// run==replay). Tested with three equal-scoring actions in both orders.
TEST(UtilityHardening, TieResolvesToLowestIdOrderIndependent) {
    UtilityAction a = act(7, 0.5f);
    UtilityAction b = act(3, 0.5f);
    UtilityAction c = act(5, 0.5f);
    EXPECT_EQ(SelectAction({a, b, c}), 3);
    EXPECT_EQ(SelectAction({c, b, a}), 3);
    EXPECT_EQ(SelectAction({b, c, a}), 3);
}

// The arbiter must pick the action with the strictly HIGHER score, never a
// lower-utility one at any margin. (Two actions, tiny score gap.)
TEST(UtilityHardening, HigherUtilityAlwaysWinsEvenAtTinyMargin) {
    UtilityAction hi = act(2, 0.5001f);
    UtilityAction lo = act(1, 0.5000f); // lower id but lower score -> must lose
    EXPECT_EQ(SelectAction({lo, hi}), 2);
    EXPECT_EQ(SelectAction({hi, lo}), 2);
}

// A higher-id action with the unique highest score beats every lower-id action
// (id only breaks EXACT ties, it is not a priority).
TEST(UtilityHardening, IdDoesNotOverrideScore) {
    UtilityAction lo0 = act(0, 0.1f);
    UtilityAction lo1 = act(1, 0.1f);
    UtilityAction best = act(9, 0.9f);
    EXPECT_EQ(SelectAction({lo0, lo1, best}), 9);
    EXPECT_EQ(SelectAction({best, lo1, lo0}), 9);
}

// Empty action set -> -1 sentinel (consumers map this to Wander/0).
TEST(UtilityHardening, EmptySelectionIsNegativeOne) {
    EXPECT_EQ(SelectAction({}), -1);
}

// All-zero-scoring actions still pick the lowest id (score 0 == 0 ties).
TEST(UtilityHardening, AllZeroScoresPickLowestId) {
    UtilityAction a = act(4, 1.0f, {con(0.0f, CurveType::Linear)});
    UtilityAction b = act(2, 1.0f, {con(0.0f, CurveType::Linear)});
    EXPECT_EQ(SelectAction({a, b}), 2);
    EXPECT_EQ(SelectAction({b, a}), 2);
}

// ===========================================================================
// 3. CreatureBrain: senses -> action mapping, threshold edges, gating by role.
// ===========================================================================

// SCORED FIXTURE (headline): the higher-utility action wins. A near predator
// dominates a hungry+fed prey -> Flee, not Graze.
TEST(UtilityHardening, BrainPreyFleesWhenThreatDominates) {
    EXPECT_EQ(DecideCreatureAction(prey(0.95f, 0.95f, 0.95f, 0.9f)), CreatureAction::Flee);
}

// Same prey, threat removed -> Graze now wins (the threat axis no longer
// collapses graze and inflates flee).
TEST(UtilityHardening, BrainPreyGrazesWhenThreatRemoved) {
    EXPECT_EQ(DecideCreatureAction(prey(0.95f, 0.0f, 0.95f, 0.9f)), CreatureAction::Graze);
}

// GATING: a predator never selects prey-only actions (Flee/Graze). Even hungry
// with food near and a (self-inflicted) threat field, the predator picks a
// predator action (Hunt/Wander/Rest), never id 1 (Graze) or 2 (Flee).
TEST(UtilityHardening, BrainPredatorNeverSelectsPreyActions) {
    CreatureSenses s;
    s.is_predator = true;
    s.hunger = 0.9f;
    s.food_proximity = 0.9f;
    s.threat_proximity = 0.9f; // would matter only for a prey
    s.stamina = 0.9f;
    const CreatureAction a = DecideCreatureAction(s);
    EXPECT_NE(a, CreatureAction::Graze);
    EXPECT_NE(a, CreatureAction::Flee);
}

// GATING: a prey never Hunts (id 3 is predator-only in the action set).
TEST(UtilityHardening, BrainPreyNeverHunts) {
    for (int hi = 0; hi <= 10; ++hi)
        for (int fi = 0; fi <= 10; ++fi) {
            const float h = hi / 10.0f, f = fi / 10.0f;
            EXPECT_NE(DecideCreatureAction(prey(h, 0.0f, f, 1.0f)), CreatureAction::Hunt);
        }
}

// A starving predator with prey adjacent but ZERO stamina cannot Hunt (the
// stamina consideration collapses Hunt's product); it must not chase on empty.
TEST(UtilityHardening, BrainExhaustedPredatorDoesNotHunt) {
    CreatureSenses s;
    s.is_predator = true;
    s.hunger = 1.0f;
    s.food_proximity = 1.0f;
    s.stamina = 0.0f;
    EXPECT_NE(DecideCreatureAction(s), CreatureAction::Hunt);
}

// Decision is a pure function across the whole sense grid (run==replay at the
// brain level).
TEST(UtilityHardening, BrainDecisionPureOverGrid) {
    for (int i = 0; i <= 6; ++i)
        for (int j = 0; j <= 6; ++j) {
            const CreatureSenses s = prey(i / 6.0f, j / 6.0f, 0.5f, 0.5f);
            EXPECT_EQ(DecideCreatureAction(s), DecideCreatureAction(s));
        }
}

// ===========================================================================
// 4. CreatureBrainSystem tick: gating no-op, direction, wish-VELOCITY scale,
//    order-independence, coincident degeneracy, run==replay.
// ===========================================================================

// GATING NO-OP: a registry with no CreatureComponent leaves the system a no-op
// (updated == 0) so the canonical roster stays byte-identical.
TEST(UtilityHardening, SystemEmptyRosterNoOp) {
    entt::registry r;
    (void)r.create(); // a bare entity with no creature component
    EXPECT_EQ(RunCreatureBrainSystemOnTick(r, kDt).updated, 0);
}

// A lone prey with no predator anywhere senses no threat -> it Wanders, and its
// wish VELOCITY magnitude equals move_speed (NOT the 1.5x flee/hunt sprint).
// Verifies the wish is a velocity in m/s at cruise scale.
TEST(UtilityHardening, LoneWandererWishIsCruiseSpeed) {
    entt::registry r;
    const auto e = spawn(r, 0.0f, 0.0f, /*predator=*/false);
    auto& cr = r.get<Comp::CreatureComponent>(e);
    cr.move_speed = 4.0f;
    RunCreatureBrainSystemOnTick(r, kDt);
    const auto& c = r.get<Comp::CreatureComponent>(e);
    EXPECT_EQ(c.last_action, static_cast<int>(CreatureAction::Wander));
    const float wishLen = std::sqrt(c.wish_x * c.wish_x + c.wish_z * c.wish_z);
    EXPECT_NEAR(wishLen, 4.0f, 1e-3f) << "wander wish should be exactly cruise move_speed";
}

// Flee/hunt sprint at 1.5x cruise. A hungry predator hunting nearby prey moves
// at 1.5*move_speed, THEN scaled by 's starvation degradation (a
// starving animal is genuinely weaker — CreatureBrainSystem.h "needs have
// consequences, "). Because the hunt hunger (0.95) sits inside the 0.85-1.0
// degradation band, we recover and assert the pure 1.5x sprint MULTIPLIER by
// dividing out the known starve_degrade — so this verifies the sprint contract
// the test name asserts, independent of the (separately tested) starvation
// weakening. starve_degrade formula mirrors CreatureBrainSystem.h:200-201.
TEST(UtilityHardening, HuntWishIsOnePointFiveCruise) {
    entt::registry r;
    constexpr float kHunger = 0.95f;
    const auto pred = spawn(r, 0.0f, 0.0f, /*predator=*/true, /*hunger=*/kHunger);
    r.get<Comp::CreatureComponent>(pred).move_speed = 2.0f;
    // prey placed beyond catch reach so the predator chases rather than eating.
    spawn(r, 8.0f, 0.0f, /*predator=*/false);
    RunCreatureBrainSystemOnTick(r, kDt);
    const auto& c = r.get<Comp::CreatureComponent>(pred);
    ASSERT_EQ(c.last_action, static_cast<int>(CreatureAction::Hunt));
    const float wishLen = std::sqrt(c.wish_x * c.wish_x + c.wish_z * c.wish_z);
    const float starve01 = std::clamp((kHunger - 0.85f) / 0.15f, 0.0f, 1.0f);
    const float starve_degrade = 1.0f - 0.5f * starve01; // 0.667 at hunger 0.95
    const float sprintMult = wishLen / (2.0f * starve_degrade);
    EXPECT_NEAR(sprintMult, 1.5f, 1e-3f)
        << "hunt should sprint at 1.5x cruise (starvation-degraded per )";
}

// DIRECTION: prey flees AWAY from a predator on +x (moves toward -x), single tick.
TEST(UtilityHardening, FleeDirectionIsAwayFromThreat) {
    entt::registry r;
    const auto prey_e = spawn(r, 0.0f, 0.0f, false);
    spawn(r, 3.0f, 0.0f, true, /*hunger=*/0.8f);
    RunCreatureBrainSystemOnTick(r, kDt);
    const auto& c = r.get<Comp::CreatureComponent>(prey_e);
    ASSERT_EQ(c.last_action, static_cast<int>(CreatureAction::Flee));
    EXPECT_LT(c.wish_x, 0.0f) << "prey must flee in -x away from the +x predator";
}

// COINCIDENT DEGENERACY: a predator exactly on top of its prey (zero distance).
// The catch fires (within reach) and nothing produces NaN; the carcass is inert.
TEST(UtilityHardening, CoincidentPredatorPreyNoNaN) {
    entt::registry r;
    const auto pred = spawn(r, 5.0f, 5.0f, true, /*hunger=*/0.95f);
    const auto prey_e = spawn(r, 5.0f, 5.0f, false); // exactly coincident
    RunCreatureBrainSystemOnTick(r, kDt);
    const auto& pc = r.get<Comp::CreatureComponent>(pred);
    EXPECT_FALSE(std::isnan(pc.wish_x));
    EXPECT_FALSE(std::isnan(pc.wish_z));
    EXPECT_FALSE(std::isnan(xOf(r, pred)));
    EXPECT_TRUE(r.get<Comp::CreatureComponent>(prey_e).eaten)
        << "adjacent (coincident) prey is caught";
}

// ORDER INDEPENDENCE: the per-creature result must not depend on the order in
// which the (sorted) traversal updates creatures. The pre-tick SNAPSHOT is the
// mechanism: every creature senses the SAME pre-tick state regardless of who
// moves first. We verify this directly by spawning the predator BEFORE the prey
// in one registry and AFTER in another (same positions, a Flee/Hunt chase that
// uses position-based directions, not the id-seeded wander). The chase outcome
// (prey flees -x, predator advances +x) must hold either way and the displacement
// magnitudes must match, because neither creature's flee/hunt direction depends
// on traversal order.
TEST(UtilityHardening, SystemSnapshotMakesUpdateOrderIrrelevant) {
    auto chase = [](bool predator_first) {
        entt::registry r;
        entt::entity prey_e, pred_e;
        if (predator_first) {
            pred_e = spawn(r, 6.0f, 0.0f, true, 0.8f);
            prey_e = spawn(r, 0.0f, 0.0f, false);
        } else {
            prey_e = spawn(r, 0.0f, 0.0f, false);
            pred_e = spawn(r, 6.0f, 0.0f, true, 0.8f);
        }
        // A single tick: each acts on the same pre-tick snapshot. The lone-pair
        // case has no same-role herd neighbor, so flee/hunt headings are purely
        // position-derived and order-free.
        RunCreatureBrainSystemOnTick(r, kDt);
        const auto& pc = r.get<Comp::CreatureComponent>(prey_e);
        const auto& dc = r.get<Comp::CreatureComponent>(pred_e);
        return std::vector<float>{pc.wish_x, pc.wish_z, dc.wish_x, dc.wish_z};
    };
    EXPECT_EQ(chase(true), chase(false))
        << "update order changed the wish; the pre-tick snapshot is not isolating order";
}

// run==replay EXACT: identical setup + many ticks -> flattened float snapshot is
// EXACTLY equal across two independent runs.
TEST(UtilityHardening, SystemRunEqualsReplayExact) {
    auto run = [] {
        entt::registry r;
        const auto a = spawn(r, 0.0f, 0.0f, false, 0.2f);
        const auto b = spawn(r, 5.0f, 2.0f, true, 0.8f);
        const auto c = spawn(r, -3.0f, -1.0f, false, 0.5f);
        for (int i = 0; i < 90; ++i)
            RunCreatureBrainSystemOnTick(r, kDt);
        std::vector<float> snap;
        for (auto e : {a, b, c}) {
            const auto& cr = r.get<Comp::CreatureComponent>(e);
            snap.push_back(xOf(r, e));
            snap.push_back(zOf(r, e));
            snap.push_back(cr.hunger);
            snap.push_back(cr.stamina);
            snap.push_back(cr.wish_x);
            snap.push_back(cr.wish_z);
        }
        return snap;
    };
    EXPECT_EQ(run(), run());
}

// GATING NO-OP for the optional AlarmComponent: a prey WITHOUT an AlarmComponent
// must behave byte-identically to the same scene where the alarm path is simply
// absent (the component is the opt-in; no component -> untouched + same result).
TEST(UtilityHardening, AlarmComponentGatingByteIdentical) {
    auto run = [](bool with_alarm) {
        entt::registry r;
        const auto prey_e = spawn(r, 0.0f, 0.0f, false);
        spawn(r, 20.0f, 0.0f, true, 0.5f); // distant predator: nearNorm small
        if (with_alarm)
            r.emplace<Comp::AlarmComponent>(prey_e); // level 0, alarmed 0
        for (int i = 0; i < 10; ++i)
            RunCreatureBrainSystemOnTick(r, kDt);
        const auto& c = r.get<Comp::CreatureComponent>(prey_e);
        return std::vector<float>{xOf(r, prey_e), zOf(r, prey_e), c.wish_x, c.wish_z};
    };
    // A zero-level/zero-alarmed AlarmComponent must not change behaviour vs none
    // when the predator is too far to trip the alarm threshold.
    EXPECT_EQ(run(false), run(true));
}

// An already-eaten carcass is inert: zero wish, no movement, across many ticks.
TEST(UtilityHardening, EatenCarcassStaysPutWithZeroWish) {
    entt::registry r;
    const auto prey_e = spawn(r, 2.0f, -2.0f, false);
    spawn(r, 4.0f, -2.0f, true, 0.9f); // a predator it would otherwise flee
    r.get<Comp::CreatureComponent>(prey_e).eaten = true;
    for (int i = 0; i < 30; ++i)
        RunCreatureBrainSystemOnTick(r, kDt);
    const auto& c = r.get<Comp::CreatureComponent>(prey_e);
    EXPECT_NEAR(xOf(r, prey_e), 2.0f, 1e-4f);
    EXPECT_NEAR(zOf(r, prey_e), -2.0f, 1e-4f);
    EXPECT_FLOAT_EQ(c.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(c.wish_z, 0.0f);
}

// One prey can sate at most ONE predator per tick: two predators coincident on a
// single prey, only one gets the satiation (the other re-checks the live flag).
TEST(UtilityHardening, OnePreySatesAtMostOnePredator) {
    entt::registry r;
    const auto p1 = spawn(r, 0.0f, 0.0f, true, 0.95f);
    const auto p2 = spawn(r, 0.0f, 0.0f, true, 0.95f);
    spawn(r, 0.5f, 0.0f, false); // within catch reach of both
    RunCreatureBrainSystemOnTick(r, kDt);
    const float h1 = r.get<Comp::CreatureComponent>(p1).hunger;
    const float h2 = r.get<Comp::CreatureComponent>(p2).hunger;
    // Exactly one predator should have dropped its hunger by the catch satiation.
    const bool p1_ate = h1 < 0.9f;
    const bool p2_ate = h2 < 0.9f;
    EXPECT_NE(p1_ate, p2_ate) << "exactly one predator may feed on a single prey per tick";
}

// ===========================================================================
// 5. InstinctLocomotionSystem: seek scale, arrival edge, flee, gating, zero-dist.
// ===========================================================================

entt::entity MakeLocoTarget(entt::registry& reg, float x, float z) {
    const auto e = reg.create();
    auto& tf = reg.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    return e;
}

entt::entity MakeLocoAgent(entt::registry& reg,
                           float x,
                           float z,
                           entt::entity target,
                           Comp::LocomotionProfile profile = {},
                           bool flee = false) {
    const auto e = reg.create();
    auto& tf = reg.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    reg.emplace<Comp::LocomotionProfile>(e) = profile;
    auto& plan = reg.emplace<Comp::ActionPlanComponent>(e);
    Comp::Action a;
    a.name = "approach";
    a.target = target;
    a.flee = flee;
    plan.plan.push_back(a);
    return e;
}

// Far seek: wish is a VELOCITY at exactly move_speed pointing at the target.
TEST(UtilityHardening, LocoSeekWishIsMoveSpeed) {
    entt::registry reg;
    const auto t = MakeLocoTarget(reg, 100.0f, 0.0f);
    Comp::LocomotionProfile p;
    p.move_speed = 3.0f;
    p.arrival_radius = 1.0f;
    p.slow_radius = 4.0f;
    const auto a = MakeLocoAgent(reg, 0.0f, 0.0f, t, p);
    RunInstinctLocomotionOnTick(reg);
    const auto& w = reg.get<Comp::LocomotionIntentComponent>(a).wish_xz;
    EXPECT_NEAR(Len2(w), 3.0f, 1e-4f);
    EXPECT_NEAR(w.x, 3.0f, 1e-4f);
}

// THRESHOLD EDGE: dist exactly == arrival_radius counts as ARRIVED (dist <=
// arrival_radius), so the agent holds and the plan advances. (Boundary is
// inclusive on the arrive side.)
TEST(UtilityHardening, LocoArrivalRadiusBoundaryArrives) {
    entt::registry reg;
    Comp::LocomotionProfile p;
    p.move_speed = 3.0f;
    p.arrival_radius = 2.0f;
    p.slow_radius = 4.0f;
    const auto t = MakeLocoTarget(reg, 2.0f, 0.0f); // dist == arrival_radius exactly
    const auto a = MakeLocoAgent(reg, 0.0f, 0.0f, t, p);
    const auto stats = RunInstinctLocomotionOnTick(reg);
    EXPECT_EQ(stats.agents_arrived, 1u);
    EXPECT_TRUE(reg.get<Comp::LocomotionIntentComponent>(a).arrived);
    EXPECT_EQ(reg.get<Comp::ActionPlanComponent>(a).current_action_index, 1u);
}

// GATING: an action whose target component is ABSENT (entt::null target) -> idle,
// zero wish, no crash. (precondition missing -> no steer.)
TEST(UtilityHardening, LocoNullTargetIsIdleZeroWish) {
    entt::registry reg;
    const auto a = MakeLocoAgent(reg, 0.0f, 0.0f, entt::null);
    const auto stats = RunInstinctLocomotionOnTick(reg);
    EXPECT_EQ(stats.agents_idle, 1u);
    EXPECT_EQ(stats.agents_steered, 0u);
    EXPECT_NEAR(Len2(reg.get<Comp::LocomotionIntentComponent>(a).wish_xz), 0.0f, 1e-6f);
}

// GATING: a target entity that exists but carries NO TransformComponent -> idle
// (can't position it), not a crash or a garbage wish.
TEST(UtilityHardening, LocoTargetWithoutTransformIsIdle) {
    entt::registry reg;
    const auto bare = reg.create(); // no TransformComponent
    const auto a = MakeLocoAgent(reg, 0.0f, 0.0f, bare);
    const auto stats = RunInstinctLocomotionOnTick(reg);
    EXPECT_EQ(stats.agents_idle, 1u);
    EXPECT_NEAR(Len2(reg.get<Comp::LocomotionIntentComponent>(a).wish_xz), 0.0f, 1e-6f);
}

// FLEE: with flee=true the wish points directly AWAY from the threat at full
// cruise while inside the safe (slow) radius.
TEST(UtilityHardening, LocoFleeMovesAwayAtCruise) {
    entt::registry reg;
    const auto threat = MakeLocoTarget(reg, 2.0f, 0.0f);
    Comp::LocomotionProfile p;
    p.move_speed = 3.0f;
    p.slow_radius = 5.0f;
    const auto a = MakeLocoAgent(reg, 0.0f, 0.0f, threat, p, /*flee=*/true);
    const auto stats = RunInstinctLocomotionOnTick(reg);
    EXPECT_EQ(stats.agents_steered, 1u);
    const auto& w = reg.get<Comp::LocomotionIntentComponent>(a).wish_xz;
    EXPECT_NEAR(w.x, -3.0f, 1e-4f);
    EXPECT_NEAR(Len2(w), 3.0f, 1e-4f);
}

// FLEE ZERO-DISTANCE DEGENERACY: agent sitting exactly on the threat must not
// produce a NaN wish (the system seeds a deterministic +X escape).
TEST(UtilityHardening, LocoFleeCoincidentNoNaN) {
    entt::registry reg;
    const auto threat = MakeLocoTarget(reg, 5.0f, 5.0f);
    Comp::LocomotionProfile p;
    p.move_speed = 3.0f;
    p.slow_radius = 5.0f;
    const auto a = MakeLocoAgent(reg, 5.0f, 5.0f, threat, p, /*flee=*/true); // coincident
    RunInstinctLocomotionOnTick(reg);
    const auto& w = reg.get<Comp::LocomotionIntentComponent>(a).wish_xz;
    EXPECT_FALSE(std::isnan(w.x));
    EXPECT_FALSE(std::isnan(w.y));
    EXPECT_NEAR(Len2(w), 3.0f, 1e-4f) << "coincident flee should still be a full-speed escape";
}

// ARRIVAL RAMP: inside slow_radius but outside arrival_radius the speed scales
// linearly with distance (Reynolds arrival). dist 2 of slow 4 -> half speed.
TEST(UtilityHardening, LocoArrivalRampHalvesSpeed) {
    entt::registry reg;
    const auto t = MakeLocoTarget(reg, 2.0f, 0.0f);
    Comp::LocomotionProfile p;
    p.move_speed = 4.0f;
    p.arrival_radius = 1.0f;
    p.slow_radius = 4.0f;
    const auto a = MakeLocoAgent(reg, 0.0f, 0.0f, t, p);
    RunInstinctLocomotionOnTick(reg);
    EXPECT_NEAR(Len2(reg.get<Comp::LocomotionIntentComponent>(a).wish_xz), 2.0f, 1e-4f);
}

// run==replay EXACT (bit-level) for the locomotion wish over a non-trivial scene.
TEST(UtilityHardening, LocoRunEqualsReplayExact) {
    auto run = [] {
        entt::registry reg;
        const auto t = MakeLocoTarget(reg, 13.7f, -8.2f);
        Comp::LocomotionProfile p;
        p.move_speed = 2.5f;
        const auto a = MakeLocoAgent(reg, -3.1f, 4.4f, t, p);
        RunInstinctLocomotionOnTick(reg);
        const auto w = reg.get<Comp::LocomotionIntentComponent>(a).wish_xz;
        return std::vector<float>{w.x, w.y};
    };
    EXPECT_EQ(run(), run());
}

// ORDER INDEPENDENCE: shifting entity ids (a throwaway entity first) must not
// change the agent's wish (the system sorts by id; result is per-entity stable).
TEST(UtilityHardening, LocoOrderIndependentWish) {
    auto wish = [](bool shift) {
        entt::registry reg;
        if (shift)
            (void)reg.create();
        const auto t = MakeLocoTarget(reg, 10.0f, 10.0f);
        const auto a = MakeLocoAgent(reg, 0.0f, 0.0f, t);
        RunInstinctLocomotionOnTick(reg);
        return reg.get<Comp::LocomotionIntentComponent>(a).wish_xz;
    };
    const auto w0 = wish(false);
    const auto w1 = wish(true);
    EXPECT_NEAR(w0.x, w1.x, 1e-5f);
    EXPECT_NEAR(w0.y, w1.y, 1e-5f);
}

// ===========================================================================
// 6. Awareness fusion: threshold edges, decay, memory clear, purity, clamp.
// ===========================================================================

// THRESHOLD EDGE: a meter exactly at alert_at while sensing is Alert (the
// boundary is inclusive: meter >= alert_at).
TEST(UtilityHardening, AwarenessAlertBoundaryInclusive) {
    AwarenessParams p;
    Awareness a;
    a.meter = p.alert_at; // exactly at the alert threshold
    const Awareness r =
        UpdateAwareness(a, /*signal=*/0.0f, 0.0f, 0.0f, 0.0f, p); // dt 0 -> meter unchanged
    EXPECT_FLOAT_EQ(r.meter, p.alert_at);
    EXPECT_EQ(r.state, AwarenessState::Searching); // at alert, not sensing -> Searching
}

// Losing a strong signal -> Searching (still aware, heads to last-known), and
// last-known position is the one captured while sensing.
TEST(UtilityHardening, AwarenessSearchingRemembersLastKnown) {
    AwarenessParams p;
    Awareness a;
    for (int i = 0; i < 60; ++i)
        a = UpdateAwareness(a, 1.0f, 9.0f, -4.0f, kDt, p);
    ASSERT_EQ(a.state, AwarenessState::Engaged);
    a = UpdateAwareness(a, 0.0f, 0.0f, 0.0f, kDt, p);
    EXPECT_EQ(a.state, AwarenessState::Searching);
    EXPECT_TRUE(a.has_last_known);
    EXPECT_FLOAT_EQ(a.last_known_x, 9.0f);
    EXPECT_FLOAT_EQ(a.last_known_z, -4.0f);
}

// Forgetting: as the meter decays below suspicious_at the memory is cleared
// (has_last_known false) so the creature truly gives up.
TEST(UtilityHardening, AwarenessForgetsBelowSuspicious) {
    AwarenessParams p;
    Awareness a;
    for (int i = 0; i < 60; ++i)
        a = UpdateAwareness(a, 1.0f, 2.0f, 2.0f, kDt, p);
    for (int i = 0; i < 400; ++i)
        a = UpdateAwareness(a, 0.0f, 0.0f, 0.0f, kDt, p);
    EXPECT_EQ(a.state, AwarenessState::Unaware);
    EXPECT_FALSE(a.has_last_known);
    EXPECT_FLOAT_EQ(a.meter, 0.0f);
}

// Meter clamps to [0,1] under absurd signal*dt and absurd decay*dt (no overshoot,
// no negative).
TEST(UtilityHardening, AwarenessMeterClampedUnderExtremes) {
    AwarenessParams p;
    Awareness a;
    a = UpdateAwareness(a, 1e6f, 1.0f, 1.0f, 1e3f, p);
    EXPECT_LE(a.meter, 1.0f);
    EXPECT_FALSE(std::isnan(a.meter));
    a = UpdateAwareness(a, 0.0f, 0.0f, 0.0f, 1e6f, p);
    EXPECT_GE(a.meter, 0.0f);
}

// Engaged requires BOTH a current signal AND meter >= engaged_at. A high meter
// with NO current signal is Searching/Alert, never Engaged (sensing gate).
TEST(UtilityHardening, AwarenessEngagedRequiresCurrentSignal) {
    AwarenessParams p;
    Awareness a;
    for (int i = 0; i < 60; ++i)
        a = UpdateAwareness(a, 1.0f, 1.0f, 1.0f, kDt, p);
    ASSERT_EQ(a.state, AwarenessState::Engaged);
    // One tick with no signal: meter barely drops but sensing is false -> not Engaged.
    a = UpdateAwareness(a, 0.0f, 0.0f, 0.0f, kDt, p);
    EXPECT_NE(a.state, AwarenessState::Engaged);
}

// Purity: UpdateAwareness does not mutate its input and is bit-deterministic.
TEST(UtilityHardening, AwarenessUpdateIsPure) {
    AwarenessParams p;
    Awareness base;
    base.meter = 0.42f;
    const Awareness r1 = UpdateAwareness(base, 0.3f, 1.0f, 2.0f, kDt, p);
    const Awareness r2 = UpdateAwareness(base, 0.3f, 1.0f, 2.0f, kDt, p);
    EXPECT_EQ(r1.meter, r2.meter);
    EXPECT_EQ(r1.state, r2.state);
    EXPECT_FLOAT_EQ(base.meter, 0.42f); // input untouched
}

} // namespace
