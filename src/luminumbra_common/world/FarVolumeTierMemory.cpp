#include "FarVolumeTierMemory.h"

#include <limits>
#include <utility>

namespace Luminumbra::World {
namespace {
std::uint64_t Limit(std::uint64_t budget) noexcept {
    return budget == 0 ? std::numeric_limits<std::uint64_t>::max() : budget;
}
bool ValidTier(std::size_t tier) noexcept {
    return tier > 0 && tier <= kFarTierCount;
}
} // namespace

FarVolumeGpuLease::FarVolumeGpuLease(FarVolumeGpuLease&& other) noexcept
    : lease_(std::move(other.lease_))
    , gpu_(std::exchange(other.gpu_, nullptr)) {}
FarVolumeGpuLease::~FarVolumeGpuLease() {
    if (gpu_)
        gpu_->fetch_sub(lease_.bytes(), std::memory_order_acq_rel);
    // lease_ then returns the reservation to the shared CPU/GPU pool.
}
FarVolumeTierMemory::FarVolumeTierMemory(const FarTierBudgets& budgets) noexcept
    : pools_{{FarVolumeMemoryPool(Limit(budgets.bytes[0])),
              FarVolumeMemoryPool(Limit(budgets.bytes[1])),
              FarVolumeMemoryPool(Limit(budgets.bytes[2])),
              FarVolumeMemoryPool(Limit(budgets.bytes[3])),
              FarVolumeMemoryPool(Limit(budgets.bytes[4]))}} {
    static_assert(kFarTierCount == 5, "Update explicit fixed pool construction with tier table");
}
FarVolumeMemoryPool* FarVolumeTierMemory::Pool(std::size_t tier) noexcept {
    return ValidTier(tier) ? &pools_[tier - 1] : nullptr;
}
const FarVolumeMemoryPool* FarVolumeTierMemory::Pool(std::size_t tier) const noexcept {
    return ValidTier(tier) ? &pools_[tier - 1] : nullptr;
}
std::optional<FarVolumeGpuLease> FarVolumeTierMemory::ReserveGpu(std::size_t tier,
                                                                 std::uint64_t bytes) noexcept {
    auto* pool = Pool(tier);
    if (!pool)
        return std::nullopt;
    FarVolumeGpuLease staged(*pool, gpu_[tier - 1]);
    if (!staged.lease_.TryCharge(bytes)) {
        // A refused lease has zero bytes and contributes nothing on destruction.
        return std::nullopt;
    }
    gpu_[tier - 1].fetch_add(bytes, std::memory_order_acq_rel);
    return std::optional<FarVolumeGpuLease>(std::move(staged));
}
std::optional<FarVolumeTierMemoryStats>
FarVolumeTierMemory::Stats(std::size_t tier) const noexcept {
    const auto* pool = Pool(tier);
    if (!pool)
        return std::nullopt;
    const auto gpu = gpu_[tier - 1].load(std::memory_order_acquire);
    const auto total = pool->used();
    return FarVolumeTierMemoryStats{
        pool->limit(), total >= gpu ? total - gpu : 0, gpu, total, pool->peak(), pool->refusals()};
}
} // namespace Luminumbra::World
