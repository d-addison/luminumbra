// I9-ECO COMPOSED-PIPELINE determinism: the per-system tests prove each system is run==replay
// in isolation, but the integrated STACK (brain -> mate-seek -> steering consumer -> sexual
// reproduction -> lifespan -> decomposition -> herd alarm -> pack -> migration -> territory),
// run in GameSession's deterministic slot order, must ALSO be byte-exact run==replay -- so that
// e2e determinism holds. This composes the sim systems (no world/physics) on a registry carrying
// the full §4 component set and asserts two identical runs produce identical state.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureBrainSystem.h"
#include "ai/CreatureReproductionSystem.h"
#include "ai/HerdAlarmSystem.h"
#include "ai/LifespanSystem.h"
#include "ai/DecompositionSystem.h"
#include "ai/PredatorPackSystem.h"
#include "ai/MigrationSystem.h"
#include "ai/TerritorySystem.h"
#include "ai/SteeringConsumer.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/AlarmComponents.h"
#include "components/MortalComponents.h"
#include "components/DecayComponents.h"
#include "components/PackHunterComponents.h"
#include "components/MigratoryComponents.h"
#include "components/TerritoryComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;

// One deterministic ecology tick in GameSession's slot order (sim-only: physics is a separate
// non-hashed layer; here every creature uses the brain's direct X/Z integration).
void EcologyTick(entt::registry& r, std::uint64_t tick) {
    constexpr float dt = 1.0f / 30.0f;
    luminumbra::ai::RunCreatureBrainSystemOnTick(r, dt);     // decide + move (bodyless)
    luminumbra::ai::RunMateSeekingOnTick(r);                 // 2e-mate A
    luminumbra::ai::RunSteeringConsumerOnTick(r);            // 2e-steer
    luminumbra::ai::RunMatingResolveOnTick(r, tick);        // 2e-mate B
    luminumbra::ai::RunHerdAlarmOnTick(r, dt);
    luminumbra::ai::RunLifespanOnTick(r, tick);
    luminumbra::ai::RunDecompositionOnTick(r, tick);
    luminumbra::ai::RunPredatorPackOnTick(r, tick);
    luminumbra::ai::RunMigrationOnTick(r, 0.25f);
    luminumbra::ai::RunTerritoryOnTick(r, tick);
}

// Build a rich roster: a 2-predator pack, a small breeding/aging/territorial/migratory herd.
void Populate(entt::registry& r) {
    auto pred = [&](float x, float z) {
        auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position = Luminumbra::Vec3(x, 0.0f, z);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = true; cr.hunger = 0.9f; cr.move_speed = 4.2f;
        r.emplace<Comp::PackHunterComponent>(e);
        r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 5000u;
    };
    int idx = 0;
    auto prey = [&](float x, float z) {
        auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position = Luminumbra::Vec3(x, 0.0f, z);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = false; cr.hunger = 0.05f; cr.stamina = 1.0f; cr.move_speed = 3.0f;
        auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
        gn.female = (idx++ % 2 == 0); gn.age_ticks = 100u;
        r.emplace<Comp::AlarmComponent>(e);
        r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 600u;
        r.emplace<Comp::DecayComponent>(e).decay_duration = 90u;
        r.emplace<Comp::MigratoryComponent>(e);
        r.emplace<Comp::TerritoryComponent>(e);
        r.emplace<Comp::TerritoryBiasComponent>(e);
    };
    pred(-6.0f, 9.0f); pred(6.0f, 9.0f);
    for (int i = 0; i < 6; ++i) prey(-7.0f + i * 2.4f, -2.0f);
}

// Snapshot all hashable creature state into a flat vector (entity-id sorted -> stable order).
std::vector<float> Snapshot(entt::registry& r) {
    std::vector<entt::entity> es(r.view<Comp::CreatureComponent>().begin(),
                                 r.view<Comp::CreatureComponent>().end());
    std::sort(es.begin(), es.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    std::vector<float> out;
    out.push_back(static_cast<float>(es.size()));
    for (auto e : es) {
        const auto& tf = r.get<Comp::TransformComponent>(e);
        const auto& cr = r.get<Comp::CreatureComponent>(e);
        out.insert(out.end(), {tf.position.x, tf.position.y, tf.position.z,
                               cr.wish_x, cr.wish_z, cr.hunger, cr.stamina,
                               static_cast<float>(cr.eaten)});
        if (const auto* gn = r.try_get<Comp::CreatureGenomeComponent>(e))
            out.insert(out.end(), {gn->move_speed, static_cast<float>(gn->generation),
                                   static_cast<float>(gn->age_ticks)});
        if (const auto* al = r.try_get<Comp::AlarmComponent>(e)) out.push_back(al->level);
        if (const auto* pk = r.try_get<Comp::PackHunterComponent>(e))
            out.insert(out.end(), {pk->coord_x, pk->coord_z, static_cast<float>(pk->in_pack)});
    }
    return out;
}

std::vector<float> RunPipeline(int ticks) {
    entt::registry r;
    Populate(r);
    for (int t = 0; t < ticks; ++t) EcologyTick(r, static_cast<std::uint64_t>(t));
    return Snapshot(r);
}

// The whole composed ecology stack is byte-exact run == replay over a long horizon.
TEST(EcologyPipeline, DeterministicOverManyTicks) {
    EXPECT_EQ(RunPipeline(400), RunPipeline(400));
}

// Sanity: the pipeline is actually DOING something (so the determinism check isn't trivially
// satisfied by a frozen world) -- the population changes (births and/or deaths) over time.
TEST(EcologyPipeline, PopulationEvolves) {
    entt::registry r;
    Populate(r);
    const std::size_t start = r.view<Comp::CreatureComponent>().size();
    for (int t = 0; t < 400; ++t) EcologyTick(r, static_cast<std::uint64_t>(t));
    const std::size_t end = r.view<Comp::CreatureComponent>().size();
    EXPECT_NE(start, end) << "the ecology should birth and/or cull creatures over 400 ticks";
}

// An empty world ticks to a no-op (the whole stack is gated).
TEST(EcologyPipeline, EmptyRosterNoOp) {
    entt::registry r;
    for (int t = 0; t < 20; ++t) EcologyTick(r, static_cast<std::uint64_t>(t));
    EXPECT_EQ(r.view<Comp::CreatureComponent>().size(), 0u);
}

}  // namespace
