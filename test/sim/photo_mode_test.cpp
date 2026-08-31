// game.photo_mode: the photography CAPTURE LOOP glue (feature).
//
// These tests pin the capture loop's two hard invariants:
//
//   1. CAPTURE-SCORING DETERMINISM: a fixed set of PhotoSubjectViews -> BuildShotInput
//      -> CaptureShot run twice yields a byte-identical ShotVerdict AND identical
//      PhotoCodex entries (order + bytes); commit order is irrelevant (the codex is
//      species_id-sorted). Mirrors PhotoSession.RunEqualsReplay_*.
//
//   2. SIM-ISOLATION GUARD: driving a capture against an entt registry of creature
//      subjects mutates NO component state — a content hash over the registry's
//      sim-relevant fields is byte-identical before and after the capture. Photo mode
//      is a read-mostly observer, so it perturbs no world_hash.
//
// PURE w.r.t. the scoring flow: BuildShotInput/CaptureShot touch no entt, no rng, no
// wall-clock. The registry in the isolation guard is a CONST read in the capture path;
// the test only constructs it and hashes it (the capture adapter reads it const).
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "game/PhotoMode.h"

namespace {

using luminumbra::game::BuildShotInput;
using luminumbra::game::CaptureShot;
using luminumbra::game::LensSettings;
using luminumbra::game::PhotoCodex;
using luminumbra::game::PhotoSidecar;
using luminumbra::game::PhotoSubjectView;
using luminumbra::game::SerializePhotoSidecar;
using luminumbra::game::ShotInput;
using luminumbra::game::ShotVerdict;

namespace Components = Luminumbra::Components;

// ---------------------------------------------------------------------------
// A representative frame: three in-frustum subjects of two species (a bold main
// subject on a power point, a smaller one, a distant tiny one) plus one OUT-of-
// frustum view that BuildShotInput must drop. Distances/sizes drive the optics.
// ---------------------------------------------------------------------------
std::vector<PhotoSubjectView> MakeFrameViews() {
    std::vector<PhotoSubjectView> views;

    PhotoSubjectView main;
    main.ndc_x = 0.33333334f; // rule-of-thirds power point
    main.ndc_y = 0.33333334f;
    main.size = 0.55f; // largest -> principal subject
    main.light = 0.7f;
    main.species_id = 42;
    main.distance_m = 3.0f;
    main.size_m = 0.6f;
    main.in_frustum = true;
    views.push_back(main);

    PhotoSubjectView second;
    second.ndc_x = -0.2f;
    second.ndc_y = 0.1f;
    second.size = 0.25f;
    second.light = 0.6f;
    second.species_id = 7;
    second.distance_m = 5.0f;
    second.size_m = 0.4f;
    second.in_frustum = true;
    views.push_back(second);

    PhotoSubjectView distant;
    distant.ndc_x = 0.05f;
    distant.ndc_y = -0.4f;
    distant.size = 0.08f;
    distant.light = 0.5f;
    distant.species_id = 42;
    distant.distance_m = 18.0f;
    distant.size_m = 0.5f;
    distant.in_frustum = true;
    views.push_back(distant);

    PhotoSubjectView offscreen; // BEHIND the camera / outside the frustum
    offscreen.ndc_x = 0.0f;
    offscreen.ndc_y = 0.0f;
    offscreen.size = 0.9f; // would be "main" if it counted — it must NOT
    offscreen.light = 0.7f;
    offscreen.species_id = 99;
    offscreen.distance_m = 2.0f;
    offscreen.size_m = 1.0f;
    offscreen.in_frustum = false; // dropped by BuildShotInput
    views.push_back(offscreen);

    return views;
}

LensSettings MakePortraitLens() {
    LensSettings lens;
    lens.focal_length_mm = 85.0f;
    lens.aperture_f = 1.8f;
    lens.focus_distance_m = 3.0f; // on the main subject
    lens.iso = 100.0f;
    lens.shutter_s = 0.004f;
    return lens;
}

// ---------------------------------------------------------------------------
// BuildShotInput drops out-of-frustum views and keys the main subject on the
// LARGEST in-frustum size (NOT the bigger offscreen one).
// ---------------------------------------------------------------------------
TEST(PhotoMode, BuildShotInputDropsOutOfFrustumAndPicksMain) {
    const ShotInput in = BuildShotInput(MakeFrameViews(), MakePortraitLens(), 0.5f);

    // 3 in-frustum subjects survive; the offscreen one is gone.
    ASSERT_EQ(in.composition.subjects.size(), 3u);

    // The principal subject is species 42 (size 0.55), NOT the offscreen species 99.
    EXPECT_EQ(in.main_species_id, 42);
    EXPECT_FLOAT_EQ(in.main_subject_distance_m, 3.0f);
    EXPECT_FLOAT_EQ(in.main_subject_size_m, 0.6f);

    // The lens + scene carried through verbatim.
    EXPECT_FLOAT_EQ(in.lens.aperture_f, 1.8f);
    EXPECT_FLOAT_EQ(in.scene_luminance, 0.5f);
}

// An all-out-of-frustum frame is a defined subject-less ShotInput -> zero verdict.
TEST(PhotoMode, EmptyFrameIsDefinedZeroVerdict) {
    std::vector<PhotoSubjectView> none;
    PhotoSubjectView v;
    v.in_frustum = false;
    none.push_back(v);

    const ShotInput in = BuildShotInput(none, MakePortraitLens(), 0.5f);
    EXPECT_TRUE(in.composition.subjects.empty());

    PhotoCodex codex;
    const ShotVerdict verdict = CaptureShot(codex, in);
    EXPECT_FLOAT_EQ(verdict.total, 0.0f);
    EXPECT_EQ(verdict.stars, 0);
    // A photo of nothing records nothing.
    EXPECT_EQ(codex.species_count(), 0u);
}

// ---------------------------------------------------------------------------
// CAPTURE-SCORING DETERMINISM FIXTURE: the same frame -> byte-identical
// verdict + codex on a re-run; commit order is irrelevant.
// ---------------------------------------------------------------------------
TEST(PhotoMode, CaptureRunEqualsReplay_Verdict) {
    const ShotInput in = BuildShotInput(MakeFrameViews(), MakePortraitLens(), 0.5f);

    PhotoCodex a_codex;
    PhotoCodex b_codex;
    const ShotVerdict a = CaptureShot(a_codex, in);
    const ShotVerdict b = CaptureShot(b_codex, in);

    EXPECT_EQ(a.composition, b.composition);
    EXPECT_EQ(a.exposure, b.exposure);
    EXPECT_EQ(a.focus_isolation, b.focus_isolation);
    EXPECT_EQ(a.total, b.total);
    EXPECT_EQ(a.stars, b.stars);
}

TEST(PhotoMode, CaptureRunEqualsReplay_Codex) {
    const ShotInput in = BuildShotInput(MakeFrameViews(), MakePortraitLens(), 0.5f);

    // Capture the SAME shot 3 times into two codexes; they must agree byte-for-byte.
    PhotoCodex run;
    PhotoCodex replay;
    for (int i = 0; i < 3; ++i) {
        CaptureShot(run, in);
        CaptureShot(replay, in);
    }

    ASSERT_EQ(run.species_count(), replay.species_count());
    const auto& re = run.entries();
    const auto& pe = replay.entries();
    ASSERT_EQ(re.size(), pe.size());
    for (std::size_t i = 0; i < re.size(); ++i) {
        EXPECT_EQ(re[i].species_id, pe[i].species_id);
        EXPECT_EQ(re[i].captures, pe[i].captures);
        EXPECT_EQ(re[i].best_score, pe[i].best_score);
    }
    EXPECT_EQ(run.total_score(), replay.total_score());
    // The main subject (species 42) is recorded; captures climb with each shutter.
    ASSERT_TRUE(run.discovered(42));
}

// The sidecar serialization is deterministic: same verdict + lens -> same bytes.
TEST(PhotoMode, SidecarSerializationIsDeterministic) {
    const ShotInput in = BuildShotInput(MakeFrameViews(), MakePortraitLens(), 0.5f);
    PhotoCodex codex;
    const ShotVerdict v = CaptureShot(codex, in);

    PhotoSidecar side;
    side.stamp = "frame-000042";
    side.verdict = v;
    side.species_id = in.main_species_id;
    side.lens = in.lens;
    side.observation.subject_action = 5; // Sleep
    side.observation.time_of_day = 0.75f;
    side.observation.scene_luminance = 0.62f;

    const std::string a = SerializePhotoSidecar(side);
    const std::string b = SerializePhotoSidecar(side);
    EXPECT_EQ(a, b);
    // The sidecar names the principal species + carries the star rating.
    EXPECT_NE(a.find("\"species_id\": 42"), std::string::npos);
    EXPECT_NE(a.find("\"stars\":"), std::string::npos);
    // the sidecar carries the observation context (behaviour/time/light).
    EXPECT_NE(a.find("\"observation\""), std::string::npos);
    EXPECT_NE(a.find("\"subject_action\": 5"), std::string::npos);
    EXPECT_NE(a.find("\"time_of_day\":"), std::string::npos);
    EXPECT_NE(a.find("\"scene_luminance\":"), std::string::npos);
}

// ---------------------------------------------------------------------------
// SIM-ISOLATION GUARD: a capture against a CONST registry of creature
// subjects mutates no sim component state. We hash the registry's sim-relevant
// fields (transform position + creature role/hunger/stamina) before and after a
// full capture (the same client adapter path: read const -> BuildShotInput ->
// CaptureShot) and assert byte-identical.
// ---------------------------------------------------------------------------

// Build a small roster of creature entities at deterministic positions.
void SeedCreatureRoster(entt::registry& reg) {
    struct Spawn {
        float x, y, z;
        bool predator;
        float hunger;
    };
    const Spawn spawns[] = {
        {3.0f, 0.0f, 2.0f, false, 0.30f},
        {-4.0f, 0.0f, 6.0f, true, 0.55f},
        {1.5f, 0.0f, 12.0f, false, 0.10f},
        {8.0f, 0.0f, 4.0f, true, 0.80f},
    };
    for (const Spawn& s : spawns) {
        const entt::entity e = reg.create();
        Components::TransformComponent t;
        t.position = Luminumbra::Vec3{s.x, s.y, s.z};
        reg.emplace<Components::TransformComponent>(e, t);
        Components::CreatureComponent c;
        c.is_predator = s.predator;
        c.hunger = s.hunger;
        reg.emplace<Components::CreatureComponent>(e, c);
    }
}

// A simple, order-deterministic FNV-1a content hash over the sim-relevant component
// fields. Iterating a sorted view of entities keeps it stable regardless of internal
// storage order.
std::uint64_t HashCreatureState(const entt::registry& reg) {
    std::uint64_t h = 1469598103934665603ull; // FNV offset basis
    auto fold = [&h](const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull; // FNV prime
        }
    };
    // entt entity ids are assigned monotonically here, so the view order is stable.
    auto view =
        reg.view<const Components::TransformComponent, const Components::CreatureComponent>();
    for (const entt::entity e : view) {
        const auto& t = view.get<const Components::TransformComponent>(e);
        const auto& c = view.get<const Components::CreatureComponent>(e);
        fold(&t.position.x, sizeof(float));
        fold(&t.position.y, sizeof(float));
        fold(&t.position.z, sizeof(float));
        const unsigned char pred = c.is_predator ? 1u : 0u;
        fold(&pred, sizeof(pred));
        fold(&c.hunger, sizeof(float));
        fold(&c.stamina, sizeof(float));
        fold(&c.move_speed, sizeof(float));
    }
    return h;
}

// The capture adapter, read-only: extract PhotoSubjectViews from a CONST registry
// exactly as the client does (project-free here — the test pins isolation, not the
// projection math, which BuildShotInput tests cover). Derives species from the
// predator role (deterministic proxy; spike-only, no new component) and a constant
// scene luminance (R1: no luminance component to read).
std::vector<PhotoSubjectView> GatherSubjects(const entt::registry& reg) {
    std::vector<PhotoSubjectView> views;
    auto view =
        reg.view<const Components::TransformComponent, const Components::CreatureComponent>();
    for (const entt::entity e : view) {
        const auto& t = view.get<const Components::TransformComponent>(e);
        const auto& c = view.get<const Components::CreatureComponent>(e);
        PhotoSubjectView v;
        v.ndc_x = 0.1f; // a stand-in projection (isolation test)
        v.ndc_y = -0.1f;
        v.size = 0.4f;
        v.light = 0.6f;
        v.species_id = c.is_predator ? 1 : 2; // deterministic role proxy
        // Distance from origin in the XZ plane (no libm: a crude L1 proxy is fine here).
        const float dx = t.position.x < 0.0f ? -t.position.x : t.position.x;
        const float dz = t.position.z < 0.0f ? -t.position.z : t.position.z;
        v.distance_m = dx + dz + 1.0f;
        v.size_m = 0.5f;
        v.in_frustum = true;
        views.push_back(v);
    }
    return views;
}

TEST(PhotoMode, CaptureDoesNotPerturbSimState) {
    entt::registry reg;
    SeedCreatureRoster(reg);

    const std::uint64_t before = HashCreatureState(reg);

    // Drive a full capture against the CONST registry, twice, mutating a client-only
    // codex (which is documented hash-free) — never the registry.
    const entt::registry& const_reg = reg;
    PhotoCodex codex;
    for (int i = 0; i < 5; ++i) {
        const std::vector<PhotoSubjectView> views = GatherSubjects(const_reg);
        const ShotInput in = BuildShotInput(views, MakePortraitLens(), 0.5f);
        const ShotVerdict v = CaptureShot(codex, in);
        EXPECT_GE(v.total, 0.0f);
        EXPECT_LE(v.total, 1.0f);
    }

    const std::uint64_t after = HashCreatureState(reg);
    EXPECT_EQ(before, after) << "photo-mode capture perturbed sim component state";
    // The capture path did discover the role-proxy species (loop ran, codex fed).
    EXPECT_GT(codex.species_count(), 0u);
}

} // namespace
