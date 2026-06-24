// Phase 1/2 — CreatureSpeciesRegistry: data-driven species names + metadata that the
// codex/HUD resolve a captured species_id against. Pins the contract the discovery
// notification + codex UI rely on: a loaded species resolves its id-16 (the same FNV key
// the spawners stamp) to a display name, and an unregistered id gets a stable fallback
// (never a blank). Pure: JSON text in, no filesystem, no world_hash.
#include <gtest/gtest.h>

#include <string>

#include "ai/CreatureSpeciesRegistry.h"
#include "components/CreatureComponents.h"

namespace {

using luminumbra::ai::CreatureSpeciesRegistry;
namespace Components = Luminumbra::Components;

const char* kGrovestrider = R"({
  "id": "grovestrider", "display_name": "Grovestrider",
  "predator": false, "rarity": 0.35, "base_color": [0.36, 0.42, 0.30]
})";

const char* kStalker = R"({
  "id": "ridgeback_stalker", "display_name": "Ridgeback Stalker", "predator": true
})";

TEST(CreatureSpeciesRegistry, LoadsAndResolvesIdToDisplayNameAndMetadata) {
    CreatureSpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(kGrovestrider, err)) << err;
    ASSERT_TRUE(reg.AddFromJsonText(kStalker, err)) << err;
    EXPECT_EQ(reg.size(), 2u);

    // The registry's id-16 matches the value the spawn sites stamp onto creatures.
    const std::uint16_t grove_id = Components::CreatureSpeciesId16("grovestrider");
    const auto* grove = reg.Find(grove_id);
    ASSERT_NE(grove, nullptr);
    EXPECT_EQ(grove->display_name, "Grovestrider");
    EXPECT_FALSE(grove->predator);
    EXPECT_FLOAT_EQ(grove->rarity, 0.35f);
    EXPECT_FLOAT_EQ(grove->base_color[1], 0.42f);

    const auto* stalker = reg.Find(Components::CreatureSpeciesId16("ridgeback_stalker"));
    ASSERT_NE(stalker, nullptr);
    EXPECT_TRUE(stalker->predator);
    // Unspecified rarity defaults to mid.
    EXPECT_FLOAT_EQ(stalker->rarity, 0.5f);
}

TEST(CreatureSpeciesRegistry, DisplayNameFallsBackForUnregisteredId) {
    CreatureSpeciesRegistry reg;  // empty
    const std::uint16_t unknown = Components::CreatureSpeciesId16("nobody");
    // Never blank — a stable, human-readable fallback.
    const std::string name = reg.DisplayName(unknown);
    EXPECT_FALSE(name.empty());
    EXPECT_NE(name.find("Species #"), std::string::npos);
    EXPECT_EQ(reg.Find(unknown), nullptr);
}

TEST(CreatureSpeciesRegistry, RejectsSpeciesWithoutId) {
    CreatureSpeciesRegistry reg;
    std::string err;
    EXPECT_FALSE(reg.AddFromJsonText(R"({"display_name": "No Id"})", err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(reg.size(), 0u);
}

}  // namespace
