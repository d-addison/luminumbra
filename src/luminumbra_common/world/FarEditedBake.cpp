#include "FarEditedBake.h"

#include "FarVolumeRuntime.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Luminumbra::World {
namespace {

using Error = FarEditedBakeError;
constexpr std::uint64_t kPresentBytes = 16 + 3 * kFsv1SamplesPerBrick;

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) {
    const auto quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}

std::array<std::int64_t, 3> Axes(const FarVolumePosition& p) {
    return {p.x, p.y, p.z};
}

Error CheckFootprint(const FarEditedFootprint& footprint) {
    const auto lo = Axes(footprint.min), hi = Axes(footprint.max);
    for (std::size_t axis = 0; axis < lo.size(); ++axis) {
        if (lo[axis] > hi[axis])
            return Error::InvalidFootprint;
        if (lo[axis] <= -kFarVolumeExactCoordinateLimitMeters ||
            hi[axis] >= kFarVolumeExactCoordinateLimitMeters)
            return Error::CoordinateRange;
    }
    return Error::None;
}

bool Contains(const FarEditedFootprint& footprint, const FarVolumePosition& p) {
    return p.x >= footprint.min.x && p.x <= footprint.max.x && p.y >= footprint.min.y &&
           p.y <= footprint.max.y && p.z >= footprint.min.z && p.z <= footprint.max.z;
}

bool Intersects(const FarEditedFootprint& a, const FarEditedFootprint& b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y &&
           a.min.z <= b.max.z && b.min.z <= a.max.z;
}

// The halo is six displaced supports, not a diagonal expansion of the box.
bool SupportIntersects(const FarEditedFootprint& edit,
                       const FarEditedFootprint& support,
                       std::int64_t spacing) {
    if (Intersects(edit, support))
        return true;
    for (int axis = 0; axis < 3; ++axis) {
        for (const auto offset : {-spacing, spacing}) {
            auto halo = support;
            if (axis == 0) {
                halo.min.x += offset;
                halo.max.x += offset;
            } else if (axis == 1) {
                halo.min.y += offset;
                halo.max.y += offset;
            } else {
                halo.min.z += offset;
                halo.max.z += offset;
            }
            if (Intersects(edit, halo))
                return true;
        }
    }
    return false;
}

struct BrickRange {
    std::array<std::int64_t, 3> lo, hi;
};

BrickRange Range(const FarEditedFootprint& edit, const FarTierDimensions& tier) {
    BrickRange result;
    const auto lo = Axes(edit.min), hi = Axes(edit.max);
    for (std::size_t axis = 0; axis < lo.size(); ++axis) {
        // Closed intersection admits the brick ending exactly at the lower bound.
        result.lo[axis] =
            FloorDiv(lo[axis] - tier.sample_spacing_meters - 1, tier.brick_edge_meters);
        result.hi[axis] = FloorDiv(hi[axis] + tier.sample_spacing_meters, tier.brick_edge_meters);
    }
    return result;
}

bool Add(std::uint64_t amount, std::uint64_t limit, std::uint64_t& used) {
    if (used > limit || amount > limit - used)
        return false;
    used += amount;
    return true;
}

bool AddProduct(std::uint64_t count, std::uint64_t size, std::uint64_t limit, std::uint64_t& used) {
    return size == 0 || (count <= limit / size && Add(count * size, limit, used));
}

// A core box plus disjoint axis slabs: normal halos never add diagonal corners.
// Cap each product before multiplying, including when the full volume would
// overflow uint64_t. Zero-width slabs contribute nothing.
template<std::size_t N>
bool CrossCount(const std::array<std::uint64_t, N>& core,
                const std::array<std::uint64_t, N>& expanded,
                std::uint64_t cap,
                std::uint64_t& count) {
    count = 0;
    for (std::size_t slab = 0; slab <= N; ++slab) {
        if (slab < N && expanded[slab] == core[slab])
            continue;
        std::uint64_t product = 1;
        for (std::size_t axis = 0; axis < N; ++axis) {
            const auto extent = axis == slab ? expanded[axis] - core[axis] : core[axis];
            if (product > cap / extent)
                return false;
            product *= extent;
        }
        if (!Add(product, cap, count))
            return false;
    }
    return true;
}

bool FootprintCounts(const FarEditedFootprint& edit,
                     const FarTierDimensions& tier,
                     const FarEditedBakeLimits& limits,
                     std::uint64_t& bricks,
                     std::uint64_t& tiles) {
    const auto lo = Axes(edit.min), hi = Axes(edit.max);
    const auto expanded = Range(edit, tier);
    BrickRange core;
    std::array<std::uint64_t, 3> core_bricks{}, expanded_bricks{};
    for (std::size_t axis = 0; axis < lo.size(); ++axis) {
        core.lo[axis] = FloorDiv(lo[axis] - 1, tier.brick_edge_meters);
        core.hi[axis] = FloorDiv(hi[axis], tier.brick_edge_meters);
        core_bricks[axis] = static_cast<std::uint64_t>(core.hi[axis] - core.lo[axis] + 1);
        expanded_bricks[axis] =
            static_cast<std::uint64_t>(expanded.hi[axis] - expanded.lo[axis] + 1);
    }
    std::array<std::uint64_t, 2> core_tiles{}, expanded_tiles{};
    for (std::size_t i = 0; i < core_tiles.size(); ++i) {
        const auto axis = 2 * i; // Horizontal projection onto X/Z tile columns.
        core_tiles[i] =
            static_cast<std::uint64_t>(FloorDiv(core.hi[axis], kFsv1BricksPerTileEdge) -
                                       FloorDiv(core.lo[axis], kFsv1BricksPerTileEdge) + 1);
        expanded_tiles[i] =
            static_cast<std::uint64_t>(FloorDiv(expanded.hi[axis], kFsv1BricksPerTileEdge) -
                                       FloorDiv(expanded.lo[axis], kFsv1BricksPerTileEdge) + 1);
    }
    return CrossCount(core_bricks, expanded_bricks, limits.max_affected_bricks, bricks) &&
           CrossCount(core_tiles, expanded_tiles, limits.max_tiles, tiles);
}

std::uint64_t AffectedStorage(const FarEditedAffectedSet& set) {
    return static_cast<std::uint64_t>(set.bricks.capacity()) * sizeof(FarEditedBrickKey) +
           static_cast<std::uint64_t>(set.tiles.capacity()) * sizeof(FarTileKey);
}

Error ComputeAffected(std::span<const FarEditedFootprint> edits,
                      const FarEditedBakeLimits& limits,
                      FarEditedAffectedSet& out,
                      std::array<std::uint64_t, kFarTierCount>* work = nullptr,
                      FarEditedAffectedWork* observed = nullptr) {
    if (edits.size() > limits.max_edits)
        return Error::WorkLimit;
    std::array<std::uint64_t, kFarTierCount> visits{};
    std::array<std::uint64_t, kFarTierCount> brick_bounds{}, tile_bounds{};
    std::uint64_t min_bricks = 0, min_tiles = 0;
    for (const auto& edit : edits) {
        const auto error = CheckFootprint(edit);
        if (error != Error::None)
            return error;
        for (std::size_t t = 0; t < kFarTierCount; ++t) {
            // The largest single footprint in each tier is a lower bound on
            // that tier's union. Tiers are disjoint, so their bounds add, while
            // overlapping/duplicate edits must not be charged twice here.
            std::uint64_t bricks = 0, tiles = 0;
            if (!FootprintCounts(edit, kFarTierTable[t], limits, bricks, tiles))
                return Error::WorkLimit;
            if (bricks > brick_bounds[t]) {
                if (!Add(bricks - brick_bounds[t], limits.max_affected_bricks, min_bricks))
                    return Error::WorkLimit;
                brick_bounds[t] = bricks;
            }
            if (tiles > tile_bounds[t]) {
                if (!Add(tiles - tile_bounds[t], limits.max_tiles, min_tiles))
                    return Error::WorkLimit;
                tile_bounds[t] = tiles;
            }
            const auto range = Range(edit, kFarTierTable[t]);
            std::uint64_t count = 1;
            for (std::size_t axis = 0; axis < range.lo.size(); ++axis) {
                const auto extent = static_cast<std::uint64_t>(range.hi[axis] - range.lo[axis] + 1);
                if (count > limits.max_brick_visits_per_tier[t] / extent)
                    return Error::WorkLimit;
                count *= extent;
            }
            if (!Add(count, limits.max_brick_visits_per_tier[t], visits[t]))
                return Error::WorkLimit;
        }
    }
    std::uint64_t brick_capacity = 0;
    for (const auto count : visits) {
        // Saturate at admitted unique storage without summing candidate counts
        // into an overflow or retaining duplicates in the candidate buffer.
        brick_capacity += std::min(count, limits.max_affected_bricks - brick_capacity);
    }
    const auto tile_capacity = std::min<std::uint64_t>(brick_capacity, limits.max_tiles);
    std::uint64_t bytes = 0;
    if (!AddProduct(brick_capacity, sizeof(FarEditedBrickKey), limits.max_scratch_bytes, bytes) ||
        !AddProduct(tile_capacity, sizeof(FarTileKey), limits.max_scratch_bytes, bytes))
        return Error::MemoryLimit;
    FarEditedAffectedSet result;
    if (observed)
        observed->reserved_brick_slots = static_cast<std::size_t>(brick_capacity);
    result.bricks.reserve(static_cast<std::size_t>(brick_capacity));
    if (observed)
        observed->reserved_tile_slots = static_cast<std::size_t>(tile_capacity);
    result.tiles.reserve(static_cast<std::size_t>(tile_capacity));
    for (std::size_t t = 0; t < kFarTierCount; ++t) {
        const auto& tier = kFarTierTable[t];
        const auto edge = static_cast<std::int64_t>(tier.brick_edge_meters);
        for (const auto& edit : edits) {
            const auto range = Range(edit, tier);
            for (auto bz = range.lo[2]; bz <= range.hi[2]; ++bz) {
                for (auto bx = range.lo[0]; bx <= range.hi[0]; ++bx) {
                    const FarTileKey tile{static_cast<std::uint8_t>(t + 1),
                                          FloorDiv(bx, kFsv1BricksPerTileEdge),
                                          FloorDiv(bz, kFsv1BricksPerTileEdge)};
                    for (auto by = range.lo[1]; by <= range.hi[1]; ++by) {
                        if (observed)
                            ++observed->candidate_visits_per_tier[t];
                        const FarEditedFootprint support{
                            {bx * edge, by * edge, bz * edge},
                            {(bx + 1) * edge, (by + 1) * edge, (bz + 1) * edge}};
                        if (!SupportIntersects(edit, support, tier.sample_spacing_meters))
                            continue;
                        FarVolumeTileKey checked;
                        if (!ToFarVolumeTileKey(tile, checked) || by < -kFsv1MaxAbsBrickY ||
                            by > kFsv1MaxAbsBrickY ||
                            support.min.y - tier.sample_spacing_meters <=
                                -kFarVolumeExactCoordinateLimitMeters ||
                            support.max.y + tier.sample_spacing_meters >=
                                kFarVolumeExactCoordinateLimitMeters)
                            return Error::CoordinateRange;
                        const FarEditedBrickKey brick{tile,
                                                      static_cast<std::int32_t>(bx),
                                                      static_cast<std::int32_t>(by),
                                                      static_cast<std::int32_t>(bz)};
                        const auto brick_it =
                            std::lower_bound(result.bricks.begin(), result.bricks.end(), brick);
                        if (brick_it != result.bricks.end() && *brick_it == brick)
                            continue;
                        if (result.bricks.size() == limits.max_affected_bricks)
                            return Error::WorkLimit;
                        const auto tile_it =
                            std::lower_bound(result.tiles.begin(), result.tiles.end(), tile);
                        const bool new_tile = tile_it == result.tiles.end() || *tile_it != tile;
                        if (new_tile && result.tiles.size() == limits.max_tiles)
                            return Error::WorkLimit;
                        // Sorted vectors provide bounded unique storage with no
                        // auxiliary nodes/buckets; their final order is canonical.
                        result.bricks.insert(brick_it, brick);
                        if (new_tile)
                            result.tiles.insert(tile_it, tile);
                        if (observed) {
                            observed->unique_bricks = result.bricks.size();
                            observed->unique_tiles = result.tiles.size();
                        }
                    }
                }
            }
        }
    }
    if (AffectedStorage(result) > limits.max_scratch_bytes)
        return Error::MemoryLimit;
    if (work)
        *work = visits;
    out = std::move(result);
    return Error::None;
}

Error LatticeFootprint(const FarEditedLattice& record, FarEditedFootprint& out) {
    if (record.spacing_meters == 0)
        return Error::InvalidLattice;
    const auto origin = Axes(record.origin);
    std::array<std::int64_t, 3> hi{};
    for (std::size_t axis = 0; axis < hi.size(); ++axis) {
        if (record.side[axis] < 2)
            return Error::InvalidLattice;
        if (origin[axis] <= -kFarVolumeExactCoordinateLimitMeters ||
            origin[axis] >= kFarVolumeExactCoordinateLimitMeters)
            return Error::CoordinateRange;
        const auto extent =
            static_cast<std::uint64_t>(record.side[axis] - 1) * record.spacing_meters;
        if (extent >=
            static_cast<std::uint64_t>(kFarVolumeExactCoordinateLimitMeters - origin[axis]))
            return Error::CoordinateRange;
        hi[axis] = origin[axis] + static_cast<std::int64_t>(extent);
    }
    out = {record.origin, {hi[0], hi[1], hi[2]}};
    return Error::None;
}

FarVolumeSample Interpolate(const FarEditedLattice& record, const FarVolumePosition& position) {
    const auto p = Axes(position), origin = Axes(record.origin);
    std::array<std::size_t, 3> base{}, nearest{};
    std::array<double, 3> fraction{};
    for (std::size_t axis = 0; axis < p.size(); ++axis) {
        const auto offset = p[axis] - origin[axis];
        base[axis] = std::min<std::size_t>(static_cast<std::size_t>(offset / record.spacing_meters),
                                           record.side[axis] - 2);
        fraction[axis] = static_cast<double>(offset - static_cast<std::int64_t>(base[axis]) *
                                                          record.spacing_meters) /
                         record.spacing_meters;
        nearest[axis] = base[axis] + (fraction[axis] > 0.5 ? 1u : 0u);
    }
    const auto index = [&](std::size_t x, std::size_t y, std::size_t z) {
        return x + static_cast<std::size_t>(record.side[0]) *
                       (y + static_cast<std::size_t>(record.side[1]) * z);
    };
    double density = 0;
    for (std::size_t z = 0; z < 2; ++z) {
        for (std::size_t y = 0; y < 2; ++y) {
            for (std::size_t x = 0; x < 2; ++x) {
                const double weight = (x ? fraction[0] : 1 - fraction[0]) *
                                      (y ? fraction[1] : 1 - fraction[1]) *
                                      (z ? fraction[2] : 1 - fraction[2]);
                density += weight * record.density[index(base[0] + x, base[1] + y, base[2] + z)];
            }
        }
    }
    const auto material = record.material.empty()
                              ? std::uint8_t{0}
                              : record.material[index(nearest[0], nearest[1], nearest[2])];
    return {static_cast<float>(density), material};
}

Error ResolveEdited(std::span<const FarEditedLattice> records,
                    std::span<const FarEditedFootprint> footprints,
                    const FarVolumePosition& position,
                    std::optional<FarVolumeSample>& out) {
    out.reset();
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (!Contains(footprints[i], position))
            continue;
        const auto sample = Interpolate(records[i], position);
        if (!std::isfinite(sample.density))
            return Error::NonFiniteDensity;
        if (out && *out != sample)
            return Error::BoundaryConflict;
        out = sample;
    }
    return Error::None;
}

void ExtendSpan(FarEditedBakedTile& tile, const FarEditedBrickKey& brick) {
    const auto edge =
        static_cast<std::int64_t>(kFarTierTable[brick.tile.tier - 1].brick_edge_meters);
    const FarVolumeSpan span{brick.by * edge, (static_cast<std::int64_t>(brick.by) + 1) * edge};
    if (!tile.edited_span)
        tile.edited_span = span;
    else {
        tile.edited_span->min_y = std::min(tile.edited_span->min_y, span.min_y);
        tile.edited_span->max_y = std::max(tile.edited_span->max_y, span.max_y);
    }
}

Error Bake(const FarEditedSourceView& source,
           std::span<const FarEditedFootprint> changed,
           const FarEditedPristineSampler& pristine,
           const FarEditedBakeLimits& limits,
           FarEditedBakeResult& out) {
    const auto identity = source.Identity();
    if (identity.state != FarEditedImageState::Prepared &&
        identity.state != FarEditedImageState::Committed)
        return Error::IdentityMismatch;
    const auto records = source.Lattices();
    if (records.size() > limits.max_source_records)
        return Error::WorkLimit;
    FarEditedBakeResult result;
    result.source = identity;
    std::array<std::uint64_t, kFarTierCount> changed_work{};
    auto error = ComputeAffected(changed, limits, result.affected, &changed_work);
    if (error != Error::None)
        return error;
    std::uint64_t scratch_bytes = AffectedStorage(result.affected);
    if (!AddProduct(
            records.size(), sizeof(FarEditedFootprint), limits.max_scratch_bytes, scratch_bytes))
        return Error::MemoryLimit;
    std::vector<FarEditedFootprint> footprints;
    footprints.reserve(records.size());
    std::uint64_t source_samples = 0;
    for (const auto& record : records) {
        FarEditedFootprint footprint;
        error = LatticeFootprint(record, footprint);
        if (error != Error::None)
            return error;
        std::uint64_t count = 1;
        for (const auto side : record.side) {
            if (count > limits.max_source_samples / side)
                return Error::WorkLimit;
            count *= side;
        }
        if (!Add(count, limits.max_source_samples, source_samples))
            return Error::WorkLimit;
        if (count != record.density.size())
            return Error::InvalidLattice;
        if (!record.material.empty() && record.material.size() != record.density.size())
            return Error::InvalidMaterial;
        if (record.member_generation > identity.generation)
            return Error::IdentityMismatch;
        footprints.push_back(footprint);
    }
    scratch_bytes = AffectedStorage(result.affected) +
                    static_cast<std::uint64_t>(footprints.capacity()) * sizeof(FarEditedFootprint);
    if (scratch_bytes > limits.max_scratch_bytes)
        return Error::MemoryLimit;
    auto inventory_limits = limits;
    inventory_limits.max_edits = limits.max_source_records;
    inventory_limits.max_scratch_bytes -= scratch_bytes;
    for (std::size_t t = 0; t < kFarTierCount; ++t)
        inventory_limits.max_brick_visits_per_tier[t] -= changed_work[t];
    FarEditedAffectedSet inventory;
    error = ComputeAffected(footprints, inventory_limits, inventory);
    if (error != Error::None)
        return error;

    // Inventory bricks outside due tiles do not consume sampling/output work.
    inventory.bricks.erase(std::remove_if(inventory.bricks.begin(),
                                          inventory.bricks.end(),
                                          [&](const auto& brick) {
                                              return !std::binary_search(
                                                  result.affected.tiles.begin(),
                                                  result.affected.tiles.end(),
                                                  brick.tile);
                                          }),
                           inventory.bricks.end());
    std::uint64_t samples = 0, probes = 0, output_bytes = 0;
    if (!AddProduct(
            inventory.bricks.size(), kFsv1SamplesPerBrick, limits.max_density_samples, samples) ||
        !AddProduct(source_samples, records.size(), limits.max_source_probes, probes) ||
        !AddProduct(samples, records.size(), limits.max_source_probes, probes) ||
        !AddProduct(result.affected.tiles.size(), records.size(), limits.max_source_probes, probes))
        return Error::WorkLimit;
    if (!AddProduct(result.affected.tiles.size(),
                    sizeof(FarEditedBakedTile) + kFsv1HeaderBytes + kFsv1TrailerBytes,
                    limits.max_output_bytes,
                    output_bytes) ||
        !AddProduct(inventory.bricks.size(),
                    sizeof(FarOverlayBrick) + kPresentBytes,
                    limits.max_output_bytes,
                    output_bytes))
        return Error::MemoryLimit;
    if (!pristine && !inventory.bricks.empty())
        return Error::InvalidSampler;
    std::size_t tile_bricks = 0;
    std::optional<FarTileKey> previous_key;
    for (const auto& brick : inventory.bricks) {
        if (!previous_key || *previous_key != brick.tile) {
            previous_key = brick.tile;
            tile_bricks = 0;
        }
        if (++tile_bricks > kFsv1MaxBricks)
            return Error::WorkLimit;
    }

    // Validate even non-sampled source values and every shared authoritative node.
    for (const auto& record : records) {
        for (const auto value : record.density) {
            if (!std::isfinite(value))
                return Error::NonFiniteDensity;
        }
    }
    for (const auto& record : records) {
        for (std::uint32_t z = 0; z < record.side[2]; ++z) {
            for (std::uint32_t y = 0; y < record.side[1]; ++y) {
                for (std::uint32_t x = 0; x < record.side[0]; ++x) {
                    const FarVolumePosition p{
                        record.origin.x + static_cast<std::int64_t>(x) * record.spacing_meters,
                        record.origin.y + static_cast<std::int64_t>(y) * record.spacing_meters,
                        record.origin.z + static_cast<std::int64_t>(z) * record.spacing_meters};
                    std::optional<FarVolumeSample> sample;
                    error = ResolveEdited(records, footprints, p, sample);
                    if (error != Error::None)
                        return error;
                }
            }
        }
    }

    result.tiles.reserve(result.affected.tiles.size());
    auto next_brick = inventory.bricks.begin();
    auto next_changed = result.affected.bricks.begin();
    for (const auto& key : result.affected.tiles) {
        FarEditedBakedTile tile;
        tile.overlay.key = key;
        tile.overlay.generation = identity.generation;
        const auto end_brick = std::find_if(next_brick,
                                            inventory.bricks.end(),
                                            [&](const auto& brick) { return brick.tile != key; });
        const auto count = static_cast<std::size_t>(end_brick - next_brick);
        if (count > kFsv1MaxBricks)
            return Error::WorkLimit;
        tile.overlay.bricks.reserve(count);
        while (next_changed != result.affected.bricks.end() && next_changed->tile == key)
            ExtendSpan(tile, *next_changed++);
        const auto& tier = kFarTierTable[key.tier - 1];
        const auto edge = static_cast<std::int64_t>(tier.brick_edge_meters);
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto tile_edge = static_cast<std::int64_t>(tier.tile_edge_meters);
            const FarEditedFootprint support{
                {key.tx * tile_edge, footprints[i].min.y, key.tz * tile_edge},
                {(key.tx + 1) * tile_edge, footprints[i].max.y, (key.tz + 1) * tile_edge}};
            if (SupportIntersects(footprints[i], support, tier.sample_spacing_meters))
                tile.max_member_generation =
                    std::max(tile.max_member_generation, records[i].member_generation);
        }
        for (; next_brick != end_brick; ++next_brick) {
            ExtendSpan(tile, *next_brick);
            FarOverlayBrick brick;
            brick.bx = next_brick->bx;
            brick.by = next_brick->by;
            brick.bz = next_brick->bz;
            std::size_t index = 0;
            for (std::int64_t z = 0; z <= edge; z += tier.sample_spacing_meters) {
                for (std::int64_t y = 0; y <= edge; y += tier.sample_spacing_meters) {
                    for (std::int64_t x = 0; x <= edge; x += tier.sample_spacing_meters) {
                        const FarVolumePosition p{
                            brick.bx * edge + x, brick.by * edge + y, brick.bz * edge + z};
                        std::optional<FarVolumeSample> sample;
                        error = ResolveEdited(records, footprints, p, sample);
                        if (error != Error::None)
                            return error;
                        if (!sample)
                            sample = pristine(key.tier, p);
                        if (!sample)
                            return Error::SamplerFailure;
                        if (!std::isfinite(sample->density))
                            return Error::NonFiniteDensity;
                        brick.density[index] = QuantizeFarDensity(sample->density);
                        brick.material[index++] = sample->material;
                    }
                }
            }
            brick.state = ClassifyFarBrick(brick.density);
            if (brick.state != FarBrickState::Present) {
                brick.density.fill(0);
                brick.material.fill(0);
            }
            tile.overlay.bricks.push_back(brick);
        }
        if (EncodeFarOverlay(tile.overlay, tile.bytes) != FarOverlayError::None)
            return Error::EncodingFailure;
        result.tiles.push_back(std::move(tile));
    }
    out = std::move(result);
    return Error::None;
}

} // namespace

bool operator<(const FarEditedBrickKey& a, const FarEditedBrickKey& b) {
    if (a.tile != b.tile)
        return a.tile < b.tile;
    return std::tie(a.bz, a.bx, a.by) < std::tie(b.bz, b.bx, b.by);
}

FarEditedBakeError ComputeFarEditedAffectedSet(std::span<const FarEditedFootprint> edits,
                                               const FarEditedBakeLimits& limits,
                                               FarEditedAffectedSet& out,
                                               FarEditedAffectedWork* work) {
    if (work)
        *work = {};
    try {
        return ComputeAffected(edits, limits, out, nullptr, work);
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    } catch (const std::length_error&) {
        return Error::AllocationFailure;
    }
}

FarEditedBakeError BakeFarEditedTiles(const FarEditedSourceView& source,
                                      std::span<const FarEditedFootprint> changed,
                                      const FarEditedPristineSampler& pristine,
                                      const FarEditedBakeLimits& limits,
                                      FarEditedBakeResult& out) {
    try {
        return Bake(source, changed, pristine, limits, out);
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    } catch (const std::length_error&) {
        return Error::AllocationFailure;
    } catch (...) {
        return Error::SamplerFailure;
    }
}

} // namespace Luminumbra::World
