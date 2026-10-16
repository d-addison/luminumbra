#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../ai/CreatureBrainSystem.h"        // ai::EcologyTuning (data-driven brain tuning, )
#include "../ai/CreatureReproductionSystem.h" // ai::ReproductionTuning
#include "../ai/ForagingSystem.h"             // ai::ForagingParams
#include "../ai/ScavengingSystem.h"           // ai::ScavengingTuning
#include "../ai/ThirstSystem.h"               // ai::ThirstTuning
#include "../ai/WildlifeFoliageSystem.h"      // ai::WildlifeFoliageTuning (full-control)
#include "../core/SimulationClock.h"
#include "../core/SystemConfig.h"
#include "../fields/FieldPageState.h"
#include "../persistence/ChunkValidationCache.h"
#include "../simulation/SimBudgetTelemetry.h"
#include "../simulation/SimulationEventBus.h"
#include "../systems/PollinationSystem.h"
#include "../systems/SHIELD_WorldSystem.h"
#include "ActiveRegionLedger.h"
#include "AuthorityHashState.h"
#include "GroundObjectStore.h"
#include "RegionPopulator.h"
#include "WorldClock.h"
#include "WorldMetadata.h"
#include "WorldgenFileReader.h"
#include "entt/entt.hpp"
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace luminumbra::fields {
class EnergyFieldState; //  (): the stateful energy layer
}
namespace Luminumbra::scripting {
class LuaState;
}

namespace luminumbra::ai {
class ScentField;
struct CoarseBirth;
class CreatureSpeciesRegistry; // per-world species definition table
} // namespace luminumbra::ai

namespace luminumbra::foliage {
class SoilGrid; // soil nutrient field (systems/SoilNutrientSystem.h)
struct PlantEnvSample;
class IrrigationGrid; // soil-moisture field (systems/IrrigationSystem.h)
} // namespace luminumbra::foliage

namespace Luminumbra::Persistence {
struct SavedWorld;
struct WorldSaveAdmission;
} // namespace Luminumbra::Persistence

namespace Luminumbra {
class JobSystem;

namespace Systems {
struct TerrainGenParams;
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

struct WorldConfigValidationResult {
    bool ok = false;
    std::filesystem::path preset_path;
    std::vector<std::string> errors;
};

// Counters for one runtime world-state save pass. chunks_total counts resident/parked chunks;
// chunks_saved counts the selected in-memory cohort when a chunk write succeeds.
struct WorldStateSaveReport {
    std::size_t chunks_total = 0;
    std::size_t chunks_dirty = 0;
    std::size_t chunks_saved = 0;
    bool saved = false;
};

enum class WorldStateSaveScope {
    ResidentSnapshot,
    // Automatic saves retain changed authority; regenerable terrain stays a cache.
    DurableChanges
};

struct PersistenceStatus {
    std::size_t parked_count = 0;
    std::size_t parked_bytes = 0;
    std::int64_t oldest_parked_tick = -1;
    std::int64_t last_save_tick = -1;
    double last_save_ms = 0.0;
    std::uint32_t failures = 0;
    bool save_requested = false;
};

class GameSession {
public:
    GameSession();
    ~GameSession();

    // Apply one immutable effective configuration before create/load. Network peers share it.
    void ConfigureSimulation(std::shared_ptr<const luminumbra::core::SystemConfig> configuration);
    [[nodiscard]] const std::shared_ptr<const luminumbra::core::SystemConfig>&
    GetSimulationConfiguration() const {
        return m_simulationConfiguration;
    }
    // Host setup seam: forward the peer snapshot before create/load. Missing snapshots
    // and configuration replacement on a live destination refuse without mutation.
    void ConfigureSimulationFromPeer(const GameSession& peer);
    struct NetworkHostConfig {
        std::string root_path;
        std::string seed = "424242";
        std::string preset = "default";
        int surface_radius = 12;
        int collision_radius = 4;
        // Optional overrides; borrowed only for the synchronous host boot.
        // Explicit simulation rules must agree with the peer's admitted snapshot.
        const luminumbra::core::SystemConfig* systems_config_override = nullptr;
        const std::string* preset_json_override = nullptr;
    };
    // Complete headless host boot used by the network driver. The job system must outlive
    // the returned session. Failure returns null with a reason and leaves the peer unchanged.
    [[nodiscard]] static std::unique_ptr<GameSession>
    CreateNetworkHost(const GameSession& peer,
                      JobSystem& jobs,
                      const NetworkHostConfig& config,
                      std::string& failure_reason);
    // Network checkpoint recipe is chosen by the shared configuration. Refuses local flag drift.
    bool ComputeNetworkWorldHashes(const luminumbra::core::SystemConfig& configuration,
                                   std::string& world_hash,
                                   Persistence::WorldStreamingStateSubHashes& sub_hashes);

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

    // Menu scenery has no save directory and never appears in the player catalog.
    bool CreateTransientWorld(const std::string& name,
                              const std::string& seed,
                              const std::string& worldType);

    // Load an existing world from disk. Enabled sessions publish only a fully restored
    // candidate; refusal preserves the current world and its ability to save.
    bool LoadWorld(const std::string& worldId);
    // Uses the preset loader's status-reader seam for deterministic dependency I/O.
    bool LoadWorld(const std::string& worldId, const World::WorldgenFileStatusReader& read_status);
    using WorldOpenCheck = std::function<std::string(
        const Persistence::SavedWorld&, const Systems::TerrainGenParams&, std::uint32_t)>;
    // Called once after metadata/preset admission, before chunk validation or terrain planning.
    // An empty diagnostic accepts the identity; ordinary save validation still follows.
    bool LoadWorld(const std::string& worldId, const WorldOpenCheck& check);

    // Owner thread only. Reinspect current files for identity consumers, reusing exact-byte
    // chunk validation facts from admission/saves while preserving catalog refusal checks.
    Persistence::SavedWorld InspectCurrentSavedWorld();

    // Save the current world state
    bool SaveWorld();
    // Flag on: runs `body` (which may call SaveWorldState/SaveWorld and indexed-member writers)
    // inside ONE snapshot transaction published with a single index. Otherwise just runs `body`.
    // Owner thread only; returns the body's verdict, and false when the commit fails.
    bool RunInSnapshotTransaction(const std::function<bool()>& body);

    // --- Runtime world-state persistence ---
    // Persists streamed chunk voxel state under the canonical world save dir
    // (worlds/saves/<world_id>). Incremental contract: nothing is written when
    // no chunk carries unsaved voxel edits, so a never-edited world stays
    // byte-for-byte on the fresh-world path. The first write of an edited
    // world emits the selected snapshot; later writes flush through
    // WorldSaveService::save_dirty_chunks. Automatic hosts select DurableChanges;
    // explicit snapshots retain generated resident cache coverage by default.
    bool SaveWorldState(WorldStateSaveReport* report = nullptr,
                        WorldStateSaveScope scope = WorldStateSaveScope::ResidentSnapshot);
    bool SaveWorldStateTo(const std::filesystem::path& save_dir,
                          WorldStateSaveReport* report = nullptr,
                          WorldStateSaveScope scope = WorldStateSaveScope::ResidentSnapshot);

    // Derived validation reuse only; excluded from persistent and hashed state.
    [[nodiscard]] Persistence::ChunkValidationCache::Stats GetChunkValidationCacheStats() const {
        return m_chunkValidationCache->stats();
    }

    // Durable residency (sim.active_regions only). All zero/false when the flag is off.
    [[nodiscard]] PersistenceStatus GetPersistenceStatus() const;
    // True when the world asked for a save (a dirty authoritative chunk hit the parked byte
    // limit) and the retry back-off after a failed save has elapsed.
    [[nodiscard]] bool PersistenceSaveRequested() const;
    void ClearPersistenceSaveRequest();
    // 0 = unlimited. Applied when the world system is wired (and immediately if it exists).
    void SetParkedBytesLimit(std::size_t bytes);
    static constexpr std::uint64_t kSaveRetryTicks = 300;
    static constexpr std::uint64_t kAutosaveTicks = 9000;
    // Periodic autosave for hosts without their own interval (the client). True once kAutosaveTicks
    // have elapsed since the last successful save (or since first observed), outside the failure
    // back-off. Always false unless sim.active_regions is on.
    [[nodiscard]] bool PersistenceAutosaveDue();

    // Loads a previously saved snapshot into the live streaming state. Must be
    // called AFTER the world systems initialize but BEFORE chunk streaming
    // generates fresh state: loaded chunks are adopted into the streaming map,
    // and generation skips chunks that already carry voxel data, so loaded
    // edits are never clobbered by regeneration. A missing snapshot is a clean
    // miss (returns true). A state-load refusal disables saving; a rejected world
    // replacement preserves the current world and its saveability.
    const std::string& GetWorldOpenError() const {
        return m_worldOpenError.empty() ? m_worldLoadError : m_worldOpenError;
    }
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

    const entt::registry& GetRegistry() const {
        return m_registry;
    }

    // Data-driven creature-brain tuning (energy/sleep/hunger/stamina/herd/
    // catch). Defaults to the compiled constants -> byte-identical until a caller overrides it from
    // SystemConfig sim.ecology (ai::ResolveEcologyTuning). Set once after construction.
    // Copies the effective configuration into an immutable snapshot before create/load.
    // Like ConfigureSimulation, replacement on a live session refuses without mutation.
    void ApplySystemConfig(const luminumbra::core::SystemConfig& config);
    // Computed on demand from the current effective simulation gates and tuning.
    [[nodiscard]] std::string SystemConfigIdentity() const;
    [[nodiscard]] const std::string& PresetContentHash() const {
        return m_presetContentHash;
    }
    [[nodiscard]] std::map<std::string, bool> EffectiveSimulationGates() const;

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
    // Weather-driven rain is enabled by default (sim.hydrology_weather).
    // Stored pre-world; wired into the water solver at CreateWorld/LoadWorld once the
    // weather + water systems exist. Explicit OFF = null wiring = byte-identical.
    // scale_mm: full precipitation (1.0) adds this many mm/tick to a cell.
    void SetWeatherRainEnabled(bool enabled, std::int32_t scale_mm = 25) {
        m_weatherRainEnabled = enabled;
        m_weatherRainScaleMm = scale_mm;
    }
    // Stored pre-world; ordinary Create/Load conserves finite water.
    void SetFiniteHydrologyEnabled(bool enabled) {
        m_finiteHydrologyEnabled = enabled;
    }
    [[nodiscard]] bool FiniteHydrologyEnabled() const {
        return m_finiteHydrologyEnabled;
    }
    [[nodiscard]] bool WeatherRainEnabled() const {
        return m_weatherRainEnabled;
    }
    [[nodiscard]] bool WeatherEventsEnabled() const {
        return m_weatherEventsEnabled;
    }
    // High-resolution water is enabled by default (sim.water_high_res).
    // Stored pre-world; applied once at CreateWorld/LoadWorld before the first update.
    // The chosen resolution affects water/world hashes and must match on every peer.
    // Explicit OFF = Medium (8x8, 2 m cells); ON = High (16x16, 1 m cells), whose
    // 4096-cell budget derives a 16-chunk sim window. Loading a current-format save
    // resizes its water grids in one boot-time pass if needed (LoadWorldStateFrom).
    void SetWaterHighResEnabled(bool enabled) {
        m_waterHighResEnabled = enabled;
    }
    // Exposes the deterministic weather-event schedule (Markov epoch windows) on the
    // session behind sim.weather_events (explicit OFF -> always Clear/0: byte-identical).
    // Pure read: same (tick, world seed + 25) -> same state; a consumer must ALSO be a
    // participant (the scent/plant opt-in discipline) before its behavior may change.
    void SetWeatherEventsEnabled(bool enabled) {
        m_weatherEventsEnabled = enabled;
    }
    [[nodiscard]] luminumbra::sim::WeatherEventState CurrentWeatherEvent() const;

    // Enables the stateful energy-field layer (sim.aether_state,
    // explicit OFF). Stored pre-world; the layer is constructed at CreateWorld/
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

    // Immutable per-world creature definitions. Enabled worlds preflight the full species
    // inventories and resolved policy; disabled worlds retain lenient legacy loading.
    [[nodiscard]] const luminumbra::ai::CreatureSpeciesRegistry* GetSpeciesTable() const {
        return m_speciesTable.get();
    }
    [[nodiscard]] const SimulationContent* GetSimulationContent() const noexcept {
        return m_simulationContent.get();
    }
    // Identity inputs only. A registered populator supplies its versioned contract and
    // canonical simulation configuration before create/load; callback execution is separate.
    void SetSimulationPopulationIdentity(std::string contract,
                                         std::string canonical_configuration = {}) {
        if (m_worldSystem || m_simulationContent)
            throw std::logic_error("Set population identity before creating or opening a world");
        if (m_regionPopulator && (contract != m_populationContract ||
                                  canonical_configuration != m_populationConfiguration))
            throw std::logic_error("Registered population identity is immutable");
        m_populationContract = std::move(contract);
        m_populationConfiguration = std::move(canonical_configuration);
    }

    void SetRegionPopulator(RegionPopulator populator);
    void SetActivationEnqueuePreparationHook(std::function<void()> hook) {
        m_activationEnqueuePreparation = std::move(hook);
    }
    void SetPopulationPreparationHooks(PopulationPreparationHooks hooks) {
        m_populationPreparation = std::move(hooks);
    }
    struct PopulationActivity {
        std::uint64_t successes = 0, failures = 0, pending_activations = 0;
    };
    [[nodiscard]] PopulationActivity GetPopulationActivity() const noexcept {
        auto result = m_populationActivity;
        result.pending_activations = m_activeRegionLedger.pending_activations().size();
        return result;
    }

    // Set before creating/opening a world, like the other sim feature keys.
    void SetActiveRegionsEnabled(bool enabled) {
        if (m_worldSystem && enabled != m_activeRegionsEnabled)
            throw std::logic_error("Set active regions before creating or opening a world");
        m_activeRegionsEnabled = enabled;
        m_simulationEventBus.set_durable(enabled);
    }
    [[nodiscard]] bool ActiveRegionsEnabled() const {
        return m_activeRegionsEnabled;
    }
    [[nodiscard]] const WorldClock& GetWorldClock() const {
        return m_worldClock;
    }
    [[nodiscard]] const ActiveRegionLedger& GetActiveRegionLedger() const {
        return m_activeRegionLedger;
    }
    [[nodiscard]] const RegionSchedule& GetRegionSchedule() const {
        return m_regionSchedule;
    }
    [[nodiscard]] const GroundObjectStore& GetGroundObjects() const {
        return m_groundObjects;
    }
    [[nodiscard]] const FieldPageState& GetFieldPages() const {
        return m_fieldPages;
    }
    // Main-thread admission/edit seams. Enabled edits are transactional and own
    // their region before publishing any cell or identity.
    bool AdmitPlant(entt::entity entity);
    bool AdmitCreature(entt::entity entity, bool physics_controlled = false);
    struct CreatureActivity {
        std::uint64_t full = 0, coarse = 0, frozen = 0;
        std::uint64_t promotions = 0, demotions = 0, full_updates = 0, coarse_updates = 0;
        std::uint64_t births = 0, deaths = 0, removals = 0, parked_travel = 0;
        std::uint64_t controllers_created = 0, controllers_removed = 0, stale_handles = 0;
    };
    [[nodiscard]] CreatureActivity GetCreatureActivity() const;
    // Preparation hook for deterministic controller-construction fault coverage.
    void SetCreatureControllerPreparationHook(std::function<void()> hook) {
        m_creatureControllerPreparation = std::move(hook);
    }

    bool SetFoliageCell(const Vec3& position, bool soil, std::int32_t value);
    [[nodiscard]] std::int32_t SoilNutrientAt(const Vec3& position) const;
    [[nodiscard]] std::int32_t IrrigationAt(const Vec3& position) const;
    // A fatal scheduler/obligation fault. Once latched the session is fail-stop: the
    // simulation clock is rewound to the faulting tick, no further ticks run, and hosts
    // are expected to save and stop. Saving a faulted session is allowed.
    struct RegionSchedulerFault {
        std::uint64_t tick = 0;
        std::string message;
    };
    // Observational counters of rejected host inputs; never hashed, digested or saved.
    struct RegionInputRejections {
        std::uint32_t anchors = 0, edits = 0, pins = 0;
        std::uint32_t ground_spawns = 0;
        std::uint32_t field_pages = 0;
    };
    [[nodiscard]] const std::optional<RegionSchedulerFault>& GetRegionSchedulerFault() const {
        return m_regionFault;
    }
    [[nodiscard]] const RegionInputRejections& GetRegionInputRejections() const {
        return m_regionInputRejections;
    }
    // Set before world creation. An absent ledger preserves these settings; a persisted
    // ledger must match an explicit override or loading refuses. Without an override,
    // loaded settings become the effective configuration, also retained by world resets.
    void SetRegionSchedulerConfig(RegionSchedulerConfig config);
    void SetLocalPlayerSimulationPosition(const Vec3& feet, bool walking);
    void SetReplicatedSimulationAnchors(std::vector<Vec3> anchors);
    void NotifyGroundObjectEdit(const Vec3& position);
    void PinActiveRegion(RegionKey key, bool pinned);
    void RecordActiveRegionWork(RegionWork work);
    // Main-thread only, like the other host inputs. Returns the new persistent id, or 0 when the
    // flag is off (no state touched) or the spawn is refused.
    std::uint64_t SpawnGroundObject(std::uint32_t type,
                                    std::int64_t x_mm,
                                    std::int64_t y_mm,
                                    std::int64_t z_mm,
                                    std::uint64_t despawn_after,
                                    std::uint32_t payload);
    std::uint64_t SpawnGroundObject(std::uint32_t type,
                                    Vec3 position,
                                    std::uint32_t despawn_after,
                                    std::uint32_t payload);

    // Fold only when enabled, inside the existing ecology hash slot.
    [[nodiscard]] std::string FoldClockIntoEcologyHash(const std::string& ecology_hash) const;
    // Enabled chunk slot hashes authoritative truth across residency/save history. The default
    // path retains the existing streamed-state projection and wire/checkpoint slots.
    [[nodiscard]] std::string ComputeChunkStateHash();
    // Checked read at a completed boundary. The host may supply its canonical avatar snapshot.
    // Success owns only immutable projections; failure contains no digest and changes no state.
    [[nodiscard]] AuthorityHashCapture
    CaptureAuthorityState(const std::string& avatar_snapshot = {}) const;
    // Checked enabled host capture. Failure clears outputs and latches the first fault.
    [[nodiscard]] bool
    ComputeAuthorityWorldHashes(std::string& world_hash,
                                Persistence::WorldStreamingStateSubHashes& sub_hashes,
                                const std::string& avatar_snapshot = {});

    [[nodiscard]] bool IsSimulationTickBoundary() const {
        return !m_activeRegionsEnabled || (!m_simulationBatchInProgress && m_regionWork.empty());
    }

    // --- Fixed-rate simulation ---
    // Advances the 30 Hz simulation clock by one variable-dt frame and runs
    // the produced fixed ticks (clamped to the clock's catch-up limit). Per
    // fixed tick executes the deterministic system order, then the ordered event
    // bus drains every event published for that tick. Returns the number of
    // fixed ticks executed this frame.
    std::uint32_t TickSimulation(double frame_dt);

    // Opt-in observation; never part of persistent or hashed session state.
    void SetSimBudgetTelemetryEnabled(bool enabled) {
        m_simBudgetTelemetry.SetEnabled(enabled);
    }
    [[nodiscard]] luminumbra::simulation::SimBudgetTelemetry& GetSimBudgetTelemetry() {
        return m_simBudgetTelemetry;
    }
    [[nodiscard]] const luminumbra::simulation::SimBudgetTelemetry& GetSimBudgetTelemetry() const {
        return m_simBudgetTelemetry;
    }

    [[nodiscard]] std::uint64_t GetSimulationTickCount() const noexcept {
        return m_activeRegionsEnabled ? m_worldClock.tick() : m_simulationClock.tick_count();
    }
    [[nodiscard]] const luminumbra::core::SimulationClock& GetSimulationClock() const noexcept {
        return m_simulationClock;
    }
    luminumbra::simulation::OrderedEventBus& GetSimulationEventBus() noexcept {
        return m_simulationEventBus;
    }

    const luminumbra::simulation::OrderedEventBus& GetSimulationEventBus() const noexcept {
        return m_simulationEventBus;
    }

    // Owner-thread const observation of session authority outside the stores and registry.
    // References are valid only while this session lives and until its next mutation.
    struct WorldAuthorityView {
        std::vector<std::shared_ptr<const Chunk>> live;
        std::vector<std::shared_ptr<const Chunk>> parked;
        std::vector<IVec3> durable;
        int seed;
        const Systems::TerrainGenParams& params;
        bool biomes_enabled;
        const std::vector<World::BiomeDefinition>& biomes;
        bool structures_enabled;
        const std::vector<World::StructureTemplatePool>& structure_pools;
    };
    [[nodiscard]] std::optional<WorldAuthorityView> ObserveWorldAuthority() const {
        if (!m_worldSystem)
            return std::nullopt;
        const auto live = m_worldSystem->snapshot_streamed_chunks();
        const auto parked = m_worldSystem->snapshot_parked_chunks();
        return WorldAuthorityView{{live.begin(), live.end()},
                                  {parked.begin(), parked.end()},
                                  m_worldSystem->durable_chunk_inventory(),
                                  m_worldSystem->get_seed(),
                                  m_worldSystem->get_params(),
                                  m_worldSystem->biomes_enabled(),
                                  m_worldSystem->biome_table().biomes(),
                                  m_worldSystem->structures_enabled(),
                                  m_worldSystem->structure_pools()};
    }
    struct PopulationAuthorityView {
        const std::string& contract;
        const std::string& configuration;
        const std::map<std::uint32_t, std::uint32_t>& ground_types;
    };
    struct SimulationAuthorityView {
        const std::filesystem::path& durable_directory;
        const Vec3& ambient_anchor;
        const std::string& population_contract;
        const std::string& population_configuration;
        std::optional<PopulationAuthorityView> population;
        bool water_high_res;
        bool weather_rain;
        std::int32_t weather_rain_scale_mm;
        bool weather_events;
        const luminumbra::ai::EcologyTuning& ecology;
        const luminumbra::ai::WildlifeFoliageTuning& wildlife_foliage;
        const luminumbra::ai::ThirstTuning& thirst;
        const luminumbra::ai::ScavengingTuning& scavenging;
        const luminumbra::ai::ReproductionTuning& reproduction;
        const luminumbra::ai::ForagingParams& foraging;
        float circadian_amplitude;
        float plant_mutation_rate;
    };
    [[nodiscard]] SimulationAuthorityView ObserveSimulationAuthority() const noexcept {
        return {m_regionDurableDirectory,
                m_ambientFieldAnchor,
                m_populationContract,
                m_populationConfiguration,
                m_regionPopulator ? std::optional<PopulationAuthorityView>{PopulationAuthorityView{
                                        m_regionPopulator->contract,
                                        m_regionPopulator->canonical_configuration,
                                        m_regionPopulator->ground_types}}
                                  : std::nullopt,
                m_waterHighResEnabled,
                m_weatherRainEnabled,
                m_weatherRainScaleMm,
                m_weatherEventsEnabled,
                m_ecologyTuning,
                m_wildlifeFoliageTuning,
                m_thirstTuning,
                m_scavengingTuning,
                m_reproductionTuning,
                m_foragingTuning,
                m_circadianAmplitude,
                m_plantMutationRate};
    }

    // Set the job system (must be called before CreateWorld/LoadWorld)
    void SetJobSystem(JobSystem* jobSystem) {
        m_jobSystem = jobSystem;
    }
    void SetRootPath(const std::string& root_path) {
        m_rootPath = root_path;
    }

private:
    bool LoadWorldChecked(const std::string& worldId,
                          const World::WorldgenFileStatusReader& read_status,
                          const WorldOpenCheck& check);
    std::unique_ptr<Persistence::ChunkValidationCache> m_chunkValidationCache =
        std::make_unique<Persistence::ChunkValidationCache>();

    luminumbra::simulation::SimBudgetTelemetry m_simBudgetTelemetry;
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
    bool m_transientWorld = false;
    void ResetWorldSystems(bool keep_admitted_chunk_facts = false);
    bool AdoptWorldState(const std::filesystem::path& save_dir,
                         Persistence::WorldSaveAdmission* admission);
    bool LoadWorldInternal(const std::string& worldId,
                           const World::WorldgenFileStatusReader& read_status,
                           const WorldOpenCheck& check);
    void RestoreWorldClock(const WorldClock& clock);
    void RestoreAmbientFieldsAtTick(std::uint64_t tick);
    bool SaveWorldMetadataTo(const std::filesystem::path& save_dir);
    bool ClockConfigurationCompatible(const std::filesystem::path& save_dir) const;
    WorldClock m_worldClock;
    void AttachRegionEditObserver();
    void RecordPersistenceSaveOutcome(bool ok, double elapsed_ms);
    std::size_t m_parkedBytesLimit = static_cast<std::size_t>(256) * 1024u * 1024u;
    std::int64_t m_lastSaveTick = -1;
    double m_lastSaveMs = 0.0;
    std::uint32_t m_saveFailures = 0;
    std::int64_t m_lastSaveFailureTick = -1;
    RegionSchedulerConfig m_regionSchedulerConfig;
    std::optional<RegionSchedulerConfig> m_regionSchedulerOverride;
    ActiveRegionLedger m_activeRegionLedger;
    void ReplaceActiveRegionLedger(ActiveRegionLedger ledger);
    RegionSchedule m_regionSchedule;
    world::RegionScheduleScratch m_regionScheduleScratch;
    mutable std::vector<RegionKey> m_sortedDueRegions, m_sortedDueSource;
    std::optional<std::vector<Vec3>> m_replicatedSimulationAnchors;
    std::vector<RegionWork> m_regionWork;          // External inputs for the next schedule call.
    std::vector<RegionWork> m_regionCompletedWork; // Transient successful consumer work this tick.
    std::optional<RegionSchedulerFault> m_regionFault;
    RegionInputRejections m_regionInputRejections;
    // Authoritative in-memory ground objects (S5). Empty unless spawned with the flag on.
    GroundObjectStore m_groundObjects;
    // Authoritative in-memory field pages (S6). Empty unless the region flag is on.
    FieldPageState m_fieldPages;
    FieldPageState m_previousFieldPages;
    FieldPageState::ScheduledScratch m_fieldScheduleScratch;
    void LatchRegionFault(std::uint64_t tick, std::string message);
    void RunRegionScheduler(std::uint64_t current_tick);
    [[nodiscard]] AuthorityHashCapture
    CaptureAuthorityStateImpl(const std::string& avatar_snapshot = {}) const;
    [[nodiscard]] std::string AuthorityIdentityBytes() const;
    // Sorted, de-duplicated due regions of the current schedule.
    [[nodiscard]] const std::vector<RegionKey>& SortedDueRegions() const;
    // Ages the due, owned regions' ground objects by one tick and publishes despawns.
    void AdvanceGroundObjects(std::uint64_t tick);
    // Activates and steps the due, ledger-backed regions' field pages by one tick.
    void AdvanceFieldPages(std::uint64_t tick);
    bool AdmitPlantAt(entt::entity entity, std::uint64_t fresh_stamp);
    void PrepareRegionalFoliage();
    struct CreatureTickAdmission {
        struct Previous {
            RegionKey owner;
            Components::CreatureTier tier;
            std::uint64_t id;
            Vec3 position;
            bool alive;
        };
        std::uint64_t tick = 0;
        std::map<entt::entity, Previous> previous;
        std::map<entt::entity, RegionKey> positional_owners;
        std::set<entt::entity> full, coarse_corpses, positional_sources, transient_grazers;
        std::set<entt::entity> processed;
        std::set<RegionKey> processed_owners;
    } m_creatureTickAdmission;
    void SealCreatureTickAdmission(std::uint64_t tick);
    void CompleteCreatureTickAdmission();
    bool CreatureFullEligible(entt::entity entity) const;
    bool EnergyCellDue(int x, int z) const;
    bool CreatureGrazerEligible(entt::entity entity) const;
    void RecordCreatureOperation(entt::entity entity);
    bool AdmitCreatureBirth(const luminumbra::ai::CreatureBirthProposal& birth);
    const CreatureDistantPolicy& CreaturePolicy(std::uint16_t species) const;
    CreatureActivity m_creatureActivity;
    std::function<void()> m_creatureControllerPreparation;
    void ReconcileCreatureTiers();
    void RunCoarseCreatureTick();
    void StepCreaturePhysics();
    void RunFullCreatureBrain();
    void CompleteCreatureMovement(entt::entity entity, Vec3 position);
    void PublishCreaturePosition(entt::entity entity, const Vec3& position, RegionKey owner);
    void SynchronizeCreatureLocomotion(entt::entity entity);
    void CaptureFullCreature(entt::entity entity);
    void ImportCoarseCreature(entt::entity entity);
    entt::entity AdmitCoarseBirth(const luminumbra::ai::CoarseBirth& birth,
                                  const std::map<std::uint64_t, entt::entity>& parents);
    void RunRegionalEnergyTick(std::uint64_t tick);
    void RunRegionalWaterTick();
    void RunRegionalFoliageTick(luminumbra::simulation::SimBudgetTelemetry::TickScope& budget,
                                std::uint64_t tick);
    void RunRegionalFoliageTickImpl(luminumbra::simulation::SimBudgetTelemetry::TickScope& budget,
                                    std::uint64_t tick,
                                    const FieldPageState& previous,
                                    const std::map<RegionKey, std::uint32_t>& windows,
                                    const std::map<RegionKey, std::uint64_t>& resumes,
                                    std::map<std::uint64_t, std::uint32_t> plant_windows);
    void RunFoliageTick(luminumbra::simulation::SimBudgetTelemetry::TickScope& budget,
                        std::uint64_t tick);
    [[nodiscard]] luminumbra::foliage::PlantEnvSample
    SamplePlantEnvironment(const Components::TransformComponent& transform,
                           std::uint64_t tick) const;
    std::map<RegionKey, std::uint64_t> m_foliagePreviousTicks;

    void BindRegionalFieldQueries();
    [[nodiscard]] std::uint16_t SampleRegionalChannel(int channel, const Vec3& position) const;
    [[nodiscard]] std::string RegionalAmbientHash(std::uint8_t channels, bool weather) const;
    void ProjectRegionalWeather(bool restoring = false);
    void AdvanceRegionalWeather(std::uint64_t tick);
    [[nodiscard]] float ScentDomainOrigin(bool x_axis) const;
    [[nodiscard]] RegionKey ScentCellOwner(int x, int z) const;
    [[nodiscard]] bool ScentCellDue(int x, int z) const;
    [[nodiscard]] bool ScentEdgeOpen(int x, int z, int nx, int nz) const;
    void RestoreRegionalScent();
    void CaptureRegionalScent();
    // Capture workspace only; cleared before use and excluded from save/hash state.
    std::vector<std::pair<ScentCellKey, ScentCell>> m_scentCaptureUpdates;
    // Ledger edit mark for a ground object's region. False when refused (fault or counted reject).
    bool MarkGroundRegionEdited(const GroundObject& object);
    std::filesystem::path m_regionDurableDirectory;
    // Stable only in the clock slice; world-anchored pages replace this grid in C4.
    Vec3 m_ambientFieldAnchor{};
    std::string m_presetContentHash;
    luminumbra::core::SystemConfig m_appliedSystemConfig =
        luminumbra::core::SystemConfig::Defaults();
    bool m_activeRegionsEnabled = true;
    bool m_simulationBatchInProgress = false;
    bool m_activationDeliveryInProgress = false;
    [[nodiscard]] bool CanSaveWorldNow() const {
        return IsSimulationTickBoundary() ||
               (m_activeRegionsEnabled && m_activationDeliveryInProgress && m_regionWork.empty());
    }
    bool CreateWorldInternal(const std::string& name,
                             const std::string& seed,
                             const std::string& worldType,
                             const std::string* customPresetJson,
                             bool transient);
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
    // sim.aether_state was enabled BEFORE CreateWorld/LoadWorld (explicit OFF —
    // null — byte-identical). unique_ptr so this header only needs the
    // forward declaration.
    std::unique_ptr<luminumbra::fields::EnergyFieldState> m_energyFieldState;
    std::unique_ptr<Luminumbra::scripting::LuaState> m_scriptState;
    bool m_aetherStateEnabled = true;
    std::unique_ptr<luminumbra::ai::ScentField> m_scentField;
    // Immutable species registries and enabled content, adopted once per world.
    std::shared_ptr<const luminumbra::ai::CreatureSpeciesRegistry> m_speciesTable;
    std::shared_ptr<const SimulationContent> m_simulationContent;
    std::string m_populationContract = std::string(kDefaultPopulationContract);
    std::string m_populationConfiguration;
    std::shared_ptr<const RegionPopulator> m_regionPopulator;
    PopulationPreparationHooks m_populationPreparation;
    std::function<void()> m_activationEnqueuePreparation;
    PopulationActivity m_populationActivity;
    [[nodiscard]] luminumbra::simulation::OrderedEventBus::PreparedBatch
    PrepareRegionActivations(const ActiveRegionLedger& ledger,
                             const luminumbra::simulation::OrderedEventBus* source = nullptr) const;
    void QueueRegionActivations();
    void DrainSimulationEvents(std::uint64_t current_tick);
    bool PrepareRegionTick(std::uint64_t current_tick);
    void PopulateActiveRegions();
    void StagePopulationCreature(entt::registry& stage,
                                 const CreatureSpawnCommand& command,
                                 std::uint64_t id,
                                 RegionKey key,
                                 std::uint64_t tick,
                                 std::uint64_t draw) const;
    // Living-world substrate fields (lazily created when a participant first opts in, so a world
    // with none stays byte-identical). Anchored at the spawn point with a fixed grid extent.
    std::unique_ptr<luminumbra::foliage::SoilGrid> m_soilGrid;
    std::unique_ptr<luminumbra::foliage::IrrigationGrid> m_irrigationGrid;
    JobSystem* m_jobSystem = nullptr;

    // Generate a unique world ID
    std::string GenerateWorldId();
    std::unique_ptr<Systems::PhysicsSystem> m_physicsSystem;
    std::string m_worldOpenError;
    // A rejected replacement leaves the current world usable, including its save path.
    std::string m_worldLoadError;
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
    bool SaveEnergyFieldRecord(const std::filesystem::path& save_dir);
    bool PreflightSimulationContent(std::shared_ptr<const SimulationContent>& content);
    bool LoadSpeciesDefinitions(std::shared_ptr<const SimulationContent> content = {});
    void ApplyWeatherRainWiring();     // Wires weather to rain when opted in.
    void ApplyWaterResolutionWiring(); // Raises the water solver to High when opted in.
    std::shared_ptr<const luminumbra::core::SystemConfig> m_simulationConfiguration;
    bool m_finiteHydrologyEnabled = true;
    bool m_waterHighResEnabled = true;      // See SetWaterHighResEnabled.
    bool m_weatherRainEnabled = true;       // See SetWeatherRainEnabled.
    std::int32_t m_weatherRainScaleMm = 25; // Millimetres per tick at full precipitation.
    bool m_weatherEventsEnabled = true;     // See SetWeatherEventsEnabled.
    bool HasScentParticipants(bool require_due = true) const;
    bool HasScentBorderWork() const;
    void AdvanceNearScent();
    bool HasPlantParticipants() const; //  opt-in gate
    std::string m_rootPath;
};

} // namespace Luminumbra::world
