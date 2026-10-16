#include "rendering/FoliageSurfaceSlots.h"

#include <gtest/gtest.h>

namespace Luminumbra::Rendering {

TEST(FoliageSurfaceSlots, ReleasesBeforeAdmissionAndReusesDeterministically) {
    FoliageSurfaceSlots slots;
    EXPECT_EQ(slots.acquire(10), 0u);
    EXPECT_EQ(slots.capacity(), 1u);
    EXPECT_EQ(slots.acquire(20), 1u);
    EXPECT_EQ(slots.capacity(), 2u);
    EXPECT_EQ(slots.acquire(30), 2u);
    EXPECT_EQ(slots.capacity(), 4u);
    EXPECT_EQ(slots.acquire(40), 3u);
    slots.begin_frame();
    slots.retain(20);
    slots.retain(40);
    slots.release_unretained();
    EXPECT_FALSE(slots.find(10));
    EXPECT_FALSE(slots.find(30));
    EXPECT_EQ(slots.find(20), 1u);
    EXPECT_EQ(slots.find(40), 3u);
    EXPECT_EQ(slots.acquire(50), 2u);
    EXPECT_EQ(slots.acquire(60), 0u);
    EXPECT_EQ(slots.acquire(50), 2u);
    EXPECT_EQ(slots.capacity(), 4u);
    slots.begin_frame();
    slots.release_unretained();
    EXPECT_EQ(slots.acquire(70), 3u);
    slots.clear();
    EXPECT_FALSE(slots.find(70));
    EXPECT_EQ(slots.acquire(80), 0u);
    EXPECT_EQ(slots.capacity(), 4u);
}

TEST(FoliageSurfaceSlots, GeometricGrowthStopsAtCapWithoutChangingLiveOwners) {
    FoliageSurfaceSlots slots;
    for (std::size_t index = 0; index < FoliageSurfaceSlots::kCapacity; ++index) {
        EXPECT_EQ(slots.acquire(index), index);
        const auto capacity = slots.capacity();
        EXPECT_GE(capacity, index + 1);
        EXPECT_EQ(capacity & (capacity - 1), 0u);
        EXPECT_LE(capacity, FoliageSurfaceSlots::kCapacity);
    }
    EXPECT_FALSE(slots.acquire(FoliageSurfaceSlots::kCapacity));
    for (std::size_t index = 0; index < FoliageSurfaceSlots::kCapacity; ++index)
        EXPECT_EQ(slots.find(index), index);
    slots.begin_frame();
    for (std::size_t index = 1; index < FoliageSurfaceSlots::kCapacity; ++index)
        slots.retain(index);
    slots.release_unretained();
    EXPECT_EQ(slots.acquire(FoliageSurfaceSlots::kCapacity), 0u);
}

TEST(FoliageSurfaceSlots, DistinctFootprintsInOneColumnHaveIndependentLifetimes) {
    FoliageSurfaceSlots slots;
    EXPECT_EQ(slots.acquire(10, 100), 0u);
    EXPECT_EQ(slots.acquire(10, 200), 1u);
    EXPECT_EQ(slots.acquire(10, 100), 0u);
    slots.begin_frame();
    slots.retain(10, 200);
    slots.release_unretained();
    EXPECT_FALSE(slots.find(10, 100));
    EXPECT_EQ(slots.find(10, 200), 1u);
    EXPECT_EQ(slots.acquire(10, 300), 0u);
}

} // namespace Luminumbra::Rendering
