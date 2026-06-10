#pragma once

#include <string>
#include <memory>
#include <ctime>
#include <filesystem>
#include <vector>
#include "entt/entt.hpp"
#include "../../../include/luminumbra/core/Types.h"

namespace Luminumbra {
    class JobSystem;

    namespace Systems {
        class SHIELD_WorldSystem;
        class PhysicsSystem;
        class WaterSystem;
    }
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

struct WorldConfigValidationResult {
    bool ok = false;
    std::filesystem::path preset_path;
    std::vector<std::string> errors;
};

// Counters for one runtime world-state save pass (T-I2-12). The on-disk layout
// is a whole-world snapshot, so chunks_saved is the full streamed chunk count
// whenever a write happened and zero otherwise.
struct WorldStateSaveReport {
    std::size_t chunks_total = 0;
    std::size_t chunks_dirty = 0;
    std::size_t chunks_saved = 0;
    bool saved = false;
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

    // --- Runtime world-state persistence (T-I2-12) ---
    // Persists streamed chunk voxel state under the canonical world save dir
    // (worlds/saves/<world_id>). Incremental contract: nothing is written when
    // no chunk carries unsaved voxel edits, so a never-edited world stays
    // byte-for-byte on the fresh-world path. The first write of an edited
    // world emits a full snapshot; later writes flush through
    // WorldSaveService::save_dirty_chunks.
    bool SaveWorldState(WorldStateSaveReport* report = nullptr);
    bool SaveWorldStateTo(const std::filesystem::path& save_dir, WorldStateSaveReport* report = nullptr);

    // Loads a previously saved snapshot into the live streaming state. Must be
    // called AFTER the world systems initialize but BEFORE chunk streaming
    // generates fresh state: loaded chunks are adopted into the streaming map,
    // and generation skips chunks that already carry voxel data, so loaded
    // edits are never clobbered by regeneration. A missing snapshot is a clean
    // miss (returns false, fresh-world path unchanged).
    bool LoadWorldState();
    bool LoadWorldStateFrom(const std::filesystem::path& save_dir);
    std::size_t GetLastLoadedChunkCount() const { return m_lastLoadedChunkCount; }
    std::filesystem::path GetWorldSaveDir() const;

    static WorldConfigValidationResult ValidateWorldConfig(const std::string& root_path, const std::string& worldType);

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
    std::size_t m_lastLoadedChunkCount = 0;

    // Convert string seed to numeric seed
    uint32_t StringToSeed(const std::string& seedStr);
    std::string m_rootPath; 
};

} // namespace Luminumbra::world
