//  keystone — CREATURE SPECIES IDENTITY feeds the photo CODEX.
//
// Before this slice the client adapter (main_client.cpp GatherPhotoSubjects) keyed the
// codex on a binary predator/prey proxy (`is_predator ? 1 : 2`), so a world of many
// distinct creatures could only ever discover TWO "species" — the codex could never
// fill. This test pins the fix: CreatureComponent now carries a stable `species_id`
// (CreatureSpeciesId16 of the archetype name), set at spawn and inherited by offspring,
// and the capture path keys the codex on it. These are the invariants that keystone
// rests on:
//
//   1. CreatureSpeciesId16 is a pure, deterministic, nonzero, low-collision name hash
//      (mirrors foliage::SpeciesId16) — the same name always yields the same id, and
//      0 is reserved for "unspecified".
//   2. A roster of creatures with REAL distinct species_ids, run through the same
//      project->BuildShotInput->CaptureShot flow the client uses, fills the codex with
//      as many species as the roster carries (NOT two buckets).
//   3. The species-resolution policy matches the client: a creature with species_id==0
//      falls back to the predator/prey role proxy, so legacy/unspecified rosters keep
//      their old two-bucket behaviour.
//
// PURE: no GL, no rng, no wall-clock. The mapping mirrors the single client line under
// test (GatherPhotoSubjects' species assignment) so the contract is pinned without
// linking the client app.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "components/CreatureComponents.h"
#include "game/PhotoMode.h"

namespace {

namespace Components = Luminumbra::Components;
using luminumbra::game::BuildShotInput;
using luminumbra::game::CaptureShot;
using luminumbra::game::LensSettings;
using luminumbra::game::PhotoCodex;
using luminumbra::game::PhotoSubjectView;
using luminumbra::game::ShotInput;

// The species-resolution policy, identical to main_client.cpp GatherPhotoSubjects: a
// real (nonzero) species_id keys the codex; 0 falls back to the role proxy.
int ResolveCodexSpecies(const Components::CreatureComponent& cr) {
    return cr.species_id != 0 ? static_cast<int>(cr.species_id) : (cr.is_predator ? 1 : 2);
}

LensSettings MakeLens() {
    LensSettings lens;
    lens.focal_length_mm = 85.0f;
    lens.aperture_f = 1.8f;
    lens.focus_distance_m = 3.0f;
    lens.iso = 100.0f;
    lens.shutter_s = 0.004f;
    return lens;
}

// Project a creature roster into the photo subjects the client would gather, keying each
// on the resolved codex species. A single bold in-frame subject per capture so the shot
// is well-formed and the main species is unambiguous.
PhotoSubjectView ViewFor(const Components::CreatureComponent& cr) {
    PhotoSubjectView v;
    v.ndc_x = 0.33333334f; // a power point so the shot scores above the zero floor
    v.ndc_y = 0.33333334f;
    v.size = 0.55f;
    v.light = 0.7f;
    v.species_id = ResolveCodexSpecies(cr);
    v.distance_m = 3.0f;
    v.size_m = 0.6f;
    v.in_frustum = true;
    return v;
}

// ---------------------------------------------------------------------------
// (1) CreatureSpeciesId16 — deterministic, nonzero, distinct names differ.
// ---------------------------------------------------------------------------
TEST(SpeciesIdentity, SpeciesIdIsDeterministicNonzeroAndDistinct) {
    const std::uint16_t a1 = Components::CreatureSpeciesId16("grovestrider");
    const std::uint16_t a2 = Components::CreatureSpeciesId16("grovestrider");
    const std::uint16_t b = Components::CreatureSpeciesId16("ridgeback_stalker");

    EXPECT_EQ(a1, a2) << "same name must hash to the same id (run==replay)";
    EXPECT_NE(a1, 0) << "0 is reserved for unspecified";
    EXPECT_NE(b, 0);
    EXPECT_NE(a1, b) << "distinct species must get distinct ids";
    // Empty/null names still avoid the reserved 0.
    EXPECT_NE(Components::CreatureSpeciesId16(""), 0);
    EXPECT_NE(Components::CreatureSpeciesId16(nullptr), 0);
}

// ---------------------------------------------------------------------------
// (2) A roster of distinct real species fills the codex beyond two buckets.
// ---------------------------------------------------------------------------
TEST(SpeciesIdentity, RealSpeciesFillTheCodexBeyondTwoBuckets) {
    // Five distinct named species — the kind of variety procedural creatures
    // will produce. Each is photographed once.
    const char* names[] = {
        "grovestrider", "ridgeback_stalker", "lumen_moth", "tide_grazer", "ashen_corvid"};

    PhotoCodex codex;
    for (const char* name : names) {
        Components::CreatureComponent cr;
        cr.species_id = Components::CreatureSpeciesId16(name);
        const ShotInput shot = BuildShotInput({ViewFor(cr)}, MakeLens(), 0.7f);
        CaptureShot(codex, shot);
    }

    // The codex discovered all five DISTINCT species — not collapsed into two role
    // buckets the way the old is_predator?1:2 proxy would have.
    EXPECT_EQ(codex.species_count(), 5u);
    for (const char* name : names) {
        EXPECT_TRUE(codex.discovered(Components::CreatureSpeciesId16(name)))
            << "species missing from codex: " << name;
    }
}

// ---------------------------------------------------------------------------
// (3) Unspecified (species_id==0) creatures fall back to the role proxy.
// ---------------------------------------------------------------------------
TEST(SpeciesIdentity, UnspecifiedCreaturesFallBackToRoleProxy) {
    Components::CreatureComponent predator; // species_id defaults to 0
    predator.is_predator = true;
    Components::CreatureComponent prey;
    prey.is_predator = false;

    EXPECT_EQ(ResolveCodexSpecies(predator), 1);
    EXPECT_EQ(ResolveCodexSpecies(prey), 2);

    PhotoCodex codex;
    CaptureShot(codex, BuildShotInput({ViewFor(predator)}, MakeLens(), 0.7f));
    CaptureShot(codex, BuildShotInput({ViewFor(prey)}, MakeLens(), 0.7f));
    // Two unspecified creatures of opposite roles -> exactly the two legacy buckets.
    EXPECT_EQ(codex.species_count(), 2u);
    EXPECT_TRUE(codex.discovered(1));
    EXPECT_TRUE(codex.discovered(2));
}

} // namespace
