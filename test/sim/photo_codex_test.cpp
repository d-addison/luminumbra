// game.photo_codex: the creature/subject CODEX (photography progression on top
// of PhotoScoring). These tests pin the collection model: recording a new species
// DISCOVERS it (captures=1); recording it again increments captures and keeps the
// BEST score (a worse later shot never lowers the record); species_count counts
// DISTINCT species; completeness = discovered/total clamped to [0,1]; entries are
// always species_id-sorted (deterministic, order-independent); total_score sums the
// best scores; and a fresh codex is a defined zero state. PURE: no entt, no rng, no
// wall-clock — PhotoCodex operates on plain value structs.
#include <gtest/gtest.h>

#include <vector>

#include "game/PhotoCodex.h"

namespace {

using luminumbra::game::CodexEntry;
using luminumbra::game::PhotoCodex;

// ---- gating / defined edge case ----

// A fresh codex is a defined ZERO state: no species, zero aggregate score, zero
// completeness, and an empty entries list.
TEST(PhotoCodex, EmptyCodexIsDefinedZeroState) {
    PhotoCodex codex;
    EXPECT_EQ(codex.species_count(), 0u);
    EXPECT_FLOAT_EQ(codex.total_score(), 0.0f);
    EXPECT_FLOAT_EQ(codex.completeness(10), 0.0f);
    EXPECT_TRUE(codex.entries().empty());
    EXPECT_FALSE(codex.discovered(1));
}

// ---- discovery ----

// Recording a never-seen species DISCOVERS it: captures=1, best_score=the score,
// and discovered flips true.
TEST(PhotoCodex, RecordingNewSpeciesDiscoversIt) {
    PhotoCodex codex;
    EXPECT_FALSE(codex.discovered(7));
    codex.Record(7, 0.6f);
    EXPECT_TRUE(codex.discovered(7));
    EXPECT_EQ(codex.species_count(), 1u);

    ASSERT_EQ(codex.entries().size(), 1u);
    EXPECT_EQ(codex.entries()[0].species_id, 7);
    EXPECT_EQ(codex.entries()[0].captures, 1u);
    EXPECT_FLOAT_EQ(codex.entries()[0].best_score, 0.6f);
}

// ---- repeat capture: count + best score ----

// Recording the same species again increments captures and keeps the BEST score
// when the new shot is BETTER.
TEST(PhotoCodex, RepeatCaptureKeepsBetterScoreAndCounts) {
    PhotoCodex codex;
    codex.Record(3, 0.4f);
    codex.Record(3, 0.8f); // better shot
    EXPECT_EQ(codex.species_count(), 1u);
    ASSERT_EQ(codex.entries().size(), 1u);
    EXPECT_EQ(codex.entries()[0].captures, 2u);
    EXPECT_FLOAT_EQ(codex.entries()[0].best_score, 0.8f);
}

// Recording a WORSE later shot still counts the capture but does NOT lower the
// recorded best score (keeps the best, not the latest).
TEST(PhotoCodex, RepeatCaptureKeepsBestNotLatest) {
    PhotoCodex codex;
    codex.Record(3, 0.8f);
    codex.Record(3, 0.2f); // worse shot — must not overwrite the record
    EXPECT_EQ(codex.species_count(), 1u);
    ASSERT_EQ(codex.entries().size(), 1u);
    EXPECT_EQ(codex.entries()[0].captures, 2u);
    EXPECT_FLOAT_EQ(codex.entries()[0].best_score, 0.8f);
}

// ---- distinct species count ----

// species_count counts DISTINCT species, not total captures.
TEST(PhotoCodex, SpeciesCountCountsDistinct) {
    PhotoCodex codex;
    codex.Record(1, 0.5f);
    codex.Record(1, 0.6f); // same species again
    codex.Record(2, 0.5f);
    codex.Record(9, 0.5f);
    EXPECT_EQ(codex.species_count(), 3u);
}

// ---- completeness ----

// completeness = discovered / total, clamped to [0,1].
TEST(PhotoCodex, CompletenessIsDiscoveredOverTotalClamped) {
    PhotoCodex codex;
    codex.Record(1, 0.5f);
    codex.Record(2, 0.5f);
    codex.Record(3, 0.5f);
    EXPECT_FLOAT_EQ(codex.completeness(12), 3.0f / 12.0f);

    // More discovered than the supposed total clamps at 1.0 (never overflows).
    EXPECT_FLOAT_EQ(codex.completeness(2), 1.0f);

    // A non-positive total is the defined zero (no divide-by-zero).
    EXPECT_FLOAT_EQ(codex.completeness(0), 0.0f);
    EXPECT_FLOAT_EQ(codex.completeness(-5), 0.0f);
}

// ---- total score ----

// total_score sums the BEST score across distinct species (repeats fold into the
// best, they do not double-count).
TEST(PhotoCodex, TotalScoreSumsBestScores) {
    PhotoCodex codex;
    codex.Record(1, 0.4f);
    codex.Record(1, 0.9f); // best for species 1 -> 0.9
    codex.Record(2, 0.5f); // best for species 2 -> 0.5
    EXPECT_FLOAT_EQ(codex.total_score(), 0.9f + 0.5f);
}

// ---- deterministic ordering ----

// entries is ALWAYS sorted ascending by species_id, regardless of the order in
// which species were recorded.
TEST(PhotoCodex, EntriesAreSpeciesIdSorted) {
    PhotoCodex codex;
    codex.Record(9, 0.5f);
    codex.Record(2, 0.5f);
    codex.Record(5, 0.5f);
    codex.Record(1, 0.5f);
    codex.Record(2, 0.6f); // duplicate — must not disturb ordering

    const auto& es = codex.entries();
    ASSERT_EQ(es.size(), 4u);
    EXPECT_EQ(es[0].species_id, 1);
    EXPECT_EQ(es[1].species_id, 2);
    EXPECT_EQ(es[2].species_id, 5);
    EXPECT_EQ(es[3].species_id, 9);
    for (std::size_t i = 1; i < es.size(); ++i) {
        EXPECT_LT(es[i - 1].species_id, es[i].species_id);
    }
}

// ---- determinism: run == replay ----

// Recording the SAME multiset of captures in DIFFERENT orders yields a BIT-IDENTICAL
// codex (same entries, counts, best scores, aggregates). This is the run==replay
// contract: the Codex is order-independent and reproducible.
TEST(PhotoCodex, RunEqualsReplayOrderIndependent) {
    struct Cap {
        int sp;
        float score;
    };
    const Cap forward[] = {
        {3, 0.4f},
        {7, 0.9f},
        {3, 0.8f},
        {1, 0.55f},
        {7, 0.2f},
        {3, 0.1f},
    };
    const Cap shuffled[] = {
        {7, 0.2f},
        {1, 0.55f},
        {3, 0.1f},
        {3, 0.8f},
        {7, 0.9f},
        {3, 0.4f},
    };

    PhotoCodex a;
    for (const auto& c : forward)
        a.Record(c.sp, c.score);
    PhotoCodex b;
    for (const auto& c : shuffled)
        b.Record(c.sp, c.score);

    ASSERT_EQ(a.entries().size(), b.entries().size());
    for (std::size_t i = 0; i < a.entries().size(); ++i) {
        EXPECT_EQ(a.entries()[i].species_id, b.entries()[i].species_id);
        EXPECT_EQ(a.entries()[i].captures, b.entries()[i].captures);
        // Exact bit equality (==), not approximate: determinism, not tolerance.
        EXPECT_EQ(a.entries()[i].best_score, b.entries()[i].best_score);
    }
    EXPECT_EQ(a.species_count(), b.species_count());
    EXPECT_EQ(a.total_score(), b.total_score());
    EXPECT_EQ(a.completeness(20), b.completeness(20));

    // And the recorded best scores are correct: species 3 best=0.8, 7 best=0.9, 1=0.55.
    EXPECT_EQ(a.species_count(), 3u);
    EXPECT_FLOAT_EQ(a.entries()[0].best_score, 0.55f); // species 1
    EXPECT_FLOAT_EQ(a.entries()[1].best_score, 0.8f);  // species 3
    EXPECT_FLOAT_EQ(a.entries()[2].best_score, 0.9f);  // species 7
    EXPECT_EQ(a.entries()[1].captures, 3u);            // species 3 captured 3x
}

} // namespace
