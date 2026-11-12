#include <gtest/gtest.h>

#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/DecompositionSystem.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/ScentDepositSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace AI = luminumbra::ai;
namespace Comp = Luminumbra::Components;
using Luminumbra::world::RegionConsumerGate;
using Luminumbra::world::RegionKey;
using Entities = std::array<entt::entity, 3>;
constexpr std::array<std::uint64_t, 3> kIdentities{
    0xffffffffull, 0x0102030405060708ull, (std::uint64_t{1} << 63) - 1};

Entities Populate(entt::registry& registry, const std::array<std::size_t, 3>& permutation) {
    // Recycled generations, holes and unrelated residents must not become identity inputs.
    std::array<entt::entity, 7> holes{};
    for (auto& hole : holes)
        hole = registry.create();
    for (const auto hole : holes)
        registry.destroy(hole);
    Entities entities{};
    for (const auto index : permutation) {
        (void)registry.create();
        auto entity = registry.create();
        for (std::size_t generation = 0; generation < permutation[0]; ++generation) {
            registry.destroy(entity);
            entity = registry.create();
        }
        entities[index] = entity;
        registry.emplace<Comp::SimulationIdentityComponent>(entity, kIdentities[index]);
        registry.emplace<Comp::TransformComponent>(entity).position = {1.0f, 0.0f, 1.0f};
        auto& sensable = registry.emplace<Comp::SensableComponent>(entity);
        sensable.scent_channel = 0;
        sensable.scent_deposit = index == 2 ? 0x1p53f : 1.0f;
        auto& sense = registry.emplace<Comp::ScentSenseComponent>(entity);
        sense.channel = index == 2 ? -1 : 0;
        sense.sign = index == 1 ? -1.0f : 1.0f;
        sense.strength = 1.0f;
        sense.floor = 0.0001f;
        sense.weber_k = 1.0f;
        registry.emplace<Comp::LocomotionIntentComponent>(entity).wish_xz = {0.25f, -0.5f};
        registry.emplace<Comp::LocomotionProfile>(entity).move_speed = 2.0f;
        registry.emplace<Comp::MigratoryComponent>(entity);
        registry.emplace<Comp::CircadianComponent>(entity).nocturnal =
            static_cast<std::uint8_t>(index % 2);
        auto& mortal = registry.emplace<Comp::MortalComponent>(entity);
        mortal.lifespan_ticks = 1;
        auto& creature = registry.emplace<Comp::CreatureComponent>(entity);
        creature.hunger = index == 1 ? 1.0f : 0.0f;
        registry.emplace<Comp::DecayComponent>(entity).decay_duration =
            static_cast<std::uint32_t>(index);
    }
    return entities;
}

AI::ScentField GradientField() {
    AI::ScentField field(4, 4);
    field.RestoreCell(0, 2, 1, 10.0);
    return field;
}

using Snapshot = std::array<std::array<std::uint32_t, 15>, 3>;
Snapshot State(const entt::registry& registry, const Entities& entities) {
    Snapshot state{};
    for (std::size_t index = 0; index < entities.size(); ++index) {
        const auto entity = entities[index];
        const auto& wish = registry.get<Comp::LocomotionIntentComponent>(entity).wish_xz;
        const auto& migration = registry.get<Comp::MigratoryComponent>(entity);
        const auto& circadian = registry.get<Comp::CircadianComponent>(entity);
        const auto& mortal = registry.get<Comp::MortalComponent>(entity);
        const auto& decay = registry.get<Comp::DecayComponent>(entity);
        state[index] = {
            std::bit_cast<std::uint32_t>(wish.x),
            std::bit_cast<std::uint32_t>(wish.y),
            std::bit_cast<std::uint32_t>(migration.drive),
            std::bit_cast<std::uint32_t>(migration.wish_x),
            std::bit_cast<std::uint32_t>(migration.wish_z),
            std::bit_cast<std::uint32_t>(circadian.activity),
            circadian.nocturnal,
            mortal.age_ticks,
            mortal.lifespan_ticks,
            mortal.dead,
            static_cast<std::uint32_t>(registry.get<Comp::CreatureComponent>(entity).eaten),
            decay.decay_ticks,
            decay.decay_duration,
            decay.nutrient_released_milli,
            decay.fully_decomposed};
    }
    return state;
}

void RunConsumer(int consumer, entt::registry& registry, AI::ScentField& field) {
    switch (consumer) {
        case 0:
            AI::RunScentDepositOnTick(registry, field, 0, 0, 1);
            break;
        case 1:
            AI::RunScentSteeringOnTick(registry, field, 0, 0, 1);
            break;
        case 2:
            AI::RunMigrationOnTick(registry, 0.25f);
            break;
        case 3:
            AI::RunCircadianOnTick(registry, 0.25f);
            break;
        case 4:
            AI::RunLifespanOnTick(registry);
            break;
        case 5:
            AI::RunDecompositionOnTick(registry, 1);
            break;
        default:
            FAIL() << "Unknown traversal consumer";
    }
}

TEST(TraversalIdentity, FloatingDepositsIgnoreHandlePermutationWithRegionsOff) {
    std::array<std::size_t, 3> permutation{0, 1, 2};
    bool saw_different_handle_order = false;
    do {
        entt::registry registry;
        const auto entities = Populate(registry, permutation);
        auto handle_order = entities;
        std::sort(handle_order.begin(), handle_order.end(), [](entt::entity lhs, entt::entity rhs) {
            return entt::to_integral(lhs) < entt::to_integral(rhs);
        });
        saw_different_handle_order = saw_different_handle_order || handle_order != entities;
        registry.ctx().emplace<AI::SimulationTraversalContext>();
        AI::ScentField field(4, 4);
        const auto stats = AI::RunScentDepositOnTick(registry, field, 0, 0, 1);
        EXPECT_EQ(stats.emitters, 3u);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(field.Sample(0, 1, 1)),
                  std::bit_cast<std::uint64_t>(0x1p53 + 2.0));
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    EXPECT_TRUE(saw_different_handle_order);
}

TEST(TraversalIdentity, IndependentWritesAreBitIdenticalAcrossPermutationsAndLegacy) {
    for (const float phase : {0.0f, 0.125f, 0.25f, -0.25f, 1.25f}) {
        std::array<std::size_t, 3> permutation{0, 1, 2};
        Snapshot expected{};
        bool first = true;
        do {
            for (const bool candidate : {false, true}) {
                entt::registry registry;
                const auto entities = Populate(registry, permutation);
                if (candidate)
                    registry.ctx().emplace<AI::SimulationTraversalContext>();
                // Exercise both zero distance and nonzero migration direction.
                const auto target = AI::MigrationTargetAt(phase);
                registry.get<Comp::TransformComponent>(entities[2]).position = {
                    target.x, 0, target.z};
                const auto field = GradientField();
                const auto steering = AI::RunScentSteeringOnTick(registry, field, 0, 0, 1);
                EXPECT_EQ(steering.considered, 3u);
                EXPECT_EQ(steering.steered, 2u);
                const auto migration = AI::RunMigrationOnTick(registry, phase);
                const auto circadian = AI::RunCircadianOnTick(registry, phase, 1.25f);
                EXPECT_EQ(migration.participants, 3);
                EXPECT_EQ(circadian.participants, 3);
                EXPECT_EQ(circadian.diurnal, 2);
                EXPECT_EQ(circadian.nocturnal, 1);
                const auto state = State(registry, entities);
                if (first) {
                    expected = state;
                    first = false;
                }
                EXPECT_EQ(state, expected);
            }
        } while (std::next_permutation(permutation.begin(), permutation.end()));
    }
}

TEST(TraversalIdentity, DeathAndDecayPublicationsUseFullWidthIdentities) {
    std::array<std::size_t, 3> permutation{0, 1, 2};
    do {
        entt::registry registry;
        const auto entities = Populate(registry, permutation);
        auto& context = registry.ctx().emplace<AI::SimulationTraversalContext>();
        // Old age, starvation and saturating old age all publish in the same identity order.
        registry.get<Comp::MortalComponent>(entities[1]).lifespan_ticks = 100;
        auto& oldest = registry.get<Comp::MortalComponent>(entities[2]);
        oldest.age_ticks = std::numeric_limits<std::uint32_t>::max();
        oldest.lifespan_ticks = std::numeric_limits<std::uint32_t>::max();
        EXPECT_EQ(AI::RunLifespanOnTick(registry).died, 3);
        EXPECT_EQ(std::vector(context.deaths().begin(), context.deaths().end()),
                  std::vector(kIdentities.begin(), kIdentities.end()));
        for (const auto entity : entities)
            EXPECT_TRUE(registry.get<Comp::CreatureComponent>(entity).eaten);
        EXPECT_EQ(oldest.age_ticks, std::numeric_limits<std::uint32_t>::max());
        const auto first_decay = AI::RunDecompositionOnTick(registry, 1);
        EXPECT_EQ(first_decay.decaying, 3);
        EXPECT_EQ(first_decay.finished, 2);
        EXPECT_EQ(first_decay.released_total, 2500u);
        ASSERT_EQ(context.decay().size(), 3u);
        for (std::size_t index = 0; index < kIdentities.size(); ++index) {
            EXPECT_EQ(context.decay()[index].id, kIdentities[index]);
            EXPECT_EQ(context.decay()[index].released_milli, index == 2 ? 500u : 1000u);
            EXPECT_EQ(context.decay()[index].finished, index != 2);
        }
        EXPECT_EQ(AI::RunLifespanOnTick(registry).died, 0);
        EXPECT_TRUE(context.deaths().empty());
        EXPECT_EQ(AI::RunDecompositionOnTick(registry, 2).finished, 1);
        ASSERT_EQ(context.decay().size(), 1u);
        EXPECT_EQ(context.decay()[0].id, kIdentities[2]);
        EXPECT_EQ(AI::RunDecompositionOnTick(registry, 3).decaying, 0);
        EXPECT_TRUE(context.decay().empty());
    } while (std::next_permutation(permutation.begin(), permutation.end()));
}

TEST(TraversalIdentity, PublicationOrderUsesEveryIdentityByte) {
    std::vector<std::uint64_t> identities{1, (std::uint64_t{1} << 63) - 1};
    for (unsigned shift = 8; shift < 64; shift += 8) {
        identities.push_back(std::uint64_t{1} << shift);
        identities.push_back((std::uint64_t{1} << shift) | 1u);
    }
    std::sort(identities.begin(), identities.end());
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        auto& context = registry.ctx().emplace<AI::SimulationTraversalContext>();
        for (std::size_t index = 0; index < identities.size(); ++index) {
            const auto entity = registry.create();
            const auto ordinal = reverse ? identities.size() - 1 - index : index;
            registry.emplace<Comp::SimulationIdentityComponent>(entity, identities[ordinal]);
            registry.emplace<Comp::MortalComponent>(entity).lifespan_ticks = 1;
        }
        EXPECT_EQ(AI::RunLifespanOnTick(registry).died, static_cast<int>(identities.size()));
        EXPECT_EQ(std::vector(context.deaths().begin(), context.deaths().end()), identities);
    }
}

TEST(TraversalIdentity, EveryConsumerRefusesInvalidIdentityBeforeStateOrPublication) {
    for (int consumer = 0; consumer < 6; ++consumer) {
        // Missing, zero, exhausted cursor, out-of-range, and duplicate identities.
        for (int invalid = 0; invalid < 5; ++invalid) {
            SCOPED_TRACE(::testing::Message() << consumer << ':' << invalid);
            entt::registry registry;
            const auto entities = Populate(registry, {2, 0, 1});
            auto& context = registry.ctx().emplace<AI::SimulationTraversalContext>();
            AI::RunLifespanOnTick(registry);
            AI::RunDecompositionOnTick(registry, 1);
            // Keep earlier publications and make new writes observable if validation is late.
            for (const auto entity : entities) {
                registry.get<Comp::MortalComponent>(entity).dead = 0;
                registry.get<Comp::DecayComponent>(entity) = Comp::DecayComponent{};
            }
            auto& identity = registry.get<Comp::SimulationIdentityComponent>(entities[1]);
            switch (invalid) {
                case 0:
                    registry.remove<Comp::SimulationIdentityComponent>(entities[1]);
                    break;
                case 1:
                    identity.id = 0;
                    break;
                case 2:
                    identity.id = std::uint64_t{1} << 63;
                    break;
                case 3:
                    identity.id = std::numeric_limits<std::uint64_t>::max();
                    break;
                case 4:
                    identity.id = kIdentities[0];
                    break;
                default:
                    FAIL() << "Unknown invalid identity";
            }
            const auto before = State(registry, entities);
            const auto* invalid_identity =
                registry.try_get<Comp::SimulationIdentityComponent>(entities[1]);
            const auto previous_id = invalid_identity ? invalid_identity->id : 0;
            const std::vector deaths(context.deaths().begin(), context.deaths().end());
            const std::vector decay(context.decay().begin(), context.decay().end());
            auto field = GradientField();
            EXPECT_THROW(RunConsumer(consumer, registry, field), std::logic_error);
            EXPECT_EQ(State(registry, entities), before);
            if (invalid == 0) {
                EXPECT_FALSE(registry.all_of<Comp::SimulationIdentityComponent>(entities[1]));
            } else {
                ASSERT_TRUE(registry.all_of<Comp::SimulationIdentityComponent>(entities[1]));
                EXPECT_EQ(registry.get<Comp::SimulationIdentityComponent>(entities[1]).id,
                          previous_id);
            }
            EXPECT_EQ(std::vector(context.deaths().begin(), context.deaths().end()), deaths);
            ASSERT_EQ(context.decay().size(), decay.size());
            for (std::size_t index = 0; index < decay.size(); ++index) {
                EXPECT_EQ(context.decay()[index].id, decay[index].id);
                EXPECT_EQ(context.decay()[index].released_milli, decay[index].released_milli);
                EXPECT_EQ(context.decay()[index].finished, decay[index].finished);
            }
            EXPECT_EQ(field.Sample(0, 1, 1), 0.0);
            EXPECT_EQ(field.Sample(0, 2, 1), 10.0);
        }
    }
}

TEST(TraversalIdentity, AdmissionAndRegionsOffKeepTheirExistingSemantics) {
    for (const bool regions : {false, true}) {
        entt::registry registry;
        const auto entities = Populate(registry, {2, 1, 0});
        registry.get<Comp::TransformComponent>(entities[2]).position = {1000, 0, 1000};
        registry.ctx().emplace<AI::SimulationTraversalContext>();
        const std::array due{RegionKey{0, 0}};
        const auto gate = regions ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
        AI::CreatureTickAdmission admission;
        admission.Capture(registry, gate);
        const auto* admitted = regions ? &admission : nullptr;
        const auto expected = regions ? 2 : 3;
        auto field = GradientField();
        EXPECT_EQ(AI::RunScentDepositOnTick(registry, field, 0, 0, 1, admitted).emitters,
                  static_cast<std::uint64_t>(expected));
        EXPECT_EQ(AI::RunScentSteeringOnTick(registry, field, 0, 0, 1, admitted).considered,
                  static_cast<std::uint64_t>(expected));
        EXPECT_EQ(AI::RunMigrationOnTick(registry, 0.25f, admitted).participants, expected);
        EXPECT_EQ(AI::RunCircadianOnTick(registry, 0.25f, 1.0f, admitted).participants, expected);
        EXPECT_EQ(AI::RunLifespanOnTick(registry, 1, gate, admitted).died, expected);
        EXPECT_EQ(AI::RunDecompositionOnTick(registry, 1, admitted, gate).decaying, expected);
        EXPECT_EQ(registry.get<Comp::MortalComponent>(entities[2]).dead, regions ? 0 : 1);
    }
}

TEST(TraversalIdentity, EmptyAndLiveRostersAndParticipantCostStayBounded) {
    entt::registry registry;
    auto& context = registry.ctx().emplace<AI::SimulationTraversalContext>();
    AI::ScentField field(4, 4);
    for (int consumer = 0; consumer < 6; ++consumer) {
        RunConsumer(consumer, registry, field);
        EXPECT_EQ(context.participant_visits(), 0u);
    }
    const auto entities = Populate(registry, {0, 1, 2});
    for (int resident = 0; resident < 4096; ++resident)
        (void)registry.create();
    EXPECT_EQ(AI::RunDecompositionOnTick(registry, 1).decaying, 0);
    EXPECT_TRUE(context.decay().empty());
    for (int consumer = 0; consumer < 6; ++consumer) {
        RunConsumer(consumer, registry, field);
        EXPECT_EQ(context.participant_visits(), entities.size());
    }
    for (const auto entity : entities)
        registry.destroy(entity);
    for (int consumer = 0; consumer < 6; ++consumer) {
        RunConsumer(consumer, registry, field);
        EXPECT_EQ(context.participant_visits(), 0u);
    }
    EXPECT_TRUE(context.deaths().empty());
    EXPECT_TRUE(context.decay().empty());

    // A singleton non-creature still participates in mortality and decay.
    const auto singleton = registry.create();
    registry.emplace<Comp::SimulationIdentityComponent>(singleton, 1u);
    registry.emplace<Comp::MortalComponent>(singleton).lifespan_ticks = 1;
    registry.emplace<Comp::DecayComponent>(singleton).decay_duration = 0;
    EXPECT_EQ(AI::RunLifespanOnTick(registry).died, 1);
    EXPECT_EQ(context.participant_visits(), 1u);
    ASSERT_EQ(context.deaths().size(), 1u);
    EXPECT_EQ(context.deaths()[0], 1u);
    EXPECT_EQ(AI::RunDecompositionOnTick(registry, 1).finished, 1);
    EXPECT_EQ(context.participant_visits(), 1u);
    ASSERT_EQ(context.decay().size(), 1u);
    EXPECT_EQ(context.decay()[0].released_milli, 1000u);
}
} // namespace
