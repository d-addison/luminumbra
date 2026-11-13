#include "FarVolumeRuntime.h"

#include "FarTierTable.h"
#include "FarVolume.h"
#include "FarVolumeMesher.h"
#include "FarVolumeTile.h"
#include "FarWantedSet.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

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

bool FarVolumePagePlan::Reset(const FarVolumeRequest& request,
                              std::int64_t stride_meters,
                              std::uint64_t page_cap) noexcept {
    const auto tier = FarTierAt(request.key.tier);
    FarTileKey checked;
    if (!tier || !ToFarTileKey(request.key, checked) || stride_meters <= 0 ||
        stride_meters > kFarVolumeMaxPageStrideMeters ||
        stride_meters % tier->brick_edge_meters != 0 || page_cap == 0 ||
        page_cap > kFarVolumeMaxPages)
        return false;
    const auto span = request.span;
    const auto step = static_cast<std::int64_t>(tier->sample_spacing_meters);
    // Validate before subtraction, rounding, multiplication or halo expansion.
    if (span.min_y <= -kFarVolumeExactCoordinateLimitMeters + step ||
        span.max_y >= kFarVolumeExactCoordinateLimitMeters - step || span.min_y >= span.max_y ||
        span.min_y % tier->brick_edge_meters != 0 || span.max_y % tier->brick_edge_meters != 0)
        return false;
    const auto floor_page = [stride_meters](std::int64_t y) {
        return y / stride_meters - (y % stride_meters < 0 ? 1 : 0);
    };
    const auto first = floor_page(span.min_y);
    const auto last = floor_page(span.max_y - 1);
    const auto count = static_cast<std::uint64_t>(last - first + 1);
    if (count > page_cap)
        return false;
    request_ = request;
    stride_ = stride_meters;
    first_ = first;
    count_ = count;
    return true;
}

bool FarVolumePagePlan::Page(std::uint64_t index, FarVolumeRequest& output) const noexcept {
    if (index >= count_)
        return false;
    // Reset bounds first_ and count_ within the exact-coordinate domain. Even
    // the unclipped outer windows extend by at most one admitted stride.
    const auto lower = (first_ + static_cast<std::int64_t>(index)) * stride_;
    output = {
        request_.key,
        {std::max(lower, request_.span.min_y), std::min(lower + stride_, request_.span.max_y)},
        request_.field_identity};
    return true;
}

bool FarVolumePagePlan::Admit(const FarVolumePageBudget& budget,
                              FarVolumePageWork& output) const noexcept {
    FarVolumeRequest first, last;
    if (!Page(0, first) || !Page(count_ - 1, last))
        return false;
    const auto tier = FarTierAt(request_.key.tier);
    if (!tier)
        return false;
    const auto extent = count_ > 2 ? stride_
                                   : std::max(first.span.max_y - first.span.min_y,
                                              last.span.max_y - last.span.min_y);
    const auto layers = static_cast<std::uint64_t>(extent / tier->brick_edge_meters);
    const auto across =
        static_cast<std::uint64_t>(tier->tile_edge_meters / tier->brick_edge_meters);
    const auto horizontal = across * (kFarVolumeBrickSide - 1);
    const auto vertical = layers * (kFarVolumeBrickSide - 1);
    FarVolumePageWork work;
    // All products are bounded by 4096/16 layers and the tier table's 32x32
    // candidates per layer. No request-sized iteration or storage is needed.
    work.candidates = across * across * layers;
    work.generation_samples = work.candidates * kFarVolumeBrickSamples;
    work.cells = work.candidates * 64;
    work.gradient_cache_entries = (horizontal + 1) * (horizontal + 1) * (vertical + 1);
    // A rectangular superset of resident lattice samples plus the six axial
    // one-sample halos. Corners not requested by central differences overbound.
    work.sample_cache_entries = (horizontal + 3) * (horizontal + 3) * (vertical + 3);
    work.mesh_samples = work.sample_cache_entries;
    work.height_samples = (horizontal + 3) * (horizontal + 3);
    work.density_samples = work.generation_samples + work.mesh_samples;
    work.vertices = 2 * horizontal * (horizontal + 1) * (vertical + 1) +
                    vertical * (horizontal + 1) * (horizontal + 1);
    work.edge_cache_entries = work.vertices;
    work.indices = work.cells * 15;
    const auto tile_bytes = work.candidates * sizeof(FarVolumeBrick);
    const auto mesh_bytes =
        work.vertices * sizeof(FarVolumeVertex) + work.indices * sizeof(std::uint32_t);
    work.buffer_bytes = 2 * (tile_bytes + mesh_bytes) +
                        2 * work.candidates * sizeof(const FarVolumeBrick*) +
                        work.height_samples * sizeof(std::optional<float>);
    const auto cache_entries =
        work.sample_cache_entries + work.gradient_cache_entries + work.edge_cache_entries;
    if (work.candidates > budget.max_candidates ||
        work.density_samples + work.height_samples > budget.max_sample_calls ||
        work.buffer_bytes > budget.max_buffer_bytes || cache_entries > budget.max_cache_entries ||
        work.candidates > std::numeric_limits<std::size_t>::max() / sizeof(FarVolumeBrick) ||
        work.vertices > std::numeric_limits<std::size_t>::max() / sizeof(FarVolumeVertex) ||
        work.indices > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        return false;
    output = work;
    return true;
}

bool FarVolumePageCompleteness::Acknowledge(const FarVolumeRequest& page) noexcept {
    FarVolumeRequest expected;
    if (!plan_.Page(next_, expected) || page != expected)
        return false;
    ++next_;
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
    return !extra_ || (static_cast<double>(built.min_y) <= static_cast<double>(extra_->min_y) &&
                       static_cast<double>(built.max_y) >= static_cast<double>(extra_->max_y));
}

} // namespace Luminumbra::World
