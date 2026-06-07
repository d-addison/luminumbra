#pragma once

#include <string>
#include <memory>
#include <ctime>
#include "entt/entt.hpp"
#include "../../../include/luminumbra/core/Types.h"
#include "systems/SHIELD_WorldSystem.h"
#include "systems/PhysicsSystem.h"
#include "systems/WaterSystem.h"

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

namespace Luminumbra::Systems { class SHIELD_WorldSystem; class PhysicsSystem; class WaterSystem; } // <<< WaterSystem added


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
    
    // Update spawn point (useful for debugging)
    void SetSpawnPoint(const Vec3& new_spawn) { m_metadata.spawnPoint = new_spawn; }

    // Get the world system for chunk generation
    Systems::SHIELD_WorldSystem* GetWorldSystem() { return m_worldSystem.get(); }
    Systems::WaterSystem* GetWaterSystem() { return m_waterSystem.get(); }
    Systems::PhysicsSystem* GetPhysicsSystem() { return m_physicsSystem.get(); } 

    entt::registry& GetRegistry() { return m_registry; }

    // Set the job system (must be called before CreateWorld/LoadWorld)
    void SetJobSystem(JobSystem* jobSystem) { m_jobSystem = jobSystem; }
    void SetRootPath(const std::string& root_path) { m_rootPath = root_path; }

private:
    entt::registry m_registry;
    WorldMetadata m_metadata;
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_worldSystem;
    std::unique_ptr<Systems::WaterSystem> m_waterSystem;
    JobSystem* m_jobSystem = nullptr;

    // Generate a unique world ID
    std::string GenerateWorldId();
    std::unique_ptr<Systems::PhysicsSystem> m_physicsSystem; 

    // Convert string seed to numeric seed
    uint32_t StringToSeed(const std::string& seedStr);
    std::string m_rootPath; 
};

} // namespace Luminumbra::world