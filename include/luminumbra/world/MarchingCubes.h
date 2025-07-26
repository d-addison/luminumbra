#pragma once

// This is a classic implementation of the Marching Cubes algorithm by Paul Bourke.
// Source: http://paulbourke.net/geometry/polygonise/
// It has been slightly adapted to use glm::vec3 and fit into our project structure.

#include <glm/glm.hpp>
#include <vector>

namespace Luminumbra::World::MarchingCubes {

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
int Polygonise(GridCell grid, float isolevel, std::vector<Triangle>& triangles);

} // namespace Luminumbra::World::MarchingCubes