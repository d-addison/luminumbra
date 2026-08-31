// game/CodexView: the pure presentation model the codex browse screen renders.
// Pins the content contract: every registered species appears (locked until discovered),
// discovered rows carry the best-shot star rating + capture count, rows are id-sorted, and
// the completion fraction tracks discovered/total. No GL, no rng.
#include <gtest/gtest.h>

#include "ai/CreatureSpeciesRegistry.h"
#include "components/CreatureComponents.h"
#include "game/CodexView.h"
#include "game/PhotoCodex.h"

namespace {

using luminumbra::ai::CreatureSpeciesRegistry;
using luminumbra::game::BuildCodexView;
using luminumbra::game::CodexView;
using luminumbra::game::PhotoCodex;
namespace Components = Luminumbra::Components;

CreatureSpeciesRegistry MakeRegistry() {
    CreatureSpeciesRegistry reg;
    std::string err;
    reg.AddFromJsonText(R"({"id":"grovestrider","display_name":"Grovestrider","rarity":0.35})",
                        err);
    reg.AddFromJsonText(R"({"id":"lumen_moth","display_name":"Lumen Moth","rarity":0.85})", err);
    reg.AddFromJsonText(R"({"id":"tide_grazer","display_name":"Tide Grazer"})", err);
    return reg;
}

TEST(CodexView, EmptyCodexShowsAllSpeciesLocked) {
    const CreatureSpeciesRegistry reg = MakeRegistry();
    PhotoCodex codex; // nothing discovered
    const CodexView view = BuildCodexView(reg, codex);

    EXPECT_EQ(view.total_species, 3u);
    EXPECT_EQ(view.discovered_count, 0u);
    EXPECT_FLOAT_EQ(view.completeness, 0.0f);
    ASSERT_EQ(view.rows.size(), 3u);
    for (const auto& row : view.rows) {
        EXPECT_FALSE(row.discovered);
        EXPECT_EQ(row.stars, 0);
        EXPECT_EQ(row.captures, 0u);
        EXPECT_FALSE(row.display_name.empty());
    }
}

TEST(CodexView, DiscoveredRowsCarryStarsAndCounts) {
    const CreatureSpeciesRegistry reg = MakeRegistry();
    const std::uint16_t grove = Components::CreatureSpeciesId16("grovestrider");

    PhotoCodex codex;
    codex.Record(static_cast<int>(grove), 0.40f); // a 2-star-ish shot
    codex.Record(static_cast<int>(grove), 0.90f); // a better 5-star shot; codex keeps best

    const CodexView view = BuildCodexView(reg, codex);
    EXPECT_EQ(view.discovered_count, 1u);
    EXPECT_NEAR(view.completeness, 1.0f / 3.0f, 1e-6f);

    bool found = false;
    for (const auto& row : view.rows) {
        if (row.species_id == grove) {
            found = true;
            EXPECT_TRUE(row.discovered);
            EXPECT_EQ(row.captures, 2u);
            EXPECT_GE(row.stars, 4); // best shot 0.90 -> 5 stars
        }
    }
    EXPECT_TRUE(found);
}

TEST(CodexView, RowsAreSortedBySpeciesId) {
    const CreatureSpeciesRegistry reg = MakeRegistry();
    PhotoCodex codex;
    const CodexView view = BuildCodexView(reg, codex);
    for (std::size_t i = 1; i < view.rows.size(); ++i) {
        EXPECT_LT(view.rows[i - 1].species_id, view.rows[i].species_id);
    }
}

} // namespace
