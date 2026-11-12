#include <gtest/gtest.h>

#include "luminumbra_common/components/SimulationIdentityComponent.h"
#include "luminumbra_common/systems/FieldEmitterSystem.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
namespace C = Luminumbra::Components;
namespace S = Luminumbra::Systems;
using luminumbra::fields::EnergyFieldState;
using Order = S::FieldEmitterSourceOrder;

const EnergyFieldState::RegionRecords& Records() {
    static const EnergyFieldState::RegionRecords records{{{0, 0}, {}}};
    return records;
}

entt::entity AddSource(entt::registry& registry, std::uint64_t id, std::uint32_t rate = 100) {
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity);
    registry.emplace<C::FieldEmitterComponent>(entity, C::FieldEmitterComponent{0, rate, 0});
    registry.emplace<C::SimulationIdentityComponent>(entity, C::SimulationIdentityComponent{id});
    return entity;
}

S::FieldEmitterGatherStats
Gather(entt::registry& registry,
       EnergyFieldState& field,
       bool regional,
       const luminumbra::ai::CreatureTickAdmission* admission = nullptr) {
    if (!regional)
        return S::GatherFieldEmitterDeposits(
            registry, field, false, admission, Order::SimulationIdentity);
    const auto stats = S::GatherRegionalFieldEmitterDeposits(registry,
                                                             field,
                                                             Records(),
                                                             false,
                                                             EnergyFieldState::kMaxRegionalDeposits,
                                                             admission,
                                                             Order::SimulationIdentity);
    EXPECT_TRUE(stats.accepted);
    return stats.queued;
}

TEST(SourceIdentity, BothGathersOrderEveryIdentityByteAcrossHandleAndStoragePermutations) {
    // These IDs exercise byte carries, the old 32-bit handle width and the last valid ID.
    constexpr std::array<std::uint64_t, 4> ids{
        1, 257, (std::uint64_t{1} << 32) + 1, (std::uint64_t{1} << 63) - 1};
    for (const bool regional : {false, true}) {
        for (const bool admitted : {false, true}) {
            std::array<std::size_t, 4> permutation{0, 1, 2, 3};
            std::string reference;
            do {
                entt::registry registry;
                // Unrelated residents (including invalid identities) never enter the gather.
                for (int index = 0; index < 128; ++index)
                    registry.emplace<C::TransformComponent>(registry.create());
                registry.emplace<C::SimulationIdentityComponent>(registry.create());
                for (const auto index : permutation) {
                    const auto hole = registry.create();
                    registry.destroy(hole);
                    const auto entity =
                        AddSource(registry, ids[index], static_cast<std::uint32_t>(index + 1));
                    registry.get<C::FieldEmitterComponent>(entity).channel =
                        static_cast<int>(index % 2);
                }
                registry.sort<C::FieldEmitterComponent>([](const auto& a, const auto& b) {
                    return a.rate_raw_per_tick > b.rate_raw_per_tick;
                });
                EnergyFieldState field(2);
                luminumbra::ai::CreatureTickAdmission admission;
                const auto stats =
                    Gather(registry, field, regional, admitted ? &admission : nullptr);
                EXPECT_EQ(stats.emitters, ids.size());
                EXPECT_EQ(stats.deposits, ids.size());
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    const auto deposit = field.pending_deposit(index);
                    if (deposit.has_value()) {
                        EXPECT_EQ(deposit->emitter_id, ids[index]);
                        EXPECT_EQ(deposit->amount, index + 1);
                        EXPECT_EQ(deposit->channel, static_cast<int>(index % 2));
                    } else {
                        FAIL() << "Expected an ordered source deposit";
                    }
                }
                EXPECT_FALSE(field.pending_deposit(ids.size()).has_value());
                field.Tick(1);
                if (reference.empty())
                    reference = field.CanonicalBytes();
                EXPECT_EQ(field.CanonicalBytes(), reference);
            } while (std::next_permutation(permutation.begin(), permutation.end()));
        }
    }
}

TEST(SourceIdentity, MissingInvalidAndDuplicateIdentitiesRefuseBeforeAnyDeposit) {
    for (const bool regional : {false, true}) {
        for (const auto invalid : {std::uint64_t{0},
                                   std::uint64_t{5},
                                   std::uint64_t{1} << 63,
                                   (std::numeric_limits<std::uint64_t>::max)()}) {
            entt::registry registry;
            AddSource(registry, 5);
            const auto entity = AddSource(registry, invalid);
            for (const bool missing : {false, true}) {
                if (missing)
                    registry.remove<C::SimulationIdentityComponent>(entity);
                EnergyFieldState field;
                field.QueueDeposit(99, 0, 0, 0, 7);
                auto reference = field;
                EXPECT_THROW(Gather(registry, field, regional), std::logic_error);
                const auto pending = field.pending_deposit(0);
                if (pending.has_value()) {
                    EXPECT_EQ(pending->emitter_id, 99u);
                    EXPECT_EQ(pending->amount, 7u);
                } else {
                    FAIL() << "Refusal removed the prior deposit";
                }
                EXPECT_FALSE(field.pending_deposit(1).has_value());
                EXPECT_EQ(field.Tick(1), reference.Tick(1));
                EXPECT_EQ(field.CanonicalBytes(), reference.CanonicalBytes());
            }
        }
    }
}

TEST(SourceIdentity, RegionalGatherVisitsOnlySourcesAmongUnrelatedResidents) {
    entt::registry registry;
    for (int index = 0; index < 4096; ++index)
        registry.emplace<C::TransformComponent>(registry.create());
    AddSource(registry, 2);
    AddSource(registry, 1);
    EnergyFieldState field;
    const auto stats = S::GatherRegionalFieldEmitterDeposits(registry,
                                                             field,
                                                             Records(),
                                                             false,
                                                             EnergyFieldState::kMaxRegionalDeposits,
                                                             nullptr,
                                                             Order::SimulationIdentity);
    EXPECT_TRUE(stats.accepted);
    EXPECT_EQ(stats.visited_emitters, 2u);
    EXPECT_EQ(stats.queued.emitters, 2u);
    EXPECT_EQ(stats.queued.deposits, 2u);
}

TEST(SourceIdentity, EmptyInertAndUnadmittedSourcesRequireNoIdentityOrAllocator) {
    for (const bool regional : {false, true}) {
        entt::registry registry;
        EnergyFieldState field;
        EXPECT_EQ(Gather(registry, field, regional).deposits, 0u);
        const auto inert = AddSource(registry, 0, 0);
        registry.remove<C::SimulationIdentityComponent>(inert);
        EXPECT_EQ(Gather(registry, field, regional).deposits, 0u);
        const auto sleeping = AddSource(registry, 0);
        registry.emplace<C::CreatureComponent>(sleeping);
        const Luminumbra::world::RegionConsumerGate gate(registry, {});
        luminumbra::ai::CreatureTickAdmission admission;
        admission.Capture(registry, gate);
        EXPECT_EQ(Gather(registry, field, regional, &admission).deposits, 0u);
        EXPECT_THROW(Gather(registry, field, regional), std::logic_error);
        registry.emplace<C::SimulationIdentityComponent>(inert, C::SimulationIdentityComponent{8});
        registry.get<C::FieldEmitterComponent>(inert).rate_raw_per_tick = 9;
        EXPECT_EQ(Gather(registry, field, regional, &admission).deposits, 1u);
        EXPECT_FALSE(field.pending_deposit(1).has_value());
    }
}

TEST(SourceIdentity, LegacyDefaultsIgnoreSimulationIdentityAndKeepNeutralAdmissionKeys) {
    for (const bool regional : {false, true}) {
        for (const bool admitted : {false, true}) {
            entt::registry registry;
            const auto first = AddSource(registry, 0);
            const auto second = AddSource(registry, 0);
            registry.sort<C::FieldEmitterComponent>([](const auto&, const auto&) { return false; });
            luminumbra::ai::CreatureTickAdmission admission;
            EnergyFieldState field;
            if (regional) {
                EXPECT_TRUE(
                    S::GatherRegionalFieldEmitterDeposits(registry,
                                                          field,
                                                          Records(),
                                                          false,
                                                          EnergyFieldState::kMaxRegionalDeposits,
                                                          admitted ? &admission : nullptr)
                        .accepted);
            } else {
                EXPECT_EQ(S::GatherFieldEmitterDeposits(
                              registry, field, false, admitted ? &admission : nullptr)
                              .deposits,
                          2u);
            }
            const std::array entities{first, second};
            for (std::size_t index = 0; index < entities.size(); ++index) {
                const auto deposit = field.pending_deposit(index);
                if (deposit.has_value()) {
                    EXPECT_EQ(deposit->emitter_id,
                              admitted
                                  ? 0u
                                  : static_cast<std::uint64_t>(entt::to_integral(entities[index])));
                } else {
                    FAIL() << "Expected a legacy source deposit";
                }
            }
        }
    }
}

TEST(SourceIdentity, RegionalRefusalPreservesPriorInputForLateInvalidFootprintsAndCapacity) {
    for (const bool invalid_footprint : {false, true}) {
        entt::registry registry;
        AddSource(registry, 1);
        const auto last = AddSource(registry, 2);
        if (invalid_footprint)
            registry.get<C::FieldEmitterComponent>(last).radius_cells = 32;
        EnergyFieldState field;
        ASSERT_TRUE(field.QueueRegionalDeposit(99, 0, 0, 0, 7));
        auto reference = field;
        const auto stats = S::GatherRegionalFieldEmitterDeposits(
            registry, field, Records(), false, 1, nullptr, Order::SimulationIdentity);
        EXPECT_FALSE(stats.accepted);
        EXPECT_EQ(stats.queued.emitters, 0u);
        EXPECT_EQ(stats.queued.deposits, 0u);
        EXPECT_EQ(stats.visited_emitters, 2u);
        EXPECT_FALSE(field.pending_deposit(1).has_value());
        EXPECT_EQ(field.Tick(1), reference.Tick(1));
        EXPECT_EQ(field.CanonicalBytes(), reference.CanonicalBytes());
        registry.get<C::FieldEmitterComponent>(last).radius_cells = 0;
        EXPECT_TRUE(S::GatherRegionalFieldEmitterDeposits(
                        registry, field, Records(), false, 2, nullptr, Order::SimulationIdentity)
                        .accepted);
        const auto recovered = field.pending_deposit(1);
        if (recovered.has_value()) {
            EXPECT_EQ(recovered->emitter_id, 2u);
        } else {
            FAIL() << "Retry did not queue the second source";
        }
    }
}

} // namespace
