#pragma once

#include <string>
#include <memory>
#include <ctime>
#include "entt/entt.hpp"
#include "../../../include/luminumbra/core/Types.h"

namespace Luminumbra::Systems {
    class SHIELD_WorldSystem;
}

namespace Luminumbra {
    class JobSystem;
}

namespace Luminumbra::world {

struct WorldMetadata {
    std::string name;
    std::string seed;
    std::string worldType;
    std::string worldId;
    std::time_t creationTime;
    Vec3 spawnPoint;
};

class GameSession {
public:
    GameSession();
    ~GameSession();

    // Initialize a new world with the given parameters
    bool CreateWorld(const std::string& name, const std::string& seed, const std::string& worldType);

    // Load an existing world from disk
    bool LoadWorld(const std::string& worldId);

    // Save the current world state
    bool SaveWorld();

    // Get world metadata
    const WorldMetadata& GetMetadata() const { return m_metadata; }

    // Get the world system for chunk generation
    Systems::SHIELD_WorldSystem* GetWorldSystem() { return m_worldSystem.get(); }

    entt::registry& GetRegistry() { return m_registry; }

    // Set the job system (must be called before CreateWorld/LoadWorld)
    void SetJobSystem(JobSystem* jobSystem) { m_jobSystem = jobSystem; }
    void SetRootPath(const std::string& root_path) { m_rootPath = root_path; }

private:
    entt::registry m_registry;
    WorldMetadata m_metadata;
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_worldSystem;
    JobSystem* m_jobSystem = nullptr;

    // Generate a unique world ID
    std::string GenerateWorldId();

    // Convert string seed to numeric seed
    uint32_t StringToSeed(const std::string& seedStr);
    std::string m_rootPath; 
};

} // namespace Luminumbra::world
