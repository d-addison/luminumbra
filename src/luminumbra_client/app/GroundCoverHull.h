#pragma once

#include <array>
#include <cstddef>
#include <glm/glm.hpp>

namespace Luminumbra::Client::App {

struct GroundCoverHullFace {
    std::array<glm::vec3, 3> positions;
    glm::vec3 normal;
};

struct GroundCoverHull {
    // Coarse ground cover has at most eight faces; the farthest LOD uses four.
    std::array<GroundCoverHullFace, 8> faces{};
    std::size_t faceCount = 0;
};

// CPU geometry shared by rock and bush uploads. Bounds are the finite,
// non-degenerate AABB of a generated palette entry, in the prop's local axes.
inline GroundCoverHull
BuildGroundCoverHull(const glm::vec3& lo, const glm::vec3& hi, bool farthest) {
    const glm::vec3 c = (lo + hi) * 0.5f;
    const std::array<glm::vec3, 6> octahedron = {{{hi.x, c.y, c.z},
                                                  {lo.x, c.y, c.z},
                                                  {c.x, hi.y, c.z},
                                                  {c.x, lo.y, c.z},
                                                  {c.x, c.y, hi.z},
                                                  {c.x, c.y, lo.z}}};
    const std::array<std::array<std::size_t, 3>, 8> octahedronFaces = {
        {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}}};
    // Upright tetrahedron: a triangular footprint at lo.y and an apex at hi.y.
    const std::array<glm::vec3, 4> tetrahedron = {
        {{c.x, hi.y, c.z}, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {c.x, lo.y, hi.z}}};
    const std::array<std::array<std::size_t, 3>, 4> tetrahedronFaces = {
        {{0, 1, 3}, {0, 3, 2}, {0, 2, 1}, {1, 2, 3}}};
    GroundCoverHull hull;
    hull.faceCount = farthest ? tetrahedronFaces.size() : octahedronFaces.size();
    for (std::size_t f = 0; f < hull.faceCount; ++f) {
        const auto& indices = farthest ? tetrahedronFaces[f] : octahedronFaces[f];
        auto& face = hull.faces[f];
        for (std::size_t v = 0; v < face.positions.size(); ++v) {
            face.positions[v] = farthest ? tetrahedron[indices[v]] : octahedron[indices[v]];
        }
        const glm::vec3 cross = glm::cross(face.positions[1] - face.positions[0],
                                           face.positions[2] - face.positions[0]);
        const float length = glm::length(cross);
        face.normal = length > 1e-6f ? cross / length : glm::vec3(0, 1, 0);
    }
    return hull;
}

} // namespace Luminumbra::Client::App
