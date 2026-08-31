// game.codex_entry: the per-SPECIES metadata + rarity layer of the photography
// codex (photography), pairing with PhotoCodex. These tests pin the catalogue model:
// Register + Find round-trip; Find of an unknown id is nullptr; rarer species yield a
// HIGHER RarityWeight and an unknown id yields 0; size counts DISTINCT species;
// all is always species_id-sorted (deterministic, order-independent); re-registering
// a species id UPDATES it in place (defined behaviour, not a duplicate); and the same
// set of registrations in any order yields a bit-identical registry (run==replay).
// PURE: no entt, no rng, no wall-clock — SpeciesRegistry operates on plain value structs.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "game/SpeciesCodex.h"

namespace {

using luminumbra::game::kRarityWeightSlope;
using luminumbra::game::MakeDefaultSpeciesRegistry;
using luminumbra::game::SpeciesInfo;
using luminumbra::game::SpeciesRegistry;

// ---- gating / defined edge case ----

// A fresh registry is a defined ZERO state: no species, empty all, Find -> nullptr,
// RarityWeight -> 0.
TEST(SpeciesCodex, EmptyRegistryIsDefinedZeroState) {
    SpeciesRegistry reg;
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_TRUE(reg.all().empty());
    EXPECT_EQ(reg.Find(1), nullptr);
    EXPECT_FLOAT_EQ(reg.RarityWeight(1), 0.0f);
}

// ---- register + find round-trip ----

// Registering a species then Finding it returns the stored metadata verbatim.
TEST(SpeciesCodex, RegisterFindRoundTrip) {
    SpeciesRegistry reg;
    reg.Register(SpeciesInfo{7, "Dusk Owl", 2, 1, /*nocturnal*/ true});

    const SpeciesInfo* info = reg.Find(7);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->species_id, 7);
    EXPECT_STREQ(info->name, "Dusk Owl");
    EXPECT_EQ(info->rarity, 2u);
    EXPECT_EQ(info->habitat, 1u);
    EXPECT_TRUE(info->nocturnal);
    EXPECT_EQ(reg.size(), 1u);
}

// Find of an unknown species id is nullptr (even with other species present).
TEST(SpeciesCodex, FindUnknownIsNull) {
    SpeciesRegistry reg;
    reg.Register(SpeciesInfo{1, "A", 0, 0, false});
    reg.Register(SpeciesInfo{5, "B", 1, 0, false});
    EXPECT_EQ(reg.Find(3), nullptr);
    EXPECT_EQ(reg.Find(99), nullptr);
    EXPECT_NE(reg.Find(1), nullptr);
    EXPECT_NE(reg.Find(5), nullptr);
}

// ---- rarity weight ----

// RarityWeight: common (rarity 0) = baseline 1.0; each rarity tier adds a fixed
// increment so a rarer species is worth MORE. Unknown -> 0.
TEST(SpeciesCodex, RarityWeightRarerIsHigherUnknownZero) {
    SpeciesRegistry reg;
    reg.Register(SpeciesInfo{1, "Common", 0, 0, false});
    reg.Register(SpeciesInfo{2, "Uncommon", 1, 0, false});
    reg.Register(SpeciesInfo{3, "Rare", 4, 0, false});

    const float w_common = reg.RarityWeight(1);
    const float w_uncommon = reg.RarityWeight(2);
    const float w_rare = reg.RarityWeight(3);

    EXPECT_FLOAT_EQ(w_common, 1.0f);
    EXPECT_FLOAT_EQ(w_uncommon, 1.0f + 1.0f * kRarityWeightSlope);
    EXPECT_FLOAT_EQ(w_rare, 1.0f + 4.0f * kRarityWeightSlope);

    // Strictly increasing with rarity.
    EXPECT_GT(w_uncommon, w_common);
    EXPECT_GT(w_rare, w_uncommon);

    // Unknown species contributes no weight.
    EXPECT_FLOAT_EQ(reg.RarityWeight(42), 0.0f);
}

// ---- distinct count + re-register updates ----

// size counts DISTINCT species ids, and re-registering an existing id UPDATES it in
// place rather than adding a duplicate (defined behaviour).
TEST(SpeciesCodex, ReRegisterUpdatesInPlace) {
    SpeciesRegistry reg;
    reg.Register(SpeciesInfo{4, "Old Name", 1, 0, false});
    reg.Register(SpeciesInfo{4, "New Name", 3, 2, true}); // same id -> update

    EXPECT_EQ(reg.size(), 1u); // still one distinct species
    const SpeciesInfo* info = reg.Find(4);
    ASSERT_NE(info, nullptr);
    EXPECT_STREQ(info->name, "New Name");
    EXPECT_EQ(info->rarity, 3u);
    EXPECT_EQ(info->habitat, 2u);
    EXPECT_TRUE(info->nocturnal);
    // Weight reflects the updated rarity.
    EXPECT_FLOAT_EQ(reg.RarityWeight(4), 1.0f + 3.0f * kRarityWeightSlope);
}

// ---- deterministic ordering ----

// all is ALWAYS sorted ascending by species_id, regardless of registration order.
TEST(SpeciesCodex, AllIsSpeciesIdSorted) {
    SpeciesRegistry reg;
    reg.Register(SpeciesInfo{9, "i", 0, 0, false});
    reg.Register(SpeciesInfo{2, "ii", 0, 0, false});
    reg.Register(SpeciesInfo{5, "iii", 0, 0, false});
    reg.Register(SpeciesInfo{1, "iv", 0, 0, false});
    reg.Register(SpeciesInfo{2, "ii-upd", 1, 0, false}); // duplicate id -> update, no reorder

    const auto& es = reg.all();
    ASSERT_EQ(es.size(), 4u);
    EXPECT_EQ(es[0].species_id, 1);
    EXPECT_EQ(es[1].species_id, 2);
    EXPECT_EQ(es[2].species_id, 5);
    EXPECT_EQ(es[3].species_id, 9);
    for (std::size_t i = 1; i < es.size(); ++i) {
        EXPECT_LT(es[i - 1].species_id, es[i].species_id);
    }
}

// The default registry builder is itself species_id-sorted and well-formed.
TEST(SpeciesCodex, DefaultRegistryIsSortedAndComplete) {
    SpeciesRegistry reg = MakeDefaultSpeciesRegistry();
    const auto& es = reg.all();
    ASSERT_FALSE(es.empty());
    for (std::size_t i = 1; i < es.size(); ++i) {
        EXPECT_LT(es[i - 1].species_id, es[i].species_id);
    }
    // Every catalogued id is findable and yields a weight >= baseline.
    for (const auto& s : es) {
        EXPECT_EQ(reg.Find(s.species_id), &s);
        EXPECT_GE(reg.RarityWeight(s.species_id), 1.0f);
    }
}

// ---- determinism: run == replay ----

// Registering the SAME set of species in DIFFERENT orders yields a BIT-IDENTICAL
// registry (same all vector, same weights). run==replay: the catalogue is
// order-independent and reproducible.
TEST(SpeciesCodex, RunEqualsReplayOrderIndependent) {
    const SpeciesInfo forward[] = {
        {3, "c", 2, 1, true},
        {7, "g", 4, 2, false},
        {1, "a", 0, 0, false},
        {5, "e", 1, 3, true},
        {2, "b", 3, 0, false},
    };
    const SpeciesInfo shuffled[] = {
        {5, "e", 1, 3, true},
        {2, "b", 3, 0, false},
        {7, "g", 4, 2, false},
        {1, "a", 0, 0, false},
        {3, "c", 2, 1, true},
    };

    SpeciesRegistry a;
    for (const auto& s : forward)
        a.Register(s);
    SpeciesRegistry b;
    for (const auto& s : shuffled)
        b.Register(s);

    ASSERT_EQ(a.size(), b.size());
    ASSERT_EQ(a.all().size(), b.all().size());
    for (std::size_t i = 0; i < a.all().size(); ++i) {
        EXPECT_EQ(a.all()[i].species_id, b.all()[i].species_id);
        EXPECT_EQ(a.all()[i].rarity, b.all()[i].rarity);
        EXPECT_EQ(a.all()[i].habitat, b.all()[i].habitat);
        EXPECT_EQ(a.all()[i].nocturnal, b.all()[i].nocturnal);
        EXPECT_STREQ(a.all()[i].name, b.all()[i].name);
        // Exact bit equality (==), not approximate: determinism, not tolerance.
        EXPECT_EQ(a.RarityWeight(a.all()[i].species_id), b.RarityWeight(b.all()[i].species_id));
    }
    // And the catalogue is correct: 5 distinct, ascending, rare species weigh more.
    EXPECT_EQ(a.size(), 5u);
    EXPECT_EQ(a.all()[0].species_id, 1);
    EXPECT_EQ(a.all()[4].species_id, 7);
    EXPECT_GT(a.RarityWeight(7), a.RarityWeight(1)); // rarity 4 > rarity 0
}

} // namespace
