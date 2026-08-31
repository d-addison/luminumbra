// sim.herd_alarm: ALARM signalling / collective vigilance. An alarmed prey raises
// the alarm of nearby same-role neighbours so the herd flees together; the alarm relays in
//  over ticks and decays when no source is near. Deterministic (id-ordered, two-phase
// snapshot, DeterministicMath only, NO rng), gated by the AlarmComponent opt-in.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/HerdAlarmSystem.h"
#include "components/AlarmComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::HerdAlarmStats;
using luminumbra::ai::kAlarmRadius;
using luminumbra::ai::RunHerdAlarmOnTick;

// One fixed sim tick at 30Hz (matches the engine's fixed dt; constants are tuned for it).
constexpr float kDt = Luminumbra::SECONDS_PER_TICK;

// Spawn a creature carrying an AlarmComponent (the opt-in). `predator` sets the role used
// for same-role matching; `level`/`alarmed` seed the alarm state.
entt::entity spawnCreature(entt::registry& r,
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

float levelOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::AlarmComponent>(e).level;
}

// ---- gating / no-op ----

// Empty roster: the system reports nothing and creates/changes nothing.
TEST(HerdAlarm, EmptyRosterNoOp) {
    entt::registry r;
    HerdAlarmStats s = RunHerdAlarmOnTick(r, kDt);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.sources, 0);
    EXPECT_EQ(s.raised, 0);
}

// Creatures WITHOUT an AlarmComponent are invisible to the system (opt-in gate): a world of
// non-participants is a pure no-op even with a transform + creature component present.
TEST(HerdAlarm, NonParticipantsIgnored) {
    entt::registry r;
    // Bare creature, no AlarmComponent.
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
    r.emplace<Comp::CreatureComponent>(e);
    HerdAlarmStats s = RunHerdAlarmOnTick(r, kDt);
    EXPECT_EQ(s.participants, 0);
}

// ---- core: an alarmed creature raises a NEAR same-role neighbour ----

TEST(HerdAlarm, AlarmedRaisesNearSameRoleNeighbour) {
    entt::registry r;
    spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, /*level=*/0.0f, /*alarmed=*/1); // source
    auto near = spawnCreature(r, 2.0f, 0.0f, /*predator=*/false);                    // calm prey

    EXPECT_FLOAT_EQ(levelOf(r, near), 0.0f);
    HerdAlarmStats s = RunHerdAlarmOnTick(r, kDt);

    EXPECT_GE(s.sources, 1);
    EXPECT_GT(levelOf(r, near), 0.0f); // the nearby neighbour was alarmed.
}

// ---- a FAR creature is unaffected ----

TEST(HerdAlarm, FarCreatureUnaffected) {
    entt::registry r;
    spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, 0.0f, /*alarmed=*/1);      // source
    auto far = spawnCreature(r, kAlarmRadius + 5.0f, 0.0f, /*predator=*/false); // out of range

    RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(levelOf(r, far), 0.0f); // beyond kAlarmRadius -> untouched.
}

// ---- role matching: a DIFFERENT-role neighbour is not warned ----

TEST(HerdAlarm, DifferentRoleNotWarned) {
    entt::registry r;
    spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, 0.0f, /*alarmed=*/1); // prey source
    auto pred = spawnCreature(r, 2.0f, 0.0f, /*predator=*/true);           // predator nearby

    RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(levelOf(r, pred), 0.0f); // alarm is herd-internal; predator unaffected.
}

// ---- closeness: a closer neighbour ends up MORE alarmed than a farther one ----

TEST(HerdAlarm, CloserNeighbourMoreAlarmed) {
    entt::registry r;
    spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, 0.0f, /*alarmed=*/1); // source
    auto closeE = spawnCreature(r, 2.0f, 0.0f, /*predator=*/false);
    auto farE = spawnCreature(r, kAlarmRadius - 1.0f, 0.0f, /*predator=*/false);

    RunHerdAlarmOnTick(r, kDt);
    EXPECT_GT(levelOf(r, closeE), levelOf(r, farE));
    EXPECT_GT(levelOf(r, farE), 0.0f); // both inside the radius are raised.
}

// ---- decay: with NO source nearby, an elevated creature's level fades toward 0 ----

TEST(HerdAlarm, DecaysWhenNoSource) {
    entt::registry r;
    // A single elevated-but-not-alarmed creature, alone, below the source threshold so it
    // does NOT relay to itself: its level must decay each tick.
    auto e = spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, /*level=*/0.2f, /*alarmed=*/0);

    const float l0 = levelOf(r, e);
    RunHerdAlarmOnTick(r, kDt);
    const float l1 = levelOf(r, e);
    RunHerdAlarmOnTick(r, kDt);
    const float l2 = levelOf(r, e);

    EXPECT_LT(l1, l0);   // decayed.
    EXPECT_LT(l2, l1);   // keeps decaying.
    EXPECT_GE(l2, 0.0f); // never negative.
}

// A source that stays alarmed keeps relaying; once it stops being alarmed the neighbour
// fades. (Source pinned at full while alarmed==1.)
TEST(HerdAlarm, AlarmedSourceHoldsFullThenFades) {
    entt::registry r;
    auto src = spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, 0.0f, /*alarmed=*/1);
    auto near = spawnCreature(r, 2.0f, 0.0f, /*predator=*/false);

    RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(levelOf(r, src), 1.0f); // alarmed originator pinned to max.
    const float litNeighbour = levelOf(r, near);
    EXPECT_GT(litNeighbour, 0.0f);

    // Threat passes: clear the source's alarmed flag. The neighbour should now decay since
    // (depending on its level) it may drop below the relay threshold.
    r.get<Comp::AlarmComponent>(src).alarmed = 0;
    // Run enough ticks that both drain toward zero.
    for (int i = 0; i < 40; ++i)
        RunHerdAlarmOnTick(r, kDt);
    EXPECT_LT(levelOf(r, src), 0.05f);
    EXPECT_LT(levelOf(r, near), 0.05f);
}

// ---- propagation in: a chain lights up over successive ticks ----

TEST(HerdAlarm, PropagatesInWavesAlongChain) {
    entt::registry r;
    // A line of prey. The far end (d) sits MORE than kAlarmRadius from the originator (a),
    // so d cannot be reached directly: the only way d lights up is if the alarm RELAYS down
    // the chain through intermediate creatures whose own raised level crosses the relay
    // threshold ( of collective vigilance). Step is small enough that each relayed hop
    // stays above kAlarmSourceThreshold so the  moving.
    const float step = kAlarmRadius * 0.4f;
    auto a = spawnCreature(r, 0.0f * step, 0.0f, false, 0.0f, /*alarmed=*/1); // originator
    auto b = spawnCreature(r, 1.0f * step, 0.0f, false);
    auto c = spawnCreature(r, 2.0f * step, 0.0f, false);
    auto d = spawnCreature(r, 3.0f * step, 0.0f, false);
    (void)a;
    (void)b;
    (void)c;

    // a and d are > kAlarmRadius apart, so the far end cannot be reached directly.
    ASSERT_GT(step * 3.0f, kAlarmRadius);

    // Tick 1: b (and possibly c) get raised by the originator; d stays dark (out of range
    // of a, and no intermediate has yet built a level to relay).
    RunHerdAlarmOnTick(r, kDt);
    EXPECT_GT(levelOf(r, b), 0.0f);
    EXPECT_FLOAT_EQ(levelOf(r, d), 0.0f);

    // Run more ticks: the  relay through c to d (collective vigilance spreads).
    bool dLit = false;
    for (int i = 0; i < 20; ++i) {
        RunHerdAlarmOnTick(r, kDt);
        if (levelOf(r, d) > 0.0f) {
            dLit = true;
            break;
        }
    }
    EXPECT_TRUE(dLit); // the alarm reached the far end of the chain in
    EXPECT_GT(levelOf(r, c), 0.0f);
}

// ---- carcasses neither emit nor receive ----

TEST(HerdAlarm, DeadDoNotWarnOrFear) {
    entt::registry r;
    // A dead (eaten) creature flagged alarmed must NOT propagate.
    spawnCreature(r, 0.0f, 0.0f, false, /*level=*/1.0f, /*alarmed=*/1, /*eaten=*/true);
    auto living = spawnCreature(r, 2.0f, 0.0f, false);
    RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(levelOf(r, living), 0.0f); // a carcass raises no alarm.

    // A dead creature does not hold a level either (drained to 0).
    auto corpse = spawnCreature(r,
                                10.0f,
                                0.0f,
                                false,
                                /*level=*/0.8f,
                                /*alarmed=*/0,
                                /*eaten=*/true);
    RunHerdAlarmOnTick(r, kDt);
    EXPECT_FLOAT_EQ(levelOf(r, corpse), 0.0f);
}

// ---- order-independence: shuffled creation order yields the same field ----

TEST(HerdAlarm, OrderIndependent) {
    // Build two registries with the SAME geometry but creatures created in a different
    // order; the two-phase snapshot must make the resulting levels identical.
    struct Spec {
        float x, z;
        bool pred;
        float level;
        std::uint8_t alarmed;
    };
    std::vector<Spec> specs = {
        {0.0f, 0.0f, false, 0.0f, 1},
        {2.0f, 0.0f, false, 0.0f, 0},
        {4.0f, 0.0f, false, 0.0f, 0},
        {2.0f, 3.0f, false, 0.0f, 0},
    };

    auto build = [](const std::vector<Spec>& order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order) {
            es.push_back(spawnCreature(r, sp.x, sp.z, sp.pred, sp.level, sp.alarmed));
        }
        for (int t = 0; t < 5; ++t)
            RunHerdAlarmOnTick(r, kDt);
        // Read back by geometric key (x,z) so ordering of storage doesn't matter.
        std::vector<std::pair<std::pair<float, float>, float>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            out.push_back(
                {{tf.position.x, tf.position.z}, r.get<Comp::AlarmComponent>(es[i]).level});
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
        EXPECT_FLOAT_EQ(forward[i].second, reversed[i].second);
    }
}

// ---- determinism: run == replay (bit-for-bit over many ticks) ----

TEST(HerdAlarm, RunEqualsReplay) {
    auto simulate = []() {
        entt::registry r;
        std::vector<entt::entity> es;
        es.push_back(spawnCreature(r, 0.0f, 0.0f, false, 0.0f, 1));
        es.push_back(spawnCreature(r, 5.0f, 0.0f, false));
        es.push_back(spawnCreature(r, 9.0f, 1.0f, false));
        es.push_back(spawnCreature(r, 3.0f, 6.0f, true, 0.0f, 1)); // predator herd source
        es.push_back(spawnCreature(r, 4.0f, 7.0f, true));
        std::vector<float> trace;
        for (int t = 0; t < 30; ++t) {
            RunHerdAlarmOnTick(r, kDt);
            for (auto e : es) {
                // Bit-stable capture of the float level.
                trace.push_back(r.get<Comp::AlarmComponent>(e).level);
            }
        }
        return trace;
    };

    const std::vector<float> run = simulate();
    const std::vector<float> replay = simulate();
    ASSERT_EQ(run.size(), replay.size());
    for (std::size_t i = 0; i < run.size(); ++i) {
        // Exact equality: deterministic math must reproduce identical bits.
        EXPECT_EQ(run[i], replay[i]) << "divergence at sample " << i;
    }
}

} // namespace
