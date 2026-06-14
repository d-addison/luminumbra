#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <ctime>
#include <filesystem>
#include <vector>
#include "entt/entt.hpp"
#include "../../../include/luminumbra/core/Types.h"
#include "../core/SimulationClock.h"
#include "../simulation/SimulationEventBus.h"

namespace Luminumbra {
    class JobSystem;

    namespace Systems {
        class SHIELD_WorldSystem;
        class PhysicsSystem;
        class WaterSystem;
        class WindFieldSystem;
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

    // --- Asset-manifest split (T-I3-6) ---
    // The engine validates SIMULATION requirements only: a safe world type
    // and a readable, parseable world preset. Callers that additionally need
    // runtime assets (the CLIENT's shaders/RML/fonts) supply them as paths
    // relative to root_path; a headless host supplies none.
    static WorldConfigValidationResult ValidateWorldConfig(
        const std::string& root_path,
        const std::string& worldType,
        const std::vector<std::filesystem::path>& required_assets = {});

    // Registers the caller's required runtime assets (relative to the root
    // path) checked by CreateWorld/LoadWorld validation. The client populates
    // this with its shader/UI/font manifest before world create; the engine
    // default is empty (simulation-only validation).
    void SetRequiredClientAssets(std::vector<std::filesystem::path> relative_paths) {
        m_requiredClientAssets = std::move(relative_paths);
    }
    const std::vector<std::filesystem::path>& GetRequiredClientAssets() const {
        return m_requiredClientAssets;
    }

    // Get world metadata
    const WorldMetadata& GetMetadata() const { return m_metadata; }
    
    // Update spawn point (useful for debugging)
    void SetSpawnPoint(const Vec3& new_spawn) { m_metadata.spawnPoint = new_spawn; }

    // Get the world system for chunk generation
    Systems::SHIELD_WorldSystem* GetWorldSystem() { return m_worldSystem.get(); }
    Systems::WaterSystem* GetWaterSystem() { return m_waterSystem.get(); }
    Systems::PhysicsSystem* GetPhysicsSystem() { return m_physicsSystem.get(); }

    // T-I5a-2 (A2): the deterministic wind field. Sim-authoritative; its cell
    // values feed the world_hash `wind` sub-hash. Constructed on world
    // create/load (pure function of the world seed); updated per fixed tick in
    // TickSimulation around the spawn/stream anchor.
    Systems::WindFieldSystem* GetWindFieldSystem() { return m_windFieldSystem.get(); }
    const Systems::WindFieldSystem* GetWindFieldSystem() const { return m_windFieldSystem.get(); }

    entt::registry& GetRegistry() { return m_registry; }

    // --- Fixed-rate simulation (T-I3-4) ---
    // Advances the 30 Hz simulation clock by one variable-dt frame and runs
    // the produced fixed ticks (clamped to the clock's catch-up limit). Per
    // fixed tick the deterministic system order is executed (placeholder
    // slots until the owning iteration-3 tasks land), then the ordered event
    // bus drains every event published for that tick. Returns the number of
    // fixed ticks executed this frame.
    std::uint32_t TickSimulation(double frame_dt);

    [[nodiscard]] std::uint64_t GetSimulationTickCount() const noexcept {
        return m_simulationClock.tick_count();
    }
    [[nodiscard]] const luminumbra::core::SimulationClock& GetSimulationClock() const noexcept {
        return m_simulationClock;
    }
    luminumbra::simulation::OrderedEventBus& GetSimulationEventBus() noexcept {
        return m_simulationEventBus;
    }

    // Set the job system (must be called before CreateWorld/LoadWorld)
    void SetJobSystem(JobSystem* jobSystem) { m_jobSystem = jobSystem; }
    void SetRootPath(const std::string& root_path) { m_rootPath = root_path; }

private:
    entt::registry m_registry;
    WorldMetadata m_metadata;
    luminumbra::core::SimulationClock m_simulationClock;
    luminumbra::simulation::OrderedEventBus m_simulationEventBus;
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_worldSystem;
    std::unique_ptr<Systems::WaterSystem> m_waterSystem;
    std::unique_ptr<Systems::WindFieldSystem> m_windFieldSystem;
    JobSystem* m_jobSystem = nullptr;

    // Generate a unique world ID
    std::string GenerateWorldId();
    std::unique_ptr<Systems::PhysicsSystem> m_physicsSystem;
    std::size_t m_lastLoadedChunkCount = 0;
    std::vector<std::filesystem::path> m_requiredClientAssets;

    // Convert string seed to numeric seed
    uint32_t StringToSeed(const std::string& seedStr);
    std::string m_rootPath; 
};

} // namespace Luminumbra::world
