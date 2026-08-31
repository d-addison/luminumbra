// sim.decomposition: DEAD creatures DECAY and RELEASE NUTRIENTS, closing the
// death -> soil cycle. A dead entity advances its decay clock and accrues nutrient (fixed-point
// milli-units) monotonically up to a full load reached exactly at decay_duration, where it
// latches fully_decomposed and stops. A LIVE entity does not decay. Deterministic (id-ordered,
// pure-integer ticks, NO rng), gated by the DecayComponent opt-in.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/DecompositionSystem.h"
#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/MortalComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::RunDecompositionOnTick;
using luminumbra::ai::DecompositionStats;
using luminumbra::ai::kNutrientPerCorpseMilli;

// Spawn a DEAD mortal entity opted into decomposition (no creature).
entt::entity spawnDeadDecayer(entt::registry& r, std::uint32_t duration) {
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.dead = 1;  // marked dead -> eligible to decay.
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = duration;
    return e;
}

// Spawn a LIVE entity opted into decomposition — must never decay.
entt::entity spawnLiveDecayer(entt::registry& r, std::uint32_t duration) {
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.dead = 0;  // alive.
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = duration;
    return e;
}

// Empty roster: the tick is a pure no-op.
TEST(Decomposition, EmptyRosterNoOp) {
    entt::registry r;
    const DecompositionStats s = RunDecompositionOnTick(r, 1);
    EXPECT_EQ(s.decaying, 0);
    EXPECT_EQ(s.released_total, 0u);
    EXPECT_EQ(s.finished, 0);
}

// A registry with only LIVE decayers is a no-op (gate by death, not by component presence).
TEST(Decomposition, LiveEntityDoesNotDecay) {
    entt::registry r;
    const entt::entity e = spawnLiveDecayer(r, /*duration*/ 100);
    for (int i = 0; i < 50; ++i) {
        const DecompositionStats s = RunDecompositionOnTick(r, static_cast<std::uint64_t>(i));
        EXPECT_EQ(s.decaying, 0);
        EXPECT_EQ(s.released_total, 0u);
    }
    const auto& dc = r.get<Comp::DecayComponent>(e);
    EXPECT_EQ(dc.decay_ticks, 0u);
    EXPECT_EQ(dc.nutrient_released_milli, 0u);
    EXPECT_EQ(dc.fully_decomposed, 0u);
}

// A dead entity decays each tick and accrues nutrient MONOTONICALLY.
TEST(Decomposition, DeadEntityDecaysAndAccruesMonotonically) {
    entt::registry r;
    const entt::entity e = spawnDeadDecayer(r, /*duration*/ 100);
    std::uint32_t prevTicks = 0;
    std::uint16_t prevNut = 0;
    for (int i = 0; i < 50; ++i) {
        RunDecompositionOnTick(r, static_cast<std::uint64_t>(i));
        const auto& dc = r.get<Comp::DecayComponent>(e);
        EXPECT_GT(dc.decay_ticks, prevTicks) << "decay clock must advance while dead";
        EXPECT_GE(dc.nutrient_released_milli, prevNut) << "nutrient must never decrease";
        prevTicks = dc.decay_ticks;
        prevNut = dc.nutrient_released_milli;
    }
    const auto& dc = r.get<Comp::DecayComponent>(e);
    EXPECT_EQ(dc.decay_ticks, 50u);
    EXPECT_GT(dc.nutrient_released_milli, 0u);
    EXPECT_EQ(dc.fully_decomposed, 0u) << "not yet at duration";
}

// The per-tick released_total stat sums to the full corpse load over a complete decompose.
TEST(Decomposition, ReleasedTotalSumsToFullLoad) {
    entt::registry r;
    const entt::entity e = spawnDeadDecayer(r, /*duration*/ 30);
    std::uint32_t summed = 0;
    for (int i = 0; i < 30; ++i) {
        const DecompositionStats s = RunDecompositionOnTick(r, static_cast<std::uint64_t>(i));
        summed += s.released_total;
    }
    const auto& dc = r.get<Comp::DecayComponent>(e);
    EXPECT_EQ(dc.nutrient_released_milli, kNutrientPerCorpseMilli);
    EXPECT_EQ(summed, kNutrientPerCorpseMilli)
        << "sum of per-tick releases equals the cumulative total";
}

// Full decomposition sets the flag exactly at decay_duration and then stops (latched).
TEST(Decomposition, FullDecompositionLatchesAndStops) {
    entt::registry r;
    const entt::entity e = spawnDeadDecayer(r, /*duration*/ 10);
    // Ticks 1..9: still decomposing.
    for (int i = 0; i < 9; ++i) {
        const DecompositionStats s = RunDecompositionOnTick(r, static_cast<std::uint64_t>(i));
        EXPECT_EQ(s.finished, 0);
        EXPECT_EQ(r.get<Comp::DecayComponent>(e).fully_decomposed, 0u);
    }
    // Tick 10: reaches duration -> finishes.
    const DecompositionStats fin = RunDecompositionOnTick(r, 9);
    EXPECT_EQ(fin.finished, 1);
    const auto& dc = r.get<Comp::DecayComponent>(e);
    EXPECT_EQ(dc.decay_ticks, 10u);
    EXPECT_EQ(dc.fully_decomposed, 1u);
    EXPECT_EQ(dc.nutrient_released_milli, kNutrientPerCorpseMilli);

    // After finishing it is inert: clock + nutrient stable, no further release.
    const DecompositionStats after = RunDecompositionOnTick(r, 10);
    EXPECT_EQ(after.decaying, 0);
    EXPECT_EQ(after.released_total, 0u);
    EXPECT_EQ(after.finished, 0);
    EXPECT_EQ(r.get<Comp::DecayComponent>(e).decay_ticks, 10u);
    EXPECT_EQ(r.get<Comp::DecayComponent>(e).nutrient_released_milli, kNutrientPerCorpseMilli);
}

// The CreatureComponent.eaten carcass seam also counts as dead (no MortalComponent needed).
TEST(Decomposition, EatenCreatureDecays) {
    entt::registry r;
    auto e = r.create();
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.eaten = true;  // carcass.
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = 20;

    const DecompositionStats s = RunDecompositionOnTick(r, 1);
    EXPECT_EQ(s.decaying, 1);
    EXPECT_EQ(r.get<Comp::DecayComponent>(e).decay_ticks, 1u);
    EXPECT_GT(r.get<Comp::DecayComponent>(e).nutrient_released_milli, 0u);
}

// run == replay: identical setup + ticks -> identical clocks, nutrient, and death flags.
TEST(Decomposition, Deterministic) {
    auto run = [] {
        entt::registry r;
        spawnDeadDecayer(r, 7);
        spawnDeadDecayer(r, 30);
        spawnLiveDecayer(r, 30);  // must stay at 0
        {
            auto e = r.create();
            auto& cr = r.emplace<Comp::CreatureComponent>(e);
            cr.eaten = true;
            auto& dc = r.emplace<Comp::DecayComponent>(e);
            dc.decay_duration = 15;
        }
        for (std::uint64_t t = 0; t < 40; ++t) RunDecompositionOnTick(r, t);
        std::vector<std::uint32_t> out;
        for (auto e : r.view<Comp::DecayComponent>()) {
            const auto& dc = r.get<Comp::DecayComponent>(e);
            out.push_back(dc.decay_ticks);
            out.push_back(dc.nutrient_released_milli);
            out.push_back(dc.fully_decomposed);
        }
        return out;
    };
    EXPECT_EQ(run(), run());
}

}  // namespace
