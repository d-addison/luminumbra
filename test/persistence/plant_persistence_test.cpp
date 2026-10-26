//  plant persistence projection — a planted field serializes to the engine's
// deterministic EntityRegistrySnapshot and reloads byte-exact (genome + growth/soil/disease/
// pollination/lifecycle sim truth). Empty roster -> empty snapshot (no-plant save byte-identical).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "luminumbra_common/ai/ScentSteeringSystem.h"
#include "luminumbra_common/ai/WildlifeFoliageSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CropLifecycleComponents.h"
#include "luminumbra_common/components/DiseaseComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/PollinationComponents.h"
#include "luminumbra_common/components/SoilComponents.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/IrrigationSystem.h"
#include "luminumbra_common/systems/PollinationSystem.h"
#include "luminumbra_common/systems/SoilNutrientSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include "support/UniqueTempPath.h"
#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>

#if defined(_WIN32)
#include "luminumbra_common/core/FilesystemPath.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace C = ::Luminumbra::Components;
namespace F = luminumbra::foliage;
namespace E = ::Luminumbra::Ecs;

// A plant with just the required components (tag + genome + growth + transform).
entt::entity barePlant(entt::registry& r, float x, std::uint8_t stage, std::uint32_t growth) {
    auto e = r.create();
    r.emplace<C::PlantTag>(e);
    r.emplace<C::TransformComponent>(e).position = Luminumbra::Vec3(x, 1.5f, -x);
    auto& gn = r.emplace<C::PlantGenomeComponent>(e);
    for (std::size_t i = 0; i < gn.genes.size(); ++i)
        gn.genes[i] = 0.1f + 0.1f * static_cast<float>((i + static_cast<std::size_t>(x)) % 8);
    auto& g = r.emplace<C::PlantGrowthComponent>(e);
    g.species_id = static_cast<std::uint16_t>(x);
    g.stage = stage;
    g.quality = 73;
    g.growth_points = growth;
    g.stress_points = growth / 4u;
    g.tended = 12;
    g.planted_tick = 100;
    g.last_tick = 100 + growth;
    return e;
}

// Exercise the pre-extension v1 contract explicitly, including mixed legacy IDs
// and joining a disjoint resident roster without an allocator descriptor.
E::EntityRegistrySnapshot LegacyPlantSnapshot(const entt::registry& registry) {
    auto snapshot = F::BuildPlantEntitySnapshot(registry);
    for (auto& record : snapshot.entities) {
        std::erase_if(record.components,
                      [](const auto& component) { return component.type == "PlantAllocator"; });
    }
    return snapshot;
}

TEST(PlantPersistence, SavedIdsPreservePollinationAndGerminationAfterDeletion) {
    namespace P = ::Luminumbra::Persistence;
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant-identity-");
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    entt::registry plants;
    const auto removed = plants.create();
    plants.destroy(removed);
    const auto first =
        barePlant(plants, 1.0f, static_cast<std::uint8_t>(C::PlantStage::Fruiting), 100);
    const auto gap = plants.create();
    const auto second =
        barePlant(plants, 2.0f, static_cast<std::uint8_t>(C::PlantStage::Fruiting), 200);
    plants.destroy(gap);
    ASSERT_NE(entt::to_version(first), 0u);
    for (const auto entity : {first, second}) {
        plants.emplace<C::PollinationTag>(entity);
        plants.emplace<C::PollinationComponent>(entity);
        auto& lifecycle = plants.emplace<C::CropLifecycleComponent>(entity);
        lifecycle.perennial = true;
        lifecycle.lifespan_ticks = 1;
    }
    const auto saved = F::BuildPlantEntitySnapshot(plants);
    const auto bytes = E::SerializeEntityRegistrySnapshotJson(saved);
    ASSERT_TRUE(P::WorldSaveService::save_plant_entities(saved, directory));
    constexpr std::uint64_t tick = 901;
    ASSERT_EQ(F::RunPollinationOnTick(plants, tick).crossed, 2);
    const auto first_cross = plants.get<C::PollinationComponent>(first).next_genome.genes;
    const auto second_cross = plants.get<C::PollinationComponent>(second).next_genome.genes;
    ASSERT_EQ(F::RunCropLifecycleOnTick(plants, tick).reseeded, 2);
    const auto births = [](const entt::registry& registry) {
        std::vector<std::string> result;
        for (const auto entity : registry.view<const C::PlantGrowthComponent,
                                               const C::PlantGenomeComponent,
                                               const C::TransformComponent>()) {
            if (registry.get<C::PlantGrowthComponent>(entity).planted_tick != tick)
                continue;
            const auto& position = registry.get<C::TransformComponent>(entity).position;
            result.push_back(
                nlohmann::json{{"x", position.x},
                               {"y", position.y},
                               {"z", position.z},
                               {"genes", registry.get<C::PlantGenomeComponent>(entity).genes}}
                    .dump());
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    const auto expected_births = births(plants);
    ASSERT_EQ(expected_births.size(), 2u);
    const auto view = plants.view<C::PlantTag>();
    const std::vector<entt::entity> removed_plants(view.begin(), view.end());
    for (const auto entity : removed_plants)
        plants.destroy(entity);
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(P::WorldSaveService::load_plant_entities(loaded, directory));
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(plants, loaded));
    ASSERT_TRUE(plants.valid(first));
    ASSERT_TRUE(plants.valid(second));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(plants)), bytes);
    ASSERT_EQ(F::RunPollinationOnTick(plants, tick).crossed, 2);
    EXPECT_EQ(plants.get<C::PollinationComponent>(first).next_genome.genes, first_cross);
    EXPECT_EQ(plants.get<C::PollinationComponent>(second).next_genome.genes, second_cross);
    ASSERT_EQ(F::RunCropLifecycleOnTick(plants, tick).reseeded, 2);
    EXPECT_EQ(births(plants), expected_births);
}

TEST(PlantPersistence, DuplicateSlotsAndOversizedIdsRefuseTheWholeRoster) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 100);
    barePlant(source, 2.0f, 3, 200);
    const auto original = F::BuildPlantEntitySnapshot(source);
    using Traits = entt::entt_traits<entt::entity>;
    for (const auto invalid_id :
         {original.entities.front().entity_id,
          static_cast<std::uint64_t>(entt::to_integral(Traits::construct(0, 1))),
          static_cast<std::uint64_t>(std::numeric_limits<entt::id_type>::max()) + 1u,
          std::numeric_limits<std::uint64_t>::max()}) {
        SCOPED_TRACE(invalid_id);
        auto snapshot = original;
        snapshot.entities.back().entity_id = invalid_id;
        entt::registry target;
        EXPECT_FALSE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_TRUE(F::BuildPlantEntitySnapshot(target).entities.empty());
        EXPECT_EQ(entt::to_integral(target.create()), 0u);
    }
}

TEST(PlantPersistence, LiveSlotClashesRefuseBeforeCreatingAnEarlierPlant) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 100);
    barePlant(source, 2.0f, 3, 200);
    const auto snapshot = F::BuildPlantEntitySnapshot(source);
    for (const auto version : std::array<entt::entt_traits<entt::entity>::version_type, 2>{0, 1}) {
        SCOPED_TRACE(version);
        entt::registry target;
        const auto resident = target.create(entt::entt_traits<entt::entity>::construct(1, version));
        target.emplace<C::TransformComponent>(resident).position = Luminumbra::Vec3(7.0f);
        EXPECT_TRUE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::CanApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        ASSERT_TRUE(target.valid(resident));
        EXPECT_EQ(target.get<C::TransformComponent>(resident).position, Luminumbra::Vec3(7.0f));
        EXPECT_TRUE(F::BuildPlantEntitySnapshot(target).entities.empty());
        EXPECT_EQ(entt::to_integral(target.create()), 0u);
    }
}

TEST(PlantPersistence, SavedIdsCanJoinADisjointResidentRoster) {
    entt::registry source;
    const auto unused = source.create();
    const auto plant = barePlant(source, 1.0f, 3, 100);
    source.destroy(unused);
    entt::registry target;
    const auto resident = target.create();
    ASSERT_TRUE(
        F::ApplyPlantEntitySnapshot(target, LegacyPlantSnapshot(source), /*replace_plants=*/false));
    EXPECT_TRUE(target.valid(resident));
    ASSERT_TRUE(target.valid(plant));
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(plant).growth_points, 100u);
}

TEST(PlantPersistence, LegacyMissingIdsAndGrazingFieldsKeepFreshIdAndOptOutBehaviour) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 100);
    barePlant(source, 2.0f, 3, 200);
    auto json =
        nlohmann::json::parse(E::SerializeEntityRegistrySnapshotJson(LegacyPlantSnapshot(source)));
    for (auto& record : json["entities"])
        record.erase("entity_id");
    E::EntityRegistrySnapshot loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(json.dump(), loaded, errors));
    EXPECT_EQ(nlohmann::json::parse(E::SerializeEntityRegistrySnapshotJson(loaded)), json);
    entt::registry target;
    const auto resident = target.create();
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, loaded));
    EXPECT_TRUE(target.valid(resident));
    EXPECT_EQ(target.view<C::PlantTag>().size(), 2u);
    EXPECT_TRUE(target.view<C::GrazeableComponent>().empty());
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(static_cast<entt::entity>(1)).growth_points,
              100u);
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(static_cast<entt::entity>(2)).growth_points,
              200u);
}

TEST(PlantPersistence, LegacyUnusableIdsAndMixedRostersDoNotConsumeSavedSlots) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 100);
    barePlant(source, 2.0f, 3, 200);
    const auto original = LegacyPlantSnapshot(source);
    for (const auto unusable :
         {entt::to_integral(static_cast<entt::entity>(entt::null)),
          entt::to_integral(entt::entt_traits<entt::entity>::construct(
              3, entt::to_version(static_cast<entt::entity>(entt::tombstone))))}) {
        auto snapshot = original;
        snapshot.entities.front().entity_id = unusable;
        snapshot.entities.back().entity_id = 0;
        entt::registry target;
        ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(target.get<C::PlantGrowthComponent>(static_cast<entt::entity>(0)).growth_points,
                  200u);
        EXPECT_EQ(target.get<C::PlantGrowthComponent>(static_cast<entt::entity>(1)).growth_points,
                  100u);
    }
    auto mixed = original;
    mixed.entities.front().has_entity_id = false;
    mixed.entities.back().entity_id = 0;
    entt::registry target;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, mixed));
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(static_cast<entt::entity>(0)).growth_points,
              200u);
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(static_cast<entt::entity>(1)).growth_points,
              100u);
    EXPECT_TRUE(F::ApplyPlantEntitySnapshot(target, {}));
    auto unknown = original;
    unknown.entities.front().name = "future_entity";
    unknown.entities.front().entity_id = std::numeric_limits<std::uint64_t>::max();
    unknown.entities.pop_back();
    EXPECT_TRUE(F::ApplyPlantEntitySnapshot(target, unknown));
}

TEST(PlantPersistence, SpeciesGrazingBiomassAndTickRoundTripAndKeepSimulating) {
    entt::registry source;
    auto rng = luminumbra::core::DeterministicRng::seeded(15, 17, 19);
    F::SpeciesTemplate species;
    species.id = "test_crop";
    const auto plant = F::MakePlantFromSpecies(source, Luminumbra::Vec3(1.0f), species, rng, 0);
    auto& grazing = source.get<C::GrazeableComponent>(plant);
    grazing.biomass = 0.375f;
    grazing.last_grazed_tick = 42;
    const auto bytes = E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(source));
    E::EntityRegistrySnapshot loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(bytes, loaded, errors));
    entt::registry target;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, loaded));
    ASSERT_TRUE(target.all_of<C::GrazeableComponent>(plant));
    EXPECT_EQ(target.get<C::GrazeableComponent>(plant).biomass, 0.375f);
    EXPECT_EQ(target.get<C::GrazeableComponent>(plant).last_grazed_tick, 42u);
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)), bytes);
    for (auto* registry : {&source, &target}) {
        const auto regrowth = luminumbra::ai::RunWildlifeFoliageOnTick(*registry, 43);
        EXPECT_EQ(regrowth.plants, 1);
        EXPECT_GT(regrowth.biomass_regrown, 0.0f);
        const auto creature = registry->create();
        registry->emplace<C::TransformComponent>(creature).position = Luminumbra::Vec3(1.0f);
        registry->emplace<C::CreatureComponent>(creature).is_predator = false;
        const auto grazed = luminumbra::ai::RunWildlifeFoliageOnTick(*registry, 44);
        EXPECT_EQ(grazed.grazed_plants, 1);
        EXPECT_GT(grazed.biomass_removed, 0.0f);
    }
    EXPECT_EQ(target.get<C::GrazeableComponent>(plant).biomass,
              source.get<C::GrazeableComponent>(plant).biomass);
    EXPECT_EQ(target.get<C::GrazeableComponent>(plant).last_grazed_tick, 44u);
}

TEST(PlantPersistence, InvalidGrazingDataRefusesBeforeAnyPlantIsRestored) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 100);
    const auto plant = barePlant(source, 2.0f, 3, 200);
    source.emplace<C::GrazeableComponent>(plant);
    const auto original = F::BuildPlantEntitySnapshot(source);
    const auto expect_refused = [](const E::EntityRegistrySnapshot& snapshot) {
        entt::registry target;
        EXPECT_FALSE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_TRUE(F::BuildPlantEntitySnapshot(target).entities.empty());
        EXPECT_EQ(entt::to_integral(target.create()), 0u);
    };
    for (const auto* field : {"biomass", "last_grazed_tick"}) {
        const std::vector<nlohmann::json> invalid =
            std::string(field) == "biomass"
                ? std::vector<nlohmann::json>{-0.1,
                                              1.1,
                                              -1e-50,
                                              1.0 + 1e-12,
                                              1e38,
                                              std::numeric_limits<double>::infinity(),
                                              std::numeric_limits<double>::quiet_NaN(),
                                              "0",
                                              nullptr,
                                              true,
                                              nlohmann::json::array()}
                : std::vector<nlohmann::json>{-1,
                                              1e38,
                                              1.0,
                                              std::numeric_limits<std::uint64_t>::max(),
                                              "0",
                                              nullptr,
                                              true,
                                              nlohmann::json::array()};
        for (const auto& value : invalid) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(value.dump());
            auto snapshot = original;
            for (auto& component : snapshot.entities.back().components) {
                if (component.type == "Grazeable")
                    component.data[field] = value;
            }
            expect_refused(snapshot);
        }
        auto partial = original;
        for (auto& component : partial.entities.back().components) {
            if (component.type == "Grazeable")
                component.data.erase(field);
        }
        expect_refused(partial);
    }
    for (const auto& invalid : {nlohmann::json(nullptr), nlohmann::json::array()}) {
        auto snapshot = original;
        for (auto& component : snapshot.entities.back().components) {
            if (component.type == "Grazeable")
                component.data = invalid;
        }
        expect_refused(snapshot);
    }
    auto duplicate = original;
    duplicate.entities.back().components.push_back(
        {"Grazeable", {{"biomass", 1.0f}, {"last_grazed_tick", 0}}});
    expect_refused(duplicate);
    for (const auto biomass : {0.0, 1.0}) {
        auto boundary = original;
        for (auto& component : boundary.entities.back().components) {
            if (component.type == "Grazeable") {
                component.data["biomass"] = biomass;
                component.data["last_grazed_tick"] = std::numeric_limits<std::uint32_t>::max();
            }
        }
        entt::registry target;
        ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, boundary));
        EXPECT_EQ(target.get<C::GrazeableComponent>(plant).biomass, biomass);
        EXPECT_EQ(target.get<C::GrazeableComponent>(plant).last_grazed_tick,
                  std::numeric_limits<std::uint32_t>::max());
    }
}

TEST(PlantPersistence, SnapshotDecoderRefusesInvalidIntegerRepresentationsWithoutAdoption) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 100);
    const auto original = F::BuildPlantEntitySnapshot(source);
    const auto bytes = E::SerializeEntityRegistrySnapshotJson(original);
    const auto json = nlohmann::json::parse(bytes);
    const auto expect_refused = [&](const nlohmann::json& invalid) {
        auto retained = original;
        std::vector<std::string> errors;
        EXPECT_FALSE(E::LoadEntityRegistrySnapshotJson(invalid.dump(), retained, errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(retained), bytes);
    };
    for (const auto* field : {"entity_id", "entity_count", "component_count"}) {
        for (const auto& value : std::vector<nlohmann::json>{1e38,
                                                             -1e38,
                                                             -1,
                                                             0.5,
                                                             1.0,
                                                             true,
                                                             "1",
                                                             nullptr,
                                                             nlohmann::json::array(),
                                                             nlohmann::json::object()}) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(value.dump());
            auto invalid = json;
            (std::string(field) == "entity_id" ? invalid["entities"][0] : invalid)[field] = value;
            expect_refused(invalid);
        }
    }
    for (const auto* field : {"entity_count", "component_count"}) {
        auto invalid = json;
        invalid[field] = static_cast<double>(json.at(field).get<std::size_t>());
        expect_refused(invalid);
        invalid = json;
        invalid.erase(field);
        expect_refused(invalid);
        invalid = json;
        invalid[field] = std::numeric_limits<std::uint64_t>::max();
        expect_refused(invalid);
        invalid[field] = 0;
        expect_refused(invalid);
    }
    for (const auto& invalid_array :
         {nlohmann::json(nullptr), nlohmann::json::object(), nlohmann::json(1)}) {
        auto invalid = json;
        invalid["entities"] = invalid_array;
        expect_refused(invalid);
        invalid = json;
        invalid["entities"][0]["components"] = invalid_array;
        expect_refused(invalid);
    }
    // The generic schema supports 64-bit IDs even though EnTT plants use narrower IDs.
    for (const auto& valid_id : {nlohmann::json(std::uint64_t{0}),
                                 nlohmann::json(std::int64_t{1}),
                                 nlohmann::json(std::numeric_limits<std::uint64_t>::max())}) {
        auto valid = json;
        valid["entities"][0]["entity_id"] = valid_id;
        E::EntityRegistrySnapshot loaded;
        std::vector<std::string> errors;
        ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(valid.dump(), loaded, errors));
        EXPECT_EQ(nlohmann::json::parse(E::SerializeEntityRegistrySnapshotJson(loaded)), valid);
    }
}

#if !defined(_WIN32)
TEST(PlantPersistence, EmptyRosterRemovalFlushesParentEvenAfterFailedFlush) {
    namespace SS = ::Luminumbra::Persistence;
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant-unlink-");
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() {
            SS::WorldSaveService::set_directory_flush_failure_for_testing(-1);
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto path = SS::WorldSaveService::plant_entities_path(directory);
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    ASSERT_TRUE(
        SS::WorldSaveService::save_plant_entities(F::BuildPlantEntitySnapshot(plants), directory));
    const Luminumbra::Ecs::EntityRegistrySnapshot empty;
    for (int attempt = 0; attempt < 2; ++attempt) {
        SS::WorldSaveService::set_directory_flush_failure_for_testing(0);
        std::vector<std::string> errors;
        EXPECT_FALSE(SS::WorldSaveService::save_plant_entities(empty, directory, &errors));
        EXPECT_FALSE(std::filesystem::exists(path));
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors.front(),
                  "directory flush failed (test seam): " + path.parent_path().string());
    }
    EXPECT_TRUE(SS::WorldSaveService::save_plant_entities(empty, directory));
}
#endif

TEST(PlantPersistence, InvalidLastPlantLeavesRegistryAndEntitySequenceUnchanged) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    barePlant(source, 2.0f, 3, 600);
    const auto original = F::BuildPlantEntitySnapshot(source);
    const std::vector<nlohmann::json> invalid_numbers = {1e38, -1, 256, 1.5, "3", nullptr, true};
    for (const auto& invalid : invalid_numbers) {
        SCOPED_TRACE(invalid.dump());
        auto snapshot = original;
        for (auto& component : snapshot.entities.back().components) {
            if (component.type == "PlantGrowth")
                component.data["stage"] = invalid;
        }
        entt::registry target;
        const auto existing = barePlant(target, 4.0f, 3, 700);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
        EXPECT_EQ(entt::to_integral(target.create()), entt::to_integral(existing) + 1u);
    }
}

TEST(PlantPersistence, EveryNumericAndBooleanProjectionIsValidatedBeforeMutation) {
    entt::registry source;
    const auto plant = barePlant(source, 1.0f, 3, 500);
    source.emplace<C::SoilFeederComponent>(plant);
    source.emplace<C::PlantHealthComponent>(plant);
    source.emplace<C::PollinationComponent>(plant);
    source.emplace<C::CropLifecycleComponent>(plant);
    const auto original = F::BuildPlantEntitySnapshot(source);
    for (std::size_t component_index = 0;
         component_index < original.entities.front().components.size();
         ++component_index) {
        const auto& data = original.entities.front().components[component_index].data;
        for (auto field = data.begin(); field != data.end(); ++field) {
            for (const bool missing : {false, true}) {
                SCOPED_TRACE(original.entities.front().components[component_index].type);
                SCOPED_TRACE(field.key());
                SCOPED_TRACE(missing);
                auto snapshot = original;
                auto& changed = snapshot.entities.front().components[component_index].data;
                if (missing)
                    changed.erase(field.key());
                else if (field.value().is_array())
                    changed[field.key()] = nlohmann::json::array({"invalid"});
                else
                    changed[field.key()] = "invalid";
                entt::registry target;
                EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
                EXPECT_TRUE(target.view<C::PlantTag>().empty());
                EXPECT_EQ(entt::to_integral(target.create()), 0u);
            }
        }
    }
    for (const auto& type : {"Transform",
                             "PlantGenome",
                             "PlantGrowth",
                             "SoilFeeder",
                             "PlantHealth",
                             "Pollination",
                             "CropLifecycle"}) {
        auto snapshot = original;
        const auto& components = original.entities.front().components;
        const auto found = std::find_if(
            components.begin(), components.end(), [&](const auto& c) { return c.type == type; });
        ASSERT_NE(found, components.end());
        snapshot.entities.front().components.push_back(*found);
        entt::registry target;
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(entt::to_integral(target.create()), 0u);
    }
}

TEST(PlantPersistence, NumericRangeChecksPrecedeEveryProjection) {
    entt::registry source;
    const auto plant = barePlant(source, 1.0f, 3, 500);
    source.emplace<C::SoilFeederComponent>(plant);
    source.emplace<C::PlantHealthComponent>(plant);
    source.emplace<C::PollinationComponent>(plant);
    source.emplace<C::CropLifecycleComponent>(plant);
    const auto original = F::BuildPlantEntitySnapshot(source);
    for (std::size_t i = 0; i < original.entities.front().components.size(); ++i) {
        const auto& component = original.entities.front().components[i];
        for (auto field = component.data.begin(); field != component.data.end(); ++field) {
            if (field->is_boolean())
                continue;
            const std::vector<nlohmann::json> invalid =
                field->is_array()
                    ? std::vector<nlohmann::json>{nlohmann::json::array({1e38 * 10.0}),
                                                  nlohmann::json::array({nullptr})}
                : field->is_number_float()
                    ? std::vector<nlohmann::json>{1e38 * 10.0,
                                                  std::numeric_limits<double>::infinity()}
                    : std::vector<nlohmann::json>{
                          -1, 1e38, std::numeric_limits<std::uint64_t>::max()};
            for (const auto& value : invalid) {
                // uint64 max is a valid boundary for the tick fields, checked separately.
                if (value.is_number_unsigned() &&
                    (field.key() == "planted_tick" || field.key() == "last_tick" ||
                     field.key() == "last_pollen_tick"))
                    continue;
                SCOPED_TRACE(component.type + "." + field.key());
                auto snapshot = original;
                snapshot.entities.front().components[i].data[field.key()] = value;
                entt::registry target;
                EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
                EXPECT_EQ(entt::to_integral(target.create()), 0u);
            }
        }
    }
}

TEST(PlantPersistence, DiskRefusalsLeaveOutputAndExistingFilesUnchanged) {
    namespace P = ::Luminumbra::Persistence;
    const auto dir = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant-admission-");
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{dir};
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    const auto original = F::BuildPlantEntitySnapshot(source);
    ASSERT_TRUE(P::WorldSaveService::save_plant_entities(original, dir));
    auto invalid = original;
    for (auto& component : invalid.entities.front().components) {
        if (component.type == "PlantGrowth")
            component.data["stage"] = 1e38;
    }
    std::vector<std::string> errors;
    EXPECT_FALSE(P::WorldSaveService::save_plant_entities(invalid, dir, &errors));
    EXPECT_FALSE(errors.empty());
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(P::WorldSaveService::load_plant_entities(loaded, dir));
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    const auto path = P::WorldSaveService::plant_entities_path(dir);
    const auto invalid_bytes = E::SerializeEntityRegistrySnapshotJson(invalid);
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(path, invalid_bytes));
    EXPECT_FALSE(P::WorldSaveService::load_plant_entities(loaded, dir, &errors));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    std::ifstream input(path, std::ios::binary);
    const std::string disk_bytes{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
    EXPECT_EQ(disk_bytes, invalid_bytes);
}

TEST(PlantPersistence, ComponentDomainsAreCheckedBeforeAnyPlantIsAdopted) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    const auto last = barePlant(source, 2.0f, 3, 600);
    source.emplace<C::PlantHealthComponent>(last);
    source.emplace<C::PollinationComponent>(last);
    const auto original = F::BuildPlantEntitySnapshot(source);
    struct InvalidField {
        std::string type;
        std::string field;
        nlohmann::json value;
    };
    const std::vector<InvalidField> invalid = {
        {"PlantGrowth", "stage", C::kPlantStageCount},
        {"PlantGrowth", "stage", 255},
        {"PlantGrowth", "quality", 101},
        {"PlantHealth", "state", C::kPlantDiseaseStateCount},
        {"PlantHealth", "infection", C::kDiseaseUnitScale + 1u},
        {"PlantHealth", "resistance", C::kDiseaseUnitScale + 1u}};
    const auto expect_refused = [](const E::EntityRegistrySnapshot& snapshot) {
        entt::registry target;
        const auto existing = barePlant(target, 4.0f, 3, 700);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
        EXPECT_EQ(entt::to_integral(target.create()), entt::to_integral(existing) + 1u);
    };
    for (const auto& field : invalid) {
        SCOPED_TRACE(field.type + "." + field.field);
        auto snapshot = original;
        for (auto& component : snapshot.entities.back().components) {
            if (component.type == field.type)
                component.data[field.field] = field.value;
        }
        expect_refused(snapshot);
    }
    // Check every adopted gene in both the current and pending pollination genome.
    // The tiny excursions would round back into [0,1] if checked after float conversion.
    for (const auto& type : {"PlantGenome", "Pollination"}) {
        for (std::size_t gene = 0; gene < C::kPlantGeneCount; ++gene) {
            for (const double value : {-0.01, 1.01, 1e20, -1e-50, 1.0 + 1e-12}) {
                SCOPED_TRACE(type);
                SCOPED_TRACE(gene);
                SCOPED_TRACE(value);
                auto snapshot = original;
                for (auto& component : snapshot.entities.back().components) {
                    if (component.type == type)
                        component
                            .data[component.type == "PlantGenome" ? "genes" : "next_genome"][gene] =
                            value;
                }
                expect_refused(snapshot);
            }
        }
    }
}

TEST(PlantPersistence, PositionsMustFitThePackedChunkDomainBeforeAndAfterFloatRounding) {
    entt::registry source;
    const auto unused = source.create();
    const auto plant = barePlant(source, 1.0f, 3, 500);
    source.destroy(unused);
    source.emplace<C::SoilFeederComponent>(plant);
    const auto original = LegacyPlantSnapshot(source);
    for (const auto* axis : {"x", "y", "z"}) {
        const float limit = axis == std::string("y") ? 33554432.0f : 16777216.0f;
        for (const double coordinate :
             {-static_cast<double>(limit),
              static_cast<double>(std::nextafter(limit, 0.0f)),
              static_cast<double>(limit),
              static_cast<double>(limit) - 0.01,
              static_cast<double>(std::nextafter(-limit, -std::numeric_limits<float>::infinity())),
              static_cast<double>(std::numeric_limits<float>::max()),
              -static_cast<double>(std::numeric_limits<float>::max())}) {
            SCOPED_TRACE(axis);
            SCOPED_TRACE(coordinate);
            auto snapshot = original;
            for (auto& component : snapshot.entities.front().components) {
                if (component.type == "Transform")
                    component.data[axis] = coordinate;
            }
            const bool supported = coordinate >= -static_cast<double>(limit) &&
                                   coordinate < static_cast<double>(limit) - 0.01;
            EXPECT_EQ(F::ValidatePlantEntitySnapshot(snapshot), supported);
            entt::registry target;
            const auto existing = barePlant(target, 1.0f, 3, 500);
            target.emplace<C::SoilFeederComponent>(existing);
            const auto expected =
                E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
            EXPECT_EQ(F::ApplyPlantEntitySnapshot(target, snapshot, /*replace_plants=*/false),
                      supported);
            if (!supported) {
                EXPECT_EQ(
                    E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                    expected);
                EXPECT_EQ(entt::to_integral(target.create()), entt::to_integral(existing) + 1u);
            }
        }
    }
}

TEST(PlantPersistence, GridCoordinateConversionsRefuseUnrepresentableCells) {
    const auto maximum = std::numeric_limits<float>::max();
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    struct Input {
        float position;
        float origin;
        float cell_size;
    };
    for (const auto& input : std::array<Input, 14>{
             {{maximum, 0.0f, 1.0f},
              {-maximum, 0.0f, 1.0f},
              {infinity, 0.0f, 1.0f},
              {nan, 0.0f, 1.0f},
              {0.0f, infinity, 1.0f},
              {0.0f, nan, 1.0f},
              {1.0f, 0.0f, 0.0f},
              {1.0f, 0.0f, -1.0f},
              {1.0f, 0.0f, infinity},
              {1.0f, 0.0f, nan},
              {maximum, -maximum, 1.0f},
              {static_cast<float>(std::numeric_limits<int>::max()), 0.0f, 1.0f},
              {std::nextafter(static_cast<float>(std::numeric_limits<int>::min()), -infinity),
               0.0f,
               1.0f},
              {1.0f, 0.0f, std::numeric_limits<float>::min()}}}) {
        int cx = 0;
        int cz = 0;
        F::WorldToCell(input.position, 0.0f, input.origin, 0.0f, input.cell_size, cx, cz);
        EXPECT_EQ(cx, -1);
        EXPECT_EQ(cz, -1);
        F::IrrigationWorldToCell(0.0f, input.position, 0.0f, input.origin, input.cell_size, cx, cz);
        EXPECT_EQ(cx, -1);
        EXPECT_EQ(cz, -1);
        EXPECT_EQ(luminumbra::ai::WorldToCell(input.position, input.origin, input.cell_size), -1);
    }
    int cx = 0;
    int cz = 0;
    F::WorldToCell(-0.5f, 5.9f, 0.0f, 0.0f, 1.0f, cx, cz);
    EXPECT_EQ(cx, -1);
    EXPECT_EQ(cz, 5);
    F::IrrigationWorldToCell(-0.5f, 5.9f, 0.0f, 0.0f, 1.0f, cx, cz);
    EXPECT_EQ(cx, -1);
    EXPECT_EQ(cz, 5);
    EXPECT_EQ(luminumbra::ai::WorldToCell(-0.5f, 0.0f, 1.0f), -1);
    EXPECT_EQ(luminumbra::ai::WorldToCell(5.9f, 0.0f, 1.0f), 5);
    const auto minimum = static_cast<float>(std::numeric_limits<int>::min());
    F::WorldToCell(minimum, minimum, 0.0f, 0.0f, 1.0f, cx, cz);
    EXPECT_EQ(cx, std::numeric_limits<int>::min());
    EXPECT_EQ(cz, std::numeric_limits<int>::min());
    F::IrrigationWorldToCell(minimum, minimum, 0.0f, 0.0f, 1.0f, cx, cz);
    EXPECT_EQ(cx, std::numeric_limits<int>::min());
    EXPECT_EQ(cz, std::numeric_limits<int>::min());
    EXPECT_EQ(luminumbra::ai::WorldToCell(minimum, 0.0f, 1.0f), std::numeric_limits<int>::min());
    const Luminumbra::world::GameSession session;
    for (const float value : {maximum, -maximum, infinity, nan}) {
        EXPECT_EQ(session.ScentWorldToCellX(value), -1);
        EXPECT_EQ(session.ScentWorldToCellZ(value), -1);
    }
}

TEST(PlantPersistence, EveryPlantRequiresAllCoreComponentsBeforeRegistryMutation) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    barePlant(source, 2.0f, 3, 600);
    const auto original = F::BuildPlantEntitySnapshot(source);
    for (unsigned mask = 0; mask < 7; ++mask) {
        SCOPED_TRACE(mask);
        auto snapshot = original;
        auto& components = snapshot.entities.back().components;
        std::erase_if(components, [mask](const auto& component) {
            return (component.type == "Transform" && (mask & 1u) == 0) ||
                   (component.type == "PlantGenome" && (mask & 2u) == 0) ||
                   (component.type == "PlantGrowth" && (mask & 4u) == 0);
        });
        entt::registry target;
        const auto existing = barePlant(target, 4.0f, 3, 700);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
        EXPECT_EQ(entt::to_integral(target.create()), entt::to_integral(existing) + 1u);
    }
}

TEST(PlantPersistence, RepresentableMaximaAndHistoricalPartialGenomesStillLoad) {
    entt::registry source;
    const auto plant = barePlant(source, 1.0f, 3, 500);
    source.emplace<C::SoilFeederComponent>(plant);
    source.emplace<C::PlantHealthComponent>(plant);
    source.emplace<C::PollinationComponent>(plant);
    source.emplace<C::CropLifecycleComponent>(plant);
    auto snapshot = F::BuildPlantEntitySnapshot(source);
    for (auto& component : snapshot.entities.front().components) {
        if (component.type == "Transform")
            component.data["x"] = std::nextafter(16777216.0f, 0.0f);
        if (component.type == "PlantGenome")
            component.data["genes"] = nlohmann::json::array({0.25f});
        if (component.type == "PlantGrowth") {
            component.data["stage"] = C::kPlantStageCount - 1u;
            component.data["quality"] = 100;
            component.data["species_id"] = std::numeric_limits<std::uint16_t>::max();
            component.data["growth_points"] = std::numeric_limits<std::uint32_t>::max();
            component.data["last_tick"] = std::numeric_limits<std::uint64_t>::max();
        }
        if (component.type == "PlantHealth") {
            component.data["infection"] = C::kDiseaseUnitScale;
            component.data["resistance"] = C::kDiseaseUnitScale;
            component.data["state"] = C::kPlantDiseaseStateCount - 1u;
        }
        if (component.type == "Pollination")
            component.data["next_genome"] = nlohmann::json::array({0.0, 1.0});
    }
    snapshot.entities.front().components.push_back({"FuturePlantExtension", {{"opaque", true}}});
    snapshot.entities.front().components.push_back({"FuturePlantExtension", nullptr});
    entt::registry target;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, snapshot));
    const auto restored = *target.view<C::PlantTag>().begin();
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(restored).last_tick,
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(target.get<C::TransformComponent>(restored).position.x,
              std::nextafter(16777216.0f, 0.0f));
    EXPECT_EQ(target.get<C::PlantGenomeComponent>(restored).genes[0], 0.25f);
    EXPECT_EQ(target.get<C::PlantGenomeComponent>(restored).genes[1], 0.0f);
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(restored).stage, C::kPlantStageCount - 1u);
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(restored).quality, 100);
    EXPECT_EQ(target.get<C::PlantHealthComponent>(restored).infection, C::kDiseaseUnitScale);
    EXPECT_EQ(target.get<C::PlantHealthComponent>(restored).resistance, C::kDiseaseUnitScale);
    EXPECT_EQ(target.get<C::PlantHealthComponent>(restored).state, C::kPlantDiseaseStateCount - 1u);
    EXPECT_EQ(target.get<C::PollinationComponent>(restored).next_genome.genes[0], 0.0f);
    EXPECT_EQ(target.get<C::PollinationComponent>(restored).next_genome.genes[1], 1.0f);
}

TEST(PlantPersistence, EmptyRosterIsEmptySnapshot) {
    entt::registry r;
    EXPECT_TRUE(F::BuildPlantEntitySnapshot(r).entities.empty());
}

TEST(PlantPersistence, RoundtripPreservesAllPlantState) {
    entt::registry a;
    // plant 0: minimal. plant 1: + soil + health. plant 2: + pollination + lifecycle.
    barePlant(a, 0.0f, static_cast<std::uint8_t>(C::PlantStage::Seed), 500);

    const auto p1 = barePlant(a, 1.0f, static_cast<std::uint8_t>(C::PlantStage::Mature), 12000);
    a.emplace<C::SoilFeederComponent>(p1, C::SoilFeederComponent{800, 4200});
    auto& h1 = a.emplace<C::PlantHealthComponent>(p1);
    h1.infection = 640;
    h1.resistance = 120;
    h1.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
    h1.infected_ticks = 33;

    const auto p2 = barePlant(a, 2.0f, static_cast<std::uint8_t>(C::PlantStage::Fruiting), 18500);
    auto& pc2 = a.emplace<C::PollinationComponent>(p2);
    pc2.pollinated = true;
    pc2.last_pollen_tick = 900;
    pc2.crosses = 4;
    for (std::size_t i = 0; i < pc2.next_genome.genes.size(); ++i)
        pc2.next_genome.genes[i] = 0.05f * static_cast<float>(i + 1);
    auto& lc2 = a.emplace<C::CropLifecycleComponent>(p2);
    lc2.perennial = true;
    lc2.ripe_ticks = 77;
    lc2.lifespan_ticks = 1500;
    lc2.generations = 3;
    lc2.species_id = 2;

    // Build -> serialize -> load -> apply into a FRESH registry -> re-serialize must be byte-exact.
    const std::string json = E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(a));
    E::EntityRegistrySnapshot loaded;
    std::vector<std::string> errs;
    ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(json, loaded, errs))
        << (errs.empty() ? "" : errs.front());

    entt::registry b;
    F::ApplyPlantEntitySnapshot(b, loaded);
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(b)), json)
        << "planted field must reload byte-exact";

    // Spot-check that reconstructed component values match (not just the serialized bytes).
    EXPECT_EQ(b.view<C::PlantTag>().size(), 3u);
    bool sawInfected = false, sawPerennial = false;
    for (auto e : b.view<C::PlantTag>()) {
        if (auto* h = b.try_get<C::PlantHealthComponent>(e); h && h->infection == 640) {
            sawInfected = true;
            EXPECT_EQ(h->infected_ticks, 33u);
        }
        if (auto* lc = b.try_get<C::CropLifecycleComponent>(e); lc && lc->perennial) {
            sawPerennial = true;
            EXPECT_EQ(lc->generations, 3u);
        }
    }
    EXPECT_TRUE(sawInfected) << "blighted plant's health survived the roundtrip";
    EXPECT_TRUE(sawPerennial) << "perennial lifecycle survived the roundtrip";
}

TEST(PlantPersistence, GenesSurviveFullPrecision) {
    entt::registry a;
    auto e = barePlant(a, 0.0f, 0, 0);
    auto& gn = a.get<C::PlantGenomeComponent>(e);
    gn.genes = {0.123456791f,
                0.987654328f,
                0.333333343f,
                0.0009765625f,
                0.5f,
                0.7071067691f,
                0.0f,
                0.999999940f};
    const auto saved = gn.genes;

    const std::string json = E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(a));
    E::EntityRegistrySnapshot loaded;
    std::vector<std::string> errs;
    ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(json, loaded, errs));
    entt::registry b;
    F::ApplyPlantEntitySnapshot(b, loaded);
    ASSERT_EQ(b.view<C::PlantTag>().size(), 1u);
    for (auto pe : b.view<C::PlantTag>())
        EXPECT_EQ(b.get<C::PlantGenomeComponent>(pe).genes, saved) << "genes survive bit-exact";
}

//  disk path: WorldSaveService writes/reads plant-entities.json byte-exact.
TEST(PlantPersistence, DiskSaveLoadByteExact) {
    namespace fs = std::filesystem;
    namespace SS = ::Luminumbra::Persistence;
    entt::registry a;
    barePlant(a, 0.0f, static_cast<std::uint8_t>(C::PlantStage::Seed), 500);
    const auto p1 = barePlant(a, 1.0f, static_cast<std::uint8_t>(C::PlantStage::Mature), 12000);
    a.emplace<C::SoilFeederComponent>(p1, C::SoilFeederComponent{800, 4200});
    const auto snap = F::BuildPlantEntitySnapshot(a);
    const std::string json = E::SerializeEntityRegistrySnapshotJson(snap);

    const fs::path dir = fs::temp_directory_path() / "lumin_plant_persist_disk";
    fs::remove_all(dir);
    std::vector<std::string> errs;
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(snap, dir, &errs))
        << (errs.empty() ? "" : errs.front());
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, dir, &errs));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), json) << "disk save->load byte-exact";
    fs::remove_all(dir);
}

// A no-plant save writes NO file and loads as a clean (empty) miss -> byte-identical no-plant save.
TEST(PlantPersistence, EmptyRosterWritesNoFile) {
    namespace fs = std::filesystem;
    namespace SS = ::Luminumbra::Persistence;
    const fs::path dir = fs::temp_directory_path() / "lumin_plant_persist_empty";
    fs::remove_all(dir);
    std::vector<std::string> errs;
    E::EntityRegistrySnapshot empty;
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(empty, dir, &errs));
    EXPECT_FALSE(fs::exists(SS::WorldSaveService::plant_entities_path(dir)))
        << "no-plant save writes no plant-entities.json";
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, dir, &errs)); // clean miss
    EXPECT_TRUE(loaded.entities.empty());
    fs::remove_all(dir);
}

} // namespace

namespace {
class PlantSaveRecovery : public testing::Test {
private:
    std::filesystem::path m_directory;

protected:
    const std::filesystem::path& Directory() const {
        return m_directory;
    }
    void SetUp() override {
        m_directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant_save_recovery_");
    }
    void TearDown() override {
        Luminumbra::Persistence::WorldSaveService::set_interrupt_before_region_replace_for_testing(
            false);
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
    }
};
} // namespace

TEST_F(PlantSaveRecovery, AbandonedStagingDoesNotBlockLoadsAndIsReclaimedOnEverySave) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto snapshot = F::BuildPlantEntitySnapshot(plants);
    const E::EntityRegistrySnapshot empty;
    const auto abandoned = Directory() / "plant-entities.json.tmp.abandoned";
    const auto unrelated = Directory() / "unrelated.tmp.abandoned";
    for (const bool existing_roster : {false, true}) {
        for (const bool empty_roster : {false, true}) {
            SCOPED_TRACE(existing_roster);
            SCOPED_TRACE(empty_roster);
            ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(
                existing_roster ? snapshot : empty, Directory()));
            ASSERT_TRUE(SS::WorldSaveService::write_file_durably(abandoned, "{partial roster"));
            ASSERT_TRUE(SS::WorldSaveService::write_file_durably(unrelated, "keep"));
            E::EntityRegistrySnapshot loaded;
            ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
            EXPECT_EQ(loaded.entities.empty(), !existing_roster);
            Luminumbra::WorldStreamingState chunks;
            std::vector<std::string> errors;
            SS::WorldSaveService{}.load_world(chunks, Directory(), errors);
            EXPECT_TRUE(errors.empty());
            ASSERT_TRUE(SS::WorldSaveService::validate_save(Directory()));
            ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(empty_roster ? empty : snapshot,
                                                                  Directory()));
            EXPECT_FALSE(fs::exists(abandoned));
            EXPECT_TRUE(fs::exists(unrelated));
            ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
            EXPECT_EQ(loaded.entities.empty(), empty_roster);
        }
    }
}

TEST_F(PlantSaveRecovery, InterruptedReplacementPreservesRosterAndRetryLeavesNoRegionDebris) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    const auto plant = barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(original, Directory()));
    plants.get<C::PlantGrowthComponent>(plant).growth_points = 42;
    const auto changed = F::BuildPlantEntitySnapshot(plants);
    SS::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool saved = SS::WorldSaveService::save_plant_entities(changed, Directory());
    SS::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(saved);
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded),
              E::SerializeEntityRegistrySnapshotJson(original));
    ASSERT_TRUE(SS::WorldSaveService::validate_save(Directory()));
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(changed, Directory()));
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded),
              E::SerializeEntityRegistrySnapshotJson(changed));
    for (const auto& entry : fs::recursive_directory_iterator(Directory()))
        EXPECT_EQ(entry.path().filename().string().find(".tmp."), std::string::npos);
    const auto unknown = SS::WorldSaveService::region_directory(Directory()) / "unknown.tmp";
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(unknown, "unknown artifact"));
    EXPECT_FALSE(SS::WorldSaveService::validate_save(Directory()));
}

TEST_F(PlantSaveRecovery, MissingIsCleanButInvalidPathsAndPayloadsPreserveTheOutput) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    auto loaded = original;
    std::vector<std::string> errors;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_TRUE(loaded.entities.empty());
    EXPECT_TRUE(errors.empty());
    loaded = original;
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(
        loaded, Directory() / std::string(512, 'x'), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    fs::create_directories(path);
    errors.clear();
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    fs::remove(path);
    for (const auto& bytes :
         {std::string{}, std::string("{"), expected.substr(0, expected.size() / 2)}) {
        SCOPED_TRACE(bytes);
        ASSERT_TRUE(SS::WorldSaveService::write_file_durably(path, bytes));
        errors.clear();
        EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    }
}

TEST_F(PlantSaveRecovery, MissingParentsAreCleanButNonDirectoryAndLoopingParentsAreRefused) {
    namespace SS = ::Luminumbra::Persistence;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    auto loaded = original;
    std::vector<std::string> errors;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(
        loaded, Directory() / "missing" / "nested", &errors));
    EXPECT_TRUE(loaded.entities.empty());
    EXPECT_TRUE(errors.empty());
    // Also reach the current-directory ancestor when no absolute root is given.
    const auto relative_missing = Directory().filename() / "missing" / "nested";
    ASSERT_FALSE(std::filesystem::exists(relative_missing.begin()->native()));
    loaded = original;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, relative_missing, &errors));
    EXPECT_TRUE(loaded.entities.empty());
    EXPECT_TRUE(errors.empty());
    const auto parent = Directory() / "file-parent";
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(parent, expected));
    loaded = original;
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, parent, &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
#if !defined(_WIN32)
    const auto loop = Directory() / "loop";
    std::filesystem::create_symlink(loop, loop);
    errors.clear();
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, loop, &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
#endif
}

TEST_F(PlantSaveRecovery, PayloadCapAcceptsTheBoundaryAndRefusesOneByteMore) {
    namespace SS = ::Luminumbra::Persistence;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output);
        output << expected;
        // Bound temporary storage to 64 KiB; valid JSON whitespace reaches the
        // real compatibility boundary without a second payload-sized buffer.
        const std::string padding(std::size_t{64} * 1024, ' ');
        auto remaining = SS::WorldSaveService::kMaxPlantPayloadBytes - expected.size();
        while (remaining != 0) {
            const auto count = std::min<std::uint64_t>(remaining, padding.size());
            output.write(padding.data(), static_cast<std::streamsize>(count));
            remaining -= count;
        }
        output.close();
        ASSERT_TRUE(output);
    }
    auto loaded = original;
    std::vector<std::string> errors;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    EXPECT_TRUE(errors.empty());
    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output.put(' ');
        output.close();
        ASSERT_TRUE(output);
    }
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("256 MiB"), std::string::npos);
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
}

TEST_F(PlantSaveRecovery, SparseOversizedPayloadIsRefusedBeforeAllocationOnAllLoadPaths) {
    namespace SS = ::Luminumbra::Persistence;
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(path, "{}"));
#if defined(_WIN32)
    const HANDLE file = ::CreateFileW(path.c_str(),
                                      GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    const bool sparse =
        ::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
    const bool closed = ::CloseHandle(file) != 0;
    ASSERT_TRUE(sparse);
    ASSERT_TRUE(closed);
#endif
    std::filesystem::resize_file(path, SS::WorldSaveService::kMaxPlantPayloadBytes + 1);
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    auto loaded = original;
    std::vector<std::string> errors;
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("256 MiB"), std::string::npos);
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded),
              E::SerializeEntityRegistrySnapshotJson(original));
    Luminumbra::WorldStreamingState chunks;
    errors.clear();
    EXPECT_FALSE(SS::WorldSaveService{}.load_world(chunks, Directory(), errors));
    EXPECT_FALSE(errors.empty());
    errors.clear();
    EXPECT_FALSE(SS::WorldSaveService::validate_save(Directory(), &errors));
    EXPECT_FALSE(errors.empty());
}

#if defined(_WIN32)
TEST_F(PlantSaveRecovery, WindowsDriveExtendedAndShortSpellingsReadTheSameRoster) {
    namespace SS = ::Luminumbra::Persistence;
    namespace FS = ::Luminumbra::Filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(original, Directory()));
    const auto absolute = FS::AbsolutePath(Directory());
    const auto extended = FS::IsWindowsUncPath(absolute)
                              ? std::filesystem::path(LR"(\\?\UNC\)" + absolute.native().substr(2))
                              : std::filesystem::path(LR"(\\?\)" + absolute.native());
    // Filesystems with short-name generation disabled return the long spelling;
    // both forms must remain usable, including on such runners.
    std::array<wchar_t, 32768> short_name{};
    const DWORD length = ::GetShortPathNameW(
        absolute.c_str(), short_name.data(), static_cast<DWORD>(short_name.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, short_name.size());
    for (const auto& spelling : {absolute, extended, std::filesystem::path(short_name.data())}) {
        E::EntityRegistrySnapshot loaded;
        std::vector<std::string> errors;
        ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, spelling, &errors));
        EXPECT_TRUE(errors.empty());
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    }
}

TEST_F(PlantSaveRecovery, WindowsLockedPayloadIsARefusalAndPreservesOutput) {
    namespace SS = ::Luminumbra::Persistence;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(original, Directory()));
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    const HANDLE file = ::CreateFileW(path.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    OVERLAPPED range{};
    const bool locked =
        ::LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range) !=
        0;
    if (!locked) {
        ::CloseHandle(file);
        FAIL() << "cannot lock plant payload";
    }
    auto loaded = original;
    std::vector<std::string> errors;
    const bool read = SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors);
    const bool unlocked = ::UnlockFileEx(file, 0, 1, 0, &range) != 0;
    const bool closed = ::CloseHandle(file) != 0;
    EXPECT_FALSE(read);
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    EXPECT_TRUE(unlocked);
    EXPECT_TRUE(closed);
}

TEST_F(PlantSaveRecovery, WindowsStreamReadersReportInitialAndPartialReadFailures) {
    namespace SS = ::Luminumbra::Persistence;
    const auto path = Directory() / "world_info.json";
    std::string bytes(std::size_t{128} * 1024, ' ');
    bytes.replace(0, 2, "{}");
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(path, bytes));
    for (const DWORD offset : {0u, 64u * 1024u}) {
        const HANDLE file = ::CreateFileW(path.c_str(),
                                          GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
        ASSERT_NE(file, INVALID_HANDLE_VALUE);
        OVERLAPPED range{};
        range.Offset = offset;
        const bool locked =
            ::LockFileEx(
                file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range) != 0;
        if (!locked) {
            ::CloseHandle(file);
            FAIL() << "cannot lock metadata payload";
        }
        Luminumbra::world::WorldClock clock;
        bool required = false;
        std::vector<std::string> errors;
        const bool read =
            SS::WorldSaveService::read_clock_metadata(Directory(), clock, required, &errors);
        const bool unlocked = ::UnlockFileEx(file, 0, 1, 0, &range) != 0;
        const bool closed = ::CloseHandle(file) != 0;
        EXPECT_FALSE(read);
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(unlocked);
        EXPECT_TRUE(closed);
    }
}

TEST_F(PlantSaveRecovery, WindowsInvalidNamesCannotNormalizeIntoCleanAbsenceOrAnotherRoster) {
    namespace SS = ::Luminumbra::Persistence;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    // The ordinary spelling exists, so dot/space normalization would read a
    // different roster successfully even if all Win32 open errors were refused.
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(original, Directory() / "alias"));
    for (const auto& name : {std::string(512, 'x'),
                             std::string("alias."),
                             std::string("alias "),
                             std::string("NUL"),
                             std::string("con.txt"),
                             std::string("COM1"),
                             std::string("LPT9.txt")}) {
        SCOPED_TRACE(name);
        auto loaded = original;
        std::vector<std::string> errors;
        EXPECT_FALSE(
            SS::WorldSaveService::load_plant_entities(loaded, Directory() / name, &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    }
}

TEST_F(PlantSaveRecovery, WindowsReparseEntriesAreRefusedIncludingWithoutSymlinkPrivilege) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    fs::create_directories(path.parent_path());
    const auto target = Directory() / "valid-roster.json";
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(target, expected));
    // Developer-mode file symlinks need no elevation. If that mode is unavailable,
    // assert the denied operation and exercise a mount-point reparse entry below.
    const bool linked = ::CreateSymbolicLinkW(path.c_str(),
                                              target.c_str(),
                                              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    if (!linked) {
        EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_PRIVILEGE_NOT_HELD));
        fs::create_directory(path);
        const auto junction_target = L"\\??\\" + fs::absolute(Directory()).native();
        struct MountPointData {
            DWORD tag;
            WORD data_length;
            WORD reserved;
            WORD substitute_offset;
            WORD substitute_length;
            WORD print_offset;
            WORD print_length;
            // The complete reparse buffer is bounded to the Win32 16 KiB limit.
            std::array<wchar_t, (MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 16) / sizeof(wchar_t)> names;
        } data{};
        ASSERT_LT(junction_target.size() + 2, data.names.size());
        data.tag = IO_REPARSE_TAG_MOUNT_POINT;
        data.substitute_length = static_cast<WORD>(junction_target.size() * sizeof(wchar_t));
        data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
        data.data_length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
        std::copy(junction_target.begin(), junction_target.end(), data.names.begin());
        const HANDLE directory =
            ::CreateFileW(path.c_str(),
                          GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                          nullptr);
        ASSERT_NE(directory, INVALID_HANDLE_VALUE);
        DWORD returned = 0;
        const bool installed = ::DeviceIoControl(directory,
                                                 FSCTL_SET_REPARSE_POINT,
                                                 &data,
                                                 static_cast<DWORD>(data.data_length) + 8,
                                                 nullptr,
                                                 0,
                                                 &returned,
                                                 nullptr) != 0;
        const bool closed = ::CloseHandle(directory) != 0;
        ASSERT_TRUE(installed);
        ASSERT_TRUE(closed);
    }
    EXPECT_NE(::GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT, 0u);
    auto loaded = original;
    std::vector<std::string> errors;
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    fs::remove(path);
}
#endif

#if !defined(_WIN32)
TEST_F(PlantSaveRecovery, SymlinksIncludingDeviceAndDanglingTargetsAreRefusedBeforeReading) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    fs::create_directories(path.parent_path());
    const auto valid_target = Directory() / "valid-roster.json";
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(valid_target, expected));
    for (const auto& target : {valid_target, Directory() / "absent", fs::path("/dev/zero")}) {
        SCOPED_TRACE(target);
        fs::create_symlink(target, path);
        auto loaded = original;
        std::vector<std::string> errors;
        ASSERT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
        Luminumbra::WorldStreamingState chunks;
        errors.clear();
        EXPECT_FALSE(SS::WorldSaveService{}.load_world(chunks, Directory(), errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(fs::is_symlink(path));
        fs::remove(path);
    }
}

TEST_F(PlantSaveRecovery, FifoWithValidPayloadIsRefusedBeforeOpeningAStream) {
    namespace SS = ::Luminumbra::Persistence;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    std::filesystem::create_directories(path.parent_path());
    ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
    // Keep a nonblocking reader alive so a finite valid payload survives the
    // writer's close. Even the unsafe stream reader reaches EOF without hanging.
    const int keeper = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
    ASSERT_GE(keeper, 0);
    struct CloseReader {
        int descriptor;
        ~CloseReader() {
            ::close(descriptor);
        }
    } close_reader{keeper};
    const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    ASSERT_GE(writer, 0);
    const auto written = ::write(writer, expected.data(), expected.size());
    const int closed = ::close(writer);
    ASSERT_EQ(written, static_cast<ssize_t>(expected.size()));
    ASSERT_EQ(closed, 0);
    auto loaded = original;
    std::vector<std::string> errors;
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    Luminumbra::WorldStreamingState chunks;
    errors.clear();
    EXPECT_FALSE(SS::WorldSaveService{}.load_world(chunks, Directory(), errors));
    EXPECT_FALSE(errors.empty());
}
#endif

TEST(PlantPersistence, EmptyRosterRemovalErrorIsReportedAndCanRetry) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    const auto dir = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant_remove_error_");
    const auto path = SS::WorldSaveService::plant_entities_path(dir);
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto snapshot = F::BuildPlantEntitySnapshot(plants);
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(snapshot, dir));
    ASSERT_TRUE(fs::is_regular_file(path));
#ifdef _WIN32
    // MSVC remove can delete read-only files. A CRT reader denies delete sharing
    // while allowing the validation reader to inspect the existing valid roster.
    std::ifstream held_roster(path, std::ios::binary);
    ASSERT_TRUE(held_roster);
#else
    const auto file_permissions = fs::status(path).permissions();
    const auto parent_permissions = fs::status(path.parent_path()).permissions();
    // POSIX checks the parent, while validation can still read the valid roster.
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    fs::permissions(path.parent_path(),
                    fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
#endif
    const E::EntityRegistrySnapshot empty;
    std::vector<std::string> errors;
    const bool removed = SS::WorldSaveService::save_plant_entities(empty, dir, &errors);
#ifdef _WIN32
    held_roster.close();
    EXPECT_FALSE(held_roster.fail());
#else
    fs::permissions(path.parent_path(), parent_permissions);
    if (fs::exists(path))
        fs::permissions(path, file_permissions);
#endif
    EXPECT_FALSE(removed);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("failed to remove plant entities file"), std::string::npos);
    EXPECT_TRUE(fs::exists(path));
    E::EntityRegistrySnapshot retained;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(retained, dir));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(retained),
              E::SerializeEntityRegistrySnapshotJson(snapshot));
    errors.clear();
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(empty, dir, &errors));
    EXPECT_FALSE(fs::exists(path));
    // Invalid occupied directories must also fail without deleting their contents.
    fs::create_directories(path);
    {
        std::ofstream keep(path / "keep");
        keep << "occupied";
    }
    EXPECT_FALSE(SS::WorldSaveService::save_plant_entities(empty, dir, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_TRUE(fs::exists(path / "keep"));
    fs::remove_all(path);
    errors.clear();
    EXPECT_TRUE(SS::WorldSaveService::save_plant_entities(empty, dir, &errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_FALSE(fs::exists(path));
    fs::remove_all(dir);
}
