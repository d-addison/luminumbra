#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/world/GameSession.h"
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>

namespace {
using namespace Luminumbra::world;
namespace C = Luminumbra::Components;
namespace A = luminumbra::ai;
using Luminumbra::Vec3;
constexpr RegionKey kOwner{0, 0};
const Vec3 kPosition{128.0f, 3.0f, 128.0f};
RegionPopulator Populator(std::function<void(const RegionPopulateRequest&)> callback) {
    return {"fixture:v1", "fixed", {{7u, 9u}}, std::move(callback)};
}
void Factory(entt::registry& registry,
             const CreatureSpawnCommand& command,
             std::uint64_t id,
             RegionKey key,
             std::uint64_t tick,
             std::uint64_t draw) {
    if (command.species_id != 17)
        throw std::invalid_argument("Unknown fixture species");
    const auto e = registry.create();
    registry.emplace<C::TransformComponent>(e).position = command.position;
    registry.emplace<C::CreatureComponent>(e).species_id = command.species_id;
    registry.emplace<C::CreatureRegionComponent>(
        e, C::CreatureRegionComponent{id, key.x, key.z, tick});
    auto& coarse = registry.emplace<C::CreatureCoarseComponent>(e);
    coarse.x_mm = std::llround(static_cast<double>(command.position.x) * 1000.0);
    coarse.y_mm = std::llround(static_cast<double>(command.position.y) * 1000.0);
    coarse.z_mm = std::llround(static_cast<double>(command.position.z) * 1000.0);
    registry.emplace<C::CreatureControllerComponent>(e, C::CreatureControllerComponent{false});
    registry.emplace<C::CreatureGenomeComponent>(e).female = (draw & 1u) != 0;
}
struct Authority {
    ActiveRegionLedger ledger;
    GroundObjectStore ground;
    entt::registry registry;
    luminumbra::simulation::OrderedEventBus events;
    std::vector<RegionWork> work;
    Authority() {
        ledger.pin(kOwner, true, WorldClock{});
        ledger.schedule(WorldClock(1), std::span<const Vec3>{});
        ledger.acknowledge_activation(1, kOwner);
    }
    void execute(const RegionPopulator& populator, PopulationPreparationHooks hooks = {}) {
        RegionPopulationTransaction transaction(ledger, ground, registry, events, work);
        transaction.execute(populator, kOwner, 1, 19, {}, Factory, hooks);
    }
    void unchanged(const std::string& before) {
        EXPECT_EQ(ledger.encode(), before);
        EXPECT_TRUE(ground.empty());
        EXPECT_TRUE(registry.view<C::CreatureComponent>().empty());
        EXPECT_EQ(events.pending_count(), 0u);
        EXPECT_TRUE(work.empty());
    }
};
void Mixed(const RegionPopulateRequest& request) {
    request.sink.spawn(GroundObjectSpawnCommand{7u, kPosition, 0u, 2u});
    request.sink.spawn(CreatureSpawnCommand{std::uint16_t{17}, kPosition, false});
    request.sink.spawn(GroundObjectSpawnCommand{7u, kPosition, 0u, 3u});
}
TEST(RegionPopulator, CommandsAreStagedInCallOrder) {
    RegionSpawnSink sink;
    Mixed({kOwner, 1, RegionRng(19, kOwner, RegionRngTag::Population, 1), {}, sink});
    ASSERT_EQ(sink.commands().size(), 3u);
    EXPECT_EQ(std::get<GroundObjectSpawnCommand>(sink.commands()[0]).payload, 2u);
    EXPECT_EQ(std::get<CreatureSpawnCommand>(sink.commands()[1]).species_id, 17u);
    EXPECT_EQ(std::get<GroundObjectSpawnCommand>(sink.commands()[2]).payload, 3u);
}
TEST(RegionPopulator, ThrowAfterSeveralCommandsChangesNoAuthority) {
    Authority authority;
    const auto before = authority.ledger.encode();
    auto populator = Populator([](const auto& request) {
        Mixed(request);
        throw std::runtime_error("Population callback failure");
    });
    EXPECT_THROW(authority.execute(populator), std::runtime_error);
    authority.unchanged(before);
}
TEST(RegionPopulator, ValidationFailureRollsBackAllocatorEntitiesObjectsAndEvents) {
    for (unsigned failure = 0; failure < 4; ++failure) {
        Authority authority;
        const auto before = authority.ledger.encode();
        auto populator = Populator([failure](const auto& request) {
            Mixed(request);
            if (failure == 0)
                request.sink.spawn(CreatureSpawnCommand{std::uint16_t{99}, kPosition, false});
            else if (failure == 1)
                request.sink.spawn(GroundObjectSpawnCommand{99u, kPosition, 0u, 0u});
            else if (failure == 2)
                request.sink.spawn(GroundObjectSpawnCommand{7u, kPosition, 0u, 10u});
            else
                request.sink.spawn(GroundObjectSpawnCommand{
                    7u, {128.0f, std::numeric_limits<float>::infinity(), 128.0f}, 0u, 0u});
        });
        EXPECT_THROW(authority.execute(populator), std::invalid_argument);
        authority.unchanged(before);
    }
}
TEST(RegionPopulator, CapacityFailureRollsBackTheWholeRegion) {
    Authority authority;
    const auto before = authority.ledger.encode();
    const auto populator = Populator([](const auto& request) {
        request.sink.spawn(CreatureSpawnCommand{std::uint16_t{17}, kPosition, false});
        for (std::size_t i = 0; i <= GroundObjectStore::kMaxObjects; ++i)
            request.sink.spawn(GroundObjectSpawnCommand{7u, kPosition, 0u, 0u});
    });
    EXPECT_THROW(authority.execute(populator), std::length_error);
    authority.unchanged(before);
}
struct Signals {
    unsigned constructed = 0;
    void construct(entt::registry&, entt::entity) {
        ++constructed;
    }
};
TEST(RegionPopulator, EntityPreparationFailurePreservesAllocatorGenerations) {
    Authority authority;
    entt::registry reference;
    const auto old = authority.registry.create();
    const auto expected_old = reference.create();
    ASSERT_EQ(old, expected_old);
    authority.registry.destroy(old);
    reference.destroy(expected_old);
    const auto attachment = authority.registry.create();
    const auto expected_attachment = reference.create();
    ASSERT_EQ(attachment, expected_attachment);
    authority.registry.emplace<C::TagComponent>(attachment).tag = "retained attachment";
    const auto recycled = authority.registry.create();
    const auto expected_recycled = reference.create();
    authority.registry.destroy(recycled);
    reference.destroy(expected_recycled);
    Signals signals;
    authority.registry.on_construct<C::CreatureComponent>().connect<&Signals::construct>(signals);
    const auto before = authority.ledger.encode();
    EXPECT_THROW(
        authority.execute(Populator(Mixed),
                          {[] { throw std::runtime_error("Entity preparation failure"); }, {}}),
        std::runtime_error);
    authority.unchanged(before);
    EXPECT_EQ(signals.constructed, 0u);
    EXPECT_EQ(authority.registry.get<C::TagComponent>(attachment).tag, "retained attachment");
    EXPECT_EQ(authority.registry.create(), reference.create());
    EXPECT_EQ(authority.registry.create(), reference.create());
    authority.execute(Populator(Mixed));
    EXPECT_EQ(authority.registry.get<C::TagComponent>(attachment).tag, "retained attachment");
    EXPECT_EQ(signals.constructed, 0u);
    const auto later = authority.registry.create();
    authority.registry.emplace<C::CreatureComponent>(later);
    EXPECT_EQ(signals.constructed, 1u);
}
TEST(RegionPopulator, NonOwningGroupRefusesPublicationWithoutChangingMembership) {
    Authority authority;
    std::uint64_t id = 0;
    ASSERT_TRUE(authority.ledger.allocate_persistent_id(id));
    Factory(authority.registry,
            CreatureSpawnCommand{std::uint16_t{17}, kPosition, false},
            id,
            kOwner,
            1,
            37);
    const auto existing = *authority.registry.view<C::CreatureComponent>().begin();
    const auto group =
        authority.registry.group<>(entt::get<C::CreatureComponent, C::TransformComponent>);
    ASSERT_EQ(group.size(), 1u);
    ASSERT_TRUE(group.contains(existing));
    const auto before = authority.ledger.encode();
    const auto creatures =
        A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(authority.registry));
    const auto next_entity = authority.registry.storage<entt::entity>().size();
    EXPECT_THROW(authority.execute(Populator(Mixed)), std::logic_error);
    EXPECT_EQ(authority.ledger.encode(), before);
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(authority.registry)),
              creatures);
    EXPECT_EQ(authority.registry.storage<entt::entity>().size(), next_entity);
    EXPECT_TRUE(authority.ground.empty());
    EXPECT_TRUE(authority.work.empty());
    EXPECT_EQ(authority.events.pending_count(), 0u);
    ASSERT_EQ(group.size(), 1u);
    EXPECT_TRUE(group.contains(existing));
    // Refusal leaves EnTT's listeners connected for ordinary registry operations.
    const auto later = authority.registry.create();
    authority.registry.emplace<C::CreatureComponent>(later);
    authority.registry.emplace<C::TransformComponent>(later);
    EXPECT_TRUE(group.contains(later));
    authority.registry.remove<C::TransformComponent>(existing);
    EXPECT_FALSE(group.contains(existing));
}
TEST(RegionPopulator, EmptyNonOwningGroupRefusesBeforeAnyEntityOrSequenceIsPublished) {
    Authority authority;
    const auto group =
        authority.registry.group<>(entt::get<C::CreatureComponent, C::TransformComponent>);
    const auto before = authority.ledger.encode();
    EXPECT_THROW(authority.execute(Populator(Mixed)), std::logic_error);
    authority.unchanged(before);
    EXPECT_TRUE(group.empty());
    entt::registry reference;
    EXPECT_EQ(authority.registry.create(), reference.create());
    EXPECT_EQ(authority.events.publish(1, "sequence.probe", ""), 0u);
}
TEST(RegionPopulator, EventPreparationFailureConsumesNoSequences) {
    Authority authority;
    const auto before = authority.ledger.encode();
    EXPECT_THROW(authority.execute(Populator(Mixed),
                                   {{},
                                    [] {
                                        throw std::runtime_error("Event preparation failure");
                                    }}),
                 std::runtime_error);
    authority.unchanged(before);
    EXPECT_EQ(authority.events.publish(1, "probe", ""), 0u);
    authority.execute(Populator(Mixed));
    const auto events = authority.events.drain(1);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[1].sequence, 1u);
    EXPECT_EQ(events[2].sequence, 2u);
}
TEST(RegionPopulator, MixedCommandsAllocateIdsInSinkOrder) {
    Authority authority;
    authority.execute(Populator(Mixed));
    EXPECT_EQ(authority.ledger.next_persistent_id(), 4u);
    ASSERT_NE(authority.ground.find(1), nullptr);
    ASSERT_NE(authority.ground.find(3), nullptr);
    EXPECT_EQ(authority.ground.find(1)->payload, 2u);
    EXPECT_EQ(authority.ground.find(3)->payload, 3u);
    const auto snapshot = A::BuildCreatureSnapshot(authority.registry);
    ASSERT_EQ(snapshot.entities.size(), 1u);
    EXPECT_EQ(snapshot.entities[0].id, 2u);
    EXPECT_TRUE(authority.ledger.records().at(kOwner).populated);
    ASSERT_EQ(authority.work.size(), 1u);
    EXPECT_EQ(authority.work[0].entity_updates, 3u);
}
TEST(RegionPopulator, EmptySuccessSetsPopulatedWithoutIdsOrWork) {
    Authority authority;
    authority.execute(Populator([](const auto&) {}));
    EXPECT_TRUE(authority.ledger.records().at(kOwner).populated);
    EXPECT_EQ(authority.ledger.next_persistent_id(), 1u);
    EXPECT_TRUE(authority.work.empty());
    EXPECT_TRUE(authority.ground.empty());
    EXPECT_EQ(authority.events.pending_count(), 0u);
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(authority.ledger.encode(), restored, error)) << error;
    EXPECT_TRUE(restored.records().at(kOwner).populated);
    EXPECT_EQ(restored.next_persistent_id(), 1u);
}
TEST(RegionPopulator, RetryUsesTheSamePopulationStream) {
    Authority authority;
    std::vector<std::uint64_t> draws;
    auto populator = Populator([&](const auto& request) {
        draws.push_back(request.rng.draw(0));
        Mixed(request);
    });
    EXPECT_THROW(authority.execute(populator,
                                   {{},
                                    [] {
                                        throw std::runtime_error("Retry");
                                    }}),
                 std::runtime_error);
    authority.execute(populator);
    ASSERT_EQ(draws.size(), 2u);
    EXPECT_EQ(draws[0], draws[1]);
    const auto& genome = authority.registry.get<C::CreatureGenomeComponent>(
        *authority.registry.view<C::CreatureGenomeComponent>().begin());
    const RegionRng rng(19, kOwner, RegionRngTag::Population, 1);
    EXPECT_EQ(genome.female, (rng.draw(1) & 1u) != 0);
}
TEST(RegionPopulator, CommandsCannotSpawnOutsideRequestedOwner) {
    for (const float x : {-0.0001f, 511.9999f, 512.0f}) {
        for (const bool creature : {false, true}) {
            Authority authority;
            const auto before = authority.ledger.encode();
            auto populator = Populator([=](const auto& request) {
                Mixed(request);
                const Vec3 position{x, 0.0f, 128.0f};
                if (creature)
                    request.sink.spawn(CreatureSpawnCommand{std::uint16_t{17}, position, false});
                else
                    request.sink.spawn(GroundObjectSpawnCommand{7u, position, 0u, 0u});
            });
            EXPECT_THROW(authority.execute(populator), std::invalid_argument);
            authority.unchanged(before);
        }
    }
}
TEST(RegionPopulator, SuccessfulPublicationPreservesExistingOptionalInventoryAndReferences) {
    Authority authority;
    std::uint64_t id = 0;
    ASSERT_TRUE(authority.ledger.allocate_persistent_id(id));
    Factory(authority.registry,
            CreatureSpawnCommand{std::uint16_t{17}, kPosition, false},
            id,
            kOwner,
            1,
            37);
    const auto old = *authority.registry.view<C::CreatureComponent>().begin();
    authority.registry.emplace<C::SensesComponent>(old).nearest_threat = old;
    authority.registry.emplace<C::ActionPlanComponent>(old).plan.push_back(
        C::Action{"Observe", old, 2.0f, false});
    authority.registry.emplace<C::NeedsComponent>(old).needs.push_back(
        C::Need{"rest", 0.125f, 0.01f});
    authority.registry.emplace<C::TagComponent>(old).tag = "runtime attachment";
    const auto before = A::BuildCreatureSnapshot(authority.registry);
    authority.execute(Populator(Mixed));
    const auto after = A::BuildCreatureSnapshot(authority.registry);
    ASSERT_EQ(after.entities.size(), 2u);
    ASSERT_EQ(before.entities.size(), 1u);
    EXPECT_EQ(after.entities.front().components, before.entities.front().components);
    EXPECT_EQ(authority.registry.get<C::SensesComponent>(old).nearest_threat, old);
    EXPECT_EQ(authority.registry.get<C::ActionPlanComponent>(old).plan.front().target, old);
    EXPECT_EQ(authority.registry.get<C::TagComponent>(old).tag, "runtime attachment");
}
TEST(RegionPopulator, DefaultNoneCreatesNoAmbientPopulation) {
    Authority authority;
    const auto before = authority.ledger.encode();
    RegionPopulator none{std::string(kDefaultPopulationContract), {}, {}, {}};
    EXPECT_THROW(authority.execute(none), std::invalid_argument);
    authority.unchanged(before);
    Luminumbra::world::GameSession session;
    EXPECT_NO_THROW(session.SetRegionPopulator(none));
}
} // namespace
