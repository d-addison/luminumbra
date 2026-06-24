// Phase 1 — game/Objectives: the progression layer over the codex. Pins the pure
// evaluation contract the HUD/journal reads: each objective is a deterministic function
// of codex state, progress is monotonic toward completion, and the starter set advances
// as captures accrue. No GL, no rng, no wall-clock.
#include <gtest/gtest.h>

#include "game/Objectives.h"
#include "game/PhotoCodex.h"

namespace {

using luminumbra::game::Objective;
using luminumbra::game::ObjectiveKind;
using luminumbra::game::ObjectiveSet;
using luminumbra::game::EvaluateObjective;
using luminumbra::game::DefaultObjectives;
using luminumbra::game::PhotoCodex;

// kStar4 is 0.75 (PhotoSession): a best_score >= 0.75 is a 4-star shot.
TEST(Objectives, DiscoverCountProgressAndCompletion) {
    Objective o; o.kind = ObjectiveKind::DiscoverCount; o.target_count = 3;
    PhotoCodex codex;

    EXPECT_FALSE(EvaluateObjective(o, codex).complete);
    EXPECT_FLOAT_EQ(EvaluateObjective(o, codex).progress, 0.0f);

    codex.Record(10, 0.5f);
    EXPECT_FLOAT_EQ(EvaluateObjective(o, codex).progress, 1.0f / 3.0f);
    codex.Record(20, 0.5f);
    codex.Record(30, 0.5f);
    const auto s = EvaluateObjective(o, codex);
    EXPECT_TRUE(s.complete);
    EXPECT_FLOAT_EQ(s.progress, 1.0f);
    // Further discoveries keep it complete and clamped at 1.
    codex.Record(40, 0.5f);
    EXPECT_TRUE(EvaluateObjective(o, codex).complete);
    EXPECT_FLOAT_EQ(EvaluateObjective(o, codex).progress, 1.0f);
}

TEST(Objectives, DiscoverSpeciesIsBinary) {
    Objective o; o.kind = ObjectiveKind::DiscoverSpecies; o.species_id = 77;
    PhotoCodex codex;
    EXPECT_FALSE(EvaluateObjective(o, codex).complete);
    codex.Record(77, 0.1f);
    EXPECT_TRUE(EvaluateObjective(o, codex).complete);
    EXPECT_FLOAT_EQ(EvaluateObjective(o, codex).progress, 1.0f);
}

TEST(Objectives, StarRatingNeedsAGoodEnoughBestShot) {
    Objective o; o.kind = ObjectiveKind::StarRating; o.species_id = 5; o.min_stars = 4;
    PhotoCodex codex;
    // A weak shot discovers the species but does not satisfy the star goal.
    codex.Record(5, 0.30f);  // ~1 star
    EXPECT_FALSE(EvaluateObjective(o, codex).complete);
    // A strong shot (>=0.75 total -> 4 stars) completes it; codex keeps the best.
    codex.Record(5, 0.80f);
    EXPECT_TRUE(EvaluateObjective(o, codex).complete);
    EXPECT_FLOAT_EQ(EvaluateObjective(o, codex).progress, 1.0f);
}

TEST(Objectives, CollectionScoreAccumulates) {
    Objective o; o.kind = ObjectiveKind::CollectionScore; o.min_score = 1.5f;
    PhotoCodex codex;
    codex.Record(1, 0.6f);
    codex.Record(2, 0.6f);
    EXPECT_FALSE(EvaluateObjective(o, codex).complete);  // 1.2 < 1.5
    codex.Record(3, 0.6f);
    EXPECT_TRUE(EvaluateObjective(o, codex).complete);   // 1.8 >= 1.5
}

TEST(Objectives, StarterSetAdvancesAndReportsNextIncomplete) {
    const int grove = 1234;
    ObjectiveSet set = DefaultObjectives(grove);
    ASSERT_EQ(set.size(), 5u);

    PhotoCodex codex;
    // Nothing done: the first incomplete is "photograph your first creature".
    const Objective* next0 = set.next_incomplete(codex);
    ASSERT_NE(next0, nullptr);
    EXPECT_EQ(next0->kind, ObjectiveKind::DiscoverSpecies);
    EXPECT_EQ(set.completed_count(codex), 0u);
    EXPECT_FALSE(set.all_complete(codex));

    // Photograph the grovestrider well -> first + the 4-star goals complete.
    codex.Record(grove, 0.90f);
    EXPECT_GE(set.completed_count(codex), 2u);

    // Discover six species, each a strong shot -> every starter objective completes.
    for (int i = 0; i < 6; ++i) codex.Record(grove + 100 + i, 0.90f);
    EXPECT_TRUE(set.all_complete(codex));
    EXPECT_EQ(set.next_incomplete(codex), nullptr);
}

}  // namespace
