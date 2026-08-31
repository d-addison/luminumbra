// game/Objectives: the progression layer over the codex. Pins the pure
// evaluation contract the HUD/journal reads: each objective is a deterministic function
// of codex state, progress is monotonic toward completion, and the starter set advances
// as captures accrue. No GL, no rng, no wall-clock.
#include <gtest/gtest.h>

#include "game/Objectives.h"
#include "game/PhotoCodex.h"

namespace {

using luminumbra::game::DefaultObjectives;
using luminumbra::game::EvaluateObjective;
using luminumbra::game::Objective;
using luminumbra::game::ObjectiveKind;
using luminumbra::game::ObjectiveSet;
using luminumbra::game::PhotoCodex;

// kStar4 is 0.75 (PhotoSession): a best_score >= 0.75 is a 4-star shot.
TEST(Objectives, DiscoverCountProgressAndCompletion) {
    Objective o;
    o.kind = ObjectiveKind::DiscoverCount;
    o.target_count = 3;
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
    Objective o;
    o.kind = ObjectiveKind::DiscoverSpecies;
    o.species_id = 77;
    PhotoCodex codex;
    EXPECT_FALSE(EvaluateObjective(o, codex).complete);
    codex.Record(77, 0.1f);
    EXPECT_TRUE(EvaluateObjective(o, codex).complete);
    EXPECT_FLOAT_EQ(EvaluateObjective(o, codex).progress, 1.0f);
}

TEST(Objectives, StarRatingNeedsAGoodEnoughBestShot) {
    Objective o;
    o.kind = ObjectiveKind::StarRating;
    o.species_id = 5;
    o.min_stars = 4;
    PhotoCodex codex;
    // A weak shot discovers the species but does not satisfy the star goal.
    codex.Record(5, 0.30f); // ~1 star
    EXPECT_FALSE(EvaluateObjective(o, codex).complete);
    // A strong shot (>=0.75 total -> 4 stars) completes it; codex keeps the best.
    codex.Record(5, 0.80f);
    EXPECT_TRUE(EvaluateObjective(o, codex).complete);
    EXPECT_FLOAT_EQ(EvaluateObjective(o, codex).progress, 1.0f);
}

TEST(Objectives, CollectionScoreAccumulates) {
    Objective o;
    o.kind = ObjectiveKind::CollectionScore;
    o.min_score = 1.5f;
    PhotoCodex codex;
    codex.Record(1, 0.6f);
    codex.Record(2, 0.6f);
    EXPECT_FALSE(EvaluateObjective(o, codex).complete); // 1.2 < 1.5
    codex.Record(3, 0.6f);
    EXPECT_TRUE(EvaluateObjective(o, codex).complete); // 1.8 >= 1.5
}

TEST(Objectives, StarterSetAdvancesAndReportsNextIncomplete) {
    const int grove = 1234;
    ObjectiveSet set = DefaultObjectives(grove);
    ASSERT_EQ(set.size(), 6u);

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

    // Discover six species, each a strong shot -> every species/score goal completes,
    // but the behavioural goal still pends (none captured asleep yet).
    for (int i = 0; i < 6; ++i)
        codex.Record(grove + 100 + i, 0.90f);
    EXPECT_FALSE(set.all_complete(codex));
    const Objective* pending = set.next_incomplete(codex);
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(pending->kind, ObjectiveKind::BehavioralMatch);

    // Photograph a sleeping creature (brain action 5 == Sleep) -> the set completes.
    codex.Record(grove, 0.90f, /*subject_action=*/5);
    EXPECT_TRUE(set.all_complete(codex));
    EXPECT_EQ(set.next_incomplete(codex), nullptr);
}

// BehavioralMatch is binary on whether the target behaviour was ever captured. With
// species_id 0 it matches ANY species; with a species it matches only that one.
TEST(Objectives, BehavioralMatchAnyAndSpeciesScoped) {
    PhotoCodex codex;

    Objective any_sleep;
    any_sleep.kind = ObjectiveKind::BehavioralMatch;
    any_sleep.target_action = 5;
    any_sleep.species_id = 0; // any species asleep
    Objective grove_sleep;
    grove_sleep.kind = ObjectiveKind::BehavioralMatch;
    grove_sleep.target_action = 5;
    grove_sleep.species_id = 1234; // the grovestrider asleep

    EXPECT_FALSE(EvaluateObjective(any_sleep, codex).complete);
    EXPECT_FALSE(EvaluateObjective(grove_sleep, codex).complete);

    // A plain (no-behaviour) capture does not satisfy a behavioural goal.
    codex.Record(1234, 0.8f);
    EXPECT_FALSE(EvaluateObjective(any_sleep, codex).complete);

    // Capture a DIFFERENT species asleep -> the any-species goal completes, the
    // grovestrider-scoped one does not.
    codex.Record(99, 0.5f, /*subject_action=*/5);
    EXPECT_TRUE(EvaluateObjective(any_sleep, codex).complete);
    EXPECT_FALSE(EvaluateObjective(grove_sleep, codex).complete);

    // Capture the grovestrider asleep -> the scoped goal completes too.
    codex.Record(1234, 0.6f, /*subject_action=*/5);
    EXPECT_TRUE(EvaluateObjective(grove_sleep, codex).complete);
}

} // namespace
