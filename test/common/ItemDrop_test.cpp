#include "luminumbra_common/game/ItemDrop.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include <climits>
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
using namespace luminumbra::game;
TEST(ItemDropTest, EnabledHarvestKeepsAuthoritativeSeedsWithinPersistenceRange) {
    entt::registry registry;
    const auto plant = registry.create();
    auto& growth = registry.emplace<Luminumbra::Components::PlantGrowthComponent>(plant);
    growth.stage = static_cast<std::uint8_t>(Luminumbra::Components::PlantStage::Mature);
    growth.quality = 100;
    registry.emplace<Luminumbra::Components::PlantGenomeComponent>(plant);
    luminumbra::foliage::FarmingController farming;
    farming.seeds = INT_MAX;
    const auto harvest = farming.Harvest(registry, plant, true);
    ASSERT_TRUE(harvest.harvestable);
    EXPECT_GT(harvest.seeds, 0);
    EXPECT_EQ(farming.seeds, INT_MAX);
    EXPECT_EQ(farming.harvests, 1);
}
TEST(ItemDropTest, HeldPressAndCapturedOrRefusedPressRequireReleaseBeforeRetry) {
    FarmDropSeedEdge edge;
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, false, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, HeldThroughGameplayEntryRequiresReleaseThenGameplayPress) {
    FarmDropSeedEdge edge;
    // Loading, menus and other noninteractive states cannot initiate a drop.
    EXPECT_FALSE(FarmDropSeedPressed(false, false, false, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, false, false, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, false, false, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    // World entry resets interaction state; even without a loading sample a hold is inert.
    edge = {};
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, EligibleReleaseThenPressFiresExactlyOnce) {
    FarmDropSeedEdge edge;
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
    for (int frame = 0; frame < 20; ++frame)
        EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, PressWhilePausedThenUnpausedInSameBatchDoesNotFire) {
    FarmDropSeedEdge edge;
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, false, edge)); // Paused frame, key up.
    // The next event batch presses B while paused, then Escape unpauses. The frame sampler
    // sees only the final key-down/unpaused state; the callback also invalidates the interval.
    edge.Invalidate();
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, PressWhileSettingsOpenThenClosedDoesNotFire) {
    FarmDropSeedEdge edge;
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, false, edge)); // Settings open, key up.
    edge.Invalidate(); // B press followed by closing settings in the same event batch.
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, ReleaseAndPressAcrossFocusLossAndRegainRequireEligibleRelease) {
    FarmDropSeedEdge edge;
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, false, edge)); // Released while unfocused.
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));   // Regain focus with key down.
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    // Both focus callbacks occur between eligible frame samples; release/press in that
    // interval cannot use the last focused release to arm a drop.
    edge.Invalidate(); // Focus lost.
    edge.Invalidate(); // Focus regained in the same batch.
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, FirstEligibleReleaseOnlyRecordsStateAfterIneligibleFrame) {
    FarmDropSeedEdge edge;
    EXPECT_FALSE(FarmDropSeedPressed(false, true, false, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, false, edge));
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, OverlayOpenedAndClosedBetweenSamplesInvalidatesEligibleRelease) {
    FarmDropSeedEdge edge;
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    edge.Invalidate(); // Open settings/pause/UI.
    edge.Invalidate(); // Close again before the next frame sample.
    EXPECT_FALSE(FarmDropSeedPressed(true, true, true, edge));
    EXPECT_FALSE(FarmDropSeedPressed(false, true, true, edge));
    EXPECT_TRUE(FarmDropSeedPressed(true, true, true, edge));
}
TEST(ItemDropTest, PositionUsesFeetHorizontalForwardAndClearanceTransactionally) {
    Luminumbra::Vec3 out{1, 2, 3};
    ASSERT_TRUE(FarmingSeedDropPosition({10, 20, 30}, {3, 999, 4}, out));
    EXPECT_FLOAT_EQ(out.x, 10.6f);
    EXPECT_FLOAT_EQ(out.y, 20.25f);
    EXPECT_FLOAT_EQ(out.z, 30.8f);
    const auto before = out;
    EXPECT_FALSE(FarmingSeedDropPosition({10, 20, 30}, {0, 1, 0}, out));
    EXPECT_FALSE(
        FarmingSeedDropPosition({10, 20, 30}, {1, std::numeric_limits<float>::infinity(), 0}, out));
    EXPECT_FALSE(
        FarmingSeedDropPosition({std::numeric_limits<float>::quiet_NaN(), 20, 30}, {1, 0, 0}, out));
    EXPECT_EQ(out, before);
}
TEST(ItemDropTest, InventoryStrictnessAndLegacyDefaultAdoption) {
    int seeds = 5;
    std::string error;
    ASSERT_TRUE(ParseFarmingInventory("{}", seeds, error));
    EXPECT_EQ(seeds, 5);
    for (const int count : {0, 4, INT_MAX}) {
        const nlohmann::json metadata{{"farmingInventory", {{"version", 1}, {"seeds", count}}}};
        ASSERT_TRUE(ParseFarmingInventory(metadata.dump(), seeds, error)) << error;
        EXPECT_EQ(seeds, count);
    }
    const int before = seeds;
    for (const char* metadata : {"{\"farmingInventory\":null}",
                                 "{\"farmingInventory\":{}}",
                                 "{\"farmingInventory\":{\"version\":1}}",
                                 "{\"farmingInventory\":{\"version\":2,\"seeds\":4}}",
                                 "{\"farmingInventory\":{\"version\":true,\"seeds\":4}}",
                                 "{\"farmingInventory\":{\"version\":1.0,\"seeds\":4}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":-1}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":2147483648}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":true}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":4.0}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":4e0}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":\"4\"}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":null}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":4,\"extra\":0}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":4,\"seeds\":3}}",
                                 "{\"farmingInventory\":{\"version\":1,\"seeds\":4},"
                                 "\"farmingInventory\":{\"version\":1,\"seeds\":3}}"}) {
        EXPECT_FALSE(ParseFarmingInventory(metadata, seeds, error)) << metadata;
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(seeds, before);
    }
}
} // namespace
