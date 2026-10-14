#pragma once

#include "luminumbra_common/world/FarVolumeTile.h"

#include <array>
#include <span>

namespace Luminumbra::Rendering {

// std430 ivec4: integer 3D chunk cell, fourth lane readiness bits. Lexicographic XYZ.
inline constexpr std::int32_t kFarWorldTerrainReady = 1, kFarWorldWaterReady = 2;
struct FarWorldLiveCell {
    std::int32_t x = 0, y = 0, z = 0, reserved = 0;
    bool operator==(const FarWorldLiveCell&) const = default;
};
static_assert(sizeof(FarWorldLiveCell) == 16);
inline constexpr std::size_t kFarWorldLiveCellLimit = 8192;
inline constexpr std::uint64_t kFarWorldOwnershipGpuBytes =
    kFarWorldLiveCellLimit * sizeof(FarWorldLiveCell);
inline constexpr double kFarWorldInnerRadiusMeters = 176;

struct FarWorldLiveUpload {
    FarWorldLiveCell cell;
    std::uint32_t snapshot_version = 0, uploaded_version = 0;
    std::uint64_t uploaded_indices = 0;
    bool valid_allocation = false;
    std::uint32_t water_snapshot_version = 0, water_uploaded_version = 0;
    std::uint64_t water_uploaded_indices = 0;
    bool water_valid_allocation = false, water_pass_eligible = false;
};
bool IsFarWorldLiveWaterUploadCurrent(const FarWorldLiveUpload& upload) noexcept;
bool IsFarWorldLiveUploadCurrent(const FarWorldLiveUpload& upload) noexcept;
bool FarWorldChunkCell(const std::array<double, 3>& position, FarWorldLiveCell& cell) noexcept;

// Per-draw decomposition, independent of camera and view rotation. Remainder is
// origin - key * chunk edge; only exactly representable remainders are accepted.
struct FarWorldCellOrigin {
    FarWorldLiveCell key;
    std::array<float, 3> remainder{};
};
bool MakeFarWorldCellOrigin(const std::array<double, 3>& origin,
                            FarWorldCellOrigin& output) noexcept;
// Float CPU mirror of the production shader: key + floor((remainder + local)/16).
bool FarWorldChunkCell(const FarWorldCellOrigin& origin,
                       const std::array<float, 3>& local,
                       FarWorldLiveCell& cell) noexcept;

// Fixed, charged as part of the renderer owner. Begin invalidates the old frame;
// a refused/incomplete publication exposes no partial mask. Only Finish seals a
// value snapshot. No visibility or camera-region membership enters eligibility.
class FarWorldOwnership {
public:
    void Begin(std::uint64_t frame, std::size_t limit = kFarWorldLiveCellLimit) noexcept;
    bool Add(const FarWorldLiveUpload& upload) noexcept;
    bool Finish() noexcept;
    bool complete() const noexcept {
        return complete_;
    }
    std::uint64_t frame() const noexcept {
        return frame_;
    }
    std::span<const FarWorldLiveCell> cells() const noexcept;
    bool Owns(const std::array<double, 3>& position) const noexcept;
    bool OwnsWater(const std::array<double, 3>& position) const noexcept;
    bool Owns(const FarWorldCellOrigin& origin, const std::array<float, 3>& local) const noexcept;

private:
    std::array<FarWorldLiveCell, kFarWorldLiveCellLimit> cells_{};
    std::size_t count_ = 0, limit_ = 0, candidates_ = 0;
    std::uint64_t frame_ = 0;
    bool failed_ = false, complete_ = false;
};

struct FarWorldArrival {
    bool eligible = false;
    double finer_outer_radius_meters = 0;
    std::array<World::FarVolumeSpan, 4> children{}; // x + 2*z, built spans, inclusive samples
};
// CPU mirror for Q20: stored local XZ selects half-open children, and local Y
// is compared to built spans minus the integer tile/span origin, just as in the
// shader. Built Y includes the sampled upper boundary. Camera-relative values
// are used only for the Q27 horizontal distance, never for child/span boundaries.
bool FarWorldArrivalReplaces(const FarWorldArrival& arrival,
                             float tile_edge,
                             const std::array<float, 3>& local_position,
                             std::int64_t tile_origin_y,
                             double camera_relative_x,
                             double camera_relative_z) noexcept;
bool FarWorldFragmentInClip(const std::array<double, 3>& camera_relative,
                            double outer_radius) noexcept;
} // namespace Luminumbra::Rendering
