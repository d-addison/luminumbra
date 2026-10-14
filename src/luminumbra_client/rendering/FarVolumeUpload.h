#pragma once

#include "FarVolumeBuildQueue.h"
#include "luminumbra_common/world/FarVolumeTierMemory.h"

#include <functional>
#include <memory>

namespace Luminumbra::Rendering {

// Backend resource destructor frees its actual GPU objects on the owner thread.
// Upload allocates exactly the admitted byte count. All backend temporary GL
// objects are also RAII-managed on failure; no uncharged caching/extra buffers.
class FarVolumeGpuAllocation {
public:
    virtual ~FarVolumeGpuAllocation() = default;
    virtual std::uint64_t bytes() const noexcept = 0;
};
using FarVolumeUploadBackend =
    std::function<std::unique_ptr<FarVolumeGpuAllocation>(const FarVolumeRenderMesh&)>;

enum class FarVolumeUploadError {
    None,
    NotReady,
    Stale,
    InvalidIntent,
    ByteLimit,
    InvalidBinding,
    AdmissionRefused,
    BackendFailure
};

// One uploaded resource + admission charge. Controller and GPU context outlive
// it; destruction/moves belong to the GL owner thread. Move assignment destroys
// old GPU resources BEFORE returning their charge. Empty is covering without a
// resource, after the same complete identity/span check as a GPU-backed tile.
class FarVolumeUploadedTile {
public:
    FarVolumeUploadedTile() = default;
    FarVolumeUploadedTile(FarVolumeUploadedTile&& other) noexcept;
    FarVolumeUploadedTile& operator=(FarVolumeUploadedTile&& other) noexcept;
    FarVolumeUploadedTile(const FarVolumeUploadedTile&) = delete;
    FarVolumeUploadedTile& operator=(const FarVolumeUploadedTile&) = delete;
    World::FarTileStatus Status(const World::FarBuildIdentity& current,
                                const World::FarVolumeCoverageIntent& intent) const noexcept;
    const World::FarBuildIdentity& identity() const noexcept {
        return identity_;
    }
    const World::FarVolumeRequest& request() const noexcept {
        return request_;
    }
    const FarVolumeGpuAllocation* resource() const noexcept {
        return resource_.get();
    }
    std::uint64_t gpu_bytes() const noexcept {
        return charge_ ? charge_->bytes() : 0;
    }

private:
    friend bool UploadFarVolume(const FarVolumeQueueCompletion&,
                                const World::FarBuildIdentity&,
                                const World::FarVolumeCoverageIntent&,
                                World::FarVolumeTierMemory&,
                                std::uint64_t,
                                const FarVolumeUploadBackend&,
                                FarVolumeUploadedTile&,
                                FarVolumeUploadError*);
    bool valid_ = false, empty_ = false;
    World::FarBuildIdentity identity_;
    World::FarVolumeRequest request_;
    // Reverse destruction: resource is freed before its budget reservation.
    std::optional<World::FarVolumeGpuLease> charge_;
    std::unique_ptr<FarVolumeGpuAllocation> resource_;
};

// CPU completion remains owned by caller through admission/upload (CPU+GPU
// overlap). Refusals/exceptions leave previous output and all its charges intact.
// max_upload_bytes is a hard caller-supplied bound BEFORE backend allocation.
// No GL calls, native defaults, scheduler integration or Ladder activation here.
bool UploadFarVolume(const FarVolumeQueueCompletion& completion,
                     const World::FarBuildIdentity& expected,
                     const World::FarVolumeCoverageIntent& current_intent,
                     World::FarVolumeTierMemory& memory,
                     std::uint64_t max_upload_bytes,
                     const FarVolumeUploadBackend& backend,
                     FarVolumeUploadedTile& output,
                     FarVolumeUploadError* error = nullptr);

} // namespace Luminumbra::Rendering
