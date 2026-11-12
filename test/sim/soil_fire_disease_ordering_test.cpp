#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "systems/FireSpreadSystem.h"
#include "systems/PlantDiseaseSystem.h"
#include "systems/SoilNutrientSystem.h"

namespace {
namespace Comp = Luminumbra::Components;
namespace F = luminumbra::foliage;
namespace S = luminumbra::sim;
using Luminumbra::world::RegionConsumerGate;
using Luminumbra::world::RegionKey;

entt::entity Identified(entt::registry& registry, std::uint64_t id, bool churn) {
    if (churn) {
        // Unrelated residents, a hole and a recycled generation must not affect order.
        registry.emplace<Comp::TransformComponent>(registry.create());
        const auto hole = registry.create();
        registry.destroy(hole);
    }
    const auto entity = registry.create();
    registry.emplace<Comp::SimulationIdentityComponent>(entity, id);
    registry.emplace<Comp::TransformComponent>(entity);
    return entity;
}

TEST(SoilFireDiseaseOrdering, DepletionFollowsAll64IdentityBitsAcrossPermutationsAndGates) {
    constexpr std::array<std::uint64_t, 4> ids{
        1, 256, (std::uint64_t{1} << 40) + 1, (std::uint64_t{1} << 63) - 1};
    for (const bool regional : {false, true}) {
        std::array<unsigned, 4> permutation{0, 1, 2, 3};
        do {
            entt::registry registry;
            std::array<entt::entity, 4> plants{};
            for (const auto index : permutation) {
                const auto plant = Identified(registry, ids[index], true);
                plants[index] = plant;
                registry.emplace<Comp::PlantTag>(plant);
                registry.emplace<Comp::PlantGrowthComponent>(plant).stage =
                    static_cast<std::uint8_t>(Comp::PlantStage::Mature);
                registry.emplace<Comp::SoilFeederComponent>(plant).uptake = 1000;
            }
            // Ordering work must count participants, independent of unrelated residents.
            for (unsigned index = 0; index < 128; ++index)
                registry.emplace<Comp::TransformComponent>(registry.create());
            const std::array due{RegionKey{0, 0}};
            const auto gate = regional ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
            F::SoilGrid soil(1, 1, 750);
            const auto stats = F::RunSoilNutrientOnTick<true>(registry, soil, 0, 0, 1, gate);
            EXPECT_EQ(stats.participants, 4);
            EXPECT_EQ(stats.consumed, 750);
            EXPECT_EQ(stats.ordering_visits, 18u * plants.size() - 1u);
            EXPECT_EQ(soil.AtCell(0, 0), F::kSoilRegenPerTick);
            for (std::size_t index = 0; index < plants.size(); ++index) {
                const std::array<std::uint32_t, 4> expected{500, 250, 0, 0};
                EXPECT_EQ(registry.get<Comp::SoilFeederComponent>(plants[index]).absorbed,
                          expected[index]);
            }
        } while (std::next_permutation(permutation.begin(), permutation.end()));
    }
}

TEST(SoilFireDiseaseOrdering, DormantSoilStillAwardsTheFirstHandleWithoutIdentities) {
    entt::registry registry;
    std::array<entt::entity, 2> plants{};
    for (std::size_t index = 0; index < plants.size(); ++index) {
        const auto plant = Identified(registry, 2u - index, false);
        plants[index] = plant;
        registry.emplace<Comp::PlantTag>(plant);
        registry.emplace<Comp::PlantGrowthComponent>(plant).stage =
            static_cast<std::uint8_t>(Comp::PlantStage::Mature);
        registry.emplace<Comp::SoilFeederComponent>(plant).uptake = 1000;
    }
    // Conflicting identities do not select the new ordering implicitly.
    F::SoilGrid soil(1, 1, 500);
    EXPECT_EQ(F::RunSoilNutrientOnTick(registry, soil, 0, 0, 1).ordering_visits, 0u);
    EXPECT_EQ(registry.get<Comp::SoilFeederComponent>(plants[0]).absorbed, 500u);
    EXPECT_EQ(registry.get<Comp::SoilFeederComponent>(plants[1]).absorbed, 0u);
    registry.clear<Comp::SimulationIdentityComponent>();
    soil.SetCell(0, 0, 500);
    F::RunSoilNutrientOnTick(registry, soil, 0, 0, 1);
    EXPECT_EQ(registry.get<Comp::SoilFeederComponent>(plants[0]).absorbed, 1000u);
}

// Each list independently exercises a binary32 threshold: big-first addition loses
// two half-ulp terms; small-first addition reaches the resistance exactly. Stable IDs
// prescribe small-first. The source kind changes, the mathematical contributions do not.
template<bool StableIdentity>
Comp::BurnState FireThreshold(const std::array<unsigned, 4>& permutation,
                              bool explicit_sources,
                              bool regional,
                              bool churn) {
    entt::registry registry;
    constexpr std::array<float, 4> heat{1.0f, 0x1.47ac8p-7f, 0x1p-24f, 0x1p-24f};
    // For burning sources, 1-distance yields the same four contributions above.
    constexpr std::array<float, 4> distance{0.0f, 0x1.fae14ep-1f, 0x1.fffffep-1f, 0x1.fffffep-1f};
    constexpr std::array<std::uint64_t, 4> ids{
        (std::uint64_t{1} << 62) + 1, (std::uint64_t{1} << 40) + 1, 256, 1};
    for (const auto index : permutation) {
        const auto source = Identified(registry, ids[index], churn);
        if (explicit_sources) {
            auto& ignition = registry.emplace<Comp::IgnitionSourceComponent>(source);
            ignition.radius = 1.0f;
            ignition.intensity = heat[index];
        } else {
            registry.get<Comp::TransformComponent>(source).position.x = distance[index];
            auto& fuel = registry.emplace<Comp::CombustibleComponent>(source);
            fuel.set_state(Comp::BurnState::Burning);
            fuel.ignition_radius = 1.0f;
            fuel.burn_ticks_remaining = 2;
        }
    }
    const auto candidate = Identified(registry, 512, churn);
    auto& fuel = registry.emplace<Comp::CombustibleComponent>(candidate);
    fuel.moisture_milli = 640;
    fuel.fuel_milli = 1000;
    const std::array due{RegionKey{0, 0}};
    const auto gate = regional ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
    const auto stats =
        S::RunFireSpreadOnTick<StableIdentity>(registry, 1, glm::vec2(0.0f), {}, gate);
    if constexpr (StableIdentity) {
        // The interleaved candidate ID adds three cross-list comparisons.
        const auto expected = explicit_sources ? (18u * 4u - 1u) + (18u - 1u) + 3u : 18u * 5u - 1u;
        EXPECT_EQ(stats.ordering_visits, expected);
    }
    return fuel.state();
}

TEST(SoilFireDiseaseOrdering, BothFireListsIgnoreHandlePermutationsAtTheHeatThreshold) {
    for (const bool explicit_sources : {false, true}) {
        for (const bool regional : {false, true}) {
            std::array<unsigned, 4> permutation{0, 1, 2, 3};
            do {
                EXPECT_EQ(FireThreshold<true>(permutation, explicit_sources, regional, true),
                          Comp::BurnState::Burning);
            } while (std::next_permutation(permutation.begin(), permutation.end()));
        }
        // Controls ensure this oracle distinguishes handle accumulation from stable order.
        EXPECT_EQ(FireThreshold<false>({0, 1, 2, 3}, explicit_sources, false, false),
                  Comp::BurnState::Unburnt);
        EXPECT_EQ(FireThreshold<false>({3, 2, 1, 0}, explicit_sources, false, false),
                  Comp::BurnState::Burning);
    }
}

TEST(SoilFireDiseaseOrdering, ExcludedParticipantsNeedNoIdentityAndRemainUnchanged) {
    entt::registry registry;
    const auto excluded = registry.create();
    registry.emplace<Comp::TransformComponent>(excluded).position.x = 512;
    registry.emplace<Comp::PlantTag>(excluded);
    registry.emplace<Comp::PlantGrowthComponent>(excluded);
    registry.emplace<Comp::SoilFeederComponent>(excluded);
    registry.emplace<Comp::PlantHealthComponent>(excluded);
    registry.emplace<Comp::CombustibleComponent>(excluded).set_state(Comp::BurnState::Burning);
    registry.emplace<Comp::IgnitionSourceComponent>(excluded);
    const std::array due{RegionKey{0, 0}};
    const RegionConsumerGate gate(registry, due);
    F::SoilGrid soil(1, 1, 100);
    EXPECT_EQ(F::RunSoilNutrientOnTick<true>(registry, soil, 512, 0, 1, gate).participants, 0);
    EXPECT_EQ(soil.AtCell(0, 0), 100);
    EXPECT_EQ(S::RunFireSpreadOnTick<true>(registry, 1, glm::vec2(0.0f), {}, gate).considered, 0);
    EXPECT_EQ(F::RunPlantDiseaseOnTick<true>(registry, 1, 0, gate).considered, 0);
    EXPECT_EQ(registry.get<Comp::SoilFeederComponent>(excluded).absorbed, 0u);
    EXPECT_EQ(registry.get<Comp::CombustibleComponent>(excluded).state(), Comp::BurnState::Burning);
    EXPECT_EQ(registry.get<Comp::PlantHealthComponent>(excluded).infection, 0u);
}

template<bool StableIdentity>
std::vector<std::uint32_t>
DiseaseTrace(const std::array<unsigned, 6>& permutation, bool regional, bool churn) {
    entt::registry registry;
    std::array<entt::entity, 6> plants{};
    constexpr std::array states{Comp::PlantDiseaseState::Infected,
                                Comp::PlantDiseaseState::Infected,
                                Comp::PlantDiseaseState::Healthy,
                                Comp::PlantDiseaseState::Recovered,
                                Comp::PlantDiseaseState::Dead,
                                Comp::PlantDiseaseState::Healthy};
    constexpr std::array<float, 6> positions{0, 2, 1, 1, 3, 6};
    for (const auto index : permutation) {
        const auto plant = Identified(registry, 10u + index, churn);
        plants[index] = plant;
        registry.emplace<Comp::PlantTag>(plant);
        registry.get<Comp::TransformComponent>(plant).position.x = positions[index];
        auto& health = registry.emplace<Comp::PlantHealthComponent>(plant);
        health.state = static_cast<std::uint8_t>(states[index]);
        health.infection = 400;
        health.resistance = index == 1 || index == 3 ? 600 : 0;
        health.infected_ticks = index < 2 ? F::kSickTicks - 1 : 0;
    }
    const std::array due{RegionKey{0, 0}};
    const auto gate = regional ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
    std::vector<std::uint32_t> trace;
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        const auto stats = F::RunPlantDiseaseOnTick<StableIdentity>(registry, tick, 0, gate);
        EXPECT_EQ(stats.considered, 6);
        if (tick == 1) {
            EXPECT_EQ(stats.died, 1);
            EXPECT_EQ(stats.recovered, 1);
            EXPECT_EQ(stats.new_infections, 1);
        }
        trace.push_back(static_cast<std::uint32_t>(stats.infected));
        trace.push_back(static_cast<std::uint32_t>(stats.new_infections));
        trace.push_back(static_cast<std::uint32_t>(stats.recovered));
        trace.push_back(static_cast<std::uint32_t>(stats.died));
        for (const auto plant : plants) {
            const auto& health = registry.get<Comp::PlantHealthComponent>(plant);
            trace.push_back(health.infection);
            trace.push_back(health.resistance);
            trace.push_back(health.state);
            trace.push_back(health.infected_ticks);
        }
    }
    return trace;
}

TEST(SoilFireDiseaseOrdering, DiseaseIntegerReductionAndWritesArePermutationInvariant) {
    const auto expected = DiseaseTrace<false>({0, 1, 2, 3, 4, 5}, false, false);
    for (const bool regional : {false, true}) {
        std::array<unsigned, 6> permutation{0, 1, 2, 3, 4, 5};
        do {
            EXPECT_EQ(DiseaseTrace<true>(permutation, regional, true), expected);
        } while (std::next_permutation(permutation.begin(), permutation.end()));
    }
}
} // namespace
