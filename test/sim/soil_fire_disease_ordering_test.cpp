#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
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

void ExpectDiseaseRosterUnchanged(const entt::registry& registry,
                                  const std::array<entt::entity, 5>& plants,
                                  const std::array<Comp::PlantHealthComponent, 5>& initial,
                                  std::size_t rejected,
                                  const std::optional<std::uint64_t>& rejected_id) {
    for (std::size_t index = 0; index < plants.size(); ++index) {
        const auto plant = plants[index];
        ASSERT_TRUE(registry.valid(plant));
        EXPECT_TRUE(registry.all_of<Comp::PlantTag>(plant));
        const auto& health = registry.get<Comp::PlantHealthComponent>(plant);
        EXPECT_EQ(health.infection, initial[index].infection);
        EXPECT_EQ(health.resistance, initial[index].resistance);
        EXPECT_EQ(health.state, initial[index].state);
        EXPECT_EQ(health.infected_ticks, initial[index].infected_ticks);
        const auto& transform = registry.get<Comp::TransformComponent>(plant);
        EXPECT_EQ(transform.position, glm::vec3(static_cast<float>(index), 0, 0));
        EXPECT_EQ(transform.rotation, glm::quat(1, 0, 0, 0));
        EXPECT_EQ(transform.scale, glm::vec3(1));
        const auto* identity = registry.try_get<Comp::SimulationIdentityComponent>(plant);
        if (index == rejected && !rejected_id) {
            EXPECT_EQ(identity, nullptr);
        } else {
            ASSERT_NE(identity, nullptr);
            EXPECT_EQ(identity->id, index == rejected ? rejected_id.value_or(0) : 100u + index);
        }
    }
}

void ExpectDiseaseIdentityRefusal(const std::optional<std::uint64_t>& rejected_id,
                                  bool has_sources) {
    std::array<Comp::PlantHealthComponent, 5> initial{{
        {.infection = 400,
         .resistance = 0,
         .state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Infected),
         .infected_ticks = F::kSickTicks - 1},
        {.infection = 400,
         .resistance = 600,
         .state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Infected),
         .infected_ticks = F::kSickTicks - 1},
        {.infection = 490,
         .resistance = 0,
         .state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Healthy),
         .infected_ticks = 0},
        {.infection = 100,
         .resistance = 600,
         .state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Recovered),
         .infected_ticks = 0},
        {.infection = 1000,
         .resistance = 0,
         .state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Dead),
         .infected_ticks = F::kSickTicks},
    }};
    if (!has_sources) {
        for (auto& health : initial) {
            if (health.state == static_cast<std::uint8_t>(Comp::PlantDiseaseState::Infected)) {
                health.state = static_cast<std::uint8_t>(Comp::PlantDiseaseState::Healthy);
                health.infected_ticks = 0;
            }
        }
    }
    for (const bool regional : {false, true}) {
        // Every role, including a terminal plant, must be admitted before any writes.
        for (std::size_t rejected = 0; rejected < initial.size(); ++rejected) {
            if (rejected_id && *rejected_id == 100u + rejected) {
                continue;
            }
            entt::registry registry;
            std::array<entt::entity, 5> plants{};
            for (std::size_t index = 0; index < plants.size(); ++index) {
                plants[index] = Identified(registry, 100u + index, true);
                registry.emplace<Comp::PlantTag>(plants[index]);
                registry.emplace<Comp::PlantHealthComponent>(plants[index], initial[index]);
                registry.get<Comp::TransformComponent>(plants[index]).position.x =
                    static_cast<float>(index);
            }
            registry.remove<Comp::SimulationIdentityComponent>(plants[rejected]);
            if (rejected_id) {
                registry.emplace<Comp::SimulationIdentityComponent>(plants[rejected], *rejected_id);
            }
            const std::array due{RegionKey{0, 0}};
            const auto gate = regional ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
            auto expected_rng = registry.ctx().emplace<luminumbra::core::DeterministicRng>(123);
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                EXPECT_THROW(
                    static_cast<void>(F::RunPlantDiseaseOnTick<true>(registry, 1, 123, gate)),
                    std::logic_error);
                ExpectDiseaseRosterUnchanged(registry, plants, initial, rejected, rejected_id);
                EXPECT_EQ(registry.ctx().get<luminumbra::core::DeterministicRng>().next_u64(),
                          expected_rng.next_u64());
            }
            // Shipping mode accepts broken IDs, with or without infectious sources.
            const auto stats = F::RunPlantDiseaseOnTick(registry, 1, 123, gate);
            EXPECT_EQ(stats.considered, 5);
            EXPECT_EQ(stats.infected, has_sources ? 1 : 0);
            EXPECT_EQ(stats.new_infections, has_sources ? 1 : 0);
            EXPECT_EQ(stats.recovered, has_sources ? 1 : 0);
            EXPECT_EQ(stats.died, has_sources ? 1 : 0);
        }
    }
}

TEST(SoilFireDiseaseOrdering, CandidateDiseaseRefusesMissingIdentitiesBeforeMutation) {
    for (const bool has_sources : {false, true}) {
        ExpectDiseaseIdentityRefusal(std::nullopt, has_sources);
    }
}

TEST(SoilFireDiseaseOrdering, CandidateDiseaseRefusesInvalidIdentitiesBeforeMutation) {
    for (const auto id : {std::uint64_t{0},
                          std::uint64_t{1} << 63,
                          (std::uint64_t{1} << 63) + 1,
                          ~std::uint64_t{0}}) {
        for (const bool has_sources : {false, true}) {
            ExpectDiseaseIdentityRefusal(id, has_sources);
        }
    }
}

TEST(SoilFireDiseaseOrdering, CandidateDiseaseRefusesDuplicateIdentitiesBeforeMutation) {
    // Exercise collisions with both ends of the participant list, excluding self matches.
    for (const auto id : {std::uint64_t{100}, std::uint64_t{104}}) {
        for (const bool has_sources : {false, true}) {
            ExpectDiseaseIdentityRefusal(id, has_sources);
        }
    }
}

TEST(SoilFireDiseaseOrdering, CandidateDiseaseAdmitsBoundaryIdsAndIgnoresNonparticipants) {
    entt::registry registry;
    for (const auto id : {std::uint64_t{1}, (std::uint64_t{1} << 63) - 1}) {
        const auto plant = Identified(registry, id, true);
        registry.emplace<Comp::PlantTag>(plant);
        registry.emplace<Comp::PlantHealthComponent>(plant).state =
            static_cast<std::uint8_t>(Comp::PlantDiseaseState::Dead);
    }
    // Each missing required component excludes the entity, even with a colliding ID.
    for (unsigned missing = 0; missing < 3; ++missing) {
        const auto entity = Identified(registry, 1, false);
        if (missing != 0) {
            registry.emplace<Comp::PlantTag>(entity);
        }
        if (missing != 1) {
            registry.emplace<Comp::PlantHealthComponent>(entity).infection = 100;
        }
        if (missing == 2) {
            registry.remove<Comp::TransformComponent>(entity);
        }
    }
    EXPECT_EQ(F::RunPlantDiseaseOnTick<true>(registry, 1).considered, 2);
    const auto excluded = Identified(registry, 0, false);
    registry.emplace<Comp::PlantTag>(excluded);
    registry.emplace<Comp::PlantHealthComponent>(excluded).infection = 100;
    registry.get<Comp::TransformComponent>(excluded).position.x = 512;
    const std::array due{RegionKey{0, 0}};
    const RegionConsumerGate gate(registry, due);
    EXPECT_EQ(F::RunPlantDiseaseOnTick<true>(registry, 1, 0, gate).considered, 2);
    for (const auto entity : registry.view<Comp::PlantHealthComponent>()) {
        const auto& health = registry.get<Comp::PlantHealthComponent>(entity);
        if (health.state == static_cast<std::uint8_t>(Comp::PlantDiseaseState::Healthy)) {
            EXPECT_EQ(health.infection, 100);
        }
    }
}
} // namespace
