//  COMPOSED-PIPELINE determinism: the per-system tests prove each system is run==replay
// in isolation, but the integrated STACK (brain -> mate-seek -> steering consumer -> sexual
// reproduction -> lifespan -> decomposition -> herd alarm -> pack -> migration -> territory),
// run in GameSession's deterministic slot order, must ALSO be byte-exact run==replay -- so that
// e2e determinism holds. This composes the sim systems (no world/physics) on a registry carrying
// the full  component set and asserts two identical runs produce identical state.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureBrainSystem.h"
#include "ai/CreatureReproductionSystem.h"
#include "ai/DecompositionSystem.h"
#include "ai/HerdAlarmSystem.h"
#include "ai/LifespanSystem.h"
#include "ai/MigrationSystem.h"
#include "ai/PredatorPackSystem.h"
#include "ai/ScentDepositSystem.h" // prey deposits
#include "ai/ScentField.h"         // the stigmergy substrate
#include "ai/SteeringConsumer.h"
#include "ai/TerritorySystem.h"
#include "ai/WildlifeFoliageSystem.h" // the live feeding loop
#include "components/AlarmComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/InstinctComponents.h" // SensableComponent
#include "components/MigratoryComponents.h"
#include "components/MortalComponents.h"
#include "components/PackHunterComponents.h"
#include "components/TerritoryComponents.h"
#include "systems/FarmingSystem.h" // MakePlantFromSpecies (the sim wiring point)

namespace {

namespace Comp = ::Luminumbra::Components;

// One deterministic ecology tick in GameSession's slot order (sim-only: physics is a separate
// non-hashed layer; here every creature uses the brain's direct X/Z integration).
void EcologyTick(entt::registry& r, std::uint64_t tick) {
    constexpr float dt = 1.0f / 30.0f;
    luminumbra::ai::RunCreatureBrainSystemOnTick(r, dt); // decide + move (bodyless)
    luminumbra::ai::RunMateSeekingOnTick(r);             // 2e-mate A
    luminumbra::ai::RunSteeringConsumerOnTick(r);        // 2e-steer
    luminumbra::ai::RunMatingResolveOnTick(r, tick);     // 2e-mate B
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
        cr.is_predator = true;
        cr.hunger = 0.9f;
        cr.move_speed = 4.2f;
        r.emplace<Comp::PackHunterComponent>(e);
        r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 5000u;
    };
    int idx = 0;
    auto prey = [&](float x, float z) {
        auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position = Luminumbra::Vec3(x, 0.0f, z);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = false;
        cr.hunger = 0.05f;
        cr.stamina = 1.0f;
        cr.move_speed = 3.0f;
        auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
        gn.female = (idx++ % 2 == 0);
        gn.age_ticks = 100u;
        r.emplace<Comp::AlarmComponent>(e);
        r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 600u;
        r.emplace<Comp::DecayComponent>(e).decay_duration = 90u;
        r.emplace<Comp::MigratoryComponent>(e);
        r.emplace<Comp::TerritoryComponent>(e);
        r.emplace<Comp::TerritoryBiasComponent>(e);
    };
    pred(-6.0f, 9.0f);
    pred(6.0f, 9.0f);
    for (int i = 0; i < 6; ++i)
        prey(-7.0f + i * 2.4f, -2.0f);
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
        out.insert(out.end(),
                   {tf.position.x,
                    tf.position.y,
                    tf.position.z,
                    cr.wish_x,
                    cr.wish_z,
                    cr.hunger,
                    cr.stamina,
                    static_cast<float>(cr.eaten)});
        if (const auto* gn = r.try_get<Comp::CreatureGenomeComponent>(e))
            out.insert(out.end(),
                       {gn->move_speed,
                        static_cast<float>(gn->generation),
                        static_cast<float>(gn->age_ticks)});
        if (const auto* al = r.try_get<Comp::AlarmComponent>(e))
            out.push_back(al->level);
        if (const auto* pk = r.try_get<Comp::PackHunterComponent>(e))
            out.insert(out.end(), {pk->coord_x, pk->coord_z, static_cast<float>(pk->in_pack)});
    }
    return out;
}

std::vector<float> RunPipeline(int ticks) {
    entt::registry r;
    Populate(r);
    for (int t = 0; t < ticks; ++t)
        EcologyTick(r, static_cast<std::uint64_t>(t));
    return Snapshot(r);
}

// The whole composed ecology stack is byte-exact run == replay over a long horizon.
TEST(EcologyPipeline, DeterministicOverManyTicks) {
    EXPECT_EQ(RunPipeline(400), RunPipeline(400));
}

//  ( scent tracking): SCENT HUNTING. With wind advection ON, a predator
// whose sensory genome CANNOT directly perceive distant prey (short vision +
// hearing) closes on it along the wind-advected prey-scent gradient — the
// vertebrate stigmergy loop on the brain path. Deterministic run==replay.
TEST(ScentHunt, PredatorTracksPreyUpwind) {
    auto run = [] {
        constexpr float kCell = 2.0f;
        constexpr int kGrid = 96;
        luminumbra::ai::ScentField field(kGrid, kGrid, /*channels=*/2);
        entt::registry r;
        const float ox = -kCell * kGrid * 0.5f, oz = -kCell * kGrid * 0.5f;
        // Stationary prey UPWIND, depositing scent on channel 0.
        const auto prey = r.create();
        r.emplace<Comp::TransformComponent>(prey).position = Luminumbra::Vec3(40.0f, 0.0f, 0.0f);
        auto& pc = r.emplace<Comp::CreatureComponent>(prey);
        pc.is_predator = false;
        pc.move_speed = 0.0f;
        pc.hunger = 0.0f;
        auto& sn = r.emplace<Comp::SensableComponent>(prey);
        sn.scent_channel = 0;
        sn.scent_deposit = 1.0f;
        // A hungry predator DOWNWIND with senses too short to see/hear the prey.
        const auto pred = r.create();
        r.emplace<Comp::TransformComponent>(pred).position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
        auto& dc = r.emplace<Comp::CreatureComponent>(pred);
        dc.is_predator = true;
        dc.hunger = 0.7f;
        dc.move_speed = 3.0f;
        auto& gn = r.emplace<Comp::CreatureGenomeComponent>(pred);
        gn.vision_range = 10.0f; // prey at 40 m: invisible
        gn.hearing_range = 8.0f; // inaudible
        constexpr float dt = 1.0f / 30.0f;
        float dist_start = 40.0f, dist_end = 40.0f;
        for (int t = 0; t < 600; ++t) {
            luminumbra::ai::RunScentDepositOnTick(r, field, ox, oz, kCell);
            // Wind blows the scent FROM the prey TOWARD the predator (-X), laying
            // the advected trail the predator climbs.
            field.Step(/*diffusion=*/0.10,
                       /*iters=*/1,
                       /*evaporation=*/0.01,
                       /*wind_cx=*/-0.4,
                       /*wind_cz=*/0.0);
            luminumbra::ai::RunCreatureBrainSystemOnTick(r, dt, {}, &field, ox, oz, kCell);
            const auto& tp = r.get<Comp::TransformComponent>(pred).position;
            const auto& yp = r.get<Comp::TransformComponent>(prey).position;
            const float dx = yp.x - tp.x, dz = yp.z - tp.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (t == 0)
                dist_start = d;
            dist_end = d;
        }
        return std::pair<float, float>(dist_start, dist_end);
    };
    const auto [d0, d1] = run();
    EXPECT_LT(d1, d0 - 10.0f)
        << "the predator did not close on the prey along the scent gradient (start " << d0
        << " m, end " << d1 << " m)";
    // run==replay: the scent-tracking loop is byte-exact.
    const auto again = run();
    EXPECT_EQ(d0, again.first);
    EXPECT_EQ(d1, again.second);
}

//  ( starvation handling): starvation DEGRADES, then KILLS, then the carcass
// DECAYS. A lone mortal creature with no food: as hunger crosses the 0.85
// degradation band its effective speed (|wish|) measurably drops below cruise;
// at hunger 1.0 the LifespanSystem marks it dead + eaten; the DecayComponent
// then progresses the carcass. Deterministic run==replay. (Subject is a lone
// predator — see the fixture comment for why prey can't sample this since the
//  graze arbiter.)
TEST(EcologyPipeline, StarvationDegradesThenKills) {
    auto run = [] {
        entt::registry r;
        // Subject: a LONE PREDATOR with no prey. Why a predator and not the
        // herbivore this test originally used? Since the  arbiter (/
        // 05), a starving PREY correctly Grazes ambient plants (food_proximity is a
        // hardcoded 0.6 for prey) rather than moving — a zero-velocity action, so the
        // starving-band |wish| sample was vacuous. A predator with no prey has
        // food_proximity 0, so Hunt/Graze score ~0 and Wander (the constant baseline)
        // wins at every hunger — giving a sustained, measurable |wish| that isolates
        // the pure  locomotion degradation (starve_degrade), which is
        // exactly what this test verifies. LifespanSystem's starvation death (hunger
        // >= 1.0 -> dead + eaten) is role-agnostic, so the die+carcass arc is intact.
        const auto e = r.create();
        r.emplace<Comp::TransformComponent>(e).position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = true;
        cr.hunger = 0.80f; // just below the degradation band
        cr.move_speed = 3.0f;
        cr.stamina = 1.0f; // full (see the zero move-drain tuning below)
        r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 1000000u; // starvation, not old age
        r.emplace<Comp::DecayComponent>(e).decay_duration = 60u;

        // Zero stamina move-drain so the wanderer never tires into Rest (a
        // zero-velocity action) before it starves — the test measures starvation
        // degradation of speed, not stamina dynamics. Every other tuning is default.
        luminumbra::ai::EcologyTuning tuning;
        tuning.stamina_move_drain = 0.0f;

        constexpr float dt = 1.0f / 30.0f;
        float healthy_speed = -1.0f, starving_speed = -1.0f;
        std::uint64_t death_tick = 0;
        for (std::uint64_t t = 0; t < 4000 && death_tick == 0; ++t) {
            luminumbra::ai::RunCreatureBrainSystemOnTick(r, dt, tuning);
            luminumbra::ai::RunLifespanOnTick(r, t);
            luminumbra::ai::RunDecompositionOnTick(r, t);
            const auto& c = r.get<Comp::CreatureComponent>(e);
            const float sp = std::sqrt(c.wish_x * c.wish_x + c.wish_z * c.wish_z);
            if (healthy_speed < 0.0f && c.hunger < 0.85f && sp > 0.1f)
                healthy_speed = sp;
            if (c.hunger > 0.97f && c.hunger < 1.0f && sp > 0.1f)
                starving_speed = sp;
            if (r.get<Comp::MortalComponent>(e).dead != 0)
                death_tick = t;
        }
        return std::make_tuple(
            healthy_speed, starving_speed, death_tick, r.get<Comp::CreatureComponent>(e).eaten);
    };
    const auto [healthy, starving, death_tick, eaten] = run();
    ASSERT_GT(healthy, 0.0f) << "the creature never moved while healthy (vacuous)";
    ASSERT_GT(starving, 0.0f) << "no starving-band movement sample captured (vacuous)";
    EXPECT_LT(starving, healthy * 0.75f)
        << "sustained starvation did not degrade effective speed (healthy=" << healthy
        << " starving=" << starving << ")";
    EXPECT_GT(death_tick, 0u) << "max hunger never killed the creature";
    EXPECT_TRUE(eaten) << "death did not mark the carcass inert (the carcass seam)";
    // run==replay for the whole degrade->die arc.
    const auto again = run();
    EXPECT_EQ(healthy, std::get<0>(again));
    EXPECT_EQ(starving, std::get<1>(again));
    EXPECT_EQ(death_tick, std::get<2>(again));
}

//  ( live feeding): the LIVE feeding loop. Real plants (the
// MakePlantFromSpecies wiring point) now carry GrazeableComponent, so a hungry
// herd standing on a patch DRAWS DOWN its standing biomass and sates its hunger
// through kFeedPerGraze — the previously wired-but-dormant WildlifeFoliageSystem
// running against real participants. Deterministic run==replay.
TEST(EcologyPipeline, GrazeDepletesLiveBiomass) {
    auto build_and_run = [](int ticks) {
        entt::registry r;
        // Real plants via the single sim wiring point (samples a genome, stamps
        // lifecycle + pollination + soil + NOW grazeable).
        luminumbra::foliage::SpeciesTemplate tmpl;
        tmpl.id = "meadow_grass";
        tmpl.gene_lo.fill(0.30f);
        tmpl.gene_hi.fill(0.70f);
        auto rng = luminumbra::core::DeterministicRng::seeded(101u, 2024u, 3u);
        std::vector<entt::entity> plants;
        for (int i = 0; i < 6; ++i) {
            plants.push_back(luminumbra::foliage::MakePlantFromSpecies(
                r, Luminumbra::Vec3(static_cast<float>(i) * 1.5f, 0.0f, 0.0f), tmpl, rng, 0u));
        }
        // The wiring-point contract: a real plant IS grazeable.
        for (auto p : plants) {
            EXPECT_TRUE(r.all_of<Comp::GrazeableComponent>(p))
                << "MakePlantFromSpecies must emplace GrazeableComponent ()";
        }
        // A hungry herd standing right on the patch (no movement needed).
        std::vector<entt::entity> herd;
        for (int i = 0; i < 4; ++i) {
            const auto e = r.create();
            auto& tf = r.emplace<Comp::TransformComponent>(e);
            tf.position = Luminumbra::Vec3(static_cast<float>(i) * 1.5f, 0.0f, 0.5f);
            auto& cr = r.emplace<Comp::CreatureComponent>(e);
            cr.is_predator = false;
            cr.hunger = 0.85f;
            herd.push_back(e);
        }
        for (int t = 0; t < ticks; ++t) {
            luminumbra::ai::RunWildlifeFoliageOnTick(r, static_cast<std::uint64_t>(t));
        }
        float biomass = 0.0f, hunger = 0.0f;
        for (auto p : plants)
            biomass += r.get<Comp::GrazeableComponent>(p).biomass;
        for (auto e : herd)
            hunger += r.get<Comp::CreatureComponent>(e).hunger;
        return std::pair<float, float>(biomass, hunger);
    };
    const auto [biomass_after, hunger_after] = build_and_run(120);
    // 6 full plants started at 6.0 total; 4 hungry grazers started at 3.4 total.
    EXPECT_LT(biomass_after, 6.0f - 0.5f)
        << "the herd did not measurably draw down the standing biomass";
    EXPECT_LT(hunger_after, 3.4f - 0.5f)
        << "grazing did not sate the herd's hunger (kFeedPerGraze not applied)";
    EXPECT_GT(biomass_after, 0.0f);
    // Determinism: the whole feeding loop is byte-exact run == replay.
    const auto again = build_and_run(120);
    EXPECT_EQ(biomass_after, again.first);
    EXPECT_EQ(hunger_after, again.second);
}

// Sanity: the pipeline is actually DOING something (so the determinism check isn't trivially
// satisfied by a frozen world) -- the population changes (births and/or deaths) over time.
TEST(EcologyPipeline, PopulationEvolves) {
    entt::registry r;
    Populate(r);
    const std::size_t start = r.view<Comp::CreatureComponent>().size();
    for (int t = 0; t < 400; ++t)
        EcologyTick(r, static_cast<std::uint64_t>(t));
    const std::size_t end = r.view<Comp::CreatureComponent>().size();
    EXPECT_NE(start, end) << "the ecology should birth and/or cull creatures over 400 ticks";
}

// An empty world ticks to a no-op (the whole stack is gated).
TEST(EcologyPipeline, EmptyRosterNoOp) {
    entt::registry r;
    for (int t = 0; t < 20; ++t)
        EcologyTick(r, static_cast<std::uint64_t>(t));
    EXPECT_EQ(r.view<Comp::CreatureComponent>().size(), 0u);
}

} // namespace
