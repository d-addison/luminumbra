#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/systems/PollinationSystem.h"

namespace {
namespace C = Luminumbra::Components;
namespace E = Luminumbra::Ecs;
namespace F = luminumbra::foliage;

entt::entity MakePlant(entt::registry& registry, float x = 0.0f) {
    const auto entity = registry.create();
    registry.emplace<C::PlantTag>(entity);
    registry.emplace<C::PlantGrowthComponent>(entity).stage =
        static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    registry.emplace<C::PlantGenomeComponent>(entity).genes.fill(0.25f + x * 0.1f);
    registry.emplace<C::TransformComponent>(entity).position = Luminumbra::Vec3(x, 0, 0);
    registry.emplace<C::PollinationTag>(entity);
    registry.emplace<C::PollinationComponent>(entity);
    return entity;
}

nlohmann::json& AllocatorData(E::EntityRegistrySnapshot& snapshot) {
    auto& components = snapshot.entities.front().components;
    return std::find_if(components.begin(),
                        components.end(),
                        [](const auto& component) { return component.type == "PlantAllocator"; })
        ->data;
}

TEST(PlantAllocatorPersistence, DeletedMiddleSlotKeepsNextIdAndPollinationDraw) {
    entt::registry continuing;
    const auto first = MakePlant(continuing);
    const auto deleted = MakePlant(continuing, 1.0f);
    const auto last = MakePlant(continuing, 2.0f);
    ASSERT_EQ(entt::to_integral(first), 0u);
    ASSERT_EQ(entt::to_integral(deleted), 1u);
    ASSERT_EQ(entt::to_integral(last), 2u);
    continuing.destroy(deleted);
    const auto bytes =
        E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(continuing));
    E::EntityRegistrySnapshot loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(bytes, loaded, errors));
    entt::registry restored;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(restored, loaded));
    const auto expected = MakePlant(continuing, 1.0f);
    const auto actual = MakePlant(restored, 1.0f);
    ASSERT_EQ(entt::to_entity(expected), 1u);
    ASSERT_EQ(entt::to_version(expected), 1u);
    EXPECT_EQ(actual, expected);
    auto expected_rng =
        F::PollinationPairRng(entt::to_integral(expected), entt::to_integral(last), 901);
    auto actual_rng =
        F::PollinationPairRng(entt::to_integral(actual), entt::to_integral(last), 901);
    EXPECT_EQ(actual_rng.next_u64(), expected_rng.next_u64());
    ASSERT_EQ(F::RunPollinationOnTick(continuing, 901).crossed, 3);
    ASSERT_EQ(F::RunPollinationOnTick(restored, 901).crossed, 3);
    for (const auto entity : {first, last, expected}) {
        ASSERT_TRUE(restored.valid(entity));
        EXPECT_EQ(restored.get<C::PollinationComponent>(entity).next_genome.genes,
                  continuing.get<C::PollinationComponent>(entity).next_genome.genes);
    }
}

TEST(PlantAllocatorPersistence, FreeTailOrderVersionsAndSparseFreshIdsContinueExactly) {
    entt::registry source;
    MakePlant(source);
    const auto first = source.create();
    const auto second = source.create();
    const auto high = source.create(entt::entt_traits<entt::entity>::construct(50, 7));
    source.destroy(second);
    source.destroy(first);
    source.destroy(high);
    const auto reused = source.create();
    source.destroy(reused);
    entt::registry target;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, F::BuildPlantEntitySnapshot(source)));
    for (int index = 0; index < 8; ++index)
        EXPECT_EQ(target.create(), source.create());
}

TEST(PlantAllocatorPersistence, ReplacementPreservesForeignComponentsAndRewindsHistory) {
    entt::registry source;
    const auto foreign = source.create();
    const auto plant = MakePlant(source);
    const auto dead = MakePlant(source);
    source.destroy(dead);
    const auto snapshot = F::BuildPlantEntitySnapshot(source);
    entt::registry target;
    ASSERT_EQ(target.create(), foreign);
    target.emplace<C::TransformComponent>(foreign).position = Luminumbra::Vec3(9.0f);
    MakePlant(target);
    MakePlant(target);
    MakePlant(target);
    for (int reload = 0; reload < 2; ++reload) {
        ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, snapshot, true));
        EXPECT_EQ(target.view<C::PlantTag>().size(), 1u);
        EXPECT_EQ(target.get<C::TransformComponent>(foreign).position, Luminumbra::Vec3(9.0f));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  E::SerializeEntityRegistrySnapshotJson(snapshot));
        EXPECT_EQ(target.create(), entt::entt_traits<entt::entity>::construct(2, 1));
        target.destroy(entt::entt_traits<entt::entity>::construct(2, 1));
        EXPECT_TRUE(target.valid(plant));
    }
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, {}, true));
    EXPECT_TRUE(target.view<C::PlantTag>().empty());
    EXPECT_TRUE(target.valid(foreign));
}

TEST(PlantAllocatorPersistence, ForeignIdentityMismatchRefusesWithoutChangingAnyStorage) {
    entt::registry source;
    (void)source.create();
    MakePlant(source);
    const auto snapshot = F::BuildPlantEntitySnapshot(source);
    for (int kind = 0; kind < 4; ++kind) {
        SCOPED_TRACE(kind);
        entt::registry target;
        if (kind != 0) {
            const auto foreign = target.create(entt::entt_traits<entt::entity>::construct(
                kind == 2 ? 3u : 0u, kind == 1 ? 1u : 0u));
            target.emplace<C::TransformComponent>(foreign).position = Luminumbra::Vec3(7.0f);
            if (kind == 3)
                (void)target.create();
        }
        MakePlant(target);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::CanApplyPlantEntitySnapshot(target, snapshot, true));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot, true));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
    }
}

TEST(PlantAllocatorPersistence, AppendAdmissionCannotDiscardAResidentPlant) {
    entt::registry source;
    MakePlant(source);
    const auto snapshot = F::BuildPlantEntitySnapshot(source);
    entt::registry target;
    const auto resident = target.create(entt::entt_traits<entt::entity>::construct(2, 3));
    target.emplace<C::PlantTag>(resident);
    target.emplace<C::PlantGrowthComponent>(resident);
    target.emplace<C::PlantGenomeComponent>(resident);
    target.emplace<C::TransformComponent>(resident);
    const auto before = E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
    EXPECT_FALSE(F::CanApplyPlantEntitySnapshot(target, snapshot, false));
    EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot, false));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)), before);
    EXPECT_TRUE(target.valid(resident));
    EXPECT_EQ(entt::to_integral(target.create()), 0u);
    entt::registry empty;
    EXPECT_TRUE(F::ApplyPlantEntitySnapshot(empty, snapshot, false));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(empty)),
              E::SerializeEntityRegistrySnapshotJson(snapshot));
}

TEST(PlantAllocatorPersistence, InvalidDescriptorRefusesWholeRosterAndKeepsNextAllocation) {
    entt::registry source;
    MakePlant(source);
    MakePlant(source);
    const auto original = F::BuildPlantEntitySnapshot(source);
    std::vector<E::EntityRegistrySnapshot> invalid;
    for (const auto& value : {nlohmann::json(nullptr), nlohmann::json::array()}) {
        auto snapshot = original;
        AllocatorData(snapshot) = value;
        invalid.push_back(snapshot);
    }
    for (const auto& field : {"live", "packed"}) {
        auto missing = original;
        AllocatorData(missing).erase(field);
        invalid.push_back(missing);
        for (const auto& value : std::vector<nlohmann::json>{nullptr, true, -1, 1.5, 1e38, "2"}) {
            auto snapshot = original;
            AllocatorData(snapshot)[field] = value;
            invalid.push_back(snapshot);
        }
    }
    for (const auto& packed : std::vector<nlohmann::json>{
             nlohmann::json::array(),
             nlohmann::json::array({0, 0}),
             nlohmann::json::array(
                 {0, entt::to_integral(entt::entt_traits<entt::entity>::construct(0, 1))}),
             nlohmann::json::array({0, -1}),
             nlohmann::json::array({0, 1.5}),
             nlohmann::json::array({0, std::numeric_limits<std::uint64_t>::max()}),
             nlohmann::json::array({0, entt::to_integral(static_cast<entt::entity>(entt::null))}),
             nlohmann::json::array(
                 {0, entt::to_integral(static_cast<entt::entity>(entt::tombstone))})}) {
        auto snapshot = original;
        AllocatorData(snapshot)["packed"] = packed;
        invalid.push_back(snapshot);
    }
    for (const auto live : {0, 1, 3}) {
        auto snapshot = original;
        AllocatorData(snapshot)["live"] = live;
        invalid.push_back(snapshot);
    }
    auto duplicate = original;
    duplicate.entities.back().components.push_back({"PlantAllocator", AllocatorData(duplicate)});
    invalid.push_back(duplicate);
    auto legacy_id = original;
    legacy_id.entities.back().has_entity_id = false;
    invalid.push_back(legacy_id);
    auto wrong_version = original;
    wrong_version.entities.back().entity_id =
        entt::to_integral(entt::entt_traits<entt::entity>::construct(1, 2));
    invalid.push_back(wrong_version);
    for (const auto& snapshot : invalid) {
        SCOPED_TRACE(E::SerializeEntityRegistrySnapshotJson(snapshot));
        entt::registry target;
        const auto resident = MakePlant(target);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot, true));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
        EXPECT_TRUE(target.valid(resident));
        EXPECT_EQ(entt::to_integral(target.create()), 1u);
    }
    auto extended = original;
    AllocatorData(extended)["future_metadata"] = {{"opaque", true}};
    entt::registry target;
    EXPECT_TRUE(F::ApplyPlantEntitySnapshot(target, extended));
}

TEST(PlantAllocatorPersistence, DescriptorCannotExceedTheEnTTSlotBound) {
    entt::registry source;
    MakePlant(source);
    auto snapshot = F::BuildPlantEntitySnapshot(source);
    AllocatorData(snapshot)["packed"] = nlohmann::json::array_t(
        static_cast<std::size_t>(entt::entt_traits<entt::entity>::entity_mask) + 1, 0);
    entt::registry target;
    EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
    EXPECT_EQ(entt::to_integral(target.create()), 0u);
}
} // namespace
