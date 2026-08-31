// Adversarial hardening for the PERCEPTION & SOCIAL / BRAIN cluster:
//   * ai/HerdAlarmSystem.h (+ AlarmComponents.h): alarm level propagation in,
//     full DECAY-to-zero once the threat clears, and the "pin keys off the FLAG, not
//     level>=1" rule (the recent self-pin regression).
//   * ai/CreatureBrainSystem.h + CreatureBrain.h: IAUS decide -> wish-VELOCITY, flee/hunt
//     sign + direction, catch radius edges, flocking blend, run==replay + gating.
//   * ai/UtilityAI.h: Consideration curve bounds, Dave-Mark compensation, SelectAction
//     lowest-id tie-break.
//
// These probe what the happy-path per-system tests miss: determinism (run==replay),
// strict gating no-ops, output bounds / NaN-freedom on degenerate input, order
// independence, producer->consumer field MEANING (wish is a VELOCITY, flee dir is
// self-minus-target), and any state that fails to decay or pins.
//
// Determinism rule: only the systems' own seeded/integer paths; NO wall-clock/std::random.
// run==replay asserts EXACT float equality (systems are -ffp-contract=off bit-exact).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureBrain.h"
#include "ai/CreatureBrainSystem.h"
#include "ai/HerdAlarmSystem.h"
#include "ai/UtilityAI.h"
#include "components/AlarmComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
namespace ai = ::luminumbra::ai;

using ai::Consideration;
using ai::CreatureAction;
using ai::CreatureSenses;
using ai::CurveType;
using ai::DecideCreatureAction;
using ai::HerdAlarmStats;
using ai::kAlarmDecay;
using ai::kAlarmRadius;
using ai::kAlarmSourceThreshold;
using ai::kCatchRadius;
using ai::RunCreatureBrainSystemOnTick;
using ai::RunHerdAlarmOnTick;
using ai::SelectAction;
using ai::UtilityAction;

constexpr float kDt = Luminumbra::SECONDS_PER_TICK; // one fixed 30Hz tick

// ----------------------------------------------------------------------------- helpers

entt::entity spawnAlarm(entt::registry& r,
                        float x,
                        float z,
                        bool predator = false,
                        float level = 0.0f,
                        std::uint8_t alarmed = 0,
                        bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = predator;
    cr.eaten = eaten;
    auto& al = r.emplace<Comp::AlarmComponent>(e);
    al.level = level;
    al.alarmed = alarmed;
    return e;
}

entt::entity spawnCreature(entt::registry& r,
                           float x,
                           float z,
                           bool predator,
                           float hunger = 0.0f,
                           float stamina = 1.0f,
                           bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = predator;
    cr.hunger = hunger;
    cr.stamina = stamina;
    cr.eaten = eaten;
    return e;
}

bool isFinite(float v) {
    return v == v && v <= 1.0e30f && v >= -1.0e30f;
}

// =============================================================================
// UtilityAI — curve bounds, Dave-Mark compensation, tie-break.
// =============================================================================

// Every curve, for ANY input/param, must return a value in [0,1] with no NaN/inf.
TEST(SocialHardening, UtilityCurvesClampedAndFinite) {
    const CurveType curves[] = {
        CurveType::Linear, CurveType::InvLinear, CurveType::Quadratic, CurveType::Logistic};
    const float params[] = {-5.0f, -1.0f, -0.001f, 0.0f, 0.001f, 0.5f, 1.0f, 5.0f, 100.0f};
    const float inputs[] = {-10.0f, -0.5f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f, 1.0e9f};
    for (CurveType cv : curves) {
        for (float in : inputs) {
            for (float m : params) {
                for (float b : params) {
                    for (float c : params) {
                        Consideration k;
                        k.input = in;
                        k.curve = cv;
                        k.m = m;
                        k.b = b;
                        k.c = c;
                        const float v = k.Evaluate();
                        EXPECT_TRUE(isFinite(v)) << "non-finite curve output";
                        EXPECT_GE(v, 0.0f);
                        EXPECT_LE(v, 1.0f);
                    }
                }
            }
        }
    }
}

// A single consideration must apply NO Dave-Mark compensation (modFactor = 1 - 1/1 = 0):
// the action score is exactly weight*Evaluate, not inflated.
TEST(SocialHardening, SingleConsiderationNoCompensation) {
    UtilityAction a;
    a.id = 0;
    a.weight = 1.0f;
    Consideration k;
    k.input = 0.5f;
    k.curve = CurveType::Linear;
    k.m = 1.0f;
    k.b = 0.0f;
    k.c = 0.0f;
    a.considerations.push_back(k);
    // Evaluate = clamp01(1*(0.5-0)+0) = 0.5; with n==1 modFactor=0 so score == 0.5 exactly.
    EXPECT_FLOAT_EQ(a.Score(), 0.5f);
}

// Compensation must only ever RAISE a multi-consideration product toward 1 (never push it
// above 1 or below the un-compensated product), and stay in [0,1].
TEST(SocialHardening, CompensationRaisesButStaysBounded) {
    UtilityAction a;
    a.id = 0;
    a.weight = 1.0f;
    auto mk = [](float in) {
        Consideration k;
        k.input = in;
        k.curve = CurveType::Linear;
        k.m = 1.0f;
        k.b = 0.0f;
        k.c = 0.0f;
        return k;
    };
    a.considerations = {mk(0.5f), mk(0.5f), mk(0.5f)};
    const float s = a.Score();
    EXPECT_GE(s, 0.0f);
    EXPECT_LE(s, 1.0f);
    // raw product would be 0.125; compensation must lift it strictly above that.
    EXPECT_GT(s, 0.125f);
}

// Empty action set -> -1; weight-only action scores to clamped weight.
TEST(SocialHardening, SelectActionEmptyAndWeightOnly) {
    EXPECT_EQ(SelectAction({}), -1);
    UtilityAction w;
    w.id = 7;
    w.weight = 0.42f;
    EXPECT_FLOAT_EQ(w.Score(), 0.42f);
}

// Tie-break: equal-scoring actions MUST resolve to the LOWEST id, independent of vector
// order (canonical -> run==replay). Probe both orderings.
TEST(SocialHardening, SelectActionLowestIdTieBreak) {
    UtilityAction a;
    a.id = 5;
    a.weight = 0.5f; // identical scores (no considerations)
    UtilityAction b;
    b.id = 2;
    b.weight = 0.5f;
    UtilityAction c;
    c.id = 9;
    c.weight = 0.5f;
    EXPECT_EQ(SelectAction({a, b, c}), 2);
    EXPECT_EQ(SelectAction({c, a, b}), 2);
    EXPECT_EQ(SelectAction({b, c, a}), 2);
}

// =============================================================================
// CreatureBrain — IAUS decision boundaries.
// =============================================================================

// A prey with a near predator (high threat_proximity) above the Flee logistic center
// (~0.45) must FLEE, dominating graze/rest/wander.
TEST(SocialHardening, PreyFleesNearPredator) {
    CreatureSenses s;
    s.is_predator = false;
    s.threat_proximity = 0.9f;
    s.hunger = 0.8f; // hungry, but flee must still win
    s.food_proximity = 0.6f;
    s.stamina = 1.0f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Flee);
}

// A safe, hungry prey with food nearby GRAZES (not flee/rest/wander).
TEST(SocialHardening, PreyGrazesWhenSafeAndHungry) {
    CreatureSenses s;
    s.is_predator = false;
    s.threat_proximity = 0.0f;
    s.hunger = 0.9f;
    s.food_proximity = 0.9f;
    s.stamina = 1.0f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Graze);
}

// An exhausted, safe creature RESTS.
TEST(SocialHardening, ExhaustedSafeCreatureRests) {
    CreatureSenses s;
    s.is_predator = false;
    s.threat_proximity = 0.0f;
    s.hunger = 0.0f;
    s.food_proximity = 0.0f;
    s.stamina = 0.0f; // exhausted
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Rest);
}

// A hungry predator with prey nearby and stamina HUNTS.
TEST(SocialHardening, HungryPredatorHunts) {
    CreatureSenses s;
    s.is_predator = true;
    s.hunger = 0.9f;
    s.food_proximity = 0.9f;
    s.stamina = 1.0f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Hunt);
}

// Degenerate senses (all zero) must yield a valid action id (Wander baseline), never -1.
TEST(SocialHardening, ZeroSensesGivesValidAction) {
    CreatureSenses prey;
    prey.is_predator = false;
    CreatureSenses pred;
    pred.is_predator = true;
    const auto ap = DecideCreatureAction(prey);
    const auto aq = DecideCreatureAction(pred);
    EXPECT_GE(static_cast<int>(ap), 0);
    EXPECT_LE(static_cast<int>(ap), 4);
    EXPECT_GE(static_cast<int>(aq), 0);
    EXPECT_LE(static_cast<int>(aq), 4);
}

// =============================================================================
// CreatureBrainSystem — gating, determinism, wish-VELOCITY contract, catch edges.
// =============================================================================

// GATING: a registry with no CreatureComponent is a strict no-op (nothing updated).
TEST(SocialHardening, BrainEmptyRosterNoOp) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e); // a transform but NO creature
    const auto stats = RunCreatureBrainSystemOnTick(r, kDt);
    EXPECT_EQ(stats.updated, 0);
}

// PRODUCER->CONSUMER: wish_x/wish_z is a VELOCITY in m/s. A fleeing prey's wish magnitude
// must be the sprint speed (move_speed * 1.5), NOT a unit direction and NOT a world point.
TEST(SocialHardening, FleeWishIsSprintVelocityMagnitude) {
    entt::registry r;
    auto prey = spawnCreature(r, 0.0f, 0.0f, /*predator=*/false);
    spawnCreature(r, 4.0f, 0.0f, /*predator=*/true, /*hunger=*/0.9f); // near threat
    const float move_speed = r.get<Comp::CreatureComponent>(prey).move_speed;
    RunCreatureBrainSystemOnTick(r, kDt);
    const auto& cr = r.get<Comp::CreatureComponent>(prey);
    ASSERT_EQ(cr.last_action, static_cast<int>(CreatureAction::Flee));
    const float speed = std::sqrt(cr.wish_x * cr.wish_x + cr.wish_z * cr.wish_z);
    // Lone prey (no herd) -> flee dir is pure self-minus-target -> wish magnitude == sprint.
    EXPECT_NEAR(speed, move_speed * 1.5f, 1.0e-3f)
        << "wish must be a sprint VELOCITY, not unit dir / world point";
    // Direction: fleeing AWAY from predator at +x means wish_x < 0.
    EXPECT_LT(cr.wish_x, 0.0f) << "flee must steer self-minus-target (away)";
}

// Hunt steers TOWARD prey (target-minus-self): a predator left of prey wishes +x.
TEST(SocialHardening, HuntWishStreersTowardPrey) {
    entt::registry r;
    auto pred = spawnCreature(r, 0.0f, 0.0f, /*predator=*/true, /*hunger=*/0.95f);
    spawnCreature(r, 10.0f, 0.0f, /*predator=*/false); // prey to the +x, beyond catch reach
    RunCreatureBrainSystemOnTick(r, kDt);
    const auto& cr = r.get<Comp::CreatureComponent>(pred);
    ASSERT_EQ(cr.last_action, static_cast<int>(CreatureAction::Hunt));
    EXPECT_GT(cr.wish_x, 0.0f) << "hunt must steer target-minus-self (toward prey)";
}

// CATCH EDGE: prey strictly INSIDE kCatchRadius is eaten; prey strictly OUTSIDE is not.
TEST(SocialHardening, CatchRadiusEdges) {
    {
        entt::registry r;
        spawnCreature(r, 0.0f, 0.0f, /*predator=*/true, /*hunger=*/0.95f);
        auto prey = spawnCreature(r, kCatchRadius - 0.05f, 0.0f, /*predator=*/false);
        RunCreatureBrainSystemOnTick(r, kDt);
        EXPECT_TRUE(r.get<Comp::CreatureComponent>(prey).eaten) << "inside catch radius -> eaten";
    }
    {
        entt::registry r;
        spawnCreature(r, 0.0f, 0.0f, /*predator=*/true, /*hunger=*/0.95f);
        auto prey = spawnCreature(r, kCatchRadius + 0.05f, 0.0f, /*predator=*/false);
        RunCreatureBrainSystemOnTick(r, kDt);
        EXPECT_FALSE(r.get<Comp::CreatureComponent>(prey).eaten)
            << "outside catch radius -> not eaten";
    }
}

// An already-eaten prey is inert: zero wish, still counted updated, never resurrected.
TEST(SocialHardening, EatenPreyIsInert) {
    entt::registry r;
    auto prey = spawnCreature(r, 5.0f, 0.0f, /*predator=*/false, 0.0f, 1.0f, /*eaten=*/true);
    const auto stats = RunCreatureBrainSystemOnTick(r, kDt);
    const auto& cr = r.get<Comp::CreatureComponent>(prey);
    EXPECT_TRUE(cr.eaten);
    EXPECT_FLOAT_EQ(cr.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(cr.wish_z, 0.0f);
    EXPECT_GE(stats.updated, 1);
}

// BUG (double-satiation): two predators BOTH within catch reach of the SAME single prey
// each sate their hunger from one carcass in one tick — because the catch test reads the
// PRE-tick snapshot (prey alive for both) while the live `eaten` flag set by the first
// predator is not re-checked. One prey should feed at most ONE predator per tick.
// Expected-correct: exactly ONE predator's hunger drops; this test documents that.
TEST(SocialHardening, OnePreyFeedsAtMostOnePredatorPerTick) {
    entt::registry r;
    auto p1 = spawnCreature(r, -1.0f, 0.0f, /*predator=*/true, /*hunger=*/0.95f);
    auto p2 = spawnCreature(r, 1.0f, 0.0f, /*predator=*/true, /*hunger=*/0.95f);
    spawnCreature(r, 0.0f, 0.0f, /*predator=*/false); // single prey between them, both in reach
    RunCreatureBrainSystemOnTick(r, kDt);
    const float h1 = r.get<Comp::CreatureComponent>(p1).hunger;
    const float h2 = r.get<Comp::CreatureComponent>(p2).hunger;
    const bool fed1 = h1 < 0.9f; // satiation drops hunger by ~0.8
    const bool fed2 = h2 < 0.9f;
    EXPECT_FALSE(fed1 && fed2) << "one prey must not sate BOTH predators in a single tick";
}

// DETERMINISM: run==replay over multiple ticks on a mixed roster — byte-identical state.
TEST(SocialHardening, BrainRunEqualsReplay) {
    auto build = [](entt::registry& r) {
        spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, 0.3f, 1.0f);
        spawnCreature(r, 6.0f, 2.0f, /*predator=*/true, 0.7f, 0.9f);
        spawnCreature(r, -4.0f, 5.0f, /*predator=*/false, 0.5f, 0.8f);
        spawnCreature(r, 3.0f, -3.0f, /*predator=*/false, 0.2f, 1.0f);
    };
    entt::registry a, b;
    build(a);
    build(b);
    for (int t = 0; t < 20; ++t) {
        RunCreatureBrainSystemOnTick(a, kDt);
        RunCreatureBrainSystemOnTick(b, kDt);
    }
    std::vector<float> sa, sb;
    auto dump = [](entt::registry& r, std::vector<float>& out) {
        auto v = r.view<Comp::CreatureComponent, Comp::TransformComponent>();
        std::vector<entt::entity> es(v.begin(), v.end());
        std::sort(es.begin(), es.end(), [](auto x, auto y) {
            return entt::to_integral(x) < entt::to_integral(y);
        });
        for (auto e : es) {
            const auto& c = v.get<Comp::CreatureComponent>(e);
            const auto& tf = v.get<Comp::TransformComponent>(e);
            out.insert(out.end(),
                       {c.hunger,
                        c.stamina,
                        c.wish_x,
                        c.wish_z,
                        tf.position.x,
                        tf.position.z,
                        static_cast<float>(c.last_action),
                        static_cast<float>(c.eaten ? 1 : 0)});
        }
    };
    dump(a, sa);
    dump(b, sb);
    ASSERT_EQ(sa.size(), sb.size());
    for (std::size_t i = 0; i < sa.size(); ++i)
        EXPECT_FLOAT_EQ(sa[i], sb[i]) << "run!=replay at field " << i;
}

// ORDER-INDEPENDENCE: shuffling creation order yields the SAME per-creature result keyed
// by geometry (positions are unique). Pre-tick snapshot must make order irrelevant.
TEST(SocialHardening, BrainOrderIndependentByGeometry) {
    struct Spec {
        float x, z;
        bool pred;
        float hunger, stamina;
    };
    const Spec specs[] = {
        {0.0f, 0.0f, false, 0.3f, 1.0f},
        {6.0f, 0.0f, true, 0.8f, 0.9f},
        {-3.0f, 4.0f, false, 0.5f, 0.7f},
        {2.0f, -5.0f, false, 0.2f, 1.0f},
    };
    auto runWith = [&](const std::vector<int>& order) {
        entt::registry r;
        for (int i : order) {
            const Spec& s = specs[i];
            spawnCreature(r, s.x, s.z, s.pred, s.hunger, s.stamina);
        }
        for (int t = 0; t < 5; ++t)
            RunCreatureBrainSystemOnTick(r, kDt);
        // key results by spawn position (unique) so iteration order is removed.
        std::vector<std::pair<std::pair<float, float>, std::vector<float>>> out;
        auto v = r.view<Comp::CreatureComponent, Comp::TransformComponent>();
        for (auto e : v) {
            const auto& c = v.get<Comp::CreatureComponent>(e);
            const auto& tf = v.get<Comp::TransformComponent>(e);
            out.push_back(
                {{tf.position.x, tf.position.z},
                 {c.hunger, c.stamina, c.wish_x, c.wish_z, static_cast<float>(c.last_action)}});
        }
        std::sort(out.begin(), out.end(), [](const auto& A, const auto& B) {
            return A.second < B.second; // stable-ish ordering for comparison
        });
        return out;
    };
    auto forward = runWith({0, 1, 2, 3});
    auto reverse = runWith({3, 2, 1, 0});
    ASSERT_EQ(forward.size(), reverse.size());
    // Build position-keyed maps for a geometry-keyed compare.
    auto findByPos = [](const auto& v, float x, float z) -> std::vector<float> {
        for (const auto& kv : v)
            if (kv.first.first == x && kv.first.second == z)
                return kv.second;
        return {};
    };
    for (const Spec& s : specs) {
        auto f = findByPos(forward, s.x, s.z);
        auto g = findByPos(reverse, s.x, s.z);
        ASSERT_EQ(f.size(), g.size());
        for (std::size_t i = 0; i < f.size(); ++i)
            EXPECT_FLOAT_EQ(f[i], g[i])
                << "order-dependent result at pos (" << s.x << "," << s.z << ")";
    }
}

// =============================================================================
// HerdAlarmSystem — gating, propagation, full decay, pin-keys-off-flag.
// =============================================================================

// GATING: no AlarmComponent anywhere -> strict no-op even with creatures present.
TEST(SocialHardening, AlarmGatingNoComponentNoOp) {
    entt::registry r;
    spawnCreature(r, 0.0f, 0.0f, false); // creature WITHOUT an AlarmComponent
    const auto stats = RunHerdAlarmOnTick(r, kDt);
    EXPECT_EQ(stats.participants, 0);
    EXPECT_EQ(stats.sources, 0);
    EXPECT_EQ(stats.raised, 0);
}

// Empty registry -> pure no-op.
TEST(SocialHardening, AlarmEmptyRosterNoOp) {
    entt::registry r;
    const auto stats = RunHerdAlarmOnTick(r, kDt);
    EXPECT_EQ(stats.participants, 0);
}

// Level must stay clamped to [0,1] and finite for any seeded input (including >1 / <0 seeds).
TEST(SocialHardening, AlarmLevelStaysClamped) {
    entt::registry r;
    auto a = spawnAlarm(r, 0.0f, 0.0f, false, /*level=*/5.0f, /*alarmed=*/0);
    auto b = spawnAlarm(r, 1.0f, 0.0f, false, /*level=*/-3.0f, /*alarmed=*/1);
    for (int t = 0; t < 10; ++t)
        RunHerdAlarmOnTick(r, kDt);
    for (auto e : {a, b}) {
        const float L = r.get<Comp::AlarmComponent>(e).level;
        EXPECT_TRUE(isFinite(L));
        EXPECT_GE(L, 0.0f);
        EXPECT_LE(L, 1.0f);
    }
}

// PIN KEYS OFF THE FLAG: an alarmed (alarmed==1) creature pins its level to 1.0 and HOLDS
// it while the flag stays set. A relay that merely REACHED level 1.0 but has alarmed==0
// must NOT be pinned — it must decay once its source is gone (the self-pin regression).
TEST(SocialHardening, AlarmPinKeysOffFlagNotLevel) {
    // (a) flagged source stays pinned at 1.0 across ticks.
    {
        entt::registry r;
        auto src = spawnAlarm(r, 0.0f, 0.0f, false, /*level=*/0.0f, /*alarmed=*/1);
        for (int t = 0; t < 5; ++t)
            RunHerdAlarmOnTick(r, kDt);
        EXPECT_FLOAT_EQ(r.get<Comp::AlarmComponent>(src).level, 1.0f) << "flagged source must pin";
    }
    // (b) a creature at level 1.0 with alarmed==0 and NO source nearby must DECAY, not pin.
    {
        entt::registry r;
        auto relay = spawnAlarm(r, 0.0f, 0.0f, false, /*level=*/1.0f, /*alarmed=*/0);
        const float before = r.get<Comp::AlarmComponent>(relay).level;
        RunHerdAlarmOnTick(r, kDt);
        const float after = r.get<Comp::AlarmComponent>(relay).level;
        EXPECT_LT(after, before) << "level==1.0 with alarmed==0 must NOT pin; it must decay";
        EXPECT_FLOAT_EQ(after, 1.0f * kAlarmDecay); // exactly one decay step at 30Hz
    }
}

// FULL DECAY-TO-ZERO: once the threat clears (flag cleared, no source in range), every
// creature's level must drain to (effectively) zero — nothing pins indefinitely.
TEST(SocialHardening, AlarmDecaysToZeroAfterThreatClears) {
    entt::registry r;
    // A line of 4 same-role prey within relay range; seed the first as a flagged source so
    // the, then clear the flag and let it decay.
    auto e0 = spawnAlarm(r, 0.0f, 0.0f, false, 0.0f, /*alarmed=*/1);
    auto e1 = spawnAlarm(r, 4.0f, 0.0f, false);
    auto e2 = spawnAlarm(r, 8.0f, 0.0f, false);
    auto e3 = spawnAlarm(r, 12.0f, 0.0f, false);
    // Propagate the  several ticks.
    for (int t = 0; t < 8; ++t)
        RunHerdAlarmOnTick(r, kDt);
    // The relay nearest the source must have lit up ( it).
    EXPECT_GT(r.get<Comp::AlarmComponent>(e1).level, 0.0f) << " reach the herd";
    // Threat clears: drop the flag. Now NOTHING should remain a source past the threshold.
    r.get<Comp::AlarmComponent>(e0).alarmed = 0;
    for (int t = 0; t < 200; ++t)
        RunHerdAlarmOnTick(r, kDt);
    for (auto e : {e0, e1, e2, e3}) {
        EXPECT_LT(r.get<Comp::AlarmComponent>(e).level, 1.0e-3f)
            << "alarm must fully decay to ~0 after the threat clears";
    }
}

// PROPAGATION IN: an alarmed source raises the level of a same-role neighbour in
// range; the  a chained relay only over successive ticks (not instantly across
// the whole chain). Probe that the near relay lights before the far one.
TEST(SocialHardening, AlarmPropagatesInWaves) {
    entt::registry r;
    auto src = spawnAlarm(r, 0.0f, 0.0f, false, 0.0f, /*alarmed=*/1);
    auto near_ = spawnAlarm(r, 6.0f, 0.0f, false); // within radius (12) of src
    auto far_ = spawnAlarm(r, 16.0f, 0.0f, false); // dist 16 from src (out), 10 from near_ (in)
    (void)src;
    // After ONE tick: near lights (direct from source); far still 0 (source out of range,
    // and near isn't a strong-enough relay yet).
    RunHerdAlarmOnTick(r, kDt);
    EXPECT_GT(r.get<Comp::AlarmComponent>(near_).level, 0.0f) << "near relay lights tick 1";
    EXPECT_FLOAT_EQ(r.get<Comp::AlarmComponent>(far_).level, 0.0f) << "far relay still dark tick 1";
    // After several more ticks the far relay should receive the relayed
    for (int t = 0; t < 6; ++t)
        RunHerdAlarmOnTick(r, kDt);
    EXPECT_GT(r.get<Comp::AlarmComponent>(far_).level, 0.0f) << " reach far relay over time";
}

// ROLE-GATED: an alarmed PREDATOR does NOT raise nearby PREY (different is_predator role).
TEST(SocialHardening, AlarmDoesNotCrossRoles) {
    entt::registry r;
    spawnAlarm(r, 0.0f, 0.0f, /*predator=*/true, 0.0f, /*alarmed=*/1); // alarmed predator
    auto prey = spawnAlarm(r, 2.0f, 0.0f, /*predator=*/false);         // prey right next to it
    for (int t = 0; t < 5; ++t)
        RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(r.get<Comp::AlarmComponent>(prey).level, 0.0f)
        << "alarm must not cross is_predator roles";
}

// DEAD don't warn or fear: a carcass source emits nothing and its own level drains to 0.
TEST(SocialHardening, AlarmCarcassEmitsNothingAndDrains) {
    entt::registry r;
    auto dead = spawnAlarm(r, 0.0f, 0.0f, false, /*level=*/1.0f, /*alarmed=*/1, /*eaten=*/true);
    auto live = spawnAlarm(r, 2.0f, 0.0f, false);
    RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(r.get<Comp::AlarmComponent>(dead).level, 0.0f) << "carcass drains to 0";
    EXPECT_FLOAT_EQ(r.get<Comp::AlarmComponent>(live).level, 0.0f) << "carcass must emit nothing";
}

// EDGE: a creature exactly AT the alarm radius gets no contribution (>= radius excluded).
TEST(SocialHardening, AlarmRadiusEdgeExcluded) {
    entt::registry r;
    spawnAlarm(r, 0.0f, 0.0f, false, 0.0f, /*alarmed=*/1);
    auto edge = spawnAlarm(r, kAlarmRadius, 0.0f, false); // exactly at the radius
    RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(r.get<Comp::AlarmComponent>(edge).level, 0.0f)
        << "a neighbour exactly at kAlarmRadius is out of earshot";
}

// COINCIDENT entities (zero distance) must not produce NaN/inf and stay clamped.
TEST(SocialHardening, AlarmCoincidentNoNaN) {
    entt::registry r;
    spawnAlarm(r, 0.0f, 0.0f, false, 0.0f, /*alarmed=*/1);
    auto co = spawnAlarm(r, 0.0f, 0.0f, false); // coincident with the source
    RunHerdAlarmOnTick(r, kDt);
    const float L = r.get<Comp::AlarmComponent>(co).level;
    EXPECT_TRUE(isFinite(L));
    EXPECT_GE(L, 0.0f);
    EXPECT_LE(L, 1.0f);
    EXPECT_GT(L, 0.0f) << "coincident same-role neighbour of a source should light up";
}

// DETERMINISM: run==replay for the alarm field over many ticks.
TEST(SocialHardening, AlarmRunEqualsReplay) {
    auto build = [](entt::registry& r) {
        spawnAlarm(r, 0.0f, 0.0f, false, 0.0f, 1);
        spawnAlarm(r, 5.0f, 1.0f, false, 0.3f, 0);
        spawnAlarm(r, -3.0f, 4.0f, false, 0.0f, 0);
        spawnAlarm(r, 9.0f, -2.0f, true, 0.0f, 1); // a predator source (own role)
        spawnAlarm(r, 7.0f, -2.0f, true, 0.0f, 0);
    };
    entt::registry a, b;
    build(a);
    build(b);
    for (int t = 0; t < 30; ++t) {
        RunHerdAlarmOnTick(a, kDt);
        RunHerdAlarmOnTick(b, kDt);
    }
    auto dump = [](entt::registry& r) {
        auto v = r.view<Comp::AlarmComponent>();
        std::vector<entt::entity> es(v.begin(), v.end());
        std::sort(es.begin(), es.end(), [](auto x, auto y) {
            return entt::to_integral(x) < entt::to_integral(y);
        });
        std::vector<float> out;
        for (auto e : es)
            out.push_back(r.get<Comp::AlarmComponent>(e).level);
        return out;
    };
    auto da = dump(a), db = dump(b);
    ASSERT_EQ(da.size(), db.size());
    for (std::size_t i = 0; i < da.size(); ++i)
        EXPECT_FLOAT_EQ(da[i], db[i]) << "alarm run!=replay at " << i;
}

// ORDER-INDEPENDENCE: shuffling creation order yields identical per-position alarm levels
// (two-phase snapshot must make iteration order irrelevant).
TEST(SocialHardening, AlarmOrderIndependentByGeometry) {
    struct Spec {
        float x, z;
        bool pred;
        float level;
        std::uint8_t alarmed;
    };
    const Spec specs[] = {
        {0.0f, 0.0f, false, 0.0f, 1},
        {5.0f, 0.0f, false, 0.0f, 0},
        {10.0f, 0.0f, false, 0.0f, 0},
        {-4.0f, 3.0f, false, 0.0f, 0},
    };
    auto runWith = [&](const std::vector<int>& order) {
        entt::registry r;
        for (int i : order) {
            const Spec& s = specs[i];
            spawnAlarm(r, s.x, s.z, s.pred, s.level, s.alarmed);
        }
        for (int t = 0; t < 6; ++t)
            RunHerdAlarmOnTick(r, kDt);
        std::vector<std::pair<std::pair<float, float>, float>> out;
        auto v = r.view<Comp::AlarmComponent, Comp::TransformComponent>();
        for (auto e : v) {
            const auto& tf = v.get<Comp::TransformComponent>(e);
            out.push_back({{tf.position.x, tf.position.z}, v.get<Comp::AlarmComponent>(e).level});
        }
        return out;
    };
    auto fwd = runWith({0, 1, 2, 3});
    auto rev = runWith({3, 2, 1, 0});
    auto findByPos = [](const auto& v, float x, float z) -> float {
        for (const auto& kv : v)
            if (kv.first.first == x && kv.first.second == z)
                return kv.second;
        return -1.0f;
    };
    for (const Spec& s : specs) {
        EXPECT_FLOAT_EQ(findByPos(fwd, s.x, s.z), findByPos(rev, s.x, s.z))
            << "order-dependent alarm at (" << s.x << "," << s.z << ")";
    }
}

// INTEGRATION CONTRACT: the brain SETS AlarmComponent.alarmed when a prey directly senses a
// predator (nearNorm > 0.4), and the alarm system then propagates from that flag. Verify the
// brain raises the flag when a predator is close, and clears it when the predator is far.
TEST(SocialHardening, BrainRaisesAndClearsAlarmFlag) {
    {
        entt::registry r;
        auto prey = spawnCreature(r, 0.0f, 0.0f, /*predator=*/false);
        r.emplace<Comp::AlarmComponent>(prey);
        spawnCreature(r, 3.0f, 0.0f, /*predator=*/true, 0.9f); // close threat -> nearNorm high
        RunCreatureBrainSystemOnTick(r, kDt);
        EXPECT_EQ(r.get<Comp::AlarmComponent>(prey).alarmed, 1u)
            << "close predator must raise flag";
    }
    {
        entt::registry r;
        auto prey = spawnCreature(r, 0.0f, 0.0f, /*predator=*/false);
        auto& al = r.emplace<Comp::AlarmComponent>(prey);
        al.alarmed = 1;                                          // was alarmed
        spawnCreature(r, 100.0f, 0.0f, /*predator=*/true, 0.9f); // far -> nearNorm ~0
        RunCreatureBrainSystemOnTick(r, kDt);
        EXPECT_EQ(r.get<Comp::AlarmComponent>(prey).alarmed, 0u)
            << "a distant predator must clear the flag (so the alarm can decay)";
    }
}

} // namespace
