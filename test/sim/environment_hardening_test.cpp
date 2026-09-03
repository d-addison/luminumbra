// environment_hardening_test — ADVERSARIAL hardening of the ENVIRONMENT / FOLIAGE
// cluster. These tests probe what the per-system happy-path suites MISS:
//   * grid CONSERVATION + diffusion stability (no overshoot / negative / NaN)
//   * fixed-point milli-unit bounds
//   * fire downwind-bias SIGN + burn-out lifecycle
//   * wind-coupling sign across fire / pollination
//   * weather schedule validity + run==replay + forbidden-transition bounds
//   * determinism (run==replay byte-identical) + order-independence
//   * gating / empty-grid / empty-roster strict no-op for each system
//
// DETERMINISM: only the systems' own seeded/integer paths are used — NO wall-clock,
// NO std::random. run==replay assertions use exact float equality (the sim path is
// -ffp-contract=off bit-exact).
//
// Tests marked `// BUG:` document the EXPECTED-correct behaviour and may FAIL against
// the current code; that is intentional (the orchestrator fixes the system + keeps
// the test). Everything else should pass against the present headers.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "components/CombustionComponents.h"
#include "components/CoreComponents.h"
#include "components/DiseaseComponents.h"
#include "components/IrrigationComponents.h"
#include "components/PlantComponents.h"
#include "components/PollinationComponents.h"
#include "components/SoilComponents.h"
#include "systems/FireSpreadSystem.h"
#include "systems/IrrigationSystem.h"
#include "systems/PlantDiseaseSystem.h"
#include "systems/PollinationSystem.h"
#include "systems/SoilNutrientSystem.h"
#include "systems/WeatherEventSystem.h"

namespace {

namespace Comp = ::Luminumbra::Components;

// ===========================================================================
// helpers
// ===========================================================================

entt::entity makeCombustible(entt::registry& r,
                             float x,
                             float z,
                             Comp::BurnState state,
                             std::uint16_t fuel = 1000,
                             std::uint16_t moisture = 0,
                             float radius = 2.0f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = {x, 0.0f, z};
    auto& cb = r.emplace<Comp::CombustibleComponent>(e);
    cb.fuel_milli = fuel;
    cb.moisture_milli = moisture;
    cb.ignition_radius = radius;
    cb.set_state(state);
    if (state == Comp::BurnState::Burning)
        cb.burn_ticks_remaining = 5;
    return e;
}

entt::entity makeSoilPlant(
    entt::registry& r, float x, float z, std::uint8_t stage, std::uint16_t uptake = 1000) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = {x, 0.0f, z};
    r.emplace<Comp::PlantTag>(e);
    auto& f = r.emplace<Comp::SoilFeederComponent>(e);
    f.uptake = uptake;
    auto& g = r.emplace<Comp::PlantGrowthComponent>(e);
    g.stage = stage;
    return e;
}

entt::entity makeWaterSource(entt::registry& r, float x, float z, std::uint16_t output) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = {x, 0.0f, z};
    auto& w = r.emplace<Comp::WaterSourceComponent>(e);
    w.output = output;
    return e;
}

entt::entity makeDiseasePlant(
    entt::registry& r, float x, float z, Comp::PlantDiseaseState st, std::uint16_t resistance = 0) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = {x, 0.0f, z};
    r.emplace<Comp::PlantTag>(e);
    auto& h = r.emplace<Comp::PlantHealthComponent>(e);
    h.state = static_cast<std::uint8_t>(st);
    h.resistance = resistance;
    return e;
}

entt::entity
makePollinator(entt::registry& r, float x, float z, std::uint8_t stage, float gene0 = 0.5f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = {x, 0.0f, z};
    r.emplace<Comp::PollinationTag>(e);
    auto& gen = r.emplace<Comp::PlantGenomeComponent>(e);
    for (auto& v : gen.genes)
        v = gene0;
    auto& g = r.emplace<Comp::PlantGrowthComponent>(e);
    g.stage = stage;
    r.emplace<Comp::PollinationComponent>(e);
    return e;
}

// Sum every cell of a SoilGrid (milli-units).
std::int64_t soilSum(const luminumbra::foliage::SoilGrid& g) {
    std::int64_t s = 0;
    for (int z = 0; z < g.height(); ++z)
        for (int x = 0; x < g.width(); ++x)
            s += g.AtCell(x, z);
    return s;
}

std::int64_t irrigSum(const luminumbra::foliage::IrrigationGrid& g) {
    std::int64_t s = 0;
    for (int z = 0; z < g.height(); ++z)
        for (int x = 0; x < g.width(); ++x)
            s += g.AtCell(x, z);
    return s;
}

// ===========================================================================
// FIRE SPREAD
// ===========================================================================

// Gating: a registry with no combustibles is a strict no-op (zero work).
TEST(FireHardening, EmptyRosterNoOp) {
    entt::registry r;
    const auto stats = luminumbra::sim::RunFireSpreadOnTick(r, 1, glm::vec2(1.0f, 0.0f));
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(stats.ignited, 0);
    EXPECT_EQ(stats.burnt_out, 0);
}

// run == replay: two independent registries, identical setup + ticks -> identical state.
TEST(FireHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        makeCombustible(r, 0.0f, 0.0f, Comp::BurnState::Burning);
        makeCombustible(r, 1.0f, 0.0f, Comp::BurnState::Unburnt);
        makeCombustible(r, 2.0f, 0.0f, Comp::BurnState::Unburnt, /*fuel*/ 800, /*moist*/ 200);
        makeCombustible(r, -1.0f, 0.0f, Comp::BurnState::Unburnt);
    };
    entt::registry a, b;
    build(a);
    build(b);
    const glm::vec2 wind(1.0f, 0.0f);
    for (std::uint64_t t = 0; t < 20; ++t) {
        luminumbra::sim::RunFireSpreadOnTick(a, t, wind);
        luminumbra::sim::RunFireSpreadOnTick(b, t, wind);
    }
    // Compare keyed by position (order-independent over the two registries).
    auto snap = [](entt::registry& r) {
        std::vector<std::tuple<float, std::uint8_t, std::uint32_t, std::uint16_t>> v;
        auto view = r.view<Comp::CombustibleComponent, Comp::TransformComponent>();
        for (auto e : view) {
            const auto& cb = view.get<Comp::CombustibleComponent>(e);
            const auto& tf = view.get<Comp::TransformComponent>(e);
            v.push_back({tf.position.x, cb.burn_state, cb.burn_ticks_remaining, cb.fuel_milli});
        }
        std::sort(v.begin(), v.end());
        return v;
    };
    EXPECT_EQ(snap(a), snap(b));
}

// Downwind bias SIGN: with wind blowing toward +x, a candidate DOWNWIND (+x of the
// source) must receive STRICTLY MORE heat than a mirror candidate UPWIND (-x), at the
// same distance. This is the core wind-coupling contract.
TEST(FireHardening, DownwindGetsMoreHeatThanUpwind) {
    using luminumbra::sim::FireHeatContribution;
    const float radius = 4.0f;
    const float d = 2.0f;
    const glm::vec2 wind(3.0f, 0.0f); // blowing toward +x
    // dir = unit source->candidate.
    const glm::vec2 downwind_dir(1.0f, 0.0f);
    const glm::vec2 upwind_dir(-1.0f, 0.0f);
    const float h_down = FireHeatContribution(d, radius, downwind_dir, wind, 1.0f);
    const float h_up = FireHeatContribution(d, radius, upwind_dir, wind, 1.0f);
    EXPECT_GT(h_down, h_up);
    // Crosswind (perpendicular) should equal the still-air base (no along-wind term).
    const glm::vec2 cross_dir(0.0f, 1.0f);
    const float h_cross = FireHeatContribution(d, radius, cross_dir, wind, 1.0f);
    const float h_still = FireHeatContribution(d, radius, cross_dir, glm::vec2(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(h_cross, h_still);
}

// Heat never goes negative even strongly upwind, and is zero beyond the radius.
TEST(FireHardening, HeatBoundsNonNegativeAndZeroBeyondRadius) {
    using luminumbra::sim::FireHeatContribution;
    const glm::vec2 wind(10.0f, 0.0f);
    // Far upwind candidate: along-wind term is strongly negative -> clamp to >= 0.
    const float h = FireHeatContribution(1.0f, 4.0f, glm::vec2(-1.0f, 0.0f), wind, 1.0f);
    EXPECT_GE(h, 0.0f);
    // Beyond the radius -> exactly zero.
    EXPECT_FLOAT_EQ(FireHeatContribution(5.0f, 4.0f, glm::vec2(1.0f, 0.0f), wind, 1.0f), 0.0f);
    // Zero / negative radius -> zero (degenerate input, no NaN).
    EXPECT_FLOAT_EQ(FireHeatContribution(0.0f, 0.0f, glm::vec2(0.0f), wind, 1.0f), 0.0f);
}

// Coincident source+candidate (d==0): pure base heat, no NaN from the 1/d direction.
TEST(FireHardening, CoincidentSourceNoNaN) {
    entt::registry r;
    makeCombustible(r, 0.0f, 0.0f, Comp::BurnState::Burning);
    makeCombustible(r, 0.0f, 0.0f, Comp::BurnState::Unburnt); // exactly on top
    const auto stats = luminumbra::sim::RunFireSpreadOnTick(r, 1, glm::vec2(5.0f, 0.0f));
    // The coincident unburnt cell should ignite from pure base heat (>= resistance).
    EXPECT_GE(stats.ignited, 1);
}

// Burn-out lifecycle: a Burning cell counts down burn_ticks_remaining and becomes
// Burnt (inert, fuel consumed) exactly when it hits zero, and never re-ignites.
TEST(FireHardening, BurnOutTransitionAndInert) {
    entt::registry r;
    auto e = makeCombustible(r, 0.0f, 0.0f, Comp::BurnState::Burning);
    r.get<Comp::CombustibleComponent>(e).burn_ticks_remaining = 3;
    // No unburnt neighbours -> only the burn-down path runs.
    for (int i = 0; i < 3; ++i)
        luminumbra::sim::RunFireSpreadOnTick(r, i, glm::vec2(0.0f));
    const auto& cb = r.get<Comp::CombustibleComponent>(e);
    EXPECT_EQ(cb.state(), Comp::BurnState::Burnt);
    EXPECT_EQ(cb.fuel_milli, 0); // fuel consumed
    // Further ticks keep it inert (Burnt never spreads / re-ignites).
    const auto stats = luminumbra::sim::RunFireSpreadOnTick(r, 99, glm::vec2(0.0f));
    EXPECT_EQ(stats.ignited, 0);
    EXPECT_EQ(cb.state(), Comp::BurnState::Burnt);
}

// Two-phase: a cell ignited THIS tick cannot chain-ignite a third cell within the
// SAME tick (spread depends only on last tick's burning set). With three cells in a
// line spaced so heat only reaches the immediate neighbour, exactly ONE ignites/tick.
TEST(FireHardening, NoSameTickChainIgnition) {
    entt::registry r;
    // radius 1.5 so each cell only reaches its immediate (1m) neighbour, not the 2m one.
    makeCombustible(r, 0.0f, 0.0f, Comp::BurnState::Burning, 1000, 0, 1.5f);
    makeCombustible(r, 1.0f, 0.0f, Comp::BurnState::Unburnt, 1000, 0, 1.5f);
    makeCombustible(r, 2.0f, 0.0f, Comp::BurnState::Unburnt, 1000, 0, 1.5f);
    const auto stats = luminumbra::sim::RunFireSpreadOnTick(r, 1, glm::vec2(0.0f));
    EXPECT_EQ(stats.ignited, 1); // only the middle cell, not the far one this tick
}

// Fuel-exhausted cells cannot ignite (has_fuel gate).
TEST(FireHardening, ZeroFuelCannotIgnite) {
    entt::registry r;
    makeCombustible(r, 0.0f, 0.0f, Comp::BurnState::Burning);
    makeCombustible(r, 0.5f, 0.0f, Comp::BurnState::Unburnt, /*fuel*/ 0);
    const auto stats = luminumbra::sim::RunFireSpreadOnTick(r, 1, glm::vec2(0.0f));
    EXPECT_EQ(stats.ignited, 0);
}

// ===========================================================================
// SOIL NUTRIENT
// ===========================================================================

using luminumbra::foliage::kSoilBaseline;
using luminumbra::foliage::kSoilRegenPerTick;
using luminumbra::foliage::RunSoilNutrientOnTick;
using luminumbra::foliage::SoilGrid;

// Gating: empty roster + at-baseline grid -> a TRUE no-op (grid stays at baseline).
TEST(SoilHardening, EmptyRosterAtBaselineIsNoOp) {
    entt::registry r;
    SoilGrid g(4, 4); // starts at baseline
    EXPECT_TRUE(g.all_at(kSoilBaseline));
    const auto stats = RunSoilNutrientOnTick(r, g, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(stats.participants, 0);
    EXPECT_EQ(stats.consumed, 0);
    EXPECT_TRUE(g.all_at(kSoilBaseline)); // regen cannot push above baseline
}

// Degenerate grid (zero cell_size) -> no-op, no divide-by-zero crash.
TEST(SoilHardening, DegenerateCellSizeNoOp) {
    entt::registry r;
    makeSoilPlant(r, 0.0f, 0.0f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting));
    SoilGrid g(4, 4);
    const auto stats = RunSoilNutrientOnTick(r, g, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(stats.participants, 0);
}

// Bounds: a cell never goes negative even with a huge over-draw, and regen never
// pushes a cell above the baseline.
TEST(SoilHardening, CellBoundsNonNegativeAndCapped) {
    entt::registry r;
    // Many hungry fruiting plants on one cell, each max uptake.
    for (int i = 0; i < 50; ++i)
        makeSoilPlant(r, 0.2f, 0.2f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 65535);
    SoilGrid g(2, 2);
    for (int t = 0; t < 50; ++t)
        RunSoilNutrientOnTick(r, g, 0.0f, 0.0f, 1.0f);
    for (int z = 0; z < 2; ++z)
        for (int x = 0; x < 2; ++x) {
            EXPECT_GE(g.AtCell(x, z), 0);
            EXPECT_LE(g.AtCell(x, z), kSoilBaseline);
        }
}

// Order-independence: total consumed from a contested cell is the SAME regardless of
// plant creation (id) order. Two plants on one cell that together exceed availability.
TEST(SoilHardening, ConsumptionOrderIndependent) {
    auto run = [](bool reversed) {
        entt::registry r;
        // Drain the cell low first so two plants contend for what's left.
        SoilGrid g(1, 1, /*initial*/ 5); // only 5 milli available
        if (!reversed) {
            makeSoilPlant(
                r, 0.5f, 0.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1000);
            makeSoilPlant(
                r, 0.5f, 0.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1000);
        } else {
            // Same two plants, but a throwaway entity bumps ids so order differs.
            auto tmp = r.create();
            r.destroy(tmp);
            makeSoilPlant(
                r, 0.5f, 0.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1000);
            makeSoilPlant(
                r, 0.5f, 0.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1000);
        }
        const auto stats = RunSoilNutrientOnTick(r, g, 0.0f, 0.0f, 1.0f);
        return stats.consumed;
    };
    EXPECT_EQ(run(false), run(true));
}

// Producer->consumer: NutrientAt(world) reads the SAME cell the tick consumed from.
// A plant at (x,z) draws from its cell; NutrientAt at that (x,z) must reflect the draw.
TEST(SoilHardening, NutrientAtMatchesConsumedCell) {
    entt::registry r;
    makeSoilPlant(r, 3.5f, 7.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 16000);
    SoilGrid g(16, 16);
    const std::int32_t before = luminumbra::foliage::NutrientAt(g, 3.5f, 7.5f, 0.0f, 0.0f, 1.0f);
    RunSoilNutrientOnTick(r, g, 0.0f, 0.0f, 1.0f);
    const std::int32_t after = luminumbra::foliage::NutrientAt(g, 3.5f, 7.5f, 0.0f, 0.0f, 1.0f);
    // uptake 16000 * weight12 / 16 = 12000 milli draw; regen +4 -> net big drop.
    EXPECT_LT(after, before);
}

// Negative-coordinate cell mapping must floor (not truncate toward zero) so a plant at
// x=-0.5 maps to cell -1, not 0. A plant just left of the origin should NOT draw from
// cell 0; outside the grid it draws nothing.
TEST(SoilHardening, NegativeCoordFloorMapping) {
    entt::registry r;
    makeSoilPlant(r, -0.5f, 0.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 16000);
    SoilGrid g(4, 4); // covers cells [0,3]; cell -1 is outside
    const std::int64_t before = soilSum(g);
    const auto stats = RunSoilNutrientOnTick(r, g, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(stats.consumed, 0); // x=-0.5 -> cell -1 -> outside -> no draw
    // Only regen happened (grid was at baseline -> stays at baseline).
    EXPECT_EQ(soilSum(g), before);
}

// run==replay for the soil tick (grid + absorbed accrual byte-identical).
TEST(SoilHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        for (int i = 0; i < 4; ++i)
            makeSoilPlant(r,
                          static_cast<float>(i) + 0.5f,
                          0.5f,
                          static_cast<std::uint8_t>(i % Comp::kPlantStageCount),
                          static_cast<std::uint16_t>(1000 + i * 7));
    };
    entt::registry a, b;
    SoilGrid ga(8, 8), gb(8, 8);
    build(a);
    build(b);
    for (int t = 0; t < 30; ++t) {
        RunSoilNutrientOnTick(a, ga, 0.0f, 0.0f, 1.0f);
        RunSoilNutrientOnTick(b, gb, 0.0f, 0.0f, 1.0f);
    }
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x)
            EXPECT_EQ(ga.AtCell(x, z), gb.AtCell(x, z));
}

// ===========================================================================
// IRRIGATION (deposit + diffuse + drain; conservation + stability)
// ===========================================================================

using luminumbra::foliage::IrrigationGrid;
using luminumbra::foliage::kMoistureBaseline;
using luminumbra::foliage::kMoistureDrainPerTick;
using luminumbra::foliage::kMoistureMax;
using luminumbra::foliage::RunIrrigationOnTick;

// Gating: no sources + at-baseline grid -> true no-op (stays dry).
TEST(IrrigationHardening, EmptyRosterAtBaselineNoOp) {
    entt::registry r;
    IrrigationGrid g(4, 4); // baseline 0
    const auto stats = RunIrrigationOnTick(r, g, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(stats.sources, 0);
    EXPECT_EQ(stats.deposited, 0);
    EXPECT_TRUE(g.all_at(kMoistureBaseline));
}

// Diffusion CONSERVATION: with NO drain interference (drain only removes above
// baseline), the pure diffuse step must conserve total moisture exactly. We verify
// by depositing once then checking that, ignoring the deterministic drain, the
// diffused mass equals deposit minus drain. Simplest robust check: total after one
// tick == deposited - drained (telemetry self-consistency), and never exceeds the
// deposited total (diffusion adds nothing).
TEST(IrrigationHardening, DiffusionConservesNoCreation) {
    entt::registry r;
    makeWaterSource(r, 2.5f, 2.5f, 4000); // strong central deposit
    const int W = 5, H = 5;
    IrrigationGrid g(W, H);
    const std::int64_t before = irrigSum(g); // 0 (baseline)
    const auto stats = RunIrrigationOnTick(r, g, 0.0f, 0.0f, 1.0f);
    const std::int64_t total = irrigSum(g);
    // Telemetry self-consistency: total == before + deposited - drained.
    EXPECT_EQ(total, before + stats.deposited - stats.drained);
    // INDEPENDENT conservation bound: diffusion creates NOTHING, drain removes at most
    // kMoistureDrainPerTick per cell. So the post-tick mass sits in a tight window
    // [deposited - N*drain, deposited]. If diffusion leaked/created mass this fails.
    const std::int64_t N = static_cast<std::int64_t>(W) * H;
    EXPECT_LE(total, before + stats.deposited);                             // no creation
    EXPECT_GE(total, before + stats.deposited - N * kMoistureDrainPerTick); // only drain removes
}

// Diffusion STABILITY: no cell overshoots max, no cell goes negative, no oscillation
// blow-up over many ticks with a steady strong source.
TEST(IrrigationHardening, DiffusionStableNoOvershootOrNegative) {
    entt::registry r;
    makeWaterSource(r, 4.5f, 4.5f, 4000);
    IrrigationGrid g(10, 10);
    for (int t = 0; t < 200; ++t) {
        RunIrrigationOnTick(r, g, 0.0f, 0.0f, 1.0f);
        for (int z = 0; z < 10; ++z)
            for (int x = 0; x < 10; ++x) {
                EXPECT_GE(g.AtCell(x, z), kMoistureBaseline);
                EXPECT_LE(g.AtCell(x, z), kMoistureMax);
            }
    }
}

// Diffusion SPREADS moisture to neighbours (a single source waters a band, not one
// tile): after a tick, a neighbour cell of the source is wetter than baseline.
TEST(IrrigationHardening, DiffusionSpreadsToNeighbour) {
    entt::registry r;
    makeWaterSource(r, 5.5f, 5.5f, 4000);
    IrrigationGrid g(12, 12);
    // Run a few ticks so diffusion propagates.
    for (int t = 0; t < 5; ++t)
        RunIrrigationOnTick(r, g, 0.0f, 0.0f, 1.0f);
    // Cell (5,5) is the source cell; (6,5) is its neighbour.
    EXPECT_GT(g.AtCell(6, 5), kMoistureBaseline);
}

// Drain: with the source removed, every wet cell relaxes toward the dry baseline and
// never undershoots it.
TEST(IrrigationHardening, DrainRelaxesToBaselineNoUndershoot) {
    entt::registry r_src;
    makeWaterSource(r_src, 2.5f, 2.5f, 4000);
    IrrigationGrid g(5, 5);
    for (int t = 0; t < 10; ++t)
        RunIrrigationOnTick(r_src, g, 0.0f, 0.0f, 1.0f);
    // Now drain with an EMPTY roster (no sources).
    entt::registry empty;
    for (int t = 0; t < 5000; ++t)
        RunIrrigationOnTick(empty, g, 0.0f, 0.0f, 1.0f);
    EXPECT_TRUE(g.all_at(kMoistureBaseline)); // fully drained, never below baseline
}

// Deposit order-independence: two sources on one cell add the same total regardless
// of id order.
TEST(IrrigationHardening, DepositOrderIndependent) {
    auto run = [](bool reversed) {
        entt::registry r;
        if (reversed) {
            auto t = r.create();
            r.destroy(t);
        }
        makeWaterSource(r, 0.5f, 0.5f, 1000);
        makeWaterSource(r, 0.5f, 0.5f, 1500);
        IrrigationGrid g(3, 3);
        const auto stats = RunIrrigationOnTick(r, g, 0.0f, 0.0f, 1.0f);
        return stats.deposited;
    };
    EXPECT_EQ(run(false), run(true));
}

// run==replay across two grids.
TEST(IrrigationHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        makeWaterSource(r, 1.5f, 1.5f, 2000);
        makeWaterSource(r, 4.5f, 4.5f, 3500);
    };
    entt::registry a, b;
    build(a);
    build(b);
    IrrigationGrid ga(8, 8), gb(8, 8);
    for (int t = 0; t < 40; ++t) {
        RunIrrigationOnTick(a, ga, 0.0f, 0.0f, 1.0f);
        RunIrrigationOnTick(b, gb, 0.0f, 0.0f, 1.0f);
    }
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x)
            EXPECT_EQ(ga.AtCell(x, z), gb.AtCell(x, z));
}

// MoistureAt producer->consumer: the world query reads the cell the source deposited
// into.
TEST(IrrigationHardening, MoistureAtMatchesDepositedCell) {
    entt::registry r;
    makeWaterSource(r, 6.5f, 2.5f, 4000);
    IrrigationGrid g(16, 16);
    RunIrrigationOnTick(r, g, 0.0f, 0.0f, 1.0f);
    EXPECT_GT(luminumbra::foliage::MoistureAt(g, 6.5f, 2.5f, 0.0f, 0.0f, 1.0f), kMoistureBaseline);
}

// ===========================================================================
// POLLINATION
// ===========================================================================

using luminumbra::foliage::kPollinationRadius;
using luminumbra::foliage::PollinateCross;
using luminumbra::foliage::PollinationEffectiveDistance;
using luminumbra::foliage::RunPollinationOnTick;

// Gating: empty roster -> no work, nothing written.
TEST(PollinationHardening, EmptyRosterNoOp) {
    entt::registry r;
    const auto stats = RunPollinationOnTick(r, 1);
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(stats.crossed, 0);
}

// A field of only non-reproductive (Seed/Juvenile) plants has NO donors -> no crosses.
TEST(PollinationHardening, NoDonorsNoCross) {
    entt::registry r;
    makePollinator(r, 0.0f, 0.0f, static_cast<std::uint8_t>(Comp::PlantStage::Juvenile));
    makePollinator(r, 1.0f, 0.0f, static_cast<std::uint8_t>(Comp::PlantStage::Seed));
    const auto stats = RunPollinationOnTick(r, 1);
    EXPECT_EQ(stats.donors, 0);
    EXPECT_EQ(stats.crossed, 0);
}

// An isolated receiver (donor out of reach) is left unchanged.
TEST(PollinationHardening, IsolatedReceiverUnchanged) {
    entt::registry r;
    makePollinator(r, 0.0f, 0.0f, static_cast<std::uint8_t>(Comp::PlantStage::Juvenile));
    makePollinator(r, 100.0f, 0.0f, static_cast<std::uint8_t>(Comp::PlantStage::Flowering));
    const auto stats = RunPollinationOnTick(r, 1);
    EXPECT_EQ(stats.crossed, 0);
}

// PAIR-SYMMETRY: the cross A-receives-from-B must produce the identical genome as
// B-receives-from-A (seed built from the SORTED id pair). This is the core
// order-independence contract of pollination.
TEST(PollinationHardening, CrossIsPairSymmetric) {
    Comp::PlantGenomeComponent gA, gB;
    for (std::size_t i = 0; i < gA.genes.size(); ++i) {
        gA.genes[i] = 0.2f + 0.05f * static_cast<float>(i);
        gB.genes[i] = 0.8f - 0.03f * static_cast<float>(i);
    }
    const std::uint64_t idA = 7, idB = 42, tick = 123;
    const auto c1 = PollinateCross(gA, gB, idA, idB, tick); // A receives from B
    const auto c2 = PollinateCross(gB, gA, idB, idA, tick); // B receives from A
    for (std::size_t i = 0; i < c1.genes.size(); ++i)
        EXPECT_FLOAT_EQ(c1.genes[i], c2.genes[i]) << "gene " << i;
}

// Crossed genes stay within the [0,1] gene bounds (BreedPlants clamps).
TEST(PollinationHardening, CrossGenesWithinUnitBounds) {
    Comp::PlantGenomeComponent gA, gB;
    for (auto& v : gA.genes)
        v = 0.0f;
    for (auto& v : gB.genes)
        v = 1.0f;
    const auto c = PollinateCross(gA, gB, 3, 9, 55);
    for (std::size_t i = 0; i < c.genes.size(); ++i) {
        EXPECT_GE(c.genes[i], 0.0f) << "gene " << i;
        EXPECT_LE(c.genes[i], 1.0f) << "gene " << i;
    }
}

// WIND-COUPLING SIGN for pollen reach: a receiver DOWNWIND of a donor has a SMALLER
// effective distance than the same geometric distance UPWIND (pollen carries
// downwind). Mirror the fire wind contract.
TEST(PollinationHardening, DownwindReducesEffectiveDistance) {
    const ::Luminumbra::Vec3 donor(0.0f, 0.0f, 0.0f);
    const ::Luminumbra::Vec3 downwind_r(3.0f, 0.0f, 0.0f); // +x of donor
    const ::Luminumbra::Vec3 upwind_r(-3.0f, 0.0f, 0.0f);  // -x of donor
    const ::Luminumbra::Vec2 wind(2.0f, 0.0f);             // blowing toward +x
    const float eff_down = PollinationEffectiveDistance(donor, downwind_r, wind);
    const float eff_up = PollinationEffectiveDistance(donor, upwind_r, wind);
    EXPECT_LT(eff_down, eff_up);
    // Zero wind -> plain Euclidean distance (no bias, equal both sides).
    const float still_down =
        PollinationEffectiveDistance(donor, downwind_r, ::Luminumbra::Vec2(0.0f));
    const float still_up = PollinationEffectiveDistance(donor, upwind_r, ::Luminumbra::Vec2(0.0f));
    EXPECT_FLOAT_EQ(still_down, still_up);
    EXPECT_GE(eff_down, 0.0f); // clamped non-negative
}

// run==replay for the full pollination tick (next_genome byte-identical across runs).
TEST(PollinationHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        makePollinator(r, 0.0f, 0.0f, static_cast<std::uint8_t>(Comp::PlantStage::Juvenile), 0.3f);
        makePollinator(r, 2.0f, 0.0f, static_cast<std::uint8_t>(Comp::PlantStage::Flowering), 0.7f);
        makePollinator(r, 1.0f, 1.0f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 0.5f);
    };
    entt::registry a, b;
    build(a);
    build(b);
    for (std::uint64_t t = 0; t < 5; ++t) {
        RunPollinationOnTick(a, t, ::Luminumbra::Vec2(1.0f, 0.0f));
        RunPollinationOnTick(b, t, ::Luminumbra::Vec2(1.0f, 0.0f));
    }
    auto snap = [](entt::registry& r) {
        std::vector<std::tuple<float, std::array<float, Comp::kPlantGeneCount>, std::uint32_t>> v;
        auto view = r.view<Comp::PollinationComponent, Comp::TransformComponent>();
        for (auto e : view) {
            const auto& pc = view.get<Comp::PollinationComponent>(e);
            const auto& tf = view.get<Comp::TransformComponent>(e);
            v.push_back({tf.position.x, pc.next_genome.genes, pc.crosses});
        }
        std::sort(v.begin(), v.end(), [](const auto& l, const auto& rr) {
            return std::get<0>(l) < std::get<0>(rr);
        });
        return v;
    };
    EXPECT_EQ(snap(a), snap(b));
}

// ===========================================================================
// PLANT DISEASE
// ===========================================================================

using luminumbra::foliage::kRecoverResistance;
using luminumbra::foliage::kSickTicks;
using luminumbra::foliage::RunPlantDiseaseOnTick;

// Gating: empty roster -> no-op.
TEST(DiseaseHardening, EmptyRosterNoOp) {
    entt::registry r;
    const auto stats = RunPlantDiseaseOnTick(r, 1);
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(stats.new_infections, 0);
}

// A single Healthy plant with no infected neighbour never gets sick (and its infection
// stays at the floor, never wrapping below zero).
TEST(DiseaseHardening, SoleHealthyStaysHealthy) {
    entt::registry r;
    auto e = makeDiseasePlant(r, 0.0f, 0.0f, Comp::PlantDiseaseState::Healthy);
    for (int t = 0; t < 50; ++t)
        RunPlantDiseaseOnTick(r, t);
    const auto& h = r.get<Comp::PlantHealthComponent>(e);
    EXPECT_EQ(h.state, static_cast<std::uint8_t>(Comp::PlantDiseaseState::Healthy));
    EXPECT_EQ(h.infection, 0); // SatSub floored at 0, no underflow wrap
}

// Bounds: infection + resistance stay within [0, 1000] (milli-units) under sustained
// heavy pressure (many infected neighbours).
TEST(DiseaseHardening, FixedPointBoundsUnderHeavyPressure) {
    entt::registry r;
    // A ring of infected plants around a low-resistance target -> max pressure.
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 0.7f;
        makeDiseasePlant(r, 0.5f * a, 0.5f, Comp::PlantDiseaseState::Infected);
    }
    auto target = makeDiseasePlant(r, 0.0f, 0.0f, Comp::PlantDiseaseState::Healthy, /*res*/ 0);
    for (int t = 0; t < 200; ++t) {
        RunPlantDiseaseOnTick(r, t);
        const auto& h = r.get<Comp::PlantHealthComponent>(target);
        EXPECT_LE(h.infection, Comp::kDiseaseUnitScale);
        EXPECT_LE(h.resistance, Comp::kDiseaseUnitScale);
    }
}

// A fully-resistant (1000) plant takes ZERO effective infection and never tips.
TEST(DiseaseHardening, FullyResistantNeverInfected) {
    entt::registry r;
    makeDiseasePlant(r, 0.5f, 0.0f, Comp::PlantDiseaseState::Infected);
    auto target = makeDiseasePlant(r, 0.0f, 0.0f, Comp::PlantDiseaseState::Healthy, /*res*/ 1000);
    for (int t = 0; t < 100; ++t)
        RunPlantDiseaseOnTick(r, t);
    EXPECT_EQ(r.get<Comp::PlantHealthComponent>(target).state,
              static_cast<std::uint8_t>(Comp::PlantDiseaseState::Healthy));
}

// Lifecycle: an Infected plant with high resistance RECOVERS at kSickTicks (and
// resistance rises); with low resistance it DIES. Resolution boundary is exact.
TEST(DiseaseHardening, InfectedResolvesRecoverOrDie) {
    // High-resistance infected, isolated -> recovers at kSickTicks.
    {
        entt::registry r;
        auto e = makeDiseasePlant(r,
                                  0.0f,
                                  0.0f,
                                  Comp::PlantDiseaseState::Infected,
                                  /*res*/ kRecoverResistance);
        for (std::uint32_t t = 0; t < kSickTicks; ++t)
            RunPlantDiseaseOnTick(r, t);
        const auto& h = r.get<Comp::PlantHealthComponent>(e);
        EXPECT_EQ(h.state, static_cast<std::uint8_t>(Comp::PlantDiseaseState::Recovered));
        EXPECT_GT(h.resistance, kRecoverResistance); // acquired immunity raised it
        EXPECT_EQ(h.infection, 0);
    }
    // Low-resistance infected, isolated -> dies at kSickTicks.
    {
        entt::registry r;
        auto e = makeDiseasePlant(r, 0.0f, 0.0f, Comp::PlantDiseaseState::Infected, /*res*/ 0);
        for (std::uint32_t t = 0; t < kSickTicks; ++t)
            RunPlantDiseaseOnTick(r, t);
        const auto& h = r.get<Comp::PlantHealthComponent>(e);
        EXPECT_EQ(h.state, static_cast<std::uint8_t>(Comp::PlantDiseaseState::Dead));
    }
}

// A Dead plant is a terminal carcass: it neither spreads nor heals nor changes state.
TEST(DiseaseHardening, DeadPlantInert) {
    entt::registry r;
    auto dead = makeDiseasePlant(r, 0.0f, 0.0f, Comp::PlantDiseaseState::Dead, /*res*/ 1000);
    auto healthy = makeDiseasePlant(r, 0.5f, 0.0f, Comp::PlantDiseaseState::Healthy, /*res*/ 0);
    const std::uint16_t inf0 = r.get<Comp::PlantHealthComponent>(dead).infection;
    for (int t = 0; t < 100; ++t)
        RunPlantDiseaseOnTick(r, t);
    // Dead state unchanged; the dead plant did not infect the adjacent healthy plant.
    EXPECT_EQ(r.get<Comp::PlantHealthComponent>(dead).state,
              static_cast<std::uint8_t>(Comp::PlantDiseaseState::Dead));
    EXPECT_EQ(r.get<Comp::PlantHealthComponent>(healthy).state,
              static_cast<std::uint8_t>(Comp::PlantDiseaseState::Healthy));
    (void)inf0;
}

// Order-independence: spread result is identical regardless of plant id order
// (two-phase snapshot). Key by position.
TEST(DiseaseHardening, SpreadOrderIndependent) {
    auto run = [](bool reversed) {
        entt::registry r;
        if (reversed) {
            auto t = r.create();
            r.destroy(t);
        }
        makeDiseasePlant(r, 0.0f, 0.0f, Comp::PlantDiseaseState::Infected);
        makeDiseasePlant(r, 1.0f, 0.0f, Comp::PlantDiseaseState::Healthy, /*res*/ 0);
        makeDiseasePlant(r, 2.0f, 0.0f, Comp::PlantDiseaseState::Healthy, /*res*/ 0);
        for (int t = 0; t < 10; ++t)
            RunPlantDiseaseOnTick(r, t);
        std::vector<std::pair<float, std::uint8_t>> v;
        auto view = r.view<Comp::PlantHealthComponent, Comp::TransformComponent>();
        for (auto e : view) {
            v.push_back({view.get<Comp::TransformComponent>(e).position.x,
                         view.get<Comp::PlantHealthComponent>(e).state});
        }
        std::sort(v.begin(), v.end());
        return v;
    };
    EXPECT_EQ(run(false), run(true));
}

// run==replay across two registries.
TEST(DiseaseHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        makeDiseasePlant(r, 0.0f, 0.0f, Comp::PlantDiseaseState::Infected);
        makeDiseasePlant(r, 1.5f, 0.0f, Comp::PlantDiseaseState::Healthy, 300);
        makeDiseasePlant(r, 3.0f, 0.0f, Comp::PlantDiseaseState::Healthy, 600);
    };
    entt::registry a, b;
    build(a);
    build(b);
    for (std::uint64_t t = 0; t < 120; ++t) {
        RunPlantDiseaseOnTick(a, t);
        RunPlantDiseaseOnTick(b, t);
    }
    auto snap = [](entt::registry& r) {
        std::vector<std::tuple<float, std::uint8_t, std::uint16_t, std::uint16_t, std::uint32_t>> v;
        auto view = r.view<Comp::PlantHealthComponent, Comp::TransformComponent>();
        for (auto e : view) {
            const auto& h = view.get<Comp::PlantHealthComponent>(e);
            v.push_back({view.get<Comp::TransformComponent>(e).position.x,
                         h.state,
                         h.infection,
                         h.resistance,
                         h.infected_ticks});
        }
        std::sort(v.begin(), v.end());
        return v;
    };
    EXPECT_EQ(snap(a), snap(b));
}

// ===========================================================================
// WEATHER EVENT SCHEDULER (pure)
// ===========================================================================

using luminumbra::sim::kWeatherEventCount;
using luminumbra::sim::kWindowTicks;
using luminumbra::sim::WeatherEvent;
using luminumbra::sim::WeatherEventAt;
using luminumbra::sim::WeatherEventDriver;

// Window 0 always starts Clear (the calm premise).
TEST(WeatherHardening, WindowZeroIsClear) {
    for (std::uint64_t seed = 0; seed < 32; ++seed) {
        const auto s = WeatherEventAt(0, seed);
        EXPECT_EQ(s.event, WeatherEvent::Clear) << "seed " << seed;
        EXPECT_EQ(s.window_index, 0u);
    }
}

// run==replay: the same (tick, seed) ALWAYS yields the identical state.
TEST(WeatherHardening, RunEqualsReplay) {
    for (std::uint64_t seed : {0ull, 1ull, 7ull, 9999ull}) {
        for (std::uint64_t tick = 0; tick < kWindowTicks * 12; tick += 137) {
            const auto a = WeatherEventAt(tick, seed);
            const auto b = WeatherEventAt(tick, seed);
            EXPECT_EQ(a.event, b.event);
            EXPECT_FLOAT_EQ(a.intensity, b.intensity);
            EXPECT_EQ(a.window_index, b.window_index);
        }
    }
}

// Intensity ALWAYS within [0,1] across many windows + seeds (clamp contract).
TEST(WeatherHardening, IntensityWithinUnitRange) {
    for (std::uint64_t seed = 0; seed < 16; ++seed) {
        for (std::uint64_t w = 0; w < 50; ++w) {
            const auto s = WeatherEventAt(w * kWindowTicks, seed);
            EXPECT_GE(s.intensity, 0.0f);
            EXPECT_LE(s.intensity, 1.0f);
        }
    }
}

// FORBIDDEN transitions: Drought->Snow and Snow->Drought never occur back-to-back
// between consecutive windows (transition weight 0).
TEST(WeatherHardening, ForbiddenAdjacentTransitions) {
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        WeatherEvent prev = WeatherEventAt(0, seed).event;
        for (std::uint64_t w = 1; w < 80; ++w) {
            const WeatherEvent cur = WeatherEventAt(w * kWindowTicks, seed).event;
            const bool droughtToSnow = prev == WeatherEvent::Drought && cur == WeatherEvent::Snow;
            const bool snowToDrought = prev == WeatherEvent::Snow && cur == WeatherEvent::Drought;
            EXPECT_FALSE(droughtToSnow) << "seed " << seed << " window " << w;
            EXPECT_FALSE(snowToDrought) << "seed " << seed << " window " << w;
            prev = cur;
        }
    }
}

// Event index is always a valid enum value in [0, count).
TEST(WeatherHardening, EventAlwaysValidEnum) {
    for (std::uint64_t seed = 0; seed < 16; ++seed)
        for (std::uint64_t w = 0; w < 40; ++w) {
            const int ei = static_cast<int>(WeatherEventAt(w * kWindowTicks, seed).event);
            EXPECT_GE(ei, 0);
            EXPECT_LT(ei, kWeatherEventCount);
        }
}

// The window index is exactly tick / kWindowTicks, and is constant within a window.
TEST(WeatherHardening, WindowIndexAndEventConstantWithinWindow) {
    const std::uint64_t seed = 5;
    // Pick a window well into the chain.
    const std::uint64_t w = 6;
    const auto at_start = WeatherEventAt(w * kWindowTicks, seed);
    EXPECT_EQ(at_start.window_index, w);
    for (std::uint64_t off = 0; off < kWindowTicks; off += 101) {
        const auto s = WeatherEventAt(w * kWindowTicks + off, seed);
        EXPECT_EQ(s.window_index, w);
        EXPECT_EQ(s.event, at_start.event);               // event is window-keyed
        EXPECT_FLOAT_EQ(s.intensity, at_start.intensity); // intensity too
    }
}

// The stateful driver must match the pure WeatherEventAt at every tick it advances to
// (the driver "changes nothing observable").
TEST(WeatherHardening, DriverMatchesPureQuery) {
    const std::uint64_t seed = 13;
    WeatherEventDriver drv(seed);
    for (std::uint64_t tick = 0; tick < kWindowTicks * 5; tick += 7) {
        drv.AdvanceTo(tick);
        const auto pure = WeatherEventAt(tick, seed);
        EXPECT_EQ(drv.current().event, pure.event) << "tick " << tick;
        EXPECT_FLOAT_EQ(drv.current().intensity, pure.intensity) << "tick " << tick;
        EXPECT_EQ(drv.current().window_index, pure.window_index) << "tick " << tick;
    }
}

// Stepping one tick at a time must also match the pure query (catches a stale-cache
// bug where intensity is not refreshed inside a window).
TEST(WeatherHardening, DriverStepMatchesPureQuery) {
    const std::uint64_t seed = 99;
    WeatherEventDriver drv(seed);
    // Step across a couple of window boundaries.
    for (std::uint64_t tick = 0; tick <= kWindowTicks * 3; ++tick) {
        if (tick > 0)
            drv.Step();
        const auto pure = WeatherEventAt(tick, seed);
        ASSERT_EQ(drv.tick(), tick);
        EXPECT_EQ(drv.current().event, pure.event) << "tick " << tick;
        EXPECT_FLOAT_EQ(drv.current().intensity, pure.intensity) << "tick " << tick;
    }
}

// Different seeds diverge somewhere in the schedule (the seed actually perturbs the
// stream — a no-op seed mix would be a silent determinism bug).
TEST(WeatherHardening, DistinctSeedsDiverge) {
    bool diverged = false;
    for (std::uint64_t w = 1; w < 200 && !diverged; ++w) {
        if (WeatherEventAt(w * kWindowTicks, 0).event !=
            WeatherEventAt(w * kWindowTicks, 12345).event) {
            diverged = true;
        }
    }
    EXPECT_TRUE(diverged);
}

} // namespace
