#pragma once

// This is a classic implementation of the Marching Cubes algorithm by Paul Bourke.
// Source: http://paulbourke.net/geometry/polygonise/
// It has been slightly adapted to use glm::vec3 and fit into our project structure.

#include <glm/glm.hpp>
#include <vector>
#include <map>
#include <unordered_map>
#include "luminumbra/core/Hash.h"

namespace Luminumbra::World::MarchingCubes {

struct IndexedMesh {
    std::vector<glm::vec3> vertices;
    std::vector<unsigned int> indices;
};

struct Triangle {
    glm::vec3 p[3];
};

struct GridCell {
    glm::vec3 p[8];
    float val[8];
};

extern const int edgeTable[256];
extern const int triTable[256][16];

glm::vec3 VertexInterp(float isolevel, glm::vec3 p1, glm::vec3 p2, float valp1, float valp2);
void Polygonise(GridCell grid, float isolevel, IndexedMesh& mesh,
                std::unordered_map<glm::ivec3, unsigned int>& cacheX,
                std::unordered_map<glm::ivec3, unsigned int>& cacheY,
                std::unordered_map<glm::ivec3, unsigned int>& cacheZ);

} // namespace Luminumbra::World::MarchingCubes
