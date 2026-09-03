// sim.scavenging: SCAVENGERS EAT CARCASSES (death -> food). A hungry scavenger seeks
// the nearest carcass (a dead creature: eaten==1 or MortalComponent.dead==1), steers toward
// it via a wish vector, and FEEDS when in range (hunger falls, feeding=1). Deterministic
// (id-ordered, two-phase carcass snapshot, DeterministicMath only, NO rng), gated by the
// ScavengerComponent opt-in.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "ai/ScavengingSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/MortalComponents.h"
#include "components/ScavengerComponent.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::kScavengeFeedRadius;
using luminumbra::ai::kScavengeHungerThreshold;
using luminumbra::ai::RunScavengingOnTick;
using luminumbra::ai::ScavengingStats;

// Spawn a SCAVENGER: a creature carrying a ScavengerComponent (the opt-in gate). `hunger`
// seeds its appetite (0 sated .. 1 starving).
entt::entity spawnScavenger(entt::registry& r, float x, float z, float hunger) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.hunger = hunger;
    cr.eaten = false;
    r.emplace<Comp::ScavengerComponent>(e);
    return e;
}

// Spawn a CARCASS via the eaten==1 path (no MortalComponent).
entt::entity spawnCarcassEaten(entt::registry& r, float x, float z) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.eaten = true;
    return e;
}

// Spawn a CARCASS via the MortalComponent.dead==1 path (a creature that died of old age /
// starvation, not predation).
entt::entity spawnCarcassMortal(entt::registry& r, float x, float z) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    r.emplace<Comp::CreatureComponent>(e); // alive flag-wise (eaten==0)...
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.dead = 1; // ...but flagged dead -> still a carcass.
    return e;
}

const Comp::ScavengerComponent& scav(entt::registry& r, entt::entity e) {
    return r.get<Comp::ScavengerComponent>(e);
}
float hungerOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::CreatureComponent>(e).hunger;
}

// ---- gating / no-op ----

// Empty roster: the system reports nothing and creates/changes nothing.
TEST(Scavenging, EmptyRosterNoOp) {
    entt::registry r;
    ScavengingStats s = RunScavengingOnTick(r, /*tick=*/0);
    EXPECT_EQ(s.scavengers, 0);
    EXPECT_EQ(s.carcasses, 0);
    EXPECT_EQ(s.seeking, 0);
    EXPECT_EQ(s.feeding, 0);
}

// A world with creatures but NO ScavengerComponent is a pure no-op (opt-in gate): even a
// carcass present, nothing participates.
TEST(Scavenging, NonParticipantsIgnored) {
    entt::registry r;
    spawnCarcassEaten(r, 0.0f, 0.0f); // a carcass, but nobody to eat it.
    ScavengingStats s = RunScavengingOnTick(r, 0);
    EXPECT_EQ(s.scavengers, 0);
    EXPECT_EQ(s.feeding, 0);
}

// ---- core: a hungry scavenger's wish points TOWARD the nearest carcass ----

TEST(Scavenging, HungryScavengerWishPointsToNearestCarcass) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.9f);
    // Carcass to the +x (and far enough to steer, not feed).
    spawnCarcassEaten(r, 10.0f, 0.0f);

    ScavengingStats st = RunScavengingOnTick(r, 0);
    EXPECT_EQ(st.seeking, 1);
    EXPECT_EQ(st.feeding, 0);

    const auto& sc = scav(r, s);
    EXPECT_GT(sc.wish_x, 0.0f);       // steers toward +x carcass.
    EXPECT_FLOAT_EQ(sc.wish_z, 0.0f); // no z component for a colinear target.
    EXPECT_EQ(sc.feeding, 0);         // not in range yet.
    // wish is a unit steer.
    const float mag = sc.wish_x * sc.wish_x + sc.wish_z * sc.wish_z;
    EXPECT_NEAR(mag, 1.0f, 1.0e-4f);
}

// Two carcasses: the scavenger steers toward the NEARER one.
TEST(Scavenging, SteersTowardNearestOfTwo) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, 0.9f);
    spawnCarcassEaten(r, 20.0f, 0.0f); // far, +x
    spawnCarcassEaten(r, 0.0f, -8.0f); // nearer, -z

    RunScavengingOnTick(r, 0);
    const auto& sc = scav(r, s);
    EXPECT_LT(sc.wish_z, 0.0f); // pulled toward the nearer -z carcass.
    EXPECT_NEAR(sc.wish_x, 0.0f, 1.0e-4f);
}

// ---- reaching a carcass feeds: hunger falls, feeding=1 ----

TEST(Scavenging, ReachingCarcassFeeds) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.9f);
    // Carcass within the feed radius.
    spawnCarcassEaten(r, kScavengeFeedRadius * 0.5f, 0.0f);

    const float h0 = hungerOf(r, s);
    ScavengingStats st = RunScavengingOnTick(r, 0);
    EXPECT_EQ(st.feeding, 1);

    EXPECT_EQ(scav(r, s).feeding, 1);
    EXPECT_LT(hungerOf(r, s), h0); // ate -> hunger dropped.
    // On the carcass: holds position (zero wish).
    EXPECT_FLOAT_EQ(scav(r, s).wish_x, 0.0f);
    EXPECT_FLOAT_EQ(scav(r, s).wish_z, 0.0f);
}

// Feeding lowers hunger but never below 0 (clamped) over repeated ticks.
TEST(Scavenging, FeedingClampsHungerAtZero) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.45f); // just above threshold
    spawnCarcassEaten(r, 0.0f, 0.0f);                         // right on top
    for (int i = 0; i < 50; ++i)
        RunScavengingOnTick(r, static_cast<std::uint64_t>(i));
    EXPECT_GE(hungerOf(r, s), 0.0f);
    EXPECT_LE(hungerOf(r, s), kScavengeHungerThreshold); // ate down to/below threshold.
}

// A carcass reached via the MortalComponent.dead path also feeds.
TEST(Scavenging, MortalDeadCountsAsCarcass) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, 0.9f);
    spawnCarcassMortal(r, kScavengeFeedRadius * 0.5f, 0.0f);

    ScavengingStats st = RunScavengingOnTick(r, 0);
    EXPECT_EQ(st.carcasses, 1);
    EXPECT_EQ(st.feeding, 1);
    EXPECT_EQ(scav(r, s).feeding, 1);
}

// ---- a well-fed scavenger doesn't seek or feed ----

TEST(Scavenging, WellFedDoesNotSeek) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.0f); // sated
    spawnCarcassEaten(r, 1.0f, 0.0f);                        // carcass right there

    const float h0 = hungerOf(r, s);
    ScavengingStats st = RunScavengingOnTick(r, 0);
    EXPECT_EQ(st.seeking, 0);
    EXPECT_EQ(st.feeding, 0);
    EXPECT_FLOAT_EQ(scav(r, s).wish_x, 0.0f);
    EXPECT_FLOAT_EQ(scav(r, s).wish_z, 0.0f);
    EXPECT_EQ(scav(r, s).feeding, 0);
    EXPECT_FLOAT_EQ(hungerOf(r, s), h0); // didn't eat.
}

// ---- no carcasses -> zero wish + no feeding ----

TEST(Scavenging, NoCarcassesZeroWishNoFeeding) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.9f); // hungry...
    // ...but only LIVE creatures around (no carcass).
    auto live = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(live);
    tf.position = Luminumbra::Vec3(2.0f, 0.0f, 0.0f);
    r.emplace<Comp::CreatureComponent>(live); // alive (eaten==0, no mortal)

    const float h0 = hungerOf(r, s);
    ScavengingStats st = RunScavengingOnTick(r, 0);
    EXPECT_EQ(st.carcasses, 0);
    EXPECT_EQ(st.seeking, 0);
    EXPECT_EQ(st.feeding, 0);
    EXPECT_FLOAT_EQ(scav(r, s).wish_x, 0.0f);
    EXPECT_FLOAT_EQ(scav(r, s).wish_z, 0.0f);
    EXPECT_EQ(scav(r, s).feeding, 0);
    EXPECT_FLOAT_EQ(hungerOf(r, s), h0);
}

// A dead scavenger (its own eaten flag) doesn't scavenge.
TEST(Scavenging, DeadScavengerDoesNotScavenge) {
    entt::registry r;
    auto s = spawnScavenger(r, 0.0f, 0.0f, /*hunger=*/0.9f);
    r.get<Comp::CreatureComponent>(s).eaten = true; // the scavenger itself is now a carcass
    spawnCarcassEaten(r, 5.0f, 0.0f);

    const float h0 = hungerOf(r, s);
    ScavengingStats st = RunScavengingOnTick(r, 0);
    EXPECT_EQ(st.seeking, 0);
    EXPECT_EQ(st.feeding, 0);
    EXPECT_FLOAT_EQ(scav(r, s).wish_x, 0.0f);
    EXPECT_FLOAT_EQ(scav(r, s).wish_z, 0.0f);
    EXPECT_FLOAT_EQ(hungerOf(r, s), h0);
}

// ---- order-independence: shuffled creation order yields the same outcome ----

TEST(Scavenging, OrderIndependent) {
    struct Spec {
        float x, z;
        bool scavenger;
        float hunger;
        bool carcass;
    };
    std::vector<Spec> specs = {
        {0.0f, 0.0f, true, 0.9f, false},   // hungry scavenger at origin
        {10.0f, 0.0f, false, 0.0f, true},  // carcass +x
        {0.0f, 6.0f, false, 0.0f, true},   // nearer carcass +z
        {-3.0f, -3.0f, true, 0.8f, false}, // another scavenger
    };

    auto build = [](const std::vector<Spec>& order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order) {
            if (sp.scavenger) {
                es.push_back(spawnScavenger(r, sp.x, sp.z, sp.hunger));
            } else if (sp.carcass) {
                es.push_back(spawnCarcassEaten(r, sp.x, sp.z));
            }
        }
        for (int t = 0; t < 5; ++t)
            RunScavengingOnTick(r, static_cast<std::uint64_t>(t));
        // Read back scavenger results keyed by geometric position (storage order agnostic).
        std::vector<std::pair<std::pair<float, float>, std::pair<float, float>>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (!order[i].scavenger)
                continue;
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            const auto& sc = r.get<Comp::ScavengerComponent>(es[i]);
            out.push_back({{tf.position.x, tf.position.z}, {sc.wish_x, sc.wish_z}});
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

// ---- determinism: run == replay (bit-for-bit over many ticks) ----

TEST(Scavenging, RunEqualsReplay) {
    auto simulate = []() {
        entt::registry r;
        std::vector<entt::entity> scavs;
        scavs.push_back(spawnScavenger(r, 0.0f, 0.0f, 0.9f));
        scavs.push_back(spawnScavenger(r, 8.0f, 2.0f, 0.7f));
        scavs.push_back(spawnScavenger(r, -5.0f, 4.0f, 0.5f));
        spawnCarcassEaten(r, 3.0f, 3.0f);
        spawnCarcassMortal(r, -2.0f, 6.0f);
        spawnCarcassEaten(r, 12.0f, -1.0f);
        std::vector<float> trace;
        for (int t = 0; t < 40; ++t) {
            RunScavengingOnTick(r, static_cast<std::uint64_t>(t));
            for (auto e : scavs) {
                const auto& sc = r.get<Comp::ScavengerComponent>(e);
                trace.push_back(sc.wish_x);
                trace.push_back(sc.wish_z);
                trace.push_back(static_cast<float>(sc.feeding));
                trace.push_back(r.get<Comp::CreatureComponent>(e).hunger);
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

} // namespace
