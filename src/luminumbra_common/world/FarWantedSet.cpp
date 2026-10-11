#include "FarWantedSet.h"

#include "FarTierTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Luminumbra::World {

namespace {

constexpr bool IsValidTier(std::size_t tier) {
    return tier >= 1 && tier <= kFarTierCount;
}

constexpr std::size_t TierOf(const FarTileKey& key) {
    return static_cast<std::size_t>(key.tier);
}

// Floor division by two; correct for negative values (-1 -> -1, -2 -> -1, -3 -> -2).
constexpr std::int64_t FloorDivTwo(std::int64_t value) {
    std::int64_t quotient = value / 2;
    if (value % 2 != 0 && value < 0) {
        --quotient;
    }
    return quotient;
}

std::uint64_t ResidentBytes(const FarResidentTile& entry) {
    return entry.cpu_bytes + entry.gpu_bytes;
}

const FarResidentTile* FindResident(std::span<const FarResidentTile> resident,
                                    const FarTileKey& key) {
    const auto it = std::find_if(resident.begin(),
                                 resident.end(),
                                 [&key](const FarResidentTile& entry) { return entry.key == key; });
    return it == resident.end() ? nullptr : &*it;
}

// A tile paired with its camera distance so the comparators do not recompute it.
struct RankedTile {
    FarTileKey key;
    double distance = 0.0;
    std::uint64_t bytes = 0;
};

// Tier descending, then distance ascending, then key ascending.
bool BuildRankBefore(const RankedTile& a, const RankedTile& b) {
    if (a.key.tier != b.key.tier) {
        return a.key.tier > b.key.tier;
    }
    if (a.distance != b.distance) {
        return a.distance < b.distance;
    }
    return a.key < b.key;
}

// Distance descending, then key ascending.
bool EvictRankBefore(const RankedTile& a, const RankedTile& b) {
    if (a.distance != b.distance) {
        return a.distance > b.distance;
    }
    return a.key < b.key;
}

// Ordering needs a strict weak order, so a non-finite camera coordinate is
// treated as the origin instead of producing NaN distances.
double FiniteOrZero(double value) {
    return std::isfinite(value) ? value : 0.0;
}

RankedTile Rank(const FarTileKey& key, double camera_x, double camera_z, std::uint64_t bytes = 0) {
    return RankedTile{
        key, FarTileNearestDistance(key, FiniteOrZero(camera_x), FiniteOrZero(camera_z)), bytes};
}

// Whether a resident tile needs a build started under the SelectFarBuilds rule.
bool NeedsBuild(const FarTileKey& key, std::span<const FarResidentTile> resident) {
    const FarResidentTile* entry = FindResident(resident, key);
    if (entry == nullptr) {
        return true;
    }
    switch (entry->status.state) {
        case FarTileState::Building:
        case FarTileState::Rejected:
            return false;
        case FarTileState::Wanted:
            return true;
        case FarTileState::Ready:
        case FarTileState::Uploaded:
        case FarTileState::Empty:
            return !entry->status.current || !entry->status.span_ok;
    }
    return false;
}

} // namespace

double FarWantedRadiusMeters(std::size_t tier) {
    const auto dims = FarTierAt(tier);
    if (!dims.has_value()) {
        return 0.0;
    }
    const auto outer = static_cast<double>(dims->outer_radius_meters);
    if (tier < kFarTierCount) {
        return outer + static_cast<double>(dims->brick_edge_meters);
    }
    return outer;
}

std::int64_t FarTileEdgeMeters(std::size_t tier) {
    const auto dims = FarTierAt(tier);
    if (!dims.has_value()) {
        return 0;
    }
    return static_cast<std::int64_t>(dims->tile_edge_meters);
}

double FarTileNearestDistance(const FarTileKey& tile, double x, double z) {
    const std::size_t tier = TierOf(tile);
    if (!IsValidTier(tier)) {
        return std::numeric_limits<double>::infinity();
    }
    const auto edge = static_cast<double>(FarTileEdgeMeters(tier));
    const double lo_x = static_cast<double>(tile.tx) * edge;
    const double hi_x = lo_x + edge;
    const double lo_z = static_cast<double>(tile.tz) * edge;
    const double hi_z = lo_z + edge;
    const double dx = std::max({lo_x - x, 0.0, x - hi_x});
    const double dz = std::max({lo_z - z, 0.0, z - hi_z});
    return std::sqrt(dx * dx + dz * dz);
}

FarTileKey FarTileParent(const FarTileKey& tile) {
    const std::size_t tier = TierOf(tile);
    if (!IsValidTier(tier) || tier == kFarTierCount) {
        return tile;
    }
    FarTileKey parent;
    parent.tier = static_cast<std::uint8_t>(tier + 1);
    parent.tx = FloorDivTwo(tile.tx);
    parent.tz = FloorDivTwo(tile.tz);
    return parent;
}

FarWantedError ComputeFarWantedSet(double camera_x, double camera_z, std::vector<FarTileKey>& out) {
    out.clear();
    if (!std::isfinite(camera_x) || !std::isfinite(camera_z)) {
        return FarWantedError::NonFiniteCamera;
    }
    if (std::abs(camera_x) > kFarWantedCameraLimitMeters ||
        std::abs(camera_z) > kFarWantedCameraLimitMeters) {
        return FarWantedError::CameraOutOfRange;
    }

    for (std::size_t tier = 1; tier <= kFarTierCount; ++tier) {
        const double edge = static_cast<double>(FarTileEdgeMeters(tier));
        const double radius = FarWantedRadiusMeters(tier);

        const auto x_lo = static_cast<std::int64_t>(std::floor((camera_x - radius) / edge)) - 1;
        const auto x_hi = static_cast<std::int64_t>(std::floor((camera_x + radius) / edge)) + 1;
        const auto z_lo = static_cast<std::int64_t>(std::floor((camera_z - radius) / edge)) - 1;
        const auto z_hi = static_cast<std::int64_t>(std::floor((camera_z + radius) / edge)) + 1;

        for (std::int64_t tz = z_lo; tz <= z_hi; ++tz) {
            for (std::int64_t tx = x_lo; tx <= x_hi; ++tx) {
                FarTileKey candidate;
                candidate.tier = static_cast<std::uint8_t>(tier);
                candidate.tx = tx;
                candidate.tz = tz;
                if (FarTileNearestDistance(candidate, camera_x, camera_z) <= radius) {
                    out.push_back(candidate);
                }
            }
        }
    }

    std::sort(out.begin(), out.end());
    return FarWantedError::None;
}

bool CoarseSurfaceMayYield(const FarTileKey& coarse,
                           std::span<const FarTileKey> wanted,
                           std::span<const FarResidentTile> resident) {
    const std::size_t coarse_tier = TierOf(coarse);
    if (!IsValidTier(coarse_tier) || coarse_tier < 2) {
        return false;
    }

    constexpr std::int64_t kMaxCoarseCoordinate = std::numeric_limits<std::int64_t>::max() / 2 - 1;
    if (coarse.tx > kMaxCoarseCoordinate || coarse.tx < -kMaxCoarseCoordinate ||
        coarse.tz > kMaxCoarseCoordinate || coarse.tz < -kMaxCoarseCoordinate) {
        return false; // children would not be representable
    }

    // The coarse footprint is replaced only when all four finer tiles that tile it
    // are wanted and covering; a partly wanted footprint keeps the coarse surface.
    for (std::int64_t dz = 0; dz < 2; ++dz) {
        for (std::int64_t dx = 0; dx < 2; ++dx) {
            FarTileKey child;
            child.tier = static_cast<std::uint8_t>(coarse_tier - 1);
            child.tx = coarse.tx * 2 + dx;
            child.tz = coarse.tz * 2 + dz;
            if (std::find(wanted.begin(), wanted.end(), child) == wanted.end()) {
                return false;
            }
            const FarResidentTile* entry = FindResident(resident, child);
            if (entry == nullptr || !IsFarTileCovering(entry->status)) {
                return false;
            }
        }
    }
    return true;
}

FarEvictionPlan PlanFarEviction(double camera_x,
                                double camera_z,
                                std::span<const FarResidentTile> resident,
                                std::span<const FarTileKey> wanted,
                                const FarTierBudgets& budgets) {
    FarEvictionPlan plan;

    std::vector<FarTileKey> sorted_wanted(wanted.begin(), wanted.end());
    std::sort(sorted_wanted.begin(), sorted_wanted.end());
    const auto is_wanted = [&sorted_wanted](const FarTileKey& key) {
        return std::binary_search(sorted_wanted.begin(), sorted_wanted.end(), key);
    };

    // Step 1: resident tiles that are no longer wanted are evicted without
    // counting as budget rejections.
    std::vector<RankedTile> unwanted;
    for (const FarResidentTile& entry : resident) {
        if (!is_wanted(entry.key)) {
            unwanted.push_back(Rank(entry.key, camera_x, camera_z));
        }
    }
    std::sort(unwanted.begin(), unwanted.end(), EvictRankBefore);
    for (const RankedTile& tile : unwanted) {
        plan.evict.push_back(tile.key);
    }

    // Step 2: finest tiers first; evict only tiles whose parent is covering.
    for (std::size_t tier = 1; tier < kFarTierCount; ++tier) {
        std::uint64_t sum = 0;
        for (const FarResidentTile& entry : resident) {
            if (TierOf(entry.key) != tier || entry.status.state == FarTileState::Rejected ||
                !is_wanted(entry.key)) {
                continue;
            }
            sum += ResidentBytes(entry);
        }

        const std::uint64_t budget = budgets.bytes[tier - 1];
        if (budget == 0 || sum <= budget) {
            continue;
        }

        std::vector<RankedTile> candidates;
        for (const FarResidentTile& entry : resident) {
            if (TierOf(entry.key) != tier || entry.status.state == FarTileState::Rejected ||
                !is_wanted(entry.key)) {
                continue;
            }
            const FarTileKey parent = FarTileParent(entry.key);
            const auto parent_covering = std::find_if(
                resident.begin(), resident.end(), [&parent, &plan](const FarResidentTile& other) {
                    return other.key == parent && IsFarTileCovering(other.status) &&
                           std::find(plan.evict.begin(), plan.evict.end(), parent) ==
                               plan.evict.end();
                });
            if (parent_covering != resident.end()) {
                candidates.push_back(Rank(entry.key, camera_x, camera_z, ResidentBytes(entry)));
            }
        }
        std::sort(candidates.begin(), candidates.end(), EvictRankBefore);

        for (const RankedTile& candidate : candidates) {
            if (sum <= budget) {
                break;
            }
            sum -= candidate.bytes;
            plan.evict.push_back(candidate.key);
            plan.rejected.push_back(candidate.key);
        }
    }

    // Step 3: tier 5 is never evicted; overflow is reported instead.
    std::uint64_t horizon_sum = 0;
    for (const FarResidentTile& entry : resident) {
        if (TierOf(entry.key) != kFarTierCount || entry.status.state == FarTileState::Rejected ||
            !is_wanted(entry.key)) {
            continue;
        }
        horizon_sum += ResidentBytes(entry);
    }
    const std::uint64_t horizon_budget = budgets.bytes[kFarTierCount - 1];
    if (horizon_budget != 0 && horizon_sum > horizon_budget) {
        plan.horizon_overflow = true;
    }

    return plan;
}

std::vector<FarTileKey> SelectFarBuilds(double camera_x,
                                        double camera_z,
                                        std::span<const FarTileKey> wanted,
                                        std::span<const FarResidentTile> resident,
                                        std::size_t max_start) {
    std::vector<FarTileKey> result;
    if (max_start == 0) {
        return result;
    }

    std::vector<RankedTile> ranked;
    for (const FarTileKey& key : wanted) {
        if (!IsValidTier(TierOf(key)) || !NeedsBuild(key, resident)) {
            continue;
        }
        ranked.push_back(Rank(key, camera_x, camera_z));
    }
    std::sort(ranked.begin(), ranked.end(), BuildRankBefore);

    const std::size_t count = std::min(max_start, ranked.size());
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(ranked[index].key);
    }
    return result;
}

std::vector<FarTileKey> SelectFarUploads(double camera_x,
                                         double camera_z,
                                         std::span<const FarResidentTile> resident,
                                         std::size_t max_count,
                                         std::uint64_t max_bytes) {
    std::vector<FarTileKey> result;
    if (max_count == 0) {
        return result;
    }

    std::vector<RankedTile> ranked;
    std::vector<std::uint64_t> sizes;
    for (const FarResidentTile& entry : resident) {
        if (entry.status.state != FarTileState::Ready) {
            continue;
        }
        ranked.push_back(Rank(entry.key, camera_x, camera_z));
        sizes.push_back(entry.cpu_bytes);
    }

    // Sort an index permutation so each tile keeps its own byte count.
    std::vector<std::size_t> order(ranked.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&ranked](std::size_t a, std::size_t b) {
        return BuildRankBefore(ranked[a], ranked[b]);
    });

    std::uint64_t total = 0;
    for (const std::size_t index : order) {
        if (result.size() >= max_count) {
            break;
        }
        const std::uint64_t bytes = sizes[index];
        if (!result.empty() && (total > max_bytes || bytes > max_bytes - total)) {
            break;
        }
        result.push_back(ranked[index].key);
        total += bytes;
    }
    return result;
}

std::array<FarTierCounters, kFarTierCount>
AccumulateFarCounters(std::span<const FarResidentTile> resident,
                      std::span<const FarTileKey> wanted) {
    std::array<FarTierCounters, kFarTierCount> counters{};

    for (const FarTileKey& key : wanted) {
        if (IsValidTier(TierOf(key))) {
            ++counters[TierOf(key) - 1].wanted;
        }
    }

    for (const FarResidentTile& entry : resident) {
        if (!IsValidTier(TierOf(entry.key))) {
            continue;
        }
        FarTierCounters& counter = counters[TierOf(entry.key) - 1];
        counter.cpu_bytes += entry.cpu_bytes;
        counter.gpu_bytes += entry.gpu_bytes;
        switch (entry.status.state) {
            case FarTileState::Wanted:
                break;
            case FarTileState::Building:
                ++counter.building;
                break;
            case FarTileState::Ready:
                ++counter.ready;
                break;
            case FarTileState::Uploaded:
                ++counter.uploaded;
                break;
            case FarTileState::Empty:
                ++counter.empty;
                break;
            case FarTileState::Rejected:
                ++counter.rejected;
                break;
        }
    }
    return counters;
}

} // namespace Luminumbra::World
