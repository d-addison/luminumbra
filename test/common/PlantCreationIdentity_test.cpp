#include <gtest/gtest.h>

#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/systems/FoliageRegistryStage.h"
#include "luminumbra_common/systems/PlantCoarse.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
using luminumbra::core::DeterministicRng;
using Luminumbra::world::ActiveRegionLedger;
// gtest cannot print entt::null_t, so compare against a typed null handle.
constexpr entt::entity kNullEntity = entt::null;

entt::entity RenderResident(entt::registry& registry) {
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity);
    registry.emplace<C::PlantGenomeComponent>(entity);
    return entity;
}

F::SpeciesTemplate Species() {
    F::SpeciesTemplate species;
    species.id = "identity-crop";
    species.gene_hi.fill(1.0f);
    return species;
}

enum class CreationPath {
    Seed,
    Species,
    Farming,
    Promotion
};

entt::entity
CreatePlant(entt::registry& registry, ActiveRegionLedger& allocator, CreationPath path) {
    auto rng = DeterministicRng::seeded(4);
    switch (path) {
        case CreationPath::Seed:
            return F::PlantSeed(registry, {}, {}, 7, 12, allocator);
        case CreationPath::Species:
            return F::MakePlantFromSpecies(registry, {}, Species(), rng, 12, &allocator);
        case CreationPath::Farming: {
            F::FarmingController controller;
            return controller.Seed(registry, {}, Species(), rng, 12, allocator);
        }
        case CreationPath::Promotion: {
            const auto resident = RenderResident(registry);
            return F::PromotePlantToSimulation(registry, resident, 7, 12, allocator) ? resident
                                                                                     : kNullEntity;
        }
    }
    throw std::invalid_argument("Unknown plant creation path");
}

TEST(PlantCreationIdentity, SeedSpeciesAndFarmingShareTheAllocator) {
    ActiveRegionLedger ledger;
    entt::registry registry;
    const auto seed = F::PlantSeed(registry, {}, {}, 7, 11, ledger);
    auto rng = DeterministicRng::seeded(4);
    const auto species = F::MakePlantFromSpecies(registry, {}, Species(), rng, 12, &ledger);
    F::FarmingController controller;
    const auto farmed = controller.Seed(registry, {}, Species(), rng, 13, ledger);
    ASSERT_NE(seed, kNullEntity);
    ASSERT_NE(species, kNullEntity);
    ASSERT_NE(farmed, kNullEntity);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, seed), 1u);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, species), 2u);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, farmed), 3u);
    EXPECT_EQ(controller.seeds, 4);
    EXPECT_EQ(ledger.next_persistent_id(), 4u);
    EXPECT_TRUE((registry.all_of<C::GrazeableComponent,
                                 C::PollinationTag,
                                 C::SoilFeederComponent,
                                 C::CropLifecycleComponent>(farmed)));
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(farmed).planted_tick, 13u);
    // A different kind consumes the same stream between plant births.
    std::uint64_t ground_id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(ground_id));
    const auto child = F::PlantSeed(registry, {}, {}, 7, 14, ledger);
    EXPECT_EQ(ground_id, 4u);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, child), 5u);
}

TEST(PlantCreationIdentity, RenderResidentsConsumeNothingAndPromotionIsIdempotent) {
    ActiveRegionLedger ledger;
    entt::registry registry;
    for (int index = 0; index < 1024; ++index)
        (void)RenderResident(registry);
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    F::FoliageRegistryStage stage;
    stage.Begin(registry, ledger);
    EXPECT_EQ(stage.entity_visits(), 0u);
    stage.PreparePublication(registry);
    stage.Commit(registry);
    EXPECT_EQ(stage.publication_entity_visits(), 0u);
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    const auto resident = RenderResident(registry);
    ASSERT_TRUE(F::PromotePlantToSimulation(registry, resident, 9, 20, ledger));
    EXPECT_EQ(F::RequirePlantSimulationId(registry, resident), 1u);
    EXPECT_TRUE(registry.all_of<C::PlantTag>(resident));
    ASSERT_TRUE(F::PromotePlantToSimulation(registry, resident, 10, 21, ledger));
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(resident).species_id, 9u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(resident).last_tick, 20u);
}

TEST(PlantCreationIdentity, PromotionKeepsAnExistingRoleIdentityAndRefusesInvalidResidents) {
    ActiveRegionLedger ledger;
    entt::registry registry;
    const auto resident = RenderResident(registry);
    const auto identity = Luminumbra::simulation::AllocateSimulationIdentity(ledger);
    ASSERT_TRUE(identity.has_value());
    registry.emplace<C::SimulationIdentityComponent>(
        resident, identity.value_or(C::SimulationIdentityComponent{}));
    registry.emplace<C::PlantGrowthComponent>(
        resident,
        C::PlantGrowthComponent{
            .species_id = 8, .growth_points = 71, .planted_tick = 3, .last_tick = 19});
    registry.emplace<C::PlantRegionComponent>(
        resident, C::PlantRegionComponent{.persistent_id = 1, .region_stamp = 19});
    ASSERT_TRUE(F::PromotePlantToSimulation(registry, resident, 9, 20, ledger));
    EXPECT_EQ(F::RequirePlantSimulationId(registry, resident), 1u);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(resident).growth_points, 71u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(resident).species_id, 8u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(resident).last_tick, 19u);
    EXPECT_EQ(registry.get<C::PlantRegionComponent>(resident).region_stamp, 19u);
    EXPECT_FALSE(F::PromotePlantToSimulation(registry, entt::null, 9, 20, ledger));
    const auto incomplete = registry.create();
    EXPECT_FALSE(F::PromotePlantToSimulation(registry, incomplete, 9, 20, ledger));
    const auto invalid = RenderResident(registry);
    registry.emplace<C::SimulationIdentityComponent>(invalid);
    EXPECT_THROW(F::PromotePlantToSimulation(registry, invalid, 9, 20, ledger), std::runtime_error);
    EXPECT_FALSE(registry.all_of<C::PlantTag>(invalid));
    const auto unissued = RenderResident(registry);
    registry.emplace<C::SimulationIdentityComponent>(
        unissued, C::SimulationIdentityComponent{.id = ledger.next_persistent_id()});
    EXPECT_THROW(F::PromotePlantToSimulation(registry, unissued, 9, 20, ledger),
                 std::runtime_error);
    EXPECT_FALSE(registry.all_of<C::PlantTag>(unissued));
    const auto unidentified = F::PlantSeed(registry, {}, {}, 9, 20);
    EXPECT_THROW(F::PromotePlantToSimulation(registry, unidentified, 9, 20, ledger),
                 std::runtime_error);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
}

TEST(PlantCreationIdentity, ExhaustionPreservesPlantsSeedInventoryAndRandomStream) {
    ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit - 1));
    entt::registry registry;
    const auto last = F::PlantSeed(registry, {}, {}, 7, 11, ledger);
    ASSERT_NE(last, kNullEntity);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, last),
              ActiveRegionLedger::kPersistentIdLimit - 1);
    const auto resident = RenderResident(registry);
    auto rng = DeterministicRng::seeded(4);
    auto control = rng;
    F::FarmingController controller;
    EXPECT_EQ(F::PlantSeed(registry, {}, {}, 7, 12, ledger), kNullEntity);
    EXPECT_EQ(F::MakePlantFromSpecies(registry, {}, Species(), rng, 12, &ledger), kNullEntity);
    EXPECT_EQ(controller.Seed(registry, {}, Species(), rng, 12, ledger), kNullEntity);
    EXPECT_FALSE(F::PromotePlantToSimulation(registry, resident, 9, 12, ledger));
    EXPECT_EQ(rng.next_u64(), control.next_u64());
    EXPECT_EQ(controller.seeds, 5);
    EXPECT_EQ(registry.view<C::PlantTag>().size(), 1u);
    EXPECT_EQ(registry.view<C::PlantGrowthComponent>().size(), 1u);
    EXPECT_EQ(registry.view<C::SimulationIdentityComponent>().size(), 1u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(last).last_tick, 11u);
    EXPECT_FALSE(registry.all_of<C::PlantTag>(resident));
    EXPECT_FALSE(registry.all_of<C::PlantGrowthComponent>(resident));
    EXPECT_EQ(ledger.next_persistent_id(), ActiveRegionLedger::kPersistentIdLimit);
}

TEST(PlantCreationIdentity, EmptySeedInventoryRefusesBeforeAllocationOrSampling) {
    ActiveRegionLedger ledger;
    entt::registry registry;
    F::FarmingController controller;
    controller.seeds = 0;
    auto rng = DeterministicRng::seeded(4);
    auto control = rng;
    EXPECT_EQ(controller.Seed(registry, {}, Species(), rng, 1, ledger), kNullEntity);
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    EXPECT_EQ(rng.next_u64(), control.next_u64());
    EXPECT_TRUE(registry.view<C::PlantTag>().empty());
}

TEST(PlantCreationIdentity, StagingAndCoarseGrowthKeepIdentityAndBirthsPublishInIdentityOrder) {
    ActiveRegionLedger ledger;
    entt::registry live;
    const auto parent = F::PlantSeed(live, {}, {}, 7, 0, ledger);
    live.emplace<C::PlantRegionComponent>(parent, C::PlantRegionComponent{.persistent_id = 1});
    F::FoliageRegistryStage stage;
    stage.Begin(live, ledger);
    auto& candidate = stage.registry();
    EXPECT_EQ(F::RequirePlantSimulationId(candidate, parent), 1u);
    auto& growth = candidate.get<C::PlantGrowthComponent>(parent);
    F::AdvancePlantGrowthCoarse(
        growth, {}, {}, 0, 4, 4, &candidate.get<C::PlantRegionComponent>(parent));
    EXPECT_EQ(F::RequirePlantSimulationId(candidate, parent), 1u);
    // Force recycled handles to have the opposite order to issued identities.
    const auto recycled = candidate.create();
    candidate.destroy(recycled);
    const auto child = F::PlantSeed(candidate, {}, {}, 7, 4, stage.identity_allocator());
    const auto sibling = F::PlantSeed(candidate, {}, {}, 7, 4, stage.identity_allocator());
    ASSERT_NE(child, kNullEntity);
    ASSERT_NE(sibling, kNullEntity);
    ASSERT_GT(entt::to_integral(child), entt::to_integral(sibling));
    EXPECT_EQ(F::RequirePlantSimulationId(candidate, child), 2u);
    EXPECT_EQ(F::RequirePlantSimulationId(candidate, sibling), 3u);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
    stage.PreparePublication(live);
    ASSERT_TRUE(ledger.reserve_persistent_ids(stage.next_id()));
    stage.Commit(live);
    EXPECT_EQ(F::RequirePlantSimulationId(live, parent), 1u);
    EXPECT_EQ(live.get<C::PlantRegionComponent>(parent).region_stamp, 4u);
    const auto first_birth = static_cast<entt::entity>(1);
    const auto second_birth = static_cast<entt::entity>(2);
    EXPECT_EQ(F::RequirePlantSimulationId(live, first_birth), 2u);
    EXPECT_EQ(F::RequirePlantSimulationId(live, second_birth), 3u);
    EXPECT_EQ(ledger.next_persistent_id(), 4u);
}

TEST(PlantCreationIdentity, ExhaustedDetachedBirthRefusesBeforeCandidateOrLiveMutation) {
    ActiveRegionLedger ledger;
    entt::registry live;
    const auto parent = F::PlantSeed(live, {}, {}, 7, 11, ledger);
    ASSERT_TRUE(ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit));
    F::FoliageRegistryStage stage;
    stage.Begin(live, ledger);
    EXPECT_EQ(F::PlantSeed(stage.registry(), {}, {}, 7, 12, stage.identity_allocator()),
              kNullEntity);
    EXPECT_EQ(stage.registry().view<C::PlantTag>().size(), 1u);
    EXPECT_EQ(stage.registry().get<C::PlantGrowthComponent>(parent).last_tick, 11u);
    EXPECT_EQ(live.get<C::PlantGrowthComponent>(parent).last_tick, 11u);
    EXPECT_EQ(stage.next_id(), ledger.next_persistent_id());
    stage.Abort();
}

TEST(PlantCreationIdentity, IdentityAdmissionRefusesMissingInvalidDuplicateAndChangedIds) {
    for (const int failure : {0, 1, 2, 3, 4, 5, 6}) {
        ActiveRegionLedger ledger;
        entt::registry live;
        const auto parent = F::PlantSeed(live, {}, {}, 7, 11, ledger);
        F::FoliageRegistryStage stage;
        stage.Begin(live, ledger);
        auto& candidate = stage.registry();
        const auto child = F::PlantSeed(candidate, {}, {}, 7, 12, stage.identity_allocator());
        if (failure == 0)
            candidate.remove<C::SimulationIdentityComponent>(child);
        else if (failure == 1)
            candidate.get<C::SimulationIdentityComponent>(child).id = 0;
        else if (failure == 2)
            candidate.get<C::SimulationIdentityComponent>(child).id = 1;
        else if (failure == 3)
            candidate.get<C::SimulationIdentityComponent>(parent).id = 2;
        else if (failure == 4)
            candidate.emplace<C::PlantRegionComponent>(
                child, C::PlantRegionComponent{.persistent_id = 3, .region_stamp = 12});
        else if (failure == 5)
            candidate.get<C::SimulationIdentityComponent>(child).id =
                ActiveRegionLedger::kPersistentIdLimit;
        else
            candidate.get<C::SimulationIdentityComponent>(child).id = stage.next_id();
        EXPECT_THROW(stage.PreparePublication(live), std::runtime_error);
        stage.Commit(live);
        EXPECT_EQ(live.view<C::PlantTag>().size(), 1u);
        EXPECT_EQ(F::RequirePlantSimulationId(live, parent), 1u);
        EXPECT_EQ(live.get<C::PlantGrowthComponent>(parent).last_tick, 11u);
        EXPECT_EQ(ledger.next_persistent_id(), 2u);
        stage.Abort();
    }
}

TEST(PlantCreationIdentity, DestroyedParentIdentityCannotBeReusedByAnyCreationPath) {
    for (const auto path : {CreationPath::Seed,
                            CreationPath::Species,
                            CreationPath::Farming,
                            CreationPath::Promotion}) {
        SCOPED_TRACE(static_cast<int>(path));
        ActiveRegionLedger ledger;
        entt::registry live;
        const auto parent = F::PlantSeed(live, {}, {}, 7, 11, ledger);
        F::FoliageRegistryStage stage;
        stage.Begin(live, ledger);
        auto& candidate = stage.registry();
        candidate.destroy(parent);
        entt::entity child = entt::null;
        if (path == CreationPath::Promotion) {
            child = RenderResident(candidate);
            candidate.emplace<C::SimulationIdentityComponent>(
                child, C::SimulationIdentityComponent{.id = 1});
            ASSERT_TRUE(
                F::PromotePlantToSimulation(candidate, child, 7, 12, stage.identity_allocator()));
        } else {
            child = CreatePlant(candidate, stage.identity_allocator(), path);
        }
        ASSERT_NE(child, kNullEntity);
        ASSERT_NE(child, parent);
        candidate.get<C::SimulationIdentityComponent>(child).id = 1;
        ASSERT_EQ(candidate.view<C::PlantTag>().size(), 1u);
        EXPECT_THROW(stage.PreparePublication(live), std::runtime_error);
        stage.Commit(live);
        EXPECT_EQ(stage.publications(), 0u);
        ASSERT_TRUE(live.valid(parent));
        EXPECT_EQ(live.view<C::PlantTag>().size(), 1u);
        EXPECT_EQ(F::RequirePlantSimulationId(live, parent), 1u);
        EXPECT_EQ(live.get<C::PlantGrowthComponent>(parent).last_tick, 11u);
        EXPECT_EQ(ledger.next_persistent_id(), 2u);
        stage.Abort();
    }
}

TEST(PlantCreationIdentity, FreshBirthsFromEveryCreationPathPublishWithRetainedIdentities) {
    for (const auto path : {CreationPath::Seed,
                            CreationPath::Species,
                            CreationPath::Farming,
                            CreationPath::Promotion}) {
        SCOPED_TRACE(static_cast<int>(path));
        ActiveRegionLedger ledger;
        entt::registry live;
        const auto parent = F::PlantSeed(live, {}, {}, 7, 11, ledger);
        F::FoliageRegistryStage stage;
        stage.Begin(live, ledger);
        const auto child = CreatePlant(stage.registry(), stage.identity_allocator(), path);
        ASSERT_NE(child, kNullEntity);
        EXPECT_NO_THROW(stage.PreparePublication(live));
        ASSERT_TRUE(ledger.reserve_persistent_ids(stage.next_id()));
        stage.Commit(live);
        EXPECT_EQ(stage.publications(), 1u);
        EXPECT_EQ(live.view<C::PlantTag>().size(), 2u);
        EXPECT_EQ(F::RequirePlantSimulationId(live, parent), 1u);
        EXPECT_EQ(F::RequirePlantSimulationId(live, child), 2u);
        EXPECT_EQ(ledger.next_persistent_id(), 3u);
    }
}

TEST(PlantCreationIdentity, BirthAdmissionChecksBothAllocatorBoundsAndKeepsRetainedIdentity) {
    for (const auto id : {std::uint64_t{0},
                          std::uint64_t{4},
                          std::uint64_t{5},
                          std::uint64_t{6},
                          std::uint64_t{7},
                          std::uint64_t{8},
                          ActiveRegionLedger::kPersistentIdLimit}) {
        SCOPED_TRACE(id);
        ActiveRegionLedger ledger;
        entt::registry live;
        const auto parent = F::PlantSeed(live, {}, {}, 7, 11, ledger);
        ASSERT_TRUE(ledger.reserve_persistent_ids(5));
        F::FoliageRegistryStage stage;
        stage.Begin(live, ledger);
        auto& candidate = stage.registry();
        const auto child = F::PlantSeed(candidate, {}, {}, 7, 12, stage.identity_allocator());
        std::uint64_t unused = 0;
        ASSERT_TRUE(stage.AllocatePersistentId(unused));
        ASSERT_EQ(unused, 6u);
        ASSERT_EQ(stage.initial_id(), 5u);
        ASSERT_EQ(stage.next_id(), 7u);
        candidate.get<C::SimulationIdentityComponent>(child).id = id;
        candidate.get<C::PlantGrowthComponent>(parent).last_tick = 12;
        if (id == 5 || id == 6) {
            EXPECT_NO_THROW(stage.PreparePublication(live));
            ASSERT_TRUE(ledger.reserve_persistent_ids(stage.next_id()));
            stage.Commit(live);
            EXPECT_EQ(stage.publications(), 1u);
            EXPECT_EQ(live.view<C::PlantTag>().size(), 2u);
            EXPECT_EQ(F::RequirePlantSimulationId(live, parent), 1u);
            EXPECT_EQ(F::RequirePlantSimulationId(live, child), id);
            EXPECT_EQ(live.get<C::PlantGrowthComponent>(parent).last_tick, 12u);
            EXPECT_EQ(ledger.next_persistent_id(), 7u);
        } else {
            EXPECT_THROW(stage.PreparePublication(live), std::runtime_error);
            stage.Commit(live);
            EXPECT_EQ(stage.publications(), 0u);
            EXPECT_EQ(live.view<C::PlantTag>().size(), 1u);
            EXPECT_EQ(F::RequirePlantSimulationId(live, parent), 1u);
            EXPECT_EQ(live.get<C::PlantGrowthComponent>(parent).last_tick, 11u);
            EXPECT_EQ(ledger.next_persistent_id(), 5u);
            stage.Abort();
        }
    }
}

TEST(PlantCreationIdentity, EmptyCandidateAllocationRangeCannotAdmitBirths) {
    for (const auto id : {std::uint64_t{4}, std::uint64_t{5}, std::uint64_t{6}}) {
        SCOPED_TRACE(id);
        ActiveRegionLedger ledger;
        ASSERT_TRUE(ledger.reserve_persistent_ids(5));
        entt::registry live;
        F::FoliageRegistryStage stage;
        stage.Begin(live, ledger);
        const auto child = F::PlantSeed(stage.registry(), {}, {}, 7, 12);
        stage.registry().emplace<C::SimulationIdentityComponent>(
            child, C::SimulationIdentityComponent{.id = id});
        ASSERT_EQ(stage.initial_id(), stage.next_id());
        EXPECT_THROW(stage.PreparePublication(live), std::runtime_error);
        stage.Commit(live);
        EXPECT_EQ(stage.publications(), 0u);
        EXPECT_TRUE(live.view<C::PlantTag>().empty());
        EXPECT_TRUE(live.view<C::SimulationIdentityComponent>().empty());
        EXPECT_EQ(ledger.next_persistent_id(), 5u);
        stage.Abort();
    }
}

TEST(PlantCreationIdentity, RetainedIdentityMustPrecedeCandidateAllocationRange) {
    ActiveRegionLedger ledger;
    entt::registry live;
    const auto parent = F::PlantSeed(live, {}, {}, 7, 11, ledger);
    // A committed identity cannot be justified by an allocation in a later candidate.
    live.get<C::SimulationIdentityComponent>(parent).id = ledger.next_persistent_id();
    F::FoliageRegistryStage stage;
    stage.Begin(live, ledger);
    std::uint64_t unused = 0;
    ASSERT_TRUE(stage.AllocatePersistentId(unused));
    ASSERT_EQ(unused, 2u);
    stage.registry().get<C::PlantGrowthComponent>(parent).last_tick = 12;
    EXPECT_THROW(stage.PreparePublication(live), std::runtime_error);
    stage.Commit(live);
    EXPECT_EQ(stage.publications(), 0u);
    EXPECT_EQ(live.view<C::PlantTag>().size(), 1u);
    EXPECT_EQ(F::RequirePlantSimulationId(live, parent), 2u);
    EXPECT_EQ(live.get<C::PlantGrowthComponent>(parent).last_tick, 11u);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
    stage.Abort();
}

TEST(PlantCreationIdentity, RadixOrderChecksAllBytesAndRejectsIdentityTies) {
    entt::registry registry;
    std::vector<entt::entity> entities;
    std::vector<entt::entity> scratch;
    F::OrderPlantsBySimulationIdentity(registry, entities, scratch);
    constexpr std::array ids{std::uint64_t{1} << 62,
                             std::uint64_t{256},
                             std::uint64_t{1},
                             ActiveRegionLedger::kPersistentIdLimit - 1};
    for (const auto id : ids) {
        const auto entity = registry.create();
        registry.emplace<C::SimulationIdentityComponent>(entity, id);
        entities.push_back(entity);
    }
    F::OrderPlantsBySimulationIdentity(registry, entities, scratch);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, entities[0]), 1u);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, entities[1]), 256u);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, entities[2]), std::uint64_t{1} << 62);
    EXPECT_EQ(F::RequirePlantSimulationId(registry, entities[3]),
              ActiveRegionLedger::kPersistentIdLimit - 1);
    registry.get<C::SimulationIdentityComponent>(entities[1]).id = 1;
    EXPECT_THROW(F::OrderPlantsBySimulationIdentity(registry, entities, scratch),
                 std::runtime_error);
}

TEST(PlantCreationIdentity, LegacyFactoriesAndStagingRemainUnselected) {
    entt::registry registry;
    const auto plant = F::PlantSeed(registry, {}, {}, 7, 11);
    EXPECT_FALSE(registry.all_of<C::SimulationIdentityComponent>(plant));
    F::FoliageRegistryStage stage;
    stage.Begin(registry, std::uint64_t{1});
    EXPECT_THROW(stage.identity_allocator(), std::runtime_error);
    stage.PreparePublication(registry);
    stage.Commit(registry);
    EXPECT_FALSE(registry.all_of<C::SimulationIdentityComponent>(plant));
    EXPECT_EQ(stage.next_id(), 1u);
}

TEST(PlantCreationIdentity, GrowthCandidateRequiresIdentityInBothRegionalModes) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    for (const bool regional : {false, true}) {
        ActiveRegionLedger ledger;
        entt::registry registry;
        const auto recycled = registry.create();
        registry.destroy(recycled);
        const auto first = F::PlantSeed(registry, {2, 0, 0}, {}, 7, 0, ledger);
        const auto second = F::PlantSeed(registry, {1, 0, 0}, {}, 7, 0, ledger);
        ASSERT_GT(entt::to_integral(first), entt::to_integral(second));
        const std::array due{Luminumbra::world::RegionKey{0, 0}};
        std::vector<std::uint64_t> owners;
        const auto gate = regional ? Gate(registry, due, owners, Gate::PlantIdentity::Simulation)
                                   : Gate(Gate::PlantIdentity::Simulation);
        EXPECT_TRUE(gate.UsesSimulationPlantIdentity());
        std::vector<float> visited;
        const auto sampler = [&](const C::TransformComponent& transform) {
            visited.push_back(transform.position.x);
            return F::PlantEnvSample{};
        };
        EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, 1, sampler, gate).updated, 2);
        EXPECT_EQ(visited, (std::vector<float>{2, 1}));
        registry.remove<C::SimulationIdentityComponent>(second);
        visited.clear();
        EXPECT_THROW(F::RunPlantGrowthSystemOnTick(registry, 2, sampler, gate), std::runtime_error);
        EXPECT_TRUE(visited.empty());
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(first).last_tick, 1u);
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(second).last_tick, 1u);
    }
}

TEST(PlantCreationIdentity, PublicationVisitCountDependsOnlyOnParticipants) {
    ActiveRegionLedger ledger;
    entt::registry registry;
    (void)F::PlantSeed(registry, {}, {}, 7, 0, ledger);
    F::FoliageRegistryStage stage;
    stage.Begin(registry, ledger);
    stage.PreparePublication(registry);
    stage.Commit(registry);
    const auto visits = stage.publication_entity_visits();
    ASSERT_GT(visits, 0u);
    for (int index = 0; index < 4096; ++index)
        (void)RenderResident(registry);
    stage.Begin(registry, ledger);
    stage.PreparePublication(registry);
    stage.Commit(registry);
    EXPECT_EQ(stage.publication_entity_visits(), visits * 2);
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
}

TEST(PlantCreationIdentity, InvalidLookupAndAliasedOrderingScratchRefuse) {
    entt::registry registry;
    EXPECT_THROW(F::RequirePlantSimulationId(registry, entt::null), std::runtime_error);
    std::vector<entt::entity> entities;
    EXPECT_THROW(F::OrderPlantsBySimulationIdentity(registry, entities, entities),
                 std::invalid_argument);
    std::vector<entt::entity> oversized(
        static_cast<std::size_t>(entt::entt_traits<entt::entity>::entity_mask) + 1, kNullEntity);
    EXPECT_THROW(F::OrderPlantsBySimulationIdentity(registry, oversized, entities),
                 std::length_error);
    EXPECT_TRUE(entities.empty());
}

TEST(PlantCreationIdentity, PromotionRefusesInconsistentCoarseOwnershipBeforeAllocation) {
    for (const int failure : {0, 1, 2}) {
        ActiveRegionLedger ledger;
        entt::registry registry;
        const auto entity = RenderResident(registry);
        registry.emplace<C::PlantRegionComponent>(
            entity, C::PlantRegionComponent{.persistent_id = failure == 2 ? 1u : 2u});
        if (failure != 0) {
            const auto identity = Luminumbra::simulation::AllocateSimulationIdentity(ledger);
            ASSERT_TRUE(identity.has_value());
            registry.emplace<C::SimulationIdentityComponent>(
                entity, identity.value_or(C::SimulationIdentityComponent{}));
        }
        const auto cursor = ledger.next_persistent_id();
        EXPECT_THROW(F::PromotePlantToSimulation(registry, entity, 7, 12, ledger),
                     std::runtime_error);
        EXPECT_FALSE(registry.all_of<C::PlantTag>(entity));
        EXPECT_FALSE(registry.all_of<C::PlantGrowthComponent>(entity));
        EXPECT_EQ(ledger.next_persistent_id(), cursor);
    }
}

TEST(PlantCreationIdentity, StagedAllocatorChecksTheLastIdentityWithoutWrapping) {
    ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit - 1));
    entt::registry registry;
    F::FoliageRegistryStage stage;
    stage.Begin(registry, ledger);
    std::uint64_t id = 42;
    ASSERT_TRUE(stage.AllocatePersistentId(id));
    EXPECT_EQ(id, ActiveRegionLedger::kPersistentIdLimit - 1);
    id = 42;
    EXPECT_FALSE(stage.AllocatePersistentId(id));
    EXPECT_EQ(id, 42u);
    EXPECT_EQ(stage.next_id(), ActiveRegionLedger::kPersistentIdLimit);
    EXPECT_EQ(ledger.next_persistent_id(), ActiveRegionLedger::kPersistentIdLimit - 1);
}
} // namespace
