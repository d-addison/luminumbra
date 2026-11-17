#include "NearTerrainRefinement.h"
#include "luminumbra_common/world/NearTerrainMeshSnapshot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace Luminumbra::Rendering {
namespace {
constexpr u32 kNoSplit = std::numeric_limits<u32>::max();
constexpr int kSide = CHUNK_SIZE_X + 1;
constexpr std::size_t kDensityCount =
    static_cast<std::size_t>(kSide) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
static_assert(CHUNK_SIZE_X == 16 && CHUNK_SIZE_Y == 16 && CHUNK_SIZE_Z == 16);

struct Edge {
    u32 low;
    u32 high;
    u32 occurrence;
};
static_assert(sizeof(Edge) == 3 * sizeof(u32));
static_assert(kNearTerrainMaxInputVertices + kNearTerrainMaxAddedVertices < kNoSplit);
static_assert(kNearTerrainMaxInputVertices + kNearTerrainMaxAddedVertices <=
              std::numeric_limits<std::size_t>::max() / sizeof(VoxelVertex));
static_assert(kNearTerrainMaxOutputTriangles <=
              std::numeric_limits<std::size_t>::max() / (3 * sizeof(u32)));
static_assert(kNearTerrainMaxInputTriangles <=
              std::numeric_limits<std::size_t>::max() / (3 * sizeof(Edge)));

bool Eligible(const NearTerrainRefinementInput& input) {
    return input.enabled && input.generated_current_params && input.unedited_halo &&
           input.sample_step == 1 && std::isfinite(input.chunk_distance_squared) &&
           input.chunk_distance_squared >= 0.0f &&
           input.chunk_distance_squared <= kNearTerrainRadiusMeters * kNearTerrainRadiusMeters &&
           input.densities.size() == kDensityCount;
}

bool HeightLike(const std::span<const float>& densities) {
    // Pure generated height densities increase by one per vertical metre.
    // Checking the entire bounded lattice also rejects caves/multiple crossings
    // away from the visible triangles, without relying on surface normals alone.
    return World::IsHeightLikeNearTerrainSdf(densities);
}

bool ValidMesh(const NearTerrainRefinementInput& input) {
    for (const auto& vertex : input.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(vertex.position[axis]) || vertex.position[axis] < 0.0f ||
                vertex.position[axis] > 16.0f || !std::isfinite(vertex.normal[axis])) {
                return false;
            }
        }
        const float length_squared = glm::dot(vertex.normal, vertex.normal);
        if (!std::isfinite(length_squared) || std::abs(length_squared - 1.0f) > 0.01f ||
            vertex.normal.y < 0.25f) {
            return false;
        }
    }
    for (std::size_t start = 0; start < input.indices.size(); start += 3) {
        const auto a = input.indices[start];
        const auto b = input.indices[start + 1];
        const auto c = input.indices[start + 2];
        if (a >= input.vertices.size() || b >= input.vertices.size() ||
            c >= input.vertices.size()) {
            return false;
        }
        const Vec3 ab = input.vertices[b].position - input.vertices[a].position;
        const Vec3 ac = input.vertices[c].position - input.vertices[a].position;
        const Vec3 face = glm::cross(ab, ac);
        if (face.y <= 0.000001f || face.y < 0.25f * glm::length(face)) {
            return false;
        }
    }
    return true;
}

bool SharedChunkFace(const Vec3& a, const Vec3& b) {
    for (int axis = 0; axis < 3; ++axis) {
        if ((a[axis] == 0.0f && b[axis] == 0.0f) || (a[axis] == 16.0f && b[axis] == 16.0f)) {
            return true;
        }
    }
    return false;
}

float HeightDerivative(const VoxelVertex& from, const VoxelVertex& to) {
    const Vec3 direction = to.position - from.position;
    return -(from.normal.x * direction.x + from.normal.z * direction.z) / from.normal.y;
}

VoxelVertex
CurvedVertex(const Vec3& linear, const Vec3& normal, u32 material, float curved_height) {
    Vec3 position = linear;
    // Clamp to the chunk too: a refinement must not extend its culling bounds
    // or move a face vertex away from its unrefined neighbour's surface.
    if (!SharedChunkFace(linear, linear)) {
        position.y = std::clamp(linear.y + std::clamp(curved_height - linear.y,
                                                      -kNearTerrainMaxDisplacementMeters,
                                                      kNearTerrainMaxDisplacementMeters),
                                0.0f,
                                16.0f);
    }
    return {position, glm::normalize(normal), material};
}

VoxelVertex Midpoint(const VoxelVertex& a, const VoxelVertex& b) {
    const Vec3 linear = (a.position + b.position) * 0.5f;
    const float height = linear.y + (HeightDerivative(a, b) + HeightDerivative(b, a)) * 0.125f;
    return CurvedVertex(linear, a.normal + b.normal, a.material_id, height);
}

VoxelVertex Centre(const VoxelVertex& a, const VoxelVertex& b, const VoxelVertex& c) {
    const Vec3 linear = (a.position + b.position + c.position) / 3.0f;
    // At barycentric (1/3,1/3,1/3), the cubic triangular patch with the
    // PN-style central control equals the mean of its six Hermite edge controls.
    const float height =
        linear.y + (HeightDerivative(a, b) + HeightDerivative(b, a) + HeightDerivative(b, c) +
                    HeightDerivative(c, b) + HeightDerivative(c, a) + HeightDerivative(a, c)) /
                       18.0f;
    // Every child ends with this centre. Preserve the source triangle's last
    // (provoking) vertex material used by the terrain G-buffer's flat input.
    return CurvedVertex(linear, a.normal + b.normal + c.normal, c.material_id, height);
}

bool BuildSplits(const NearTerrainRefinementInput& input,
                 std::vector<Edge>& edges,
                 std::vector<u32>& splits,
                 std::size_t& interior_edges) {
    for (std::size_t occurrence = 0; occurrence < input.indices.size(); ++occurrence) {
        const auto next = occurrence - occurrence % 3 + (occurrence + 1) % 3;
        const u32 a = input.indices[occurrence];
        const u32 b = input.indices[next];
        edges.push_back({std::min(a, b), std::max(a, b), static_cast<u32>(occurrence)});
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        return std::tie(a.low, a.high, a.occurrence) < std::tie(b.low, b.high, b.occurrence);
    });
    for (std::size_t start = 0; start < edges.size();) {
        std::size_t end = start + 1;
        while (end < edges.size() && edges[end].low == edges[start].low &&
               edges[end].high == edges[start].high) {
            ++end;
        }
        if (end - start > 2) {
            return false;
        }
        if (end - start == 2) {
            const auto& first = edges[start];
            const auto& second = edges[start + 1];
            if (input.indices[first.occurrence] == input.indices[second.occurrence]) {
                return false;
            }
            if (!SharedChunkFace(input.vertices[first.low].position,
                                 input.vertices[first.high].position)) {
                const auto split = static_cast<u32>(input.vertices.size() + interior_edges);
                splits[first.occurrence] = split;
                splits[second.occurrence] = split;
                ++interior_edges;
            }
        }
        start = end;
    }
    return true;
}

void BuildMesh(const NearTerrainRefinementInput& input,
               const std::vector<Edge>& edges,
               const std::vector<u32>& splits,
               NearTerrainRenderMesh& mesh) {
    mesh.vertices.insert(mesh.vertices.end(), input.vertices.begin(), input.vertices.end());
    for (const auto& edge : edges) {
        if (splits[edge.occurrence] == mesh.vertices.size()) {
            mesh.vertices.push_back(Midpoint(input.vertices[edge.low], input.vertices[edge.high]));
        }
    }
    for (std::size_t start = 0; start < input.indices.size(); start += 3) {
        const auto centre = static_cast<u32>(mesh.vertices.size());
        mesh.vertices.push_back(Centre(input.vertices[input.indices[start]],
                                       input.vertices[input.indices[start + 1]],
                                       input.vertices[input.indices[start + 2]]));
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const u32 a = input.indices[start + corner];
            const u32 b = input.indices[start + (corner + 1) % 3];
            const u32 split = splits[start + corner];
            if (split == kNoSplit) {
                mesh.indices.insert(mesh.indices.end(), {a, b, centre});
            } else {
                mesh.indices.insert(mesh.indices.end(), {a, split, centre, split, b, centre});
            }
        }
    }
}
} // namespace

NearTerrainRefinementResult RefineNearTerrain(const NearTerrainRefinementInput& input,
                                              NearTerrainRenderMesh& output) {
    // Division/constant bounds precede all scans and size products/allocations.
    if (input.indices.size() / 3 > kNearTerrainMaxInputTriangles ||
        input.vertices.size() > kNearTerrainMaxInputVertices) {
        return NearTerrainRefinementResult::CapacityExceeded;
    }
    if (!Eligible(input) || !HeightLike(input.densities)) {
        return NearTerrainRefinementResult::Ineligible;
    }
    if (input.vertices.empty() || input.indices.empty() || input.indices.size() % 3 != 0 ||
        !ValidMesh(input)) {
        return NearTerrainRefinementResult::InvalidMesh;
    }
    std::vector<Edge> edges;
    edges.reserve(input.indices.size());
    std::vector<u32> splits(input.indices.size(), kNoSplit);
    std::size_t interior_edges = 0;
    if (!BuildSplits(input, edges, splits, interior_edges)) {
        return NearTerrainRefinementResult::InvalidMesh;
    }
    const std::size_t triangles = input.indices.size() / 3;
    const std::size_t vertex_count = input.vertices.size() + interior_edges + triangles;
    const std::size_t index_count = input.indices.size() * 3 + interior_edges * 6;
    // Each interior edge consumes two of the at most 3*2048 occurrences.
    // Thus I <= 3072, V+I+T <= 11264, and 9*T+6*I <= 36864 indices.
    NearTerrainRenderMesh mesh;
    mesh.vertices.reserve(vertex_count);
    mesh.indices.reserve(index_count);
    BuildMesh(input, edges, splits, mesh);
    output.vertices.swap(mesh.vertices);
    output.indices.swap(mesh.indices);
    return NearTerrainRefinementResult::Refined;
}

} // namespace Luminumbra::Rendering
