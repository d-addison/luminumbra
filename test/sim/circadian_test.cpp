// sim.day_night_activity: DIURNAL / NOCTURNAL ACTIVITY. A creature's activity scales
// with time-of-day: a DIURNAL creature peaks at noon and rests at midnight; a NOCTURNAL
// creature is the inverse. Deterministic (id-ordered, DeterministicMath::Cos only, NO rng),
// gated by the CircadianComponent opt-in. These tests pin the curve: high-at-noon /
// low-at-midnight for diurnal (inverse for nocturnal), activity in [0,1], smooth/continuous
// across the day boundary, run==replay, and empty-roster no-op.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CircadianSystem.h"
#include "components/CircadianComponents.h"

#include "world/WorldClock.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::CircadianActivity;
using luminumbra::ai::CircadianStats;
using luminumbra::ai::RunCircadianOnTick;

// Spawn a creature carrying a CircadianComponent (the opt-in). `nocturnal` sets the phenotype.
entt::entity spawnCreature(entt::registry& r, bool nocturnal) {
    auto e = r.create();
    auto& cc = r.emplace<Comp::CircadianComponent>(e);
    cc.nocturnal = nocturnal ? 1 : 0;
    return e;
}

float activityOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::CircadianComponent>(e).activity;
}

// ---- gating / no-op ----

// Empty roster: the system reports nothing and creates/changes nothing.
TEST(Circadian, EmptyRosterNoOp) {
    entt::registry r;
    CircadianStats s = RunCircadianOnTick(r, 0.5f);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.diurnal, 0);
    EXPECT_EQ(s.nocturnal, 0);
}

// Creatures WITHOUT a CircadianComponent are invisible to the system (opt-in gate): a world
// of non-participants is a pure no-op.
TEST(Circadian, NonParticipantsIgnored) {
    entt::registry r;
    auto e = r.create(); // bare entity, no CircadianComponent
    (void)e;
    CircadianStats s = RunCircadianOnTick(r, 0.5f);
    EXPECT_EQ(s.participants, 0);
}

// ---- core: diurnal HIGH at noon, LOW at midnight ----

TEST(Circadian, DiurnalHighAtNoonLowAtMidnight) {
    entt::registry r;
    auto e = spawnCreature(r, /*nocturnal=*/false);

    RunCircadianOnTick(r, 0.5f); // noon
    const float noon = activityOf(r, e);

    RunCircadianOnTick(r, 0.0f); // midnight
    const float midnight = activityOf(r, e);

    EXPECT_GT(noon, 0.99f);     // peaks at noon (~1).
    EXPECT_LT(midnight, 0.01f); // bottoms out at midnight (~0).
    EXPECT_GT(noon, midnight);
}

// ---- core: nocturnal is the INVERSE ----

TEST(Circadian, NocturnalIsInverse) {
    entt::registry r;
    auto e = spawnCreature(r, /*nocturnal=*/true);

    RunCircadianOnTick(r, 0.5f); // noon
    const float noon = activityOf(r, e);

    RunCircadianOnTick(r, 0.0f); // midnight
    const float midnight = activityOf(r, e);

    EXPECT_LT(noon, 0.01f);     // nocturnal rests at noon (~0).
    EXPECT_GT(midnight, 0.99f); // nocturnal peaks at midnight (~1).
    EXPECT_GT(midnight, noon);
}

// A diurnal and a nocturnal creature at the same instant are complements (sum ~= 1) because
// the two curves are 0.5*(1 -/+ cos) of the same angle.
TEST(Circadian, DiurnalAndNocturnalAreComplements) {
    entt::registry r;
    auto day = spawnCreature(r, /*nocturnal=*/false);
    auto night = spawnCreature(r, /*nocturnal=*/true);

    // Sample several times of day.
    for (float t : {0.0f, 0.13f, 0.25f, 0.5f, 0.77f, 0.9f}) {
        RunCircadianOnTick(r, t);
        const float a = activityOf(r, day);
        const float b = activityOf(r, night);
        EXPECT_NEAR(a + b, 1.0f, 1e-3f) << "at t=" << t;
    }
}

// ---- activity stays in [0,1] across the whole day ----

TEST(Circadian, ActivityInUnitRange) {
    entt::registry r;
    auto day = spawnCreature(r, /*nocturnal=*/false);
    auto night = spawnCreature(r, /*nocturnal=*/true);

    for (int i = 0; i <= 200; ++i) {
        const float t = static_cast<float>(i) / 200.0f; // sweep 0..1
        RunCircadianOnTick(r, t);
        const float a = activityOf(r, day);
        const float b = activityOf(r, night);
        EXPECT_GE(a, 0.0f);
        EXPECT_LE(a, 1.0f);
        EXPECT_GE(b, 0.0f);
        EXPECT_LE(b, 1.0f);
    }
}

// ---- smooth / continuous across the day (incl. the midnight boundary) ----

TEST(Circadian, SmoothAndContinuous) {
    // Step the curve finely; no adjacent samples should jump (the cos curve is continuous,
    // and because cos is periodic there is NO discontinuity at the t=1 -> t=0 wrap either).
    float prev = CircadianActivity(0.0f, /*nocturnal=*/false);
    const int N = 1000;
    for (int i = 1; i <= N; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(N); // 1/N .. 1.0
        const float cur = CircadianActivity(t, /*nocturnal=*/false);
        // Max slope of 0.5*(1-cos(2*pi*t)) is pi (~3.1416); over a step of 1/N the change is
        // well under 0.02. A generous bound catches any discontinuity.
        EXPECT_LT(std::fabs(cur - prev), 0.02f) << "jump at t=" << t;
        prev = cur;
    }

    // Explicit day-boundary continuity: t just below 1 ~= t just above 0 (same clock phase).
    const float justBelowOne = CircadianActivity(0.999f, false);
    const float justAboveZero = CircadianActivity(0.001f, false);
    EXPECT_NEAR(justBelowOne, justAboveZero, 1e-2f);

    // And the wrap is real: t=1.0 maps to the same value as t=0.0 (full period).
    EXPECT_NEAR(CircadianActivity(1.0f, false), CircadianActivity(0.0f, false), 1e-3f);
}

// Activity rises monotonically from midnight to noon for a diurnal creature (then would fall
// back) — sanity that "scales with time-of-day" means a real ramp, not a step.
TEST(Circadian, DiurnalRisesFromMidnightToNoon) {
    float prev = CircadianActivity(0.0f, false);
    for (int i = 1; i <= 50; ++i) {
        const float t = 0.5f * static_cast<float>(i) / 50.0f; // 0 .. 0.5 (midnight -> noon)
        const float cur = CircadianActivity(t, false);
        EXPECT_GE(cur, prev - 1e-4f) << "should not decrease before noon, t=" << t;
        prev = cur;
    }
}

// ---- wrap handling: out-of-range clock inputs map onto the day ----

TEST(Circadian, ClockWrapsIntoDay) {
    // 1.5 is the same phase as 0.5 (noon); -0.5 is the same phase as 0.5 too.
    EXPECT_NEAR(CircadianActivity(1.5f, false), CircadianActivity(0.5f, false), 1e-3f);
    EXPECT_NEAR(CircadianActivity(-0.5f, false), CircadianActivity(0.5f, false), 1e-3f);
    // 2.0 wraps to 0.0 (midnight).
    EXPECT_NEAR(CircadianActivity(2.0f, false), CircadianActivity(0.0f, false), 1e-3f);
}

// ---- stats reporting ----

TEST(Circadian, StatsCountPhenotypes) {
    entt::registry r;
    spawnCreature(r, false);
    spawnCreature(r, false);
    spawnCreature(r, true);
    CircadianStats s = RunCircadianOnTick(r, 0.5f);
    EXPECT_EQ(s.participants, 3);
    EXPECT_EQ(s.diurnal, 2);
    EXPECT_EQ(s.nocturnal, 1);
}

// ---- determinism: run == replay (bit-for-bit over a day sweep) ----

TEST(Circadian, RunEqualsReplay) {
    auto simulate = []() {
        entt::registry r;
        std::vector<entt::entity> es;
        es.push_back(spawnCreature(r, false));
        es.push_back(spawnCreature(r, true));
        es.push_back(spawnCreature(r, false));
        es.push_back(spawnCreature(r, true));
        std::vector<float> trace;
        for (int t = 0; t < 120; ++t) {
            const float tod = static_cast<float>(t) / 120.0f; // sweep a full day
            RunCircadianOnTick(r, tod);
            for (auto e : es) {
                trace.push_back(r.get<Comp::CircadianComponent>(e).activity);
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

TEST(Circadian, RegionalElapsedSamplesApprovedSolarPhase) {
    namespace W = Luminumbra::world;
    for (const auto ticks : {1u, 8u, 36001u, 65536u}) {
        SCOPED_TRACE(ticks);
        entt::registry r;
        const auto active = spawnCreature(r, false);
        const auto nocturnal = spawnCreature(r, true); // supported non-creature opt-in
        const auto frozen = spawnCreature(r, false);
        const auto zero = spawnCreature(r, false);
        const auto missing_window = spawnCreature(r, false);
        const auto missing_position = spawnCreature(r, false);
        const auto invalid_position = spawnCreature(r, false);
        const std::array entities{
            active, nocturnal, frozen, zero, missing_window, missing_position, invalid_position};
        for (const auto entity : entities) {
            r.get<Comp::CircadianComponent>(entity).activity = 0.37f;
            if (entity != missing_position) {
                r.emplace<Comp::TransformComponent>(entity).position = {4, 0, 4};
            }
        }
        r.emplace<Comp::CreatureComponent>(active);
        r.emplace<Comp::CreatureComponent>(frozen);
        r.get<Comp::TransformComponent>(frozen).position.x = 513;
        r.get<Comp::TransformComponent>(zero).position.x = 1025;
        r.get<Comp::TransformComponent>(missing_window).position.x = 1537;
        r.get<Comp::TransformComponent>(invalid_position).position.x =
            std::numeric_limits<float>::infinity();
        const std::array<W::RegionKey, 3> due{{{0, 0}, {2, 0}, {3, 0}}};
        const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, ticks}, {{2, 0}, 0}};
        const W::RegionConsumerGate gate(r, due, W::RegionElapsedTicks(windows));
        luminumbra::ai::CreatureTickAdmission admission;
        admission.Capture(r, gate);
        // Moving cannot transfer an owner's elapsed window or thaw a frozen creature.
        r.get<Comp::TransformComponent>(active).position.x = 513;
        r.get<Comp::TransformComponent>(frozen).position.x = 4;
        const W::WorldClock clock;
        const auto phase = static_cast<float>(clock.solar_phase(27000));
        EXPECT_EQ(phase, 0.25f);
        EXPECT_NE(phase, static_cast<float>(clock.day_phase(27000)));
        const auto stats = RunCircadianOnTick(r, phase, 0.8f, &admission, &gate);
        EXPECT_EQ(stats.participants, 2);
        EXPECT_EQ(stats.diurnal, 1);
        EXPECT_EQ(stats.nocturnal, 1);
        // Three due-index entries, two captured owners and three positioned opt-ins:
        // elapsed window length must not multiply owner lookups or replay the roster.
        EXPECT_EQ(gate.owner_visits(), 8u);
        EXPECT_EQ(activityOf(r, active), CircadianActivity(phase, false, 0.8f));
        EXPECT_EQ(activityOf(r, nocturnal), CircadianActivity(phase, true, 0.8f));
        for (const auto entity :
             {frozen, zero, missing_window, missing_position, invalid_position}) {
            EXPECT_EQ(activityOf(r, entity), 0.37f);
        }
        // Gate-only callers also schedule non-creature opt-ins; empty due sets do nothing.
        const W::RegionConsumerGate empty(r, std::span<const W::RegionKey>{});
        EXPECT_EQ(RunCircadianOnTick(r, 0.0f, 1.0f, nullptr, &empty).participants, 0);
        // The existing entry point keeps its boolean-only zero-window behaviour.
        EXPECT_EQ(RunCircadianOnTick(r, 0.0f, 1.0f, &admission).participants, 6);
        EXPECT_EQ(activityOf(r, zero), CircadianActivity(0.0f, false));
    }
}

} // namespace
