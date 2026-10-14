#include "FarVolumeRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Luminumbra::World {
namespace {
bool AxisAdmitted(std::int64_t coordinate, const FarTierDimensions& tier) {
    const auto edge = static_cast<std::int64_t>(tier.tile_edge_meters);
    const auto spacing = static_cast<std::int64_t>(tier.sample_spacing_meters);
    const auto limit = kFarVolumeExactCoordinateLimitMeters;
    // Bound before multiplication; negative tile coordinates are valid.
    if (coordinate <= -limit / edge || coordinate >= limit / edge)
        return false;
    const auto origin = coordinate * edge;
    return origin - spacing > -limit && origin + edge + spacing < limit;
}
bool FiniteSpan(const FarVolumeCoverageSpan& span) {
    return std::isfinite(span.min_y) && std::isfinite(span.max_y) && span.min_y <= span.max_y;
}
} // namespace

bool ToFarVolumeTileKey(const FarTileKey& key, FarVolumeTileKey& output) {
    const auto tier = FarTierAt(key.tier);
    if (!tier || !AxisAdmitted(key.tx, *tier) || !AxisAdmitted(key.tz, *tier))
        return false;
    output = {key.tier, key.tx, key.tz};
    return true;
}

bool ToFarTileKey(const FarVolumeTileKey& key, FarTileKey& output) {
    if (key.tier == 0 || key.tier > kFarTierCount)
        return false;
    const FarTileKey candidate{static_cast<std::uint8_t>(key.tier), key.x, key.z};
    FarVolumeTileKey checked;
    if (!ToFarVolumeTileKey(candidate, checked))
        return false;
    output = candidate;
    return true;
}

bool FarVolumeSpanLedger::Reset(const FarTileKey& key) {
    FarVolumeTileKey candidate;
    if (!ToFarVolumeTileKey(key, candidate))
        return false;
    key_ = candidate;
    extra_.reset();
    generation_ = 0;
    return true;
}

bool FarVolumeSpanLedger::Extend(const FarVolumeCoverageSpan& request,
                                 const FarVolumeSpanLedgerLimits& limits) {
    constexpr auto guard = kFarVolumeExactCoordinateLimitMeters - 1024;
    if (!key_ || !FiniteSpan(request) || !FiniteSpan({limits.min_y, limits.max_y}) ||
        limits.min_y <= -guard || limits.max_y >= guard ||
        !std::isfinite(limits.max_extent_meters) || !(limits.max_extent_meters > 0))
        return false;
    const FarVolumeCoverageSpan candidate{
        extra_ ? std::min(extra_->min_y, request.min_y) : request.min_y,
        extra_ ? std::max(extra_->max_y, request.max_y) : request.max_y};
    if (candidate.min_y < limits.min_y || candidate.max_y > limits.max_y ||
        double(candidate.max_y) - double(candidate.min_y) > limits.max_extent_meters)
        return false;
    if (extra_ && candidate == *extra_)
        return true;
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    extra_ = candidate;
    ++generation_;
    return true;
}

std::optional<FarVolumeCoverageIntent> FarVolumeSpanLedger::Intent() const {
    if (!key_)
        return std::nullopt;
    return FarVolumeCoverageIntent{*key_, extra_};
}

bool FarVolumeSpanLedger::Covers(const FarVolumeSpan& built) const {
    if (!key_)
        return false;
    const auto tier = FarTierAt(key_->tier);
    if (!tier)
        return false;
    const auto edge = static_cast<std::int64_t>(tier->brick_edge_meters);
    if (built.min_y <= -kFarVolumeExactCoordinateLimitMeters ||
        built.max_y >= kFarVolumeExactCoordinateLimitMeters || built.min_y >= built.max_y ||
        built.min_y % edge != 0 || built.max_y % edge != 0)
        return false;
    return !extra_ ||
           (built.min_y <= double(extra_->min_y) && built.max_y >= double(extra_->max_y));
}

} // namespace Luminumbra::World
