#include "FarWorldOwnership.h"

#include "luminumbra/core/Types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace Luminumbra::Rendering {
namespace {
bool Less(const FarWorldLiveCell& a, const FarWorldLiveCell& b) noexcept {
    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}
bool Finite(const std::array<double, 3>& p) noexcept {
    return std::all_of(p.begin(), p.end(), [](double v) { return std::isfinite(v); });
}
} // namespace
bool IsFarWorldLiveUploadCurrent(const FarWorldLiveUpload& upload) noexcept {
    return upload.valid_allocation && upload.uploaded_indices != 0 &&
           upload.snapshot_version == upload.uploaded_version;
}
bool IsFarWorldLiveWaterUploadCurrent(const FarWorldLiveUpload& upload) noexcept {
    return upload.water_valid_allocation && upload.water_uploaded_indices != 0 &&
           upload.water_snapshot_version == upload.water_uploaded_version &&
           upload.water_pass_eligible;
}
bool FarWorldChunkCell(const std::array<double, 3>& position, FarWorldLiveCell& cell) noexcept {
    if (!Finite(position))
        return false;
    constexpr std::array<double, 3> edges{CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z};
    std::array<std::int32_t, 3> coordinates{};
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        const auto value = std::floor(position[i] / edges[i]);
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max())
            return false;
        coordinates[i] = static_cast<std::int32_t>(value);
    }
    cell = {coordinates[0], coordinates[1], coordinates[2], 0};
    return true;
}
bool MakeFarWorldCellOrigin(const std::array<double, 3>& origin,
                            FarWorldCellOrigin& output) noexcept {
    FarWorldCellOrigin candidate;
    if (!FarWorldChunkCell(origin, candidate.key))
        return false;
    constexpr std::array<double, 3> edges{CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z};
    const std::array keys{candidate.key.x, candidate.key.y, candidate.key.z};
    for (std::size_t i = 0; i < origin.size(); ++i) {
        const double remainder = origin[i] - static_cast<double>(keys[i]) * edges[i];
        candidate.remainder[i] = static_cast<float>(remainder);
        if (remainder < 0 || remainder >= edges[i] ||
            static_cast<double>(candidate.remainder[i]) != remainder)
            return false;
    }
    output = candidate;
    return true;
}
bool FarWorldChunkCell(const FarWorldCellOrigin& origin,
                       const std::array<float, 3>& local,
                       FarWorldLiveCell& cell) noexcept {
    // Canonical tile XZ origins and brick-aligned span minima are multiples of
    // 16, hence remainder == 0. Adding zero preserves every stored float local
    // (including reconstructed noninteger surfaces); division by 16 is exact
    // binary exponent scaling. Integer/power-of-two boundary locals remain exact
    // below 2^24, and floor handles negative values. Nonzero integer remainders
    // plus integer locals are also exact below 2^24. The key stays integer even
    // for large translated worlds. No camera subtraction/addition participates.
    static_assert(CHUNK_SIZE_X == 16 && CHUNK_SIZE_Y == 16 && CHUNK_SIZE_Z == 16);
    static_assert(std::numeric_limits<float>::radix == 2 &&
                  std::numeric_limits<float>::digits == 24);
    static_assert([] {
        for (const auto& tier : World::kFarTierTable)
            if (tier.tile_edge_meters % 16 != 0 || tier.brick_edge_meters % 16 != 0)
                return false;
        return true;
    }());
    const std::array keys{origin.key.x, origin.key.y, origin.key.z};
    std::array<std::int32_t, 3> coordinates{};
    for (std::size_t i = 0; i < local.size(); ++i) {
        const float offset = std::floor((origin.remainder[i] + local[i]) / 16.0f);
        if (!std::isfinite(offset) || origin.remainder[i] < 0 || origin.remainder[i] >= 16 ||
            static_cast<double>(offset) < std::numeric_limits<std::int32_t>::min() ||
            static_cast<double>(offset) > std::numeric_limits<std::int32_t>::max())
            return false;
        const auto value = static_cast<std::int64_t>(keys[i]) + static_cast<std::int64_t>(offset);
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max())
            return false;
        coordinates[i] = static_cast<std::int32_t>(value);
    }
    cell = {coordinates[0], coordinates[1], coordinates[2], 0};
    return true;
}
void FarWorldOwnership::Begin(std::uint64_t frame, std::size_t limit) noexcept {
    frame_ = frame;
    count_ = 0;
    candidates_ = 0;
    limit_ = std::min(limit, kFarWorldLiveCellLimit);
    failed_ = limit > kFarWorldLiveCellLimit;
    complete_ = false;
}
bool FarWorldOwnership::Add(const FarWorldLiveUpload& upload) noexcept {
    if (failed_ || complete_)
        return false;
    if (candidates_ == kFarWorldLiveCellLimit) {
        failed_ = true;
        return false;
    }
    ++candidates_;
    const auto bits = (IsFarWorldLiveUploadCurrent(upload) ? kFarWorldTerrainReady : 0) |
                      (IsFarWorldLiveWaterUploadCurrent(upload) ? kFarWorldWaterReady : 0);
    if (bits == 0)
        return true;
    if (count_ == limit_ || upload.cell.reserved != 0) {
        failed_ = true;
        return false;
    }
    cells_[count_] = upload.cell;
    cells_[count_++].reserved = bits;
    return true;
}
bool FarWorldOwnership::Finish() noexcept {
    if (failed_)
        return false;
    std::sort(cells_.begin(), cells_.begin() + static_cast<std::ptrdiff_t>(count_), Less);
    std::size_t merged = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        if (merged && !Less(cells_[merged - 1], cells_[i]))
            cells_[merged - 1].reserved |= cells_[i].reserved;
        else
            cells_[merged++] = cells_[i];
    }
    count_ = merged;
    complete_ = true;
    return true;
}
std::span<const FarWorldLiveCell> FarWorldOwnership::cells() const noexcept {
    return {cells_.data(), complete_ ? count_ : 0};
}
bool FarWorldOwnership::Owns(const std::array<double, 3>& position) const noexcept {
    FarWorldLiveCell cell;
    if (!complete_ || !FarWorldChunkCell(position, cell))
        return false;
    const auto found = std::lower_bound(cells().begin(), cells().end(), cell, Less);
    return found != cells().end() && !Less(cell, *found) &&
           (found->reserved & kFarWorldTerrainReady);
}
bool FarWorldOwnership::OwnsWater(const std::array<double, 3>& position) const noexcept {
    FarWorldLiveCell cell;
    if (!complete_ || !FarWorldChunkCell(position, cell))
        return false;
    const auto found = std::lower_bound(cells().begin(), cells().end(), cell, Less);
    return found != cells().end() && !Less(cell, *found) && (found->reserved & kFarWorldWaterReady);
}
bool FarWorldOwnership::Owns(const FarWorldCellOrigin& origin,
                             const std::array<float, 3>& local) const noexcept {
    FarWorldLiveCell cell;
    if (!complete_ || !FarWorldChunkCell(origin, local, cell))
        return false;
    const auto found = std::lower_bound(cells().begin(), cells().end(), cell, Less);
    return found != cells().end() && !Less(cell, *found) &&
           (found->reserved & kFarWorldTerrainReady);
}
bool FarWorldArrivalReplaces(const FarWorldArrival& arrival,
                             float tile_edge,
                             const std::array<float, 3>& p,
                             std::int64_t tile_origin_y,
                             double relative_x,
                             double relative_z) noexcept {
    if (!arrival.eligible ||
        !std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v); }) ||
        !std::isfinite(relative_x) || !std::isfinite(relative_z) || !(tile_edge > 0) || p[0] < 0 ||
        p[2] < 0 || p[0] >= tile_edge || p[2] >= tile_edge ||
        std::hypot(relative_x, relative_z) > arrival.finer_outer_radius_meters)
        return false;
    const auto child = std::size_t(p[0] >= tile_edge / 2) + 2 * std::size_t(p[2] >= tile_edge / 2);
    // Same local Y comparison as the shader; canonical integer span differences
    // are exactly representable floats within the admitted +/-2^23 world domain.
    const auto min_y = static_cast<float>(static_cast<double>(arrival.children[child].min_y) -
                                          static_cast<double>(tile_origin_y));
    const auto max_y = static_cast<float>(static_cast<double>(arrival.children[child].max_y) -
                                          static_cast<double>(tile_origin_y));
    return p[1] >= min_y && p[1] <= max_y;
}
bool FarWorldFragmentInClip(const std::array<double, 3>& p, double outer_radius) noexcept {
    return Finite(p) && std::isfinite(outer_radius) && outer_radius > 0 &&
           std::hypot(p[0], p[1], p[2]) >= kFarWorldInnerRadiusMeters &&
           std::hypot(p[0], p[2]) <= outer_radius;
}
} // namespace Luminumbra::Rendering
