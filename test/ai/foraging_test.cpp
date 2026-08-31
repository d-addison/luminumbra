// ant-trail FORAGING (Deneubourg double-bridge) — deposit-on-both-trips into two
// ScentField channels, follow the opposite channel, pick up at food / deliver at the nest. Pure
// grid sim: integer cells, id-ordered, libm-free, deterministic.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/ForagingSystem.h"
#include "components/ForagingComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::ForagingParams;
using luminumbra::ai::kFoodTrailChannel;
using luminumbra::ai::kHomeTrailChannel;
using luminumbra::ai::RunForagingOnTick;
using luminumbra::ai::ScentField;

entt::entity spawnForager(entt::registry& r, std::int32_t home_x, std::int32_t home_z) {
    auto e = r.create();
    auto& f = r.emplace<Comp::ForagerComponent>(e);
    f.home_x = home_x;
    f.home_z = home_z;
    f.cell_x = home_x;
    f.cell_z = home_z;
    return e;
}
entt::entity
spawnFood(entt::registry& r, std::int32_t x, std::int32_t z, std::int32_t amount = 1000000) {
    auto e = r.create();
    auto& f = r.emplace<Comp::FoodSourceComponent>(e);
    f.cell_x = x;
    f.cell_z = z;
    f.amount = amount;
    return e;
}

// Empty roster: no foragers -> no-op (canonical world_hash byte-identical).
TEST(Foraging, EmptyRosterNoOp) {
    entt::registry r;
    ScentField f(8, 8, 4);
    const auto s = RunForagingOnTick(r, f, {});
    EXPECT_EQ(s.foragers, 0u);
    EXPECT_EQ(s.deliveries, 0u);
}

// A forager walks out to food, picks it up, returns to the nest, and delivers.
TEST(Foraging, RoundTripDelivers) {
    entt::registry r;
    ScentField f(20, 5, 4);
    auto ant = spawnForager(r, 0, 0);
    spawnFood(r, 4, 0);
    for (int t = 0; t < 30; ++t)
        RunForagingOnTick(r, f, {});
    EXPECT_GE(r.get<Comp::ForagerComponent>(ant).deliveries, 1u)
        << "should complete >=1 round trip";
}

// Deposit-on-both-trips: after a round trip BOTH the home trail (laid outbound) and the food trail
// (laid laden) carry pheromone somewhere along the route.
TEST(Foraging, DepositsOnBothTrips) {
    entt::registry r;
    ScentField f(20, 5, 4);
    spawnForager(r, 0, 0);
    spawnFood(r, 4, 0);
    for (int t = 0; t < 20; ++t)
        RunForagingOnTick(r, f, {});
    double home = 0.0, food = 0.0;
    for (int x = 0; x <= 4; ++x) {
        home += f.Sample(kHomeTrailChannel, x, 0);
        food += f.Sample(kFoodTrailChannel, x, 0);
    }
    EXPECT_GT(home, 0.0) << "outbound legs lay the home trail";
    EXPECT_GT(food, 0.0) << "laden legs lay the food trail";
}

// run == replay: identical setup + ticks -> identical deliveries + field accumulation.
TEST(Foraging, Deterministic) {
    auto run = [] {
        entt::registry r;
        ScentField f(24, 5, 4);
        auto ant = spawnForager(r, 0, 0);
        spawnFood(r, 6, 0);
        for (int t = 0; t < 60; ++t) {
            RunForagingOnTick(r, f, {});
            f.Step(0.2, 1, 0.05);
        }
        double acc = 0.0;
        for (int c = 2; c < 4; ++c)
            for (int z = 0; z < 5; ++z)
                for (int x = 0; x < 24; ++x)
                    acc += f.Sample(c, x, z);
        return std::pair<std::uint32_t, double>(r.get<Comp::ForagerComponent>(ant).deliveries, acc);
    };
    EXPECT_EQ(run(), run());
}

// Shortest-path throughput (the basis of double-bridge selection): a shorter route is completed
// more often per unit time, so it delivers more food than a longer one over the same number of
// ticks.
TEST(Foraging, ShorterRouteDeliversMore) {
    auto routeDeliveries = [](std::int32_t foodX, int ticks) {
        entt::registry r;
        ScentField f(40, 5, 4);
        auto ant = spawnForager(r, 0, 0);
        spawnFood(r, foodX, 0);
        for (int t = 0; t < ticks; ++t) {
            RunForagingOnTick(r, f, {});
            f.Step(0.2, 1, 0.05);
        }
        return r.get<Comp::ForagerComponent>(ant).deliveries;
    };
    EXPECT_GT(routeDeliveries(3, 200), routeDeliveries(12, 200))
        << "the shorter round trip should yield more deliveries (shortest-path reinforcement)";
}

//  evaporation contrast (the double-bridge / Deneubourg basis): evaporation (rho > 0) bounds the
// trail to an equilibrium so the ACTIVE route stays a sharp, followable signal, whereas ZERO
// evaporation lets every visited cell accumulate without bound — the trail SATURATES and the colony
// can no longer tell routes apart (the stagnation the MAX-MIN ant system guards against, spec
// "stagnates at rho=0"). Identical foraging traffic under rho=0.05 vs rho=0; the no-evaporation
// peak trail grows far larger (unbounded buildup). Pure grid sim, deterministic.
TEST(Foraging, EvaporationBoundsTrailZeroRhoSaturates) {
    auto peakTrail = [](double rho) {
        entt::registry r;
        ScentField f(40, 5, 4);
        spawnForager(r, 0, 0);
        spawnFood(r, 5, 0);
        for (int t = 0; t < 400; ++t) {
            RunForagingOnTick(r, f, {});
            f.Step(0.2, 1, rho);
        }
        double peak = 0.0;
        for (int c = 2; c < 4; ++c)
            for (int z = 0; z < 5; ++z)
                for (int x = 0; x < 40; ++x)
                    peak = std::max(peak, f.Sample(c, x, z));
        return peak;
    };
    const double evap = peakTrail(0.05); // bounded equilibrium
    const double none = peakTrail(0.0);  // unbounded accumulation -> saturation
    EXPECT_GT(none, evap * 2.0)
        << "zero evaporation lets the trail accumulate unbounded (stagnation); "
           "evaporation bounds it so the active route stays distinguished";
}

// Pure trail following (goal_weight 0): with a pre-seeded food-trail gradient an OUTBOUND forager
// climbs UP the trail — the stigmergy read side, independent of goal-seeking.
TEST(Foraging, FollowsTrailWhenGoalNeutral) {
    entt::registry r;
    ScentField f(20, 5, 4);
    auto ant = spawnForager(r, 5, 2); // outbound (not carrying)
    // Seed a food-trail blob to the EAST and diffuse it into a smooth gradient.
    f.Deposit(kFoodTrailChannel, 12, 2, 500.0);
    for (int i = 0; i < 6; ++i)
        f.Step(0.25, 2, 0.0);
    ForagingParams p;
    p.goal_weight = 0.0; // pure trail following
    const std::int32_t x0 = r.get<Comp::ForagerComponent>(ant).cell_x;
    for (int t = 0; t < 4; ++t)
        RunForagingOnTick(r, f, p);
    EXPECT_GT(r.get<Comp::ForagerComponent>(ant).cell_x, x0)
        << "should climb the food trail toward +x";
}

} // namespace
