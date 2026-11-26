// sim.fire — deterministic emergent FIRE SPREAD. Deterministic (id-ordered, two-phase
// snapshot/apply; pure threshold ignition + libm-free Sqrt; NO RNG / wall-clock). Gated by
// CombustibleComponent: a registry with none does nothing (canonical NetworkStateHash baseline
// byte-identical). Tests derive from the acceptance criteria: spread to a dry neighbor, a
// wet/fuel-poor neighbor resists, burn-down -> Burnt stops spreading, wind biases downwind,
// run==replay, and an EMPTY roster no-op.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "ai/CreatureTickAdmission.h"
#include "components/CombustionComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "systems/FireSpreadSystem.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::sim::RunFireSpreadOnTick;

// Spawn a combustible at (x,z) with given state / moisture / fuel. moisture_milli and
// fuel_milli are 0..1000. radius is the ignition reach (m).
entt::entity spawnFuel(entt::registry& r,
                       float x,
                       float z,
                       Comp::BurnState state,
                       std::uint16_t moisture_milli = 0,
                       std::uint16_t fuel_milli = 1000,
                       float radius = 2.0f,
                       std::uint32_t burn_ticks = 0) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& cb = r.emplace<Comp::CombustibleComponent>(e);
    cb.set_state(state);
    cb.moisture_milli = moisture_milli;
    cb.fuel_milli = fuel_milli;
    cb.ignition_radius = radius;
    cb.burn_ticks_remaining = burn_ticks;
    return e;
}

Comp::BurnState stateOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::CombustibleComponent>(e).state();
}

int combustibleCount(entt::registry& r) {
    int n = 0;
    for (auto e : r.view<Comp::CombustibleComponent>()) {
        (void)e;
        ++n;
    }
    return n;
}

// ---- gating: empty / non-participant rosters ----

// No combustibles at all -> the system does nothing (byte-identical roster).
TEST(FireSpread, EmptyRosterNoOp) {
    entt::registry r;
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s.considered, 0);
    EXPECT_EQ(s.ignited, 0);
    EXPECT_EQ(s.burnt_out, 0);
    EXPECT_EQ(combustibleCount(r), 0);
}

// An entity with a TransformComponent but NO CombustibleComponent is never considered.
TEST(FireSpread, NonCombustibleIgnored) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s.considered, 0);
    EXPECT_EQ(s.ignited, 0);
}

// ---- core spread ----

// A burning cell ignites a DRY adjacent combustible.
TEST(FireSpread, SpreadsToDryNeighbor) {
    entt::registry r;
    spawnFuel(r,
              0.0f,
              0.0f,
              Comp::BurnState::Burning,
              /*moist*/ 0,
              /*fuel*/ 1000,
              /*radius*/ 2.0f,
              /*burn_ticks*/ 90);
    auto dry = spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, /*moist*/ 0, /*fuel*/ 1000);
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s.ignited, 1);
    EXPECT_EQ(stateOf(r, dry), Comp::BurnState::Burning);
}

// A WET neighbor (high moisture) resists ignition at the same distance.
TEST(FireSpread, WetNeighborResists) {
    entt::registry r;
    spawnFuel(r, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, 2.0f, 90);
    auto wet = spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, /*moist*/ 1000, /*fuel*/ 1000);
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s.ignited, 0);
    EXPECT_EQ(stateOf(r, wet), Comp::BurnState::Unburnt);
}

// A FUEL-EXHAUSTED neighbor (no fuel) cannot catch even if dry and close.
TEST(FireSpread, NoFuelNeighborCannotIgnite) {
    entt::registry r;
    spawnFuel(r, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, 2.0f, 90);
    auto spent = spawnFuel(r, 0.5f, 0.0f, Comp::BurnState::Unburnt, /*moist*/ 0, /*fuel*/ 0);
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s.ignited, 0);
    EXPECT_EQ(stateOf(r, spent), Comp::BurnState::Unburnt);
}

// A neighbor OUTSIDE the burning cell's ignition radius does not catch.
TEST(FireSpread, OutsideRadiusDoesNotIgnite) {
    entt::registry r;
    spawnFuel(r, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, /*radius*/ 2.0f, 90);
    auto far = spawnFuel(r, 5.0f, 0.0f, Comp::BurnState::Unburnt, 0, 1000);
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s.ignited, 0);
    EXPECT_EQ(stateOf(r, far), Comp::BurnState::Unburnt);
}

// ---- burn-down lifecycle ----

// A Burning cell counts down burn_ticks and becomes Burnt; once Burnt it stops spreading.
TEST(FireSpread, BurnsDownToBurntAndStopsSpreading) {
    entt::registry r;
    // Burning cell with just 1 tick of fuel-time left, plus a dry neighbor in range.
    auto src = spawnFuel(r,
                         0.0f,
                         0.0f,
                         Comp::BurnState::Burning,
                         0,
                         1000,
                         2.0f,
                         /*burn_ticks*/ 1);
    auto dry = spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, 0, 1000);

    // Tick 1: src is still Burning at snapshot time -> it ignites dry AND burns down to Burnt.
    const auto s1 = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stateOf(r, src), Comp::BurnState::Burnt);
    EXPECT_EQ(s1.burnt_out, 1);
    EXPECT_EQ(stateOf(r, dry), Comp::BurnState::Burning); // it caught this tick

    // Make a SECOND unburnt neighbor of the now-Burnt src; a Burnt cell must not ignite it.
    auto other = spawnFuel(r, -1.0f, 0.0f, Comp::BurnState::Unburnt, 0, 1000);
    // Disable the freshly-lit `dry` so only the Burnt src could (wrongly) ignite `other`.
    r.get<Comp::CombustibleComponent>(dry).set_state(Comp::BurnState::Burnt);
    const auto s2 = RunFireSpreadOnTick(r, /*tick*/ 2);
    EXPECT_EQ(s2.ignited, 0) << "a Burnt cell must not spread";
    EXPECT_EQ(stateOf(r, other), Comp::BurnState::Unburnt);
}

// A single-tick ignition does NOT chain in the same tick (two-phase): a cell ignited this
// tick cannot itself ignite a third cell until the NEXT tick.
TEST(FireSpread, NoChainWithinSameTick) {
    entt::registry r;
    // Line: burning(0) -- unburnt A(1) -- unburnt B(2). Radius 1.5 so each only reaches its
    // immediate neighbor (dist 1), not 2 cells away (dist 2).
    spawnFuel(r, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, /*radius*/ 1.5f, 90);
    auto a = spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, 0, 1000, /*radius*/ 1.5f);
    auto b = spawnFuel(r, 2.0f, 0.0f, Comp::BurnState::Unburnt, 0, 1000, /*radius*/ 1.5f);
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stateOf(r, a), Comp::BurnState::Burning); // caught from source
    EXPECT_EQ(stateOf(r, b), Comp::BurnState::Unburnt); // A only just lit -> no same-tick chain
    EXPECT_EQ(s.ignited, 1);
}

// ---- wind bias ----

// WIND biases spread downwind: with wind blowing toward +x, a moderately-moist downwind
// neighbor ignites while the symmetric upwind neighbor does not.
TEST(FireSpread, WindBiasesSpreadDownwind) {
    entt::registry r;
    // Source at origin. Two identical neighbors at +/- x, dist 1, radius 2 (falloff 0.5).
    // Pick moisture so resistance sits between the upwind and downwind delivered heat.
    //   base heat = 1.0 * 0.5 = 0.5
    //   wind term = 0.6 * (dir.wind) * 0.5 ; wind=(2,0): downwind +0.6, upwind -0.6
    //   => downwind heat = 1.1, upwind heat = -0.1 -> 0 (clamped)
    // resistance(moist) = 0.25 + moist*1.5 - fuel*0.20. With fuel=1.0, moist=0.5:
    //   resistance = 0.25 + 0.75 - 0.20 = 0.80 -> downwind(1.1) ignites, upwind(0) does not.
    spawnFuel(r, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, /*radius*/ 2.0f, 90);
    auto downwind = spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, /*moist*/ 500, 1000);
    auto upwind = spawnFuel(r, -1.0f, 0.0f, Comp::BurnState::Unburnt, /*moist*/ 500, 1000);

    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1, /*wind*/ glm::vec2(2.0f, 0.0f));
    EXPECT_EQ(stateOf(r, downwind), Comp::BurnState::Burning) << "downwind neighbor should catch";
    EXPECT_EQ(stateOf(r, upwind), Comp::BurnState::Unburnt) << "upwind neighbor should resist";
    EXPECT_EQ(s.ignited, 1);
}

// ---- ignition source ----

// An explicit IgnitionSourceComponent lights a nearby dry combustible even with no burning cell.
TEST(FireSpread, IgnitionSourceStartsFire) {
    entt::registry r;
    auto torch = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(torch);
    tf.position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
    auto& src = r.emplace<Comp::IgnitionSourceComponent>(torch);
    src.radius = 2.0f;
    src.intensity = 1.0f;
    auto dry = spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, /*moist*/ 0, 1000);
    const auto s = RunFireSpreadOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s.ignited, 1);
    EXPECT_EQ(stateOf(r, dry), Comp::BurnState::Burning);
}

TEST(FireSpread, ExternalWeatherStrikeStartsFire) {
    entt::registry r;
    auto dry = spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, /*moist*/ 0, 1000);
    const std::array<luminumbra::sim::FireIgnitionSource, 1> strikes{{
        {glm::vec2(0.0f, 0.0f), 2.0f, 1.0f},
    }};
    const auto stats = RunFireSpreadOnTick(r, /*tick*/ 1, glm::vec2(0.0f), strikes);
    EXPECT_EQ(stats.ignited, 1);
    EXPECT_EQ(stateOf(r, dry), Comp::BurnState::Burning);
}

// ---- determinism: run == replay ----

// Identical setup + tick + wind -> identical resulting states across two independent runs.
TEST(FireSpread, RunEqualsReplay) {
    auto run = [] {
        entt::registry r;
        // A small field of mixed-moisture fuel with one initial fire + a wind.
        spawnFuel(r, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, 2.5f, 90);
        spawnFuel(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, 100, 1000, 2.5f);
        spawnFuel(r, 2.0f, 0.0f, Comp::BurnState::Unburnt, 300, 900, 2.5f);
        spawnFuel(r, 1.0f, 1.0f, Comp::BurnState::Unburnt, 600, 800, 2.5f);
        spawnFuel(r, -1.0f, 0.0f, Comp::BurnState::Unburnt, 200, 1000, 2.5f);
        std::vector<int> trace;
        for (std::uint64_t t = 1; t <= 12; ++t) {
            RunFireSpreadOnTick(r, t, glm::vec2(1.0f, 0.5f));
            // Snapshot every combustible's (state, burn_ticks, fuel) in id order.
            std::vector<entt::entity> ents;
            for (auto e : r.view<Comp::CombustibleComponent>())
                ents.push_back(e);
            std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
                return entt::to_integral(a) < entt::to_integral(b);
            });
            for (auto e : ents) {
                const auto& cb = r.get<Comp::CombustibleComponent>(e);
                trace.push_back(static_cast<int>(cb.burn_state));
                trace.push_back(static_cast<int>(cb.burn_ticks_remaining));
                trace.push_back(static_cast<int>(cb.fuel_milli));
            }
        }
        return trace;
    };
    EXPECT_EQ(run(), run());
}

TEST(FireSpread, CandidateRefusesBrokenIdentityInEitherSourceListBeforeBurnDown) {
    for (const bool explicit_source : {false, true}) {
        for (const auto id :
             {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 63, ~std::uint64_t{0}}) {
            entt::registry registry;
            const auto first =
                spawnFuel(registry, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, 2.0f, 1);
            registry.emplace<Comp::SimulationIdentityComponent>(first, std::uint64_t{1});
            entt::entity second;
            if (explicit_source) {
                registry.emplace<Comp::IgnitionSourceComponent>(first);
                second = registry.create();
                registry.emplace<Comp::TransformComponent>(second);
                registry.emplace<Comp::IgnitionSourceComponent>(second);
            } else {
                second = spawnFuel(registry, 1.0f, 0.0f, Comp::BurnState::Unburnt);
            }
            EXPECT_THROW(RunFireSpreadOnTick<true>(registry, 1), std::logic_error);
            registry.emplace<Comp::SimulationIdentityComponent>(second, id);
            EXPECT_THROW(RunFireSpreadOnTick<true>(registry, 1), std::logic_error);
            EXPECT_EQ(stateOf(registry, first), Comp::BurnState::Burning);
            EXPECT_EQ(registry.get<Comp::CombustibleComponent>(first).burn_ticks_remaining, 1u);
        }
    }
}

TEST(FireSpread, CandidateEmptyRosterAndExternalSourcesRetainTheirSemantics) {
    entt::registry registry;
    EXPECT_EQ(RunFireSpreadOnTick<true>(registry, 1).ordering_visits, 0u);
    const auto dry = spawnFuel(registry, 1.0f, 0.0f, Comp::BurnState::Unburnt);
    registry.emplace<Comp::SimulationIdentityComponent>(dry, std::uint64_t{1});
    const std::array<luminumbra::sim::FireIgnitionSource, 1> strikes{{
        {glm::vec2(0.0f), 2.0f, 1.0f},
    }};
    EXPECT_EQ(RunFireSpreadOnTick<true>(registry, 1, glm::vec2(0.0f), strikes).ignited, 1);
    EXPECT_EQ(stateOf(registry, dry), Comp::BurnState::Burning);
}

TEST(FireSpread, CandidateChecksIdentityAcrossListsAndAllowsOneEntityWithBothRoles) {
    entt::registry registry;
    const auto burning =
        spawnFuel(registry, 0.0f, 0.0f, Comp::BurnState::Burning, 0, 1000, 2.0f, 1);
    registry.emplace<Comp::SimulationIdentityComponent>(burning, std::uint64_t{1});
    const auto torch = registry.create();
    registry.emplace<Comp::TransformComponent>(torch);
    registry.emplace<Comp::IgnitionSourceComponent>(torch);
    registry.emplace<Comp::SimulationIdentityComponent>(torch, std::uint64_t{1});
    EXPECT_THROW(RunFireSpreadOnTick<true>(registry, 1), std::logic_error);
    EXPECT_EQ(stateOf(registry, burning), Comp::BurnState::Burning);
    EXPECT_EQ(registry.get<Comp::CombustibleComponent>(burning).burn_ticks_remaining, 1u);
    EXPECT_EQ(registry.get<Comp::CombustibleComponent>(burning).fuel_milli, 1000u);

    registry.destroy(torch);
    registry.emplace<Comp::IgnitionSourceComponent>(burning);
    const auto dry = spawnFuel(registry, 1.0f, 0.0f, Comp::BurnState::Unburnt);
    registry.emplace<Comp::SimulationIdentityComponent>(dry, std::uint64_t{2});
    const auto stats = RunFireSpreadOnTick<true>(registry, 1);
    EXPECT_EQ(stats.ignited, 1);
    EXPECT_EQ(stats.burnt_out, 1);
    EXPECT_EQ(stats.ordering_visits, (18u * 2u - 1u) + (18u - 1u) + 1u);
    EXPECT_EQ(stateOf(registry, dry), Comp::BurnState::Burning);
    EXPECT_EQ(stateOf(registry, burning), Comp::BurnState::Burnt);
}

// Compare the elapsed candidate against repeated legacy snapshot/apply steps,
// including delayed participation, frozen sources/targets and transient strikes.
TEST(FireSpread, RegionalElapsedMatchesApprovedReference) {
    namespace W = ::Luminumbra::world;
    for (const bool stable : {false, true}) {
        for (const std::uint32_t steps : {0u, 1u, 8u, 36000u, 36001u, 65536u}) {
            SCOPED_TRACE(steps);
            entt::registry actual, reference;
            auto seed = [](entt::registry& registry) {
                std::vector<entt::entity> entities;
                entities.push_back(
                    spawnFuel(registry, 0, 0, Comp::BurnState::Burning, 0, 1000, 2, 2));
                entities.push_back(spawnFuel(registry, 1.5f, 0, Comp::BurnState::Unburnt));
                entities.push_back(spawnFuel(registry, 3, 0, Comp::BurnState::Unburnt));
                entities.push_back(spawnFuel(registry, 4.5f, 0, Comp::BurnState::Unburnt));
                entities.push_back(
                    spawnFuel(registry, 0, 0, Comp::BurnState::Burning, 0, 1000, 20, 7));
                entities.push_back(spawnFuel(registry, 20, 0, Comp::BurnState::Unburnt));
                entities.push_back(spawnFuel(registry, 30, 0, Comp::BurnState::Unburnt));
                entities.push_back(spawnFuel(registry, 40, 0, Comp::BurnState::Unburnt));
                entities.push_back(spawnFuel(registry,
                                             1,
                                             0,
                                             Comp::BurnState::Unburnt,
                                             1000)); // wet resistance
                entities.push_back(
                    spawnFuel(registry, 1, 0, Comp::BurnState::Unburnt, 0, 0)); // exhausted fuel
                auto& source = registry.emplace<Comp::IgnitionSourceComponent>(entities[6]);
                source.radius = 1;
                source.intensity = 1;
                registry.emplace<Comp::IgnitionSourceComponent>(entities[4]);
                registry.emplace<Comp::IgnitionSourceComponent>(entities[7]);
                entities.push_back(spawnFuel(registry, 38.5f, 0, Comp::BurnState::Unburnt));
                for (std::size_t index = 0; index < entities.size(); ++index) {
                    auto& owner = registry.emplace<Comp::PlantRegionComponent>(entities[index]);
                    owner.owner_x = index == 3 || index == 4 ? 1 : (index == 7 ? 2 : 0);
                    registry.emplace<Comp::SimulationIdentityComponent>(
                        entities[index], static_cast<std::uint64_t>(index + 1));
                }
                return entities;
            };
            const auto entities = seed(actual);
            const auto expected = seed(reference);
            const std::array<W::RegionKey, 2> due{{{0, 0}, {2, 0}}};
            const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, steps},
                                                                {{2, 0}, std::min(steps, 1u)}};
            const W::RegionConsumerGate gate(actual, due, W::RegionElapsedTicks(windows));
            const std::array<luminumbra::sim::FireIgnitionSource, 1> strikes{
                luminumbra::sim::FireIgnitionSource{{20, 0}, 1, 1}};
            const auto stats =
                stable ? RunFireSpreadOnTick<true, true>(actual, 65536, {}, strikes, gate)
                       : RunFireSpreadOnTick<false, true>(actual, 65536, {}, strikes, gate);
            int ignited = 0, burnt = 0;
            for (std::uint32_t step = 0; step < steps; ++step) {
                const std::span<const W::RegionKey> admitted(due.data(), step + 1 == steps ? 2 : 1);
                const W::RegionConsumerGate single(reference, admitted);
                const auto events =
                    step + 1 == steps
                        ? std::span<const luminumbra::sim::FireIgnitionSource>(strikes)
                        : std::span<const luminumbra::sim::FireIgnitionSource>();
                const auto result =
                    RunFireSpreadOnTick(reference, 65537 - steps + step, {}, events, single);
                ignited += result.ignited;
                burnt += result.burnt_out;
            }
            EXPECT_EQ(stats.ignited, ignited);
            EXPECT_EQ(stats.burnt_out, burnt);
            // Membership/identity preparation is once per participant, not per elapsed step.
            EXPECT_LE(gate.owner_visits(), 100u);
            EXPECT_LE(stats.ordering_visits, 20u * (entities.size() + 3u));
            for (std::size_t index = 0; index < entities.size(); ++index) {
                const auto& got = actual.get<Comp::CombustibleComponent>(entities[index]);
                const auto& want = reference.get<Comp::CombustibleComponent>(expected[index]);
                EXPECT_EQ(got.state(), want.state());
                EXPECT_EQ(got.burn_ticks_remaining, want.burn_ticks_remaining);
                EXPECT_EQ(got.fuel_milli, want.fuel_milli);
            }
            if (steps != 0) {
                EXPECT_EQ(actual.get<Comp::CombustibleComponent>(entities[5]).burn_ticks_remaining,
                          luminumbra::sim::kFireBurnTicks); // strike only at due time
            }
        }
    }
}

TEST(FireSpread, RegionalElapsedRefusesInvalidWindowsAndIdentityBeforeMutation) {
    namespace W = ::Luminumbra::world;
    for (const bool ignition : {false, true}) {
        entt::registry registry;
        const auto fuel = spawnFuel(registry, 0, 0, Comp::BurnState::Burning, 0, 1000, 2, 7);
        const auto other = spawnFuel(registry, 1, 0, Comp::BurnState::Unburnt);
        if (ignition) {
            registry.remove<Comp::CombustibleComponent>(other);
            registry.emplace<Comp::IgnitionSourceComponent>(other);
        }
        registry.emplace<Comp::PlantRegionComponent>(other).owner_x = 1;
        const std::array<W::RegionKey, 2> due{{{0, 0}, {1, 0}}};
        const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, 8}, {{1, 0}, 65537}};
        const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows));
        EXPECT_THROW((RunFireSpreadOnTick<false, true>(registry, 70000, {}, {}, gate)),
                     std::invalid_argument);
        EXPECT_EQ(registry.get<Comp::CombustibleComponent>(fuel).burn_ticks_remaining, 7u);
        EXPECT_THROW((RunFireSpreadOnTick<true, true>(registry, 70000, {}, {}, gate)),
                     std::logic_error); // absent stable identities
        EXPECT_EQ(registry.get<Comp::CombustibleComponent>(fuel).burn_ticks_remaining, 7u);
    }
    entt::registry empty;
    EXPECT_EQ((RunFireSpreadOnTick<true, true>(empty, 1).considered), 0);
}

TEST(FireSpread, RegionalElapsedRetainsCapturedCreatureSourcesAndTargets) {
    namespace W = ::Luminumbra::world;
    entt::registry registry;
    const auto source = spawnFuel(registry, 0, 0, Comp::BurnState::Burning, 0, 1000, 2, 10);
    const auto target = spawnFuel(registry, 0.5f, 0, Comp::BurnState::Unburnt);
    const auto frozen_target = spawnFuel(registry, 512, 0, Comp::BurnState::Unburnt);
    const auto frozen_source =
        spawnFuel(registry, 512, 0, Comp::BurnState::Burning, 0, 1000, 2, 10);
    const auto cold = spawnFuel(registry, 100, 0, Comp::BurnState::Unburnt);
    const auto torch_target = spawnFuel(registry, 520, 0, Comp::BurnState::Unburnt);
    const auto frozen_plant = spawnFuel(registry, 512.6f, 0, Comp::BurnState::Unburnt);
    registry.emplace<Comp::PlantRegionComponent>(torch_target);
    registry.emplace<Comp::PlantRegionComponent>(frozen_plant).owner_x = 1;
    const auto torch = registry.create();
    registry.emplace<Comp::TransformComponent>(torch);
    registry.emplace<Comp::IgnitionSourceComponent>(torch);
    const auto frozen_torch = registry.create();
    registry.emplace<Comp::TransformComponent>(frozen_torch).position.x = 512;
    registry.emplace<Comp::IgnitionSourceComponent>(frozen_torch);
    const auto stationary = registry.create();
    registry.emplace<Comp::TransformComponent>(stationary);
    registry.emplace<Comp::IgnitionSourceComponent>(stationary);
    for (auto entity : {source, target, frozen_target, frozen_source, torch, frozen_torch})
        registry.emplace<Comp::CreatureComponent>(entity);
    const std::array<W::RegionKey, 1> due{{{0, 0}}};
    const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, 3}};
    const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows));
    luminumbra::ai::CreatureTickAdmission captured;
    captured.Capture(registry, gate);
    registry.get<Comp::TransformComponent>(source).position.x = 512;
    registry.get<Comp::TransformComponent>(target).position.x = 512.5f;
    registry.get<Comp::TransformComponent>(frozen_target).position.x = 0;
    registry.get<Comp::TransformComponent>(frozen_source).position.x = 100;
    registry.get<Comp::TransformComponent>(torch).position.x = 520;
    registry.get<Comp::TransformComponent>(frozen_torch).position.x = 100;
    const auto admitted = [&](entt::entity entity) {
        if (registry.all_of<Comp::CreatureComponent>(entity)) {
            return W::RegionConsumerGate::Admission{captured.Allows(entity),
                                                    captured.ElapsedTicks(entity)};
        }
        return gate.AdmissionFor(entity);
    };
    const auto stats = RunFireSpreadOnTick<false, true>(registry, 3, {}, {}, gate, admitted);
    EXPECT_EQ(stats.ignited, 2);
    EXPECT_EQ(registry.get<Comp::CombustibleComponent>(source).burn_ticks_remaining, 7u);
    EXPECT_EQ(registry.get<Comp::CombustibleComponent>(target).burn_ticks_remaining, 88u);
    EXPECT_EQ(registry.get<Comp::CombustibleComponent>(torch_target).burn_ticks_remaining, 88u);
    EXPECT_EQ(registry.get<Comp::CombustibleComponent>(frozen_source).burn_ticks_remaining, 10u);
    EXPECT_EQ(stateOf(registry, frozen_target), Comp::BurnState::Unburnt);
    EXPECT_EQ(stateOf(registry, frozen_plant), Comp::BurnState::Unburnt);
    EXPECT_EQ(stateOf(registry, cold), Comp::BurnState::Unburnt);
}

} // namespace
