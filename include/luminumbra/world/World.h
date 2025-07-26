// include/luminumbra/world/World.h
#pragma once

#include <memory>
#include <map>
#include <glm/glm.hpp>

namespace Luminumbra::Rendering { class Shader; }
namespace Luminumbra::World { class Chunk; }

namespace Luminumbra::World {

struct IVec3Compare {
    bool operator()(const glm::ivec3& a, const glm::ivec3& b) const {
        if (a.x < b.x) return true;
        if (a.x > b.x) return false;
        if (a.y < b.y) return true;
        if (a.y > b.y) return false;
        if (a.z < b.z) return true;
        return false;
    }
};

class World {
public:
    World();
    ~World();

    void render(Luminumbra::Rendering::Shader& shader) const;
    bool isSolid(const glm::vec3& worldPosition) const;

    const std::map<glm::ivec3, std::unique_ptr<Chunk>, IVec3Compare>& getChunks() const { return m_Chunks; }

private:
    // Using a map to store chunks by their grid coordinates
    std::map<glm::ivec3, std::unique_ptr<Chunk>, IVec3Compare> m_Chunks;
};

} // namespace Luminumbra::World
