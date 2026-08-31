// per-species behaviour data: IAUS curve/weight overrides + genome
// ranges in the species JSON schema, resolved through CreatureSpeciesRegistry at world load.
//
// THE CONTRACT UNDER TEST (the byte-identical-defaults law):
//   * CreatureBrainParams{} and SpeciesGenomeRanges{} reproduce the compiled-in constants
//     EXACTLY (field equality / memcmp) — so a species JSON that omits the "brain" /
//     "genome_ranges" blocks, a directory with no JSON at all, and the shipped roster all
//     load a table whose behaviour blocks equal the compiled constants byte-for-byte.
//   * An explicit override CHANGES the loaded table (non-vacuity: the schema really is
//     data-driven, both for decisions and for gene clamp bands).
//   * The parameterised breeding operators with default ranges are byte-identical to the
//     historical operators for the same DeterministicRng seed (exact float equality — same
//     draws, same clamps).
//
// Pure CPU (no GL, no world); links only luminumbra_common + gtest.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

#include "ai/CreatureBrain.h"
#include "ai/CreatureGenome.h"
#include "ai/CreatureSpeciesRegistry.h"
#include "core/DeterministicRng.h"

namespace {

using luminumbra::ai::CreatureAction;
using luminumbra::ai::CreatureBrainParams;
using luminumbra::ai::CreatureGenome;
using luminumbra::ai::CreatureSenses;
using luminumbra::ai::CreatureSpecies;
using luminumbra::ai::CreatureSpeciesRegistry;
using luminumbra::ai::GeneBound;
using luminumbra::ai::SpeciesGenomeRanges;

// Exact (bitwise-value) comparison helpers. EXPECT_EQ on floats is an exact compare, which
// is precisely the byte-identical contract here — no tolerance.
void ExpectBoundEq(const GeneBound& got, const GeneBound& want, const char* label) {
    EXPECT_EQ(got.lo, want.lo) << label << ".lo";
    EXPECT_EQ(got.hi, want.hi) << label << ".hi";
}

void ExpectBrainEquals(const CreatureBrainParams& got,
                       const CreatureBrainParams& want,
                       const std::string& label) {
    EXPECT_EQ(got.wander_weight, want.wander_weight) << label;
    EXPECT_EQ(got.rest_weight, want.rest_weight) << label;
    EXPECT_EQ(got.sleep_weight, want.sleep_weight) << label;
    EXPECT_EQ(got.hunt_weight, want.hunt_weight) << label;
    EXPECT_EQ(got.flee_weight, want.flee_weight) << label;
    EXPECT_EQ(got.graze_weight, want.graze_weight) << label;
    EXPECT_EQ(got.flee_logistic_m, want.flee_logistic_m) << label;
    EXPECT_EQ(got.flee_logistic_c, want.flee_logistic_c) << label;
}

// SpeciesGenomeRanges is a flat POD of GeneBounds (float pairs, no padding), so memcmp is a
// valid whole-struct byte-identity check.
static_assert(std::is_trivially_copyable_v<SpeciesGenomeRanges>,
              "memcmp equality below requires a trivially-copyable ranges struct");
bool RangesBytesEqual(const SpeciesGenomeRanges& a, const SpeciesGenomeRanges& b) {
    return std::memcmp(&a, &b, sizeof(SpeciesGenomeRanges)) == 0;
}

// ---------------------------------------------------------------------------
// Defaults reproduce the compiled constants (the historical inline literals).
// ---------------------------------------------------------------------------

TEST(SpeciesBehaviorOverrides, DefaultBrainParamsMatchCompiledConstants) {
    // These literals are the values DecideCreatureAction carried inline before the seam was
    // externalised. If a default here drifts, the "absent JSON is byte-identical" law breaks
    // silently — this test is the drift guard.
    const CreatureBrainParams p{};
    EXPECT_EQ(p.wander_weight, 0.20f);
    EXPECT_EQ(p.rest_weight, 0.9f);
    EXPECT_EQ(p.sleep_weight, 1.0f);
    EXPECT_EQ(p.hunt_weight, 1.0f);
    EXPECT_EQ(p.flee_weight, 1.1f);
    EXPECT_EQ(p.graze_weight, 1.0f);
    EXPECT_EQ(p.flee_logistic_m, 2.0f);
    EXPECT_EQ(p.flee_logistic_c, 0.45f);
}

TEST(SpeciesBehaviorOverrides, DefaultGenomeRangesMatchCanonicalBounds) {
    const SpeciesGenomeRanges r{};
    const auto core = luminumbra::ai::CreatureGeneBounds();
    const auto sensory = luminumbra::ai::CreatureSensoryGeneBounds();
    ASSERT_EQ(r.core.size(), core.size());
    ASSERT_EQ(r.sensory.size(), sensory.size());
    for (std::size_t i = 0; i < core.size(); ++i)
        ExpectBoundEq(r.core[i], core[i], "core");
    for (std::size_t i = 0; i < sensory.size(); ++i)
        ExpectBoundEq(r.sensory[i], sensory[i], "sensory");
}

// Default params through the arbiter give the same decision as the no-params call across a
// dense senses sweep (both roles; every axis exercised at its curve knees). This holds by
// construction (the default argument IS CreatureBrainParams{}), so the load-bearing pins are
// the constant checks above — the sweep documents the equivalence and catches an accidental
// future divergence between the two call forms.
TEST(SpeciesBehaviorOverrides, DefaultParamsDecideIdenticallyAcrossSensesSweep) {
    const float levels[] = {0.0f, 0.25f, 0.45f, 0.6f, 0.75f, 1.0f};
    for (bool predator : {false, true}) {
        for (float hunger : levels) {
            for (float threat : levels) {
                for (float stamina : levels) {
                    for (float energy : levels) {
                        CreatureSenses s;
                        s.is_predator = predator;
                        s.hunger = hunger;
                        s.threat_proximity = threat;
                        s.food_proximity = predator ? threat : 0.6f;
                        s.stamina = stamina;
                        s.energy = energy;
                        s.circadian_activity = energy; // exercise the Sleep axis too
                        EXPECT_EQ(DecideCreatureAction(s),
                                  DecideCreatureAction(s, CreatureBrainParams{}));
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Overrides actually change behaviour (non-vacuity).
// ---------------------------------------------------------------------------

TEST(SpeciesBehaviorOverrides, BrainOverrideChangesDecision) {
    // Threatened prey with full reserves: default arbiter flees (threat 0.6 is past the
    // logistic center 0.45 and Flee outweighs the 0.2 Wander baseline).
    CreatureSenses s;
    s.is_predator = false;
    s.threat_proximity = 0.6f;
    ASSERT_EQ(DecideCreatureAction(s), CreatureAction::Flee);

    // Weight override: a fearless species (flee weight zeroed) wanders instead.
    CreatureBrainParams fearless;
    fearless.flee_weight = 0.0f;
    EXPECT_EQ(DecideCreatureAction(s, fearless), CreatureAction::Wander);

    // Curve override: pushing the flee logistic center past the sensed threat (0.95 vs 0.6)
    // zeroes the response — same decision flip via the curve knob rather than the weight.
    CreatureBrainParams stoic;
    stoic.flee_logistic_c = 0.95f;
    EXPECT_EQ(DecideCreatureAction(s, stoic), CreatureAction::Wander);
}

TEST(SpeciesBehaviorOverrides, GenomeRangeOverrideClampsBreeding) {
    // A range narrowed to a point pins the gene regardless of parents/mutation: the clamp
    // band is really consumed by the breeding operator.
    SpeciesGenomeRanges ranges;
    ranges.core[0] = GeneBound{2.5f, 2.5f}; // move_speed pinned
    CreatureGenome a;
    a.move_speed = 4.0f;
    CreatureGenome b;
    b.move_speed = 6.0f;
    auto rng = luminumbra::core::DeterministicRng::seeded(16, 1234, 1);
    const CreatureGenome child = luminumbra::ai::BreedOffspring(a, b, rng, ranges);
    EXPECT_EQ(child.move_speed, 2.5f);
}

// ---------------------------------------------------------------------------
// Parameterised breeding with DEFAULT ranges is byte-identical to the
// historical operators (same seed -> exact float equality on every field).
// ---------------------------------------------------------------------------

TEST(SpeciesBehaviorOverrides, BreedingWithDefaultRangesIsByteIdentical) {
    CreatureGenome a;
    a.move_speed = 4.5f;
    a.vigilance = 0.7f;
    a.hunger_threshold = 0.4f;
    a.size_scale = 1.2f;
    a.vision_cos_half_fov = 0.6f;
    a.vision_range = 25.0f;
    a.hearing_range = 18.0f;
    CreatureGenome b;
    b.move_speed = 2.5f;
    b.vigilance = 0.2f;
    b.hunger_threshold = 0.2f;
    b.size_scale = 0.9f;
    b.vision_cos_half_fov = 0.3f;
    b.vision_range = 12.0f;
    b.hearing_range = 30.0f;

    auto expect_genomes_equal = [](const CreatureGenome& x, const CreatureGenome& y) {
        EXPECT_EQ(x.move_speed, y.move_speed);
        EXPECT_EQ(x.vigilance, y.vigilance);
        EXPECT_EQ(x.hunger_threshold, y.hunger_threshold);
        EXPECT_EQ(x.size_scale, y.size_scale);
        EXPECT_EQ(x.vision_cos_half_fov, y.vision_cos_half_fov);
        EXPECT_EQ(x.vision_range, y.vision_range);
        EXPECT_EQ(x.hearing_range, y.hearing_range);
    };

    {
        auto rng1 = luminumbra::core::DeterministicRng::seeded(16, 42, 7);
        auto rng2 = luminumbra::core::DeterministicRng::seeded(16, 42, 7);
        expect_genomes_equal(luminumbra::ai::BreedOffspring(a, b, rng1),
                             luminumbra::ai::BreedOffspring(a, b, rng2, SpeciesGenomeRanges{}));
    }
    {
        auto rng1 = luminumbra::core::DeterministicRng::seeded(16, 99, 3);
        auto rng2 = luminumbra::core::DeterministicRng::seeded(16, 99, 3);
        expect_genomes_equal(luminumbra::ai::MutateOffspring(a, rng1),
                             luminumbra::ai::MutateOffspring(a, rng2, SpeciesGenomeRanges{}));
    }
    {
        auto rng1 = luminumbra::core::DeterministicRng::seeded(16, 5, 11);
        auto rng2 = luminumbra::core::DeterministicRng::seeded(16, 5, 11);
        CreatureGenome child1 = luminumbra::ai::BreedOffspring(a, b, rng1);
        CreatureGenome child2 = luminumbra::ai::BreedOffspring(a, b, rng2, SpeciesGenomeRanges{});
        expect_genomes_equal(
            luminumbra::ai::BreedSensoryInto(child1, a, b, rng1),
            luminumbra::ai::BreedSensoryInto(child2, a, b, rng2, SpeciesGenomeRanges{}));
    }
}

// ---------------------------------------------------------------------------
// Species JSON round-trip: absent blocks -> compiled defaults; present blocks
// -> the loaded table changes; malformed entries -> defaults hold.
// ---------------------------------------------------------------------------

TEST(SpeciesBehaviorOverrides, SpeciesJsonWithoutOverridesLoadsCompiledDefaults) {
    CreatureSpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(R"({
      "id": "grovestrider", "display_name": "Grovestrider",
      "predator": false, "rarity": 0.35, "base_color": [0.36, 0.42, 0.30]
    })",
                                    err))
        << err;
    const auto* s = reg.FindByName("grovestrider");
    ASSERT_NE(s, nullptr);
    ExpectBrainEquals(s->brain, CreatureBrainParams{}, "absent brain block");
    EXPECT_TRUE(RangesBytesEqual(s->genome_ranges, SpeciesGenomeRanges{}));
}

TEST(SpeciesBehaviorOverrides, SpeciesJsonMatchingDefaultsLoadsCompiledDefaults) {
    // Explicitly writing the default values must be indistinguishable from omitting them.
    CreatureSpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(R"({
      "id": "echo",
      "brain": { "wander_weight": 0.20, "rest_weight": 0.9, "sleep_weight": 1.0,
                 "hunt_weight": 1.0, "flee_weight": 1.1, "graze_weight": 1.0,
                 "flee_logistic_m": 2.0, "flee_logistic_c": 0.45 },
      "genome_ranges": { "move_speed": [1.0, 8.0], "vigilance": [0.0, 1.0],
                         "hunger_threshold": [0.05, 0.6], "size_scale": [0.6, 1.8],
                         "vision_cos_half_fov": [0.2, 0.95], "vision_range": [6.0, 45.0],
                         "hearing_range": [6.0, 45.0] }
    })",
                                    err))
        << err;
    const auto* s = reg.FindByName("echo");
    ASSERT_NE(s, nullptr);
    ExpectBrainEquals(s->brain, CreatureBrainParams{}, "defaults-matching brain block");
    EXPECT_TRUE(RangesBytesEqual(s->genome_ranges, SpeciesGenomeRanges{}));
}

TEST(SpeciesBehaviorOverrides, SpeciesJsonOverridesChangeLoadedTable) {
    CreatureSpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(R"({
      "id": "test_darter",
      "brain": { "flee_weight": 0.8, "flee_logistic_c": 0.3, "wander_weight": 0.35 },
      "genome_ranges": { "move_speed": [2.0, 6.0], "hearing_range": [10.0, 30.0] }
    })",
                                    err))
        << err;
    const auto* s = reg.FindByName("test_darter");
    ASSERT_NE(s, nullptr);

    // Overridden fields carry the JSON values...
    EXPECT_FLOAT_EQ(s->brain.flee_weight, 0.8f);
    EXPECT_FLOAT_EQ(s->brain.flee_logistic_c, 0.3f);
    EXPECT_FLOAT_EQ(s->brain.wander_weight, 0.35f);
    EXPECT_FLOAT_EQ(s->genome_ranges.core[0].lo, 2.0f);
    EXPECT_FLOAT_EQ(s->genome_ranges.core[0].hi, 6.0f);
    EXPECT_FLOAT_EQ(s->genome_ranges.sensory[2].lo, 10.0f);
    EXPECT_FLOAT_EQ(s->genome_ranges.sensory[2].hi, 30.0f);

    //...while every untouched field keeps its compiled default (partial overrides are safe).
    const CreatureBrainParams def{};
    EXPECT_EQ(s->brain.rest_weight, def.rest_weight);
    EXPECT_EQ(s->brain.sleep_weight, def.sleep_weight);
    EXPECT_EQ(s->brain.hunt_weight, def.hunt_weight);
    EXPECT_EQ(s->brain.graze_weight, def.graze_weight);
    EXPECT_EQ(s->brain.flee_logistic_m, def.flee_logistic_m);
    const SpeciesGenomeRanges defr{};
    for (std::size_t i = 1; i < defr.core.size(); ++i)
        ExpectBoundEq(s->genome_ranges.core[i], defr.core[i], "untouched core");
    for (std::size_t i = 0; i < 2; ++i)
        ExpectBoundEq(s->genome_ranges.sensory[i], defr.sensory[i], "untouched sensory");
}

TEST(SpeciesBehaviorOverrides, MalformedOverrideEntriesKeepDefaults) {
    CreatureSpeciesRegistry reg;
    std::string err;
    // Wrong types, wrong arity, and an inverted band: each entry is individually rejected
    // (the species still loads; the compiled defaults hold — a bad file must never invent
    // behaviour).
    ASSERT_TRUE(reg.AddFromJsonText(R"({
      "id": "mangled",
      "brain": { "flee_weight": "high", "wander_weight": [0.5] },
      "genome_ranges": { "move_speed": [6.0, 2.0], "vigilance": "wide",
                         "size_scale": [1.0], "vision_range": [null, 20.0] }
    })",
                                    err))
        << err;
    const auto* s = reg.FindByName("mangled");
    ASSERT_NE(s, nullptr);
    ExpectBrainEquals(s->brain, CreatureBrainParams{}, "malformed brain entries");
    EXPECT_TRUE(RangesBytesEqual(s->genome_ranges, SpeciesGenomeRanges{}));
}

// The SHIPPED species roster carries no behaviour overrides, so the world-load table
// (GameSession::LoadSpeciesDefinitions reads this same directory) resolves behaviour blocks
// byte-identical to the compiled constants — the absent-JSON byte-identity proof at the
// content level.
TEST(SpeciesBehaviorOverrides, ShippedSpeciesFilesCarryNoOverrides) {
    CreatureSpeciesRegistry reg;
    std::vector<std::string> errors;
    const std::filesystem::path dir =
        std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "creatures" / "species";
    const std::size_t n = reg.LoadFromDirectory(dir, errors);
    ASSERT_GE(n, 10u) << (errors.empty() ? "" : errors.front());
    for (const CreatureSpecies& s : reg.all()) {
        ExpectBrainEquals(s.brain, CreatureBrainParams{}, "shipped species " + s.id);
        EXPECT_TRUE(RangesBytesEqual(s.genome_ranges, SpeciesGenomeRanges{}))
            << "shipped species " << s.id << " carries genome-range overrides";
    }
}

} // namespace
