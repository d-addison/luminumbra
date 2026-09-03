#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../ai/CreatureBrainSystem.h"        // ai::EcologyTuning (data-driven brain tuning, )
#include "../ai/CreatureReproductionSystem.h" // ai::ReproductionTuning
#include "../ai/ForagingSystem.h"             // ai::ForagingParams
#include "../ai/ScavengingSystem.h"           // ai::ScavengingTuning
#include "../ai/ThirstSystem.h"               // ai::ThirstTuning
#include "../ai/WildlifeFoliageSystem.h"      // ai::WildlifeFoliageTuning (full-control)
#include "../core/SimulationClock.h"
#include "../simulation/SimulationEventBus.h"
#include "../systems/PollinationSystem.h"
#include "entt/entt.hpp"
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace luminumbra::fields {
class EnergyFieldState; //  (): the stateful energy layer
}
namespace Luminumbra::scripting {
class LuaState;
}

namespace luminumbra::ai {
class ScentField;
class CreatureSpeciesRegistry; // per-world species definition table
} // namespace luminumbra::ai

namespace luminumbra::foliage {
class SoilGrid;       // soil nutrient field (systems/SoilNutrientSystem.h)
class IrrigationGrid; // soil-moisture field (systems/IrrigationSystem.h)
} // namespace luminumbra::foliage

namespace Luminumbra {
class JobSystem;

namespace Systems {
class SHIELD_WorldSystem;
class PhysicsSystem;
class WaterSystem;
class WindFieldSystem;
class WeatherSystem;
class AetherFieldSystem;
} // namespace Systems
} // namespace Luminumbra

namespace luminumbra::sim {
struct WeatherEventState; //  (): the epoch-schedule POD (WeatherEventSystem.h)
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

// Counters for one runtime world-state save pass. The on-disk layout
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

    // Initialize a new world with the given parameters.
    // customPresetJson (optional): a fully-resolved preset JSON (base preset + create-world
    // customize overrides). When provided it is written into THIS world's own save dir
    // (worlds/saves/<id>/preset.json) and used for generation, so the world is self-contained and
    // copyable — no global custom-preset files, no dangling references. worldType still records the
    // base preset name (provenance + asset validation). When null, the named preset is used.
    bool CreateWorld(const std::string& name,
                     const std::string& seed,
                     const std::string& worldType,
                     const std::string* customPresetJson);
    // 3-arg overload (named-preset path). Kept as a distinct overload — not a default arg — so
    // translation units compiled against the prior header still resolve a real symbol.
    bool
    CreateWorld(const std::string& name, const std::string& seed, const std::string& worldType) {
        return CreateWorld(name, seed, worldType, nullptr);
    }

    // Load an existing world from disk
    bool LoadWorld(const std::string& worldId);

    // Save the current world state
    bool SaveWorld();

    // --- Runtime world-state persistence ---
    // Persists streamed chunk voxel state under the canonical world save dir
    // (worlds/saves/<world_id>). Incremental contract: nothing is written when
    // no chunk carries unsaved voxel edits, so a never-edited world stays
    // byte-for-byte on the fresh-world path. The first write of an edited
    // world emits a full snapshot; later writes flush through
    // WorldSaveService::save_dirty_chunks.
    bool SaveWorldState(WorldStateSaveReport* report = nullptr);
    bool SaveWorldStateTo(const std::filesystem::path& save_dir,
                          WorldStateSaveReport* report = nullptr);

    // Loads a previously saved snapshot into the live streaming state. Must be
    // called AFTER the world systems initialize but BEFORE chunk streaming
    // generates fresh state: loaded chunks are adopted into the streaming map,
    // and generation skips chunks that already carry voxel data, so loaded
    // edits are never clobbered by regeneration. A missing snapshot is a clean
    // miss (returns false, fresh-world path unchanged).
    bool LoadWorldState();
    bool LoadWorldStateFrom(const std::filesystem::path& save_dir);
    std::size_t GetLastLoadedChunkCount() const {
        return m_lastLoadedChunkCount;
    }
    std::filesystem::path GetWorldSaveDir() const;

    // --- Asset-manifest split ---
    // The engine validates SIMULATION requirements only: a safe world type
    // and a readable, parseable world preset. Callers that additionally need
    // runtime assets (the CLIENT's shaders/RML/fonts) supply them as paths
    // relative to root_path; a headless host supplies none.
    static WorldConfigValidationResult
    ValidateWorldConfig(const std::string& root_path,
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
    const WorldMetadata& GetMetadata() const {
        return m_metadata;
    }

    // Update spawn point (useful for debugging)
    void SetSpawnPoint(const Vec3& new_spawn) {
        m_metadata.spawnPoint = new_spawn;
    }

    // Get the world system for chunk generation
    Systems::SHIELD_WorldSystem* GetWorldSystem() {
        return m_worldSystem.get();
    }
    Systems::WaterSystem* GetWaterSystem() {
        return m_waterSystem.get();
    }
    Systems::PhysicsSystem* GetPhysicsSystem() {
        return m_physicsSystem.get();
    }

    // the deterministic wind field. Sim-authoritative; its cell
    // values feed the world_hash `wind` sub-hash. Constructed on world
    // create/load (pure function of the world seed); updated per fixed tick in
    // TickSimulation around the spawn/stream anchor.
    Systems::WindFieldSystem* GetWindFieldSystem() {
        return m_windFieldSystem.get();
    }
    const Systems::WindFieldSystem* GetWindFieldSystem() const {
        return m_windFieldSystem.get();
    }

    // the deterministic weather core. Sim-authoritative; its state
    // (region category + storm cells + precip field) feeds the world_hash
    // `weather` sub-hash. Constructed on world create/load (pure function of the
    // world seed); updated per fixed tick in TickSimulation AFTER the wind field
    // (it advects storm cells by the just-updated wind), around the stream anchor.
    Systems::WeatherSystem* GetWeatherSystem() {
        return m_weatherSystem.get();
    }
    const Systems::WeatherSystem* GetWeatherSystem() const {
        return m_weatherSystem.get();
    }

    // the deterministic Aether scalar field. Sim-authoritative; its
    // cell values feed the world_hash `aether` sub-hash. Constructed on world
    // create/load (pure function of the world seed, uses seed+14); updated per
    // fixed tick in TickSimulation AFTER the weather core, around the stream
    // anchor, advected by the just-updated wind field.
    Systems::AetherFieldSystem* GetAetherFieldSystem() {
        return m_aetherFieldSystem.get();
    }
    const Systems::AetherFieldSystem* GetAetherFieldSystem() const {
        return m_aetherFieldSystem.get();
    }

    // live deterministic ecology substrate. Owned by the
    // authoritative simulation session and ticked only when game data opts an
    // entity into scent emission/sensing. The server hash folds the returned
    // scent sub-hash append-only after aether.
    luminumbra::ai::ScentField* GetScentField() {
        return m_scentField.get();
    }
    const luminumbra::ai::ScentField* GetScentField() const {
        return m_scentField.get();
    }
    [[nodiscard]] std::string ComputeScentSubHash() const;

    // scent/foraging GRID <-> WORLD mapping. Foraging cells are integers on the
    // ScentField, centered on the spawn anchor. The client colony spawner uses these to place a
    // nest + food at cells and to mirror each forager's authoritative cell back to a world
    // position for rendering. Pure helpers (no sim state); safe to call any time after init.
    [[nodiscard]] float ScentCellSize() const;
    [[nodiscard]] int ScentFieldCells() const;
    [[nodiscard]] int ScentWorldToCellX(float world_x) const;
    [[nodiscard]] int ScentWorldToCellZ(float world_z) const;
    [[nodiscard]] float ScentCellToWorldX(int cell_x) const;
    [[nodiscard]] float ScentCellToWorldZ(int cell_z) const;

    // True if any entity carries a PlantTag. The plant sub-hash folds the id-ordered
    // integer growth state for run==replay verification; empty when no plants.
    [[nodiscard]] std::string ComputePlantSubHash() const;

    entt::registry& GetRegistry() {
        return m_registry;
    }

    // Data-driven creature-brain tuning (energy/sleep/hunger/stamina/herd/
    // catch). Defaults to the compiled constants -> byte-identical until a caller overrides it from
    // SystemConfig sim.ecology (ai::ResolveEcologyTuning). Set once after construction.
    void SetEcologyTuning(const luminumbra::ai::EcologyTuning& t) {
        m_ecologyTuning = t;
    }
    [[nodiscard]] const luminumbra::ai::EcologyTuning& GetEcologyTuning() const {
        return m_ecologyTuning;
    }
    // Full-control: per-system creature tuning. All default to compiled constants -> byte-identical
    // until the client/server overrides them from SystemConfig (ai::Resolve* in SimTuningConfig.h).
    void SetWildlifeFoliageTuning(const luminumbra::ai::WildlifeFoliageTuning& t) {
        m_wildlifeFoliageTuning = t;
    }
    void SetThirstTuning(const luminumbra::ai::ThirstTuning& t) {
        m_thirstTuning = t;
    }
    void SetScavengingTuning(const luminumbra::ai::ScavengingTuning& t) {
        m_scavengingTuning = t;
    }
    void SetForagingTuning(const luminumbra::ai::ForagingParams& t) {
        m_foragingTuning = t;
    }
    void SetReproductionTuning(const luminumbra::ai::ReproductionTuning& t) {
        m_reproductionTuning = t;
    }
    void SetCircadianAmplitude(float a) {
        m_circadianAmplitude = a;
    }
    void SetPlantMutationRate(float rate) {
        m_plantMutationRate = rate < 0.0f ? 0.0f : rate;
    }
    // Opt-in weather-driven rain (sim.hydrology_weather).
    // Stored pre-world; wired into the water solver at CreateWorld/LoadWorld once the
    // weather + water systems exist. Default-OFF = null wiring = byte-identical.
    // scale_mm: full precipitation (1.0) adds this many mm/tick to a cell.
    void SetWeatherRainEnabled(bool enabled, std::int32_t scale_mm = 25) {
        m_weatherRainEnabled = enabled;
        m_weatherRainScaleMm = scale_mm;
    }
    // Opt-in High-resolution water (sim.water_high_res, experimental).
    // Stored pre-world; applied to the water solver ONCE at CreateWorld/LoadWorld —
    // immediately after construction, before its first update — because the uniform
    // hashed grid resolution can never change mid-run (the seam pass hard-gates on
    // it). Default-OFF = Medium (8x8, 2 m cells) = byte-identical; ON = High (16x16,
    // 1 m cells; the 4096-cell budget then derives a 16-chunk sim window). A loaded
    // save whose chunks were written at another resolution migrates in one boot-time
    // pass (see LoadWorldStateFrom).
    void SetWaterHighResEnabled(bool enabled) {
        m_waterHighResEnabled = enabled;
    }
    // Exposes the deterministic weather-event schedule (Markov epoch windows) on the
    // session behind sim.weather_events (default OFF -> always Clear/0: byte-identical).
    // Pure read: same (tick, world seed + 25) -> same state; a consumer must ALSO be a
    // participant (the scent/plant opt-in discipline) before its behavior may change.
    void SetWeatherEventsEnabled(bool enabled) {
        m_weatherEventsEnabled = enabled;
    }
    [[nodiscard]] luminumbra::sim::WeatherEventState CurrentWeatherEvent() const;

    // Enables the stateful energy-field layer (sim.aether_state,
    // default OFF). Stored pre-world; the layer is constructed at CreateWorld/
    // LoadWorld ONLY when enabled — OFF means no allocation, no tick work, zero
    // sub-hash bytes, saves unchanged (byte-identical by construction). The
    // layer is world-anchored sparse-page truth (the re-derivable
    // AetherFieldSystem above is the ambience half; this one holds
    // gameplay-caused deposits). Ticked in TickSimulation directly after the
    // re-derivable field, anchored on the SAME replicated stream anchor.
    void SetAetherStateEnabled(bool enabled) {
        m_aetherStateEnabled = enabled;
    }
    [[nodiscard]] bool AetherStateEnabled() const {
        return m_aetherStateEnabled;
    }
    luminumbra::fields::EnergyFieldState* GetEnergyFieldState() {
        return m_energyFieldState.get();
    }
    const luminumbra::fields::EnergyFieldState* GetEnergyFieldState() const {
        return m_energyFieldState.get();
    }
    Luminumbra::scripting::LuaState* GetScriptState() {
        return m_scriptState.get();
    }
    const Luminumbra::scripting::LuaState* GetScriptState() const {
        return m_scriptState.get();
    }
    // The aether_state:v1: additive sub-hash (StableChecksum over the layer's
    // canonical bytes), or EMPTY when the layer is off/absent/all-zero — the
    // scent/plant empty-neutral contract. The runner folds a nonempty value
    // into the |aether: world_hash slot (-3) and surfaces it as its own
    // named diagnostic for the heavy oracle's authoritative compare.
    [[nodiscard]] std::string ComputeAetherStateSubHash() const;

    // Returns the per-world species-definition table, loaded at world
    // create/load from <root>/data/common/creatures/species/*.json (filename-sorted — a pure
    // function of file content, never filesystem iteration order). Each entry's behaviour
    // blocks (IAUS "brain" overrides + "genome_ranges") default to the compiled constants,
    // so a missing directory / absent overrides resolve byte-identically to the compiled-in
    // behaviour. The creature brain and reproduction systems resolve each entity's behaviour
    // weights and genome bounds through this table. Never null after CreateWorld/LoadWorld;
    // null before either (callers must null-check).
    [[nodiscard]] const luminumbra::ai::CreatureSpeciesRegistry* GetSpeciesTable() const {
        return m_speciesTable.get();
    }

    // --- Fixed-rate simulation ---
    // Advances the 30 Hz simulation clock by one variable-dt frame and runs
    // the produced fixed ticks (clamped to the clock's catch-up limit). Per
    // fixed tick executes the deterministic system order, then the ordered event
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
    void SetJobSystem(JobSystem* jobSystem) {
        m_jobSystem = jobSystem;
    }
    void SetRootPath(const std::string& root_path) {
        m_rootPath = root_path;
    }

private:
    entt::registry m_registry;
    luminumbra::ai::EcologyTuning m_ecologyTuning{}; // Defaults match compiled constants.
    luminumbra::ai::WildlifeFoliageTuning
        m_wildlifeFoliageTuning{}; // full-control: defaults preserved
    luminumbra::ai::ThirstTuning m_thirstTuning{};
    luminumbra::ai::ScavengingTuning m_scavengingTuning{};
    luminumbra::ai::ForagingParams m_foragingTuning{};
    luminumbra::ai::ReproductionTuning m_reproductionTuning{};
    float m_circadianAmplitude = 1.0f;
    float m_plantMutationRate = luminumbra::foliage::kPollinationMutationFrac;
    WorldMetadata m_metadata;
    // Cap catch-up to 2 ticks/frame (default is 4) so a
    // single slow frame replays at most 2 sim ticks instead of 4 — halving the worst-case
    // TickSimulation spike. Scoped HERE (not the shared SimulationClock.h constant, which
    // SimulationClock_test pins at 4). Determinism-safe: over-cap ticks are already dropped (not
    // replayed) and the clamp NEVER fires under the fixed-dt headless oracle, so --smoke
    // run==replay is byte-identical.
    luminumbra::core::SimulationClock m_simulationClock{
        luminumbra::core::SimulationClock::kCanonicalTickRateHz, 2u};
    luminumbra::simulation::OrderedEventBus m_simulationEventBus;
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_worldSystem;
    std::unique_ptr<Systems::WaterSystem> m_waterSystem;
    std::unique_ptr<Systems::WindFieldSystem> m_windFieldSystem;
    std::unique_ptr<Systems::WeatherSystem> m_weatherSystem;
    std::unique_ptr<Systems::AetherFieldSystem> m_aetherFieldSystem;
    // The stateful energy layer. Null unless
    // sim.aether_state was enabled BEFORE CreateWorld/LoadWorld (default OFF —
    // null — byte-identical). unique_ptr so this header only needs the
    // forward declaration.
    std::unique_ptr<luminumbra::fields::EnergyFieldState> m_energyFieldState;
    std::unique_ptr<Luminumbra::scripting::LuaState> m_scriptState;
    bool m_aetherStateEnabled = false;
    std::unique_ptr<luminumbra::ai::ScentField> m_scentField;
    // Species definitions (see GetSpeciesTable). unique_ptr so this header only
    // needs the forward declaration (the registry is header-only nlohmann-parsing code).
    std::unique_ptr<luminumbra::ai::CreatureSpeciesRegistry> m_speciesTable;
    // Living-world substrate fields (lazily created when a participant first opts in, so a world
    // with none stays byte-identical). Anchored at the spawn point with a fixed grid extent.
    std::unique_ptr<luminumbra::foliage::SoilGrid> m_soilGrid;
    std::unique_ptr<luminumbra::foliage::IrrigationGrid> m_irrigationGrid;
    JobSystem* m_jobSystem = nullptr;

    // Generate a unique world ID
    std::string GenerateWorldId();
    std::unique_ptr<Systems::PhysicsSystem> m_physicsSystem;
    std::size_t m_lastLoadedChunkCount = 0;
    std::vector<std::filesystem::path> m_requiredClientAssets;

    // Convert string seed to numeric seed
    uint32_t StringToSeed(const std::string& seedStr) const;
    void InitializeScentField(const Vec3& anchor);
    // Construct the stateful energy layer iff
    // m_aetherStateEnabled (called at CreateWorld/LoadWorld after the
    // re-derivable aether system exists). OFF -> stays null -> byte-identical.
    void InitializeEnergyFieldState();
    // Persist the layer's record beside the chunk save (null/all-zero -> no
    // file). Serialize normalizes, which is state-idempotent at save time.
    void SaveEnergyFieldRecord(const std::filesystem::path& save_dir);
    void LoadSpeciesDefinitions();          // fills m_speciesTable (world create + load)
    void ApplyWeatherRainWiring();          // Wires weather to rain when opted in.
    void ApplyWaterResolutionWiring();      // Raises the water solver to High when opted in.
    bool m_waterHighResEnabled = false;     // See SetWaterHighResEnabled.
    bool m_weatherRainEnabled = false;      // See SetWeatherRainEnabled.
    std::int32_t m_weatherRainScaleMm = 25; // Millimetres per tick at full precipitation.
    bool m_weatherEventsEnabled = false;    // See SetWeatherEventsEnabled.
    bool HasScentParticipants() const;
    bool HasPlantParticipants() const; //  opt-in gate
    std::string m_rootPath;
};

} // namespace Luminumbra::world
