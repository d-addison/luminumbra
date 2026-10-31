#include "GameSession.h"
#include "../ai/CreatureBrain.h"
#include "../components/CreatureComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/ForagingComponents.h"
#include "../components/ScavengerComponent.h"
#include "../components/ThirstComponents.h"
#include "../core/SystemConfig.h"
#include "../simulation/SimBudgetTelemetry.h"
#include "ActiveRegionLedger.h"
#include "Chunk.h"
#include "entt/entity/fwd.hpp"
#include "entt/entt.hpp"
#include "luminumbra/core/Types.h"
#include "nlohmann/json_fwd.hpp"

#include "../ai/CircadianSystem.h" // Diurnal and nocturnal activity.
#include "../ai/CreatureBrainSystem.h"
#include "../ai/CreatureReproductionSystem.h" // Generational evolution.
#include "../ai/DecompositionSystem.h"        // Releases nutrients after death.
#include "../ai/EcologyTuningConfig.h"
#include "../ai/ForagingSystem.h"  // ant-trail foraging (Deneubourg double-bridge)
#include "../ai/HerdAlarmSystem.h" // Collective-vigilance alarm.
#include "../ai/InstinctLocomotionSystem.h"
#include "../ai/InstinctSystem.h"
#include "../ai/LifespanSystem.h"  // Age and starvation mortality.
#include "../ai/MigrationSystem.h" // Seasonal migration drive.
#include "../ai/PerceptionSystem.h"
#include "../ai/PredatorPackSystem.h" // Pack flanking coordination.
#include "../ai/ScavengingSystem.h"   // Converts carcasses into scavenger food.
#include "../ai/ScentDepositSystem.h"
#include "../ai/ScentField.h"
#include "../ai/ScentSteeringSystem.h"
#include "../ai/SimTuningConfig.h"
#include "../ai/SteeringConsumer.h" // Blends steering biases into desired motion.
#include "../ai/StimulusChannels.h"
#include "../ai/TerritorySystem.h"       // Home territory and homing bias.
#include "../ai/ThirstSystem.h"          // Water seeking and drinking.
#include "../ai/WildlifeFoliageSystem.h" // Grazing and trampling.
#include "../animation/AnimationRuntime.h"
#include "../components/AlarmComponents.h"
#include "../components/CircadianComponents.h"
#include "../components/CombustionComponents.h"
#include "../components/CoreComponents.h"
#include "../components/DecayComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/GrazeableComponent.h"
#include "../components/InstinctComponents.h"
#include "../components/IrrigationComponents.h"
#include "../components/MigratoryComponents.h"
#include "../components/MortalComponents.h"
#include "../components/PackHunterComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../components/SoilComponents.h"
#include "../components/TerritoryComponents.h"
#include "../core/FilesystemPath.h"
#include "../core/JobSystem.h"
#include "../core/Log.h"
#include "../fields/EnergyFieldState.h"
#include "../persistence/CreatedWorldArtifacts.h"
#include "../persistence/PlantPersistence.h"
#include "../persistence/SaveTransaction.h"
#include "../persistence/SavedEnergyInput.h"
#include "../persistence/SavedJsonAdmission.h"
#include "../persistence/SavedWorldCatalog.h"
#include "../persistence/WorldPersistenceRoundtrip.h" // Persistence::StableChecksum (ComputeScentSubHash)
#include "../persistence/WorldSaveService.h"
#include "../scripting/LuaState.h"
#include "../simulation/SimWorkCounters.h"
#include "../systems/AetherFieldSystem.h"
#include "../systems/CropLifecycleSystem.h"
#include "../systems/FieldEmitterSystem.h" // Gathers field-emitter deposits.
#include "../systems/FireSpreadSystem.h"   // Fire propagation.
#include "../systems/IrrigationSystem.h"   // Soil-moisture irrigation.
#include "../systems/PhysicsSystem.h"
#include "../systems/PlantDiseaseSystem.h" // Plant disease propagation.
#include "../systems/PlantGrowthSystem.h"  // Plant growth tick.
#include "../systems/PollinationSystem.h"  // Cross-pollination.
#include "../systems/SHIELD_WorldSystem.h" // This includes TerrainGenParams
#include "../systems/SoilNutrientSystem.h" // Soil-nutrient cycling.
#include "../systems/WaterSystem.h"
#include "../systems/WeatherEventSystem.h" // Pure deterministic weather-event schedule.
#include "../systems/WeatherSystem.h"
#include "../systems/WindFieldSystem.h"
#include "TerrainPresetLoader.h"
#include "WorldStreamingState.h"
#include "entt/entity/entity.hpp"
#include "nlohmann/json.hpp" // For parsing JSON
#include <array>
#include <atomic>
#include <cmath> // std::floor (energy-layer anchor quantization)
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <ios>
#include <map>
#include <span>
#include <string>
#include <system_error>

#include <algorithm> // explicit: stabilise std template instantiation for entt storage helpers
#include <chrono>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator> // (GCC-15 vague-linkage: must be visible at the GrazeableComponent storage
#include <memory>   //  instantiation point, else std::__advance/iter_move get externalized)
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// A using-declaration to make the code cleaner. This brings the struct into the current scope.
using Luminumbra::Systems::TerrainGenParams;

namespace {
constexpr float kSpawnEyeHeight = 1.95f;
constexpr int kScentFieldCells = 128;
constexpr int kScentFieldChannels = 4;
constexpr float kScentCellSize = 1.0f;
constexpr double kScentDiffusionRate = 0.25;
constexpr std::size_t kScentDiffusionIterations = 4;
constexpr double kScentEvaporation = 0.05;
constexpr double kScentTauMin = 1.0e-9;
constexpr double kScentTauMax = 1.0e6;
//  scent wind-advection strength. 0 = OFF -> ScentField::Step skips advection. Tuned ON at 1.0
// (advect at the TRUE wind velocity — the physically-correct semi-Lagrangian drift) so scent drifts
// downwind and a predator can track prey up-wind. Only worlds with scent/forager participants carry
// a scent field, so the canonical empty roster is still byte-identical (default --smoke unchanged);
// the populated PopulatedWorldReplay gate stays run==replay (deterministic double math), so no
// literal re-pin.
constexpr double kScentWindAdvectionScale = 1.0;

// Const views do not instantiate storage or change registry iteration order.
template<typename... Components>
std::uint64_t SimParticipantCount(const entt::registry& registry) {
    const auto view = registry.view<const Components...>();
    return static_cast<std::uint64_t>(std::distance(view.begin(), view.end()));
}

void AddValidationError(Luminumbra::world::WorldConfigValidationResult& result, std::string error) {
    result.errors.push_back(std::move(error));
    result.ok = false;
}

bool IsSafeWorldType(const std::string& world_type) {
    return !world_type.empty() && world_type.find("..") == std::string::npos &&
           world_type.find('/') == std::string::npos && world_type.find('\\') == std::string::npos;
}

fs::path RuntimeRoot(const std::string& root_path) {
    return root_path.empty() ? fs::path(".") : fs::path(root_path);
}

fs::path PresetPathFor(const std::string& root_path, const std::string& world_type) {
    return RuntimeRoot(root_path) / "worlds" / "atlas" / "presets" / (world_type + ".json");
}

// Clamp generation params to sane invariants BEFORE building the world system. The preset loader
// only type-checks; a customized (or hand-edited) preset can still carry octaves=0, negative
// amplitude, persistence outside (0,1], etc., which are degenerate/UB in the noise backend. This
// runs on both the create and load paths so headless/server generation is protected too.
void ClampTerrainParams(TerrainGenParams& p) {
    p.octaves = std::clamp(p.octaves, 1, 12);
    p.persistence = std::clamp(p.persistence, 0.05f, 1.0f);
    p.lacunarity = std::max(p.lacunarity, 1.0f);
    p.base_frequency = std::max(p.base_frequency, 1e-5f);
    p.base_amplitude = std::max(p.base_amplitude, 0.0f);
    p.peaks_amplitude = std::max(p.peaks_amplitude, 0.0f);
    p.peaks_frequency = std::max(p.peaks_frequency, 1e-5f);
    p.domain_warp_amplitude = std::max(p.domain_warp_amplitude, 0.0f);
    p.hydro_iterations = std::max(p.hydro_iterations, 0);
    p.cliff_step = std::max(p.cliff_step, 0.0f);
}

float ScentOriginFor(float anchor) {
    return anchor - (static_cast<float>(kScentFieldCells) * kScentCellSize * 0.5f);
}
} // namespace

namespace Luminumbra::world {

struct GameSession::LoadedWorldState {
    WorldStreamingState chunks;
    std::vector<IVec3> durable_inventory;
    WorldClock clock;
    std::size_t water_sim_cursor = 0;
    std::optional<Vec3> ambient_anchor;
    std::optional<WorldCalendar> render_calendar;
    std::optional<ActiveRegionLedger> ledger;
    bool snapshot_present = false;
    Ecs::EntityRegistrySnapshot plants;
    bool input_refused = false;
    std::optional<luminumbra::fields::EnergyFieldState> energy;
};

struct GameSession::WorldPreparation {
    WorldPreparationOptions options;
    std::stop_token stop;
    std::string error;
    WorldMetadata metadata;
    TerrainPresetLoadResult preset;
    std::string preset_json;
    fs::path state_directory;
    LoadedWorldState loaded;
    bool create = false;
    bool state_only = false;
    bool transient = false;
    bool has_spawn_point = false;
    bool ready = false;
};

GameSession::WorldPreparationOptions GameSession::CaptureWorldPreparationOptions() const {
    return {.root_path = m_rootPath,
            .save_root = GetSaveRootPath(),
            .configured_save_root = GetSaveRootPath(),
            .required_assets = m_requiredClientAssets,
            .ledger_config = m_activeRegionLedger.config(),
            .scheduler_override = m_regionSchedulerOverride,
            .simulation_tick = GetSimulationTickCount(),
            .world_revision = WorldRevision(),
            .active_regions = m_activeRegionsEnabled,
            .aether_state = m_aetherStateEnabled,
            .job_system_available = m_jobSystem != nullptr,
            .transient_creation = false};
}

std::string GameSession::WorldPreparationError(const WorldPreparation& prepared) {
    return prepared.stop.stop_requested() ? "Saved-world validation cancelled." : prepared.error;
}

bool GameSession::AdoptPreparedWorld(const std::shared_ptr<WorldPreparation>& prepared,
                                     Persistence::CreatedWorldArtifacts* artifacts) {
    if (!prepared || (!prepared->create && !IsSimulationTickBoundary()))
        return false;
    m_worldOpenError = WorldPreparationError(*prepared);
    if (!prepared->ready || !m_worldOpenError.empty())
        return false;
    if (!m_jobSystem) {
        m_worldOpenError = prepared->create ? "JobSystem not set before creating world."
                                            : "JobSystem not set before loading world.";
        return false;
    }
    const auto& options = prepared->options;
    if (options.root_path != m_rootPath || options.configured_save_root != GetSaveRootPath() ||
        options.required_assets != m_requiredClientAssets ||
        options.active_regions != m_activeRegionsEnabled ||
        options.aether_state != m_aetherStateEnabled ||
        options.ledger_config != m_activeRegionLedger.config() ||
        options.scheduler_override != m_regionSchedulerOverride ||
        (prepared->state_only &&
         (!m_worldUsable || !m_worldSystem || options.world_revision != WorldRevision()))) {
        m_worldOpenError = "World preparation no longer matches session configuration.";
        return false;
    }
    // Consuming a prepared result twice must never replace live state twice.
    prepared->ready = false;
    if (prepared->state_only) {
        AdoptWorldStateFrom(prepared->state_directory, prepared->loaded);
        return true;
    }
    return prepared->create ? AdoptCreatedWorld(*prepared, artifacts) : AdoptLoadedWorld(*prepared);
}

GameSession::GameSession() {
    m_scriptState = std::make_unique<Luminumbra::scripting::LuaState>();
}

GameSession::~GameSession() {
    // Jobs can read world, water, weather and physics. Drain before member destruction.
    if (m_worldSystem)
        m_worldSystem->quiesce_streaming_jobs_for_save();
    if (m_physicsSystem)
        m_physicsSystem->set_world_system(nullptr);
    m_worldSystem.reset();
}

void GameSession::ResetWorldSystems() {
    m_worldUsable = false;
    if (m_worldSystem)
        m_worldSystem->quiesce_streaming_jobs_for_save();
    // Keep all dependencies alive while the world destructor fences admitted work.
    if (m_physicsSystem)
        m_physicsSystem->set_world_system(nullptr);
    ++m_worldRevision;
    m_worldSystem.reset();
    m_physicsSystem.reset();
    m_waterSystem.reset();
    m_registry.clear();
    m_soilGrid.reset();
    m_irrigationGrid.reset();
    m_simulationClock.reset();
    m_simBudgetTelemetry.Clear();
    m_worldClock = WorldClock{};
    m_renderCalendar.reset();
    ReplaceActiveRegionLedger(ActiveRegionLedger(m_regionSchedulerConfig));
    m_regionSchedule = {};
    m_replicatedSimulationAnchors.reset();
    m_regionWork.clear();
    m_regionFault.reset();
    m_regionInputRejections = {};
    m_regionDurableDirectory.clear();
    m_simulationEventBus.clear();
    m_lastLoadedChunkCount = 0;
    m_lastSaveTick = -1;
    m_lastSaveMs = 0.0;
    m_saveFailures = 0;
    m_lastSaveFailureTick = -1;
}

// The preset dependency snapshot captures the per-world species table at create/load.
// DETERMINISM: CreatureSpeciesRegistry::LoadFromDirectory sorts the *.json entries by
// filename before parsing, so the table is a pure function of the files' CONTENT — never of
// filesystem iteration order. A missing/empty directory yields an EMPTY table, and every
// per-species behaviour block (IAUS "brain" overrides, "genome_ranges") defaults to the
// compiled constants. The creature brain and reproduction systems consume the resolved entry;
// absent or defaults-matching JSON remains byte-identical to compiled behaviour.
// the deterministic weather-EVENT schedule read. Explicit OFF
// always returns Clear/0 — byte-identical for every existing consumer; ON resolves
// WeatherEventAt(tick, worldSeed + 25) (the seed-offset registry slot the backlog
// defines). Pure function — no state, no RNG, no wall-clock.
luminumbra::sim::WeatherEventState GameSession::CurrentWeatherEvent() const {
    if (!m_weatherEventsEnabled) {
        return luminumbra::sim::WeatherEventState{};
    }
    const std::uint64_t seed = static_cast<std::uint64_t>(StringToSeed(m_metadata.seed)) + 25ull;
    return luminumbra::sim::WeatherEventAt(GetSimulationTickCount(), seed);
}

//  ( == ): wire the weather system into the water solver's
// per-cell rain when the owner opted in (sim.hydrology_weather -> SetWeatherRainEnabled).
// Called from BOTH CreateWorld and LoadWorld after the systems exist. Explicit OFF
// leaves the water solver's weather pointer null — byte-identical to pre-.
void GameSession::ApplyWeatherRainWiring() {
    if (!m_worldSystem) {
        return;
    }
    if (m_weatherRainEnabled && m_weatherSystem) {
        m_worldSystem->SetWaterWeatherRain(m_weatherSystem.get(), m_weatherRainScaleMm);
        LUMINUMBRA_CORE_INFO("Weather-driven rain WIRED (scale {} mm/tick at full precipitation)",
                             m_weatherRainScaleMm);
    } else {
        m_worldSystem->SetWaterWeatherRain(nullptr, 0);
    }
}

// Apply the same simulation gates and tuning before client/server world boot.
void GameSession::ApplySystemConfig(const luminumbra::core::SystemConfig& config) {
    m_appliedSystemConfig = config;
    SetEcologyTuning(luminumbra::ai::ResolveEcologyTuning(config));
    SetWeatherRainEnabled(config.enabled(luminumbra::core::SysKey::SimHydrologyWeather));
    SetActiveRegionsEnabled(config.enabled(luminumbra::core::SysKey::SimActiveRegions));
    SetWaterHighResEnabled(config.enabled(luminumbra::core::SysKey::SimWaterHighRes));
    SetWeatherEventsEnabled(config.enabled(luminumbra::core::SysKey::SimWeatherEvents));
    SetAetherStateEnabled(config.enabled(luminumbra::core::SysKey::SimAetherState));
    SetWildlifeFoliageTuning(luminumbra::ai::ResolveWildlifeFoliageTuning(config));
    SetThirstTuning(luminumbra::ai::ResolveThirstTuning(config));
    SetScavengingTuning(luminumbra::ai::ResolveScavengingTuning(config));
    SetForagingTuning(luminumbra::ai::ResolveForagingTuning(config));
    SetReproductionTuning(luminumbra::ai::ResolveReproductionTuning(config));
    SetCircadianAmplitude(luminumbra::ai::ResolveCircadianAmplitude(config));
    SetPlantMutationRate(luminumbra::ai::ResolvePlantMutationRate(config));
}

std::map<std::string, bool> GameSession::EffectiveSimulationGates() const {
    auto gates = m_appliedSystemConfig.SimulationGates();
    gates["active_regions"] = m_activeRegionsEnabled;
    gates["aether_state"] = m_aetherStateEnabled;
    gates["water_high_res"] = m_waterHighResEnabled;
    gates["hydrology_weather"] = m_weatherRainEnabled;
    gates["weather_events"] = m_weatherEventsEnabled;
    return gates;
}

std::string GameSession::SystemConfigIdentity() const {
    return SystemConfigIdentity(GetRenderCalendar());
}

std::string GameSession::SystemConfigIdentity(const WorldCalendar& calendar) const {
    using luminumbra::core::SysKey;
    using luminumbra::core::SysParam;
    luminumbra::core::SimulationIdentityValues values;
    values.finite_hydrology_enabled = m_finiteHydrologyEnabled;
    values.weather_rain_scale_mm = m_weatherRainScaleMm;
    const auto& scheduler = m_activeRegionLedger.config();
    values.region_work_limit = scheduler.work_limit;
    values.region_hold_ticks = scheduler.hold_ticks;
    values.region_reduced_shift = scheduler.reduced_shift;
    values.day_length_ticks = calendar.dayLengthTicks;
    values.days_per_year = calendar.daysPerYear;
    values.daylight_numerator = calendar.daylightNumerator;
    values.daylight_denominator = calendar.daylightDenominator;
    values.lunar_cycle_days = calendar.lunarCycleDays;
    // Tuning-only gates resolve to values in ApplySystemConfig; their flags are
    // normalized to true. Erosion has no session gate. Preserve the canonical slots.
    values.enabled[static_cast<std::size_t>(SysKey::SimHydrologyWeather)] = m_weatherRainEnabled;
    values.enabled[static_cast<std::size_t>(SysKey::SimWeatherEvents)] = m_weatherEventsEnabled;
    values.enabled[static_cast<std::size_t>(SysKey::SimAetherState)] = m_aetherStateEnabled;
    values.enabled[static_cast<std::size_t>(SysKey::SimWaterHighRes)] = m_waterHighResEnabled;
    values.enabled[static_cast<std::size_t>(SysKey::SimActiveRegions)] = m_activeRegionsEnabled;
    values.scalars[static_cast<std::size_t>(SysParam::PlantMutationRate)] = m_plantMutationRate;
    values.scalars[static_cast<std::size_t>(SysParam::EcoEnergyDrain)] =
        m_ecologyTuning.energy_drain_per_second;
    values.scalars[static_cast<std::size_t>(SysParam::EcoEnergyRestRecover)] =
        m_ecologyTuning.energy_rest_recover;
    values.scalars[static_cast<std::size_t>(SysParam::EcoEnergySleepRecover)] =
        m_ecologyTuning.energy_sleep_recover;
    values.scalars[static_cast<std::size_t>(SysParam::EcoHungerGrowth)] =
        m_ecologyTuning.hunger_growth_per_second;
    values.scalars[static_cast<std::size_t>(SysParam::EcoHungerGrazeSate)] =
        m_ecologyTuning.hunger_graze_sate;
    values.scalars[static_cast<std::size_t>(SysParam::EcoStaminaRestRecover)] =
        m_ecologyTuning.stamina_rest_recover;
    values.scalars[static_cast<std::size_t>(SysParam::EcoStaminaMoveDrain)] =
        m_ecologyTuning.stamina_move_drain;
    values.scalars[static_cast<std::size_t>(SysParam::EcoHerdWeight)] = m_ecologyTuning.herd_weight;
    values.scalars[static_cast<std::size_t>(SysParam::EcoAlignmentWeight)] =
        m_ecologyTuning.alignment_weight;
    values.scalars[static_cast<std::size_t>(SysParam::EcoCatchRadius)] =
        m_ecologyTuning.catch_radius;
    values.scalars[static_cast<std::size_t>(SysParam::EcoCatchSatiation)] =
        m_ecologyTuning.catch_satiation;
    values.scalars[static_cast<std::size_t>(SysParam::EcoFlockNeighborRadius)] =
        m_ecologyTuning.flock_neighbor_radius;
    values.scalars[static_cast<std::size_t>(SysParam::EcoFlockSeparationRadius)] =
        m_ecologyTuning.flock_separation_radius;
    values.scalars[static_cast<std::size_t>(SysParam::EcoFlockCohesionWeight)] =
        m_ecologyTuning.flock_cohesion_weight;
    values.scalars[static_cast<std::size_t>(SysParam::EcoFlockSeparationWeight)] =
        m_ecologyTuning.flock_separation_weight;
    values.scalars[static_cast<std::size_t>(SysParam::WfGrazeRadius)] =
        m_wildlifeFoliageTuning.graze_radius;
    values.scalars[static_cast<std::size_t>(SysParam::WfGrazePerCreature)] =
        m_wildlifeFoliageTuning.graze_per_creature;
    values.scalars[static_cast<std::size_t>(SysParam::WfRegrowPerTick)] =
        m_wildlifeFoliageTuning.regrow_per_tick;
    values.scalars[static_cast<std::size_t>(SysParam::WfFeedPerGraze)] =
        m_wildlifeFoliageTuning.feed_per_graze;
    values.scalars[static_cast<std::size_t>(SysParam::ThirstRiseRate)] = m_thirstTuning.rise_rate;
    values.scalars[static_cast<std::size_t>(SysParam::ThirstDrinkRate)] = m_thirstTuning.drink_rate;
    values.scalars[static_cast<std::size_t>(SysParam::ThirstSeekThreshold)] =
        m_thirstTuning.seek_threshold;
    values.scalars[static_cast<std::size_t>(SysParam::ScavHungerThreshold)] =
        m_scavengingTuning.hunger_threshold;
    values.scalars[static_cast<std::size_t>(SysParam::ScavFeedRadius)] =
        m_scavengingTuning.feed_radius;
    values.scalars[static_cast<std::size_t>(SysParam::ScavFeedRate)] = m_scavengingTuning.feed_rate;
    values.scalars[static_cast<std::size_t>(SysParam::ReproMaturityTicks)] =
        m_reproductionTuning.maturity_ticks;
    values.scalars[static_cast<std::size_t>(SysParam::ReproCooldownTicks)] =
        m_reproductionTuning.cooldown_ticks;
    values.scalars[static_cast<std::size_t>(SysParam::ReproHealthyStamina)] =
        m_reproductionTuning.healthy_stamina;
    values.scalars[static_cast<std::size_t>(SysParam::ReproMateSeekRadius)] =
        m_reproductionTuning.mate_seek_radius;
    values.scalars[static_cast<std::size_t>(SysParam::ReproCourtshipRadius)] =
        m_reproductionTuning.courtship_radius;
    values.scalars[static_cast<std::size_t>(SysParam::ReproCourtshipTicks)] =
        m_reproductionTuning.courtship_ticks;
    values.scalars[static_cast<std::size_t>(SysParam::ReproSpawnRadius)] =
        m_reproductionTuning.spawn_radius;
    values.scalars[static_cast<std::size_t>(SysParam::ForagingDeposit)] = m_foragingTuning.deposit;
    values.scalars[static_cast<std::size_t>(SysParam::ForagingTrailWeight)] =
        m_foragingTuning.trail_weight;
    values.scalars[static_cast<std::size_t>(SysParam::ForagingGoalWeight)] =
        m_foragingTuning.goal_weight;
    values.scalars[static_cast<std::size_t>(SysParam::CircadianAmplitude)] = m_circadianAmplitude;
    return values.ComputeIdentity();
}

// apply the session water-sim resolution when the owner opted in
// (sim.water_high_res -> SetWaterHighResEnabled). Called from BOTH CreateWorld and
// LoadWorld immediately after the water system is constructed and linked — before its
// first update, because the uniform hashed grid resolution can never change mid-run.
// Explicit OFF actively selects Medium; the shipping default selects High.
void GameSession::ApplyWaterResolutionWiring() {
    if (!m_worldSystem) {
        return;
    }
    m_worldSystem->SetWaterSimResolution(static_cast<int>(m_waterHighResEnabled
                                                              ? Systems::WaterDetailLevel::High
                                                              : Systems::WaterDetailLevel::Medium));
    LUMINUMBRA_CORE_INFO("Water resolution wired: {}", m_waterHighResEnabled ? "High" : "Medium");
}

std::uint32_t GameSession::TickSimulation(double frame_dt) {
    if (m_regionFault)
        return 0;
    // Queued work belongs to this next tick; only reentrant batches must refuse.
    if (m_activeRegionsEnabled && m_simulationBatchInProgress)
        return 0;
    class BatchGuard {
    public:
        BatchGuard(bool& value, bool enabled)
            : m_active(value)
            , m_previous(value) {
            m_active = enabled;
        }
        ~BatchGuard() {
            m_active = m_previous;
        }

    private:
        bool& m_active;
        bool m_previous;
    };
    const BatchGuard guard(m_simulationBatchInProgress, m_activeRegionsEnabled);
    std::uint32_t ticks_executed;
    if (m_activeRegionsEnabled) {
        auto advanced_clock = m_simulationClock;
        ticks_executed = advanced_clock.advance(frame_dt);
        if (!m_worldClock.can_advance(ticks_executed))
            throw std::overflow_error("World clock tick limit reached");
        m_simulationClock = advanced_clock;
    } else {
        ticks_executed = m_simulationClock.advance(frame_dt);
    }
    if (ticks_executed == 0) {
        return 0;
    }

    // Tick ids are 1-based; the clock already advanced past this frame's
    // ticks, so recover the id of the first one.
    const std::uint64_t first_tick = m_simulationClock.tick_count() - ticks_executed + 1;
    for (std::uint32_t i = 0; i < ticks_executed; ++i) {
        const std::uint64_t current_tick = first_tick + i;
        using Stage = luminumbra::simulation::SimBudgetStage;
        namespace TelemetryComp = Luminumbra::Components;
        luminumbra::simulation::SimBudgetTelemetry::TickScope budget(m_simBudgetTelemetry,
                                                                     current_tick);
        if (m_activeRegionsEnabled)
            m_worldClock.set_tick(current_tick);

        // Deterministic per-tick system order. The ordered event bus drains everything
        // published for this tick after simulation systems finish (tick -> lane -> sequence).

        budget.Next(Stage::Animation, [&] {
            return SimParticipantCount<luminumbra::animation::AnimationPlayerComponent>(m_registry);
        });
        // 1. Animation pose sampling: FIRST in the tick order.
        luminumbra::animation::SamplePosesOnTick(m_registry, m_simulationClock.fixed_dt());

        budget.Next(Stage::Instinct, [&] {
            return SimParticipantCount<TelemetryComp::InstinctAgentComponent,
                                       TelemetryComp::NeedsComponent>(m_registry);
        });
        // 2. Instinct planning: need growth + deterministic
        // replanning over the registry.
        //
        // the ecology stimulus-channel registry is supplied ONLY
        // when at least one creature has OPTED IN via a game-data
        // StimulusSubscriptionComponent. The canonical roster carries none, so
        // the `view.empty` guard keeps the call BYTE-IDENTICAL to the pre-5b
        // path (nullptr context) -- world_hash stays d950a6afc12a5cdc (critique
        //  / ). When subscribers exist, the context reads the replicated
        // weather state (one-way; weather updated on the PREVIOUS tick, slot 4)
        // and the per-tick time-of-day/season/light channels derived from the
        // tick. The engine still names no creature behavior.
        if (m_registry.view<const Luminumbra::Components::StimulusSubscriptionComponent>()
                .empty()) {
            luminumbra::ai::RunInstinctSystemOnTick(m_registry, current_tick);
        } else {
            luminumbra::ai::StimulusContext stimulus_context;
            stimulus_context.tick = current_tick;
            stimulus_context.world_clock = m_activeRegionsEnabled ? &m_worldClock : nullptr;
            stimulus_context.sample_position = m_metadata.spawnPoint;
            stimulus_context.weather = m_weatherSystem.get();
            //  (-7): the composite energy environment. Prefer the
            // STATEFUL layer when sim.aether_state is ON (gameplay deposits are the
            // truth creatures react to): the cell at the sample position, quantized
            // by the shared 24 m grid identity and normalized by the PINNED
            // fields::kEnergyRawPerUnit (1 gameplay unit == full stimulus; the
            // channel clamps [0, 1]). Else the re-derivable ambience, whose
            // emission is already [0, 1] by construction (identity pin -- the same
            // scalar the render emissive tap consumes). Both reads are one-way
            // PRIOR-tick state (the fields update later this tick at slot 5/5a),
            // mirroring the weather slot-order rule above. Neither system present
            // -> the -1 sentinel -> the channel's deterministic neutral 0.
            if (m_energyFieldState) {
                const int stim_cell_x = static_cast<int>(
                    std::floor(m_metadata.spawnPoint.x / Systems::kAetherCellSizeM));
                const int stim_cell_z = static_cast<int>(
                    std::floor(m_metadata.spawnPoint.z / Systems::kAetherCellSizeM));
                stimulus_context.aether_level =
                    static_cast<float>(m_energyFieldState->at_cell(stim_cell_x, stim_cell_z)) /
                    static_cast<float>(luminumbra::fields::kEnergyRawPerUnit);
            } else if (m_aetherFieldSystem) {
                stimulus_context.aether_level =
                    m_aetherFieldSystem->SampleAether(m_metadata.spawnPoint);
            }
            const luminumbra::ai::StimulusChannelRegistry stimulus_registry(stimulus_context);
            luminumbra::ai::RunInstinctSystemOnTick(m_registry, current_tick, &stimulus_registry);
        }

        budget.Next(Stage::Perception, [&] {
            return SimParticipantCount<TelemetryComp::TransformComponent,
                                       TelemetryComp::PerceptionComponent,
                                       TelemetryComp::AwarenessComponent>(m_registry);
        });
        // 2b.  : perception fusion. Updates each perceiving creature's
        // awareness (vision cone + hearing audiogram over Sensable entities of
        // other factions -> detection meter/state + last-known memory). Runs after
        // planning so awareness is fresh for the NEXT plan/locomotion. Operates
        // ONLY on entities carrying PerceptionComponent+AwarenessComponent; the
        // canonical headless roster carries none, so this is a no-op there and
        // world_hash is UNCHANGED (byte-identical) -- same discipline as the
        // stimulus-channel opt-in above. No RNG/wall-clock; id-ordered.
        luminumbra::ai::RunPerceptionSystemOnTick(m_registry,
                                                  static_cast<float>(m_simulationClock.fixed_dt()));

        budget.Next(Stage::Scent, [&] {
            return (m_scentField &&
                    (HasScentParticipants() ||
                     SimParticipantCount<TelemetryComp::ForagerComponent>(m_registry) != 0))
                       ? static_cast<std::uint64_t>(kScentFieldCells) * kScentFieldCells *
                             kScentFieldChannels
                       : 0ull;
        });
        // 2c.: scent stigmergy write/update. Game-data opt-in:
        // no scent emitter/sensor components means no field mutation, while active
        // ecology worlds get deterministic deposit -> diffuse/evaporate -> clamp.
        const bool scent_active = HasScentParticipants();
        // ant foraging deposits trails into the SAME scent field (channels 2/3), so it runs in
        // this slot BEFORE the diffuse/evaporate Step (its deposits then diffuse + evaporate this
        // tick, like creature scent). Opt-in via ForagerComponent: a roster with none is a no-op,
        // so the canonical roster (and any scent-only world) is byte-identical.
        // Integer/id-ordered/libm-free.
        const auto foragers_view =
            m_registry.view<const Luminumbra::Components::ForagerComponent>();
        const bool foraging_active = foragers_view.begin() != foragers_view.end();
        if ((scent_active || foraging_active) && m_scentField) {
            if (scent_active) {
                luminumbra::ai::RunScentDepositOnTick(m_registry,
                                                      *m_scentField,
                                                      ScentOriginFor(m_metadata.spawnPoint.x),
                                                      ScentOriginFor(m_metadata.spawnPoint.z),
                                                      kScentCellSize);
            }
            if (foraging_active) {
                luminumbra::ai::RunForagingOnTick(m_registry, *m_scentField, m_foragingTuning);
            }
            // advect the scent downwind by the PRIOR-tick wind (the wind field is updated
            // later this tick at slot 3, so sampling here keeps the slot order stable). Convert
            // world-units/sec -> cells/step via dt and the cell size. Scale 0 -> zero wind ->
            // Step skips advection -> byte-identical (no re-pin until tuned on).
            double wind_cx = 0.0, wind_cz = 0.0;
            if (kScentWindAdvectionScale != 0.0 && m_windFieldSystem) {
                const Luminumbra::Vec2 w = m_windFieldSystem->SampleWind(m_metadata.spawnPoint);
                const double k = kScentWindAdvectionScale * m_simulationClock.fixed_dt() /
                                 static_cast<double>(kScentCellSize);
                wind_cx = static_cast<double>(w.x) * k;
                wind_cz = static_cast<double>(w.y) * k;
            }
            m_scentField->Step(kScentDiffusionRate,
                               kScentDiffusionIterations,
                               kScentEvaporation,
                               wind_cx,
                               wind_cz);
            m_scentField->Clamp(kScentTauMin, kScentTauMax);
        }

        budget.Next(Stage::Locomotion, [&] {
            return SimParticipantCount<TelemetryComp::ActionPlanComponent,
                                       TelemetryComp::TransformComponent,
                                       TelemetryComp::LocomotionProfile>(m_registry);
        });
        // 2d.: action-plan locomotion, then scent gradient bias.
        // The executor writes only LocomotionIntentComponent; physics/render owners
        // consume that intent in their existing lanes.
        luminumbra::ai::RunInstinctLocomotionOnTick(m_registry);
        if (scent_active && m_scentField) {
            luminumbra::ai::RunScentSteeringOnTick(m_registry,
                                                   *m_scentField,
                                                   ScentOriginFor(m_metadata.spawnPoint.x),
                                                   ScentOriginFor(m_metadata.spawnPoint.z),
                                                   kScentCellSize);
        }

        budget.Next(Stage::Creatures, [&] {
            return SimParticipantCount<TelemetryComp::CreatureComponent,
                                       TelemetryComp::TransformComponent>(m_registry);
        });
        // 2e. : Utility-AI creature brain (predator/prey movement). Per-entity opt-in
        // via CreatureComponent -> a world with none is a no-op (canonical roster byte-identical,
        // same discipline as plants/scent). Deterministic (id-ordered, libm-free).
        {
            auto creatures = m_registry.view<const Luminumbra::Components::CreatureComponent>();
            if (creatures.begin() != creatures.end()) {
                // hand the brain the scent field so a predator without a
                // directly-perceived target tracks the prey-scent gradient (channel 0).
                // No scent participants -> the field is all-zero -> byte-identical.
                luminumbra::ai::RunCreatureBrainSystemOnTick(
                    m_registry,
                    static_cast<float>(m_simulationClock.fixed_dt()),
                    m_ecologyTuning,
                    m_scentField.get(),
                    ScentOriginFor(m_metadata.spawnPoint.x),
                    ScentOriginFor(m_metadata.spawnPoint.z),
                    kScentCellSize,
                    true,
                    m_speciesTable.get());

                // 2e-mate: SEXUAL reproduction, phase A. Ready creatures (mature, well-fed,
                // off cooldown) carrying a CreatureGenomeComponent steer toward the nearest
                // ready OPPOSITE-SEX mate by overriding the brain's wish velocity (unless
                // fleeing) — so the physics bridge below actually walks them together. Opt-in
                // (genome component); no genome -> untouched.
                luminumbra::ai::RunMateSeekingOnTick(m_registry, m_reproductionTuning);

                // 2e-steer: blend the  bias systems' outputs (computed last tick, slot 7) into
                // the wish velocity before the physics bridge applies it -- pack flank steer,
                // migration drift, territory homing. Extracted to ai/SteeringConsumer.h so this
                // integration layer is unit-tested independently of the producers.
                luminumbra::ai::RunSteeringConsumerOnTick(m_registry);

                // 2e-survival: water-seeking (THIRST) + carcass SCAVENGING — built sim
                // systems wired into the live tick. Opt-in via ThirstComponent /
                // ScavengerComponent (+ WaterHoleComponent water sources / carcasses); a
                // roster carrying none is a pure no-op, so the canonical NetworkStateHash
                // stays byte-identical. Thirst rises + drinks at water; hungry scavengers
                // feed on carcasses (death -> food). Each writes its OWN component's wish,
                // blended into the creature wish below so the physics bridge walks them there.
                luminumbra::ai::RunThirstOnTick(
                    m_registry, static_cast<float>(m_simulationClock.fixed_dt()), m_thirstTuning);
                luminumbra::ai::RunScavengingOnTick(m_registry, current_tick, m_scavengingTuning);
                {
                    // Additive blend of the survival wishes into CreatureComponent.wish so a
                    // thirsty/scavenging creature actually steers to water/carrion. Entities
                    // without the opt-in components are untouched (try_get -> null), keeping
                    // the baseline byte-identical; the physics bridge below consumes wish_x/z.
                    auto sv = m_registry.view<Luminumbra::Components::CreatureComponent>();
                    for (auto e : sv) {
                        auto& cr = sv.get<Luminumbra::Components::CreatureComponent>(e);
                        // the thirst pull applies ONLY when the arbiter
                        // actually chose Drink — the old unconditional blend let a
                        // FLEEING creature be simultaneously steered toward water
                        // (the contract's exact defect). Thirst now competes inside
                        // DecideCreatureAction; this blend is its motor output.
                        if (cr.last_action ==
                            static_cast<int>(luminumbra::ai::CreatureAction::Drink)) {
                            if (const auto* th =
                                    m_registry.try_get<Luminumbra::Components::ThirstComponent>(
                                        e)) {
                                cr.wish_x += th->wish_x;
                                cr.wish_z += th->wish_z;
                            }
                        }
                        if (const auto* scv =
                                m_registry.try_get<Luminumbra::Components::ScavengerComponent>(e)) {
                            cr.wish_x += scv->wish_x;
                            cr.wish_z += scv->wish_z;
                        }
                    }
                }

                // 2e-phys: TRUE-PHYSICS locomotion bridge. Creatures carrying a
                // CreaturePhysicsComponent are driven by the deterministic Jolt avatar
                // controller (the same one player/networked avatars use): push the brain's
                // wish velocity in, step the avatars once at the fixed dt (index-ordered =
                // deterministic same-binary, mirrors ServerWorldRunner), and read the
                // terrain-resolved position (gravity/collision/slopes) back into the
                // transform. Gated on a physics system + at least one physics creature, so
                // the canonical roster (none) keeps world_hash byte-identical.
                if (m_physicsSystem) {
                    auto phys = m_registry.view<Luminumbra::Components::CreaturePhysicsComponent,
                                                Luminumbra::Components::CreatureComponent,
                                                Luminumbra::Components::TransformComponent>();
                    if (phys.begin() != phys.end()) {
                        std::vector<entt::entity> pe(phys.begin(), phys.end());
                        std::sort(pe.begin(), pe.end(), [](entt::entity a, entt::entity b) {
                            return entt::to_integral(a) < entt::to_integral(b);
                        });
                        for (auto e : pe) {
                            const auto& cr = phys.get<Luminumbra::Components::CreatureComponent>(e);
                            const auto& cp =
                                phys.get<Luminumbra::Components::CreaturePhysicsComponent>(e);
                            m_physicsSystem->set_avatar_wish_velocity(
                                cp.avatar_index, glm::vec2(cr.wish_x, cr.wish_z));
                        }
                        m_physicsSystem->update_avatars(
                            static_cast<float>(m_simulationClock.fixed_dt()));
                        for (auto e : pe) {
                            const auto& cp =
                                phys.get<Luminumbra::Components::CreaturePhysicsComponent>(e);
                            phys.get<Luminumbra::Components::TransformComponent>(e).position =
                                m_physicsSystem->get_avatar_position(cp.avatar_index);
                        }
                    }
                }

                // 2e-mate: SEXUAL reproduction, phase B (after the pair has moved together via
                // the bridge). Ready adjacent opposite-sex pairs accumulate courtship; once a
                // pair has courted long enough, ONE offspring is born from a SEEDED blend of
                // BOTH parents' genomes (caught prey leave none -> selection). Deterministic
                // (id-ordered, libm-free, RNG seeded from offset 16 + parent ids + tick).
                // Per-entity opt-in (genome component): no genome / no creatures -> nothing
                // created, so the canonical NetworkStateHash baseline stays byte-identical.
                // the REAL world seed feeds the per-birth RNG —
                // previously 0ull, so offspring genomes were identical across worlds
                // with the same entity ids and ticks. Same-seed worlds reproduce
                // identically; different seeds diverge (the reproduction test pins both).
                const auto repro = luminumbra::ai::RunMatingResolveOnTick(
                    m_registry,
                    current_tick,
                    static_cast<std::uint64_t>(StringToSeed(m_metadata.seed)),
                    m_reproductionTuning,
                    m_speciesTable.get());
                if (repro.born > 0) {
                    LUMINUMBRA_CORE_INFO(
                        ": {} offspring born (sexual) at tick {}", repro.born, current_tick);
                }
            }
        }

        if (m_activeRegionsEnabled && m_windFieldSystem && m_weatherSystem) {
            // A distinct stage owns the whole clock-driven wind/weather sequence.
            // Count each input field cell once (wind + weather = 8192), not
            // rebuild passes: UpdateFromClock rebuilds wind twice around weather.
            // Wind and Weather have no samples on this path; see telemetry v2.
            budget.Next(Stage::WindWeather, [] {
                return static_cast<std::uint64_t>(Systems::kWindExtentCells) *
                           Systems::kWindExtentCells +
                       static_cast<std::uint64_t>(Systems::kWeatherExtentCells) *
                           Systems::kWeatherExtentCells;
            });
            m_weatherSystem->UpdateFromClock(
                current_tick, m_ambientFieldAnchor, *m_windFieldSystem);
        } else {
            budget.Next(Stage::Wind, [&] {
                return m_windFieldSystem ? static_cast<std::uint64_t>(Systems::kWindExtentCells) *
                                               Systems::kWindExtentCells
                                         : 0ull;
            });
            // 3.  : wind field update. Deterministic (DeterministicMath +
            // FastNoise batch path; no wall-clock/RNG). Anchored on the spawn/stream
            // anchor so the streamed-region grid follows it. The wind cell values
            // feed the world_hash `wind` sub-hash; the update must run every tick so
            // run==replay and resim agree on the field at every checkpoint.
            if (m_windFieldSystem) {
                m_windFieldSystem->ClearStormPerturbations();
                if (m_weatherSystem) {
                    for (const Systems::StormCell& storm : m_weatherSystem->StormCells()) {
                        m_windFieldSystem->InjectStormPerturbation({
                            storm.center_world,
                            96.0f,
                            storm.velocity,
                            storm.intensity,
                        });
                    }
                }
                m_windFieldSystem->Update(current_tick, m_metadata.spawnPoint);
            }

            budget.Next(Stage::Weather, [&] {
                return m_weatherSystem ? static_cast<std::uint64_t>(Systems::kWeatherExtentCells) *
                                             Systems::kWeatherExtentCells
                                       : 0ull;
            });
            // 4.  : weather core update. Runs AFTER wind so storm cells
            // advect by the freshly-updated wind grid. Deterministic (DeterministicMath
            // + FastNoise batch path; no wall-clock/RNG, no std::random). The weather
            // state (category map + storm cells + precip field) feeds the world_hash
            // `weather` sub-hash; the update runs every tick so run==replay and resim
            // agree on the state at every checkpoint.
            if (m_weatherSystem) {
                m_weatherSystem->Update(
                    current_tick, m_metadata.spawnPoint, m_windFieldSystem.get());
            }
        }

        budget.Next(Stage::Aether, [&] {
            return m_aetherFieldSystem ? static_cast<std::uint64_t>(Systems::kAetherExtentCells) *
                                             Systems::kAetherExtentCells
                                       : 0ull;
        });
        // 5.: Aether scalar field update. Runs AFTER weather so it
        // advects its emission source by the freshly-updated wind grid, keeping
        // same-tick weather coupling current. Deterministic
        // (pure function of seed+14, tick, origin[, wind]; DeterministicMath +
        // FastNoise batch path; no wall-clock/RNG). The cell values feed the
        // world_hash `aether` sub-hash; the update runs every tick so run==replay
        // and resim agree on the field at every checkpoint.
        if (m_aetherFieldSystem) {
            m_aetherFieldSystem->Update(current_tick,
                                        m_activeRegionsEnabled ? m_ambientFieldAnchor
                                                               : m_metadata.spawnPoint,
                                        m_windFieldSystem.get());
        }

        budget.Next(Stage::Energy,
                    [&] { return m_energyFieldState ? m_energyFieldState->page_count() : 0ull; });
        // 5a.: the STATEFUL energy layer ticks directly
        // after the re-derivable field, anchored on the SAME replicated anchor
        // quantized by the shared 24 m cell size (never camera state). Null
        // unless sim.aether_state opted in — the canonical world skips this
        // block entirely (byte-identical). Deposits are queued by emitters
        // right below; Tick applies them sorted.
        if (m_energyFieldState) {
            const int anchor_cx =
                static_cast<int>(std::floor(m_metadata.spawnPoint.x / Systems::kAetherCellSizeM));
            const int anchor_cz =
                static_cast<int>(std::floor(m_metadata.spawnPoint.z / Systems::kAetherCellSizeM));
            m_energyFieldState->SetAnchorCell(anchor_cx, anchor_cz);
            // gather this tick's FieldEmitterComponent deposits
            // (id-sorted, two-phase) into the layer before it ticks.
            // Participant-gated: no emitter component, no deposits, no bytes.
            Systems::GatherFieldEmitterDeposits(m_registry, *m_energyFieldState);
            m_energyFieldState->Tick(current_tick);
        }

        // 5b.: the soil-nutrient + irrigation-moisture FIELDS update BEFORE plant
        // growth (slot 6) so growth reads FRESH availability, not last tick's (the prior order had
        // growth at slot 6 reading grids that only updated at slot 7 -> a 1-tick-stale env). Opt-in
        // via WaterSource/SoilFeeder; a world with none updates no grid, so the canonical roster is
        // byte-identical. The shared foliage field anchor (origin + cell size) is hoisted here so
        // both the growth env-sampler (slot 6) and the  living-world systems (slot 7) share it.
        constexpr int kFoliageFieldCells = 256;   // grid extent (cells)
        constexpr float kFoliageFieldCell = 1.0f; // metres / cell
        const float foliageOriginX =
            m_metadata.spawnPoint.x - kFoliageFieldCells * kFoliageFieldCell * 0.5f;
        const float foliageOriginZ =
            m_metadata.spawnPoint.z - kFoliageFieldCells * kFoliageFieldCell * 0.5f;
        {
            namespace Comp = Luminumbra::Components;
            namespace fol = luminumbra::foliage;
            budget.Next(Stage::Irrigation, [&] {
                return SimParticipantCount<TelemetryComp::WaterSourceComponent>(m_registry) != 0
                           ? static_cast<std::uint64_t>(kFoliageFieldCells) * kFoliageFieldCells
                           : 0ull;
            });
            if (!m_registry.view<Comp::WaterSourceComponent>().empty()) {
                if (!m_irrigationGrid)
                    m_irrigationGrid = std::make_unique<fol::IrrigationGrid>(kFoliageFieldCells,
                                                                             kFoliageFieldCells);
                fol::RunIrrigationOnTick(m_registry,
                                         *m_irrigationGrid,
                                         foliageOriginX,
                                         foliageOriginZ,
                                         kFoliageFieldCell);
            }
            budget.Next(Stage::Soil, [&] {
                return SimParticipantCount<TelemetryComp::SoilFeederComponent>(m_registry) != 0
                           ? static_cast<std::uint64_t>(kFoliageFieldCells) * kFoliageFieldCells
                           : 0ull;
            });
            if (!m_registry.view<Comp::SoilFeederComponent>().empty()) {
                if (!m_soilGrid)
                    m_soilGrid =
                        std::make_unique<fol::SoilGrid>(kFoliageFieldCells, kFoliageFieldCells);
                fol::RunSoilNutrientOnTick(
                    m_registry, *m_soilGrid, foliageOriginX, foliageOriginZ, kFoliageFieldCell);
            }
        }

        budget.Next(Stage::Plants, [&] {
            return SimParticipantCount<TelemetryComp::PlantTag,
                                       TelemetryComp::PlantGrowthComponent,
                                       TelemetryComp::PlantGenomeComponent,
                                       TelemetryComp::TransformComponent>(m_registry);
        });
        // 6. : deterministic plant GROWTH. Game-data opt-in (PlantTag):
        // no plants -> the system never runs and world_hash stays byte-identical
        // (same discipline as scent). The environment is ATMOSPHERIC — moisture is
        // driven by the freshly-updated weather precip field (rain -> growth), so
        // growth runs AFTER weather. Integer/fixed-point + id-ordered = run==replay.
        // Temperature, light, soil, weather, and irrigation are sampled below.
        if (HasPlantParticipants()) {
            const luminumbra::foliage::EnvSampler plant_env =
                [this, foliageOriginX, foliageOriginZ, current_tick](
                    const Luminumbra::Components::TransformComponent& tf) {
                    luminumbra::foliage::PlantEnvSample s;
                    const Luminumbra::Vec3 p = tf.position;
                    // Moisture: weather precipitation (rain -> growth) + irrigation (player
                    // watering).
                    const float precip =
                        m_weatherSystem ? m_weatherSystem->PrecipitationAt(p) : 0.0f;
                    s.moisture = luminumbra::foliage::clamp01(0.30f + precip * 0.70f);
                    // fold the freshly-updated IRRIGATION grid (milli 0..1000) into moisture —
                    // watered cells grow better (watered-beats-dry). No grid -> unchanged.
                    if (m_irrigationGrid) {
                        const int moist = luminumbra::foliage::MoistureAt(*m_irrigationGrid,
                                                                          p.x,
                                                                          p.z,
                                                                          foliageOriginX,
                                                                          foliageOriginZ,
                                                                          kFoliageFieldCell);
                        s.moisture = luminumbra::foliage::clamp01(
                            s.moisture + static_cast<float>(moist) / 1000.0f * 0.5f);
                    }
                    // Soil + temperature from the TERRAIN: surface-material favourability
                    // (grass/soil rich, sand/stone poor) and an altitude lapse (higher
                    // ground is colder -> alpine vs lowland growth). The real atmospheric/
                    // terrain coupling. (Only runs for opt-in sim plants, so the per-plant
                    // world queries are bounded.)
                    float soil = 0.5f, temp = 0.55f;
                    if (m_worldSystem) {
                        const float th = m_worldSystem->GetTerrainHeightAt(p.x, p.z);
                        switch (m_worldSystem->SurfaceVertexMaterial(p.x, p.z, th)) {
                            case Luminumbra::MaterialType::Grass:
                                soil = 0.95f;
                                break;
                            case Luminumbra::MaterialType::Soil:
                                soil = 0.85f;
                                break;
                            case Luminumbra::MaterialType::Sand:
                                soil = 0.45f;
                                break;
                            case Luminumbra::MaterialType::Stone:
                                soil = 0.30f;
                                break;
                            default:
                                soil = 0.25f;
                                break;
                        }
                        const float altitude = th - static_cast<float>(Luminumbra::SEA_LEVEL);
                        temp = luminumbra::foliage::clamp01(
                            0.60f - (altitude > 0.0f ? altitude : 0.0f) * 0.00045f);
                    }
                    s.soil_quality = soil;
                    // fold the freshly-updated SOIL-NUTRIENT grid (milli 0..1000) into soil
                    // quality. Foragers/feeders DEPLETE the cell, so a monoculture on one cell
                    // starves itself (monoculture-starves). No grid -> unchanged terrain quality.
                    if (m_soilGrid) {
                        const int nut = luminumbra::foliage::NutrientAt(*m_soilGrid,
                                                                        p.x,
                                                                        p.z,
                                                                        foliageOriginX,
                                                                        foliageOriginZ,
                                                                        kFoliageFieldCell);
                        // NutrientAt is milli-of-milli (kSoilBaseline == 1.0 nutrient), so
                        // normalise by kSoilBaseline (NOT 1000) to a [0,1] fraction before
                        // blending.
                        const float nutFrac =
                            static_cast<float>(nut) /
                            static_cast<float>(luminumbra::foliage::kSoilBaseline);
                        s.soil_quality =
                            luminumbra::foliage::clamp01(s.soil_quality * 0.5f + nutFrac * 0.5f);
                    }
                    s.temperature = temp;
                    // Real day/night light from the sim clock. Triangular day
                    // curve peaking at noon, with a twilight floor so growth slows (not halts) at
                    // night. kTicksPerDay = 20-min day @30Hz (mirrors the circadian slot).
                    constexpr std::uint64_t kTicksPerDay = 30ull * 60ull * 20ull;
                    const float tod01 =
                        m_activeRegionsEnabled
                            ? static_cast<float>(m_worldClock.solar_phase(current_tick))
                            : static_cast<float>(current_tick % kTicksPerDay) /
                                  static_cast<float>(kTicksPerDay);
                    const float noonDist = tod01 < 0.5f ? (0.5f - tod01) : (tod01 - 0.5f);
                    const float day = 1.0f - 2.0f * noonDist; // 0 at midnight -> 1 at noon
                    s.light = luminumbra::foliage::clamp01(0.15f + 0.85f * day);
                    //  (season): a deterministic ANNUAL temperature swing folded onto the terrain
                    // temp so growth varies across the year — colder in winter (slower growth +
                    // cold stress), warmer in summer. Triangular (libm-free) over a year of
                    // kSeasonDays in-game days; tick-based -> run==replay (kSeasonSeedOffset 37
                    // reserved, no RNG). Only opt-in plant rosters run this, so the canonical
                    // (no-plant) world is unchanged.
                    constexpr std::uint64_t kSeasonDays = 8ull;
                    const std::uint64_t kTicksPerYear = kTicksPerDay * kSeasonDays;
                    const float season01 =
                        m_activeRegionsEnabled
                            ? static_cast<float>(m_worldClock.year_phase(current_tick))
                            : static_cast<float>(current_tick % kTicksPerYear) /
                                  static_cast<float>(kTicksPerYear);
                    const float midDist = season01 < 0.5f ? (0.5f - season01) : (season01 - 0.5f);
                    const float warmth = 1.0f - 2.0f * midDist; // 0 = midwinter -> 1 = midsummer
                    constexpr float kSeasonAmplitude = 0.30f;   // +-0.15 temperature swing
                    s.temperature = luminumbra::foliage::clamp01(
                        s.temperature + (warmth - 0.5f) * kSeasonAmplitude);
                    return s;
                };
            luminumbra::foliage::RunPlantGrowthSystemOnTick(m_registry, current_tick, plant_env);
        }

        // 7. LIVING-WORLD SYSTEMS: pollination / disease / fire / wildlife-grazing / lifespan
        // / … Each is per-entity OPT-IN via its own participant component, so a world carrying none
        // runs ZERO of them and world_hash stays byte-identical (canonical NetworkStateHash
        // baseline holds) — same discipline as plants/creatures/scent. Deterministic (id-ordered,
        // libm-free, seeded-from-ints). Fixed run order for run==replay. NOTE: the soil-nutrient +
        // irrigation FIELDS now update at slot 5b (BEFORE plant growth) so growth reads fresh
        // availability; only the consumers remain here. Wind coupling (fire/pollination) drifts
        // downwind.
        {
            namespace Comp = Luminumbra::Components;
            namespace fol = luminumbra::foliage;
            // Wind coupling: fire spreads + pollen drifts DOWNWIND, sampled from the
            // (already deterministic, already-hashed) wind field at the anchor.
            const ::Luminumbra::Vec2 windXZ =
                m_windFieldSystem ? m_windFieldSystem->SampleWind(m_metadata.spawnPoint)
                                  : ::Luminumbra::Vec2(0.0f);

            budget.Next(Stage::Pollination, [&] {
                return SimParticipantCount<TelemetryComp::PollinationTag,
                                           TelemetryComp::PlantGenomeComponent,
                                           TelemetryComp::PlantGrowthComponent,
                                           TelemetryComp::TransformComponent,
                                           TelemetryComp::PollinationComponent>(m_registry);
            });
            if (!m_registry.view<Comp::PlantGenomeComponent>().empty())
                fol::RunPollinationOnTick(m_registry, current_tick, windXZ, 0, m_plantMutationRate);
            budget.Next(Stage::Disease, [&] {
                return SimParticipantCount<TelemetryComp::PlantTag,
                                           TelemetryComp::PlantHealthComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::PlantHealthComponent>().empty())
                fol::RunPlantDiseaseOnTick(m_registry, current_tick);
            //  germination: ripe plants senesce -> annual dies + reseeds, perennial
            // resets + reseeds (child genome = the pollination cross, computed just above, or
            // self). Runs AFTER pollination so next_genome is fresh. Opt-in via
            // CropLifecycleComponent.
            budget.Next(Stage::Crops, [&] {
                return SimParticipantCount<TelemetryComp::CropLifecycleComponent,
                                           TelemetryComp::PlantGrowthComponent,
                                           TelemetryComp::PlantGenomeComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::CropLifecycleComponent>().empty())
                fol::RunCropLifecycleOnTick(m_registry, current_tick);
            budget.Next(Stage::Fire, [&] {
                return SimParticipantCount<TelemetryComp::CombustibleComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::CombustibleComponent>().empty()) {
                std::vector<luminumbra::sim::FireIgnitionSource> weather_ignitions;
                if (m_weatherSystem) {
                    for (const Systems::StrikeEvent& strike : m_weatherSystem->StrikesThisTick()) {
                        weather_ignitions.push_back({
                            glm::vec2(strike.world_x, strike.world_z),
                            3.0f + strike.magnitude * 5.0f,
                            std::max(0.25f, strike.magnitude),
                        });
                    }
                }
                luminumbra::sim::RunFireSpreadOnTick(
                    m_registry, current_tick, windXZ, weather_ignitions);
            }
            budget.Next(Stage::Grazing, [&] {
                return SimParticipantCount<TelemetryComp::GrazeableComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::GrazeableComponent>().empty())
                luminumbra::ai::RunWildlifeFoliageOnTick(
                    m_registry, current_tick, m_wildlifeFoliageTuning);
            budget.Next(Stage::Lifespan, [&] {
                return SimParticipantCount<TelemetryComp::MortalComponent>(m_registry);
            });
            if (!m_registry.view<Comp::MortalComponent>().empty())
                luminumbra::ai::RunLifespanOnTick(m_registry, current_tick);
            // Herd alarm: propagate collective vigilance among same-role creatures. The brain
            // (slot 2e) sets AlarmComponent.alarmed when a prey senses a threat and reads the
            // propagated level to flee with the herd; this maintains the field.
            budget.Next(Stage::Alarm, [&] {
                return SimParticipantCount<TelemetryComp::AlarmComponent,
                                           TelemetryComp::CreatureComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::AlarmComponent>().empty())
                luminumbra::ai::RunHerdAlarmOnTick(
                    m_registry, static_cast<float>(m_simulationClock.fixed_dt()));
            // Decomposition: dead creatures decay + release nutrient (consumed by the soil loop).
            budget.Next(Stage::Decay, [&] {
                return SimParticipantCount<TelemetryComp::DecayComponent>(m_registry);
            });
            if (!m_registry.view<Comp::DecayComponent>().empty())
                luminumbra::ai::RunDecompositionOnTick(m_registry, current_tick);
            // Circadian: diurnal/nocturnal activity from a tick-derived day fraction.
            budget.Next(Stage::Circadian, [&] {
                return SimParticipantCount<TelemetryComp::CircadianComponent>(m_registry);
            });
            if (!m_registry.view<Comp::CircadianComponent>().empty()) {
                constexpr std::uint64_t kTicksPerDay = 30ull * 60ull * 20ull; // 20-min day @30Hz
                const float tod01 = m_activeRegionsEnabled
                                        ? static_cast<float>(m_worldClock.solar_phase(current_tick))
                                        : static_cast<float>(current_tick % kTicksPerDay) /
                                              static_cast<float>(kTicksPerDay);
                luminumbra::ai::RunCircadianOnTick(m_registry, tod01, m_circadianAmplitude);
            }
            // Territory: claim home + emit a homing bias (movement blends it like mate-seeking).
            budget.Next(Stage::Territory, [&] {
                return SimParticipantCount<TelemetryComp::TerritoryComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::TerritoryComponent>().empty())
                luminumbra::ai::RunTerritoryOnTick(m_registry, current_tick);
            // Predator pack: flanking coordination (a pack surrounds the prey).
            budget.Next(Stage::Packs, [&] {
                return SimParticipantCount<TelemetryComp::PackHunterComponent,
                                           TelemetryComp::CreatureComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::PackHunterComponent>().empty())
                luminumbra::ai::RunPredatorPackOnTick(m_registry, current_tick);
            // Migration: seasonal drive toward a moving target (tick-derived year fraction).
            budget.Next(Stage::Migration, [&] {
                return SimParticipantCount<TelemetryComp::MigratoryComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            if (!m_registry.view<Comp::MigratoryComponent>().empty()) {
                constexpr std::uint64_t kTicksPerYear = 30ull * 60ull * 60ull; // ~1h year @30Hz
                const float season01 =
                    m_activeRegionsEnabled
                        ? static_cast<float>(m_worldClock.spring_phase(current_tick))
                        : static_cast<float>(current_tick % kTicksPerYear) /
                              static_cast<float>(kTicksPerYear);
                luminumbra::ai::RunMigrationOnTick(m_registry, season01);
            }
        }

        budget.Next(Stage::Events, [&] { return m_simulationEventBus.pending_count(); });
        m_simulationEventBus.drain(current_tick);
        // The v2 events stage measures only the ordered drain. Region scheduling
        // records its separate time-free work items after the timed stages.
        budget.Finish();
        RunRegionScheduler(current_tick);
        if (m_regionFault)
            return i + 1; // the faulting tick's systems ran; the clock was rewound to it
    }
    return ticks_executed;
}

WorldConfigValidationResult
GameSession::ValidateWorldConfig(const std::string& root_path,
                                 const std::string& worldType,
                                 const std::vector<std::filesystem::path>& required_assets,
                                 const std::stop_token& stop) {
    WorldConfigValidationResult result;
    result.ok = true;

    if (!IsSafeWorldType(worldType)) {
        AddValidationError(result,
                           "world type must be non-empty and must not contain path separators: " +
                               worldType);
        return result;
    }

    const fs::path root = RuntimeRoot(root_path);
    result.preset_path = PresetPathFor(root_path, worldType);
    if (!fs::exists(result.preset_path)) {
        AddValidationError(result,
                           "missing world preset: " +
                               Filesystem::GenericUtf8PathString(result.preset_path));
    }

    // Preset loading below validates simulation dependencies. Further runtime
    // assets are caller-supplied (the client registers shaders/RML/fonts; a
    // headless host registers none).
    for (const fs::path& relative : required_assets) {
        const fs::path path = root / relative;
        if (!fs::exists(path)) {
            AddValidationError(result,
                               "missing required runtime asset: " +
                                   Filesystem::GenericUtf8PathString(path));
        }
    }

    if (result.ok) {
        const TerrainPresetLoadResult preset =
            LoadTerrainPreset(result.preset_path, root / "data", stop);
        if (!preset.ok) {
            for (const std::string& error : preset.errors) {
                AddValidationError(result, error);
            }
        }
    }

    return result;
}

bool GameSession::CreateWorld(const std::string& name,
                              const std::string& seed,
                              const std::string& worldType,
                              const std::string* customPresetJson) {
    return CreateWorld(name, seed, worldType, customPresetJson, TerrainPresetUse::NewWorld);
}

bool GameSession::CreateWorld(const std::string& name,
                              const std::string& seed,
                              const std::string& worldType,
                              const std::string* customPresetJson,
                              Persistence::CreatedWorldArtifacts& artifacts) {
    return CreateWorldInternal(
        name, seed, worldType, customPresetJson, false, &artifacts, TerrainPresetUse::NewWorld);
}

bool GameSession::CreateWorld(const std::string& name,
                              const std::string& seed,
                              const std::string& worldType,
                              const std::string* customPresetJson,
                              TerrainPresetUse presetUse,
                              const std::string& requiredPresetContentHash) {
    return CreateWorldInternal(name,
                               seed,
                               worldType,
                               customPresetJson,
                               false,
                               nullptr,
                               presetUse,
                               requiredPresetContentHash);
}

bool GameSession::CreateTransientWorld(const std::string& name,
                                       const std::string& seed,
                                       const std::string& worldType) {
    return CreateWorldInternal(
        name, seed, worldType, nullptr, true, nullptr, TerrainPresetUse::NewWorld);
}

std::shared_ptr<GameSession::WorldPreparation>
GameSession::PrepareWorldCreation(const WorldPreparationOptions& options,
                                  const std::string& name,
                                  const std::string& seed,
                                  const std::string& worldType,
                                  const std::string* customPresetJson,
                                  TerrainPresetUse presetUse,
                                  const std::string& requiredPresetContentHash,
                                  const std::stop_token& stop) {
    auto prepared = std::make_shared<WorldPreparation>();
    prepared->options = options;
    prepared->stop = stop;
    prepared->create = true;
    prepared->transient = options.transient_creation;
    if (stop.stop_requested())
        return prepared;
    if (name.size() > Persistence::SavedInputLimits::kWorldNameBytes ||
        seed.size() > Persistence::SavedInputLimits::kWorldSeedBytes) {
        prepared->error =
            "Saved-input admission refused: world name or seed exceeds 4 KiB compatibility limit.";
        return prepared;
    }

    if (!options.job_system_available) {
        prepared->error = "JobSystem not set before creating world.";
        LUMINUMBRA_CORE_ERROR("{}", prepared->error);
        return prepared;
    }

    const WorldConfigValidationResult validation =
        ValidateWorldConfig(options.root_path, worldType, options.required_assets, stop);
    if (!validation.ok) {
        prepared->error =
            validation.errors.empty() ? "Invalid world configuration." : validation.errors.front();
        for (const std::string& error : validation.errors) {
            LUMINUMBRA_CORE_ERROR("World config validation failed: {}", error);
        }
        return prepared;
    }

    WorldMetadata metadata;
    metadata.name = name;
    metadata.seed = seed.empty() ? std::to_string(std::random_device{}()) : seed;
    metadata.worldType = worldType;
    metadata.worldId = options.transient_creation ? std::string{} : GenerateWorldId();
    metadata.creationTime = std::time(nullptr);
    metadata.spawnPoint = Vec3(0, 100, 0);

    nlohmann::json resolved;
    try {
        if (customPresetJson) {
            if (customPresetJson->size() > Persistence::SavedInputLimits::kPresetBytes ||
                !Persistence::SavedInputLimits::JsonDepthAllowed(*customPresetJson)) {
                prepared->error = "Saved-input admission refused: preset compatibility limit.";
                return prepared;
            }
            resolved = nlohmann::json::parse(*customPresetJson);
        } else {
            bool missing = false;
            if (!Persistence::ReadSavedJson(validation.preset_path,
                                            Persistence::SavedInputLimits::kPresetBytes,
                                            resolved,
                                            missing,
                                            false,
                                            nullptr,
                                            stop) ||
                missing) {
                prepared->error = "Could not read this world's preset.";
                return prepared;
            }
        }
        resolved = ResolveEffectivePreset(resolved, presetUse);
    } catch (const nlohmann::json::exception& error) {
        prepared->error = "Could not parse this world's preset: " + std::string(error.what());
        return prepared;
    }
    // Admit the exact compact encoding plus its trailing newline before any
    // directory creation, recursive preset validation or live-world replacement.
    std::string saved_preset_json;
    if (!Persistence::BoundedJsonDump(resolved,
                                      Persistence::SavedInputLimits::kPresetBytes - 1,
                                      saved_preset_json,
                                      -1,
                                      false)) {
        prepared->error = "Saved-input admission refused: preset compatibility limit.";
        return prepared;
    }
    // Both persisted and transient worlds adopt the parameters and identity of
    // these same resolved bytes. Explicit revision 1 remains authoritative.
    TerrainPresetLoadResult preset =
        LoadTerrainPresetFromJson(resolved,
                                  RuntimeRoot(options.root_path) / "data",
                                  "<memory>",
                                  World::ReadWorldgenSymlinkStatus,
                                  World::ReadWorldgenFile,
                                  stop);
    if (!preset.ok) {
        prepared->error = preset.errors.empty() ? "Invalid world preset." : preset.errors.front();
        for (const std::string& error : preset.errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return prepared;
    }
    // Use the identity of the dependency snapshot generation will adopt. A
    // replay preflight cannot admit dependency changes between resolution and boot.
    if (!requiredPresetContentHash.empty() &&
        preset.preset_content_hash != requiredPresetContentHash) {
        prepared->error = "This world's resolved preset content does not match the recording.";
        return prepared;
    }

    if (stop.stop_requested())
        return prepared;
    prepared->metadata = std::move(metadata);
    prepared->preset = std::move(preset);
    prepared->preset_json = std::move(saved_preset_json);
    prepared->ready = true;
    return prepared;
}

bool GameSession::CreateWorldInternal(const std::string& name,
                                      const std::string& seed,
                                      const std::string& worldType,
                                      const std::string* customPresetJson,
                                      bool transient,
                                      Persistence::CreatedWorldArtifacts* artifacts,
                                      TerrainPresetUse presetUse,
                                      const std::string& requiredPresetContentHash) {
    auto options = CaptureWorldPreparationOptions();
    options.transient_creation = transient;
    auto prepared = PrepareWorldCreation(
        options, name, seed, worldType, customPresetJson, presetUse, requiredPresetContentHash);
    return AdoptPreparedWorld(prepared, artifacts);
}

bool GameSession::AdoptCreatedWorld(WorldPreparation& prepared,
                                    Persistence::CreatedWorldArtifacts* artifacts) {
    auto& metadata = prepared.metadata;
    auto& preset = prepared.preset;
    auto& saved_preset_json = prepared.preset_json;
    const bool transient = prepared.transient;
    const auto worldPath = prepared.options.save_root / "worlds/saves" / metadata.worldId;
    if (!transient) {
        try {
            fs::create_directories(worldPath.parent_path());
            if (artifacts)
                artifacts->create_root(worldPath);
            else if (!fs::create_directory(worldPath))
                throw std::runtime_error("Refusing to reuse an existing world save directory.");
        } catch (const std::exception& e) {
            m_worldOpenError = "Could not create world save directory: " + std::string(e.what());
            LUMINUMBRA_CORE_ERROR("{}", m_worldOpenError);
            return false;
        }
    }
    // Keep the same effective preset for persistent worlds and explicit transient snapshots.
    // Save inspection uses compact UTF-8 JSON; identity uses ASCII escapes of these choices.
    if (!transient) {
        const fs::path world_preset = worldPath / "preset.json";
        try {
            if (!Persistence::WorldSaveService::write_file_durably(
                    world_preset, saved_preset_json + "\n", nullptr, artifacts)) {
                m_worldOpenError = "Could not save this world's resolved preset.";
                return false;
            }
        } catch (const std::exception& error) {
            m_worldOpenError =
                "Could not save this world's resolved preset: " + std::string(error.what());
            return false;
        }
    }
    // Copies the immutable dependency snapshot with the identity's parameters.
    TerrainGenParams params = preset.params;
    ClampTerrainParams(params);

    ResetWorldSystems();
    if (!m_activeRegionsEnabled)
        m_renderCalendar = WorldCalendar{};
    m_presetContentHash = preset.preset_content_hash;
    m_savedPresetJson = std::move(saved_preset_json);
    m_metadata = std::move(metadata);
    m_transientWorld = transient;
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    const int world_seed = static_cast<int>(StringToSeed(m_metadata.seed));
    LUMINUMBRA_CORE_INFO("Loaded world preset '{}': height_offset={}, amplitude={}, caves={}",
                         m_metadata.worldType,
                         params.height_offset,
                         params.base_amplitude,
                         params.caves_enabled);

    // 1. Create the World System
    m_worldSystem =
        std::make_unique<Systems::SHIELD_WorldSystem>(m_jobSystem, nullptr, params, world_seed);
    // Worldgen seam for audio-raycast surface materials (non-owning; physics
    // is destroyed before the world system by member order, so the pointer
    // cannot dangle).
    m_physicsSystem->set_world_system(m_worldSystem.get());
    LUMINUMBRA_CORE_INFO("World system initialized for seed {}.", world_seed);

    // 2. Create the Water System
    m_waterSystem = std::make_unique<Systems::WaterSystem>(m_jobSystem, m_worldSystem.get());
    LUMINUMBRA_CORE_INFO("Water system initialized.");

    // 3. Link them together
    m_worldSystem->SetWaterSystem(m_waterSystem.get());
    m_worldSystem->SetWaterHydrology(m_finiteHydrologyEnabled, 0, 0);
    LUMINUMBRA_CORE_INFO("World and water systems linked.");

    // sim.water_high_res: raise the solver to High BEFORE any chunk seeds
    // (explicit OFF = selected Medium = byte-identical).
    ApplyWaterResolutionWiring();

    // 4.  : the deterministic wind field. Pure function of the world
    //    seed (uses seed+11 for its base-direction noise); updated per tick.
    m_windFieldSystem = std::make_unique<Systems::WindFieldSystem>(world_seed);
    LUMINUMBRA_CORE_INFO("Wind field system initialized.");

    // 5.  : the deterministic weather core. Pure function of the world
    //    seed (uses seed+12 for its pressure/climate noise + storm schedule);
    //    updated per tick AFTER wind (advects storm cells by the wind grid).
    m_weatherSystem = std::make_unique<Systems::WeatherSystem>(world_seed);
    LUMINUMBRA_CORE_INFO("Weather system initialized.");

    // 6.: the deterministic Aether scalar field. Pure function of the
    //    world seed (uses seed+14 for its emission noise); updated per tick AFTER
    //    weather, advected by the wind grid. Feeds the world_hash `aether` slot.
    m_aetherFieldSystem = std::make_unique<Systems::AetherFieldSystem>(world_seed);
    LUMINUMBRA_CORE_INFO("Aether field system initialized.");

    // the stateful energy layer, constructed ONLY when
    // sim.aether_state opted in (explicit OFF -> null -> byte-identical).
    InitializeEnergyFieldState();

    // Calculate appropriate spawn point based on actual terrain height
    const float spawn_x = 8.0f;
    const float spawn_z = 8.0f;
    float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
    LUMINUMBRA_CORE_INFO("Initial terrain height sampled at spawn: {}.", terrain_height);

    m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + kSpawnEyeHeight, spawn_z);
    m_ambientFieldAnchor = m_metadata.spawnPoint;
    if (m_activeRegionsEnabled)
        m_activeRegionLedger.try_set_local_anchor(m_metadata.spawnPoint);
    AttachRegionEditObserver();
    InitializeScentField(m_metadata.spawnPoint);

    // Adopt the registry parsed from the bytes bound into the session identity.
    m_speciesTable = preset.params.worldgen_dependencies->species_table;

    // wire weather-driven rain into the water solver when the
    // owner opted in (sim.hydrology_weather via SetWeatherRainEnabled). Explicit OFF =
    // null pointer = byte-identical.
    ApplyWeatherRainWiring();

    LUMINUMBRA_CORE_INFO(
        "World created successfully: {} (ID: {})", m_metadata.name, m_metadata.worldId);
    LUMINUMBRA_CORE_INFO("Spawn point set to ({}, {}, {}) - terrain height: {}",
                         m_metadata.spawnPoint.x,
                         m_metadata.spawnPoint.y,
                         m_metadata.spawnPoint.z,
                         terrain_height);

    // Save world metadata
    if (m_activeRegionsEnabled)
        RestoreWorldClock(m_worldClock);
    // Systems, clock and identity are coherent; allow the initial metadata save.
    m_worldUsable = true;
    if (!transient && !SaveWorldMetadataTo(GetWorldSaveDir(), artifacts)) {
        m_worldUsable = false;
        m_worldOpenError = "Could not save the new world's metadata.";
        LUMINUMBRA_CORE_ERROR("{}", m_worldOpenError);
        return false;
    }
    return true;
}

bool GameSession::LoadWorld(const std::string& worldId) {
    if (!IsSimulationTickBoundary())
        return false;
    return AdoptPreparedWorld(PrepareWorldLoad(CaptureWorldPreparationOptions(), worldId));
}

bool GameSession::LoadWorldFrom(const std::string& worldId, const fs::path& save_root) {
    if (!IsSimulationTickBoundary())
        return false;
    auto options = CaptureWorldPreparationOptions();
    options.save_root = save_root;
    return AdoptPreparedWorld(PrepareWorldLoad(options, worldId));
}

std::shared_ptr<GameSession::WorldPreparation>
GameSession::PrepareWorldLoad(const WorldPreparationOptions& options,
                              const std::string& worldId,
                              const std::stop_token& stop) {
    auto prepared = std::make_shared<WorldPreparation>();
    prepared->options = options;
    prepared->stop = stop;
    if (stop.stop_requested())
        return prepared;
    auto saved = Persistence::InspectSavedWorld(
        options.save_root, RuntimeRoot(options.root_path), worldId, stop);
    if (saved.pending_recovery && saved.error.empty()) {
        const Persistence::WorldSaveService::SaveOwnership ownership;
        std::vector<std::string> errors;
        if (Persistence::recover_pending_journal(options.save_root / "worlds/saves" / worldId,
                                                 &errors) == Persistence::RecoveryOutcome::Failed) {
            prepared->error = errors.empty()
                                  ? "World unavailable: interrupted save recovery failed."
                                  : errors.front();
            return prepared;
        }
        saved = Persistence::InspectSavedWorld(
            options.save_root, RuntimeRoot(options.root_path), worldId, stop);
    }
    prepared->error = saved.error;
    if (prepared->error.empty() && saved.requires_active_regions && !options.active_regions)
        prepared->error = "Incompatible configuration: this world requires sim.active_regions.";
    if (!prepared->error.empty()) {
        LUMINUMBRA_CORE_ERROR("World open refused: {}", prepared->error);
        return prepared;
    }
    if (!options.job_system_available) {
        prepared->error = "JobSystem not set before loading world.";
        return prepared;
    }
    for (const auto& asset : options.required_assets) {
        if (!fs::exists(RuntimeRoot(options.root_path) / asset)) {
            prepared->error =
                "Missing required runtime asset: " + Filesystem::GenericUtf8PathString(asset);
            return prepared;
        }
    }
    const fs::path worldPath = options.save_root / "worlds/saves" / worldId;
    // Generate from the same canonical bytes whose saved identity was verified.
    TerrainPresetLoadResult preset =
        LoadTerrainPresetFromJson(nlohmann::json::parse(saved.preset_json),
                                  RuntimeRoot(options.root_path) / "data",
                                  "<memory>",
                                  World::ReadWorldgenSymlinkStatus,
                                  World::ReadWorldgenFile,
                                  stop);
    if (!preset.ok) {
        prepared->error = preset.errors.empty() ? "Invalid world preset." : preset.errors.front();
        for (const std::string& error : preset.errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return prepared;
    }
    // Validate all refusing state readers before replacing the live world.
    auto& loaded = prepared->loaded;
    if (!ReadPreparedWorldState(worldPath, options, loaded, prepared->error, true, stop))
        return prepared;

    if (stop.stop_requested())
        return prepared;
    prepared->metadata = saved.metadata;
    prepared->has_spawn_point = saved.has_spawn_point;
    prepared->preset_json = saved.preset_json;
    prepared->preset = std::move(preset);
    prepared->ready = true;
    return prepared;
}

std::shared_ptr<GameSession::WorldPreparation>
GameSession::PrepareWorldState(const WorldPreparationOptions& options,
                               const fs::path& save_directory,
                               const std::stop_token& stop) {
    auto prepared = std::make_shared<WorldPreparation>();
    prepared->options = options;
    prepared->stop = stop;
    prepared->state_only = true;
    prepared->state_directory = save_directory;
    if (save_directory.empty()) {
        prepared->error = "World state directory is empty.";
        return prepared;
    }
    prepared->ready = ReadPreparedWorldState(
        save_directory, options, prepared->loaded, prepared->error, false, stop);
    return prepared;
}

bool GameSession::AdoptLoadedWorld(WorldPreparation& prepared) {
    auto& saved = prepared;
    auto& preset = prepared.preset;
    auto& loaded = prepared.loaded;
    const auto worldPath = prepared.options.save_root / "worlds/saves" / prepared.metadata.worldId;
    TerrainGenParams params = preset.params;
    ClampTerrainParams(params);
    ResetWorldSystems();
    m_presetContentHash = preset.preset_content_hash;
    m_savedPresetJson = std::move(saved.preset_json);
    m_metadata = saved.metadata;
    m_renderCalendar = loaded.render_calendar;
    if (m_activeRegionsEnabled) {
        m_worldClock = loaded.clock;
        m_simulationClock.reset(m_worldClock.tick());
    }
    m_transientWorld = false;
    const int world_seed = static_cast<int>(StringToSeed(m_metadata.seed));

    // Initialize Physics System after config and preset validation passes.
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    m_worldSystem =
        std::make_unique<Systems::SHIELD_WorldSystem>(m_jobSystem, nullptr, params, world_seed);
    // Worldgen seam for audio-raycast surface materials (non-owning; physics
    // is destroyed before the world system by member order, so the pointer
    // cannot dangle).
    m_physicsSystem->set_world_system(m_worldSystem.get());
    m_waterSystem = std::make_unique<Systems::WaterSystem>(m_jobSystem, m_worldSystem.get());
    m_worldSystem->SetWaterSystem(m_waterSystem.get());
    m_worldSystem->SetWaterHydrology(m_finiteHydrologyEnabled, 0, 0);

    // sim.water_high_res: raise the solver to High BEFORE any chunk is adopted
    // or seeded (explicit OFF = selected Medium = byte-identical). A loaded save
    // at another resolution migrates in LoadWorldStateFrom's boot pass.
    ApplyWaterResolutionWiring();

    // The legacy path starts at zero. Active-region clocks restore the ambient
    // fields at the saved absolute tick in LoadWorldStateFrom, before any tick.
    m_windFieldSystem = std::make_unique<Systems::WindFieldSystem>(world_seed);
    m_weatherSystem = std::make_unique<Systems::WeatherSystem>(world_seed);
    m_aetherFieldSystem = std::make_unique<Systems::AetherFieldSystem>(world_seed);

    // unlike the re-derivable trio above, the stateful
    // energy layer is AUTHORITATIVE state — when enabled, a persisted record
    // restores it (epoch-rebased onto the session clock); an absent
    // record is all-zeros by contract. OFF -> null -> byte-identical.
    InitializeEnergyFieldState();

    // Legacy saves without a persisted spawnPoint: derive it from terrain
    // height exactly like CreateWorld does (pure function of seed/params).
    if (!saved.has_spawn_point) {
        const float spawn_x = 8.0f;
        const float spawn_z = 8.0f;
        const float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
        m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + kSpawnEyeHeight, spawn_z);
    }
    InitializeScentField(m_metadata.spawnPoint);

    // Saved worlds adopt the same captured session content as new worlds.
    m_speciesTable = preset.params.worldgen_dependencies->species_table;

    // same weather-rain wiring as CreateWorld (explicit OFF = no-op).
    ApplyWeatherRainWiring();

    m_worldOpenError.clear();
    AttachRegionEditObserver();
    AdoptWorldStateFrom(worldPath, loaded);
    m_worldUsable = true;
    LUMINUMBRA_CORE_INFO("World loaded successfully: {}", m_metadata.name);
    return true;
}

bool GameSession::ClockConfigurationCompatible(const fs::path& save_dir) const {
    // An interrupted save may have left uncommitted clock-bearing metadata: recover before reading
    // the clock so the compatibility verdict describes the committed snapshot.
    // Never during a live transaction on this directory (its journal is the active one), and
    // never for an empty destination (a relative join would resolve against the cwd).
    std::vector<std::string> recovery_errors;
    if (!save_dir.empty() && !Persistence::SaveTransaction::active_for(save_dir) &&
        Persistence::recover_pending_journal(save_dir, &recovery_errors) ==
            Persistence::RecoveryOutcome::Failed) {
        for (const std::string& error : recovery_errors)
            LUMINUMBRA_CORE_ERROR("World save recovery failed: {}", error);
        return false;
    }
    WorldClock clock;
    bool required = false;
    std::optional<WorldCalendar> render_calendar;
    const bool valid = Persistence::WorldSaveService::read_clock_metadata(
        save_dir, clock, required, nullptr, nullptr, &render_calendar);
    return valid && (!required || m_activeRegionsEnabled);
}

void GameSession::RunRegionScheduler(std::uint64_t current_tick) {
    if (m_activeRegionsEnabled && !m_regionFault) {
        // Water divergence becomes LoadedOrEdited authority BEFORE any digest or ownership
        // probe.
        m_worldSystem->note_persistence_tick(static_cast<std::int64_t>(current_tick));
        m_worldSystem->sync_water_authority();
        const auto anchors =
            m_replicatedSimulationAnchors
                ? std::optional<std::span<const Vec3>>(*m_replicatedSimulationAnchors)
                : std::nullopt;
        // Built lazily on the first probe call of this tick.
        std::optional<std::set<RegionKey>> owned_regions;
        try {
            m_regionSchedule = m_activeRegionLedger.schedule(
                m_worldClock,
                anchors,
                m_regionWork,
                [this](RegionKey key) {
                    const auto directory = m_regionDurableDirectory.empty()
                                               ? GetWorldSaveDir()
                                               : m_regionDurableDirectory;
                    return Persistence::WorldSaveService::region_simulation_digest(
                        directory,
                        key,
                        Persistence::RegionDigestInputs{m_worldSystem->snapshot_streamed_chunks(),
                                                        m_worldSystem->snapshot_parked_chunks(),
                                                        m_worldSystem->durable_chunk_inventory()});
                },
                // Later slices extend this probe with ground objects,
                // pages, creatures, plants and water debt.
                [this, &owned_regions](RegionKey key) {
                    if (!owned_regions) {
                        owned_regions.emplace();
                        for (const auto& entry : m_regionWork)
                            owned_regions->insert(entry.key);
                        for (const auto& coords : m_worldSystem->durable_chunk_inventory()) {
                            int rx = 0, rz = 0;
                            Persistence::WorldSaveService::region_coords_for_chunk(coords, rx, rz);
                            owned_regions->insert(RegionKey{rx, rz});
                        }
                        for (const auto& chunk : m_worldSystem->snapshot_authoritative_chunks()) {
                            if (!chunk)
                                continue;
                            int rx = 0, rz = 0;
                            Persistence::WorldSaveService::region_coords_for_chunk(
                                chunk->get_coords(), rx, rz);
                            owned_regions->insert(RegionKey{rx, rz});
                        }
                    }
                    return owned_regions->contains(key);
                });
            m_regionWork.clear();
        } catch (const std::exception& error) {
            LatchRegionFault(current_tick, error.what());
        }
    }
    using luminumbra::simulation::SimWork;
    m_simBudgetTelemetry.RecordWork(SimWork::ActiveRegionRecords,
                                    current_tick,
                                    m_activeRegionsEnabled ? m_regionSchedule.ledger_records : 0);
    m_simBudgetTelemetry.RecordWork(SimWork::ActiveRegionDue,
                                    current_tick,
                                    m_activeRegionsEnabled ? m_regionSchedule.due.size() : 0);
}

void GameSession::SetRegionSchedulerConfig(RegionSchedulerConfig config) {
    if (m_worldSystem)
        throw std::logic_error("Set region scheduler configuration before creating a world");
    ReplaceActiveRegionLedger(ActiveRegionLedger(config));
    m_regionSchedulerOverride = config;
}
void GameSession::ReplaceActiveRegionLedger(ActiveRegionLedger ledger) {
    m_activeRegionLedger = std::move(ledger);
    // Retain the effective settings when create/transient/reset clears region state.
    m_regionSchedulerConfig = m_activeRegionLedger.config();
}
void GameSession::LatchRegionFault(std::uint64_t tick, const std::string& message) {
    if (m_regionFault)
        return; // first fault wins
    m_regionFault = RegionSchedulerFault{tick, message};
    m_simulationClock.reset(tick);
    m_activeRegionLedger.acknowledge_world_clock(m_worldClock);
    m_regionWork.clear();
    LUMINUMBRA_CORE_ERROR("Region scheduler fault at tick {}: {}", tick, message);
}
void GameSession::SetLocalPlayerSimulationPosition(const Vec3& feet, bool walking) {
    if (m_activeRegionsEnabled && walking && !m_activeRegionLedger.try_set_local_anchor(feet))
        ++m_regionInputRejections.anchors;
}
void GameSession::SetReplicatedSimulationAnchors(std::vector<Vec3> anchors) {
    if (m_activeRegionsEnabled)
        m_replicatedSimulationAnchors = std::move(anchors);
}
void GameSession::NotifyGroundObjectEdit(const Vec3& position) {
    if (!m_activeRegionsEnabled)
        return;
    if (!ActiveRegionLedger::valid_anchor(position)) {
        ++m_regionInputRejections.edits;
        return;
    }
    try {
        m_activeRegionLedger.mark_edited(ActiveRegionLedger::region_at(position), m_worldClock);
    } catch (const std::length_error& error) {
        LatchRegionFault(m_worldClock.tick(), error.what());
    } catch (const std::invalid_argument&) {
        ++m_regionInputRejections.edits;
    }
}
void GameSession::PinActiveRegion(RegionKey key, bool pinned) {
    if (!m_activeRegionsEnabled)
        return;
    try {
        m_activeRegionLedger.pin(key, pinned, m_worldClock);
    } catch (const std::length_error& error) {
        LatchRegionFault(m_worldClock.tick(), error.what());
    } catch (const std::invalid_argument&) {
        ++m_regionInputRejections.pins;
    }
}
void GameSession::RecordActiveRegionWork(RegionWork work) {
    if (m_activeRegionsEnabled && !m_regionFault)
        m_regionWork.push_back(work);
}
void GameSession::AttachRegionEditObserver() {
    if (m_activeRegionsEnabled && m_worldSystem) {
        m_worldSystem->set_durable_residency(true);
        m_worldSystem->set_parked_bytes_limit(m_parkedBytesLimit);
        m_worldSystem->set_durable_chunk_loader(
            [this](const IVec3& coords) {
                const auto directory =
                    m_regionDurableDirectory.empty() ? GetWorldSaveDir() : m_regionDurableDirectory;
                const auto result =
                    Persistence::WorldSaveService::read_chunk(directory, coords, true);
                if (result.status != Persistence::WorldSaveService::ChunkReadStatus::Found)
                    throw std::runtime_error(
                        result.error.empty() ? "expected durable chunk is missing" : result.error);
                return result.chunk;
            },
            [this](const std::string& error) { LatchRegionFault(m_worldClock.tick(), error); });
        m_worldSystem->SetVoxelEditObserver([this](const IVec3& chunk) {
            int x = 0, z = 0;
            Persistence::WorldSaveService::region_coords_for_chunk(chunk, x, z);
            try {
                m_activeRegionLedger.mark_edited({x, z}, m_worldClock);
            } catch (const std::length_error& error) {
                LatchRegionFault(m_worldClock.tick(), error.what());
            } catch (const std::invalid_argument&) {
                ++m_regionInputRejections.edits;
            }
        });
    }
}

PersistenceStatus GameSession::GetPersistenceStatus() const {
    PersistenceStatus status;
    if (!m_activeRegionsEnabled || !m_worldSystem)
        return status;
    const auto parking = m_worldSystem->parking_status();
    status.parked_count = parking.parked_count;
    status.parked_bytes = parking.parked_bytes;
    status.oldest_parked_tick = parking.oldest_parked_tick;
    status.last_save_tick = m_lastSaveTick;
    status.last_save_ms = m_lastSaveMs;
    status.failures = m_saveFailures;
    status.save_requested = parking.save_requested;
    return status;
}

bool GameSession::PersistenceSaveRequested() const {
    if (!m_activeRegionsEnabled || !m_worldSystem || !m_worldSystem->persistence_save_requested())
        return false;
    if (m_lastSaveFailureTick >= 0 &&
        static_cast<std::int64_t>(GetSimulationTickCount()) <
            m_lastSaveFailureTick + static_cast<std::int64_t>(kSaveRetryTicks))
        return false;
    return true;
}

bool GameSession::PersistenceAutosaveDue() {
    if (!m_activeRegionsEnabled || !m_worldSystem)
        return false;
    const auto tick = static_cast<std::int64_t>(GetSimulationTickCount());
    if (m_lastSaveTick < 0) {
        m_lastSaveTick = tick; // first observation is the baseline, not a save
        return false;
    }
    if (m_lastSaveFailureTick >= 0 &&
        tick < m_lastSaveFailureTick + static_cast<std::int64_t>(kSaveRetryTicks))
        return false;
    return tick >= m_lastSaveTick + static_cast<std::int64_t>(kAutosaveTicks);
}

void GameSession::ClearPersistenceSaveRequest() {
    if (m_worldSystem)
        m_worldSystem->clear_persistence_save_request();
}

void GameSession::SetParkedBytesLimit(std::size_t bytes) {
    m_parkedBytesLimit = bytes;
    if (m_activeRegionsEnabled && m_worldSystem)
        m_worldSystem->set_parked_bytes_limit(bytes);
}

void GameSession::RecordPersistenceSaveOutcome(bool ok, double elapsed_ms) {
    if (!m_activeRegionsEnabled)
        return;
    if (m_worldSystem)
        m_worldSystem->note_persistence_outcome(ok);
    const auto tick = static_cast<std::int64_t>(GetSimulationTickCount());
    if (ok) {
        m_lastSaveTick = tick;
        m_lastSaveMs = elapsed_ms;
        m_lastSaveFailureTick = -1;
        if (m_worldSystem)
            m_worldSystem->clear_persistence_save_request();
    } else {
        ++m_saveFailures;
        m_lastSaveFailureTick = tick;
    }
}

std::string GameSession::ComputeChunkStateHash() {
    if (m_activeRegionsEnabled && m_worldSystem)
        m_worldSystem->sync_water_authority();
    if (!m_worldSystem)
        return {};
    const auto live = m_worldSystem->snapshot_streamed_chunks();
    if (!m_activeRegionsEnabled) {
        WorldStreamingState state;
        for (const auto& chunk : live)
            state.insert_chunk(chunk);
        return Persistence::ComputeWorldStreamingStateHash(
            state, m_worldSystem->GetWaterSimWindowCursor());
    }
    const auto parked = m_worldSystem->snapshot_parked_chunks();
    const auto durable = m_worldSystem->durable_chunk_inventory();
    std::set<RegionKey> keys;
    const auto add_key = [&keys](const IVec3& coords) {
        int rx = 0, rz = 0;
        Persistence::WorldSaveService::region_coords_for_chunk(coords, rx, rz);
        keys.insert({rx, rz});
    };
    for (const auto& chunk : live)
        if (chunk && chunk->needs_authority() && chunk->get_state() != ChunkState::Loading)
            add_key(chunk->get_coords());
    for (const auto& chunk : parked)
        if (chunk && chunk->needs_authority())
            add_key(chunk->get_coords());
    for (const auto& coords : durable)
        add_key(coords);
    const auto directory =
        m_regionDurableDirectory.empty() ? GetWorldSaveDir() : m_regionDurableDirectory;
    std::string projection =
        "authoritative_chunks:v2:" + std::to_string(m_worldSystem->GetWaterSimWindowCursor()) + ":";
    try {
        if (!durable.empty() && !Persistence::snapshot_index_exists(directory))
            throw std::runtime_error("inconsistent snapshot: index missing");
        for (const auto key : keys) {
            const auto record = m_activeRegionLedger.records().find(key);
            const auto digest = record != m_activeRegionLedger.records().end() &&
                                        record->second.state == RegionState::Frozen
                                    ? record->second.frozen_digest
                                    : Persistence::WorldSaveService::region_simulation_digest(
                                          directory, key, {live, parked, durable});
            projection += std::to_string(key.x) + "," + std::to_string(key.z) + ":" +
                          std::to_string(digest) + ";";
        }
    } catch (const std::exception& error) {
        LatchRegionFault(m_worldClock.tick(), error.what());
    }
    return Persistence::StableChecksum(projection);
}

std::string GameSession::FoldClockIntoEcologyHash(const std::string& ecology_hash) const {
    return m_activeRegionsEnabled
               ? Persistence::StableChecksum(ecology_hash + m_worldClock.canonical_bytes() +
                                             m_activeRegionLedger.canonical_bytes())
               : ecology_hash;
}

void GameSession::RestoreWorldClock(const WorldClock& clock) {
    m_worldClock = clock;
    m_simulationClock.reset(clock.tick());
    if (m_energyFieldState)
        m_energyFieldState->ResetEmptyAtTick(clock.tick());
    m_weatherSystem->RestoreAtTick(clock.tick(), m_ambientFieldAnchor, *m_windFieldSystem);
    m_aetherFieldSystem->Update(clock.tick(), m_ambientFieldAnchor, m_windFieldSystem.get());
}

namespace {

// One flag-on save is one snapshot transaction (S4): recovery, one validation, then every indexed
// member writer joins this transaction and the index is published once at Commit. Owner thread
// only.
class OuterSaveTransaction {
public:
    OuterSaveTransaction(const fs::path& save_dir,
                         std::uint64_t tick,
                         std::vector<std::array<float, 3>> anchors)
        : m_facts(save_dir)
        , m_transaction(
              save_dir, tick, std::move(anchors), &Persistence::WorldSaveService::member_digest)
        , m_save_dir(save_dir) {}

    // Recovers an interrupted save, validates the directory once, then opens the journal.
    bool Start() {
        std::vector<std::string> errors;
        if (Persistence::recover_pending_journal(m_save_dir, &errors) ==
            Persistence::RecoveryOutcome::Failed) {
            Log(errors);
            return false;
        }
        if (!m_facts.validate(&errors)) {
            Log(errors);
            return false;
        }
        if (!m_transaction.begin(&errors)) {
            Log(errors);
            return false;
        }
        return true;
    }

    bool Commit() {
        std::vector<std::string> errors;
        if (!m_transaction.commit(&errors)) {
            Log(errors);
            return false;
        }
        m_facts.run_deferred();
        return true;
    }

private:
    static void Log(const std::vector<std::string>& errors) {
        for (const std::string& error : errors)
            LUMINUMBRA_CORE_ERROR("World save transaction failed: {}", error);
    }

    // Declared in this order so the transaction (which rolls back) is destroyed before the facts.
    Persistence::WorldSaveService::SaveFactsScope m_facts;
    Persistence::SaveTransaction m_transaction;
    fs::path m_save_dir;
};

std::unique_ptr<OuterSaveTransaction>
StartOuterSave(const fs::path& save_dir, std::uint64_t tick, const std::optional<Vec3>& anchor) {
    std::vector<std::array<float, 3>> anchors;
    if (anchor)
        anchors.push_back({anchor->x, anchor->y, anchor->z});
    auto outer = std::make_unique<OuterSaveTransaction>(save_dir, tick, std::move(anchors));
    if (!outer->Start())
        return nullptr;
    return outer;
}

} // namespace

bool GameSession::SaveWorld() {
    if (m_transientWorld || m_metadata.worldId.empty())
        return false;
    return SaveWorldStateToImpl(GetWorldSaveDir(), nullptr, false, true);
}

bool GameSession::ReconcileWorldStateMetadataTo(const fs::path& save_dir) {
    if (m_activeRegionsEnabled || m_renderCalendar.has_value())
        return SaveWorldMetadataTo(save_dir);
    std::string bytes;
    if (!PrepareWorldMetadataTo(save_dir, false, bytes))
        return false;
    if (bytes.empty())
        return true;
    // This session operation may create a new metadata-only destination.
    std::error_code directory_error;
    fs::create_directories(save_dir, directory_error);
    return !directory_error && Persistence::WorldSaveService::save_metadata(bytes, save_dir);
}

bool GameSession::SavedPresetCompatible(const fs::path& save_dir) const {
    const auto& canonical = m_savedPresetJson;
    if (!Persistence::SavedInputLimits::JsonDepthAllowed(canonical))
        return false;
    try {
        const auto preset_path = save_dir / "preset.json";
        nlohmann::json preset;
        bool preset_missing = false;
        if (!Persistence::ReadSavedJson(preset_path,
                                        Persistence::SavedInputLimits::kPresetBytes,
                                        preset,
                                        preset_missing,
                                        false))
            return false;
        const bool has_preset = !preset_missing;
        const auto metadata_path = save_dir / "world_info.json";
        nlohmann::json metadata;
        bool missing = false;
        if (!Persistence::ReadSavedJson(
                metadata_path, Persistence::SavedInputLimits::kMetadataBytes, metadata, missing) ||
            (!missing && !Persistence::AdmitMetadata(metadata)))
            return false;
        if (!missing) {
            if (!metadata.is_object())
                return false;
            if (metadata.contains("presetFormat") &&
                (!metadata.at("presetFormat").is_number_integer() ||
                 metadata.at("presetFormat") != Persistence::kSavedPresetFormat || !has_preset ||
                 !metadata.contains("presetIdentity")))
                return false;
            if (metadata.contains("presetIdentity") &&
                (canonical.empty() || !metadata.at("presetIdentity").is_string() ||
                 metadata.at("presetIdentity").get<std::string>() !=
                     Persistence::StableChecksum(canonical)))
                return false;
        }
        if (has_preset)
            return !canonical.empty() && preset.dump() == canonical;
        // A new preset needs one byte for its write-time newline. Existing
        // admitted presets above take the comparison path and need no replacement.
        return canonical.size() < Persistence::SavedInputLimits::kPresetBytes;
    } catch (const std::exception&) {
        return false;
    }
}

namespace {
bool PersistSavedPreset(const fs::path& save_dir,
                        const std::string& canonical,
                        Persistence::CreatedWorldArtifacts* artifacts = nullptr) {
    if (canonical.empty())
        return false;
    try {
        const auto path = save_dir / "preset.json";
        nlohmann::json preset;
        bool missing = false;
        if (!Persistence::ReadSavedJson(
                path, Persistence::SavedInputLimits::kPresetBytes, preset, missing, false))
            return false;
        if (!missing)
            return preset.dump() == canonical;
        if (canonical.size() >= Persistence::SavedInputLimits::kPresetBytes ||
            !Persistence::SavedInputLimits::JsonDepthAllowed(canonical))
            return false;
        return Persistence::WorldSaveService::write_file_durably(
            path, canonical + "\n", nullptr, artifacts);
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace

bool GameSession::SaveWorldMetadataTo(const fs::path& save_dir,
                                      Persistence::CreatedWorldArtifacts* artifacts) const {
    if (!IsSimulationTickBoundary() || save_dir.empty() ||
        !ClockConfigurationCompatible(save_dir) ||
        ((m_transientWorld || m_metadata.worldId.empty()) &&
         (m_activeRegionsEnabled || !m_renderCalendar)) ||
        !m_worldUsable || !SavedPresetCompatible(save_dir))
        return false;
    std::string bytes;
    if (!PrepareWorldMetadataTo(save_dir, true, bytes))
        return false;
    if (!Persistence::WorldSaveService::validate_save(save_dir))
        return false;
    std::error_code directory_error;
    fs::create_directories(save_dir, directory_error);
    if (directory_error)
        return false;
    if (!PersistSavedPreset(save_dir, m_savedPresetJson, artifacts))
        return false;
    if (m_activeRegionsEnabled) {
        auto ledger = m_activeRegionLedger;
        return Persistence::WorldSaveService::save_metadata_and_active_regions(
            bytes, ledger, save_dir, nullptr, artifacts);
    }
    return Persistence::WorldSaveService::save_metadata(bytes, save_dir, nullptr, artifacts);
}

bool GameSession::PrepareWorldMetadataTo(const fs::path& save_dir,
                                         bool full_metadata,
                                         std::string& bytes) const {
    if (!full_metadata && !m_activeRegionsEnabled && !m_renderCalendar) {
        nlohmann::json metadata;
        bool missing = false;
        if (!Persistence::ReadSavedJson(save_dir / "world_info.json",
                                        Persistence::SavedInputLimits::kMetadataBytes,
                                        metadata,
                                        missing) ||
            (!missing && (!metadata.is_object() || !Persistence::AdmitMetadata(metadata))))
            return false;
        const auto cursor = m_worldSystem->GetWaterSimWindowCursor();
        if (missing && cursor == 0) {
            bytes.clear();
            return true;
        }
        if (missing)
            metadata = {{"container_version", Persistence::WorldSaveService::kContainerVersion}};
        metadata.erase("renderCalendar");
        metadata["waterSimCursor"] = cursor;
        if (!Persistence::BoundedJsonDump(
                metadata, Persistence::SavedInputLimits::kMetadataBytes, bytes, 4) ||
            !Persistence::WorldSaveService::admit_metadata_bytes(bytes)) {
            LUMINUMBRA_CORE_ERROR(
                "Saved-input admission refused: metadata exceeds 2 MiB compatibility limit.");
            return false;
        }
        return true;
    }
    // Using nlohmann::json for robust saving
    nlohmann::json metadata_json = {
        {"container_version", Persistence::WorldSaveService::kContainerVersion},
        {"presetFormat", Persistence::kSavedPresetFormat},
        {"presetIdentity", Persistence::StableChecksum(m_savedPresetJson)},
        {"name", m_metadata.name},
        {"seed", m_metadata.seed},
        {"worldType", m_metadata.worldType},
        {"creationTime", m_metadata.creationTime},
        {"spawnPoint",
         {{"x", m_metadata.spawnPoint.x},
          {"y", m_metadata.spawnPoint.y},
          {"z", m_metadata.spawnPoint.z}}},
        // the rotating water sim-window cursor is evolution-relevant sim
        // state (which 64-chunk window sims first changes subsequent depths when more
        // chunks are awake than the per-tick cap). Persist it so a loaded session
        // resimulates the exact windows the original would. Saves that predate this
        // field load as 0 (the fresh-boot value).
        {"waterSimCursor",
         m_worldSystem ? m_worldSystem->GetWaterSimWindowCursor() : std::size_t{0}}};

    if (!m_activeRegionsEnabled && m_renderCalendar)
        metadata_json["renderCalendar"] = WorldClock(0, *m_renderCalendar).calendar_metadata();
    if (m_activeRegionsEnabled) {
        m_worldClock.write_metadata(metadata_json);
        metadata_json["ambientFieldAnchor"] = {{"x", m_ambientFieldAnchor.x},
                                               {"y", m_ambientFieldAnchor.y},
                                               {"z", m_ambientFieldAnchor.z}};
        // Marks a directory that must carry a snapshot index (downgrade guard, S4). Written only
        // inside a snapshot transaction, so the marker and the index always appear together.
        metadata_json["snapshotFormat"] = 1;
    }
    if (!Persistence::AdmitMetadata(metadata_json) ||
        !Persistence::BoundedJsonDump(
            metadata_json, Persistence::SavedInputLimits::kMetadataBytes, bytes, 4)) {
        LUMINUMBRA_CORE_ERROR("Saved-input admission refused: metadata compatibility limit.");
        return false;
    }
    return Persistence::WorldSaveService::admit_metadata_bytes(bytes);
}

std::filesystem::path GameSession::GetSaveRootPath() const {
    return m_saveRootPath.empty() ? RuntimeRoot(m_rootPath) : m_saveRootPath;
}

std::filesystem::path GameSession::GetWorldSaveDir() const {
    if (m_transientWorld || m_metadata.worldId.empty()) {
        return {};
    }
    return GetSaveRootPath() / "worlds/saves" / m_metadata.worldId;
}

bool GameSession::SaveWorldState(WorldStateSaveReport* report) {
    if (report) {
        *report = {};
    }
    const fs::path save_dir = GetWorldSaveDir();
    if (save_dir.empty()) {
        // No active world session; nothing to persist.
        return false;
    }
    return SaveWorldStateTo(save_dir, report);
}

bool GameSession::SaveWorldStateTo(const std::filesystem::path& save_dir,
                                   WorldStateSaveReport* report,
                                   bool full_snapshot) {
    return SaveWorldStateToImpl(
        save_dir, report, full_snapshot, m_activeRegionsEnabled || full_snapshot);
}

bool GameSession::SaveWorldStateToImpl(const std::filesystem::path& save_dir,
                                       WorldStateSaveReport* report,
                                       bool full_snapshot,
                                       bool write_metadata) {
    const auto save_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&save_started]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         save_started)
            .count();
    };
    const bool canonical_save = save_dir == GetWorldSaveDir();
    full_snapshot =
        full_snapshot || (m_activeRegionsEnabled && !m_transientWorld && !canonical_save);
    WorldStateSaveReport result;
    if (report) {
        *report = result;
    }
    if (!IsSimulationTickBoundary() || !m_worldUsable || !m_worldSystem || save_dir.empty() ||
        (m_activeRegionsEnabled && (m_transientWorld || m_metadata.worldId.empty()))) {
        return false;
    }

    struct SaveOutcome {
        GameSession& session;
        const std::chrono::steady_clock::time_point started;
        bool accepted = false;
        ~SaveOutcome() {
            if (!accepted)
                session.RecordPersistenceSaveOutcome(false,
                                                     std::chrono::duration<double, std::milli>(
                                                         std::chrono::steady_clock::now() - started)
                                                         .count());
        }
    } outcome{*this, save_started, false};

    // Quiesce in-flight generation/promotion/meshing so chunk data is stable
    // on disk — WITHOUT publishing: a save must never be an
    // activation event. Generated chunks are already live (generation writes
    // live fields); only unpublished mesh/promotion staging stays out, which
    // the next legitimate publish point (the barrier today, the activation
    // queue after activation queue) delivers on its own schedule. On the per-tick-
    // quiesced server paths everything is already drained and published here,
    // so the saved bytes are identical to the old publishing barrier's.
    m_worldSystem->quiesce_streaming_jobs_for_save();

    if (m_activeRegionsEnabled) {
        // After the quiesce: a finishing generation worker can no longer erase the obligation.
        m_worldSystem->sync_water_authority();
    }
    WorldStreamingState state;
    for (const auto& chunk : m_activeRegionsEnabled ? m_worldSystem->snapshot_authoritative_chunks()
                                                    : m_worldSystem->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }
    result.chunks_total = state.size();

    //  project the live plant roster ONCE and persist it as a sibling file in
    // BOTH the dirty-empty and the normal save path (plants are independent of chunk dirtiness). An
    // empty roster writes NO file, so a no-plant save stays byte-identical (the save-less path
    // holds).
    const auto plant_snapshot = luminumbra::foliage::BuildPlantEntitySnapshot(m_registry);
    if (!luminumbra::foliage::ValidatePlantEntitySnapshot(plant_snapshot)) {
        LUMINUMBRA_CORE_ERROR("Saved-input admission refused: invalid plant component value.");
        return false;
    }

    const auto acknowledge =
        Persistence::WorldSaveService::chunk_save_acknowledgement(state.snapshot_chunks());
    const auto defer_success = [this, &save_dir, &state, &elapsed_ms]() {
        if (!m_activeRegionsEnabled)
            return;
        std::vector<IVec3> covered;
        for (const auto& chunk : state.snapshot_chunks())
            if (chunk && chunk->needs_authority())
                covered.push_back(chunk->get_coords());
        const auto completed_ms = elapsed_ms();
        auto committed =
            [this, directory = save_dir, covered = std::move(covered), completed_ms]() {
                m_regionDurableDirectory = directory;
                m_worldSystem->remember_durable_chunks(covered);
                m_worldSystem->release_clean_parked_chunks();
                RecordPersistenceSaveOutcome(true, completed_ms);
            };
        if (auto* transaction = Persistence::SaveTransaction::active_for(save_dir))
            transaction->defer_until_commit(std::move(committed));
    };
    const std::vector<ChunkID> dirty_ids = state.dirty_chunk_ids();
    result.chunks_dirty = dirty_ids.size();
    if (report)
        *report = result;

    // Jobs can need persistence ownership themselves. Quiesce and capture first,
    // then retain the shared recursive ownership from destination validation
    // through every payload, metadata publication and dirty acknowledgement.
    const Persistence::WorldSaveService::SaveOwnership save_ownership;
    Persistence::WorldSaveService::SaveFactsScope save_facts(save_dir);
    if (!ClockConfigurationCompatible(save_dir) ||
        !Persistence::WorldSaveService::validate_save(save_dir) || !SavedPresetCompatible(save_dir))
        return false;

    std::vector<std::string> errors;
    const bool write_chunks = full_snapshot || !dirty_ids.empty();
    // A first chunk snapshot includes all residents. Later incremental saves
    // replace only dirty regions, even when unrelated residents exceed writer caps.
    const bool has_snapshot =
        !write_chunks || full_snapshot
            ? false
            : (m_activeRegionsEnabled ? Persistence::WorldSaveService::has_chunk_snapshot(save_dir)
                                      : Persistence::WorldSaveService::has_world_save(save_dir));
    Persistence::WorldSaveService::PreparedWorldWrite prepared_chunks;
    std::string metadata_bytes;
    Persistence::WorldSaveService::PreparedPlantWrite prepared_plants;
    if (!PrepareWorldMetadataTo(save_dir, write_metadata && !m_transientWorld, metadata_bytes) ||
        !prepared_plants.prepare(plant_snapshot, &errors) ||
        (write_chunks &&
         !prepared_chunks.prepare(state, save_dir, !full_snapshot && has_snapshot, &errors))) {
        for (const auto& error : errors)
            LUMINUMBRA_CORE_ERROR("World state admission refused: {}", error);
        return false;
    }
    std::optional<luminumbra::fields::EnergyFieldState> prepared_energy;
    // Serialize once into the energy writer's named record-byte bound.
    std::string energy_bytes;
    bool remove_energy = false;
    if (m_energyFieldState) {
        try {
            auto energy_snapshot = *m_energyFieldState;
            const auto tick = GetSimulationTickCount();
            if (energy_snapshot.page_count() == 0) {
                const auto absent_remaining =
                    m_activeRegionsEnabled ? luminumbra::fields::kEnergyCadenceTicks -
                                                 tick % luminumbra::fields::kEnergyCadenceTicks
                                           : luminumbra::fields::kEnergyCadenceTicks;
                const auto next_fire = energy_snapshot.next_fire_tick();
                // Absence reconstructs this phase. A legacy import can retain
                // another phase, which still needs its header-only record.
                remove_energy = next_fire > tick && next_fire - tick == absent_remaining;
            }
            energy_bytes = energy_snapshot.SerializeRecord(tick);
            prepared_energy = std::move(energy_snapshot);
        } catch (const std::length_error& error) {
            LUMINUMBRA_CORE_ERROR("{}", error.what());
            return false;
        }
    }
    std::unique_ptr<OuterSaveTransaction> outer;
    if (m_activeRegionsEnabled && !Persistence::SaveTransaction::active_for(save_dir)) {
        outer = StartOuterSave(save_dir, m_worldClock.tick(), m_activeRegionLedger.local_anchor());
        if (!outer)
            return false;
    }

    std::error_code source_path_error, target_path_error;
    const auto source_directory = fs::weakly_canonical(m_regionDurableDirectory, source_path_error);
    const auto target_directory = fs::weakly_canonical(save_dir, target_path_error);
    if (m_activeRegionsEnabled && !m_regionDurableDirectory.empty() &&
        (source_path_error || target_path_error)) {
        return false;
    }
    if (m_activeRegionsEnabled && !m_regionDurableDirectory.empty() &&
        source_directory != target_directory) {
        // Transfer each source region once. Resident/parked truth supersedes disk;
        // memory and decode work are bounded by one admitted region at a time.
        Persistence::WorldSaveService copier;
        std::vector<std::string> copy_errors;
        std::map<std::pair<int, int>, std::unordered_set<ChunkID>> disk_regions;
        for (const auto& coords : m_worldSystem->durable_chunk_inventory()) {
            const auto id = Chunk::calculate_id(coords);
            if (state.find_chunk(id))
                continue;
            int rx = 0, rz = 0;
            Persistence::WorldSaveService::region_coords_for_chunk(coords, rx, rz);
            disk_regions[{rx, rz}].insert(id);
        }
        bool copied = true;
        for (const auto& [region, expected] : disk_regions) {
            std::vector<std::shared_ptr<Chunk>> chunks;
            const auto path = Persistence::WorldSaveService::region_file_path(
                m_regionDurableDirectory, region.first, region.second);
            if (!Persistence::WorldSaveService::read_region_chunks(path, chunks, &copy_errors)) {
                copied = false;
                break;
            }
            WorldStreamingState transferred;
            for (const auto& chunk : chunks)
                if (expected.contains(chunk->get_id()))
                    transferred.insert_chunk(chunk);
            if (transferred.size() != expected.size()) {
                copy_errors.push_back("expected durable chunk missing during save-as");
                copied = false;
                break;
            }
            if (!copier.save_world(transferred, save_dir, &copy_errors)) {
                copied = false;
                break;
            }
        }
        if (!copied || !copier.save_world(state, save_dir, &copy_errors)) {
            for (const auto& error : copy_errors)
                LUMINUMBRA_CORE_ERROR("World save-as failed: {}", error);
            return false;
        }
    }

    if (m_activeRegionsEnabled && !m_regionDurableDirectory.empty() &&
        source_directory != target_directory && write_chunks &&
        !prepared_chunks.prepare(state, save_dir, false, &errors))
        return false;
    bool ok = true;
    if (write_chunks)
        ok = prepared_chunks.publish(&errors);
    // A region replacement can succeed before a later writer fails. Invalidate
    // derived authority after every chunk attempt, including partial failures,
    // so an older worker result cannot integrate against newly persisted chunks.
    if (write_chunks) {
        for (const ChunkID id : dirty_ids) {
            if (const auto chunk = state.find_chunk(id))
                m_worldSystem->notify_far_lod_authority_durable(chunk->get_coords());
        }
    }
    if (ok)
        ok = prepared_plants.publish(save_dir, &errors);
    if (ok && prepared_energy.has_value()) {
        const auto energy_path = save_dir / "aether_state.efs";
        if (m_activeRegionsEnabled) {
            ok = remove_energy ? Persistence::WorldSaveService::remove_member(
                                     save_dir, "aether_state.efs", &errors)
                               : Persistence::WorldSaveService::write_member_bytes(
                                     save_dir, "aether_state.efs", energy_bytes, &errors);
        } else {
            ok = remove_energy
                     ? Persistence::WorldSaveService::remove_file_and_staging(energy_path)
                     : Persistence::WorldSaveService::write_file_durably(energy_path, energy_bytes);
        }
        if (ok && !remove_energy) {
            if (auto* transaction = Persistence::SaveTransaction::active_for(save_dir))
                transaction->defer_until_commit([this, energy = *prepared_energy]() mutable {
                    *m_energyFieldState = std::move(energy);
                });
            else
                *m_energyFieldState = std::move(*prepared_energy);
        }
    }
    // A newly embedded preset is outside the snapshot member format. Retain
    // its creation identity until the enclosing transaction commits, so a later
    // refusal removes only this call's file and never a substituted occupant.
    std::shared_ptr<Persistence::CreatedWorldArtifacts> preset_artifacts;
    if (ok && m_activeRegionsEnabled && write_metadata && !m_transientWorld) {
        preset_artifacts = std::shared_ptr<Persistence::CreatedWorldArtifacts>(
            new Persistence::CreatedWorldArtifacts,
            [](Persistence::CreatedWorldArtifacts* artifacts) {
                const auto error = artifacts->rollback();
                if (!error.empty())
                    LUMINUMBRA_CORE_ERROR("{}", error);
                delete artifacts;
            });
        try {
            preset_artifacts->bind_existing_root(save_dir);
            ok = PersistSavedPreset(save_dir, m_savedPresetJson, preset_artifacts.get());
        } catch (const std::exception& error) {
            LUMINUMBRA_CORE_ERROR("{}", error.what());
            ok = false;
        }
        if (ok) {
            if (auto* transaction = Persistence::SaveTransaction::active_for(save_dir))
                transaction->defer_until_commit(
                    [artifacts = preset_artifacts] { artifacts->release(); });
            else
                preset_artifacts->release();
        }
    }
    // Publish complete-save metadata and reconcile presentation calendars only
    // after every payload succeeds, before acknowledging any dirty chunks.
    write_metadata = write_metadata && !m_transientWorld;
    if (ok)
        ok = write_metadata ? SaveWorldMetadataTo(save_dir)
                            : ReconcileWorldStateMetadataTo(save_dir);
    for (const auto& error : errors)
        LUMINUMBRA_CORE_ERROR("World state save failed: {}", error);
    if (!ok)
        return false;

    if (m_transientWorld || canonical_save) {
        if (auto* transaction = Persistence::SaveTransaction::active_for(save_dir))
            transaction->defer_until_commit(acknowledge);
        else
            acknowledge();
    }
    if (m_activeRegionsEnabled) {
        const auto expected_ledger = m_activeRegionLedger.canonical_bytes();
        auto saved_ledger = m_activeRegionLedger;
        saved_ledger.mark_saved(m_worldClock);
        if (auto* transaction = Persistence::SaveTransaction::active_for(save_dir))
            transaction->defer_until_commit(
                [this, expected_ledger, saved = std::move(saved_ledger)]() mutable {
                    if (m_activeRegionLedger.canonical_bytes() == expected_ledger)
                        m_activeRegionLedger = std::move(saved);
                });
        defer_success();
    }
    if (outer && !outer->Commit()) {
        return false;
    }
    result.saved = write_chunks;
    result.chunks_saved = write_chunks ? result.chunks_total : 0;
    if (report)
        *report = result;
    outcome.accepted = true;
    return true;
}

bool GameSession::LoadWorldState() {
    if (m_transientWorld)
        return m_worldSystem && m_worldUsable;
    return LoadWorldStateFrom(GetWorldSaveDir());
}

bool GameSession::LoadWorldStateFrom(const std::filesystem::path& save_dir) {
    if (!IsSimulationTickBoundary())
        return false;
    if (!m_worldUsable || !m_worldSystem || save_dir.empty()) {
        return false;
    }

    LoadedWorldState state;
    if (!ReadWorldStateFrom(save_dir, state)) {
        if (!state.input_refused) {
            ++m_worldRevision;
            m_lastLoadedChunkCount = 0;
            m_worldUsable = false;
            // Refused snapshots invalidate resident chunks, but never publish staged members.
            m_worldSystem->clear_world(m_physicsSystem.get());
        }
        return false;
    }
    AdoptWorldStateFrom(save_dir, state);
    return true;
}

bool GameSession::ReadWorldStateFrom(const fs::path& save_dir,
                                     LoadedWorldState& state,
                                     bool resetting_clock) {
    const Persistence::WorldSaveService::SaveOwnership ownership;
    std::vector<std::string> recovery_errors;
    if (!Persistence::SaveTransaction::active_for(save_dir) &&
        Persistence::recover_pending_journal(save_dir, &recovery_errors) ==
            Persistence::RecoveryOutcome::Failed) {
        state.input_refused = true;
        m_worldOpenError =
            recovery_errors.empty() ? "Saved-world recovery failed." : recovery_errors.front();
        return false;
    }
    return ReadPreparedWorldState(save_dir,
                                  CaptureWorldPreparationOptions(),
                                  state,
                                  m_worldOpenError,
                                  resetting_clock,
                                  {},
                                  resetting_clock ? nullptr : m_energyFieldState.get());
}

bool GameSession::ReadPreparedWorldState(
    const fs::path& save_dir,
    const WorldPreparationOptions& options,
    LoadedWorldState& state,
    std::string& error,
    bool resetting_clock,
    const std::stop_token& stop,
    const luminumbra::fields::EnergyFieldState* current_energy) {
    const Persistence::WorldSaveService service;
    const auto cancelled = [&] {
        if (!stop.stop_requested())
            return false;
        error = "Saved-world validation cancelled.";
        return true;
    };
    if (cancelled())
        return false;
    std::vector<std::string> errors;
    if (Persistence::journal_pending(save_dir)) {
        const Persistence::WorldSaveService::SaveOwnership ownership;
        if (Persistence::recover_pending_journal(save_dir, &errors) ==
            Persistence::RecoveryOutcome::Failed) {
            state.input_refused = true;
            error = errors.empty() ? "Saved-world recovery failed." : errors.front();
            return false;
        }
    }
    if (!Persistence::WorldSaveService::load_plant_entities(
            state.plants, save_dir, &errors, stop) ||
        !luminumbra::foliage::ValidatePlantEntitySnapshot(state.plants)) {
        state.input_refused = true;
        error = errors.empty() ? "Saved-input admission refused: invalid plant snapshot."
                               : errors.front();
        return false;
    }
    const std::unordered_set<ChunkID> retain_none;
    state.snapshot_present =
        service.load_world(state.chunks,
                           save_dir,
                           errors,
                           stop,
                           options.active_regions ? &retain_none : nullptr,
                           options.active_regions ? &state.durable_inventory : nullptr);
    if (!state.snapshot_present && !errors.empty()) {
        error = errors.front();
        state.input_refused = error.find("Saved-input admission refused:") == 0;
        for (const std::string& diagnostic : errors)
            LUMINUMBRA_CORE_ERROR("World state load failed: {}", diagnostic);
        return false;
    }

    if (cancelled())
        return false;
    bool requires_active_regions = false;
    if (!Persistence::WorldSaveService::read_clock_metadata(save_dir,
                                                            state.clock,
                                                            requires_active_regions,
                                                            &errors,
                                                            &state.ambient_anchor,
                                                            &state.render_calendar,
                                                            &state.water_sim_cursor,
                                                            stop) ||
        (requires_active_regions && !options.active_regions)) {
        error = errors.empty()
                    ? "Incompatible configuration: this world requires sim.active_regions."
                    : errors.front();
        state.input_refused = error.find("Saved-input admission refused:") == 0;
        return false;
    }
    if (options.active_regions) {
        // Presentation-only saves recorded the calendar but no simulation tick.
        if (state.render_calendar)
            state.clock = WorldClock(0, *state.render_calendar);
        state.ledger.emplace(options.ledger_config);
        if (!Persistence::WorldSaveService::load_active_regions(
                *state.ledger, save_dir, &errors, state.clock, stop)) {
            error =
                errors.empty() ? std::string(ActiveRegionLedger::kCorruptMessage) : errors.front();
            return false;
        }
        if (options.scheduler_override && state.ledger->config() != *options.scheduler_override) {
            error = "Incompatible configuration: persisted region scheduler differs "
                    "from explicit override.";
            return false;
        }
    }

    if (options.aether_state) {
        const Persistence::SavedInputFile input(save_dir / "aether_state.efs");
        const auto tick_base = options.active_regions
                                   ? state.clock.tick()
                                   : (resetting_clock ? std::uint64_t{0} : options.simulation_tick);
        // Only the synchronous snapshot wrapper supplies existing energy. Worker
        // preparation passes nullptr and never reads a live session object.
        if (current_energy)
            state.energy.emplace(*current_energy);
        else
            state.energy.emplace(2);
        if (input.missing()) {
            state.energy->ResetEmptyAtTick(tick_base);
        } else {
            std::string record;
            if (!Persistence::ReadSavedEnergy(input, record, stop) ||
                !state.energy->DeserializeRecord(record, tick_base)) {
                state.input_refused = true;
                error =
                    "Saved-input admission refused: malformed energy record or compatibility limit "
                    "(16 channels, 8192 pages, 360218644 bytes).";
                return false;
            }
        }
    }

    // Validate every chunk before adopting any state. A malformed lattice must
    // never become an invitation to regenerate over authoritative disk bytes.
    constexpr std::size_t kFullSdfLattice =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    for (const auto& chunk : state.chunks.snapshot_chunks()) {
        if (cancelled())
            return false;
        if (chunk && !chunk->sdf_data.empty() && chunk->sdf_data.size() != kFullSdfLattice) {
            error = "corrupt world chunk SDF lattice";
            LUMINUMBRA_CORE_ERROR("World open refused: {}", error);
            return false;
        }
    }
    return !cancelled();
}

void GameSession::AdoptWorldStateFrom(const fs::path& save_dir, LoadedWorldState& saved) {
    // All refusing readers have passed; publish the staged state together.
    m_lastLoadedChunkCount = 0;
    m_renderCalendar = saved.render_calendar;
    if (saved.ledger) {
        m_ambientFieldAnchor = saved.ambient_anchor.value_or(m_metadata.spawnPoint);
        // Interrupted pairs may have an older ledger. Rebase without scheduling.
        saved.ledger->restore_clock(saved.clock);
        ReplaceActiveRegionLedger(std::move(*saved.ledger));
        if (!m_activeRegionLedger.local_anchor())
            m_activeRegionLedger.try_set_local_anchor(m_metadata.spawnPoint);
        m_regionDurableDirectory = save_dir;
        m_worldSystem->remember_durable_chunks(saved.durable_inventory);
        m_regionSchedule = {};
        m_regionWork.clear();
        m_regionFault.reset();
        m_replicatedSimulationAnchors.reset();
        RestoreWorldClock(saved.clock);
    }
    if (m_energyFieldState && saved.energy)
        *m_energyFieldState = std::move(*saved.energy);
    if (!saved.snapshot_present && !m_activeRegionsEnabled) {
        m_worldSystem->SetWaterSimWindowCursor(saved.water_sim_cursor);
        return; // legacy fresh world
    }
    auto& loaded = saved.chunks;
    std::size_t adopted = 0;
    for (const auto& chunk : loaded.snapshot_chunks()) {
        if (!chunk) {
            continue;
        }

        // Runtime-only flags do not survive process boundaries: physics
        // colliders and in-flight mesh jobs from the saving process do not
        // exist here, so normalize them before the streaming systems see the
        // chunk. Voxel data (sdf/heightmap) and meshes are kept verbatim.
        chunk->has_collision.store(false, std::memory_order_release);
        chunk->pending_mesh_ready.store(false, std::memory_order_release);
        chunk->pending_mesh_failed.store(false, std::memory_order_release);
        chunk->pending_lod.store(-1, std::memory_order_release);
        chunk->pending_mesh_vertices.clear();
        chunk->pending_mesh_indices.clear();
        chunk->pending_water_mesh_vertices.clear();
        chunk->pending_water_mesh_indices.clear();

        // Chunks saved mid-transition (Loading/Meshing/Unloading) settle to a
        // stable state; Ready and Idle are restored verbatim (a Ready chunk
        // with an empty mesh is a legitimate air chunk).
        const bool has_mesh = !chunk->mesh_vertices.empty() && !chunk->mesh_indices.empty();
        const ChunkState state = chunk->get_state();
        if (state != ChunkState::Ready && state != ChunkState::Idle) {
            chunk->set_state(has_mesh ? ChunkState::Ready : ChunkState::Idle);
        }

        if (m_worldSystem->adopt_streamed_chunk(chunk)) {
            ++adopted;
        }
    }

    if (m_activeRegionsEnabled) {
        for (const auto& coords : m_worldSystem->GetInitialChunkLoadList(m_metadata.spawnPoint)) {
            if (m_worldSystem->restore_authoritative_chunk(coords))
                ++adopted;
        }
    }

    // boot-time water-resolution migration: a save written at another
    // sim resolution (sim.water_high_res flipped between sessions) must converge
    // BEFORE the first live tick. Left to the live path it would resize at
    // MAX_WATER_RESIZES_PER_TICK=1 while the seam pass — which hard-gates on equal
    // resolution — walls off water at every mixed seam for thousands of ticks. One
    // uncapped pass here (the boot-settle cap-lifting idea applied to resizes) is
    // deterministic: each chunk's integer-bilinear mm resample is a pure function of
    // its loaded state + worldgen + the session resolution. Matching saves resize
    // nothing — the default path stays byte-identical.
    {
        const std::size_t migrated = m_worldSystem->MigrateWaterSimResolution();
        if (migrated > 0) {
            LUMINUMBRA_CORE_INFO(
                "Water-resolution migration: {} loaded chunks resized to {}x{} before first tick",
                migrated,
                m_worldSystem->debug_water_sim_resolution(),
                m_worldSystem->debug_water_sim_resolution());
        }
    }

    // Plant numeric projection was admitted before any live state changed.
    luminumbra::foliage::ApplyPlantEntitySnapshot(m_registry, saved.plants);

    // Commit the validated cursor only after every refusing load step passes.
    m_worldSystem->SetWaterSimWindowCursor(saved.water_sim_cursor);
    m_lastLoadedChunkCount = adopted;
    LUMINUMBRA_CORE_INFO("World state loaded: {} chunks adopted ({} in snapshot) from {}",
                         adopted,
                         loaded.size(),
                         Filesystem::GenericUtf8PathString(save_dir));
}

std::string GameSession::GenerateWorldId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::stringstream ss;
    ss << "world_" << timestamp << "_" << dis(gen);
    return ss.str();
}

uint32_t GameSession::StringToSeed(const std::string& seedStr) const {
    if (seedStr.empty()) {
        return std::random_device{}();
    }

    try {
        // Use std::stoull for 64-bit seed range, then cast
        return static_cast<uint32_t>(std::stoull(seedStr));
    } catch (...) {
        // If not a number, hash the string
        const std::hash<std::string> hasher;
        return static_cast<uint32_t>(hasher(seedStr));
    }
}

// scent/foraging grid <-> world helpers. The field is kScentFieldCells wide, cell
// kScentCellSize, centered on the spawn anchor (ScentOriginFor). Used by the client colony spawner.
float GameSession::ScentCellSize() const {
    return kScentCellSize;
}
int GameSession::ScentFieldCells() const {
    return kScentFieldCells;
}
int GameSession::ScentWorldToCellX(float world_x) const {
    return static_cast<int>(
        std::lround((world_x - ScentOriginFor(m_metadata.spawnPoint.x)) / kScentCellSize));
}
int GameSession::ScentWorldToCellZ(float world_z) const {
    return static_cast<int>(
        std::lround((world_z - ScentOriginFor(m_metadata.spawnPoint.z)) / kScentCellSize));
}
float GameSession::ScentCellToWorldX(int cell_x) const {
    return ScentOriginFor(m_metadata.spawnPoint.x) + static_cast<float>(cell_x) * kScentCellSize;
}
float GameSession::ScentCellToWorldZ(int cell_z) const {
    return ScentOriginFor(m_metadata.spawnPoint.z) + static_cast<float>(cell_z) * kScentCellSize;
}

void GameSession::InitializeScentField(const Vec3& /*anchor*/) {
    m_scentField = std::make_unique<luminumbra::ai::ScentField>(
        kScentFieldCells, kScentFieldCells, kScentFieldChannels);
    LUMINUMBRA_CORE_INFO("Scent field initialized: {}x{} cells, {} channel(s), cell_size={}",
                         kScentFieldCells,
                         kScentFieldCells,
                         kScentFieldChannels,
                         kScentCellSize);
}

bool GameSession::HasScentParticipants() const {
    {
        auto emitters = m_registry.view<const Luminumbra::Components::TransformComponent,
                                        const Luminumbra::Components::SensableComponent>();
        for (auto e : emitters) {
            const auto& s = emitters.get<const Luminumbra::Components::SensableComponent>(e);
            if (s.scent_channel >= 0 && s.scent_deposit > 0.0f) {
                return true;
            }
        }
    }
    {
        auto sensors = m_registry.view<const Luminumbra::Components::TransformComponent,
                                       const Luminumbra::Components::ScentSenseComponent>();
        for (auto e : sensors) {
            const auto& s = sensors.get<const Luminumbra::Components::ScentSenseComponent>(e);
            if (s.channel >= 0) {
                return true;
            }
        }
    }
    return false;
}

std::string GameSession::ComputeScentSubHash() const {
    if (!m_scentField || !HasScentParticipants()) {
        return {};
    }

    std::ostringstream bytes;
    bytes << "scent:v1:" << m_scentField->width() << ':' << m_scentField->height() << ':'
          << m_scentField->channels() << ':' << std::setprecision(17);
    for (int ch = 0; ch < m_scentField->channels(); ++ch) {
        for (int z = 0; z < m_scentField->height(); ++z) {
            for (int x = 0; x < m_scentField->width(); ++x) {
                const double v = m_scentField->Sample(ch, x, z);
                if (v != 0.0) {
                    bytes << ch << ',' << x << ',' << z << '=' << v << ';';
                }
            }
        }
    }
    return Persistence::StableChecksum(bytes.str());
}

bool GameSession::HasPlantParticipants() const {
    auto plants = m_registry.view<const Luminumbra::Components::PlantTag>();
    return plants.begin() != plants.end();
}

void GameSession::InitializeEnergyFieldState() {
    if (!m_aetherStateEnabled) {
        m_energyFieldState.reset();
        m_scriptState->set_energy_field(nullptr);
        return;
    }
    //  ( -8): TWO channels from day one — channel 0 =
    // energy, channel 1 = Lumin/Umbra polarity — so the aether_state:v1:
    // sub-hash covers polarity from its first activation and never needs a v2
    // for it. Emitters address channels as data (FieldEmitterComponent.channel).
    m_energyFieldState = std::make_unique<luminumbra::fields::EnergyFieldState>(/*channels=*/2);
    m_scriptState->set_energy_field(m_energyFieldState.get());
    LUMINUMBRA_CORE_INFO("Energy field state layer initialized (sim.aether_state ON, 2 channels).");
}

std::string GameSession::ComputeAetherStateSubHash() const {
    if (!m_energyFieldState) {
        return {};
    }
    const std::string bytes = m_energyFieldState->CanonicalBytes();
    if (bytes.empty()) {
        return {}; // all-zero == absent (the scent/plant empty-neutral contract)
    }
    return Persistence::StableChecksum(bytes);
}

std::string GameSession::ComputePlantSubHash() const {
    if (!HasPlantParticipants()) {
        return {};
    }
    // Hash the id-ordered sequence of integer growth state (geometry is visual-only;
    // THIS is the sim truth). id-robust: we hash the ordered STATE, not raw ids.
    auto view = m_registry.view<const Luminumbra::Components::PlantTag,
                                const Luminumbra::Components::PlantGrowthComponent>();
    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end());
    std::ostringstream bytes;
    bytes << "plant:v1:" << ents.size() << ':';
    for (auto e : ents) {
        const auto& g = view.get<const Luminumbra::Components::PlantGrowthComponent>(e);
        bytes << g.species_id << ',' << int(g.stage) << ',' << int(g.quality) << ','
              << g.growth_points << ',' << g.stress_points << ';';
    }
    return Persistence::StableChecksum(bytes.str());
}

} // namespace Luminumbra::world
