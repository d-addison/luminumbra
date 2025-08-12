#pragma once

namespace Luminumbra::Systems { class SHIELD_WorldSystem; }
namespace Luminumbra {
    class Chunk;
}

namespace Luminumbra::World::MarchingCubes {
    void PolygoniseChunk(const Systems::SHIELD_WorldSystem& world_system, Chunk& chunk, float isolevel, int step);
}