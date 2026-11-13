#include <gtest/gtest.h>

#include "ai/CreatureBrainSystem.h"
#include "ai/CreatureReproductionSystem.h"
#include "components/InstinctComponents.h"
#include "components/SimulationIdentityComponent.h"
#include "world/ActiveRegionLedger.h"

#include <entt/entity/registry.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace Comp = Luminumbra::Components;
namespace ai = luminumbra::ai;
using Luminumbra::world::ActiveRegionLedger;
constexpr float kDt = 1.0f / 30.0f;
constexpr std::uint64_t kSeed = 1337;
constexpr std::uint64_t kTick = 37;

// Reverse creation, recycled generations, live holes and unrelated mesh-like residents
// must never influence any candidate identity path.
void Churn(entt::registry& registry) {
    std::vector<entt::entity> holes;
    for (int index = 0; index < 19; ++index)
        holes.push_back(registry.create());
    for (const auto entity : holes)
        registry.destroy(entity);
    for (int index = 0; index < 11; ++index)
        registry.emplace<Comp::TransformComponent>(registry.create());
}

entt::entity Spawn(entt::registry& registry,
                   std::uint64_t id,
                   float x = 0.0f,
                   bool female = false,
                   bool predator = false) {
    const auto entity = registry.create();
    registry.emplace<Comp::SimulationIdentityComponent>(entity, id);
    registry.emplace<Comp::TransformComponent>(entity).position = Luminumbra::Vec3(x, 0, 0);
    auto& creature = registry.emplace<Comp::CreatureComponent>(entity);
    creature.is_predator = predator;
    creature.hunger = 0.05f;
    auto& genome = registry.emplace<Comp::CreatureGenomeComponent>(entity);
    genome.female = female;
    genome.age_ticks = 120;
    return entity;
}

ai::ReproductionTuning InstantCourtship() {
    return {.courtship_ticks = 1};
}

ai::CreatureReproductionStats Resolve(entt::registry& registry,
                                      ActiveRegionLedger& ledger,
                                      const ai::ReproductionTuning& tuning = InstantCourtship()) {
    return ai::RunMatingResolveOnTick(registry, kTick, kSeed, tuning, nullptr, nullptr, &ledger);
}

ai::CreatureBrainStats Brain(entt::registry& registry, bool indexed = true) {
    return ai::RunCreatureBrainSystemOnTick(
        registry, kDt, {}, nullptr, 0, 0, 0, indexed, nullptr, nullptr, true);
}

std::array<float, 7> Genes(const Comp::CreatureGenomeComponent& genome) {
    return {genome.move_speed,
            genome.vigilance,
            genome.hunger_threshold,
            genome.size_scale,
            genome.vision_cos_half_fov,
            genome.vision_range,
            genome.hearing_range};
}

void ExpectSameGenome(const Comp::CreatureGenomeComponent& actual,
                      const Comp::CreatureGenomeComponent& expected) {
    EXPECT_EQ(Genes(actual), Genes(expected));
    EXPECT_EQ(actual.age_ticks, expected.age_ticks);
    EXPECT_EQ(actual.reproduce_cooldown, expected.reproduce_cooldown);
    EXPECT_EQ(actual.courting_ticks, expected.courting_ticks);
    EXPECT_EQ(actual.generation, expected.generation);
    EXPECT_EQ(actual.female, expected.female);
}

TEST(CreatureIdentity, WanderIgnoresHandlesAndUsesStableIdentity) {
    entt::registry original;
    entt::registry churned;
    Churn(churned);
    constexpr std::uint64_t id = (std::uint64_t{1} << 48) + 13;
    const auto first = Spawn(original, id, 0, false, true);
    const auto second = Spawn(churned, id, 0, false, true);
    ASSERT_NE(first, second);
    for (int tick = 0; tick < 20; ++tick) {
        EXPECT_EQ(Brain(original).identity_visits, 1u);
        EXPECT_EQ(Brain(churned).identity_visits, 1u);
        const auto& a = original.get<Comp::CreatureComponent>(first);
        const auto& b = churned.get<Comp::CreatureComponent>(second);
        ASSERT_EQ(a.last_action, static_cast<int>(ai::CreatureAction::Wander));
        EXPECT_EQ(a.wish_x, b.wish_x);
        EXPECT_EQ(a.wish_z, b.wish_z);
        EXPECT_EQ(original.get<Comp::TransformComponent>(first).position.x,
                  churned.get<Comp::TransformComponent>(second).position.x);
        EXPECT_EQ(original.get<Comp::TransformComponent>(first).position.z,
                  churned.get<Comp::TransformComponent>(second).position.z);
    }
    const float angle = 2.39996323f * static_cast<float>(id % 16u + 1u);
    const auto& creature = original.get<Comp::CreatureComponent>(first);
    // The brain normalizes its direction before multiplying by speed.
    const float dx = Luminumbra::DeterministicMath::Cos(angle);
    const float dz = Luminumbra::DeterministicMath::Sin(angle);
    const float length = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);
    const float heading_x = dx / length;
    const float heading_z = dz / length;
    const float inverse =
        1.0f / Luminumbra::DeterministicMath::Sqrt(heading_x * heading_x + heading_z * heading_z);
    EXPECT_EQ(creature.wish_x, heading_x * inverse * creature.move_speed);
    EXPECT_EQ(creature.wish_z, heading_z * inverse * creature.move_speed);
}

TEST(CreatureIdentity, NearestTargetTiesAgreeAcrossHandlesAndBothPerceptionPaths) {
    for (const bool indexed : {false, true}) {
        for (const bool reverse : {false, true}) {
            entt::registry registry;
            if (reverse)
                Churn(registry);
            const auto hunter = Spawn(registry, 100, 0, false, true);
            registry.get<Comp::CreatureComponent>(hunter).hunger = 0.9f;
            if (reverse) {
                Spawn(registry, 300, -4);
                Spawn(registry, 200, 4);
            } else {
                Spawn(registry, 200, 4);
                Spawn(registry, 300, -4);
            }
            EXPECT_EQ(Brain(registry, indexed).identity_visits, 3u);
            const auto& creature = registry.get<Comp::CreatureComponent>(hunter);
            ASSERT_EQ(creature.last_action, static_cast<int>(ai::CreatureAction::Hunt));
            EXPECT_GT(creature.wish_x, 0);
            EXPECT_EQ(creature.wish_z, 0);
        }
    }
}

TEST(CreatureIdentity, BothParentSeedsUseAllIdentityBitsAndPreserveArgumentOrder) {
    // Vary each parent separately, including high bits. Creation order and live
    // handle generations also vary. The independent RNG oracle specifies both inputs.
    for (const auto mother_id : {std::uint64_t{17}, (std::uint64_t{1} << 40) + 17}) {
        for (const auto father_id : {std::uint64_t{29}, (std::uint64_t{1} << 44) + 29}) {
            for (const bool reverse : {false, true}) {
                entt::registry registry;
                entt::entity mother = entt::null;
                entt::entity father = entt::null;
                if (reverse) {
                    Churn(registry);
                    father = Spawn(registry, father_id, 1);
                    mother = Spawn(registry, mother_id, 0, true);
                } else {
                    mother = Spawn(registry, mother_id, 0, true);
                    father = Spawn(registry, father_id, 1);
                }
                registry.get<Comp::CreatureGenomeComponent>(mother).move_speed = 2;
                registry.get<Comp::CreatureGenomeComponent>(father).move_speed = 6;
                ai::CreatureGenome mother_genes;
                mother_genes.move_speed = 2;
                ai::CreatureGenome father_genes;
                father_genes.move_speed = 6;
                auto rng = luminumbra::core::DeterministicRng::seeded(
                    16u ^ kSeed, (mother_id * 0x9E3779B97F4A7C15ull) ^ father_id, kTick);
                auto expected = ai::BreedOffspring(mother_genes, father_genes, rng);
                const bool expected_female = (rng.next_u64() & 1u) == 0;
                expected = ai::BreedSensoryInto(expected, mother_genes, father_genes, rng);
                ActiveRegionLedger ledger;
                constexpr auto cursor = std::uint64_t{1} << 50;
                ASSERT_TRUE(ledger.reserve_persistent_ids(cursor));
                ASSERT_EQ(Resolve(registry, ledger).born, 1);
                EXPECT_EQ(ledger.next_persistent_id(), cursor + 1);
                int children = 0;
                for (const auto child : registry.view<Comp::CreatureGenomeComponent>()) {
                    const auto& genome = registry.get<Comp::CreatureGenomeComponent>(child);
                    if (genome.generation != 1)
                        continue;
                    ++children;
                    EXPECT_EQ(registry.get<Comp::SimulationIdentityComponent>(child).id, cursor);
                    EXPECT_EQ(Genes(genome),
                              (std::array<float, 7>{expected.move_speed,
                                                    expected.vigilance,
                                                    expected.hunger_threshold,
                                                    expected.size_scale,
                                                    expected.vision_cos_half_fov,
                                                    expected.vision_range,
                                                    expected.hearing_range}));
                    EXPECT_EQ(genome.female, expected_female);
                    EXPECT_EQ(genome.age_ticks, 0u);
                    const auto& perception = registry.get<Comp::PerceptionComponent>(child);
                    EXPECT_EQ(perception.vision_range, expected.vision_range);
                    EXPECT_EQ(perception.ear.range, expected.hearing_range);
                }
                EXPECT_EQ(children, 1);
            }
        }
    }
}

TEST(CreatureIdentity, MateSeekingAndResolutionPreferLowerStableIdOnDistanceTies) {
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        if (reverse)
            Churn(registry);
        const auto mother = Spawn(registry, 80, 0, true);
        const auto first = Spawn(registry, reverse ? 70 : 60, reverse ? -8.0f : 8.0f);
        const auto second = Spawn(registry, reverse ? 60 : 70, reverse ? 8.0f : -8.0f);
        ai::RunMateSeekingOnTick(registry, {}, nullptr, true);
        EXPECT_GT(registry.get<Comp::CreatureComponent>(mother).wish_x, 0);
        registry.get<Comp::TransformComponent>(first).position.x /= 8;
        registry.get<Comp::TransformComponent>(second).position.x /= 8;
        ActiveRegionLedger ledger;
        ASSERT_TRUE(ledger.reserve_persistent_ids(100));
        ASSERT_EQ(Resolve(registry, ledger).born, 1);
        const auto chosen = reverse ? second : first;
        const auto unchosen = reverse ? first : second;
        EXPECT_GT(registry.get<Comp::CreatureGenomeComponent>(chosen).reproduce_cooldown, 0u);
        EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(unchosen).reproduce_cooldown, 0u);
    }
}

TEST(CreatureIdentity, SimultaneousBirthsAllocateSharedIdsInMotherOrder) {
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        if (reverse)
            Churn(registry);
        for (int index = 0; index < 2; ++index) {
            const int pair = reverse ? 1 - index : index;
            const float x = static_cast<float>(pair) * 20;
            Spawn(registry, 10u + static_cast<std::uint64_t>(pair), x, true);
            Spawn(registry, 20u + static_cast<std::uint64_t>(pair), x + 1);
        }
        ActiveRegionLedger ledger;
        ASSERT_TRUE(ledger.reserve_persistent_ids(100));
        // Reserve one identity for a different authoritative kind in the same stream.
        std::uint64_t ground_id = 0;
        ASSERT_TRUE(ledger.allocate_persistent_id(ground_id));
        EXPECT_EQ(ground_id, 100u);
        const auto stats = Resolve(registry, ledger);
        ASSERT_EQ(stats.born, 2);
        EXPECT_EQ(stats.identity_visits, 4u);
        EXPECT_EQ(ledger.next_persistent_id(), 103u);
        int children = 0;
        for (const auto entity : registry.view<Comp::CreatureGenomeComponent>()) {
            if (registry.get<Comp::CreatureGenomeComponent>(entity).generation != 1)
                continue;
            ++children;
            const auto id = registry.get<Comp::SimulationIdentityComponent>(entity).id;
            const auto x = registry.get<Comp::TransformComponent>(entity).position.x;
            EXPECT_EQ(id, x < 10 ? 101u : 102u);
        }
        EXPECT_EQ(children, 2);
    }
}

TEST(CreatureIdentity, ContestedMaleGoesToLowerMotherIdentity) {
    entt::registry registry;
    const auto later = Spawn(registry, 90, -1, true);
    const auto earlier = Spawn(registry, 80, 1, true);
    Spawn(registry, 70);
    ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.reserve_persistent_ids(100));
    ASSERT_EQ(Resolve(registry, ledger).born, 1);
    EXPECT_GT(registry.get<Comp::CreatureGenomeComponent>(earlier).reproduce_cooldown, 0u);
    EXPECT_EQ(registry.get<Comp::CreatureGenomeComponent>(later).reproduce_cooldown, 0u);
}

TEST(CreatureIdentity, ExhaustionRefusesWholeBatchBeforeAnyParentMutation) {
    for (const auto remaining : {std::uint64_t{0}, std::uint64_t{1}}) {
        entt::registry registry;
        std::vector<entt::entity> parents;
        for (int pair = 0; pair < 2; ++pair) {
            const auto id = static_cast<std::uint64_t>(pair) * 2 + 1;
            const float x = static_cast<float>(pair) * 20;
            parents.push_back(Spawn(registry, id, x, true));
            parents.push_back(Spawn(registry, id + 1, x + 1));
        }
        // A non-breeding parent also must not age or decrement cooldown on refusal.
        const auto idle = Spawn(registry, 5, 100);
        registry.get<Comp::CreatureGenomeComponent>(idle).reproduce_cooldown = 17;
        parents.push_back(idle);
        std::vector<Comp::CreatureGenomeComponent> before;
        for (const auto entity : parents)
            before.push_back(registry.get<Comp::CreatureGenomeComponent>(entity));
        ActiveRegionLedger ledger;
        ASSERT_TRUE(
            ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit - remaining));
        const auto bytes = ledger.encode();
        EXPECT_THROW(Resolve(registry, ledger), std::length_error);
        EXPECT_EQ(ledger.encode(), bytes);
        EXPECT_EQ(registry.view<Comp::CreatureComponent>().size(), parents.size());
        for (std::size_t index = 0; index < parents.size(); ++index)
            ExpectSameGenome(registry.get<Comp::CreatureGenomeComponent>(parents[index]),
                             before[index]);
    }
}

TEST(CreatureIdentity, LastAvailableIdentitySucceedsAndIdleTicksConsumeNoIds) {
    entt::registry registry;
    Spawn(registry, 1, 0, true);
    Spawn(registry, 2, 1);
    ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit - 1));
    EXPECT_EQ(Resolve(registry, ledger).born, 1);
    EXPECT_EQ(ledger.next_persistent_id(), ActiveRegionLedger::kPersistentIdLimit);
    int children = 0;
    for (const auto entity : registry.view<Comp::CreatureGenomeComponent>()) {
        if (registry.get<Comp::CreatureGenomeComponent>(entity).generation == 1) {
            ++children;
            EXPECT_EQ(registry.get<Comp::SimulationIdentityComponent>(entity).id,
                      ActiveRegionLedger::kPersistentIdLimit - 1);
        }
    }
    EXPECT_EQ(children, 1);
    EXPECT_EQ(Resolve(registry, ledger).born, 0);
    EXPECT_EQ(ledger.next_persistent_id(), ActiveRegionLedger::kPersistentIdLimit);
    entt::registry empty;
    EXPECT_EQ(Brain(empty).updated, 0);
    ai::RunMateSeekingOnTick(empty, {}, nullptr, true);
    EXPECT_EQ(Resolve(empty, ledger).born, 0);
}

TEST(CreatureIdentity, InvalidOrDuplicateIdentityRefusesEveryConsumerBeforeMutation) {
    for (const auto bad_id : {std::uint64_t{0},
                              std::uint64_t{1},
                              ActiveRegionLedger::kPersistentIdLimit,
                              std::numeric_limits<std::uint64_t>::max()}) {
        for (const bool missing : {false, true}) {
            entt::registry registry;
            const auto mother = Spawn(registry, 1, 0, true);
            const auto father = Spawn(registry, bad_id, 1);
            if (missing)
                registry.remove<Comp::SimulationIdentityComponent>(father);
            const auto before = registry.get<Comp::CreatureGenomeComponent>(mother);
            ActiveRegionLedger ledger;
            ASSERT_TRUE(ledger.reserve_persistent_ids(10));
            EXPECT_THROW(Brain(registry), std::logic_error);
            EXPECT_THROW(ai::RunMateSeekingOnTick(registry, {}, nullptr, true), std::logic_error);
            EXPECT_THROW(Resolve(registry, ledger), std::logic_error);
            ExpectSameGenome(registry.get<Comp::CreatureGenomeComponent>(mother), before);
            EXPECT_EQ(registry.get<Comp::CreatureComponent>(mother).wish_x, 0);
            EXPECT_EQ(ledger.next_persistent_id(), 10u);
        }
    }
}

TEST(CreatureIdentity, AllocatorCursorMustFollowExistingParentIdentities) {
    entt::registry registry;
    Spawn(registry, 10, 0, true);
    const auto father = Spawn(registry, 11, 1);
    const auto before = registry.get<Comp::CreatureGenomeComponent>(father);
    ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.reserve_persistent_ids(11));
    EXPECT_THROW(Resolve(registry, ledger), std::logic_error);
    ExpectSameGenome(registry.get<Comp::CreatureGenomeComponent>(father), before);
    EXPECT_EQ(ledger.next_persistent_id(), 11u);
}

TEST(CreatureIdentity, OrderingVisitsOnlyParticipatingCreaturesAndSortsEveryByte) {
    entt::registry registry;
    for (int index = 0; index < 1000; ++index)
        registry.emplace<Comp::TransformComponent>(registry.create());
    const std::array ids{(std::uint64_t{1} << 62) + 1,
                         std::uint64_t{1} << 24,
                         std::uint64_t{256},
                         std::uint64_t{1} << 48,
                         std::uint64_t{1} << 32,
                         std::uint64_t{1},
                         std::uint64_t{1} << 56,
                         std::uint64_t{255},
                         std::uint64_t{1} << 40,
                         std::uint64_t{1} << 16};
    std::vector<entt::entity> entities;
    for (const auto id : ids)
        entities.push_back(Spawn(registry, id, 0, false, true));
    EXPECT_EQ(ai::OrderCreatureParticipants(registry, entities, true), ids.size());
    constexpr std::array<std::uint64_t, 10> expected{1,
                                                     255,
                                                     256,
                                                     std::uint64_t{1} << 16,
                                                     std::uint64_t{1} << 24,
                                                     std::uint64_t{1} << 32,
                                                     std::uint64_t{1} << 40,
                                                     std::uint64_t{1} << 48,
                                                     std::uint64_t{1} << 56,
                                                     (std::uint64_t{1} << 62) + 1};
    for (std::size_t index = 0; index < expected.size(); ++index)
        EXPECT_EQ(registry.get<Comp::SimulationIdentityComponent>(entities[index]).id,
                  expected[index]);
    EXPECT_EQ(Brain(registry).identity_visits, ids.size());
}

TEST(CreatureIdentity, RegionAdmissionRestrictsOrderingAndKeepsBlockedParentsUnchanged) {
    entt::registry registry;
    const auto mother = Spawn(registry, 10, 0, true);
    Spawn(registry, 20, 1);
    const auto blocked = Spawn(registry, 0, 513, true);
    registry.remove<Comp::SimulationIdentityComponent>(blocked);
    const auto before = registry.get<Comp::CreatureGenomeComponent>(blocked);
    const std::array due{Luminumbra::world::RegionKey{0, 0}};
    const Luminumbra::world::RegionConsumerGate gate(registry, due);
    ai::CreatureTickAdmission admission;
    admission.Capture(registry, gate);
    const auto brain = ai::RunCreatureBrainSystemOnTick(
        registry, kDt, {}, nullptr, 0, 0, 0, true, nullptr, &admission, true);
    EXPECT_EQ(brain.updated, 2);
    EXPECT_EQ(brain.identity_visits, 2u);
    ai::RunMateSeekingOnTick(registry, {}, &admission, true);
    ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.reserve_persistent_ids(100));
    const auto stats = ai::RunMatingResolveOnTick(
        registry, kTick, kSeed, InstantCourtship(), nullptr, &admission, &ledger);
    EXPECT_EQ(stats.born, 1);
    EXPECT_EQ(stats.identity_visits, 2u);
    ExpectSameGenome(registry.get<Comp::CreatureGenomeComponent>(blocked), before);
    EXPECT_EQ(registry.get<Comp::CreatureComponent>(blocked).wish_x, 0);
    EXPECT_EQ(registry.get<Comp::SimulationIdentityComponent>(mother).id, 10u);
}

TEST(CreatureIdentity, ShippingDefaultsRemainUsableWithoutIdentity) {
    entt::registry registry;
    const auto mother = Spawn(registry, 1, 0, true);
    const auto father = Spawn(registry, 2, 1);
    registry.remove<Comp::SimulationIdentityComponent>(mother);
    registry.remove<Comp::SimulationIdentityComponent>(father);
    EXPECT_EQ(ai::RunCreatureBrainSystemOnTick(registry, kDt).identity_visits, 0u);
    ai::RunMateSeekingOnTick(registry);
    EXPECT_EQ(ai::RunMatingResolveOnTick(registry, kTick, kSeed, InstantCourtship()).born, 1);
    EXPECT_EQ(registry.view<Comp::SimulationIdentityComponent>().size(), 0u);
}
} // namespace
