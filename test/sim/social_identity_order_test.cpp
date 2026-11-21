#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ai/HerdAlarmSystem.h"
#include "ai/PredatorPackSystem.h"
#include "ai/TerritorySystem.h"
#include "components/SimulationIdentityComponent.h"

namespace {
namespace Comp = Luminumbra::Components;
namespace ai = luminumbra::ai;

// IDs deliberately span multiple radix bytes and exceed the replication alias width.
constexpr std::uint64_t kFirstId = 0x100000002ULL;
constexpr std::uint64_t kSecondId = 0x200000001ULL;

entt::entity
Creature(entt::registry& registry, std::uint64_t id, float x, float z, bool predator = false) {
    const auto entity = registry.create();
    registry.emplace<Comp::SimulationIdentityComponent>(
        entity, Comp::SimulationIdentityComponent{.id = id});
    registry.emplace<Comp::TransformComponent>(entity).position = Luminumbra::Vec3(x, 0.0f, z);
    registry.emplace<Comp::CreatureComponent>(entity).is_predator = predator;
    return entity;
}

void Churn(entt::registry& registry) {
    // Recycle a hole with a different handle generation and retain unrelated residents.
    const auto hole = registry.create();
    registry.destroy(hole);
    (void)registry.create();
    for (int index = 0; index < 64; ++index)
        registry.emplace<Comp::TransformComponent>(registry.create());
    // The next four participants reuse holes with changed generations while
    // preserving their creation order within that generation.
    const std::array holes{
        registry.create(), registry.create(), registry.create(), registry.create()};
    for (auto entry = holes.rbegin(); entry != holes.rend(); ++entry)
        registry.destroy(*entry);
}

struct BiasPublication {
    std::vector<std::uint64_t> ids;
    void Record(entt::registry& registry, entt::entity entity) {
        ids.push_back(registry.get<Comp::SimulationIdentityComponent>(entity).id);
    }
};

TEST(SocialIdentityOrder, RadixUsesEveryByteAndReusesScratchAcrossEmptyAndSmallerRosters) {
    entt::registry registry;
    ai::SocialIdentityOrder order;
    const std::array ids{std::uint64_t{1} << 62,
                         kSecondId,
                         std::uint64_t{256},
                         kFirstId,
                         std::uint64_t{1},
                         std::uint64_t{255},
                         std::uint64_t{1} << 56,
                         std::uint64_t{1} << 48,
                         std::uint64_t{1} << 40,
                         std::uint64_t{1} << 24,
                         std::uint64_t{1} << 16,
                         (std::uint64_t{1} << 63) - 1};
    std::vector<entt::entity> entities;
    for (const auto id : ids)
        entities.push_back(Creature(registry, id, 0.0f, 0.0f));
    order.Sort(registry, entities);
    EXPECT_EQ(order.participant_visits(), ids.size());
    const std::array expected{std::uint64_t{1},
                              std::uint64_t{255},
                              std::uint64_t{256},
                              std::uint64_t{1} << 16,
                              std::uint64_t{1} << 24,
                              kFirstId,
                              kSecondId,
                              std::uint64_t{1} << 40,
                              std::uint64_t{1} << 48,
                              std::uint64_t{1} << 56,
                              std::uint64_t{1} << 62,
                              (std::uint64_t{1} << 63) - 1};
    for (std::size_t index = 0; index < entities.size(); ++index)
        EXPECT_EQ(registry.get<Comp::SimulationIdentityComponent>(entities[index]).id,
                  expected[index]);
    entities.clear();
    order.Sort(registry, entities);
    EXPECT_EQ(order.participant_visits(), 0u);
    entities.push_back(Creature(registry, 42, 0.0f, 0.0f));
    order.Sort(registry, entities);
    EXPECT_EQ(order.participant_visits(), 1u);
}

TEST(SocialIdentityOrder, EqualDistancePreyUsesIdentityWithAndWithoutRegionalAdmission) {
    for (const bool regional : {false, true}) {
        for (const bool reverse : {false, true}) {
            entt::registry registry;
            if (reverse)
                Churn(registry);
            auto& order = registry.ctx().emplace<ai::SocialIdentityOrder>();
            const auto hunter = Creature(registry, 7, 0.0f, 0.0f, true);
            registry.emplace<Comp::PackHunterComponent>(hunter);
            const auto add_prey = [&](bool first) {
                Creature(registry, first ? kFirstId : kSecondId, first ? 10.0f : -10.0f, 0.0f);
            };
            add_prey(reverse);
            add_prey(!reverse);
            // Regional admission admits both sides of the origin. Null admission is
            // the regions-disabled path; neither path may select by position or handle.
            const std::array due{Luminumbra::world::RegionKey{-1, 0},
                                 Luminumbra::world::RegionKey{0, 0}};
            const Luminumbra::world::RegionConsumerGate gate(registry, due);
            ai::CreatureTickAdmission admission;
            admission.Capture(registry, gate);
            const auto stats =
                ai::RunPredatorPackOnTick(registry, 0, regional ? &admission : nullptr);
            EXPECT_EQ(stats.hunting, 1);
            const auto& pack = registry.get<Comp::PackHunterComponent>(hunter);
            EXPECT_FLOAT_EQ(pack.coord_x, 1.0f);
            EXPECT_FLOAT_EQ(pack.coord_z, 0.0f);
            EXPECT_EQ(order.participant_visits(), 3u);
        }
    }
}

TEST(SocialIdentityOrder, CoincidentPredatorsRankByIdentityInBothRegionModes) {
    for (const bool captured : {false, true}) {
        for (const bool reverse : {false, true}) {
            entt::registry registry;
            registry.ctx().emplace<ai::SocialIdentityOrder>();
            if (reverse)
                Churn(registry);
            std::array<entt::entity, 2> hunters{};
            for (const auto index : {reverse ? 1u : 0u, reverse ? 0u : 1u}) {
                hunters[index] =
                    Creature(registry, index == 0 ? kFirstId : kSecondId, 0.0f, 0.0f, true);
                registry.emplace<Comp::PackHunterComponent>(hunters[index]);
            }
            Creature(registry, 9, 1.0f, 0.0f);
            // A default gate explicitly exercises regions-off capture as well as
            // the null-admission path used by standalone callers.
            ai::CreatureTickAdmission admission;
            admission.Capture(registry, Luminumbra::world::RegionConsumerGate{});
            const auto stats =
                ai::RunPredatorPackOnTick(registry, 0, captured ? &admission : nullptr);
            EXPECT_EQ(stats.packed, 2);
            EXPECT_GT(registry.get<Comp::PackHunterComponent>(hunters[0]).coord_x, 0.99f);
            EXPECT_LT(registry.get<Comp::PackHunterComponent>(hunters[1]).coord_x, -0.99f);
        }
    }
}

TEST(SocialIdentityOrder, CentroidAccumulationIsIdenticalUnderHandlePermutation) {
    std::array<std::array<float, 2>, 3> reference{};
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        registry.ctx().emplace<ai::SocialIdentityOrder>();
        if (reverse)
            Churn(registry);
        // All hunters are within two units, but float addition at this magnitude
        // gives different centroids when the first and last summands are swapped.
        const std::array positions{8388608.0f, 8388609.0f, 8388610.0f};
        std::array<entt::entity, 3> hunters{};
        for (std::size_t ordinal = 0; ordinal < hunters.size(); ++ordinal) {
            const auto index = reverse ? hunters.size() - 1 - ordinal : ordinal;
            hunters[index] = Creature(registry, kFirstId + index, positions[index], 0.0f, true);
            registry.emplace<Comp::PackHunterComponent>(hunters[index]);
        }
        Creature(registry, 1, 8388608.0f, 10.0f);
        ai::RunPredatorPackOnTick(registry, 0);
        for (std::size_t index = 0; index < hunters.size(); ++index) {
            const auto& pack = registry.get<Comp::PackHunterComponent>(hunters[index]);
            if (!reverse)
                reference[index] = {pack.coord_x, pack.coord_z};
            else {
                EXPECT_EQ(pack.coord_x, reference[index][0]);
                EXPECT_EQ(pack.coord_z, reference[index][1]);
            }
        }
    }
}

TEST(SocialIdentityOrder, OverlappingClaimsPublishByIdentityAndKeepIndependentHomes) {
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        auto& order = registry.ctx().emplace<ai::SocialIdentityOrder>();
        if (reverse)
            Churn(registry);
        std::array<entt::entity, 2> entities{};
        for (const auto index : {reverse ? 1u : 0u, reverse ? 0u : 1u}) {
            entities[index] = Creature(registry, index == 0 ? kFirstId : kSecondId, 0.0f, 0.0f);
            registry.emplace<Comp::TerritoryComponent>(entities[index]).radius = 1.0f;
        }
        BiasPublication publication;
        const entt::scoped_connection connection =
            registry.on_construct<Comp::TerritoryBiasComponent>().connect<&BiasPublication::Record>(
                publication);
        auto stats = ai::RunTerritoryOnTick(registry);
        EXPECT_EQ(stats.claimed, 2);
        EXPECT_EQ(publication.ids, (std::vector<std::uint64_t>{kFirstId, kSecondId}));
        registry.get<Comp::TransformComponent>(entities[0]).position.x = 3.0f;
        registry.get<Comp::TransformComponent>(entities[1]).position.x = -3.0f;
        stats = ai::RunTerritoryOnTick(registry, 1);
        EXPECT_EQ(stats.claimed, 0);
        EXPECT_EQ(stats.homing, 2);
        EXPECT_FLOAT_EQ(registry.get<Comp::TerritoryBiasComponent>(entities[0]).wish_x, -0.5f);
        EXPECT_FLOAT_EQ(registry.get<Comp::TerritoryBiasComponent>(entities[1]).wish_x, 0.5f);
        EXPECT_EQ(order.participant_visits(), 2u);
    }
}

TEST(SocialIdentityOrder, AlarmUsesFrozenSourcesAcrossHandleChurnAndRoleAndDeathFilters) {
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        auto& order = registry.ctx().emplace<ai::SocialIdentityOrder>();
        if (reverse)
            Churn(registry);
        std::array<entt::entity, 4> entities{};
        for (std::size_t ordinal = 0; ordinal < entities.size(); ++ordinal) {
            const auto index = reverse ? entities.size() - 1 - ordinal : ordinal;
            entities[index] =
                Creature(registry, kFirstId + index, static_cast<float>(index), 0.0f, index == 2);
            registry.emplace<Comp::AlarmComponent>(entities[index]);
        }
        registry.get<Comp::AlarmComponent>(entities[0]).alarmed = 1;
        registry.get<Comp::CreatureComponent>(entities[3]).eaten = true;
        registry.get<Comp::AlarmComponent>(entities[3]).level = 1.0f;
        const auto stats = ai::RunHerdAlarmOnTick(registry, Luminumbra::SECONDS_PER_TICK);
        EXPECT_EQ(stats.sources, 1);
        EXPECT_FLOAT_EQ(registry.get<Comp::AlarmComponent>(entities[0]).level, 1.0f);
        EXPECT_GT(registry.get<Comp::AlarmComponent>(entities[1]).level, 0.8f);
        EXPECT_FLOAT_EQ(registry.get<Comp::AlarmComponent>(entities[2]).level, 0.0f);
        EXPECT_FLOAT_EQ(registry.get<Comp::AlarmComponent>(entities[3]).level, 0.0f);
        EXPECT_EQ(order.participant_visits(), 4u);
    }
}

// These systems must validate the entire admitted roster before writes, including
// a bad last record or duplicates across hunter/prey roles.
TEST(SocialIdentityOrder, InvalidOrDuplicateAdmissionLeavesEverySocialFieldUnchanged) {
    for (const unsigned system : {0u, 1u, 2u}) {
        for (const unsigned invalid : {0u, 1u, 2u, 3u, 4u}) {
            entt::registry registry;
            registry.ctx().emplace<ai::SocialIdentityOrder>();
            const auto first = Creature(registry, kFirstId, 0.0f, 0.0f, system == 1);
            const auto second = Creature(registry, kSecondId, 1.0f, 0.0f);
            for (const auto entity : {first, second}) {
                registry.emplace<Comp::AlarmComponent>(entity).level = 0.75f;
                registry.emplace<Comp::TerritoryComponent>(entity);
            }
            auto& pack = registry.emplace<Comp::PackHunterComponent>(first);
            pack.coord_x = 0.5f;
            pack.coord_z = -0.5f;
            pack.in_pack = 1;
            if (invalid == 0)
                registry.remove<Comp::SimulationIdentityComponent>(second);
            else
                registry.get<Comp::SimulationIdentityComponent>(second).id =
                    invalid == 1   ? 0
                    : invalid == 2 ? std::uint64_t{1} << 63
                    : invalid == 3 ? kFirstId
                                   : UINT64_MAX;
            if (system == 0)
                EXPECT_THROW(ai::RunHerdAlarmOnTick(registry, Luminumbra::SECONDS_PER_TICK),
                             std::logic_error);
            else if (system == 1)
                EXPECT_THROW(ai::RunPredatorPackOnTick(registry, 0), std::logic_error);
            else
                EXPECT_THROW(ai::RunTerritoryOnTick(registry), std::logic_error);
            for (const auto entity : {first, second}) {
                EXPECT_FLOAT_EQ(registry.get<Comp::AlarmComponent>(entity).level, 0.75f);
                EXPECT_EQ(registry.get<Comp::TerritoryComponent>(entity).established, 0u);
                EXPECT_FALSE(registry.all_of<Comp::TerritoryBiasComponent>(entity));
            }
            EXPECT_FLOAT_EQ(pack.coord_x, 0.5f);
            EXPECT_FLOAT_EQ(pack.coord_z, -0.5f);
            EXPECT_EQ(pack.in_pack, 1u);
        }
    }
}

TEST(SocialIdentityOrder, OnlyAdmittedParticipantsAreVisitedAndEmptyRostersAreNoOps) {
    entt::registry registry;
    auto& order = registry.ctx().emplace<ai::SocialIdentityOrder>();
    Churn(registry);
    const auto active = Creature(registry, 1, 0.0f, 0.0f, true);
    registry.emplace<Comp::AlarmComponent>(active).alarmed = 1;
    registry.emplace<Comp::TerritoryComponent>(active);
    registry.emplace<Comp::PackHunterComponent>(active);
    const auto frozen = Creature(registry, 0, 10000.0f, 0.0f);
    registry.emplace<Comp::AlarmComponent>(frozen).level = 0.75f;
    registry.emplace<Comp::TerritoryComponent>(frozen);
    registry.emplace<Comp::PackHunterComponent>(frozen);
    const std::array due{Luminumbra::world::RegionKey{0, 0}};
    ai::CreatureTickAdmission admission;
    admission.Capture(registry, Luminumbra::world::RegionConsumerGate(registry, due));
    EXPECT_EQ(
        ai::RunHerdAlarmOnTick(registry, Luminumbra::SECONDS_PER_TICK, &admission).participants, 1);
    EXPECT_EQ(order.participant_visits(), 1u);
    EXPECT_EQ(ai::RunPredatorPackOnTick(registry, 0, &admission).hunting, 0);
    EXPECT_EQ(order.participant_visits(), 1u);
    EXPECT_EQ(ai::RunTerritoryOnTick(registry, 0, &admission).claimed, 1);
    EXPECT_EQ(order.participant_visits(), 1u);
    EXPECT_FLOAT_EQ(registry.get<Comp::AlarmComponent>(frozen).level, 0.75f);
    EXPECT_EQ(registry.get<Comp::TerritoryComponent>(frozen).established, 0u);
    EXPECT_FALSE(registry.all_of<Comp::TerritoryBiasComponent>(frozen));
    admission.Capture(registry, Luminumbra::world::RegionConsumerGate(registry, {}));
    EXPECT_EQ(
        ai::RunHerdAlarmOnTick(registry, Luminumbra::SECONDS_PER_TICK, &admission).participants, 0);
    EXPECT_EQ(order.participant_visits(), 0u);
    EXPECT_EQ(ai::RunPredatorPackOnTick(registry, 0, &admission).participants, 0);
    EXPECT_EQ(order.participant_visits(), 0u);
    EXPECT_EQ(ai::RunTerritoryOnTick(registry, 0, &admission).participants, 0);
    EXPECT_EQ(order.participant_visits(), 0u);
    entt::registry empty;
    empty.ctx().emplace<ai::SocialIdentityOrder>();
    EXPECT_EQ(ai::RunHerdAlarmOnTick(empty, Luminumbra::SECONDS_PER_TICK).participants, 0);
    EXPECT_EQ(ai::RunPredatorPackOnTick(empty, 0).participants, 0);
    EXPECT_EQ(ai::RunTerritoryOnTick(empty).participants, 0);
}

TEST(SocialIdentityOrder, NoHunterDoesNotAdmitOrInspectPrey) {
    entt::registry registry;
    auto& order = registry.ctx().emplace<ai::SocialIdentityOrder>();
    Creature(registry, 0, 0.0f, 0.0f);
    EXPECT_EQ(ai::RunPredatorPackOnTick(registry, 0).participants, 0);
    EXPECT_EQ(order.participant_visits(), 0u);
}

TEST(SocialIdentityOrder, PreyWithHunterTagIsValidatedOnceAndRemainsATarget) {
    entt::registry registry;
    auto& order = registry.ctx().emplace<ai::SocialIdentityOrder>();
    const auto hunter = Creature(registry, 1, 0.0f, 0.0f, true);
    registry.emplace<Comp::PackHunterComponent>(hunter);
    const auto prey = Creature(registry, 2, 1.0f, 0.0f);
    registry.emplace<Comp::PackHunterComponent>(prey);
    const auto dead = Creature(registry, 0, 0.0f, 1.0f);
    registry.get<Comp::CreatureComponent>(dead).eaten = true;
    const auto stats = ai::RunPredatorPackOnTick(registry, 0);
    EXPECT_EQ(stats.participants, 2);
    EXPECT_EQ(stats.hunting, 1);
    EXPECT_EQ(order.participant_visits(), 2u);
    EXPECT_FLOAT_EQ(registry.get<Comp::PackHunterComponent>(hunter).coord_x, 1.0f);
    EXPECT_FLOAT_EQ(registry.get<Comp::PackHunterComponent>(prey).coord_x, 0.0f);
}
} // namespace
