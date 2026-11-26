// sim.predator_pack: EMERGENT PACK HUNTING via FLANKING coordination. Predators that
// hunt close together SURROUND the prey: each pack-mate aims for a different approach angle
// around the shared nearest prey so they encircle it, while a lone predator pursues directly.
// Deterministic (id-ordered, two-phase snapshot, DeterministicMath only, NO rng), gated by the
// PackHunterComponent opt-in.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

#include <entt/entt.hpp>

#include "ai/PredatorPackSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/PackHunterComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::kPackRadius;
using luminumbra::ai::PredatorPackStats;
using luminumbra::ai::RunPredatorPackOnTick;

// Spawn a predator carrying a PackHunterComponent (the opt-in). `predator` sets the hunting
// role (a non-predator tag holder is inert).
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

// Spawn a PREY (non-predator). Prey need NOT carry a PackHunterComponent — they are read from
// the full creature roster as shared targets.
entt::entity spawnPrey(entt::registry& r, float x, float z, bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    cr.eaten = eaten;
    return e;
}

const Comp::PackHunterComponent& packOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::PackHunterComponent>(e);
}

// ---- gating / no-op ----

// Empty roster: the system reports nothing and creates/changes nothing.
TEST(PredatorPack, EmptyRosterNoOp) {
    entt::registry r;
    PredatorPackStats s = RunPredatorPackOnTick(r, /*tick=*/0);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.packed, 0);
    EXPECT_EQ(s.hunting, 0);
}

// Predators WITHOUT a PackHunterComponent are invisible to the system (opt-in gate): a world
// of non-participants is a pure no-op even with transform + creature present.
TEST(PredatorPack, NonParticipantsIgnored) {
    entt::registry r;
    // A bare predator (no PackHunterComponent) and a prey.
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = true;
    spawnPrey(r, 5.0f, 0.0f);

    PredatorPackStats s = RunPredatorPackOnTick(r, 0);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.packed, 0);
    EXPECT_EQ(s.hunting, 0);
}

// ---- pack formation: two nearby predators both flag in_pack ----

TEST(PredatorPack, TwoNearbyPredatorsFormPack) {
    entt::registry r;
    auto p0 = spawnPredator(r, 0.0f, 0.0f);
    auto p1 = spawnPredator(r, 3.0f, 0.0f); // well within kPackRadius
    spawnPrey(r, 0.0f, 30.0f);              // a quarry to the north

    PredatorPackStats s = RunPredatorPackOnTick(r, 0);
    EXPECT_EQ(s.participants, 2);
    EXPECT_EQ(s.packed, 2);
    EXPECT_EQ(packOf(r, p0).in_pack, 1u);
    EXPECT_EQ(packOf(r, p1).in_pack, 1u);
}

// ---- FLANKING: pack-mates aim at DIFFERENT approach angles (they encircle, not stack) ----

TEST(PredatorPack, PackMatesFlankAtDifferentAngles) {
    entt::registry r;
    auto p0 = spawnPredator(r, -2.0f, 0.0f);
    auto p1 = spawnPredator(r, 2.0f, 0.0f);
    spawnPrey(r, 0.0f, 30.0f); // shared quarry ahead

    RunPredatorPackOnTick(r, 0);

    const auto& a = packOf(r, p0);
    const auto& b = packOf(r, p1);

    // Both are in the pack and both got a non-zero wish.
    EXPECT_EQ(a.in_pack, 1u);
    EXPECT_EQ(b.in_pack, 1u);
    const float aLen = a.coord_x * a.coord_x + a.coord_z * a.coord_z;
    const float bLen = b.coord_x * b.coord_x + b.coord_z * b.coord_z;
    EXPECT_GT(aLen, 0.0f);
    EXPECT_GT(bLen, 0.0f);

    // Flanking: the two coordination vectors must NOT be identical — they aim at different
    // sides of the prey so the pack surrounds it. (packSize 2 -> angles pi apart.)
    const float ddx = a.coord_x - b.coord_x;
    const float ddz = a.coord_z - b.coord_z;
    EXPECT_GT(ddx * ddx + ddz * ddz, 1.0e-3f) << "pack-mates must flank, not stack";
}

// A bigger pack: three predators get three DISTINCT flank vectors (encirclement scales).
TEST(PredatorPack, ThreePredatorsGetDistinctFlankVectors) {
    entt::registry r;
    auto p0 = spawnPredator(r, -3.0f, 0.0f);
    auto p1 = spawnPredator(r, 0.0f, 0.0f);
    auto p2 = spawnPredator(r, 3.0f, 0.0f);
    spawnPrey(r, 0.0f, 40.0f);

    PredatorPackStats s = RunPredatorPackOnTick(r, 0);
    EXPECT_EQ(s.packed, 3);

    const auto& a = packOf(r, p0);
    const auto& b = packOf(r, p1);
    const auto& c = packOf(r, p2);

    auto differ = [](const Comp::PackHunterComponent& u, const Comp::PackHunterComponent& v) {
        const float dx = u.coord_x - v.coord_x;
        const float dz = u.coord_z - v.coord_z;
        return dx * dx + dz * dz > 1.0e-3f;
    };
    EXPECT_TRUE(differ(a, b));
    EXPECT_TRUE(differ(b, c));
    EXPECT_TRUE(differ(a, c));
}

// ---- a LONE predator: in_pack=0 + a DIRECT vector straight at the prey ----

TEST(PredatorPack, LonePredatorPursuesDirectly) {
    entt::registry r;
    // Single predator; the nearest potential mate is far beyond kPackRadius.
    auto lone = spawnPredator(r, 0.0f, 0.0f);
    spawnPredator(r, kPackRadius + 50.0f, 0.0f); // too far to be a pack-mate
    spawnPrey(r, 0.0f, 10.0f);                   // prey straight north

    RunPredatorPackOnTick(r, 0);

    const auto& p = packOf(r, lone);
    EXPECT_EQ(p.in_pack, 0u);
    // Direct pursuit: the wish points straight at the prey (north, +z), unit length.
    EXPECT_NEAR(p.coord_x, 0.0f, 1.0e-4f);
    EXPECT_NEAR(p.coord_z, 1.0f, 1.0e-4f);
}

// A predator with NO live prey in the world gets a zero wish (nothing to hunt).
TEST(PredatorPack, NoPreyZeroWish) {
    entt::registry r;
    auto p = spawnPredator(r, 0.0f, 0.0f);
    spawnPrey(r, 0.0f, 10.0f, /*eaten=*/true); // only a carcass -> not a valid target

    RunPredatorPackOnTick(r, 0);
    const auto& pk = packOf(r, p);
    EXPECT_FLOAT_EQ(pk.coord_x, 0.0f);
    EXPECT_FLOAT_EQ(pk.coord_z, 0.0f);
}

// A carcass predator (eaten) produces no coordination even with prey around.
TEST(PredatorPack, DeadPredatorInert) {
    entt::registry r;
    auto dead = spawnPredator(r, 0.0f, 0.0f, /*predator=*/true, /*eaten=*/true);
    spawnPredator(r, 2.0f, 0.0f); // a live ally nearby
    spawnPrey(r, 0.0f, 20.0f);

    RunPredatorPackOnTick(r, 0);
    const auto& pk = packOf(r, dead);
    EXPECT_EQ(pk.in_pack, 0u);
    EXPECT_FLOAT_EQ(pk.coord_x, 0.0f);
    EXPECT_FLOAT_EQ(pk.coord_z, 0.0f);
}

// ---- order-independence: shuffled creation order yields the same field ----

TEST(PredatorPack, OrderIndependent) {
    struct Spec {
        float x, z;
        bool predator;
        bool prey;
    };
    std::vector<Spec> specs = {
        {-2.0f, 0.0f, true, false},
        {2.0f, 0.0f, true, false},
        {0.0f, 0.0f, true, false},
        {0.0f, 30.0f, false, true}, // prey
        {6.0f, 25.0f, false, true}, // a second prey
    };

    auto build = [](const std::vector<Spec>& order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order) {
            if (sp.prey) {
                es.push_back(spawnPrey(r, sp.x, sp.z));
            } else {
                es.push_back(spawnPredator(r, sp.x, sp.z, sp.predator));
            }
        }
        RunPredatorPackOnTick(r, 0);
        // Read back predator wishes keyed by geometric position so storage order is irrelevant.
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

    auto forward = build(specs);
    std::reverse(specs.begin(), specs.end());
    auto reversed = build(specs);

    ASSERT_EQ(forward.size(), reversed.size());
    for (std::size_t i = 0; i < forward.size(); ++i) {
        EXPECT_EQ(std::get<0>(forward[i]), std::get<0>(reversed[i]));
        EXPECT_EQ(std::get<1>(forward[i]), std::get<1>(reversed[i]));
        EXPECT_FLOAT_EQ(std::get<2>(forward[i]), std::get<2>(reversed[i]));
        EXPECT_FLOAT_EQ(std::get<3>(forward[i]), std::get<3>(reversed[i]));
        EXPECT_EQ(std::get<4>(forward[i]), std::get<4>(reversed[i]));
    }
}

// ---- determinism: run == replay (bit-for-bit over many ticks) ----

TEST(PredatorPack, RunEqualsReplay) {
    auto simulate = []() {
        entt::registry r;
        std::vector<entt::entity> es;
        es.push_back(spawnPredator(r, -2.0f, 0.0f));
        es.push_back(spawnPredator(r, 2.0f, 0.0f));
        es.push_back(spawnPredator(r, 0.0f, 5.0f));
        es.push_back(spawnPredator(r, 100.0f, 100.0f)); // a lone hunter far away
        auto preyA = spawnPrey(r, 0.0f, 30.0f);
        auto preyB = spawnPrey(r, 105.0f, 90.0f);
        (void)preyA;
        (void)preyB;
        std::vector<float> trace;
        for (std::uint64_t t = 0; t < 30; ++t) {
            RunPredatorPackOnTick(r, t);
            for (auto e : es) {
                const auto& pk = r.get<Comp::PackHunterComponent>(e);
                trace.push_back(pk.coord_x);
                trace.push_back(pk.coord_z);
                trace.push_back(static_cast<float>(pk.in_pack));
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

TEST(PredatorPack, RegionalElapsedRequiresBothParticipants) {
    namespace W = Luminumbra::world;
    for (const bool zero_window : {false, true}) {
        for (const auto ticks : {1u, 8u, 36001u, 65536u}) {
            SCOPED_TRACE(zero_window);
            SCOPED_TRACE(ticks);
            entt::registry r;
            const auto hunter = spawnPredator(r, 4, 4);
            const auto ally = spawnPredator(r, 513, 4);
            const auto denied_prey = spawnPrey(r, 513, 4);
            const auto admitted_prey = spawnPrey(r, 4, 20);
            r.get<Comp::PackHunterComponent>(ally) = {0.75f, -0.25f, 1};
            const std::vector<W::RegionKey> due = zero_window
                                                      ? std::vector<W::RegionKey>{{0, 0}, {1, 0}}
                                                      : std::vector<W::RegionKey>{{0, 0}};
            const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, ticks}, {{1, 0}, 0}};
            const W::RegionConsumerGate gate(r, due, W::RegionElapsedTicks(windows));
            luminumbra::ai::CreatureTickAdmission admission;
            admission.Capture(r, gate);
            // Put denied participants nearby and the admitted prey in a frozen destination.
            r.get<Comp::TransformComponent>(ally).position.x = 6;
            r.get<Comp::TransformComponent>(denied_prey).position = {4, 0, 3};
            r.get<Comp::TransformComponent>(admitted_prey).position.x = 513;
            const auto stats = RunPredatorPackOnTick(r, 65536, &admission, &gate);
            EXPECT_EQ(stats.participants, 1);
            EXPECT_EQ(stats.packed, 0);
            EXPECT_EQ(stats.hunting, 1);
            EXPECT_EQ(packOf(r, hunter).in_pack, 0);
            EXPECT_GT(packOf(r, hunter).coord_x, 0);
            EXPECT_GT(packOf(r, hunter).coord_z, 0); // admitted prey, not the closer denied prey
            EXPECT_EQ(packOf(r, ally).coord_x, 0.75f);
            EXPECT_EQ(packOf(r, ally).coord_z, -0.25f);
            EXPECT_EQ(packOf(r, ally).in_pack, 1);
            // No admitted target: the active hunter clears its wish; the ally remains frozen.
            r.get<Comp::CreatureComponent>(admitted_prey).eaten = true;
            EXPECT_EQ(RunPredatorPackOnTick(r, 65537, &admission, &gate).hunting, 0);
            EXPECT_EQ(packOf(r, hunter).coord_x, 0);
            EXPECT_EQ(packOf(r, hunter).coord_z, 0);
            EXPECT_EQ(packOf(r, ally).coord_x, 0.75f);
            // Admit both owners on the next capture: coordination resumes with unequal windows.
            const std::array<W::RegionKey, 2> resumed_due{{{0, 0}, {1, 0}}};
            const std::map<W::RegionKey, std::uint32_t> resumed_windows{{{0, 0}, ticks},
                                                                        {{1, 0}, 1}};
            const W::RegionConsumerGate resumed(
                r, resumed_due, W::RegionElapsedTicks(resumed_windows));
            admission.Capture(r, resumed);
            EXPECT_EQ(RunPredatorPackOnTick(r, 65538, &admission, &resumed).packed, 2);
            EXPECT_EQ(packOf(r, hunter).in_pack, 1);
            EXPECT_EQ(packOf(r, ally).in_pack, 1);
        }
    }
}

TEST(PredatorPack, RegionalElapsedTargetTiesIgnoreStorageOrder) {
    namespace W = Luminumbra::world;
    for (const bool reverse : {false, true}) {
        entt::registry r;
        const auto hunter = spawnPredator(r, 20, 20);
        spawnPrey(r, reverse ? 30 : 10, 20);
        spawnPrey(r, reverse ? 10 : 30, 20);
        const std::array<W::RegionKey, 1> due{{{0, 0}}};
        const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, 65536}};
        const W::RegionConsumerGate gate(r, due, W::RegionElapsedTicks(windows));
        EXPECT_EQ(RunPredatorPackOnTick(r, 65536, nullptr, &gate).hunting, 1);
        EXPECT_EQ(packOf(r, hunter).coord_x, -1);
        EXPECT_EQ(packOf(r, hunter).coord_z, 0);
    }
}

} // namespace
