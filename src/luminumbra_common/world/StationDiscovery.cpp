#include "StationDiscovery.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

#include "../core/DeterministicMath.h"

namespace Luminumbra::world {
namespace {
constexpr float kAirMargin = 0.25f;
constexpr int kStep = 4;
constexpr std::array<std::array<int, 2>, 4> kDirections{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};

std::int64_t Millimetres(double metres) {
    if (!std::isfinite(metres) || std::abs(metres) > 2147483647.0) {
        throw std::runtime_error("nonfinite or unrepresentable world coordinate");
    }
    // Nearest millimetre, halves away from zero; no locale or libm orientation.
    return static_cast<std::int64_t>(metres * 1000.0 + (metres < 0 ? -0.5 : 0.5));
}
std::string SignedCell(int cell) {
    const auto wide = static_cast<std::int64_t>(cell);
    return std::string(wide < 0 ? "n" : "p") + std::to_string(wide < 0 ? -wide : wide);
}
std::string Id(const char* kind, int z, int x, std::uint64_t index) {
    return std::string(kind) + "-z-" + SignedCell(z) + "-x-" + SignedCell(x) + "-i-" +
           std::to_string(index);
}
std::uint64_t Distance(const StationCandidate& candidate, const StationQuery& query) {
    const auto dx = candidate.anchor_mm[0] - query.origin_mm[0];
    const auto dz = candidate.anchor_mm[2] - query.origin_mm[1];
    return static_cast<std::uint64_t>(dx * dx + dz * dz);
}

class Sampler {
public:
    Sampler(const StationDiscoverySource& source,
            const StationQuery& query,
            StationDiscoveryResult& result)
        : m_source(source)
        , m_query(query)
        , m_result(result) {}

    float Density(double x, double y, double z) {
        if (std::abs(x * 1000) > kStationHorizontalBoundMm ||
            std::abs(z * 1000) > kStationHorizontalBoundMm) {
            throw std::runtime_error("sample outside horizontal domain");
        }
        if (y < m_query.y_range_m[0] || y > m_query.y_range_m[1]) {
            throw std::runtime_error("sample outside requested vertical span");
        }
        if (m_result.density_samples == kStationSampleBudget) {
            throw std::runtime_error("density sample budget exhausted");
        }
        ++m_result.density_samples;
        const float value = m_source.density(
            Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
        if (!std::isfinite(value)) {
            throw std::runtime_error("nonfinite density");
        }
        return value;
    }
    bool Air(double x, double y, double z) {
        return Density(x, y, z) >= kAirMargin;
    }
    bool Solid(double x, double y, double z) {
        return Density(x, y, z) <= -kAirMargin;
    }
    bool Exterior(double x, double y, double z) {
        return Air(x, y, z) && Air(x, y + 4, z) && Air(x, y + 8, z) && Air(x, y + 12, z);
    }
    bool Access(double x, double y, double z, int dx, int dz) {
        return Air(x + dx * 4, y, z + dz * 4) && Exterior(x + dx * 8, y, z + dz * 8);
    }
    bool Portal(double x, double y, double z, bool arch) {
        if (!Air(x, y, z) || !Solid(x, y + 4, z)) {
            return false;
        }
        for (const auto& direction : kDirections) {
            const int dx = direction[0], dz = direction[1];
            if (!Access(x, y, z, dx, dz)) {
                continue;
            }
            if (!arch ||
                (Access(x, y, z, -dx, -dz) && Solid(x - dz * 4, y, z + dx * 4) &&
                 Solid(x + dz * 4, y, z - dx * 4) && Solid(x - dz * 4, y + 4, z + dx * 4) &&
                 Solid(x + dz * 4, y + 4, z - dx * 4))) {
                return true;
            }
        }
        return false;
    }
    bool EyeClear(const StationPose& pose) {
        if (pose.y - 0.5 < m_query.y_range_m[0] || pose.y + 0.5 > m_query.y_range_m[1]) {
            return false;
        }
        constexpr std::array<std::array<double, 3>, 7> offsets{{{0, 0, 0},
                                                                {0.5, 0, 0},
                                                                {-0.5, 0, 0},
                                                                {0, 0.5, 0},
                                                                {0, -0.5, 0},
                                                                {0, 0, 0.5},
                                                                {0, 0, -0.5}}};
        for (const auto& offset : offsets) {
            if (!Air(pose.x + offset[0], pose.y + offset[1], pose.z + offset[2])) {
                return false;
            }
        }
        return true;
    }
    double Height(double x, double z) const {
        const float h = m_source.height(static_cast<float>(x), static_cast<float>(z));
        if (!std::isfinite(h)) {
            throw std::runtime_error("nonfinite height");
        }
        return static_cast<double>(Millimetres(h)) / 1000.0;
    }
    bool Pose(StationCandidate& candidate) {
        const double x = static_cast<double>(candidate.anchor_mm[0]) / 1000;
        const double y = static_cast<double>(candidate.anchor_mm[1]) / 1000;
        const double z = static_cast<double>(candidate.anchor_mm[2]) / 1000;
        for (const auto& direction : kDirections) {
            StationPose pose;
            pose.x = x - direction[0] * m_query.view_distance_m;
            pose.z = z - direction[1] * m_query.view_distance_m;
            pose.y = std::max(y + 1.8, Height(pose.x, pose.z) + 1.8);
            if (!EyeClear(pose)) {
                continue;
            }
            const float dx = static_cast<float>(x - pose.x);
            const float dy = static_cast<float>(y - pose.y);
            const float dz = static_cast<float>(z - pose.z);
            const float horizontal = DeterministicMath::Sqrt(dx * dx + dz * dz);
            // Cardinal horizontal displacement plus vertical displacement is an
            // upper bound on line length, so rounding binary32 sqrt cannot make
            // an interval exceed the fixed 4 m sightline spacing.
            const double length_bound = m_query.view_distance_m + std::abs(y - pose.y);
            const int steps = std::max(1, static_cast<int>(std::ceil(length_bound / kStep)));
            bool visible = true;
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                if (!Air(pose.x + (x - pose.x) * t,
                         pose.y + (y - pose.y) * t,
                         pose.z + (z - pose.z) * t)) {
                    visible = false;
                    break;
                }
            }
            if (!visible) {
                continue;
            }
            constexpr float to_degrees = 180.0f / DeterministicMath::kPi;
            pose.yaw = DeterministicMath::Atan2(dz, dx) * to_degrees;
            if (pose.yaw >= 180) {
                pose.yaw -= 360;
            }
            pose.pitch =
                std::clamp(DeterministicMath::Atan2(dy, horizontal) * to_degrees, -89.0f, 89.0f);
            candidate.pose = pose;
            return true;
        }
        return false;
    }

private:
    const StationDiscoverySource& m_source;
    const StationQuery& m_query;
    StationDiscoveryResult& m_result;
};

int FloorCell(double value) {
    if (!std::isfinite(value) || value < std::numeric_limits<int>::min() + 1.0 ||
        value > std::numeric_limits<int>::max() - 1.0) {
        throw std::runtime_error("cell coordinate outside integer domain");
    }
    return static_cast<int>(std::floor(value));
}
} // namespace

std::string ValidateStationQuery(const StationQuery& query) {
    if (query.kind != StationKind::Mouth && query.kind != StationKind::Arch &&
        query.kind != StationKind::Horizon && query.kind != StationKind::Coverage) {
        return "unknown station kind";
    }
    if (query.radius_m < 1 || query.radius_m > 16384 || query.view_distance_m < 0 ||
        query.view_distance_m > 16384 || query.limit < 1 || query.limit > 64) {
        return "radius, view distance or limit outside bounds";
    }
    const auto span = static_cast<std::int64_t>(query.y_range_m[1]) - query.y_range_m[0];
    if (span < 0 || span > 512) {
        return "vertical span must be ordered and at most 512 metres";
    }
    // Controls do not move the eye by D. Feature views reserve the portal
    // stencil, viewing displacement and 0.5 m eye-clearance footprint too.
    const bool feature = query.kind == StationKind::Mouth || query.kind == StationKind::Arch;
    const auto reach =
        static_cast<std::int64_t>(query.radius_m + (feature ? query.view_distance_m + 8 : 0)) *
            1000 +
        500;
    for (auto coordinate : query.origin_mm) {
        if (coordinate < -kStationHorizontalBoundMm + reach ||
            coordinate > kStationHorizontalBoundMm - reach) {
            return "origin, search or pose footprint outside horizontal domain";
        }
    }
    return {};
}

StationDiscoveryResult DiscoverStations(const Systems::SHIELD_WorldSystem& world,
                                        const StationQuery& query) {
    // shared_mutex is not recursive (and a pending writer blocks new scopes).
    // These synchronous point samplers do not acquire the epoch themselves;
    // retain this single outer scope through source creation and discovery.
    const auto worldgen_scope = world.acquire_worldgen_sample_scope();
    const StationDiscoverySource source{
        world.SurfaceBreakCellSize(),
        world.SurfaceBreakMaxRadius(),
        [&world](int x, int z) { return world.SurfaceBreakInCell(x, z); },
        [&world](float x, float z) { return world.GetTerrainHeightAt(x, z); },
        [&world](const Vec3& p) {
            return world.get_density_at(p);
        }};
    return DiscoverStations(source, query);
}

void RankStationCandidates(std::vector<StationCandidate>& candidates, const StationQuery& query) {
    std::sort(candidates.begin(), candidates.end(), [&query](const auto& a, const auto& b) {
        return std::tuple(Distance(a, query), a.cell_z, a.cell_x, a.local_index) <
               std::tuple(Distance(b, query), b.cell_z, b.cell_x, b.local_index);
    });
    if (candidates.size() > static_cast<std::size_t>(query.limit)) {
        candidates.resize(static_cast<std::size_t>(query.limit));
    }
}

StationDiscoveryResult DiscoverStations(const StationDiscoverySource& source,
                                        const StationQuery& query) {
    StationDiscoveryResult result;
    result.error = ValidateStationQuery(query);
    if (!result.error.empty()) {
        return result;
    }
    try {
        if (!source.height || !source.density) {
            throw std::runtime_error("missing read-only sampling source");
        }
        Sampler sampler(source, query, result);
        const double ox = static_cast<double>(query.origin_mm[0]) / 1000;
        const double oz = static_cast<double>(query.origin_mm[1]) / 1000;
        if (query.kind == StationKind::Horizon || query.kind == StationKind::Coverage) {
            const double height = sampler.Height(ox, oz);
            if (height < query.y_range_m[0] || height > query.y_range_m[1]) {
                result.ok = true;
                return result;
            }
            const int count = query.kind == StationKind::Horizon ? 4 : 5;
            constexpr std::array<float, 5> yaws{{0, 90, -180, -90, 45}};
            for (int i = 0; i < count; ++i) {
                StationCandidate candidate;
                candidate.local_index = static_cast<std::uint64_t>(i);
                candidate.id =
                    Id(query.kind == StationKind::Horizon ? "horizon-control" : "coverage-control",
                       0,
                       0,
                       candidate.local_index);
                candidate.anchor_mm = {query.origin_mm[0], Millimetres(height), query.origin_mm[1]};
                candidate.pose = {ox,
                                  height + (i == 4 ? 80.0 : 1.8),
                                  oz,
                                  yaws[static_cast<std::size_t>(i)],
                                  i == 4 ? -20.0f : 0.0f};
                if (sampler.EyeClear(candidate.pose)) {
                    result.candidates.push_back(candidate);
                }
            }
        } else if (source.cell_size_m != 0) {
            if (!std::isfinite(source.cell_size_m) || source.cell_size_m <= 0 ||
                !std::isfinite(source.max_radius_m) || source.max_radius_m <= 0 || !source.cell) {
                throw std::runtime_error("invalid surface-break catalogue");
            }
            const double reach = query.radius_m + static_cast<double>(source.max_radius_m);
            const int min_x = FloorCell((ox - reach) / source.cell_size_m);
            const int max_x = FloorCell((ox + reach) / source.cell_size_m);
            const int min_z = FloorCell((oz - reach) / source.cell_size_m);
            const int max_z = FloorCell((oz + reach) / source.cell_size_m);
            const auto nx =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(max_x) - min_x + 1);
            const auto nz =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(max_z) - min_z + 1);
            if (nx > kStationCellBudget || nz > kStationCellBudget ||
                nx * nz > kStationCellBudget) {
                throw std::runtime_error("enumerated cell budget exhausted");
            }
            const auto radius_mm = static_cast<std::int64_t>(query.radius_m) * 1000;
            const auto radius_squared = static_cast<std::uint64_t>(radius_mm * radius_mm);
            for (int cz = min_z; cz <= max_z; ++cz) {
                for (int cx = min_x; cx <= max_x; ++cx) {
                    ++result.enumerated_cells;
                    const auto feature = source.cell(cx, cz);
                    if (!feature.found) {
                        continue;
                    }
                    if (!std::isfinite(feature.x) || !std::isfinite(feature.z) ||
                        !std::isfinite(feature.radius) || feature.radius <= 0 ||
                        feature.radius > source.max_radius_m) {
                        throw std::runtime_error("invalid surface-break descriptor");
                    }
                    // Global lattice, with a local index over Z, X, then Y (including skipped
                    // points).
                    const int x0 = FloorCell((feature.x - feature.radius) / kStep);
                    const int x1 = FloorCell((feature.x + feature.radius) / kStep);
                    const int z0 = FloorCell((feature.z - feature.radius) / kStep);
                    const int z1 = FloorCell((feature.z + feature.radius) / kStep);
                    const int y0 = FloorCell(static_cast<double>(query.y_range_m[0]) / kStep);
                    const int y1 = FloorCell(static_cast<double>(query.y_range_m[1]) / kStep);
                    std::uint64_t index = 0;
                    for (int gz = z0; gz <= z1; ++gz) {
                        for (int gx = x0; gx <= x1; ++gx) {
                            for (int gy = y0; gy <= y1; ++gy, ++index) {
                                const double x = static_cast<double>(gx) * kStep;
                                const double y = static_cast<double>(gy) * kStep;
                                const double z = static_cast<double>(gz) * kStep;
                                // Complete portal stencil must fit within the requested Y span.
                                if (y < query.y_range_m[0] || y + 12 > query.y_range_m[1]) {
                                    continue;
                                }
                                const double fx = x - feature.x, fz = z - feature.z;
                                if (fx * fx + fz * fz > feature.radius * feature.radius) {
                                    continue;
                                }
                                StationCandidate candidate;
                                candidate.anchor_mm = {
                                    Millimetres(x), Millimetres(y), Millimetres(z)};
                                if (std::abs(candidate.anchor_mm[0] - query.origin_mm[0]) >
                                        radius_mm ||
                                    std::abs(candidate.anchor_mm[2] - query.origin_mm[1]) >
                                        radius_mm ||
                                    Distance(candidate, query) > radius_squared) {
                                    continue;
                                }
                                if (!sampler.Portal(x, y, z, query.kind == StationKind::Arch)) {
                                    continue;
                                }
                                candidate.cell_x = cx;
                                candidate.cell_z = cz;
                                candidate.local_index = index;
                                candidate.id =
                                    Id(query.kind == StationKind::Arch ? "arch" : "mouth",
                                       cz,
                                       cx,
                                       index);
                                if (sampler.Pose(candidate)) {
                                    result.candidates.push_back(candidate);
                                }
                            }
                        }
                    }
                }
            }
        }
        RankStationCandidates(result.candidates, query);
        result.ok = true;
    } catch (const std::exception& error) {
        result.error = error.what();
        result.candidates.clear(); // refusals never expose a partial result
    }
    return result;
}
} // namespace Luminumbra::world
