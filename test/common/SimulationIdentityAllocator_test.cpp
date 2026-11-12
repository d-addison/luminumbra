#include <gtest/gtest.h>

#include "luminumbra_common/components/SimulationIdentityComponent.h"
#include "luminumbra_common/simulation/SimulationIdentity.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/GroundObjectStore.h"

#include <entt/entity/registry.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using Luminumbra::Components::SimulationIdentityComponent;
using Luminumbra::simulation::AllocateSimulationIdentity;
using Luminumbra::simulation::IsValidSimulationId;
using namespace Luminumbra::world;

struct PlantRole {};
struct CreatureRole {};
struct OtherRole {};

TEST(SimulationIdentityAllocatorTest, ValidIdsExcludeZeroAndTheExhaustedCursor) {
    EXPECT_FALSE(IsValidSimulationId(0));
    EXPECT_TRUE(IsValidSimulationId(1));
    EXPECT_TRUE(IsValidSimulationId(ActiveRegionLedger::kPersistentIdLimit - 1));
    EXPECT_FALSE(IsValidSimulationId(ActiveRegionLedger::kPersistentIdLimit));
    EXPECT_FALSE(IsValidSimulationId(std::numeric_limits<std::uint64_t>::max()));
    EXPECT_FALSE(IsValidSimulationId(SimulationIdentityComponent{}.id));
}

TEST(SimulationIdentityAllocatorTest, AllKindsShareOneStreamAndExtinctionKeepsCursor) {
    ActiveRegionLedger ledger;
    entt::registry registry;
    GroundObjectStore ground;
    const auto plant = registry.create();
    const auto creature = registry.create();
    registry.emplace<PlantRole>(plant);
    registry.emplace<CreatureRole>(creature);
    // Multiple simulation roles on one entity still use one identity.
    registry.emplace<OtherRole>(creature);
    const auto plant_identity = AllocateSimulationIdentity(ledger);
    const auto creature_identity = AllocateSimulationIdentity(ledger);
    ASSERT_TRUE(plant_identity.has_value());
    ASSERT_TRUE(creature_identity.has_value());
    registry.emplace<SimulationIdentityComponent>(
        plant, plant_identity.value_or(SimulationIdentityComponent{}));
    registry.emplace<SimulationIdentityComponent>(
        creature, creature_identity.value_or(SimulationIdentityComponent{}));
    GroundObject object{.type = 1};
    ASSERT_TRUE(ledger.allocate_persistent_id(object.id));
    ASSERT_TRUE(ground.insert(object));
    const auto other_identity = AllocateSimulationIdentity(ledger);
    ASSERT_TRUE(other_identity.has_value());
    EXPECT_EQ(registry.get<SimulationIdentityComponent>(plant).id, 1u);
    EXPECT_EQ(registry.get<SimulationIdentityComponent>(creature).id, 2u);
    EXPECT_EQ(object.id, 3u);
    EXPECT_EQ(other_identity.value_or(SimulationIdentityComponent{}).id, 4u);
    EXPECT_EQ(ledger.next_persistent_id(), 5u);

    registry.clear();
    ground.clear();
    ASSERT_TRUE(ledger.records().empty());
    EXPECT_EQ(ledger.next_persistent_id(), 5u);
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), restored, error)) << error;
    EXPECT_EQ(restored.next_persistent_id(), 5u);
    const auto next = AllocateSimulationIdentity(restored);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value_or(SimulationIdentityComponent{}).id, 5u);
}

TEST(SimulationIdentityAllocatorTest, DetachedReservationRestoresLargeCursorWithoutIteration) {
    ActiveRegionLedger ledger;
    constexpr auto cursor = std::uint64_t{1} << 62;
    const auto visits = ledger.capacity_record_visits();
    ASSERT_TRUE(ledger.reserve_persistent_ids(cursor));
    EXPECT_EQ(ledger.capacity_record_visits(), visits);
    EXPECT_EQ(ledger.next_persistent_id(), cursor);
    const auto before = ledger.encode();
    const auto canonical = ledger.canonical_bytes();
    for (const auto invalid : {std::uint64_t{0},
                               std::uint64_t{1},
                               cursor - 1,
                               ActiveRegionLedger::kPersistentIdLimit + 1,
                               std::numeric_limits<std::uint64_t>::max()}) {
        EXPECT_FALSE(ledger.reserve_persistent_ids(invalid));
        EXPECT_EQ(ledger.next_persistent_id(), cursor);
        EXPECT_EQ(ledger.encode(), before);
        EXPECT_EQ(ledger.canonical_bytes(), canonical);
    }
    EXPECT_TRUE(ledger.reserve_persistent_ids(cursor));
    EXPECT_EQ(ledger.encode(), before);
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(before, restored, error)) << error;
    std::uint64_t id = 0;
    ASSERT_TRUE(restored.allocate_persistent_id(id));
    EXPECT_EQ(id, cursor);
}

TEST(SimulationIdentityAllocatorTest, LastIdAndExhaustedReservationsNeverWrapOrMutateOnRefusal) {
    ActiveRegionLedger ledger;
    constexpr auto limit = ActiveRegionLedger::kPersistentIdLimit;
    ASSERT_TRUE(ledger.reserve_persistent_ids(limit - 1));
    const auto last = AllocateSimulationIdentity(ledger);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value_or(SimulationIdentityComponent{}).id, limit - 1);
    EXPECT_EQ(ledger.next_persistent_id(), limit);
    ActiveRegionLedger reserved;
    ASSERT_TRUE(reserved.reserve_persistent_ids(limit));
    EXPECT_EQ(reserved.encode(), ledger.encode());
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(reserved.encode(), restored, error)) << error;
    for (auto* candidate : {&ledger, &reserved, &restored}) {
        const auto before = candidate->encode();
        std::uint64_t unchanged = 42;
        EXPECT_FALSE(candidate->allocate_persistent_id(unchanged));
        EXPECT_EQ(unchanged, 42u);
        EXPECT_FALSE(AllocateSimulationIdentity(*candidate).has_value());
        EXPECT_FALSE(candidate->reserve_persistent_ids(limit - 1));
        EXPECT_FALSE(candidate->reserve_persistent_ids(limit + 1));
        EXPECT_TRUE(candidate->reserve_persistent_ids(limit));
        EXPECT_EQ(candidate->encode(), before);
    }
}

TEST(SimulationIdentityAllocatorTest, ReservationUsesTheExistingAllocatorTlvAndHashProjection) {
    ActiveRegionLedger allocated;
    for (unsigned index = 0; index < 3; ++index) {
        std::uint64_t id = 0;
        ASSERT_TRUE(allocated.allocate_persistent_id(id));
    }
    ActiveRegionLedger reserved;
    ASSERT_TRUE(reserved.reserve_persistent_ids(1));
    EXPECT_EQ(reserved.encode(), ActiveRegionLedger{}.encode());
    EXPECT_TRUE(reserved.canonical_bytes().empty());
    ASSERT_TRUE(reserved.reserve_persistent_ids(4));
    EXPECT_EQ(reserved.encode(), allocated.encode());
    EXPECT_EQ(reserved.canonical_bytes(), allocated.canonical_bytes());
    // The existing sparse publication path must admit the same cursor domain.
    ActiveRegionLedger staged;
    for (const auto invalid : {std::uint64_t{0}, ActiveRegionLedger::kPersistentIdLimit + 1}) {
        EXPECT_FALSE(staged.prepare_energy_work({}, {}, invalid));
        EXPECT_EQ(staged.encode(), ActiveRegionLedger{}.encode());
    }
    ASSERT_TRUE(staged.prepare_energy_work({}, {}, 4));
    staged.commit_energy_work();
    EXPECT_EQ(staged.encode(), reserved.encode());
    EXPECT_FALSE(staged.prepare_energy_work({}, {}, 3));
    EXPECT_EQ(staged.encode(), reserved.encode());
}

TEST(SimulationIdentityAllocatorTest, NearCapacityAllocationAndReservationNeverVisitRecords) {
    ActiveRegionLedger ledger;
    // At most 2^20-1 records. Four extension owners leave three spare bytes when the
    // allocator is added; a conservative projection allowance would exceed the cap.
    for (std::uint32_t index = 0; index + 1 < ActiveRegionLedger::kMaxRegions; ++index) {
        ledger.mark_edited(
            {static_cast<std::int32_t>(index % 1024u), static_cast<std::int32_t>(index / 1024u)},
            WorldClock{});
    }
    ASSERT_TRUE(ledger.set_energy_ages({{{0, 0}, 0}, {{1, 0}, 0}}));
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 1}}));
    const std::array work{RegionWork{{0, 0}, 1, 0, 0, 0}};
    ledger.publish_completed_work(work);
    const auto visits = ledger.capacity_record_visits();
    for (std::uint64_t expected = 1; expected <= 128; ++expected) {
        const auto identity = AllocateSimulationIdentity(ledger);
        ASSERT_TRUE(identity.has_value());
        EXPECT_EQ(identity.value_or(SimulationIdentityComponent{}).id, expected);
    }
    ASSERT_TRUE(ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit));
    EXPECT_EQ(ledger.capacity_record_visits(), visits);
    EXPECT_EQ(ledger.encode().size(), ActiveRegionLedger::kMaxFileBytes - 3);
}

} // namespace
