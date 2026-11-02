#pragma once

#include "FarWorldHandoffDiagnostics.h"
#include "FarWorldRenderer.h"

namespace Luminumbra::Rendering {
// Explicit production-capable adapter. Selection is a caller decision independent
// of simulation configuration. No client/session or persistent-cache callbacks.
class FarWorldHandoffRenderer {
public:
    FarWorldHandoffRenderer() = default;
    ~FarWorldHandoffRenderer();
    FarWorldHandoffRenderer(const FarWorldHandoffRenderer&) = delete;
    FarWorldHandoffRenderer& operator=(const FarWorldHandoffRenderer&) = delete;
    void InstallBackend(std::unique_ptr<FarVolumeGpuDriver>);
    bool Bind(JobSystem&,
              std::uint64_t world_epoch,
              World::FarCaveMode,
              World::FarVolumeNumericProfile,
              const World::FarTierBudgets&,
              const FarVolumeResidencyLimits&,
              std::pmr::memory_resource* = nullptr);
    // Owning aliases permit frozen capture to retain this exact memory domain.
    std::shared_ptr<World::FarVolumeMemoryPool> MemoryPool(std::size_t tier) const;
    bool StageTarget(const FarHandoffManifest&);
    bool AdvanceFrame(std::uint64_t frame,
                      const std::array<double, 3>& camera,
                      const FarVolumeFrameLimits&);
    void BeginLiveOwnership(std::uint64_t frame, std::size_t limit = kFarWorldLiveCellLimit);
    bool AddLiveUpload(const FarWorldLiveUpload&);
    bool FinishLiveOwnership();
    FarWorldSubmitStats Submit(const std::array<double, 3>& camera, const FarWorldPrepareDraw&);
    const FarVolumeHandoff* Handoff() const;
    const FarWorldHandoffDiagnosticSnapshot& Diagnostic() const;
    // Wait only for accepted persistent cache writes; retain binding and render state.
    // This backend currently has no persistent cache writer.
    void WaitForAcceptedCacheWrites();
    void PrepareWorldSwap();
    void Shutdown();

private:
    class Guard;
    void CheckOwner() const;
    void Unbind() noexcept;
    void Drain();
    const std::thread::id owner_ = std::this_thread::get_id();
    bool submitting_ = false, mutating_ = false;
    std::uint64_t world_epoch_ = 0;
    World::FarCaveMode caves_ = World::FarCaveMode::BandLimited;
    World::FarVolumeNumericProfile numeric_ = World::FarVolumeNumericProfile::RawFloat;
    std::optional<std::uint64_t> frame_;
    std::unique_ptr<FarVolumeGpuDriver> driver_;
    std::shared_ptr<World::FarVolumeTierMemory> memory_;
    std::optional<World::FarVolumeMemoryLease> charge_;
    std::optional<World::FarVolumeGpuLease> ownership_charge_;
    FarWorldOwnership ownership_;
    std::unique_ptr<FarVolumeHandoff> handoff_;
    FarWorldHandoffDiagnosticSnapshot diagnostic_;
};
} // namespace Luminumbra::Rendering
