#include "FarVolumeMesher.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Luminumbra::World {
namespace {
// The same topology tables, corner layout and edge layout as MarchingCubes.cpp.
#include "MarchingCubesTables.inl"
constexpr int corners[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};
constexpr int edges[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

struct PositionLess {
    bool operator()(const FarVolumePosition& a, const FarVolumePosition& b) const {
        return std::tie(a.z, a.x, a.y) < std::tie(b.z, b.x, b.y);
    }
};
struct Edge {
    FarVolumePosition origin;
    int axis = 0;
};
struct EdgeLess {
    bool operator()(const Edge& a, const Edge& b) const {
        return std::tie(a.origin.z, a.origin.x, a.origin.y, a.axis) <
               std::tie(b.origin.z, b.origin.x, b.origin.y, b.axis);
    }
};
struct Refusal {
    FarVolumeMeshError error;
};
[[noreturn]] void refuse(FarVolumeMeshError error) {
    throw Refusal{error};
}
bool same_sample(const FarVolumeSample& a, const FarVolumeSample& b) {
    return std::bit_cast<std::uint32_t>(a.density) == std::bit_cast<std::uint32_t>(b.density) &&
           a.material == b.material;
}
FarVolumeVector subtract(const FarVolumeVector& a, const FarVolumeVector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
FarVolumeVector cross(const FarVolumeVector& a, const FarVolumeVector& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const FarVolumeVector& a, const FarVolumeVector& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
std::int64_t& component(FarVolumePosition& p, int axis) {
    return axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
}
bool tile_bounds(std::int64_t index, std::int64_t edge, std::int64_t& lo, std::int64_t& hi) {
    if (index < std::numeric_limits<std::int64_t>::min() / edge ||
        index > (std::numeric_limits<std::int64_t>::max() - edge) / edge)
        return false;
    lo = index * edge;
    hi = lo + edge;
    return true;
}

void validate_tile(const FarVolumeTile& tile, const FarVolumeMeshLimits& limits) {
    const auto tier = FarTierAt(tile.request.key.tier);
    if (!tier)
        refuse(FarVolumeMeshError::InvalidTile);
    const std::int64_t edge = tier->brick_edge_meters;
    const auto span = tile.request.span;
    FarVolumePosition lo{0, span.min_y, 0}, hi{0, span.max_y, 0};
    if (span.min_y >= span.max_y || span.min_y % edge != 0 || span.max_y % edge != 0 ||
        !tile_bounds(tile.request.key.x, tier->tile_edge_meters, lo.x, hi.x) ||
        !tile_bounds(tile.request.key.z, tier->tile_edge_meters, lo.z, hi.z) ||
        tile.min_meters != lo || tile.max_meters != hi)
        refuse(FarVolumeMeshError::InvalidTile);
    const auto layers = static_cast<std::uint64_t>(span.max_y / edge - span.min_y / edge);
    const auto across = static_cast<std::uint64_t>(tier->tile_edge_meters / edge);
    const auto columns = across * across;
    if (layers > std::numeric_limits<std::uint64_t>::max() / columns / kFarVolumeBrickSamples ||
        tile.candidates_visited != layers * columns ||
        tile.samples_evaluated != tile.candidates_visited * kFarVolumeBrickSamples ||
        tile.bricks.size() > tile.candidates_visited)
        refuse(FarVolumeMeshError::InvalidTile);
    if (tile.bricks.size() > limits.max_cells / 64)
        refuse(FarVolumeMeshError::CellLimit);
    if ((tile.content != FarVolumeContent::Air && tile.content != FarVolumeContent::Solid &&
         tile.content != FarVolumeContent::Mixed) ||
        (tile.content == FarVolumeContent::Mixed) != !tile.bricks.empty())
        refuse(FarVolumeMeshError::InvalidTile);
}
} // namespace

bool MeshFarVolumeTile(const FarVolumeTile& tile,
                       std::uint64_t sampler_field_identity,
                       const FarVolumeSampler& sampler,
                       const FarVolumeMeshLimits& limits,
                       FarVolumeMesh& output,
                       FarVolumeMeshError* error) {
    const auto fail = [error](FarVolumeMeshError value) {
        if (error)
            *error = value;
        return false;
    };
    try {
        validate_tile(tile, limits);
        if (sampler_field_identity != tile.request.field_identity)
            refuse(FarVolumeMeshError::FieldIdentityMismatch);
        if (!sampler)
            refuse(FarVolumeMeshError::InvalidSampler);
        const auto tier = *FarTierAt(tile.request.key.tier);
        const std::int64_t step = tier.sample_spacing_meters;
        const std::int64_t edge = tier.brick_edge_meters;
        FarVolumeMesh mesh;
        mesh.request = tile.request;
        mesh.origin_meters = tile.min_meters;
        mesh.sampled_max_meters = tile.max_meters;
        mesh.content = tile.content;
        std::map<FarVolumePosition, FarVolumeSample, PositionLess> sample_cache;
        const auto sample = [&](const FarVolumePosition& p) -> FarVolumeSample {
            const auto found = sample_cache.find(p);
            if (found != sample_cache.end())
                return found->second;
            if (mesh.samples_evaluated >= limits.max_sample_evaluations)
                refuse(FarVolumeMeshError::SampleLimit);
            ++mesh.samples_evaluated;
            std::optional<FarVolumeSample> value;
            try {
                value = sampler(p);
            } catch (...) {
                refuse(FarVolumeMeshError::SamplerFailure);
            }
            if (!value)
                refuse(FarVolumeMeshError::SamplerFailure);
            if (!std::isfinite(value->density))
                refuse(FarVolumeMeshError::NonFiniteDensity);
            sample_cache.emplace(p, *value);
            return *value;
        };
        std::vector<const FarVolumeBrick*> bricks;
        for (const auto& brick : tile.bricks) {
            const auto p = brick.origin_meters;
            if (p.x % edge != 0 || p.y % edge != 0 || p.z % edge != 0 || p.x < tile.min_meters.x ||
                p.x > tile.max_meters.x - edge || p.y < tile.min_meters.y ||
                p.y > tile.max_meters.y - edge || p.z < tile.min_meters.z ||
                p.z > tile.max_meters.z - edge)
                refuse(FarVolumeMeshError::InvalidTile);
            bricks.push_back(&brick);
        }
        std::sort(bricks.begin(), bricks.end(), [](const auto* a, const auto* b) {
            return PositionLess{}(a->origin_meters, b->origin_meters);
        });
        // Validate all resident samples before using the authority for halo work.
        for (std::size_t i = 0; i < bricks.size(); ++i) {
            const auto& brick = *bricks[i];
            if (i != 0 && brick.origin_meters == bricks[i - 1]->origin_meters)
                refuse(FarVolumeMeshError::InvalidTile);
            bool solid = false, air = false;
            for (std::size_t j = 0; j < kFarVolumeBrickSamples; ++j) {
                const auto resident = brick.samples[j];
                if (!std::isfinite(resident.density))
                    refuse(FarVolumeMeshError::NonFiniteDensity);
                const FarVolumePosition p{
                    brick.origin_meters.x + static_cast<std::int64_t>(j % 5) * step,
                    brick.origin_meters.y + static_cast<std::int64_t>((j / 5) % 5) * step,
                    brick.origin_meters.z + static_cast<std::int64_t>(j / 25) * step};
                if (!same_sample(resident, sample(p)))
                    refuse(FarVolumeMeshError::SampleMismatch);
                solid = solid || resident.density < 0.0f;
                air = air || resident.density >= 0.0f;
            }
            if (!solid || !air)
                refuse(FarVolumeMeshError::InvalidTile);
        }
        std::map<FarVolumePosition, FarVolumeVector, PositionLess> gradient_cache;
        const auto gradient = [&](const FarVolumePosition& p) -> FarVolumeVector {
            const auto found = gradient_cache.find(p);
            if (found != gradient_cache.end())
                return found->second;
            FarVolumeVector result{};
            for (int axis = 0; axis < 3; ++axis) {
                auto below = p, above = p;
                const auto v = component(below, axis);
                if (v < std::numeric_limits<std::int64_t>::min() + step ||
                    v > std::numeric_limits<std::int64_t>::max() - step)
                    refuse(FarVolumeMeshError::CoordinateOverflow);
                component(below, axis) -= step;
                component(above, axis) += step;
                const double above_density = sample(above).density;
                const double below_density = sample(below).density;
                result[static_cast<std::size_t>(axis)] =
                    (above_density - below_density) / (2.0 * static_cast<double>(step));
            }
            gradient_cache.emplace(p, result);
            return result;
        };
        std::map<Edge, std::uint32_t, EdgeLess> vertex_cache;
        struct Crossing {
            Edge key;
            FarVolumePosition end;
            double fraction = 0;
            FarVolumeVertex vertex;
        };
        const auto vertex_index = [&](const Crossing& crossing) -> std::uint32_t {
            const auto found = vertex_cache.find(crossing.key);
            if (found != vertex_cache.end())
                return found->second;
            if (mesh.vertices.size() >= limits.max_vertices ||
                mesh.vertices.size() >= std::numeric_limits<std::uint32_t>::max())
                refuse(FarVolumeMeshError::VertexLimit);
            auto vertex = crossing.vertex;
            const auto a = gradient(crossing.key.origin), b = gradient(crossing.end);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                // Exact endpoint crossings use that endpoint's gradient, including
                // when several incident edges meet the same zero-valued lattice point.
                vertex.normal[axis] =
                    crossing.fraction == 0.0
                        ? a[axis]
                        : (crossing.fraction == 1.0
                               ? b[axis]
                               : a[axis] + crossing.fraction * (b[axis] - a[axis]));
            }
            const double length = std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            if (length == 0.0)
                refuse(FarVolumeMeshError::ZeroGradient);
            for (auto& v : vertex.normal)
                v /= length;
            const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(vertex);
            vertex_cache.emplace(crossing.key, index);
            if (!mesh.bounds)
                mesh.bounds = FarVolumeMeshBounds{vertex.position, vertex.position};
            else
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    mesh.bounds->min[axis] =
                        std::min(mesh.bounds->min[axis], vertex.position[axis]);
                    mesh.bounds->max[axis] =
                        std::max(mesh.bounds->max[axis], vertex.position[axis]);
                }
            return index;
        };
        for (const auto* brick : bricks) {
            for (int z = 0; z < 4; ++z) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        ++mesh.cells_visited;
                        std::array<FarVolumePosition, 8> positions{};
                        std::array<FarVolumeSample, 8> values{};
                        unsigned int configuration = 0;
                        for (std::size_t c = 0; c < 8; ++c) {
                            const int sx = x + corners[c][0], sy = y + corners[c][1],
                                      sz = z + corners[c][2];
                            positions[c] = {brick->origin_meters.x + sx * step,
                                            brick->origin_meters.y + sy * step,
                                            brick->origin_meters.z + sz * step};
                            values[c] =
                                brick->samples[static_cast<std::size_t>(sx + 5 * (sy + 5 * sz))];
                            if (values[c].density < 0.0f)
                                configuration |= 1u << c;
                        }
                        if (edgeTable[configuration] == 0)
                            continue;
                        std::array<Crossing, 12> crossings{};
                        for (std::size_t e = 0; e < 12; ++e) {
                            if ((edgeTable[configuration] & (1u << e)) == 0)
                                continue;
                            auto a = static_cast<std::size_t>(edges[e][0]);
                            auto b = static_cast<std::size_t>(edges[e][1]);
                            const int axis = positions[a].x != positions[b].x
                                                 ? 0
                                                 : (positions[a].y != positions[b].y ? 1 : 2);
                            if (component(positions[a], axis) > component(positions[b], axis))
                                std::swap(a, b);
                            auto& crossing = crossings[e];
                            crossing.key = {positions[a], axis};
                            crossing.end = positions[b];
                            const double da = values[a].density, db = values[b].density;
                            // Opposite classifications guarantee a nonzero double denominator,
                            // even for finite subnormal float samples. No epsilon snaps an edge.
                            crossing.fraction = -da / (db - da);
                            crossing.vertex.material = values[da < 0.0 ? a : b].material;
                            crossing.vertex.position = {
                                static_cast<double>(positions[a].x / step -
                                                    tile.min_meters.x / step) *
                                    static_cast<double>(step),
                                static_cast<double>(positions[a].y / step -
                                                    tile.min_meters.y / step) *
                                    static_cast<double>(step),
                                static_cast<double>(positions[a].z / step -
                                                    tile.min_meters.z / step) *
                                    static_cast<double>(step)};
                            crossing.vertex.position[static_cast<std::size_t>(axis)] +=
                                crossing.fraction * static_cast<double>(step);
                        }
                        // Preserve the table's negative-solid winding per component. A
                        // cell-wide gradient can reverse one disconnected component (case65).
                        for (int t = 0; triTable[configuration][t] != -1; t += 3) {
                            std::array<const Crossing*, 3> triangle{
                                &crossings[static_cast<std::size_t>(triTable[configuration][t])],
                                &crossings[static_cast<std::size_t>(
                                    triTable[configuration][t + 1])],
                                &crossings[static_cast<std::size_t>(
                                    triTable[configuration][t + 2])]};
                            const auto face = cross(subtract(triangle[1]->vertex.position,
                                                             triangle[0]->vertex.position),
                                                    subtract(triangle[2]->vertex.position,
                                                             triangle[0]->vertex.position));
                            if (dot(face, face) == 0.0)
                                continue;
                            if (limits.max_indices - mesh.indices.size() < 3)
                                refuse(FarVolumeMeshError::IndexLimit);
                            for (const auto* crossing : triangle)
                                mesh.indices.push_back(vertex_index(*crossing));
                        }
                    }
                }
            }
        }
        output = std::move(mesh);
    } catch (const Refusal& refusal) {
        return fail(refusal.error);
    } catch (const std::bad_alloc&) {
        return fail(FarVolumeMeshError::AllocationFailure);
    } catch (const std::length_error&) {
        return fail(FarVolumeMeshError::AllocationFailure);
    }
    if (error)
        *error = FarVolumeMeshError::None;
    return true;
}
} // namespace Luminumbra::World
