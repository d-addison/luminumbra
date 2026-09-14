#pragma once

#include "luminumbra_common/world/Chunk.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace Luminumbra::Rendering {

// A render-owned vertical query over the same triangles uploaded to the terrain
// pool. Bins bound the candidate search; no worldgen/SDF approximation is used.
class FoliageGroundMesh {
public:
    struct Hit {
        bool valid = false;
        float height = -std::numeric_limits<float>::infinity();
        float slope = 1.0f;
    };

    FoliageGroundMesh(const glm::vec3& origin,
                      const std::vector<VoxelVertex>& vertices,
                      const std::vector<u32>& indices)
        : m_origin(origin) {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
                indices[i + 2] >= vertices.size())
                continue;
            const auto& a = vertices[indices[i]];
            const auto& b = vertices[indices[i + 1]];
            const auto& c = vertices[indices[i + 2]];
            if (a.material_id < 1 || a.material_id > 5 ||
                (a.normal.y + b.normal.y + c.normal.y) <= 0.3f)
                continue;
            const glm::vec3 ab = b.position - a.position;
            const glm::vec3 ac = c.position - a.position;
            const float determinant = ab.x * ac.z - ab.z * ac.x;
            if (std::abs(determinant) < 1e-7f)
                continue; // vertical or degenerate triangles cannot support grass
            const glm::vec3 n = glm::cross(ab, ac);
            const float slope = std::sqrt(n.x * n.x + n.z * n.z) / std::abs(n.y);
            const auto slot = static_cast<u32>(m_triangles.size());
            m_triangles.push_back({a.position, ab, ac, 1.0f / determinant, slope});
            const int x0 = bin(std::min({a.position.x, b.position.x, c.position.x}));
            const int x1 = bin(std::max({a.position.x, b.position.x, c.position.x}));
            const int z0 = bin(std::min({a.position.z, b.position.z, c.position.z}));
            const int z1 = bin(std::max({a.position.z, b.position.z, c.position.z}));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    m_bins[z * kBins + x].push_back(slot);
        }
    }

    Hit sample(float world_x, float world_z) const {
        const float x = world_x - m_origin.x;
        const float z = world_z - m_origin.z;
        Hit hit;
        if (x < 0 || z < 0 || x > CHUNK_SIZE_X || z > CHUNK_SIZE_Z)
            return hit;
        for (u32 slot : m_bins[bin(z) * kBins + bin(x)]) {
            const auto& t = m_triangles[slot];
            const float dx = x - t.a.x;
            const float dz = z - t.a.z;
            const float u = (dx * t.ac.z - dz * t.ac.x) * t.inverse_determinant;
            const float v = (t.ab.x * dz - t.ab.z * dx) * t.inverse_determinant;
            if (u < -1e-5f || v < -1e-5f || u + v > 1.00001f)
                continue;
            const float y = m_origin.y + t.a.y + u * t.ab.y + v * t.ac.y;
            if (y > hit.height)
                hit = {true, y, t.slope};
        }
        return hit;
    }

private:
    static constexpr int kBins = 8;
    static int bin(float position) {
        return std::clamp(
            static_cast<int>(std::floor(position * kBins / CHUNK_SIZE_X)), 0, kBins - 1);
    }
    struct Triangle {
        glm::vec3 a, ab, ac;
        float inverse_determinant;
        float slope;
    };
    glm::vec3 m_origin;
    std::vector<Triangle> m_triangles;
    std::array<std::vector<u32>, kBins * kBins> m_bins;
};

} // namespace Luminumbra::Rendering
