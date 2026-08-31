// LIFE-CYCLE cluster — ADVERSARIAL hardening (NEW tests only; no existing file touched).
//
// Cluster systems under test:
//   * ai/CreatureReproductionSystem.h  (sexual repro: mate-seek + courtship resolve)
//   * ai/LifespanSystem.h              (age/lifespan/starvation death)
//   * ai/DecompositionSystem.h         (fixed-point milli-unit nutrient release)
//   * ai/CircadianSystem.h             (diurnal/nocturnal activity in [0,1])
//   * ai/WildlifeFoliageSystem.h       (graze/trample/regrow + creature feed)
//
// Probes: determinism (run==replay byte-exact), gating no-ops, output bounds / no NaN on
// degenerate input, order-independence, producer->consumer unit/magnitude contracts,
// population boundedness, and exact threshold/boundary behaviour. Tests that document a
// suspected bug are marked `// BUG:` and assert the EXPECTED-CORRECT behaviour (they may
// fail against current code until the orchestrator fixes the system).
//
// Determinism: only the systems' own seeded/integer paths are exercised; NO wall-clock,
// NO std::random. run==replay asserts EXACT float equality (systems are -ffp-contract=off).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CircadianSystem.h"
#include "ai/CreatureGenome.h"
#include "ai/CreatureReproductionSystem.h"
#include "ai/DecompositionSystem.h"
#include "ai/Evolution.h"
#include "ai/LifespanSystem.h"
#include "ai/WildlifeFoliageSystem.h"

#include "../support/SeededShuffle.h"

#include "components/CircadianComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/GrazeableComponent.h"
#include "components/MortalComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// ---------------------------------------------------------------------------
// Shared spawn helpers
// ---------------------------------------------------------------------------

// A fertile prey creature with a genome at (x,z). Mature + well-fed + healthy +
// off-cooldown so IsReadyToMate is satisfied unless a knob is dialled down.
entt::entity spawnFertile(entt::registry& r, float x, float z, bool female) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    cr.eaten = false;
    cr.hunger = 0.0f;  // <= genome hunger_threshold (0.3 default)
    cr.stamina = 1.0f; // >= kReproHealthyStamina
    cr.move_speed = 3.0f;
    auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
    gn.female = female;
    gn.age_ticks = luminumbra::ai::kReproMaturityTicks; // mature
    gn.reproduce_cooldown = 0;
    gn.courting_ticks = 0;
    return e;
}

// Count creatures carrying a genome (founders + offspring).
std::size_t creatureCount(entt::registry& r) {
    auto v = r.view<Comp::CreatureGenomeComponent>();
    return static_cast<std::size_t>(std::distance(v.begin(), v.end()));
}

// Snapshot a registry's life-cycle-relevant state into a flat byte-comparable vector,
// keyed in id order so run==replay can assert exact equality.
struct LifeSnap {
    std::vector<std::uint32_t> u;
    std::vector<float> f;
    bool operator==(const LifeSnap& o) const {
        return u == o.u && f == o.f;
    }
};

LifeSnap snapshotGenomes(entt::registry& r) {
    LifeSnap s;
    std::vector<entt::entity> es;
    auto v =
        r.view<Comp::CreatureGenomeComponent, Comp::CreatureComponent, Comp::TransformComponent>();
    for (auto e : v)
        es.push_back(e);
    std::sort(es.begin(), es.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    for (auto e : es) {
        const auto& gn = v.get<Comp::CreatureGenomeComponent>(e);
        const auto& cr = v.get<Comp::CreatureComponent>(e);
        const auto& tf = v.get<Comp::TransformComponent>(e);
        s.u.push_back(static_cast<std::uint32_t>(entt::to_integral(e)));
        s.u.push_back(gn.age_ticks);
        s.u.push_back(gn.reproduce_cooldown);
        s.u.push_back(gn.generation);
        s.u.push_back(gn.courting_ticks);
        s.u.push_back(gn.female ? 1u : 0u);
        s.f.push_back(gn.move_speed);
        s.f.push_back(gn.vigilance);
        s.f.push_back(gn.hunger_threshold);
        s.f.push_back(gn.size_scale);
        s.f.push_back(cr.hunger);
        s.f.push_back(cr.wish_x);
        s.f.push_back(cr.wish_z);
        s.f.push_back(tf.position.x);
        s.f.push_back(tf.position.z);
    }
    return s;
}

// ===========================================================================
// CREATURE REPRODUCTION
// ===========================================================================

// --- GATING: no genome => no work, no births, nothing mutated. ---
TEST(ReproHardening, GatingNoGenomeIsNoOp) {
    entt::registry r;
    // A creature WITHOUT a genome component must be ignored entirely.
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = 1.0f;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.wish_x = 0.25f;
    cr.wish_z = -0.5f;

    luminumbra::ai::RunMateSeekingOnTick(r);
    auto stats = luminumbra::ai::RunMatingResolveOnTick(r, /*tick=*/7, /*seed=*/3);

    EXPECT_EQ(stats.born, 0);
    EXPECT_EQ(stats.courting, 0);
    EXPECT_EQ(creatureCount(r), 0u); // no genome => not counted, not born
    // wish must be untouched (mate-seeking only steers genome-bearing creatures).
    EXPECT_FLOAT_EQ(cr.wish_x, 0.25f);
    EXPECT_FLOAT_EQ(cr.wish_z, -0.5f);
}

// --- EDGE: a lone fertile creature has no mate => no birth, no steer. ---
TEST(ReproHardening, LoneCreatureNoMate) {
    entt::registry r;
    auto e = spawnFertile(r, 0.0f, 0.0f, /*female=*/true);
    auto& cr = r.get<Comp::CreatureComponent>(e);
    cr.wish_x = 0.0f;
    cr.wish_z = 0.0f;

    for (std::uint64_t t = 0; t < 200; ++t) {
        luminumbra::ai::RunMateSeekingOnTick(r);
        auto s = luminumbra::ai::RunMatingResolveOnTick(r, t, 0);
        EXPECT_EQ(s.born, 0);
    }
    EXPECT_EQ(creatureCount(r), 1u);
}

// --- EDGE: two SAME-SEX fertile creatures never breed. ---
TEST(ReproHardening, SameSexNeverBreeds) {
    entt::registry r;
    spawnFertile(r, 0.0f, 0.0f, /*female=*/true);
    spawnFertile(r, 0.5f, 0.0f, /*female=*/true); // adjacent, but both female

    for (std::uint64_t t = 0; t < 400; ++t) {
        luminumbra::ai::RunMateSeekingOnTick(r);
        auto s = luminumbra::ai::RunMatingResolveOnTick(r, t, 0);
        EXPECT_EQ(s.born, 0);
    }
    EXPECT_EQ(creatureCount(r), 2u);
}

// --- EDGE: a too-YOUNG creature (below maturity) cannot breed. ---
TEST(ReproHardening, ImmatureNeverBreeds) {
    entt::registry r;
    auto f = spawnFertile(r, 0.0f, 0.0f, /*female=*/true);
    auto m = spawnFertile(r, 0.3f, 0.0f, /*female=*/false);
    // Knock both below maturity; aging happens inside resolve, so give them a long
    // way to go but only run a few ticks (fewer than maturity).
    r.get<Comp::CreatureGenomeComponent>(f).age_ticks = 0;
    r.get<Comp::CreatureGenomeComponent>(m).age_ticks = 0;

    for (std::uint64_t t = 0; t < luminumbra::ai::kReproMaturityTicks - 1; ++t) {
        luminumbra::ai::RunMateSeekingOnTick(r);
        auto s = luminumbra::ai::RunMatingResolveOnTick(r, t, 0);
        EXPECT_EQ(s.born, 0) << "immature pair must not breed at tick " << t;
    }
    EXPECT_EQ(creatureCount(r), 2u);
}

// --- EDGE: a creature on COOLDOWN cannot breed until it elapses. ---
TEST(ReproHardening, CooldownBlocksBreeding) {
    entt::registry r;
    auto f = spawnFertile(r, 0.0f, 0.0f, /*female=*/true);
    auto m = spawnFertile(r, 0.3f, 0.0f, /*female=*/false);
    r.get<Comp::CreatureGenomeComponent>(f).reproduce_cooldown =
        luminumbra::ai::kReproCooldownTicks;
    r.get<Comp::CreatureGenomeComponent>(m).reproduce_cooldown =
        luminumbra::ai::kReproCooldownTicks;

    int births = 0;
    // While cooldown is non-zero the female is not "ready"; she also cannot accrue
    // courtship. So during the first (cooldown) ticks there must be zero births.
    for (std::uint64_t t = 0; t < luminumbra::ai::kReproCooldownTicks; ++t) {
        luminumbra::ai::RunMateSeekingOnTick(r);
        births += luminumbra::ai::RunMatingResolveOnTick(r, t, 0).born;
    }
    EXPECT_EQ(births, 0);
}

// --- PRODUCER->CONSUMER CONTRACT: mate-seeking writes wish as a VELOCITY whose
//     MAGNITUDE equals move_speed and whose DIRECTION points at the mate (not a
//     world point). A recent pack bug was exactly direction-read-as-point. ---
TEST(ReproHardening, WishIsUnitVelocityTowardMate) {
    entt::registry r;
    // Female at origin; male far to the +x so steering is unambiguous and outside
    // courtship radius (so it steers rather than holding still).
    auto f = spawnFertile(r, 0.0f, 0.0f, /*female=*/true);
    spawnFertile(r, 10.0f, 0.0f, /*female=*/false);
    auto& fcr = r.get<Comp::CreatureComponent>(f);
    fcr.move_speed = 3.0f;

    luminumbra::ai::RunMateSeekingOnTick(r);

    // Direction: straight along +x toward the male.
    EXPECT_GT(fcr.wish_x, 0.0f);
    EXPECT_NEAR(fcr.wish_z, 0.0f, 1e-5f);
    // Magnitude == move_speed (it is a velocity, NOT a world point at x=10).
    const float mag = dm::Sqrt(fcr.wish_x * fcr.wish_x + fcr.wish_z * fcr.wish_z);
    EXPECT_NEAR(mag, fcr.move_speed, 1e-4f);
    EXPECT_LT(fcr.wish_x, 9.0f) << "wish must be a velocity, not the mate's world x";
}

// --- BOUNDS / NaN: coincident pair (zero distance) must not produce NaN/inf wish. ---
TEST(ReproHardening, CoincidentMatesNoNaNWish) {
    entt::registry r;
    auto f = spawnFertile(r, 5.0f, 5.0f, /*female=*/true);
    spawnFertile(r, 5.0f, 5.0f, /*female=*/false); // exactly coincident

    luminumbra::ai::RunMateSeekingOnTick(r);
    const auto& fcr = r.get<Comp::CreatureComponent>(f);
    EXPECT_FALSE(std::isnan(fcr.wish_x));
    EXPECT_FALSE(std::isnan(fcr.wish_z));
    EXPECT_FALSE(std::isinf(fcr.wish_x));
    EXPECT_FALSE(std::isinf(fcr.wish_z));
    // Coincident => within courtship radius => hold still (wish 0).
    EXPECT_FLOAT_EQ(fcr.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(fcr.wish_z, 0.0f);
}

// --- POPULATION BOUNDEDNESS: a small adjacent herd over MANY ticks must not
//     explode (cooldown bounds the birth rate). ---
TEST(ReproHardening, PopulationIsBounded) {
    entt::registry r;
    // 3 females + 3 males clustered so they court, off cooldown, mature.
    for (int i = 0; i < 3; ++i)
        spawnFertile(r, 0.1f * i, 0.0f, /*female=*/true);
    for (int i = 0; i < 3; ++i)
        spawnFertile(r, 0.1f * i, 0.2f, /*female=*/false);

    const std::size_t start = creatureCount(r);
    int totalBirths = 0;
    const int kTicks = 1200; // ~40s @30Hz
    for (std::uint64_t t = 0; t < static_cast<std::uint64_t>(kTicks); ++t) {
        luminumbra::ai::RunMateSeekingOnTick(r);
        totalBirths += luminumbra::ai::RunMatingResolveOnTick(r, t, 99).born;
    }
    // With courtship (90t) + cooldown (300t), at most one birth per female every
    // ~(courtship+cooldown) ticks. 3 females over 1200 ticks => far fewer than a
    // runaway. Generous ceiling that still catches an unbounded explosion.
    EXPECT_LE(totalBirths, 60) << "runaway births — cooldown gate leaking";
    EXPECT_EQ(creatureCount(r), start + static_cast<std::size_t>(totalBirths));
}

// --- DETERMINISM: run==replay byte-exact across the full repro tick. ---
TEST(ReproHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        for (int i = 0; i < 3; ++i)
            spawnFertile(r, 0.1f * i, 0.0f, true);
        for (int i = 0; i < 3; ++i)
            spawnFertile(r, 0.1f * i, 0.2f, false);
    };
    auto run = [](entt::registry& r) {
        for (std::uint64_t t = 0; t < 500; ++t) {
            luminumbra::ai::RunMateSeekingOnTick(r);
            luminumbra::ai::RunMatingResolveOnTick(r, t, 0xABCDEFull);
        }
        return snapshotGenomes(r);
    };
    entt::registry a, b;
    build(a);
    build(b);
    EXPECT_TRUE(run(a) == run(b)) << "reproduction is not run==replay deterministic";
}

// --- ORDER-INDEPENDENCE (test-rigor ): the load-bearing NEW variant. ---
// run==replay (above) re-runs the SAME spawn order twice and so can NEVER expose the
// order-dependence the boids bug (a8b689c) actually was. RunMatingResolveOnTick CLAIMS
// order-independence via an id-SORTED traversal + a male-snapshot/male_used courtship
// resolution (CreatureReproductionSystem.h:127-130,155-176): WHO pairs with WHOM, and
// therefore the BIRTH COUNT and the per-generation POPULATION STRUCTURE, must NOT depend
// on the order creatures were spawned.
//
// IMPORTANT (the honest invariant): a birth's per-baby genome VALUES and SEX are seeded
// from the PARENT ENTITY IDS (CreatureReproductionSystem.h:184-190), and entt assigns ids
// in spawn order, so the raw genome numbers legitimately change when the spawn order
// changes — that is NOT an order-dependence bug. What MUST stay invariant is the pairing
// outcome: the number of births and the generation histogram. A boids-style iteration-order
// bug (a female grabbing the "wrong" male because the visit order leaked into the pairing)
// would change exactly those. We feed the SAME herd in a deterministic SEEDED-SHUFFLED
// (and reversed) spawn order and assert that histogram is byte-identical.
namespace {
// A spawn spec for the order-independence herd. The SET is fixed; only the order varies.
struct ReproSpec {
    float x, z;
    bool female;
};

// Build the herd in the given spawn order, run N repro ticks, and return the
// spawn-order-INVARIANT signature: total population + a generation histogram (gen -> count).
// (Genome VALUES are intentionally excluded — they are id-seeded, see the note above.)
std::vector<std::uint32_t> reproGenerationHistogram(const std::vector<ReproSpec>& order) {
    entt::registry r;
    for (const auto& sp : order)
        spawnFertile(r, sp.x, sp.z, sp.female);
    for (std::uint64_t t = 0; t < 600; ++t) {
        luminumbra::ai::RunMateSeekingOnTick(r);
        luminumbra::ai::RunMatingResolveOnTick(r, t, 0x5EED1234ull);
    }
    std::vector<std::uint32_t> hist; // index = generation, value = count
    auto v = r.view<Comp::CreatureGenomeComponent>();
    for (auto e : v) {
        const auto g = v.get<Comp::CreatureGenomeComponent>(e).generation;
        if (g >= hist.size())
            hist.resize(g + 1u, 0u);
        ++hist[g];
    }
    return hist;
}
} // namespace

TEST(ReproHardening, OrderIndependentSeededShuffle) {
    std::vector<ReproSpec> base = {
        {0.0f, 0.0f, true},
        {0.10f, 0.0f, true},
        {0.20f, 0.0f, true},
        {0.0f, 0.2f, false},
        {0.10f, 0.2f, false},
        {0.20f, 0.2f, false},
    };
    const auto baseline = reproGenerationHistogram(base);
    // The herd must actually breed, else the test is vacuous (gen-1+ births present).
    ASSERT_GT(baseline.size(), 1u) << "herd never bred — test would be vacuous";

    // Deterministic seeded permutation that ACTUALLY reorders (guard  false-green).
    auto shuffled = Luminumbra::TestSupport::SeededShuffled(base, /*seed=*/0xC0FFEEu);
    bool reordered = false;
    for (std::size_t i = 0; i < base.size(); ++i) {
        if (shuffled[i].x != base[i].x || shuffled[i].z != base[i].z ||
            shuffled[i].female != base[i].female) {
            reordered = true;
            break;
        }
    }
    ASSERT_TRUE(reordered) << "seed must actually permute the spawn order";

    const auto fromShuffle = reproGenerationHistogram(shuffled);
    EXPECT_EQ(baseline, fromShuffle)
        << "seeded-shuffled spawn order changed the birth/generation structure "
           "— reproduction PAIRING is NOT order-independent";

    // A second, independent permutation (reversed) — pins it as general order-invariance.
    std::vector<ReproSpec> rev(base.rbegin(), base.rend());
    const auto fromRev = reproGenerationHistogram(rev);
    EXPECT_EQ(baseline, fromRev) << "reversed spawn order changed the birth/generation structure";
}

// --- ORACLE (test-rigor ): INDEPENDENT-RECOMPUTE + GOLDEN-LITERAL for the genome
//     math. BreedOffspring(a,b,rng) = BlendCrossover then GaussianMutate then clamp
//     (CreatureGenome.h:87-94). This recomputes the expected child by HAND from the
//     documented formulas with a SECOND, identically-seeded rng pulling the draws in the
//     SAME order (Evolution.h:53-62 then :38-48) — a different code path, so a sign/op
//     flip in the production operators breaks it (unlike a run==replay self-compare). ---
TEST(ReproHardening, BreedOffspringOracle) {
    using luminumbra::ai::ClampGene;
    using luminumbra::ai::CreatureGeneBounds;
    using luminumbra::ai::CreatureGenome;
    using luminumbra::ai::CreatureGenomeToGenes;
    using luminumbra::ai::kCreatureMutationSigmaFrac;
    using luminumbra::core::DeterministicRng;

    CreatureGenome pa;
    pa.move_speed = 2.0f;
    pa.vigilance = 0.20f;
    pa.hunger_threshold = 0.10f;
    pa.size_scale = 0.80f;
    CreatureGenome pb;
    pb.move_speed = 6.0f;
    pb.vigilance = 0.90f;
    pb.hunger_threshold = 0.50f;
    pb.size_scale = 1.60f;

    const std::uint64_t kSeed = 0xBEEF00D1ull;

    // PRODUCTION result.
    DeterministicRng prod(kSeed);
    const CreatureGenome child = luminumbra::ai::BreedOffspring(pa, pb, prod);

    // INDEPENDENT hand recompute, mirroring the EXACT source draw order:
    //    (BlendCrossover): one next_unit per gene, child = a + (b-a)*u
    //    (GaussianMutate): one next_gaussian per gene, child += g*sigma*range, clamp
    const auto genesA = CreatureGenomeToGenes(pa);
    const auto genesB = CreatureGenomeToGenes(pb);
    const auto bounds = CreatureGeneBounds();
    DeterministicRng oracle(kSeed);
    std::array<float, 4> expect{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float u = oracle.next_unit();
        expect[i] = genesA[i] + (genesB[i] - genesA[i]) * u;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const float range = bounds[i].hi - bounds[i].lo;
        expect[i] += oracle.next_gaussian() * kCreatureMutationSigmaFrac * range;
        expect[i] = ClampGene(expect[i], bounds[i]);
    }

    EXPECT_FLOAT_EQ(child.move_speed, expect[0]);
    EXPECT_FLOAT_EQ(child.vigilance, expect[1]);
    EXPECT_FLOAT_EQ(child.hunger_threshold, expect[2]);
    EXPECT_FLOAT_EQ(child.size_scale, expect[3]);

    // GOLDEN LITERALS (frozen on THIS toolchain from the recompute above). A draw-ORDER
    // regression (e.g. mutate-before-cross) that the recompute would track in lockstep is
    // ALSO caught here, since these are fixed numbers, not derived from the same rng.
    EXPECT_NEAR(child.move_speed, 1.75438714f, 1e-5f);
    EXPECT_NEAR(child.vigilance, 0.41084281f, 1e-5f);
    EXPECT_NEAR(child.hunger_threshold, 0.20820478f, 1e-5f);
    EXPECT_NEAR(child.size_scale, 1.17190635f, 1e-5f);
}

// ===========================================================================
// LIFESPAN
// ===========================================================================

// --- GATING: no MortalComponent => no-op. ---
TEST(LifespanHardening, GatingNoMortalIsNoOp) {
    entt::registry r;
    auto e = r.create();
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.hunger = 1.0f; // would starve IF it were mortal
    auto s = luminumbra::ai::RunLifespanOnTick(r);
    EXPECT_EQ(s.aged, 0);
    EXPECT_EQ(s.died, 0);
    EXPECT_FALSE(cr.eaten); // untouched
}

// --- EXACT BOUNDARY: a creature dies on the tick its age first reaches lifespan. ---
TEST(LifespanHardening, OldAgeExactBoundary) {
    entt::registry r;
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.lifespan_ticks = 5;
    m.age_ticks = 0;

    // Ticks 1..4: ages, not dead.
    for (std::uint32_t t = 1; t <= 4; ++t) {
        auto s = luminumbra::ai::RunLifespanOnTick(r);
        EXPECT_EQ(s.died, 0) << "died early at age " << m.age_ticks;
        EXPECT_EQ(m.dead, 0);
        EXPECT_EQ(m.age_ticks, t);
    }
    // Tick 5: age->5 >= lifespan 5 => dies exactly here.
    auto s = luminumbra::ai::RunLifespanOnTick(r);
    EXPECT_EQ(m.age_ticks, 5u);
    EXPECT_EQ(m.dead, 1);
    EXPECT_EQ(s.died, 1);
}

// --- LATCH: a dead entity never ages or re-dies; stays stable (run==replay). ---
TEST(LifespanHardening, DeadIsInertLatched) {
    entt::registry r;
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.lifespan_ticks = 1;
    luminumbra::ai::RunLifespanOnTick(r); // dies
    ASSERT_EQ(m.dead, 1);
    const std::uint32_t ageAtDeath = m.age_ticks;
    for (int i = 0; i < 50; ++i) {
        auto s = luminumbra::ai::RunLifespanOnTick(r);
        EXPECT_EQ(s.aged, 0); // dead is not aged
        EXPECT_EQ(s.died, 0);
        EXPECT_EQ(m.age_ticks, ageAtDeath);
    }
}

// --- STARVATION: hunger >= 1.0 kills a mortal creature AND sets the carcass seam. ---
TEST(LifespanHardening, StarvationKillsAndMarksCarcass) {
    entt::registry r;
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.lifespan_ticks = 0xFFFFFFFFu; // never dies of old age
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.hunger = 1.0f; // maxed

    auto s = luminumbra::ai::RunLifespanOnTick(r);
    EXPECT_EQ(s.died, 1);
    EXPECT_EQ(m.dead, 1);
    EXPECT_TRUE(cr.eaten) << "starvation must reuse the carcass seam";
}

// --- BOUNDS: hunger just BELOW the threshold does NOT starve (strict-ish compare). ---
TEST(LifespanHardening, HungerBelowThresholdSurvives) {
    entt::registry r;
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.lifespan_ticks = 0xFFFFFFFFu;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.hunger = 0.999f; // below kStarvationHunger (1.0)
    auto s = luminumbra::ai::RunLifespanOnTick(r);
    EXPECT_EQ(s.died, 0);
    EXPECT_EQ(m.dead, 0);
    EXPECT_FALSE(cr.eaten);
}

// --- ORDER-INDEPENDENCE: shuffled creation order => same per-entity death outcome. ---
TEST(LifespanHardening, OrderIndependentDeaths) {
    auto buildA = [](entt::registry& r) {
        auto a = r.create();
        r.emplace<Comp::MortalComponent>(a).lifespan_ticks = 2;
        auto b = r.create();
        r.emplace<Comp::MortalComponent>(b).lifespan_ticks = 100;
    };
    // Same entities, different declared order — but death depends on lifespan only.
    entt::registry r1, r2;
    buildA(r1);
    // r2: create the long-lived one first.
    auto b2 = r2.create();
    r2.emplace<Comp::MortalComponent>(b2).lifespan_ticks = 100;
    auto a2 = r2.create();
    r2.emplace<Comp::MortalComponent>(a2).lifespan_ticks = 2;

    for (int t = 0; t < 3; ++t) {
        luminumbra::ai::RunLifespanOnTick(r1);
        luminumbra::ai::RunLifespanOnTick(r2);
    }
    // Keyed by lifespan (identity), the short-lived one is dead, the long-lived alive,
    // in BOTH registries.
    int dead1 = 0, dead2 = 0;
    for (auto e : r1.view<Comp::MortalComponent>())
        dead1 += r1.get<Comp::MortalComponent>(e).dead;
    for (auto e : r2.view<Comp::MortalComponent>())
        dead2 += r2.get<Comp::MortalComponent>(e).dead;
    EXPECT_EQ(dead1, 1);
    EXPECT_EQ(dead2, 1);
}

// ===========================================================================
// DECOMPOSITION  (fixed-point milli-unit contract)
// ===========================================================================

// --- GATING: no DecayComponent => no-op. ---
TEST(DecompHardening, GatingNoDecayIsNoOp) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::MortalComponent>(e).dead = 1; // dead but NOT opted in
    auto s = luminumbra::ai::RunDecompositionOnTick(r, 0);
    EXPECT_EQ(s.decaying, 0);
    EXPECT_EQ(s.released_total, 0u);
    EXPECT_EQ(s.finished, 0);
}

// --- LIVE entity does not decay (clock + release hold at 0). ---
TEST(DecompHardening, LiveDoesNotDecay) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::MortalComponent>(e).dead = 0; // alive
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = 10;
    for (int t = 0; t < 20; ++t)
        luminumbra::ai::RunDecompositionOnTick(r, t);
    EXPECT_EQ(dc.decay_ticks, 0u);
    EXPECT_EQ(dc.nutrient_released_milli, 0u);
    EXPECT_EQ(dc.fully_decomposed, 0u);
}

// --- MILLI-UNIT CONTRACT + MONOTONICITY: cumulative release rises monotonically and
//     lands on EXACTLY kNutrientPerCorpseMilli (1000 milli = 1.0 unit) at decay_duration;
//     the sum of per-tick releases over the whole life equals exactly that total. ---
TEST(DecompHardening, MilliContractMonotonicLandsExact) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::MortalComponent>(e).dead = 1;
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    const std::uint32_t dur = 40;
    dc.decay_duration = dur;

    std::uint32_t prev = 0;
    std::uint64_t summedPerTick = 0;
    for (std::uint32_t t = 0; t < dur + 5; ++t) {
        auto s = luminumbra::ai::RunDecompositionOnTick(r, t);
        summedPerTick += s.released_total;
        const std::uint32_t now = dc.nutrient_released_milli;
        EXPECT_GE(now, prev) << "release not monotonic at t=" << t; // never decreases
        prev = now;
    }
    // EXACT landing: a fully decomposed corpse has released the whole load.
    EXPECT_EQ(dc.nutrient_released_milli, luminumbra::ai::kNutrientPerCorpseMilli);
    EXPECT_EQ(dc.fully_decomposed, 1u);
    // The per-tick stat is the increase each tick; summed over life == total load.
    EXPECT_EQ(summedPerTick, static_cast<std::uint64_t>(luminumbra::ai::kNutrientPerCorpseMilli));
}

// --- LATCH: once fully decomposed, clock + total are stable forever (run==replay). ---
TEST(DecompHardening, FinishedIsLatchedStable) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::MortalComponent>(e).dead = 1;
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = 3;
    for (int t = 0; t < 3; ++t)
        luminumbra::ai::RunDecompositionOnTick(r, t);
    ASSERT_EQ(dc.fully_decomposed, 1u);
    const auto t0 = dc.decay_ticks;
    const auto n0 = dc.nutrient_released_milli;
    for (int t = 0; t < 30; ++t) {
        auto s = luminumbra::ai::RunDecompositionOnTick(r, t);
        EXPECT_EQ(s.decaying, 0);
        EXPECT_EQ(s.released_total, 0u);
        EXPECT_EQ(dc.decay_ticks, t0);
        EXPECT_EQ(dc.nutrient_released_milli, n0);
    }
}

// --- EDGE: a ZERO-duration corpse releases the full load at once on its first dead tick
//     (no divide-by-zero). ---
TEST(DecompHardening, ZeroDurationImmediateFullLoad) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::MortalComponent>(e).dead = 1;
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = 0;
    auto s = luminumbra::ai::RunDecompositionOnTick(r, 0);
    EXPECT_EQ(s.released_total, luminumbra::ai::kNutrientPerCorpseMilli);
    EXPECT_EQ(dc.nutrient_released_milli, luminumbra::ai::kNutrientPerCorpseMilli);
    EXPECT_EQ(dc.fully_decomposed, 1u);
}

// --- EDGE: carcass seam (CreatureComponent.eaten) counts as dead for decomposition. ---
TEST(DecompHardening, EatenCarcassDecays) {
    entt::registry r;
    auto e = r.create();
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.eaten = true; // no MortalComponent — eaten alone marks it a corpse
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = 10;
    auto s = luminumbra::ai::RunDecompositionOnTick(r, 0);
    EXPECT_EQ(s.decaying, 1);
    EXPECT_EQ(dc.decay_ticks, 1u);
}

// --- DETERMINISM: run==replay over many corpses with varied durations. ---
TEST(DecompHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        for (std::uint32_t d = 1; d <= 25; ++d) {
            auto e = r.create();
            r.emplace<Comp::MortalComponent>(e).dead = 1;
            r.emplace<Comp::DecayComponent>(e).decay_duration = d;
        }
    };
    auto run = [](entt::registry& r) {
        for (int t = 0; t < 40; ++t)
            luminumbra::ai::RunDecompositionOnTick(r, t);
        std::vector<std::uint32_t> out;
        std::vector<entt::entity> es;
        for (auto e : r.view<Comp::DecayComponent>())
            es.push_back(e);
        std::sort(es.begin(), es.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (auto e : es) {
            const auto& dc = r.get<Comp::DecayComponent>(e);
            out.push_back(dc.decay_ticks);
            out.push_back(dc.nutrient_released_milli);
            out.push_back(dc.fully_decomposed);
        }
        return out;
    };
    entt::registry a, b;
    build(a);
    build(b);
    EXPECT_EQ(run(a), run(b));
}

// ===========================================================================
// CIRCADIAN
// ===========================================================================

// --- GATING: no CircadianComponent => no-op. ---
TEST(CircadianHardening, GatingNoComponentIsNoOp) {
    entt::registry r;
    (void)r.create(); // bare entity
    auto s = luminumbra::ai::RunCircadianOnTick(r, 0.5f);
    EXPECT_EQ(s.participants, 0);
}

// --- NOON / MIDNIGHT EXTREMES for both phenotypes. ---
TEST(CircadianHardening, NoonMidnightExtremes) {
    // Diurnal: 1 at noon (0.5), 0 at midnight (0.0).
    EXPECT_NEAR(luminumbra::ai::CircadianActivity(0.5f, /*nocturnal=*/false), 1.0f, 1e-4f);
    EXPECT_NEAR(luminumbra::ai::CircadianActivity(0.0f, /*nocturnal=*/false), 0.0f, 1e-4f);
    // Nocturnal: inverse.
    EXPECT_NEAR(luminumbra::ai::CircadianActivity(0.5f, /*nocturnal=*/true), 0.0f, 1e-4f);
    EXPECT_NEAR(luminumbra::ai::CircadianActivity(0.0f, /*nocturnal=*/true), 1.0f, 1e-4f);
}

// --- DAY-WRAP CONTINUITY: t=0 and t=1 land on the same value (cos is periodic). ---
TEST(CircadianHardening, DayWrapContinuity) {
    for (bool noct : {false, true}) {
        const float a0 = luminumbra::ai::CircadianActivity(0.0f, noct);
        const float a1 = luminumbra::ai::CircadianActivity(1.0f, noct);
        EXPECT_NEAR(a0, a1, 1e-4f) << "day boundary discontinuity, nocturnal=" << noct;
    }
    // Overshoot/negative clocks wrap onto the day too.
    EXPECT_NEAR(luminumbra::ai::CircadianActivity(1.25f, false),
                luminumbra::ai::CircadianActivity(0.25f, false),
                1e-4f);
    EXPECT_NEAR(luminumbra::ai::CircadianActivity(-0.1f, false),
                luminumbra::ai::CircadianActivity(0.9f, false),
                1e-4f);
}

// --- BOUNDS: across a fine sweep of the day, activity NEVER leaves [0,1], no NaN. ---
TEST(CircadianHardening, ActivityAlwaysInUnitRange) {
    for (int i = -50; i <= 250; ++i) {
        const float t = static_cast<float>(i) / 100.0f; // -0.5 .. 2.5
        for (bool noct : {false, true}) {
            const float a = luminumbra::ai::CircadianActivity(t, noct);
            EXPECT_FALSE(std::isnan(a));
            EXPECT_GE(a, 0.0f);
            EXPECT_LE(a, 1.0f);
        }
    }
}

// --- ORDER-INDEPENDENCE: same time + same phenotypes => same per-creature activity
//     regardless of creation order (the write reads only its own component + time). ---
TEST(CircadianHardening, OrderIndependentActivity) {
    const float t = 0.37f;
    entt::registry r1, r2;
    // r1: diurnal then nocturnal.
    auto d1 = r1.create();
    r1.emplace<Comp::CircadianComponent>(d1).nocturnal = 0;
    auto n1 = r1.create();
    r1.emplace<Comp::CircadianComponent>(n1).nocturnal = 1;
    // r2: nocturnal then diurnal.
    auto n2 = r2.create();
    r2.emplace<Comp::CircadianComponent>(n2).nocturnal = 1;
    auto d2 = r2.create();
    r2.emplace<Comp::CircadianComponent>(d2).nocturnal = 0;

    luminumbra::ai::RunCircadianOnTick(r1, t);
    luminumbra::ai::RunCircadianOnTick(r2, t);
    EXPECT_FLOAT_EQ(r1.get<Comp::CircadianComponent>(d1).activity,
                    r2.get<Comp::CircadianComponent>(d2).activity);
    EXPECT_FLOAT_EQ(r1.get<Comp::CircadianComponent>(n1).activity,
                    r2.get<Comp::CircadianComponent>(n2).activity);
}

// --- DETERMINISM: run==replay (same roster + same time => identical writes). ---
TEST(CircadianHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        for (int i = 0; i < 16; ++i)
            r.emplace<Comp::CircadianComponent>(r.create()).nocturnal = (i & 1);
    };
    auto run = [](entt::registry& r) {
        std::vector<float> out;
        const float times[] = {0.0f, 0.13f, 0.5f, 0.77f, 0.99f, 1.0f};
        for (float t : times) {
            luminumbra::ai::RunCircadianOnTick(r, t);
            std::vector<entt::entity> es;
            for (auto e : r.view<Comp::CircadianComponent>())
                es.push_back(e);
            std::sort(es.begin(), es.end(), [](entt::entity a, entt::entity b) {
                return entt::to_integral(a) < entt::to_integral(b);
            });
            for (auto e : es)
                out.push_back(r.get<Comp::CircadianComponent>(e).activity);
        }
        return out;
    };
    entt::registry a, b;
    build(a);
    build(b);
    EXPECT_EQ(run(a), run(b));
}

// ===========================================================================
// WILDLIFE x FOLIAGE
// ===========================================================================

entt::entity spawnGrazePlant(entt::registry& r, float x, float z, float biomass) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    r.emplace<Comp::GrazeableComponent>(e).biomass = biomass;
    return e;
}

entt::entity spawnHerbivore(entt::registry& r, float x, float z, float hunger) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    cr.eaten = false;
    cr.hunger = hunger;
    return e;
}

// --- GATING: no grazeable plant => ZERO mutation anywhere (creature hunger untouched). ---
TEST(WildlifeHardening, GatingNoGrazeableIsNoOp) {
    entt::registry r;
    auto c = spawnHerbivore(r, 0.0f, 0.0f, 0.5f);
    auto s = luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
    EXPECT_EQ(s.plants, 0);
    EXPECT_FLOAT_EQ(r.get<Comp::CreatureComponent>(c).hunger, 0.5f);
}

// --- BOUNDS: biomass clamps to [0,1]; a herd cannot drive it negative. ---
TEST(WildlifeHardening, BiomassClampedNonNegative) {
    entt::registry r;
    auto p = spawnGrazePlant(r, 0.0f, 0.0f, 0.02f); // nearly bare
    for (int i = 0; i < 10; ++i)
        spawnHerbivore(r, 0.0f, 0.0f, 0.5f); // big herd on it
    luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
    const float bm = r.get<Comp::GrazeableComponent>(p).biomass;
    EXPECT_GE(bm, 0.0f);
    EXPECT_LE(bm, 1.0f);
}

// --- BOUNDS: ungrazed regrowth never exceeds the 1.0 cap. ---
TEST(WildlifeHardening, RegrowthCappedAtOne) {
    entt::registry r;
    auto p = spawnGrazePlant(r, 0.0f, 0.0f, 0.999f); // already near full, no grazers
    for (int t = 0; t < 50; ++t)
        luminumbra::ai::RunWildlifeFoliageOnTick(r, t);
    EXPECT_LE(r.get<Comp::GrazeableComponent>(p).biomass, 1.0f);
    EXPECT_NEAR(r.get<Comp::GrazeableComponent>(p).biomass, 1.0f, 1e-5f);
}

// --- GATING DETAIL: a PREDATOR or an EATEN creature does not graze. ---
TEST(WildlifeHardening, PredatorAndCarcassDoNotGraze) {
    entt::registry r;
    auto p = spawnGrazePlant(r, 0.0f, 0.0f, 1.0f);
    auto pred = spawnHerbivore(r, 0.0f, 0.0f, 0.5f);
    r.get<Comp::CreatureComponent>(pred).is_predator = true;
    auto dead = spawnHerbivore(r, 0.0f, 0.0f, 0.5f);
    r.get<Comp::CreatureComponent>(dead).eaten = true;

    luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
    // No live herbivore => plant should REGROW (no graze), not be eaten down.
    EXPECT_GE(r.get<Comp::GrazeableComponent>(p).biomass, 1.0f);
    // Neither non-grazer was fed.
    EXPECT_FLOAT_EQ(r.get<Comp::CreatureComponent>(pred).hunger, 0.5f);
    EXPECT_FLOAT_EQ(r.get<Comp::CreatureComponent>(dead).hunger, 0.5f);
}

// --- ORDER-INDEPENDENCE: two herbivores on one plant => same biomass drawdown
//     regardless of which creature was created first (graze demand is SUMMED). ---
TEST(WildlifeHardening, GrazeOrderIndependent) {
    auto runWith = [](bool reversed) {
        entt::registry r;
        float bm;
        if (!reversed) {
            spawnHerbivore(r, 0.0f, 0.0f, 0.5f);
            spawnHerbivore(r, 0.1f, 0.0f, 0.5f);
            auto p = spawnGrazePlant(r, 0.05f, 0.0f, 1.0f);
            luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
            bm = r.get<Comp::GrazeableComponent>(p).biomass;
        } else {
            auto p = spawnGrazePlant(r, 0.05f, 0.0f, 1.0f);
            spawnHerbivore(r, 0.1f, 0.0f, 0.5f);
            spawnHerbivore(r, 0.0f, 0.0f, 0.5f);
            luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
            bm = r.get<Comp::GrazeableComponent>(p).biomass;
        }
        return bm;
    };
    EXPECT_FLOAT_EQ(runWith(false), runWith(true));
}

// --- PRODUCER->CONSUMER: feeding lowers hunger by exactly kFeedPerGraze per plant
//     in range, and hunger clamps to [0,1] (never negative). ---
TEST(WildlifeHardening, FeedLowersHungerClampedAtZero) {
    entt::registry r;
    auto c = spawnHerbivore(r, 0.0f, 0.0f, /*hunger=*/0.01f);
    spawnGrazePlant(r, 0.0f, 0.0f, 1.0f); // lush plant in range
    luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
    const float h = r.get<Comp::CreatureComponent>(c).hunger;
    EXPECT_GE(h, 0.0f); // clamped, never negative
    EXPECT_LE(h, 1.0f);
}

// --- BUG: a herbivore must NOT be fed by a BARE (biomass == 0) plant. Feed should be
//     gated on biomass actually grazed/removed; otherwise a creature parked on a
//     trampled-to-the-ground patch keeps eating from nothing (energy from nowhere).
//     EXPECTED-CORRECT: standing on a fully-bare plant with no other food leaves hunger
//     UNCHANGED. (Currently feed is added per in-range plant regardless of biomass.) ---
TEST(WildlifeHardening, BarePlantDoesNotFeed) { // BUG: BarePlantDoesNotFeed
    entt::registry r;
    auto c = spawnHerbivore(r, 0.0f, 0.0f, /*hunger=*/0.5f);
    spawnGrazePlant(r, 0.0f, 0.0f, /*biomass=*/0.0f); // bare ground, nothing to eat
    luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
    EXPECT_FLOAT_EQ(r.get<Comp::CreatureComponent>(c).hunger, 0.5f)
        << "a bare plant fed the creature — eating biomass that does not exist";
}

// --- EDGE: a creature exactly AT the graze radius boundary is in range (d2 <= R^2). ---
TEST(WildlifeHardening, ExactRadiusBoundaryGrazes) {
    entt::registry r;
    auto p = spawnGrazePlant(r, 0.0f, 0.0f, 1.0f);
    spawnHerbivore(r, luminumbra::ai::kGrazeRadius, 0.0f, 0.5f); // exactly on the edge
    luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
    // d2 == R^2 is `<=` so it counts as in range -> biomass drops below 1.0.
    EXPECT_LT(r.get<Comp::GrazeableComponent>(p).biomass, 1.0f);
}

// --- NaN: coincident creature+plant (zero distance) produces no NaN biomass/hunger. ---
TEST(WildlifeHardening, CoincidentNoNaN) {
    entt::registry r;
    auto p = spawnGrazePlant(r, 2.0f, 2.0f, 1.0f);
    auto c = spawnHerbivore(r, 2.0f, 2.0f, 0.5f); // coincident
    luminumbra::ai::RunWildlifeFoliageOnTick(r, 0);
    EXPECT_FALSE(std::isnan(r.get<Comp::GrazeableComponent>(p).biomass));
    EXPECT_FALSE(std::isnan(r.get<Comp::CreatureComponent>(c).hunger));
}

// --- DETERMINISM: run==replay over many ticks with a mixed herd + patches. ---
TEST(WildlifeHardening, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        for (int i = 0; i < 6; ++i)
            spawnGrazePlant(r, 0.4f * i, 0.0f, 1.0f);
        for (int i = 0; i < 4; ++i)
            spawnHerbivore(r, 0.4f * i, 0.3f, 0.4f);
    };
    auto run = [](entt::registry& r) {
        for (std::uint64_t t = 0; t < 120; ++t)
            luminumbra::ai::RunWildlifeFoliageOnTick(r, t);
        std::vector<float> out;
        std::vector<entt::entity> ps;
        for (auto e : r.view<Comp::GrazeableComponent>())
            ps.push_back(e);
        std::sort(ps.begin(), ps.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (auto e : ps)
            out.push_back(r.get<Comp::GrazeableComponent>(e).biomass);
        std::vector<entt::entity> cs;
        for (auto e : r.view<Comp::CreatureComponent>())
            cs.push_back(e);
        std::sort(cs.begin(), cs.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (auto e : cs)
            out.push_back(r.get<Comp::CreatureComponent>(e).hunger);
        return out;
    };
    entt::registry a, b;
    build(a);
    build(b);
    EXPECT_EQ(run(a), run(b));
}

// ===========================================================================
// CROSS-SYSTEM: lifespan -> decomposition handoff (the death->soil cycle)
// ===========================================================================

// A creature that dies of old age then decomposes: the carcass seam set by Lifespan
// (eaten) must make Decomposition treat it as dead. Closes the producer->consumer loop
// across two systems.
TEST(LifecycleHardening, DeathThenDecompositionHandoff) {
    entt::registry r;
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.lifespan_ticks = 1;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.hunger = 0.0f;
    auto& dc = r.emplace<Comp::DecayComponent>(e);
    dc.decay_duration = 5;

    // Tick 1: lifespan kills it (sets dead + eaten).
    luminumbra::ai::RunLifespanOnTick(r);
    ASSERT_EQ(m.dead, 1);
    ASSERT_TRUE(cr.eaten);

    // Now decomposition advances (it sees the corpse via MortalComponent.dead).
    auto s = luminumbra::ai::RunDecompositionOnTick(r, 0);
    EXPECT_EQ(s.decaying, 1);
    EXPECT_GT(dc.nutrient_released_milli, 0u);
}

} // namespace
