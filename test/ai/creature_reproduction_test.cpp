// Creature evolution: SEXUAL creature reproduction: a male + a female must find each other and
// COURT before a baby is born. Deterministic (id-ordered, seeded-from-ints, libm-free), gated by
// the CreatureGenomeComponent opt-in.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "../support/UniqueTempPath.h"
#include "ai/CreatureReproductionSystem.h"
#include "ai/DecompositionSystem.h"
#include "ai/EcologyHash.h"
#include "ai/LifespanSystem.h"
#include "ai/ScavengingSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/InstinctComponents.h" // PerceptionComponent
#include "persistence/PlantPersistence.h"
#include "replay/ReplayStream.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::BreedOffspring;
using luminumbra::ai::BreedSensoryInto;
using luminumbra::ai::CreatureGenome;
using luminumbra::ai::CreatureSensoryGeneBounds;
using luminumbra::ai::kCourtshipTicks;
using luminumbra::ai::RunMateSeekingOnTick;
using luminumbra::ai::RunMatingResolveOnTick;

// Spawn a prey with a genome. `ready` => mature, well-fed, healthy, off cooldown.
entt::entity spawnMate(entt::registry& r, float x, float z, bool female, bool ready = true) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
    gn.female = female;
    if (ready) {
        gn.age_ticks = 120; // mature (>= kReproMaturityTicks)
        gn.reproduce_cooldown = 0;
        cr.hunger = 0.05f; // <= hunger_threshold (0.3)
        cr.stamina = 1.0f; // >= healthy floor
    } else {
        gn.age_ticks = 0; // too young
    }
    return e;
}

std::size_t creatureCount(entt::registry& r) {
    return r.view<Comp::CreatureComponent>().size();
}

// Empty roster: both phases are no-ops.
TEST(CreatureReproduction, EmptyRosterNoOp) {
    entt::registry r;
    RunMateSeekingOnTick(r);
    EXPECT_EQ(RunMatingResolveOnTick(r, 1).born, 0);
}

// A creature without a genome never mates.
TEST(CreatureReproduction, NoGenomeIgnored) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    r.emplace<Comp::CreatureComponent>(e); // no CreatureGenomeComponent
    for (int i = 0; i < 200; ++i)
        RunMatingResolveOnTick(r, static_cast<std::uint64_t>(i));
    EXPECT_EQ(creatureCount(r), 1u);
}

// An adjacent ready male+female court for the duration, then produce exactly one baby; both
// parents go on cooldown afterwards.
TEST(CreatureReproduction, AdjacentPairCourtsThenBreeds) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    const entt::entity male = spawnMate(r, 1.0f, 0.0f, /*female*/ false); // within courtship radius
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 1) << "courtship should yield exactly one baby";
    EXPECT_EQ(creatureCount(r), 3u);
    EXPECT_GT(r.get<Comp::CreatureGenomeComponent>(female).reproduce_cooldown, 0u);
    EXPECT_GT(r.get<Comp::CreatureGenomeComponent>(male).reproduce_cooldown, 0u);
}

// A ready female with NO male never breeds and never accumulates courtship.
TEST(CreatureReproduction, LoneFemaleDoesNotBreed) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 50; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
    EXPECT_EQ(r.get<Comp::CreatureGenomeComponent>(female).courting_ticks, 0u);
}

// Two ready FEMALES (same sex) never breed.
TEST(CreatureReproduction, SameSexDoesNotBreed) {
    entt::registry r;
    spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    spawnMate(r, 1.0f, 0.0f, /*female*/ true);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 10; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
}

// Immature creatures don't court (age gate).
TEST(CreatureReproduction, YoungPairDoesNotBreed) {
    entt::registry r;
    spawnMate(r, 0.0f, 0.0f, /*female*/ true, /*ready*/ false);
    spawnMate(r, 1.0f, 0.0f, /*female*/ false, /*ready*/ false);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 5; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
}

// Mate seeking: a ready female steers TOWARD a ready male within sense range (+x).
TEST(CreatureReproduction, MateSeekingSteersTowardMate) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    spawnMate(r, 12.0f, 0.0f, /*female*/ false); // far but within kMateSeekRadius
    RunMateSeekingOnTick(r);
    EXPECT_GT(r.get<Comp::CreatureComponent>(female).wish_x, 0.0f)
        << "should steer toward the mate";
}

// Offspring genome is a blend of the two parents (between their move_speeds).
TEST(CreatureReproduction, OffspringGenomeBlendsParents) {
    entt::registry r;
    auto f = spawnMate(r, 0.0f, 0.0f, true);
    auto m = spawnMate(r, 1.0f, 0.0f, false);
    r.get<Comp::CreatureGenomeComponent>(f).move_speed = 2.0f;
    r.get<Comp::CreatureGenomeComponent>(m).move_speed = 6.0f;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
        RunMatingResolveOnTick(r, t);
    float childSpeed = -1.0f;
    for (auto e : r.view<Comp::CreatureGenomeComponent>()) {
        const auto& gn = r.get<Comp::CreatureGenomeComponent>(e);
        if (gn.generation == 1u)
            childSpeed = gn.move_speed;
    }
    ASSERT_GT(childSpeed, 0.0f) << "a child should exist";
    EXPECT_GT(childSpeed, 2.0f - 1.0f);
    EXPECT_LT(childSpeed, 6.0f + 1.0f); // blended between parents (+ small mutation)
}

// the WORLD SEED feeds the per-birth RNG. Same-seed
// worlds reproduce byte-identically; DIFFERENT seeds diverge (offspring genomes
// were previously identical across worlds with the same entity ids + ticks,
// because GameSession passed world_seed 0).
TEST(CreatureReproduction, WorldSeedDrivesOffspringGenomes) {
    auto run = [](std::uint64_t world_seed) {
        entt::registry r;
        auto f = spawnMate(r, 0.0f, 0.0f, true);
        auto m = spawnMate(r, 1.0f, 0.0f, false);
        r.get<Comp::CreatureGenomeComponent>(f).move_speed = 2.0f;
        r.get<Comp::CreatureGenomeComponent>(m).move_speed = 6.0f;
        for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
            RunMatingResolveOnTick(r, t, world_seed);
        std::vector<float> out;
        for (auto e : r.view<Comp::CreatureGenomeComponent>()) {
            const auto& gn = r.get<Comp::CreatureGenomeComponent>(e);
            if (gn.generation == 1u) {
                out.insert(
                    out.end(),
                    {gn.move_speed, gn.vision_range, gn.hearing_range, gn.vision_cos_half_fov});
            }
        }
        return out;
    };
    const auto seed_a1 = run(424242ull);
    const auto seed_a2 = run(424242ull);
    const auto seed_b = run(999999ull);
    ASSERT_FALSE(seed_a1.empty()) << "no offspring born (vacuous)";
    EXPECT_EQ(seed_a1, seed_a2) << "same-seed worlds must reproduce identically";
    EXPECT_NE(seed_a1, seed_b) << "different world seeds must diverge offspring genomes";
}

// run == replay: identical setup + ticks -> identical population + genomes.
TEST(CreatureReproduction, Deterministic) {
    auto run = [] {
        entt::registry r;
        spawnMate(r, 0.0f, 0.0f, true);
        spawnMate(r, 1.0f, 0.0f, false);
        for (std::uint32_t t = 0; t < kCourtshipTicks + 20; ++t)
            RunMatingResolveOnTick(r, t);
        std::vector<float> out;
        out.push_back(static_cast<float>(r.view<Comp::CreatureComponent>().size()));
        for (auto e : r.view<Comp::CreatureGenomeComponent>())
            out.push_back(r.get<Comp::CreatureGenomeComponent>(e).move_speed);
        return out;
    };
    EXPECT_EQ(run(), run());
}

// the SENSORY genes inherit as a bounded blend+mutate and are deterministic for a given rng.
TEST(CreatureReproduction, SensoryGenesInheritWithinBoundsAndDeterministic) {
    CreatureGenome a;
    a.vision_cos_half_fov = 0.30f;
    a.vision_range = 10.0f;
    a.hearing_range = 12.0f;
    CreatureGenome b;
    b.vision_cos_half_fov = 0.90f;
    b.vision_range = 40.0f;
    b.hearing_range = 38.0f;
    const auto bounds = CreatureSensoryGeneBounds();
    auto breed = [&](std::uint64_t seed) {
        luminumbra::core::DeterministicRng rng =
            luminumbra::core::DeterministicRng::seeded(seed, 1, 2);
        return BreedSensoryInto(CreatureGenome{}, a, b, rng);
    };
    const CreatureGenome c1 = breed(7);
    const CreatureGenome c2 = breed(7);
    EXPECT_FLOAT_EQ(c1.vision_cos_half_fov, c2.vision_cos_half_fov); // run==replay
    EXPECT_FLOAT_EQ(c1.vision_range, c2.vision_range);
    EXPECT_FLOAT_EQ(c1.hearing_range, c2.hearing_range);
    EXPECT_GE(c1.vision_cos_half_fov, bounds[0].lo);
    EXPECT_LE(c1.vision_cos_half_fov, bounds[0].hi);
    EXPECT_GE(c1.vision_range, bounds[1].lo);
    EXPECT_LE(c1.vision_range, bounds[1].hi);
    EXPECT_GE(c1.hearing_range, bounds[2].lo);
    EXPECT_LE(c1.hearing_range, bounds[2].hi);
}

//  determinism guard: breeding the sensory genes (drawn AFTER the core breed + sex draw) leaves
// the 4-gene CORE genome and the sex bit byte-identical -> the ecology hash (move_speed/gen/age) is
// unchanged for genome rosters, so this slice needs no re-pin.
TEST(CreatureReproduction, SensoryBreedingDoesNotPerturbCoreGenomeOrSex) {
    CreatureGenome a;
    a.move_speed = 2.5f;
    a.vision_range = 11.0f;
    CreatureGenome b;
    b.move_speed = 6.5f;
    b.vision_range = 39.0f;
    // Path 1: core breed + sex only (the pre- stream).
    luminumbra::core::DeterministicRng r1 = luminumbra::core::DeterministicRng::seeded(9, 3, 4);
    const CreatureGenome core1 = BreedOffspring(a, b, r1);
    const bool sex1 = (r1.next_u64() & 1ull) == 0ull;
    // Path 2: same stream, then sensory breeding appended after the sex draw.
    luminumbra::core::DeterministicRng r2 = luminumbra::core::DeterministicRng::seeded(9, 3, 4);
    CreatureGenome core2 = BreedOffspring(a, b, r2);
    const bool sex2 = (r2.next_u64() & 1ull) == 0ull;
    core2 = BreedSensoryInto(core2, a, b, r2);
    EXPECT_FLOAT_EQ(core1.move_speed, core2.move_speed);
    EXPECT_FLOAT_EQ(core1.vigilance, core2.vigilance);
    EXPECT_FLOAT_EQ(core1.hunger_threshold, core2.hunger_threshold);
    EXPECT_FLOAT_EQ(core1.size_scale, core2.size_scale);
    EXPECT_EQ(sex1, sex2);
}

// a born offspring is stamped with a PerceptionComponent expressed from its genome.
TEST(CreatureReproduction, OffspringPerceptionStampedFromGenome) {
    entt::registry r;
    auto f = spawnMate(r, 0.0f, 0.0f, true);
    auto m = spawnMate(r, 1.0f, 0.0f, false);
    // Distinct sensory genes so the child's values are clearly genome-driven (and in-bounds).
    auto& fg = r.get<Comp::CreatureGenomeComponent>(f);
    fg.vision_cos_half_fov = 0.85f;
    fg.vision_range = 35.0f;
    fg.hearing_range = 30.0f;
    auto& mg = r.get<Comp::CreatureGenomeComponent>(m);
    mg.vision_cos_half_fov = 0.80f;
    mg.vision_range = 33.0f;
    mg.hearing_range = 28.0f;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
        RunMatingResolveOnTick(r, t);
    entt::entity child = entt::null;
    for (auto e : r.view<Comp::CreatureGenomeComponent>())
        if (r.get<Comp::CreatureGenomeComponent>(e).generation == 1u)
            child = e;
    ASSERT_TRUE(child != entt::null)
        << "a child should exist"; // boolean form: no entt::null_t printer
    ASSERT_TRUE(r.all_of<Comp::PerceptionComponent>(child))
        << "offspring must carry PerceptionComponent";
    const auto& gn = r.get<Comp::CreatureGenomeComponent>(child);
    const auto& pc = r.get<Comp::PerceptionComponent>(child);
    EXPECT_FLOAT_EQ(pc.vision_cos_half_fov, gn.vision_cos_half_fov); // expressed from the genome
    EXPECT_FLOAT_EQ(pc.vision_range, gn.vision_range);
    EXPECT_FLOAT_EQ(pc.ear.range, gn.hearing_range);
}

// Test-only reference: the original id-ordered strict-nearest scan, including
// the used-male exclusion. Kept independent of the spatial accelerator.
entt::entity bruteForceMate(const entt::registry& registry,
                            const std::vector<entt::entity>& ordered,
                            entt::entity self,
                            float radius,
                            const luminumbra::ai::ReproductionTuning& tuning,
                            const std::vector<entt::entity>& used = {}) {
    const auto& creature = registry.get<Comp::CreatureComponent>(self);
    const auto& genome = registry.get<Comp::CreatureGenomeComponent>(self);
    const auto& transform = registry.get<Comp::TransformComponent>(self);
    entt::entity best = entt::null;
    float distance = radius;
    for (const auto other : ordered) {
        const auto& other_creature = registry.get<Comp::CreatureComponent>(other);
        const auto& other_genome = registry.get<Comp::CreatureGenomeComponent>(other);
        if (other == self || other_genome.female == genome.female ||
            other_creature.species_id != creature.species_id ||
            !luminumbra::ai::IsReadyToMate(other_creature, other_genome, tuning) ||
            std::find(used.begin(), used.end(), other) != used.end()) {
            continue;
        }
        const auto& position = registry.get<Comp::TransformComponent>(other).position;
        const float dx = position.x - transform.position.x;
        const float dz = position.z - transform.position.z;
        const float candidate_distance = luminumbra::ai::dm::Sqrt(dx * dx + dz * dz);
        if (candidate_distance < distance) {
            distance = candidate_distance;
            best = other;
        }
    }
    return best;
}

TEST(CreatureReproduction, SpatialPartnersAndBirthsMatchBruteForce) {
    namespace AI = luminumbra::ai;
    for (const float radius : {0.0f,
                               -1.0f,
                               0.001f,
                               2.6f,
                               45.0f,
                               std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
        entt::registry registry;
        auto rng = luminumbra::core::DeterministicRng::seeded(985, 7, 0);
        AI::ReproductionTuning tuning;
        tuning.courtship_ticks = 1;
        tuning.cooldown_ticks = 0;
        tuning.courtship_radius = 2.6f;
        tuning.mate_seek_radius = radius;
        tuning.species_capacity = 1000;
        std::vector<entt::entity> ordered;
        // Integer lattice includes coincident and equidistant choices across cell edges.
        for (std::uint32_t i = 0; i < 256; ++i) {
            const float x = static_cast<float>(rng.next_u64() % 33) - 16.0f;
            const float z = static_cast<float>(rng.next_u64() % 33) - 16.0f;
            const auto entity = spawnMate(registry, x, z, (i % 2) == 0, (i % 11) != 0);
            auto& creature = registry.get<Comp::CreatureComponent>(entity);
            creature.species_id = static_cast<std::uint16_t>(i % 3);
            creature.wish_x = 17.0f;
            creature.wish_z = -19.0f;
            creature.last_action = (i % 17) == 0 ? static_cast<int>(AI::CreatureAction::Flee) : 0;
            auto& genome = registry.get<Comp::CreatureGenomeComponent>(entity);
            genome.move_speed = 1.0f + static_cast<float>(i) / 256.0f;
            genome.generation = i;
            if (i == 2) {
                registry.get<Comp::TransformComponent>(entity).position.x =
                    std::numeric_limits<float>::quiet_NaN();
            }
            if (i == 5) {
                registry.get<Comp::TransformComponent>(entity).position.x =
                    std::numeric_limits<float>::infinity();
            }
            if (i == 7) {
                registry.get<Comp::TransformComponent>(entity).position.x =
                    -std::numeric_limits<float>::max();
            }
            if (i == 3) {
                registry.get<Comp::TransformComponent>(entity).position.z =
                    std::numeric_limits<float>::max();
            }
            ordered.push_back(entity);
        }
        RunMateSeekingOnTick(registry, tuning);
        for (const auto entity : ordered) {
            const auto& creature = registry.get<Comp::CreatureComponent>(entity);
            const auto& genome = registry.get<Comp::CreatureGenomeComponent>(entity);
            float wish_x = 17.0f;
            float wish_z = -19.0f;
            const auto mate = bruteForceMate(registry, ordered, entity, radius, tuning);
            if (AI::IsReadyToMate(creature, genome, tuning) &&
                creature.last_action != static_cast<int>(AI::CreatureAction::Flee) &&
                mate != entt::null) {
                const auto& from = registry.get<Comp::TransformComponent>(entity).position;
                const auto& to = registry.get<Comp::TransformComponent>(mate).position;
                const float dx = to.x - from.x;
                const float dz = to.z - from.z;
                const float distance = AI::dm::Sqrt(dx * dx + dz * dz);
                wish_x = 0.0f;
                wish_z = 0.0f;
                if (distance > tuning.courtship_radius && distance > 1.0e-5f) {
                    const float inverse = creature.move_speed / distance;
                    wish_x = dx * inverse;
                    wish_z = dz * inverse;
                }
            }
            EXPECT_EQ(creature.wish_x, wish_x);
            EXPECT_EQ(creature.wish_z, wish_z);
        }
        tuning.courtship_radius = radius;
        // Resolve ages first. Model that independently so newly mature creatures qualify.
        for (const auto entity : ordered) {
            ++registry.get<Comp::CreatureGenomeComponent>(entity).age_ticks;
        }
        std::vector<std::pair<entt::entity, entt::entity>> pairs;
        std::vector<entt::entity> used;
        for (const auto entity : ordered) {
            const auto& creature = registry.get<Comp::CreatureComponent>(entity);
            const auto& genome = registry.get<Comp::CreatureGenomeComponent>(entity);
            if (!genome.female || !AI::IsReadyToMate(creature, genome, tuning)) {
                continue;
            }
            const auto mate = bruteForceMate(registry, ordered, entity, radius, tuning, used);
            if (mate != entt::null) {
                pairs.emplace_back(entity, mate);
                used.push_back(mate);
            }
        }
        for (const auto entity : ordered) {
            --registry.get<Comp::CreatureGenomeComponent>(entity).age_ticks;
        }
        const auto stats = RunMatingResolveOnTick(registry, 123, 985, tuning);
        ASSERT_EQ(stats.born, static_cast<int>(pairs.size()));
        std::vector<entt::entity> children;
        for (const auto entity : registry.view<Comp::CreatureGenomeComponent>()) {
            if (registry.get<Comp::CreatureGenomeComponent>(entity).age_ticks == 0) {
                children.push_back(entity);
            }
        }
        std::sort(children.begin(), children.end());
        ASSERT_EQ(children.size(), pairs.size());
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            const auto [mother, father] = pairs[i];
            const auto toGenome = [&](entt::entity entity) {
                const auto& g = registry.get<Comp::CreatureGenomeComponent>(entity);
                return CreatureGenome{g.move_speed,
                                      g.vigilance,
                                      g.hunger_threshold,
                                      g.size_scale,
                                      g.vision_cos_half_fov,
                                      g.vision_range,
                                      g.hearing_range};
            };
            auto birth_rng = luminumbra::core::DeterministicRng::seeded(
                AI::kReproSeedOffset ^ 985,
                (static_cast<std::uint64_t>(entt::to_integral(mother)) * 0x9E3779B97F4A7C15ull) ^
                    static_cast<std::uint64_t>(entt::to_integral(father)),
                123);
            const auto maternal = toGenome(mother);
            const auto paternal = toGenome(father);
            auto expected = BreedOffspring(maternal, paternal, birth_rng);
            const bool female = (birth_rng.next_u64() & 1ull) == 0;
            expected = BreedSensoryInto(expected, maternal, paternal, birth_rng);
            const auto& child = registry.get<Comp::CreatureGenomeComponent>(children[i]);
            EXPECT_EQ(child.move_speed, expected.move_speed);
            EXPECT_EQ(child.vigilance, expected.vigilance);
            EXPECT_EQ(child.hunger_threshold, expected.hunger_threshold);
            EXPECT_EQ(child.size_scale, expected.size_scale);
            EXPECT_EQ(child.vision_range, expected.vision_range);
            EXPECT_EQ(child.hearing_range, expected.hearing_range);
            EXPECT_EQ(child.vision_cos_half_fov, expected.vision_cos_half_fov);
            EXPECT_EQ(child.female, female);
            EXPECT_EQ(child.generation,
                      std::max(registry.get<Comp::CreatureGenomeComponent>(mother).generation,
                               registry.get<Comp::CreatureGenomeComponent>(father).generation) +
                          1);
            const auto& from = registry.get<Comp::TransformComponent>(mother).position;
            const auto& to = registry.get<Comp::TransformComponent>(father).position;
            const auto& position = registry.get<Comp::TransformComponent>(children[i]).position;
            EXPECT_EQ(position.x, (from.x + to.x) * 0.5f);
            EXPECT_EQ(position.z, (from.z + to.z) * 0.5f + tuning.spawn_radius);
        }
    }
}

TEST(CreatureReproduction, SpatialTieBreakKeepsHandleAndStableIdentityOrdering) {
    namespace AI = luminumbra::ai;
    for (const bool stable_identity : {false, true}) {
        entt::registry registry;
        const auto mother = spawnMate(registry, 0, 0, true);
        const auto positive = spawnMate(registry, 2, 0, false);
        const auto negative = spawnMate(registry, -2, 0, false);
        registry.emplace<Comp::SimulationIdentityComponent>(mother, std::uint64_t{10});
        registry.emplace<Comp::SimulationIdentityComponent>(positive, std::uint64_t{30});
        registry.emplace<Comp::SimulationIdentityComponent>(negative, std::uint64_t{20});
        AI::ReproductionTuning tuning;
        tuning.mate_seek_radius = 3;
        tuning.courtship_radius = 1;
        tuning.courtship_ticks = 1;
        AI::RunMateSeekingOnTick(registry, tuning, nullptr, stable_identity);
        const float direction = stable_identity ? -1.0f : 1.0f;
        EXPECT_EQ(registry.get<Comp::CreatureComponent>(mother).wish_x,
                  direction * registry.get<Comp::CreatureComponent>(mother).move_speed);
        tuning.courtship_radius = 3;
        Luminumbra::world::ActiveRegionLedger ledger;
        ASSERT_TRUE(ledger.reserve_persistent_ids(40));
        ASSERT_EQ(
            AI::RunMatingResolveOnTick(
                registry, 1, 985, tuning, nullptr, nullptr, stable_identity ? &ledger : nullptr)
                .born,
            1);
        const auto chosen = stable_identity ? negative : positive;
        const auto unchosen = stable_identity ? positive : negative;
        EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(chosen).reproduce_cooldown,
                  tuning.cooldown_ticks);
        EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(unchosen).reproduce_cooldown, 0u);
    }
}

TEST(CreatureReproduction, ExactRadiusIsExcludedInBothSpatialQueries) {
    entt::registry registry;
    const auto mother = spawnMate(registry, 0, 0, true);
    spawnMate(registry, 2, 0, false);
    registry.get<Comp::CreatureComponent>(mother).wish_x = 7;
    luminumbra::ai::ReproductionTuning tuning;
    tuning.mate_seek_radius = 2;
    tuning.courtship_radius = 2;
    tuning.courtship_ticks = 1;
    RunMateSeekingOnTick(registry, tuning);
    EXPECT_EQ(registry.get<Comp::CreatureComponent>(mother).wish_x, 7);
    EXPECT_EQ(RunMatingResolveOnTick(registry, 1, 985, tuning).born, 0);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(mother).courting_ticks, 0u);
}

TEST(CreatureReproduction, CapacityRefusesLaterIdsAndCountsOnlyAdmittedLivingSpecies) {
    namespace AI = luminumbra::ai;
    entt::registry registry;
    const auto first = spawnMate(registry, 0, 0, true);
    const auto second = spawnMate(registry, 0, 0, true);
    const auto male = spawnMate(registry, 0, 0, false);
    const auto unused_male = spawnMate(registry, 0, 0, false);
    const auto other_species = spawnMate(registry, 0, 0, true);
    registry.get<Comp::CreatureComponent>(other_species).species_id = 1;
    const auto other_male = spawnMate(registry, 0, 0, false);
    registry.get<Comp::CreatureComponent>(other_male).species_id = 1;
    const auto dead = spawnMate(registry, 0, 0, false);
    registry.get<Comp::CreatureComponent>(dead).eaten = true;
    const auto mortal_dead = registry.create();
    registry.emplace<Comp::CreatureComponent>(mortal_dead);
    registry.emplace<Comp::MortalComponent>(mortal_dead).dead = 1;
    registry.emplace<Comp::TransformComponent>(mortal_dead);
    const auto blocked = spawnMate(registry, 513, 0, false);
    const std::array due{Luminumbra::world::RegionKey{0, 0}};
    const Luminumbra::world::RegionConsumerGate gate(registry, due);
    AI::CreatureTickAdmission admission;
    admission.Capture(registry, gate);
    // A non-genome creature occupies the final slot after the first birth.
    const auto non_genome = registry.create();
    registry.emplace<Comp::CreatureComponent>(non_genome);
    AI::ReproductionTuning tuning;
    tuning.courtship_ticks = 1;
    tuning.species_capacity = 6;
    EXPECT_EQ(RunMatingResolveOnTick(registry, 1, 0, tuning, nullptr, &admission).born, 2);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(first).reproduce_cooldown,
              tuning.cooldown_ticks);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(male).reproduce_cooldown,
              tuning.cooldown_ticks);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(second).reproduce_cooldown, 0u);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(second).courting_ticks, 0u);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(unused_male).reproduce_cooldown, 0u);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(blocked).age_ticks, 120u);
    // Move the male off cooldown: the full species still refuses without consuming him.
    registry.get<Comp::CreatureGenomeComponent>(male).reproduce_cooldown = 0;
    EXPECT_EQ(RunMatingResolveOnTick(registry, 2, 0, tuning, nullptr, &admission).born, 0);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(male).reproduce_cooldown, 0u);
    for (const std::uint32_t capacity : {0u, 1u}) {
        tuning.species_capacity = capacity;
        EXPECT_EQ(RunMatingResolveOnTick(registry, 3, 0, tuning).born, 0);
    }
}

TEST(CreatureReproduction, NoGenomesPreserveHashAndDoNoCandidateWork) {
    entt::registry registry;
    EXPECT_TRUE(luminumbra::ai::ComputeEcologySubHash(registry).empty());
    auto entity = registry.create();
    registry.emplace<Comp::CreatureComponent>(entity);
    registry.emplace<Comp::TransformComponent>(entity);
    const auto before = luminumbra::ai::ComputeEcologySubHash(registry);
    const auto seeking = RunMateSeekingOnTick(registry);
    const auto resolve = RunMatingResolveOnTick(registry, 1);
    EXPECT_EQ(seeking.mate_candidate_visits, 0u);
    EXPECT_EQ(resolve.mate_candidate_visits, 0u);
    EXPECT_EQ(resolve.population_visits, 0u);
    EXPECT_EQ(resolve.born, 0);
    EXPECT_EQ(luminumbra::ai::ComputeEcologySubHash(registry), before);
}

// Every state in the retirement predicate, including partial nutrient release and
// both death encodings. Untagged historical carcasses must keep their exact hash.
TEST(CreatureReproduction, CleanupPreservesIneligibleEntitiesAndProjectionBytes) {
    namespace AI = luminumbra::ai;
    entt::registry registry;
    EXPECT_EQ(AI::CleanupNewbornCarcasses(registry).visited, 0u);
    const auto historical = spawnMate(registry, 0, 0, false);
    registry.emplace<Comp::MortalComponent>(historical).dead = 1;
    auto& historical_decay = registry.emplace<Comp::DecayComponent>(historical);
    historical_decay.fully_decomposed = 1;
    historical_decay.nutrient_released_milli = AI::kNutrientPerCorpseMilli;
    const auto initial_hash = AI::ComputeEcologySubHash(registry);
    EXPECT_EQ(AI::CleanupNewbornCarcasses(registry).visited, 0u);
    EXPECT_EQ(AI::ComputeEcologySubHash(registry), initial_hash);
    for (int state = 0; state < 6; ++state) {
        SCOPED_TRACE(state);
        const auto child = spawnMate(registry, 0, 0, true);
        const auto unmarked_hash = AI::ComputeEcologySubHash(registry);
        const auto unmarked_snapshot = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
            luminumbra::foliage::BuildPlantEntitySnapshot(registry));
        registry.emplace<AI::ReproductionBorn>(child);
        EXPECT_EQ(AI::ComputeEcologySubHash(registry), unmarked_hash);
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                      luminumbra::foliage::BuildPlantEntitySnapshot(registry)),
                  unmarked_snapshot);
        if (state != 0) {
            auto& decay = registry.emplace<Comp::DecayComponent>(child);
            decay.fully_decomposed = state >= 2 ? 1 : 0;
            decay.nutrient_released_milli = state >= 3 ? AI::kNutrientPerCorpseMilli : 999u;
        }
        if (state == 1 || state == 2 || state == 4) {
            registry.emplace<Comp::MortalComponent>(child).dead = 1;
        } else if (state == 5) {
            registry.get<Comp::CreatureComponent>(child).eaten = true;
        }
        const auto hash = AI::ComputeEcologySubHash(registry);
        const auto bytes = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
            luminumbra::foliage::BuildPlantEntitySnapshot(registry));
        const auto cleanup = AI::CleanupNewbornCarcasses(registry);
        EXPECT_EQ(cleanup.removed, state >= 4 ? 1u : 0u);
        EXPECT_EQ(registry.valid(child), state < 4);
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                      luminumbra::foliage::BuildPlantEntitySnapshot(registry)),
                  bytes);
        if (state < 4) {
            EXPECT_EQ(AI::ComputeEcologySubHash(registry), hash);
            registry.destroy(child);
        }
    }
    EXPECT_TRUE(registry.valid(historical));
}

struct RetirementObserver {
    std::vector<entt::entity> order;
    entt::entity watcher = entt::null;
    void Destroying(entt::registry& registry, entt::entity entity) {
        EXPECT_TRUE(registry.valid(entity));
        EXPECT_TRUE(registry.ctx().get<luminumbra::ai::CreatureTickAdmission>().Allows(entity));
        const auto& senses = registry.get<Comp::SensesComponent>(watcher);
        EXPECT_TRUE(senses.nearest_food_source == entt::null);
        EXPECT_TRUE(senses.nearest_water_source == entt::null);
        EXPECT_TRUE(senses.nearest_threat == entt::null);
        for (const auto& action : registry.get<Comp::ActionPlanComponent>(watcher).plan) {
            EXPECT_TRUE(action.target == entt::null);
        }
        const auto& hierarchy = registry.get<Comp::HierarchyComponent>(watcher);
        EXPECT_TRUE(hierarchy.parent == entt::null);
        EXPECT_TRUE(hierarchy.children.empty());
        order.push_back(entity);
    }
};

TEST(CreatureReproduction, CleanupReleasesReferencesBeforeOrderedDestructionAndIgnoresWorldSize) {
    namespace AI = luminumbra::ai;
    entt::registry registry;
    // Unrelated entities never enter the cleanup traversal or scratch allocation.
    for (int index = 0; index < 4096; ++index) {
        registry.emplace<Comp::TransformComponent>(registry.create());
    }
    spawnMate(registry, 0, 0, true);
    spawnMate(registry, 0, 0, false);
    AI::ReproductionTuning tuning;
    tuning.courtship_ticks = 1;
    tuning.cooldown_ticks = 0;
    ASSERT_EQ(RunMatingResolveOnTick(registry, 1, 985, tuning).born, 1);
    ASSERT_EQ(RunMatingResolveOnTick(registry, 2, 985, tuning).born, 1);
    std::vector<entt::entity> children;
    for (const auto entity : registry.view<AI::ReproductionBorn>()) {
        children.push_back(entity);
        registry.get<Comp::CreatureComponent>(entity).eaten = true;
        registry.get<Comp::DecayComponent>(entity).decay_duration = 0;
    }
    ASSERT_EQ(children.size(), 2u);
    std::sort(children.begin(), children.end());
    const auto watcher = spawnMate(registry, 0, 0, false);
    registry.emplace<Comp::ScavengerComponent>(watcher);
    registry.get<Comp::CreatureComponent>(watcher).hunger = 0.8f;
    auto& senses = registry.emplace<Comp::SensesComponent>(watcher);
    senses.nearest_food_source = children[0];
    senses.nearest_water_source = children[1];
    senses.nearest_threat = children[0];
    auto& plan = registry.emplace<Comp::ActionPlanComponent>(watcher);
    plan.plan.push_back(Comp::Action{.name = "eat", .target = children[0]});
    plan.plan.push_back(Comp::Action{.name = "eat", .target = children[1]});
    auto& hierarchy = registry.emplace<Comp::HierarchyComponent>(watcher);
    hierarchy.parent = children[0];
    hierarchy.children = children;
    const auto unaffected = registry.create();
    auto& other_senses = registry.emplace<Comp::SensesComponent>(unaffected);
    other_senses.nearest_food_source = watcher;
    other_senses.nearest_threat = watcher;
    auto& other_plan = registry.emplace<Comp::ActionPlanComponent>(unaffected);
    other_plan.plan.push_back(Comp::Action{.name = "seek", .target = watcher});
    other_plan.plan.push_back(Comp::Action{.name = "idle", .target = entt::null});
    auto& other_hierarchy = registry.emplace<Comp::HierarchyComponent>(unaffected);
    other_hierarchy.parent = watcher;
    other_hierarchy.children = {watcher};
    EXPECT_EQ(AI::RunScavengingOnTick(registry, 3).carcasses, 2);
    EXPECT_EQ(AI::RunDecompositionOnTick(registry, 3).finished, 2);
    // Hash and every other tick consumer may still read the complete roster.
    EXPECT_FALSE(AI::ComputeEcologySubHash(registry).empty());
    for (const auto child : children) {
        EXPECT_TRUE(registry.valid(child));
    }
    auto& admission = registry.ctx().emplace<AI::CreatureTickAdmission>();
    const Luminumbra::world::RegionConsumerGate gate(registry, {});
    admission.Capture(registry, gate);
    const auto admitted_owners = admission.owner_visits();
    admission.Forget(entt::null);
    EXPECT_EQ(admission.owner_visits(), admitted_owners);
    for (const auto child : children) {
        EXPECT_FALSE(admission.Allows(child));
    }
    RetirementObserver observer{.order = {}, .watcher = watcher};
    registry.on_destroy<AI::ReproductionBorn>().connect<&RetirementObserver::Destroying>(observer);
    const auto result = AI::CleanupNewbornCarcasses(registry);
    registry.on_destroy<AI::ReproductionBorn>().disconnect<&RetirementObserver::Destroying>(
        observer);
    EXPECT_EQ(result.visited, 2u);
    EXPECT_EQ(result.reference_visits, 13u);
    EXPECT_EQ(result.removed, 2u);
    EXPECT_EQ(admission.owner_visits(), admitted_owners - 2u);
    EXPECT_EQ(observer.order, children);
    EXPECT_EQ(other_senses.nearest_food_source, watcher);
    EXPECT_TRUE(other_senses.nearest_water_source == entt::null);
    EXPECT_EQ(other_senses.nearest_threat, watcher);
    EXPECT_EQ(other_plan.plan[0].target, watcher);
    EXPECT_TRUE(other_plan.plan[1].target == entt::null);
    EXPECT_EQ(other_hierarchy.parent, watcher);
    EXPECT_EQ(other_hierarchy.children, std::vector<entt::entity>{watcher});
    EXPECT_EQ(AI::RunScavengingOnTick(registry, 4).carcasses, 0);
    EXPECT_EQ(AI::RunDecompositionOnTick(registry, 4).decaying, 0);
    EXPECT_EQ(AI::CleanupNewbornCarcasses(registry).visited, 0u);
    // Recycled handles must never reconnect the old senses/plans/hierarchy.
    const auto replacement = registry.create();
    EXPECT_NE(replacement, children[0]);
    EXPECT_NE(replacement, children[1]);
}

TEST(CreatureReproduction, CleanupEcologyHashesMatchTwoRunsAndRecordedInputReplay) {
    namespace AI = luminumbra::ai;
    namespace Replay = Luminumbra::Replay;
    std::vector<Replay::InputRecord> recording;
    AI::ReproductionTuning tuning;
    tuning.maturity_ticks = 1;
    tuning.cooldown_ticks = 2;
    tuning.courtship_ticks = 1;
    tuning.spawn_radius = 0;
    tuning.species_capacity = 8;
    tuning.lifespan_ticks = 5;
    const auto run = [&](bool playback, std::uint64_t seed) {
        entt::registry registry;
        spawnMate(registry, 0, 0, true);
        spawnMate(registry, 0, 0, false);
        std::vector<std::string> hashes;
        std::size_t removed = 0;
        for (std::uint64_t tick = 0; tick < 2000; ++tick) {
            const Replay::InputRecord input =
                playback ? recording[static_cast<std::size_t>(tick)]
                         : Replay::InputRecord{
                               .tick = tick, .inputs = {static_cast<std::uint8_t>(tick % 7 == 0)}};
            if (!playback && recording.size() == tick) {
                recording.push_back(input);
            }
            for (const auto entity : registry.view<Comp::CreatureComponent>()) {
                registry.get<Comp::CreatureComponent>(entity).hunger =
                    input.inputs[0] ? 0.2f : 0.0f;
            }
            RunMateSeekingOnTick(registry, tuning);
            RunMatingResolveOnTick(registry, input.tick, seed, tuning);
            // Short synthetic decay exercises retirement throughout replay while
            // keeping each hash small; the long-decay bound has its own regression.
            for (const auto entity : registry.view<AI::ReproductionBorn, Comp::DecayComponent>()) {
                registry.get<Comp::DecayComponent>(entity).decay_duration = 7;
            }
            AI::RunLifespanOnTick(registry, input.tick);
            AI::RunDecompositionOnTick(registry, input.tick);
            removed += AI::CleanupNewbornCarcasses(registry).removed;
            hashes.push_back(AI::ComputeEcologySubHash(registry));
        }
        EXPECT_GT(removed, 0u);
        return hashes;
    };
    const auto first = run(false, 985);
    EXPECT_EQ(first, run(false, 985));
    const Luminumbra::TestSupport::ScopedTempFile temporary("newborn_replay_", ".lrec");
    Replay::ReplayHeader header;
    header.seed = 985;
    header.seed_string = "985";
    Replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(temporary.Path().string(), header));
    for (const auto& input : recording) {
        writer.RecordInput(input.tick, input.inputs);
    }
    ASSERT_TRUE(writer.Finalize(recording.size()));
    const auto replay = Replay::ReadReplay(temporary.Path().string());
    if (!replay.has_value()) {
        ADD_FAILURE() << "Recorded replay could not be read";
        return;
    }
    ASSERT_FALSE(replay->truncated);
    ASSERT_EQ(replay->inputs.size(), first.size());
    recording = replay->inputs;
    for (const auto& input : recording) {
        ASSERT_EQ(input.inputs.size(), 1u);
    }
    EXPECT_EQ(first, run(true, replay->header.seed));
}

} // namespace
