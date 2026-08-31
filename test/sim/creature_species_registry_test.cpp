// CreatureSpeciesRegistry: data-driven species names + metadata that the
// codex/HUD resolve a captured species_id against. Pins the contract the discovery
// notification + codex UI rely on: a loaded species resolves its id-16 (the same FNV key
// the spawners stamp) to a display name, and an unregistered id gets a stable fallback
// (never a blank). Pure: JSON text in, no filesystem, no world_hash.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

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

// Content: the live creatures/species directory loads cleanly and includes the shipped roster
// (6 originals + 4 added) so the living-world ambient spawn + the photography codex have variety.
// Guards that the data files parse and carry the right predator/prey role.
TEST(CreatureSpeciesRegistry, LoadsShippedSpeciesFromDirectory) {
    CreatureSpeciesRegistry reg;
    std::vector<std::string> errors;
    const std::filesystem::path dir =
        std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "creatures" / "species";
    const std::size_t n = reg.LoadFromDirectory(dir, errors);
    EXPECT_GE(n, 10u) << (errors.empty() ? "" : errors.front()); // 6 original + 4 added
    struct Expect {
        const char* id;
        bool predator;
    };
    const Expect added[] = {{"dusk_heron", false},
                            {"ember_skink", false},
                            {"gloomstalker", true},
                            {"glimmer_finch", false}};
    for (const Expect& e : added) {
        const auto* s = reg.Find(Components::CreatureSpeciesId16(e.id));
        ASSERT_NE(s, nullptr) << e.id << " missing from the shipped roster";
        EXPECT_EQ(s->predator, e.predator) << e.id;
    }
}

TEST(CreatureSpeciesRegistry, DisplayNameFallsBackForUnregisteredId) {
    CreatureSpeciesRegistry reg; // empty
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

TEST(CreatureSpeciesRegistry, BiomeAssociationFiltersAndSelects) {
    CreatureSpeciesRegistry reg;
    std::string err;
    // Two wetland species, one alpine predator, one generalist (no biomes).
    reg.AddFromJsonText(R"({"id":"grovestrider","biomes":["wetland","plains"]})", err);
    reg.AddFromJsonText(R"({"id":"mire_lurker","biomes":["wetland"]})", err);
    reg.AddFromJsonText(R"({"id":"ridgeback_stalker","biomes":["alpine"]})", err);
    reg.AddFromJsonText(R"({"id":"drifter"})", err); // generalist: lives everywhere

    // Wetland: the two wetland dwellers + the generalist (NOT the alpine predator).
    const auto wet = reg.SpeciesInBiome("wetland");
    EXPECT_EQ(wet.size(), 3u);
    for (const auto* s : wet)
        EXPECT_NE(s->id, "ridgeback_stalker");

    // Alpine: the alpine predator + the generalist only.
    const auto alp = reg.SpeciesInBiome("alpine");
    EXPECT_EQ(alp.size(), 2u);

    // Deterministic pick: same (biome, index) -> same species; index wraps modulo.
    const auto* a = reg.SelectForBiome("wetland", 0);
    const auto* b = reg.SelectForBiome("wetland", 0);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b);
    EXPECT_EQ(reg.SelectForBiome("wetland", 3), reg.SelectForBiome("wetland", 0)); // wraps (3 % 3)
    // A biome no one lists but the generalist still covers -> generalist returned.
    const auto* d = reg.SelectForBiome("tundra", 0);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->id, "drifter");
}

} // namespace
