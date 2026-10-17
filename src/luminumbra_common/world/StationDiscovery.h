#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../systems/SHIELD_WorldSystem.h"

namespace Luminumbra::world {

inline constexpr std::int64_t kStationHorizontalBoundMm = 8388608000LL;
inline constexpr std::uint64_t kStationCellBudget = 262144;
inline constexpr std::uint64_t kStationSampleBudget = 16777216;
// Counts every visited Z/X/Y lattice anchor, including rejected footprint points.
// Independent of density sampling, so skipped anchors cannot evade the work bound.
inline constexpr std::uint64_t kStationLatticeVisitBudget = 16777216;

enum class StationKind {
    Mouth,
    Arch,
    Horizon,
    Coverage
};
struct StationQuery {
    StationKind kind = StationKind::Mouth;
    std::array<std::int64_t, 2> origin_mm{};
    int radius_m = 1;
    std::array<int, 2> y_range_m{};
    int view_distance_m = 0;
    int limit = 1;
};
struct StationPose {
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
};
struct StationCandidate {
    std::string id;
    std::array<std::int64_t, 3> anchor_mm{};
    StationPose pose;
    int cell_x = 0, cell_z = 0;
    std::uint64_t local_index = 0;
};
struct StationDiscoveryResult {
    bool ok = false;
    std::string error;
    std::vector<StationCandidate> candidates;
    std::uint64_t enumerated_cells = 0, density_samples = 0;
    // Read-only work diagnostic, retained on refusal; never exceeds the budget.
    std::uint64_t lattice_visits = 0;
};

// Read-only seams also let tests describe topology without seeded numeric goldens.
struct StationDiscoverySource {
    float cell_size_m = 0; // zero means the surface-break catalogue is disabled
    float max_radius_m = 0;
    std::function<Systems::SHIELD_WorldSystem::SurfaceBreakInfo(int, int)> cell;
    std::function<float(float, float)> height;
    std::function<float(const Vec3&)> density;
};

std::string ValidateStationQuery(const StationQuery& query);
// Acquires one worldgen epoch for both the catalogue snapshot and all sampling.
// Call without already holding the world's non-recursive sample scope.
StationDiscoveryResult DiscoverStations(const Systems::SHIELD_WorldSystem& world,
                                        const StationQuery& query);
StationDiscoveryResult DiscoverStations(const StationDiscoverySource& source,
                                        const StationQuery& query);
// Distance in integer millimetres, then signed cell Z/X, then local lattice index.
void RankStationCandidates(std::vector<StationCandidate>& candidates, const StationQuery& query);

} // namespace Luminumbra::world
