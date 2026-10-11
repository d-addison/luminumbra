#include "luminumbra_common/world/FarVolumeTierMemory.h"

#include <gtest/gtest.h>
#include <limits>
#include <type_traits>

using namespace Luminumbra::World;
namespace {
FarVolumeTierMemoryStats Observe(const FarVolumeTierMemory& memory, std::size_t tier) {
    const auto stats = memory.Stats(tier);
    if (!stats) {
        ADD_FAILURE() << "No memory observation for tier " << tier;
        return {};
    }
    return *stats;
}
} // namespace

TEST(FarVolumeTierMemory, SnapshotAndOldNewGpuShareAdmissionBeforeAllocation) {
    FarTierBudgets budgets;
    budgets.bytes[0] = 128;
    FarVolumeTierMemory memory(budgets);
    auto* pool = memory.Pool(1);
    ASSERT_NE(pool, nullptr);
    FarVolumeMemoryLease snapshot_charge(*pool);
    FarVolumeChargedResource resource(snapshot_charge);
    std::pmr::vector<std::byte> snapshot(&resource);
    snapshot.resize(64);
    auto old_gpu = memory.ReserveGpu(1, 48);
    ASSERT_TRUE(old_gpu);
    EXPECT_EQ(Observe(memory, 1).bytes_cpu, 64u);
    EXPECT_EQ(Observe(memory, 1).bytes_gpu, 48u);
    EXPECT_EQ(Observe(memory, 1).bytes_total, 112u);
    // No GPU allocation may be attempted when this token is refused; the old
    // resource remains protected. Snapshot growth also counts both buffers.
    EXPECT_FALSE(memory.ReserveGpu(1, 48));
    EXPECT_THROW(snapshot.reserve(128), FarVolumeMemoryRefused);
    EXPECT_EQ(snapshot.size(), 64u);
    EXPECT_EQ(old_gpu->bytes(), 48u);
    EXPECT_EQ(pool->used(), 112u);
    EXPECT_EQ(Observe(memory, 1).admission_refusals, 2u);
    old_gpu.reset(); // real caller first deletes its old GL object
    auto replacement = memory.ReserveGpu(1, 48);
    ASSERT_TRUE(replacement);
    EXPECT_EQ(Observe(memory, 1).bytes_total, 112u);
}

TEST(FarVolumeTierMemory, GpuReplacementHoldsBothAndMoveTransfersExactlyOnce) {
    static_assert(!std::is_copy_constructible_v<FarVolumeGpuLease>);
    static_assert(!std::is_move_assignable_v<FarVolumeGpuLease>);
    FarTierBudgets budgets;
    budgets.bytes[4] = 96;
    FarVolumeTierMemory memory(budgets);
    auto old_gpu = memory.ReserveGpu(5, 40);
    auto new_gpu = memory.ReserveGpu(5, 48);
    ASSERT_TRUE(old_gpu);
    ASSERT_TRUE(new_gpu);
    EXPECT_EQ(Observe(memory, 5).bytes_gpu, 88u);
    EXPECT_FALSE(memory.ReserveGpu(5, 9));
    EXPECT_EQ(Observe(memory, 5).bytes_total, 88u);
    {
        FarVolumeGpuLease transferred(std::move(*new_gpu));
        new_gpu.reset();
        EXPECT_EQ(Observe(memory, 5).bytes_gpu, 88u);
        old_gpu.reset();
        EXPECT_EQ(Observe(memory, 5).bytes_gpu, 48u);
        EXPECT_EQ(transferred.bytes(), 48u);
    }
    EXPECT_EQ(Observe(memory, 5).bytes_gpu, 0u);
    EXPECT_EQ(Observe(memory, 5).bytes_total, 0u);
    EXPECT_EQ(Observe(memory, 5).peak_total, 88u);
}

TEST(FarVolumeTierMemory, TierIsolationInvalidAndUnlimitedDoNotWrap) {
    FarTierBudgets budgets;
    budgets.bytes[0] = 8;
    budgets.bytes[1] = 16;
    FarVolumeTierMemory memory(budgets);
    EXPECT_EQ(memory.Pool(0), nullptr);
    EXPECT_EQ(memory.Pool(6), nullptr);
    EXPECT_FALSE(memory.ReserveGpu(0, 1));
    EXPECT_FALSE(memory.Stats(6));
    auto first = memory.ReserveGpu(1, 8);
    ASSERT_TRUE(first);
    EXPECT_FALSE(memory.ReserveGpu(1, 1));
    auto second = memory.ReserveGpu(2, 16);
    ASSERT_TRUE(second);
    EXPECT_EQ(Observe(memory, 1).bytes_total, 8u);
    EXPECT_EQ(Observe(memory, 2).bytes_total, 16u);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(Observe(memory, 3).limit, maximum);
    auto unlimited = memory.ReserveGpu(3, maximum);
    ASSERT_TRUE(unlimited);
    EXPECT_FALSE(memory.ReserveGpu(3, 1));
    auto zero = memory.ReserveGpu(3, 0);
    ASSERT_TRUE(zero);
    EXPECT_EQ(zero->bytes(), 0u);
    EXPECT_EQ(Observe(memory, 3).bytes_gpu, maximum);
    EXPECT_EQ(Observe(memory, 3).bytes_total, maximum);
    unlimited.reset();
    EXPECT_EQ(Observe(memory, 3).bytes_total, 0u);
}
