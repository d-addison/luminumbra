// sim.soil: DETERMINISTIC soil NUTRIENT field. Plants (PlantTag +
// SoilFeederComponent) draw nutrient from their cell proportional to growth stage;
// fallow soil regenerates toward a baseline each tick. Deterministic: integer
// fixed-point store, id-ordered traversal, per-cell consumption summed so the
// result is order-independent; NO rng / wall-clock / libm. Gated by
// SoilFeederComponent: a registry with none leaves the grid untouched (no-op),
// so the canonical NetworkStateHash baseline stays byte-identical.
#include <gtest/gtest.h>

#include <cstdint>

#include <entt/entt.hpp>

#include "components/CoreComponents.h"
#include "components/PlantComponents.h"
#include "components/SoilComponents.h"
#include "systems/SoilNutrientSystem.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::foliage::SoilGrid;
using luminumbra::foliage::RunSoilNutrientOnTick;
using luminumbra::foliage::NutrientAt;
using luminumbra::foliage::kSoilBaseline;
using luminumbra::foliage::kSoilRegenPerTick;

// Grid anchored at the origin with 1m cells, so world (x,z) maps directly to cell
// (floor(x), floor(z)). 8x8 covers the test positions.
constexpr float kOX = 0.0f;
constexpr float kOZ = 0.0f;
constexpr float kCell = 1.0f;

// Spawn a soil-feeding plant at (x,z) at a given growth stage and uptake strength.
entt::entity spawnPlant(entt::registry& r, float x, float z,
                        std::uint8_t stage = static_cast<std::uint8_t>(Comp::PlantStage::Mature),
                        std::uint16_t uptake = 1000) {
    auto e = r.create();
    r.emplace<Comp::PlantTag>(e);
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& g = r.emplace<Comp::PlantGrowthComponent>(e);
    g.stage = stage;
    auto& f = r.emplace<Comp::SoilFeederComponent>(e);
    f.uptake = uptake;
    return e;
}

// ---- gating: empty roster / no opt-in ----

// No plants at all -> the grid stays exactly at baseline (a true no-op).
TEST(SoilNutrient, EmptyRosterLeavesBaseline) {
    entt::registry r;
    SoilGrid soil(8, 8);
    const auto stats = RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
    EXPECT_EQ(stats.participants, 0);
    EXPECT_EQ(stats.consumed, 0);
    EXPECT_TRUE(soil.all_at(kSoilBaseline))
        << "no soil feeders => no consumption; baseline cells stay at baseline (regen clamps)";
}

// A plant WITHOUT the SoilFeederComponent opt-in is ignored entirely (the opt-in
// is the feeder component, not the bare PlantTag): the grid stays at baseline.
TEST(SoilNutrient, PlantWithoutFeederIsIgnored) {
    entt::registry r;
    SoilGrid soil(8, 8);
    auto e = r.create();
    r.emplace<Comp::PlantTag>(e);
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = 2.0f;
    tf.position.z = 2.0f;
    r.emplace<Comp::PlantGrowthComponent>(e);
    const auto stats = RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
    EXPECT_EQ(stats.participants, 0) << "a non-feeder plant is not a participant";
    EXPECT_EQ(stats.consumed, 0);
    EXPECT_TRUE(soil.all_at(kSoilBaseline));
}

// ---- core: a plant depletes its local cell ----

TEST(SoilNutrient, MaturePlantDepletesItsCell) {
    entt::registry r;
    SoilGrid soil(8, 8);
    spawnPlant(r, 3.0f, 4.0f, static_cast<std::uint8_t>(Comp::PlantStage::Mature), 1000);

    const std::int32_t before = NutrientAt(soil, 3.0f, 4.0f, kOX, kOZ, kCell);
    EXPECT_EQ(before, kSoilBaseline);

    const auto stats = RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
    EXPECT_EQ(stats.participants, 1);
    EXPECT_GT(stats.consumed, 0);

    // Mature weight 8/16 of uptake 1000 = 500 drawn, then +regen. Net below baseline.
    const std::int32_t after = NutrientAt(soil, 3.0f, 4.0f, kOX, kOZ, kCell);
    EXPECT_LT(after, before) << "the plant's own cell drops below baseline";
    // A neighbouring cell with no plant stays at baseline (consumption is local).
    EXPECT_EQ(NutrientAt(soil, 5.0f, 4.0f, kOX, kOZ, kCell), kSoilBaseline);
}

// A seed barely feeds; a fruiting plant feeds hard (stage scales consumption).
TEST(SoilNutrient, StageScalesConsumption) {
    entt::registry seedReg;
    SoilGrid seedSoil(4, 4);
    spawnPlant(seedReg, 1.0f, 1.0f, static_cast<std::uint8_t>(Comp::PlantStage::Seed), 1000);
    const auto seedStats = RunSoilNutrientOnTick(seedReg, seedSoil, kOX, kOZ, kCell);

    entt::registry fruitReg;
    SoilGrid fruitSoil(4, 4);
    spawnPlant(fruitReg, 1.0f, 1.0f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1000);
    const auto fruitStats = RunSoilNutrientOnTick(fruitReg, fruitSoil, kOX, kOZ, kCell);

    EXPECT_GT(fruitStats.consumed, seedStats.consumed)
        << "a fruiting plant must consume more than a seed";
}

// Many plants on ONE cell deplete it faster than a single plant on its own cell.
TEST(SoilNutrient, ManyPlantsDepleteFaster) {
    entt::registry oneReg;
    SoilGrid oneSoil(4, 4);
    spawnPlant(oneReg, 1.0f, 1.0f);
    RunSoilNutrientOnTick(oneReg, oneSoil, kOX, kOZ, kCell);
    const std::int32_t oneLeft = NutrientAt(oneSoil, 1.0f, 1.0f, kOX, kOZ, kCell);

    entt::registry manyReg;
    SoilGrid manySoil(4, 4);
    spawnPlant(manyReg, 1.0f, 1.0f); // three plants, same cell
    spawnPlant(manyReg, 1.2f, 1.3f);
    spawnPlant(manyReg, 1.4f, 1.1f);
    RunSoilNutrientOnTick(manyReg, manySoil, kOX, kOZ, kCell);
    const std::int32_t manyLeft = NutrientAt(manySoil, 1.0f, 1.0f, kOX, kOZ, kCell);

    EXPECT_LT(manyLeft, oneLeft) << "three plants on one cell draw it down further";
}

// Consumption is order-INDEPENDENT: summing per-cell demand means the cell's final
// level does not depend on plant id ordering (two plants, same cell, same total).
TEST(SoilNutrient, ConsumptionOrderIndependent) {
    auto run = [](bool reversed) {
        entt::registry r;
        SoilGrid soil(4, 4);
        // Create in one order vs another; both land on cell (1,1).
        if (!reversed) {
            spawnPlant(r, 1.0f, 1.0f, static_cast<std::uint8_t>(Comp::PlantStage::Mature), 700);
            spawnPlant(r, 1.5f, 1.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1300);
        } else {
            spawnPlant(r, 1.5f, 1.5f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1300);
            spawnPlant(r, 1.0f, 1.0f, static_cast<std::uint8_t>(Comp::PlantStage::Mature), 700);
        }
        RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
        return NutrientAt(soil, 1.0f, 1.0f, kOX, kOZ, kCell);
    };
    EXPECT_EQ(run(false), run(true));
}

// ---- regeneration ----

// Fallow soil that was depleted refills toward baseline over ticks (no plants).
TEST(SoilNutrient, FallowSoilRegeneratesTowardBaseline) {
    entt::registry r; // empty: pure regeneration
    SoilGrid soil(4, 4);
    // Knock one cell well below baseline.
    soil.SetCell(2, 2, kSoilBaseline / 2);
    const std::int32_t start = soil.AtCell(2, 2);

    std::int32_t prev = start;
    for (int t = 0; t < 50; ++t) {
        RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
        const std::int32_t now = soil.AtCell(2, 2);
        EXPECT_GE(now, prev) << "depleted soil monotonically recovers";
        prev = now;
    }
    EXPECT_GT(prev, start) << "the cell recovered toward baseline";
    EXPECT_LE(prev, kSoilBaseline) << "regen never overshoots the baseline";
}

// Regen is clamped: a cell already at baseline never exceeds it.
TEST(SoilNutrient, RegenNeverOvershootsBaseline) {
    entt::registry r;
    SoilGrid soil(4, 4);
    RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
    EXPECT_TRUE(soil.all_at(kSoilBaseline));
}

// Over enough ticks a depleted-then-fallow cell returns ALL the way to baseline.
TEST(SoilNutrient, FullRecoveryToBaseline) {
    entt::registry r;
    SoilGrid soil(2, 2);
    soil.SetCell(0, 0, 0);
    // baseline / regen ticks suffice; add headroom.
    const int ticks = (kSoilBaseline / kSoilRegenPerTick) + 4;
    for (int t = 0; t < ticks; ++t) RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
    EXPECT_EQ(soil.AtCell(0, 0), kSoilBaseline);
}

// ---- NutrientAt query stability ----

TEST(SoilNutrient, NutrientAtIsStableAndBounded) {
    entt::registry r;
    SoilGrid soil(8, 8);
    spawnPlant(r, 4.0f, 4.0f);
    RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);

    // Repeated queries return the same value (pure read, no mutation).
    const std::int32_t a = NutrientAt(soil, 4.0f, 4.0f, kOX, kOZ, kCell);
    const std::int32_t b = NutrientAt(soil, 4.0f, 4.0f, kOX, kOZ, kCell);
    EXPECT_EQ(a, b);
    // Out-of-grid query is a defined 0 (not UB).
    EXPECT_EQ(NutrientAt(soil, -100.0f, -100.0f, kOX, kOZ, kCell), 0);
    EXPECT_EQ(NutrientAt(soil, 1000.0f, 1000.0f, kOX, kOZ, kCell), 0);
    // All in-grid values stay within [0, baseline].
    for (int z = 0; z < soil.height(); ++z)
        for (int x = 0; x < soil.width(); ++x) {
            EXPECT_GE(soil.AtCell(x, z), 0);
            EXPECT_LE(soil.AtCell(x, z), kSoilBaseline);
        }
}

// ---- determinism: run == replay ----

TEST(SoilNutrient, SystemDeterministicRunEqualsReplay) {
    auto run = [] {
        entt::registry r;
        SoilGrid soil(6, 6);
        spawnPlant(r, 1.0f, 1.0f, static_cast<std::uint8_t>(Comp::PlantStage::Mature), 900);
        spawnPlant(r, 4.0f, 2.0f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1500);
        spawnPlant(r, 1.0f, 1.0f, static_cast<std::uint8_t>(Comp::PlantStage::Juvenile), 600);
        std::vector<std::int32_t> trail;
        for (int t = 0; t < 12; ++t) {
            RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
            for (int z = 0; z < soil.height(); ++z)
                for (int x = 0; x < soil.width(); ++x) trail.push_back(soil.AtCell(x, z));
        }
        // Also fold in each plant's accumulated absorption.
        for (auto e : r.view<Comp::SoilFeederComponent>())
            trail.push_back(static_cast<std::int32_t>(r.get<Comp::SoilFeederComponent>(e).absorbed));
        return trail;
    };
    EXPECT_EQ(run(), run());
}

// Absorbed accrues monotonically while soil is available, and never goes negative.
TEST(SoilNutrient, AbsorbedAccrues) {
    entt::registry r;
    SoilGrid soil(4, 4);
    auto e = spawnPlant(r, 2.0f, 2.0f, static_cast<std::uint8_t>(Comp::PlantStage::Fruiting), 1200);
    std::uint32_t prev = 0;
    for (int t = 0; t < 5; ++t) {
        RunSoilNutrientOnTick(r, soil, kOX, kOZ, kCell);
        const std::uint32_t a = r.get<Comp::SoilFeederComponent>(e).absorbed;
        EXPECT_GE(a, prev);
        prev = a;
    }
    EXPECT_GT(prev, 0u);
}

} // namespace
