#pragma once

#include "FarVolumeMemory.h"
#include "FarWantedSet.h"

#include <array>
#include <optional>

namespace Luminumbra::World {

class FarVolumeTierMemory;

// Admission token paired with ONE GPU allocation. The owner destroys its GL
// objects before this token; replacement admits a NEW token while the old GPU
// object/token remain alive. No GPU allocator or GL call is supplied here.
// The controller outlives every token. Tokens are single-owner, movable only by
// construction; replacement must explicitly dispose the previous resource pair.
class FarVolumeGpuLease {
public:
    FarVolumeGpuLease(FarVolumeGpuLease&& other) noexcept;
    FarVolumeGpuLease& operator=(FarVolumeGpuLease&&) = delete;
    FarVolumeGpuLease(const FarVolumeGpuLease&) = delete;
    FarVolumeGpuLease& operator=(const FarVolumeGpuLease&) = delete;
    ~FarVolumeGpuLease();
    std::uint64_t bytes() const noexcept {
        return lease_.bytes();
    }

private:
    friend class FarVolumeTierMemory;
    FarVolumeGpuLease(FarVolumeMemoryPool& pool, std::atomic<std::uint64_t>& gpu) noexcept
        : lease_(pool)
        , gpu_(&gpu) {}
    FarVolumeMemoryLease lease_;
    std::atomic<std::uint64_t>* gpu_ = nullptr;
};

struct FarVolumeTierMemoryStats {
    std::uint64_t limit = 0, bytes_cpu = 0, bytes_gpu = 0, bytes_total = 0;
    std::uint64_t peak_total = 0, admission_refusals = 0;
};

// S6 byte-admission infrastructure. Every tier has one pool shared by canonical
// compilation, converted/retained results, queue controls, authoritative
// overlays/snapshots and GPU reservations. Thus overlap (old + new generations,
// source + converted geometry and CPU + GPU upload) consumes the SAME limit.
//
// Callers must pass Pool(tier) to that tier's queue/compiler/adapter and charge
// snapshot/overlay payloads before allocating them; obtaining an immutable
// callback does not exempt its owned snapshot from this accounting. All leases,
// queues, snapshot allocations and GPU tokens must die before this controller.
// Uncharged callback/JobSystem bookkeeping exclusions remain those of the queue
// and canonical facade. This fixed controller has no heap control storage.
//
// Budgets are explicit inputs, never guessed shipped defaults. 0 means unlimited
// as in FarTierBudgets, mapped to UINT64_MAX for the admission pool. Limits are
// immutable; changing them requires cancel/drain/dispose and a new controller.
// This does not activate Ladder, allocate GPU objects, or replace the covering
// underlay/V5 eviction policy. A refusal feeds the runtime's Rejected policy;
// V5 protection must preserve its previous resource pair on that refusal.
class FarVolumeTierMemory {
public:
    explicit FarVolumeTierMemory(const FarTierBudgets& budgets) noexcept;
    FarVolumeTierMemory(const FarVolumeTierMemory&) = delete;
    FarVolumeTierMemory& operator=(const FarVolumeTierMemory&) = delete;

    // Null for invalid tiers. Each caller retains its own output lease.
    FarVolumeMemoryPool* Pool(std::size_t tier) noexcept;
    const FarVolumeMemoryPool* Pool(std::size_t tier) const noexcept;
    // No GPU work occurs unless this succeeds. Allocation failure afterwards
    // simply destroys the token. Zero-byte reservations are valid and uncounted.
    std::optional<FarVolumeGpuLease> ReserveGpu(std::size_t tier, std::uint64_t bytes) noexcept;
    // Diagnostics, NOT an admission decision. A concurrently compiling worker
    // can change used/peak between observations; cpu is clamped against skew.
    // Owner sampling after drain gives exact CPU + GPU = total observations.
    std::optional<FarVolumeTierMemoryStats> Stats(std::size_t tier) const noexcept;

private:
    std::array<FarVolumeMemoryPool, kFarTierCount> pools_;
    std::array<std::atomic<std::uint64_t>, kFarTierCount> gpu_{};
};

} // namespace Luminumbra::World
