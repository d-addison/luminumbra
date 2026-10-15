#include "GameSession.h"
#include "../ai/CircadianSystem.h" // Diurnal and nocturnal activity.
#include "../ai/CreatureBrainSystem.h"
#include "../ai/CreatureReproductionSystem.h" // Generational evolution.
#include "../ai/CreatureSpeciesRegistry.h"    // per-world species definitions
#include "../ai/DecompositionSystem.h"        // Releases nutrients after death.
#include "../ai/ForagingSystem.h"             // ant-trail foraging (Deneubourg double-bridge)
#include "../ai/HerdAlarmSystem.h"            // Collective-vigilance alarm.
#include "../ai/InstinctLocomotionSystem.h"
#include "../ai/InstinctSystem.h"
#include "../ai/LifespanSystem.h"  // Age and starvation mortality.
#include "../ai/MigrationSystem.h" // Seasonal migration drive.
#include "../ai/PerceptionSystem.h"
#include "../ai/PredatorPackSystem.h" // Pack flanking coordination.
#include "../ai/ScavengingSystem.h"   // Converts carcasses into scavenger food.
#include "../ai/ScentDepositSystem.h"
#include "../ai/ScentField.h"
#include "../ai/ScentFieldGates.h"
#include "../ai/ScentSteeringSystem.h"
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
#include "../core/JobSystem.h"
#include "../core/Log.h"
#include "../fields/EnergyFieldState.h"
#include "../persistence/PlantPersistence.h"
#include "../persistence/SaveTransaction.h"
#include "../persistence/SavedWorldCatalog.h"
#include "../persistence/WorldPersistenceRoundtrip.h" // Persistence::StableChecksum (ComputeScentSubHash)
#include "../persistence/WorldSaveService.h"
#include "../scripting/LuaState.h"
#include "../systems/AetherFieldSystem.h"
#include "../systems/CropLifecycleSystem.h"
#include "../systems/FieldEmitterSystem.h" // Gathers field-emitter deposits.
#include "../systems/FireSpreadSystem.h"   // Fire propagation.
#include "../systems/FoliageRegistryStage.h"
#include "../systems/IrrigationSystem.h" // Soil-moisture irrigation.
#include "../systems/PhysicsSystem.h"
#include "../systems/PlantCoarse.h"
#include "../systems/PlantDiseaseSystem.h" // Plant disease propagation.
#include "../systems/PlantGrowthSystem.h"  // Plant growth tick.
#include "../systems/PollinationSystem.h"  // Cross-pollination.
#include "../systems/RegionalFoliage.h"
#include "../systems/SHIELD_WorldSystem.h" // This includes TerrainGenParams
#include "../systems/SoilNutrientSystem.h" // Soil-nutrient cycling.
#include "../systems/WaterSystem.h"
#include "../systems/WeatherEventSystem.h" // Pure deterministic weather-event schedule.
#include "../systems/WeatherSystem.h"
#include "../systems/WindFieldSystem.h"
#include "TerrainPresetLoader.h"
#include "WorldStreamingState.h"
#include "nlohmann/json.hpp" // For parsing JSON
#include <array>
#include <bit>
#include <cmath> // std::floor (energy-layer anchor quantization)

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

template<typename... Components>
std::uint64_t RegionalParticipantCount(const entt::registry& registry,
                                       const Luminumbra::world::RegionEntityGate& gate) {
    const auto view = registry.view<const Components...>();
    return static_cast<std::uint64_t>(std::count_if(view.begin(), view.end(), gate));
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
    if (m_worldSystem)
        m_worldSystem->quiesce_streaming_jobs_for_save();
    // Keep all dependencies alive while the world destructor publishes completed work.
    if (m_physicsSystem)
        m_physicsSystem->set_world_system(nullptr);
    m_worldSystem.reset();
    m_physicsSystem.reset();
    m_waterSystem.reset();
    m_chunkValidationCache.clear();
    m_registry.clear();
    m_soilGrid.reset();
    m_irrigationGrid.reset();
    m_simulationClock.reset();
    m_simBudgetTelemetry.Clear();
    m_worldClock = WorldClock{};
    m_activeRegionLedger = ActiveRegionLedger(m_regionSchedulerConfig);
    m_groundObjects.clear();
    m_fieldPages.clear();
    m_regionSchedule = {};
    m_replicatedSimulationAnchors.reset();
    m_regionWork.clear();
    m_regionCompletedWork.clear();
    m_foliagePreviousTicks.clear();
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

// load the per-world species definition table at world create/load.
// DETERMINISM: CreatureSpeciesRegistry::LoadFromDirectory sorts the *.json entries by
// filename before parsing, so the table is a pure function of the files' CONTENT — never of
// filesystem iteration order. A missing/empty directory yields an EMPTY table, and every
// per-species behaviour block (IAUS "brain" overrides, "genome_ranges") defaults to the
// compiled constants. The creature brain and reproduction systems consume the resolved entry;
// absent or defaults-matching JSON remains byte-identical to compiled behaviour.
// the deterministic weather-EVENT schedule read. OFF (the default)
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
// Called from BOTH CreateWorld and LoadWorld after the systems exist. Default-OFF
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

// apply the session water-sim resolution when the owner opted in
// (sim.water_high_res -> SetWaterHighResEnabled). Called from BOTH CreateWorld and
// LoadWorld immediately after the water system is constructed and linked — before its
// first update, because the uniform hashed grid resolution can never change mid-run.
// Default-OFF never calls the setter: the solver keeps its Medium default, byte-identical.
void GameSession::ApplyWaterResolutionWiring() {
    if (!m_worldSystem || !m_waterHighResEnabled) {
        return;
    }
    m_worldSystem->SetWaterSimResolution(static_cast<int>(Systems::WaterDetailLevel::High));
    LUMINUMBRA_CORE_INFO(
        "High-resolution water WIRED (16x16 grid, 1 m cells; 16-chunk sim window)");
}

void GameSession::LoadSpeciesDefinitions() {
    m_speciesTable = std::make_unique<luminumbra::ai::CreatureSpeciesRegistry>();
    const fs::path dir = RuntimeRoot(m_rootPath) / "data" / "common" / "creatures" / "species";
    std::vector<std::string> errors;
    const std::size_t loaded = m_speciesTable->LoadFromDirectory(dir, errors);
    for (const std::string& err : errors) {
        // Content problems are warnings, not failures: a bad species file must never brick a
        // world load (the affected species just keeps its compiled-default behaviour/absence).
        LUMINUMBRA_CORE_WARN("Species definition load: {}", err);
    }
    LUMINUMBRA_CORE_INFO("Species definitions loaded: {} from {}", loaded, dir.string());
}

std::uint32_t GameSession::TickSimulation(double frame_dt) {
    if (m_regionFault)
        return 0;
    // Queued work belongs to this next tick; only reentrant batches must refuse.
    if (m_activeRegionsEnabled && m_simulationBatchInProgress)
        return 0;
    struct BatchGuard {
        bool& active;
        bool previous;
        BatchGuard(bool& value, bool enabled)
            : active(value)
            , previous(value) {
            active = enabled;
        }
        ~BatchGuard() {
            active = previous;
        }
    } guard(m_simulationBatchInProgress, m_activeRegionsEnabled);
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
        try {
            if (m_activeRegionsEnabled) {
                m_regionCompletedWork.clear();
                PrepareRegionalFoliage();
                m_worldClock.set_tick(current_tick);
                if (m_regionFault)
                    return i + 1;
                m_foliagePreviousTicks.clear();
                for (const auto& [key, record] : m_activeRegionLedger.records())
                    m_foliagePreviousTicks[key] = record.last_ticked;
                RunRegionScheduler(current_tick);
                if (m_regionFault)
                    return i + 1;
                AdvanceGroundObjects(current_tick);
                if (m_regionFault)
                    return i + 1;
                AdvanceFieldPages(current_tick);
                if (m_regionFault)
                    return i + 1;
            }

            // Deterministic per-tick system order. The ordered event bus drains everything
            // published for this tick after simulation systems finish (tick -> lane -> sequence).

            budget.Next(Stage::Animation, [&] {
                return SimParticipantCount<luminumbra::animation::AnimationPlayerComponent>(
                    m_registry);
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
                luminumbra::ai::RunInstinctSystemOnTick(
                    m_registry, current_tick, &stimulus_registry);
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
            luminumbra::ai::RunPerceptionSystemOnTick(
                m_registry, static_cast<float>(m_simulationClock.fixed_dt()));

            budget.Next(Stage::Scent, [&] {
                return (m_scentField &&
                        (HasScentParticipants() ||
                         SimParticipantCount<TelemetryComp::ForagerComponent>(m_registry) != 0))
                           ? static_cast<std::uint64_t>(kScentFieldCells) * kScentFieldCells *
                                 kScentFieldChannels
                           : 0ull;
            });
            const bool scent_active = HasScentParticipants();
            AdvanceNearScent();

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
                                                       ScentDomainOrigin(true),
                                                       ScentDomainOrigin(false),
                                                       kScentCellSize);
            }

            budget.Next(Stage::Creatures, [&] {
                return SimParticipantCount<TelemetryComp::CreatureComponent,
                                           TelemetryComp::TransformComponent>(m_registry);
            });
            // 2e. : Utility-AI creature brain (predator/prey movement). Per-entity opt-in
            // via CreatureComponent -> a world with none is a no-op (canonical roster
            // byte-identical, same discipline as plants/scent). Deterministic (id-ordered,
            // libm-free).
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
                        ScentDomainOrigin(true),
                        ScentDomainOrigin(false),
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
                        m_registry,
                        static_cast<float>(m_simulationClock.fixed_dt()),
                        m_thirstTuning);
                    luminumbra::ai::RunScavengingOnTick(
                        m_registry, current_tick, m_scavengingTuning);
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
                                    m_registry.try_get<Luminumbra::Components::ScavengerComponent>(
                                        e)) {
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
                        auto phys =
                            m_registry.view<Luminumbra::Components::CreaturePhysicsComponent,
                                            Luminumbra::Components::CreatureComponent,
                                            Luminumbra::Components::TransformComponent>();
                        if (phys.begin() != phys.end()) {
                            std::vector<entt::entity> pe(phys.begin(), phys.end());
                            std::sort(pe.begin(), pe.end(), [](entt::entity a, entt::entity b) {
                                return entt::to_integral(a) < entt::to_integral(b);
                            });
                            for (auto e : pe) {
                                const auto& cr =
                                    phys.get<Luminumbra::Components::CreatureComponent>(e);
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
                m_windFieldSystem->Update(current_tick, m_ambientFieldAnchor);
                AdvanceRegionalWeather(current_tick);
            } else {
                budget.Next(Stage::Wind, [&] {
                    return m_windFieldSystem
                               ? static_cast<std::uint64_t>(Systems::kWindExtentCells) *
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
                    return m_weatherSystem
                               ? static_cast<std::uint64_t>(Systems::kWeatherExtentCells) *
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
                return m_aetherFieldSystem
                           ? static_cast<std::uint64_t>(Systems::kAetherExtentCells) *
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

            budget.Next(Stage::Energy, [&] {
                return m_energyFieldState ? m_energyFieldState->page_count() : 0ull;
            });
            // 5a.: the STATEFUL energy layer ticks directly
            // after the re-derivable field, anchored on the SAME replicated anchor
            // quantized by the shared 24 m cell size (never camera state). Null
            // unless sim.aether_state opted in — the canonical world skips this
            // block entirely (byte-identical). Deposits are queued by emitters
            // right below; Tick applies them sorted.
            if (m_energyFieldState) {
                const int anchor_cx = static_cast<int>(
                    std::floor(m_metadata.spawnPoint.x / Systems::kAetherCellSizeM));
                const int anchor_cz = static_cast<int>(
                    std::floor(m_metadata.spawnPoint.z / Systems::kAetherCellSizeM));
                m_energyFieldState->SetAnchorCell(anchor_cx, anchor_cz);
                // gather this tick's FieldEmitterComponent deposits
                // (id-sorted, two-phase) into the layer before it ticks.
                // Participant-gated: no emitter component, no deposits, no bytes.
                if (m_activeRegionsEnabled) {
                    RunRegionalEnergyTick(current_tick);
                } else {
                    Systems::GatherFieldEmitterDeposits(m_registry, *m_energyFieldState);
                    m_energyFieldState->Tick(current_tick);
                }
            }

            RunRegionalWaterTick();
            RunFoliageTick(budget, current_tick);
            {
                namespace Comp = Luminumbra::Components;
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
                // Decomposition: dead creatures decay + release nutrient (consumed by the soil
                // loop).
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
                    constexpr std::uint64_t kTicksPerDay =
                        30ull * 60ull * 20ull; // 20-min day @30Hz
                    const float tod01 =
                        m_activeRegionsEnabled
                            ? static_cast<float>(m_worldClock.day_phase(current_tick))
                            : static_cast<float>(current_tick % kTicksPerDay) /
                                  static_cast<float>(kTicksPerDay);
                    luminumbra::ai::RunCircadianOnTick(m_registry, tod01, m_circadianAmplitude);
                }
                // Territory: claim home + emit a homing bias (movement blends it like
                // mate-seeking).
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
            if (m_activeRegionsEnabled)
                m_simulationEventBus.drain(current_tick, [this] { return !m_regionFault; });
            else
                m_simulationEventBus.drain(current_tick);
            // Consumer counters are published only after the ordered drain succeeds.
            budget.Finish();
            if (m_regionFault)
                return i + 1;
            if (m_activeRegionsEnabled) {
                PrepareRegionalFoliage();
                m_activeRegionLedger.publish_completed_work(m_regionCompletedWork);
                m_regionCompletedWork.clear();
            }
        } catch (const std::exception& error) {
            if (!m_activeRegionsEnabled)
                throw;
            m_worldClock.set_tick(current_tick);
            LatchRegionFault(current_tick, error.what());
            return i + 1;
        } catch (...) {
            if (!m_activeRegionsEnabled)
                throw;
            LatchRegionFault(current_tick, "Unknown simulation consumer fault");
            return i + 1;
        }
    }
    return ticks_executed;
}

luminumbra::foliage::PlantEnvSample
GameSession::SamplePlantEnvironment(const Components::TransformComponent& tf,
                                    std::uint64_t current_tick) const {
    constexpr float kFoliageFieldCell = 1.0f;
    const float foliageOriginX = m_metadata.spawnPoint.x - 128.0f;
    const float foliageOriginZ = m_metadata.spawnPoint.z - 128.0f;
    luminumbra::foliage::PlantEnvSample s;
    const Luminumbra::Vec3 p = tf.position;
    // Moisture: weather precipitation (rain -> growth) + irrigation (player
    // watering).
    const float precip = m_weatherSystem ? m_weatherSystem->PrecipitationAt(p) : 0.0f;
    s.moisture = luminumbra::foliage::clamp01(0.30f + precip * 0.70f);
    // fold the freshly-updated IRRIGATION grid (milli 0..1000) into moisture —
    // watered cells grow better (watered-beats-dry). No grid -> unchanged.
    if (m_activeRegionsEnabled ? !m_fieldPages.irrigation().empty() : bool(m_irrigationGrid)) {
        const int moist = m_activeRegionsEnabled
                              ? IrrigationAt(p)
                              : luminumbra::foliage::MoistureAt(*m_irrigationGrid,
                                                                p.x,
                                                                p.z,
                                                                foliageOriginX,
                                                                foliageOriginZ,
                                                                kFoliageFieldCell);
        s.moisture =
            luminumbra::foliage::clamp01(s.moisture + static_cast<float>(moist) / 1000.0f * 0.5f);
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
        temp = luminumbra::foliage::clamp01(0.60f - (altitude > 0.0f ? altitude : 0.0f) * 0.00045f);
    }
    s.soil_quality = soil;
    // fold the freshly-updated SOIL-NUTRIENT grid (milli 0..1000) into soil
    // quality. Foragers/feeders DEPLETE the cell, so a monoculture on one cell
    // starves itself (monoculture-starves). No grid -> unchanged terrain
    // quality.
    if (m_activeRegionsEnabled ? !m_fieldPages.soil().empty() : bool(m_soilGrid)) {
        const int nut =
            m_activeRegionsEnabled
                ? SoilNutrientAt(p)
                : luminumbra::foliage::NutrientAt(
                      *m_soilGrid, p.x, p.z, foliageOriginX, foliageOriginZ, kFoliageFieldCell);
        // NutrientAt is milli-of-milli (kSoilBaseline == 1.0 nutrient), so
        // normalise by kSoilBaseline (NOT 1000) to a [0,1] fraction before
        // blending.
        const float nutFrac =
            static_cast<float>(nut) / static_cast<float>(luminumbra::foliage::kSoilBaseline);
        s.soil_quality = luminumbra::foliage::clamp01(s.soil_quality * 0.5f + nutFrac * 0.5f);
    }
    s.temperature = temp;
    // Real day/night light from the sim clock. Triangular day
    // curve peaking at noon, with a twilight floor so growth slows (not halts)
    // at night. kTicksPerDay = 20-min day @30Hz (mirrors the circadian slot).
    constexpr std::uint64_t kTicksPerDay = 30ull * 60ull * 20ull;
    const float tod01 =
        m_activeRegionsEnabled
            ? static_cast<float>(m_worldClock.day_phase(current_tick))
            : static_cast<float>(current_tick % kTicksPerDay) / static_cast<float>(kTicksPerDay);
    const float noonDist = tod01 < 0.5f ? (0.5f - tod01) : (tod01 - 0.5f);
    const float day = 1.0f - 2.0f * noonDist; // 0 at midnight -> 1 at noon
    s.light = luminumbra::foliage::clamp01(0.15f + 0.85f * day);
    //  (season): a deterministic ANNUAL temperature swing folded onto the
    //  terrain
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
            : static_cast<float>(current_tick % kTicksPerYear) / static_cast<float>(kTicksPerYear);
    const float midDist = season01 < 0.5f ? (0.5f - season01) : (season01 - 0.5f);
    const float warmth = 1.0f - 2.0f * midDist; // 0 = midwinter -> 1 = midsummer
    constexpr float kSeasonAmplitude = 0.30f;   // +-0.15 temperature swing
    s.temperature =
        luminumbra::foliage::clamp01(s.temperature + (warmth - 0.5f) * kSeasonAmplitude);
    return s;
}

void GameSession::RunRegionalWaterTick() {
    if (!m_activeRegionsEnabled || !m_waterSystem || !m_worldSystem)
        return;
    const auto resident = m_worldSystem->resident_streamed_chunks();
    m_waterSystem->UpdateRegional(
        m_registry, resident, m_activeRegionLedger, m_regionSchedule, m_regionCompletedWork);
}

void GameSession::RunFoliageTick(luminumbra::simulation::SimBudgetTelemetry::TickScope& budget,
                                 std::uint64_t current_tick) {
    using Stage = luminumbra::simulation::SimBudgetStage;
    namespace TelemetryComp = Components;
    if (m_activeRegionsEnabled) {
        budget.Next(Stage::Plants, [] { return 0u; });
        RunRegionalFoliageTick(budget, current_tick);
        return;
    }
    const RegionEntityGate foliage_gate;

    // 5b.: the soil-nutrient + irrigation-moisture FIELDS update BEFORE plant
    // growth (slot 6) so growth reads FRESH availability, not last tick's (the prior order
    // had growth at slot 6 reading grids that only updated at slot 7 -> a 1-tick-stale
    // env). Opt-in via WaterSource/SoilFeeder; a world with none updates no grid, so the
    // canonical roster is byte-identical. The shared foliage field anchor (origin + cell
    // size) is hoisted here so both the growth env-sampler (slot 6) and the  living-world
    // systems (slot 7) share it.
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
                m_irrigationGrid =
                    std::make_unique<fol::IrrigationGrid>(kFoliageFieldCells, kFoliageFieldCells);
            fol::RunIrrigationOnTick(
                m_registry, *m_irrigationGrid, foliageOriginX, foliageOriginZ, kFoliageFieldCell);
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
        luminumbra::foliage::EnvSampler plant_env = [this, current_tick](const auto& tf) {
            return SamplePlantEnvironment(tf, current_tick);
        };
        luminumbra::foliage::RunPlantGrowthSystemOnTick(m_registry, current_tick, plant_env);
    }

    // 7. LIVING-WORLD SYSTEMS: pollination / disease / fire / wildlife-grazing / lifespan
    // / … Each is per-entity OPT-IN via its own participant component, so a world carrying
    // none runs ZERO of them and world_hash stays byte-identical (canonical
    // NetworkStateHash baseline holds) — same discipline as plants/creatures/scent.
    // Deterministic (id-ordered, libm-free, seeded-from-ints). Fixed run order for
    // run==replay. NOTE: the soil-nutrient + irrigation FIELDS now update at slot 5b
    // (BEFORE plant growth) so growth reads fresh availability; only the consumers remain
    // here. Wind coupling (fire/pollination) drifts downwind.
    {
        namespace Comp = Luminumbra::Components;
        namespace fol = luminumbra::foliage;
        // Wind coupling: fire spreads + pollen drifts DOWNWIND, sampled from the
        // (already deterministic, already-hashed) wind field at the anchor.
        const ::Luminumbra::Vec2 windXZ = m_windFieldSystem
                                              ? m_windFieldSystem->SampleWind(m_metadata.spawnPoint)
                                              : ::Luminumbra::Vec2(0.0f);

        budget.Next(Stage::Pollination, [&] {
            return SimParticipantCount<TelemetryComp::PollinationTag,
                                       TelemetryComp::PlantGenomeComponent,
                                       TelemetryComp::PlantGrowthComponent,
                                       TelemetryComp::TransformComponent,
                                       TelemetryComp::PollinationComponent>(m_registry);
        });
        if (!m_registry.view<Comp::PlantGenomeComponent>().empty())
            fol::RunPollinationOnTick(
                m_registry, current_tick, windXZ, 0, m_plantMutationRate, foliage_gate);
        budget.Next(Stage::Disease, [&] {
            return SimParticipantCount<TelemetryComp::PlantTag,
                                       TelemetryComp::PlantHealthComponent,
                                       TelemetryComp::TransformComponent>(m_registry);
        });
        if (!m_registry.view<Comp::PlantHealthComponent>().empty())
            fol::RunPlantDiseaseOnTick(m_registry, current_tick, 0, foliage_gate);
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
            fol::RunCropLifecycleOnTick(m_registry, current_tick, 0, foliage_gate);
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
                m_registry, current_tick, windXZ, weather_ignitions, foliage_gate);
        }
        budget.Next(Stage::Grazing, [&] {
            return SimParticipantCount<TelemetryComp::GrazeableComponent,
                                       TelemetryComp::TransformComponent>(m_registry);
        });
        if (!m_registry.view<Comp::GrazeableComponent>().empty())
            luminumbra::ai::RunWildlifeFoliageOnTick(
                m_registry, current_tick, m_wildlifeFoliageTuning, foliage_gate);
        budget.Next(Stage::Lifespan, [&] {
            return SimParticipantCount<TelemetryComp::MortalComponent>(m_registry);
        });
        if (!m_registry.view<Comp::MortalComponent>().empty())
            luminumbra::ai::RunLifespanOnTick(m_registry, current_tick, foliage_gate);
    }
}

WorldConfigValidationResult
GameSession::ValidateWorldConfig(const std::string& root_path,
                                 const std::string& worldType,
                                 const std::vector<std::filesystem::path>& required_assets) {
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
        AddValidationError(result, "missing world preset: " + result.preset_path.string());
    }

    // simulation needs only the preset. Any further runtime assets
    // are caller-supplied (the client registers shaders/RML/fonts; a headless
    // host registers none).
    for (const fs::path& relative : required_assets) {
        const fs::path path = root / relative;
        if (!fs::exists(path)) {
            AddValidationError(result, "missing required runtime asset: " + path.string());
        }
    }

    if (result.ok) {
        const TerrainPresetLoadResult preset = LoadTerrainPreset(result.preset_path);
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
    return CreateWorldInternal(name, seed, worldType, customPresetJson, false);
}

bool GameSession::CreateTransientWorld(const std::string& name,
                                       const std::string& seed,
                                       const std::string& worldType) {
    return CreateWorldInternal(name, seed, worldType, nullptr, true);
}

bool GameSession::CreateWorldInternal(const std::string& name,
                                      const std::string& seed,
                                      const std::string& worldType,
                                      const std::string* customPresetJson,
                                      bool transient) {
    m_worldOpenError.clear();
    if (!m_jobSystem) {
        LUMINUMBRA_CORE_ERROR("JobSystem not set before creating world");
        return false;
    }

    const WorldConfigValidationResult validation =
        ValidateWorldConfig(m_rootPath, worldType, m_requiredClientAssets);
    if (!validation.ok) {
        for (const std::string& error : validation.errors) {
            LUMINUMBRA_CORE_ERROR("World config validation failed: {}", error);
        }
        return false;
    }

    WorldMetadata metadata;
    metadata.name = name;
    metadata.seed = seed.empty() ? std::to_string(std::random_device{}()) : seed;
    metadata.worldType = worldType;
    metadata.worldId = transient ? std::string{} : GenerateWorldId();
    metadata.creationTime = std::time(nullptr);
    metadata.spawnPoint = Vec3(0, 100, 0);
    const auto worldPath = RuntimeRoot(m_rootPath) / "worlds" / "saves" / metadata.worldId;
    if (!transient) {
        try {
            fs::create_directories(worldPath);
        } catch (const std::exception& e) {
            m_worldOpenError = "Could not create world save directory: " + std::string(e.what());
            LUMINUMBRA_CORE_ERROR("{}", m_worldOpenError);
            return false;
        }
    }

    // Customized world: embed the resolved preset in THIS world's own save dir and generate from
    // it, so the world is self-contained (no global custom files / dangling references).
    fs::path preset_to_load = validation.preset_path;
    if (customPresetJson) {
        const fs::path world_preset = worldPath / "preset.json";
        std::ofstream pf(world_preset, std::ios::binary);
        pf << *customPresetJson;
        pf.close();
        if (!pf.good()) {
            m_worldOpenError = "Could not save this world's customized preset.";
            return false;
        }
        preset_to_load = world_preset;
    }

    const TerrainPresetLoadResult preset = LoadTerrainPreset(preset_to_load);
    if (!preset.ok) {
        for (const std::string& error : preset.errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return false;
    }
    TerrainGenParams params = preset.params;
    ClampTerrainParams(params);

    ResetWorldSystems();
    m_metadata = std::move(metadata);
    m_transientWorld = transient;
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    int world_seed = StringToSeed(m_metadata.seed);
    LUMINUMBRA_CORE_INFO("Loaded world preset '{}': height_offset={}, amplitude={}, caves={}",
                         worldType,
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
    m_waterSystem->SetRegionalScheduling(m_activeRegionsEnabled);
    LUMINUMBRA_CORE_INFO("World and water systems linked.");

    // sim.water_high_res: raise the solver to High BEFORE any chunk seeds
    // (default-OFF = untouched Medium = byte-identical).
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
    BindRegionalFieldQueries();
    LUMINUMBRA_CORE_INFO("Aether field system initialized.");

    // the stateful energy layer, constructed ONLY when
    // sim.aether_state opted in (default OFF -> null -> byte-identical).
    InitializeEnergyFieldState();

    // Calculate appropriate spawn point based on actual terrain height
    float spawn_x = 8.0f;
    float spawn_z = 8.0f;
    float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
    LUMINUMBRA_CORE_INFO("Initial terrain height sampled at spawn: {}.", terrain_height);

    m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + kSpawnEyeHeight, spawn_z);
    m_ambientFieldAnchor = m_metadata.spawnPoint;
    if (m_activeRegionsEnabled)
        m_activeRegionLedger.try_set_local_anchor(m_metadata.spawnPoint);
    AttachRegionEditObserver();
    InitializeScentField(m_metadata.spawnPoint);

    // species definitions load once at world create (content-pure; see the helper).
    LoadSpeciesDefinitions();

    // wire weather-driven rain into the water solver when the
    // owner opted in (sim.hydrology_weather via SetWeatherRainEnabled). Default-OFF =
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
    if (!transient && !SaveWorld()) {
        m_worldOpenError = "Could not save the new world's metadata.";
        LUMINUMBRA_CORE_ERROR("{}", m_worldOpenError);
        return false;
    }
    return true;
}

bool GameSession::LoadWorld(const std::string& worldId) {
    if (!IsSimulationTickBoundary())
        return false;
    m_lastLoadedChunkCount = 0;
    // An interrupted indexed save is rolled back (or its stale journal dropped) before anything
    // reads the directory. The identifier check mirrors InspectSavedWorld's.
    if (!worldId.empty() && worldId != "." && worldId != ".." &&
        worldId.find_first_of("/\\:") == std::string::npos) {
        const fs::path candidate = RuntimeRoot(m_rootPath) / "worlds" / "saves" / worldId;
        std::error_code status_error;
        if (fs::is_directory(candidate, status_error) && !fs::is_symlink(candidate, status_error)) {
            std::vector<std::string> recovery_errors;
            if (Persistence::recover_pending_journal(candidate, &recovery_errors) ==
                Persistence::RecoveryOutcome::Failed) {
                m_worldOpenError =
                    "World unavailable: an interrupted save could not be recovered: " +
                    (recovery_errors.empty() ? std::string("unknown error")
                                             : recovery_errors.front());
                LUMINUMBRA_CORE_ERROR("World open refused: {}", m_worldOpenError);
                return false;
            }
        }
    }
    const auto saved = Persistence::InspectSavedWorld(RuntimeRoot(m_rootPath), worldId);
    m_worldOpenError = saved.error;
    if (m_worldOpenError.empty() && saved.requires_active_regions && !m_activeRegionsEnabled)
        m_worldOpenError = "Incompatible configuration: this world requires sim.active_regions.";
    if (!m_worldOpenError.empty()) {
        LUMINUMBRA_CORE_ERROR("World open refused: {}", m_worldOpenError);
        return false;
    }
    if (!m_jobSystem) {
        m_worldOpenError = "JobSystem not set before loading world.";
        return false;
    }
    for (const auto& asset : m_requiredClientAssets) {
        if (!fs::exists(RuntimeRoot(m_rootPath) / asset)) {
            m_worldOpenError = "Missing required runtime asset: " + asset.string();
            return false;
        }
    }
    const fs::path worldPath = RuntimeRoot(m_rootPath) / "worlds" / "saves" / worldId;
    const auto& preset_to_load = saved.preset_path;
    const TerrainPresetLoadResult preset = LoadTerrainPreset(preset_to_load);
    if (!preset.ok) {
        for (const std::string& error : preset.errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return false;
    }
    TerrainGenParams params = preset.params;
    ClampTerrainParams(params);

    ResetWorldSystems();
    m_metadata = saved.metadata;
    if (m_activeRegionsEnabled) {
        m_worldClock = saved.clock;
        m_simulationClock.reset(m_worldClock.tick());
    }
    m_transientWorld = false;
    int world_seed = StringToSeed(m_metadata.seed);

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
    m_waterSystem->SetRegionalScheduling(m_activeRegionsEnabled);

    // sim.water_high_res: raise the solver to High BEFORE any chunk is adopted
    // or seeded (default-OFF = untouched Medium = byte-identical). A loaded save
    // at another resolution migrates in LoadWorldStateFrom's boot pass.
    ApplyWaterResolutionWiring();

    // The legacy path starts at zero. Active-region clocks restore the ambient
    // fields at the saved absolute tick in LoadWorldStateFrom, before any tick.
    m_windFieldSystem = std::make_unique<Systems::WindFieldSystem>(world_seed);
    m_weatherSystem = std::make_unique<Systems::WeatherSystem>(world_seed);
    m_aetherFieldSystem = std::make_unique<Systems::AetherFieldSystem>(world_seed);
    BindRegionalFieldQueries();

    // unlike the re-derivable trio above, the stateful
    // energy layer is AUTHORITATIVE state — when enabled, a persisted record
    // restores it (epoch-rebased onto the session clock); an absent
    // record is all-zeros by contract. OFF -> null -> byte-identical.
    InitializeEnergyFieldState();
    if (m_energyFieldState && !m_activeRegionsEnabled) {
        const fs::path recordPath = worldPath / "aether_state.efs";
        std::ifstream recordFile(recordPath, std::ios::binary);
        if (recordFile) {
            std::ostringstream recordBytes;
            recordBytes << recordFile.rdbuf();
            if (m_energyFieldState->DeserializeRecord(recordBytes.str(),
                                                      GetSimulationTickCount())) {
                LUMINUMBRA_CORE_INFO("Aether state record restored ({} pages).",
                                     m_energyFieldState->page_count());
            } else {
                LUMINUMBRA_CORE_WARN("Aether state record UNREADABLE at {} - starting all-zero.",
                                     recordPath.string());
            }
        }
    }

    // Legacy saves without a persisted spawnPoint: derive it from terrain
    // height exactly like CreateWorld does (pure function of seed/params).
    if (!saved.has_spawn_point) {
        const float spawn_x = 8.0f;
        const float spawn_z = 8.0f;
        const float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
        m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + kSpawnEyeHeight, spawn_z);
    }
    InitializeScentField(m_metadata.spawnPoint);

    // species definitions load once at world load, exactly as CreateWorld does
    // (content-pure; a loaded world resolves the same table as the created one).
    LoadSpeciesDefinitions();

    // restore the rotating water sim-window cursor so the loaded session
    // resimulates the exact windows the original would from the same water state.
    if (m_worldSystem) {
        m_worldSystem->SetWaterSimWindowCursor(saved.water_sim_cursor);
    }

    // same weather-rain wiring as CreateWorld (default-OFF = no-op).
    ApplyWeatherRainWiring();

    m_worldOpenError.clear();
    AttachRegionEditObserver();
    if (!LoadWorldState())
        return false;
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
    return Persistence::WorldSaveService::read_clock_metadata(save_dir, clock, required) &&
           (!required || m_activeRegionsEnabled);
}

std::int32_t GameSession::SoilNutrientAt(const Vec3& position) const {
    if (!ActiveRegionLedger::valid_anchor(position))
        return 0;
    if (m_activeRegionsEnabled)
        return m_fieldPages.soil().sample(luminumbra::foliage::FoliageCell(position), 1000000);
    return m_soilGrid ? luminumbra::foliage::NutrientAt(*m_soilGrid,
                                                        position.x,
                                                        position.z,
                                                        m_metadata.spawnPoint.x - 128,
                                                        m_metadata.spawnPoint.z - 128,
                                                        1)
                      : 1000000;
}
std::int32_t GameSession::IrrigationAt(const Vec3& position) const {
    if (!ActiveRegionLedger::valid_anchor(position))
        return 0;
    if (m_activeRegionsEnabled)
        return m_fieldPages.irrigation().sample(luminumbra::foliage::FoliageCell(position), 0);
    return m_irrigationGrid ? luminumbra::foliage::MoistureAt(*m_irrigationGrid,
                                                              position.x,
                                                              position.z,
                                                              m_metadata.spawnPoint.x - 128,
                                                              m_metadata.spawnPoint.z - 128,
                                                              1)
                            : 0;
}
bool GameSession::SetFoliageCell(const Vec3& position, bool soil, std::int32_t value) {
    if (!m_activeRegionsEnabled || m_regionFault || !ActiveRegionLedger::valid_anchor(position) ||
        value < 0 || value > (soil ? 1000000 : 4000000))
        return false;
    try {
        auto pages = m_fieldPages;
        auto ledger = m_activeRegionLedger;
        const auto cell = luminumbra::foliage::FoliageCell(position);
        if (!luminumbra::foliage::ActivateFoliagePage(pages, cell, soil))
            return false;
        ledger.mark_edited(ActiveRegionLedger::region_at(position), m_worldClock);
        if (!(soil ? pages.set_soil(cell, value) : pages.set_irrigation(cell, value)))
            return false;
        m_activeRegionLedger = std::move(ledger);
        m_fieldPages = std::move(pages);
        return true;
    } catch (const std::exception& e) {
        LatchRegionFault(m_worldClock.tick(), e.what());
        return false;
    }
}
bool GameSession::AdmitPlant(entt::entity entity) {
    return AdmitPlantAt(entity, m_worldClock.tick());
}
bool GameSession::AdmitPlantAt(entt::entity entity, std::uint64_t fresh_stamp) {
    try {
        namespace C = Components;
        if (!m_activeRegionsEnabled || !m_worldSystem || m_regionFault ||
            !m_registry.valid(entity) ||
            !m_registry.all_of<C::PlantTag,
                               C::TransformComponent,
                               C::PlantGrowthComponent,
                               C::PlantGenomeComponent>(entity))
            return false;
        const auto& tf = m_registry.get<C::TransformComponent>(entity);
        if (!ActiveRegionLedger::valid_anchor(tf.position))
            return false;
        const auto key = ActiveRegionLedger::region_at(tf.position);
        auto& growth = m_registry.get<C::PlantGrowthComponent>(entity);
        if (const auto* owner = m_registry.try_get<C::PlantRegionComponent>(entity)) {
            if (!owner->persistent_id ||
                owner->persistent_id >= m_activeRegionLedger.next_persistent_id() ||
                RegionKey{owner->owner_x, owner->owner_z} != key ||
                !m_activeRegionLedger.records().contains(key) ||
                owner->region_stamp > m_worldClock.tick() ||
                growth.last_tick != owner->region_stamp ||
                m_groundObjects.find(owner->persistent_id))
                return false;
            return true;
        }
        // Fresh ECS admission only; enabled snapshots require identities before Apply.
        if (fresh_stamp > m_worldClock.tick() || growth.planted_tick > fresh_stamp ||
            growth.last_tick > m_worldClock.tick() || growth.planted_tick > growth.last_tick ||
            growth.stage >= C::kPlantStageCount || growth.quality > 100)
            return false;
        const auto& genome = m_registry.get<C::PlantGenomeComponent>(entity);
        for (float gene : genome.genes)
            if (!std::isfinite(gene) || gene < 0 || gene > 1)
                return false;
        std::uint64_t id = 0;
        if (!m_activeRegionLedger.allocate_persistent_id(id)) {
            LatchRegionFault(m_worldClock.tick(), "Persistent plant allocator exhausted");
            return false;
        }
        auto ledger = m_activeRegionLedger;
        auto pages = m_fieldPages;
        const auto old_cells = luminumbra::foliage::FoliageCells(pages.soil()).size() +
                               luminumbra::foliage::FoliageCells(pages.irrigation()).size();
        if (m_registry.all_of<C::SoilFeederComponent>(entity) &&
            !luminumbra::foliage::ActivateFoliagePage(
                pages, luminumbra::foliage::FoliageCell(tf.position), true))
            return false;
        if (m_registry.all_of<C::WaterSourceComponent>(entity) &&
            !luminumbra::foliage::ActivateFoliagePage(
                pages, luminumbra::foliage::FoliageCell(tf.position), false))
            return false;
        ledger.mark_edited(key, m_worldClock);
        // Validate all opt-in payloads before publishing a fresh participant.
        // The consumed allocator ID survives a refusal, as for other failed spawns.
        entt::registry candidate;
        const auto staged = candidate.create();
        luminumbra::foliage::stage_detail::CopyFoliage(m_registry, entity, candidate, staged);
        candidate.get<C::PlantGrowthComponent>(staged).last_tick = fresh_stamp;
        candidate.emplace<C::PlantRegionComponent>(staged, id, key.x, key.z, fresh_stamp);
        std::string error;
        if (!luminumbra::foliage::ValidatePlantEntitySnapshot(
                luminumbra::foliage::BuildPlantEntitySnapshot(candidate, true),
                ledger,
                m_groundObjects,
                error))
            return false;
        m_activeRegionLedger = std::move(ledger);
        m_fieldPages = std::move(pages);
        growth.last_tick = fresh_stamp;
        m_registry.emplace<C::PlantRegionComponent>(entity, id, key.x, key.z, fresh_stamp);
        if (m_simulationBatchInProgress) {
            const auto initialized =
                luminumbra::foliage::FoliageCells(m_fieldPages.soil()).size() +
                luminumbra::foliage::FoliageCells(m_fieldPages.irrigation()).size() - old_cells;
            if (initialized)
                RecordActiveRegionWork({key, 0, 0, 0, initialized});
        }
        return true;
    } catch (const std::exception& error) {
        LatchRegionFault(m_worldClock.tick(), error.what());
        return false;
    }
}
void GameSession::PrepareRegionalFoliage() {
    namespace C = Components;
    std::set<std::uint64_t> ids;
    for (const auto& [id, object] : m_groundObjects.objects())
        ids.insert(id);
    std::vector<entt::entity> plants;
    for (auto e : m_registry.view<C::PlantTag>())
        plants.push_back(e);
    std::sort(plants.begin(), plants.end());
    for (auto e : plants) {
        const bool fresh = !m_registry.all_of<C::PlantRegionComponent>(e);
        if (!AdmitPlant(e)) {
            if (fresh)
                m_registry.destroy(e);
            throw std::runtime_error("Plant admission or authority refused");
        }
        const auto id = m_registry.get<C::PlantRegionComponent>(e).persistent_id;
        if (!ids.insert(id).second)
            throw std::runtime_error("Duplicate shared plant identity");
    }
    // Existing feeders may opt in after the plant itself was admitted. Sources and
    // feeders get exactly one owned page, never a player-centred domain expansion.
    const auto admit_page = [this](entt::entity e, bool soil) {
        const auto& p = m_registry.get<C::TransformComponent>(e).position;
        const auto cell = luminumbra::foliage::FoliageCell(p);
        const auto owner = FieldPageState::region_of_scent_cell(cell);
        const auto& cells = soil ? m_fieldPages.soil() : m_fieldPages.irrigation();
        if (cells.initialized(cell)) {
            if (!m_activeRegionLedger.records().contains(owner))
                throw std::runtime_error("Orphan foliage page");
            return;
        }
        auto ledger = m_activeRegionLedger;
        auto pages = m_fieldPages;
        if (!luminumbra::foliage::ActivateFoliagePage(pages, cell, soil))
            throw std::runtime_error("Foliage page admission cap");
        ledger.mark_edited(owner, m_worldClock);
        m_activeRegionLedger = std::move(ledger);
        m_fieldPages = std::move(pages);
    };
    for (auto e : m_registry.view<C::PlantTag, C::SoilFeederComponent, C::TransformComponent>())
        admit_page(e, true);
    for (auto e : m_registry.view<C::WaterSourceComponent, C::TransformComponent>())
        admit_page(e, false);
}
std::string GameSession::PlantRegionProjection(RegionKey key) const {
    auto snap = luminumbra::foliage::BuildPlantEntitySnapshot(m_registry, true);
    std::erase_if(snap.entities, [key](const auto& entity) {
        for (const auto& c : entity.components)
            if (c.type == "PlantRegion")
                return c.data.at("owner_x").template get<std::int32_t>() != key.x ||
                       c.data.at("owner_z").template get<std::int32_t>() != key.z;
        return true;
    });
    return snap.entities.empty() ? std::string{}
                                 : "|plants:" + Ecs::SerializeEntityRegistrySnapshotJson(snap);
}
void GameSession::RunRegionalEnergyTick(std::uint64_t tick) {
    if (!m_energyFieldState)
        return;
    const auto gathered =
        Systems::GatherRegionalFieldEmitterDeposits(m_registry, *m_energyFieldState);
    if (gathered.invalid_emitters != 0 || !gathered.accepted)
        LUMINUMBRA_CORE_WARN(
            "Regional energy gather: {} invalid emitters skipped; batch accepted={}",
            gathered.invalid_emitters,
            gathered.accepted);
    luminumbra::fields::EnergyFieldState::RegionalResult result;
    const auto& due = SortedDueRegions();
    if (!m_energyFieldState->TickRegional(tick,
                                          m_activeRegionLedger.records(),
                                          due,
                                          m_activeRegionLedger.energy_ages(),
                                          result) ||
        !m_activeRegionLedger.set_energy_ages(result.ages))
        throw std::runtime_error("Regional energy tick refused");
    for (const auto& work : result.work)
        RecordActiveRegionWork(work);
}

void GameSession::RunRegionalFoliageTick(
    luminumbra::simulation::SimBudgetTelemetry::TickScope& budget, std::uint64_t tick) {
    auto ledger = m_activeRegionLedger;
    auto pages = m_fieldPages.foliage_snapshot();
    auto work = m_regionCompletedWork;
    std::set<entt::entity> retained;
    auto staged = luminumbra::foliage::StageFoliageRegistry(m_registry, retained);
    auto original = std::move(m_registry);
    m_registry = std::move(staged);
    try {
        RunRegionalFoliageTickImpl(budget, tick, pages);
        luminumbra::foliage::CommitFoliageRegistry(original, m_registry, retained);
        m_registry = std::move(original);
    } catch (...) {
        // Failed births consume IDs, but no partial growth/page/resume state is
        // published. A recovery save therefore cannot replay half a resume hook.
        const auto next = m_activeRegionLedger.next_persistent_id();
        while (ledger.next_persistent_id() < next) {
            std::uint64_t consumed = 0;
            if (!ledger.allocate_persistent_id(consumed))
                break;
        }
        m_activeRegionLedger = std::move(ledger);
        m_fieldPages.restore_foliage(std::move(pages));
        m_regionCompletedWork = std::move(work);
        m_registry = std::move(original);
        if (m_regionFault) {
            m_activeRegionLedger.clear_completed_work();
            m_regionCompletedWork.clear();
            m_regionWork.clear();
        }
        throw;
    }
}
void GameSession::RunRegionalFoliageTickImpl(
    luminumbra::simulation::SimBudgetTelemetry::TickScope& budget,
    std::uint64_t tick,
    const FieldPageState& previous) {
    namespace fol = luminumbra::foliage;
    namespace C = Components;
    using Stage = luminumbra::simulation::SimBudgetStage;
    // Every foliage stage has a sample even on an off-due or empty-window tick.
    // Record coalesces the measured substeps back into these host-tick samples.
    for (const auto stage : {Stage::Irrigation,
                             Stage::Soil,
                             Stage::Plants,
                             Stage::Pollination,
                             Stage::Disease,
                             Stage::Crops,
                             Stage::Fire,
                             Stage::Grazing,
                             Stage::Lifespan})
        budget.Next(stage, [] { return 0u; });
    budget.Next(Stage::Plants, [] { return 0u; });
    const auto& due = SortedDueRegions();
    std::map<RegionKey, std::uint32_t> windows;
    std::map<RegionKey, std::uint64_t> resumes;
    for (const auto& [key, record] : m_activeRegionLedger.records()) {
        if (record.state == RegionState::Frozen)
            continue;
        if (record.frozen_at) {
            const auto window =
                ActiveRegionLedger::resume_window(key, record.frozen_at, m_worldClock, 36000);
            windows[key] = static_cast<std::uint32_t>(window.elapsed_ticks);
            resumes[key] = record.frozen_at;
        } else if (std::binary_search(due.begin(), due.end(), key)) {
            const auto old = m_foliagePreviousTicks.find(key);
            // Zero is a valid persisted tick, not an uninitialized sentinel.
            // A freshly admitted owner starts at first_active rather than replaying
            // the entire world lifetime before its page or plant existed.
            const auto prior = old == m_foliagePreviousTicks.end()
                                   ? record.first_active
                                   : std::max(old->second, record.first_active);
            const auto elapsed = tick - prior;
            windows[key] = static_cast<std::uint32_t>(std::min<std::uint64_t>(elapsed, 65536));
        }
    }
    std::map<RegionKey, std::uint64_t> page_work;
    std::set<RegionKey> completed_regions;
    struct GrowthWindow {
        fol::PlantEnvSample environment;
        std::uint16_t infection = 0;
        std::uint32_t elapsed = 0;
    };
    std::map<std::uint64_t, GrowthWindow> growth_windows;
    // Living integer paths replay bounded single ticks with current constant
    // external inputs. Source AND recipient must be eligible at the substep.
    std::uint32_t max_window = 0;
    for (const auto& [key, k] : windows)
        max_window = std::max(max_window, k);
    const Vec2 wind =
        m_windFieldSystem ? m_windFieldSystem->SampleWind(m_metadata.spawnPoint) : Vec2(0);
    fol::FoliageInteriorReplay interior_replay(m_fieldPages, max_window != 0);
    for (std::uint32_t step = 0; step < max_window; ++step) {
        const auto subtick = tick - max_window + step + 1;
        std::map<RegionKey, std::uint32_t> one_tick;
        for (const auto& [key, k] : windows)
            if (max_window - step <= k)
                one_tick[key] = 1;
        for (const auto& [key, updates] :
             interior_replay.Step(m_registry, m_fieldPages, one_tick, subtick, &budget))
            page_work[key] += updates;
        // Cache the current environment/infection once for this bounded window.
        // Interleave exact PlantCoarse single steps with the integer soil and
        // living steps so stage crossings cannot duplicate births or pollen events.
        std::vector<entt::entity> plants;
        for (auto e : m_registry.view<C::PlantTag, C::PlantRegionComponent>())
            plants.push_back(e);
        std::sort(plants.begin(), plants.end(), [this](auto a, auto b) {
            return m_registry.get<C::PlantRegionComponent>(a).persistent_id <
                   m_registry.get<C::PlantRegionComponent>(b).persistent_id;
        });
        budget.Next(Stage::Plants, [this, &plants, &one_tick, subtick] {
            return static_cast<std::uint64_t>(std::count_if(
                plants.begin(), plants.end(), [this, &one_tick, subtick](entt::entity e) {
                    const auto& owner = m_registry.get<C::PlantRegionComponent>(e);
                    const auto& growth = m_registry.get<C::PlantGrowthComponent>(e);
                    return one_tick.contains({owner.owner_x, owner.owner_z}) &&
                           owner.region_stamp < subtick && growth.planted_tick < subtick;
                }));
        });
        for (auto e : plants) {
            auto& owner = m_registry.get<C::PlantRegionComponent>(e);
            const RegionKey key{owner.owner_x, owner.owner_z};
            const auto window = windows.find(key);
            if (window == windows.end() || !one_tick.contains(key))
                continue;
            auto& g = m_registry.get<C::PlantGrowthComponent>(e);
            auto it = growth_windows.find(owner.persistent_id);
            if (it == growth_windows.end()) {
                const auto* health = m_registry.try_get<C::PlantHealthComponent>(e);
                const auto elapsed = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(window->second, tick - owner.region_stamp));
                it = growth_windows
                         .emplace(owner.persistent_id,
                                  GrowthWindow{.environment = SamplePlantEnvironment(
                                                   m_registry.get<C::TransformComponent>(e), tick),
                                               .infection = static_cast<std::uint16_t>(
                                                   health ? health->infection : 0),
                                               .elapsed = elapsed})
                         .first;
            }
            if (max_window - step > it->second.elapsed || g.planted_tick >= subtick)
                continue;
            fol::AdvancePlantGrowthCoarse(g,
                                          m_registry.get<C::PlantGenomeComponent>(e),
                                          it->second.environment,
                                          it->second.infection,
                                          1,
                                          subtick);
            RecordActiveRegionWork({key, 0, 1, 0, 0});
            completed_regions.insert(key);
        }
        const RegionEntityGate gate = [this, &windows, step, max_window, subtick](entt::entity e) {
            const auto* tf = m_registry.try_get<C::TransformComponent>(e);
            if (!tf || !ActiveRegionLedger::valid_anchor(tf->position))
                return false;
            if (const auto* owner = m_registry.try_get<C::PlantRegionComponent>(e)) {
                const auto& growth = m_registry.get<C::PlantGrowthComponent>(e);
                if (owner->region_stamp >= subtick && growth.planted_tick != subtick)
                    return false;
            }
            const auto it = windows.find(ActiveRegionLedger::region_at(tf->position));
            return it != windows.end() && max_window - step <= it->second;
        };
        const RegionEntityWork completed = [this, &completed_regions](entt::entity e) {
            const auto& tf = m_registry.get<C::TransformComponent>(e);
            const auto key = ActiveRegionLedger::region_at(tf.position);
            RecordActiveRegionWork({key, 0, 1, 0, 0});
            completed_regions.insert(key);
        };
        budget.Next(Stage::Pollination, [this, &gate] {
            return RegionalParticipantCount<C::PollinationTag,
                                            C::PlantGenomeComponent,
                                            C::PlantGrowthComponent,
                                            C::TransformComponent,
                                            C::PollinationComponent>(m_registry, gate);
        });
        fol::RunPollinationOnTick(
            m_registry, subtick, wind, 0, m_plantMutationRate, gate, completed);
        budget.Next(Stage::Disease, [this, &gate] {
            return RegionalParticipantCount<C::PlantTag,
                                            C::PlantHealthComponent,
                                            C::TransformComponent>(m_registry, gate);
        });
        fol::RunPlantDiseaseOnTick(m_registry, subtick, 0, gate, completed);
        budget.Next(Stage::Crops, [this, &gate] {
            return RegionalParticipantCount<C::CropLifecycleComponent,
                                            C::PlantGrowthComponent,
                                            C::PlantGenomeComponent,
                                            C::TransformComponent>(m_registry, gate);
        });
        fol::RunCropLifecycleOnTick(
            m_registry,
            subtick,
            0,
            gate,
            completed,
            [this](const Vec3& pos) {
                if (!ActiveRegionLedger::valid_anchor(pos))
                    return false;
                const auto it =
                    m_activeRegionLedger.records().find(ActiveRegionLedger::region_at(pos));
                return it == m_activeRegionLedger.records().end() ||
                       it->second.state != RegionState::Frozen;
            },
            [this, &completed_regions, &interior_replay](entt::entity child) {
                if (!AdmitPlantAt(child,
                                  m_registry.get<C::PlantGrowthComponent>(child).last_tick)) {
                    m_registry.destroy(child);
                    throw std::runtime_error("Crop identity admission refused");
                }
                const auto& owner = m_registry.get<C::PlantRegionComponent>(child);
                interior_replay.AdmitSoil(
                    m_fieldPages,
                    fol::FoliageCell(m_registry.get<C::TransformComponent>(child).position));
                const RegionKey key{owner.owner_x, owner.owner_z};
                RecordActiveRegionWork({key, 0, 1, 0, 0});
                completed_regions.insert(key);
            });
        budget.Next(Stage::Fire, [this, &gate] {
            return RegionalParticipantCount<C::CombustibleComponent, C::TransformComponent>(
                m_registry, gate);
        });
        std::vector<luminumbra::sim::FireIgnitionSource> ignitions;
        if (step + 1 == max_window && m_weatherSystem)
            for (const auto& strike : m_weatherSystem->StrikesThisTick()) {
                const Vec3 pos(strike.world_x, 0, strike.world_z);
                if (!ActiveRegionLedger::valid_anchor(pos))
                    continue;
                const auto it = windows.find(ActiveRegionLedger::region_at(pos));
                if (it != windows.end())
                    ignitions.push_back({Vec2(strike.world_x, strike.world_z),
                                         3 + strike.magnitude * 5,
                                         std::max(0.25f, strike.magnitude)});
            }
        luminumbra::sim::RunFireSpreadOnTick(m_registry, subtick, wind, ignitions, gate, completed);
        budget.Next(Stage::Grazing, [this, &gate] {
            return RegionalParticipantCount<C::GrazeableComponent, C::TransformComponent>(
                m_registry, gate);
        });
        luminumbra::ai::RunWildlifeFoliageOnTick(
            m_registry, subtick, m_wildlifeFoliageTuning, gate, completed);
        budget.Next(Stage::Lifespan, [this, &gate] {
            return RegionalParticipantCount<C::MortalComponent>(m_registry, gate);
        });
        luminumbra::ai::RunLifespanOnTick(m_registry, subtick, gate, completed);
    }
    budget.Next(Stage::Plants, [] { return 0u; });
    for (const auto& [key, updates] : page_work)
        RecordActiveRegionWork({key, 0, 0, 0, updates});
    budget.Next(Stage::Irrigation, [] { return 0u; });
    for (const auto& [key, updates] :
         fol::ExchangeFoliageBorders(m_fieldPages, previous, m_activeRegionLedger.records(), tick))
        RecordActiveRegionWork({key, 0, 0, 0, updates});
    budget.Next(Stage::Plants, [] { return 0u; });
    for (auto e :
         m_registry.view<C::PlantTag, C::PlantRegionComponent, C::PlantGrowthComponent>()) {
        auto& owner = m_registry.get<C::PlantRegionComponent>(e);
        if (windows.contains(RegionKey{owner.owner_x, owner.owner_z})) {
            owner.region_stamp = tick;
            m_registry.get<C::PlantGrowthComponent>(e).last_tick = tick;
        }
    }
    // All resumed plants (including births) have coherent stamps before clearing
    // the durable one-shot token. Exceptions keep it pending and fail-stop.
    for (const auto& [key, frozen_at] : resumes) {
        if (!m_activeRegionLedger.acknowledge_resume(key, frozen_at))
            throw std::runtime_error("Plant resume acknowledgement refused");
    }
    for (const auto& [key, updates] : page_work)
        if (updates)
            completed_regions.insert(key);
    for (const auto key : completed_regions)
        RecordActiveRegionWork({key, 1, 0, 0, 0});
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
                        Persistence::RegionDigestInputs{
                            .live = m_worldSystem->snapshot_streamed_chunks(),
                            .parked = m_worldSystem->snapshot_parked_chunks(),
                            .durable = m_worldSystem->durable_chunk_inventory(),
                            .ground_object_bytes =
                                m_groundObjects.region_bytes(key) + PlantRegionProjection(key),
                            .field_page_bytes = RegionalFieldProjection(key)});
                },
                // Plants, foliage pages and water debt retain their owners alongside fields,
                // ground objects, pending work and authoritative chunks.
                [this, &owned_regions](RegionKey key) {
                    if (m_activeRegionLedger.water_debts().contains(key))
                        return true;
                    if (m_groundObjects.owns_region(key))
                        return true;
                    if (m_fieldPages.owns_region(key))
                        return true;
                    if (m_activeRegionLedger.energy_ages().contains(key) ||
                        (m_energyFieldState && m_energyFieldState->OwnsRegion(key)))
                        return true;
                    if (!owned_regions) {
                        owned_regions.emplace();
                        for (auto e : m_registry.view<const Components::PlantRegionComponent>()) {
                            const auto& owner =
                                m_registry.get<const Components::PlantRegionComponent>(e);
                            owned_regions.value().insert({owner.owner_x, owner.owner_z});
                        }
                        for (const auto& entry : m_regionWork)
                            owned_regions.value().insert(entry.key);
                        for (const auto& coords : m_worldSystem->durable_chunk_inventory()) {
                            int rx = 0, rz = 0;
                            Persistence::WorldSaveService::region_coords_for_chunk(coords, rx, rz);
                            owned_regions.value().insert(RegionKey{rx, rz});
                        }
                        for (const auto& chunk : m_worldSystem->snapshot_authoritative_chunks()) {
                            if (!chunk)
                                continue;
                            int rx = 0, rz = 0;
                            Persistence::WorldSaveService::region_coords_for_chunk(
                                chunk->get_coords(), rx, rz);
                            owned_regions.value().insert(RegionKey{rx, rz});
                        }
                    }
                    return owned_regions.value().contains(key);
                },
                &m_regionScheduleScratch);
            m_regionWork.clear();
        } catch (const std::exception& error) {
            LatchRegionFault(current_tick, error.what());
        }
    }
}

const std::vector<RegionKey>& GameSession::SortedDueRegions() const {
    // The schedule's own order is rank/proximity order and must not change; callers age in key
    // order.
    auto& due = m_sortedDueRegions;
    if (m_sortedDueSource != m_regionSchedule.due) {
        due.assign(m_regionSchedule.due.begin(), m_regionSchedule.due.end());
        std::sort(due.begin(), due.end());
        due.erase(std::unique(due.begin(), due.end()), due.end());
        m_sortedDueSource = m_regionSchedule.due;
    }
    return due;
}

void GameSession::AdvanceFieldPages(std::uint64_t tick) {
    const std::uint64_t seed =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(StringToSeed(m_metadata.seed)));
    const auto& records = m_activeRegionLedger.records();
    const auto& due = SortedDueRegions();
    m_fieldPages.scheduled_snapshot_into(
        records, due, tick, m_previousFieldPages, m_fieldScheduleScratch);
    const auto& previous = m_previousFieldPages;
    for (const RegionKey key : due) {
        if (records.find(key) == records.end())
            continue;
        if (!m_fieldPages.activate_region(key, seed, tick))
            ++m_regionInputRejections.field_pages;
        const auto current_count = m_fieldPages.owned_initialized_cells(key);
        const auto previous_count = previous.owned_initialized_cells(key);
        if (current_count < previous_count)
            throw std::logic_error("Field page initialization count decreased during admission");
        const auto initialized = current_count - previous_count;
        if (initialized)
            m_regionCompletedWork.push_back(RegionWork{key, 0, 0, 0, initialized});
    }
    for (const auto& [key, updates] :
         m_fieldPages.step_scheduled(records, due, seed, tick, &previous, &m_fieldScheduleScratch))
        m_regionCompletedWork.push_back(RegionWork{key, 0, 0, 0, updates});
}

std::uint16_t GameSession::SampleRegionalChannel(int channel, const Vec3& position) const {
    if (!std::isfinite(position.x) || !std::isfinite(position.z) || position.x < -16777216.0f ||
        position.x >= 16777216.0f || position.z < -16777216.0f || position.z >= 16777216.0f)
        return 0;
    const auto x = static_cast<std::int32_t>(std::floor(static_cast<double>(position.x) / 24.0));
    const auto z = static_cast<std::int32_t>(std::floor(static_cast<double>(position.z) / 24.0));
    const auto owner = FieldPageState::region_of_cell(x, z);
    const auto found = m_activeRegionLedger.records().find(owner);
    std::uint16_t value = 0;
    if (found != m_activeRegionLedger.records().end() &&
        found->second.state != RegionState::Frozen && m_fieldPages.sample(channel, x, z, value))
        return value;
    const auto seed =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(StringToSeed(m_metadata.seed)));
    return FieldPageState::seed_value(
        seed, channel, x, z, m_worldClock.tick() / FieldPageState::kCadenceTicks);
}

std::string GameSession::RegionalAmbientHash(std::uint8_t channels, bool weather) const {
    auto bytes = m_fieldPages.ambient_bytes(channels, weather);
    // The same regional authority can be projected at different anchors. The anchor
    // drives ambient diagnostics and the scent domain, so include its identity in
    // the slot hash without changing the stored region/freeze digest projection.
    bytes += "|ambient_anchor:";
    for (const float coordinate : {m_ambientFieldAnchor.x, m_ambientFieldAnchor.z}) {
        const auto bits = std::bit_cast<std::uint32_t>(coordinate);
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<char>((bits >> (8 * i)) & 255));
    }
    return Persistence::StableChecksum(bytes);
}

void GameSession::BindRegionalFieldQueries() {
    if (!m_activeRegionsEnabled)
        return;
    m_windFieldSystem->BindRegionalBackend(
        [this](const Vec3& position, Systems::WindLayer layer) {
            // Stored signed words are 1/1024 m/s. Higher layers use exact powers of two.
            const float scale = layer == Systems::WindLayer::Ground ? 1.0f
                                : layer == Systems::WindLayer::Mid  ? 2.0f
                                                                    : 4.0f;
            return Vec2(static_cast<float>(
                            std::bit_cast<std::int16_t>(SampleRegionalChannel(0, position))) *
                            (scale / 1024.0f),
                        static_cast<float>(
                            std::bit_cast<std::int16_t>(SampleRegionalChannel(1, position))) *
                            (scale / 1024.0f));
        },
        [this] { return RegionalAmbientHash(3, false); });
    m_weatherSystem->BindRegionalBackend(
        [this](const Vec3& position) {
            Systems::WeatherSample sample;
            const auto moisture = SampleRegionalChannel(2, position);
            const auto front = SampleRegionalChannel(3, position);
            sample.category = front < 13107   ? Systems::WeatherCategory::Clear
                              : front < 26214 ? Systems::WeatherCategory::Overcast
                              : front < 39321 ? Systems::WeatherCategory::Rain
                              : front < 52428 ? Systems::WeatherCategory::Snow
                                              : Systems::WeatherCategory::Fog;
            sample.precip_intensity = sample.category == Systems::WeatherCategory::Rain ||
                                              sample.category == Systems::WeatherCategory::Snow
                                          ? static_cast<float>(moisture) / 65535.0f
                                          : 0.0f;
            sample.wind = m_windFieldSystem->SampleWind(position);
            return sample;
        },
        [this] { return RegionalAmbientHash(12, true); });
    m_aetherFieldSystem->BindRegionalBackend(
        [this](const Vec3& position) {
            return static_cast<float>(SampleRegionalChannel(4, position)) / 65535.0f;
        },
        [this] { return RegionalAmbientHash(16, false); });
}

std::string GameSession::RegionalFieldProjection(RegionKey key) const {
    auto bytes = m_fieldPages.region_bytes(key);
    if (m_energyFieldState)
        bytes += m_energyFieldState->RegionBytes(key);
    const auto debt = m_activeRegionLedger.water_debts().find(key);
    if (debt != m_activeRegionLedger.water_debts().end()) {
        bytes += "|water_debt:";
        for (int i = 0; i < 8; ++i)
            bytes.push_back(static_cast<char>((debt->second >> (8 * i)) & 255));
    }
    const auto age = m_activeRegionLedger.energy_ages().find(key);
    if (age != m_activeRegionLedger.energy_ages().end()) {
        bytes += "|energy_age:";
        for (int i = 0; i < 8; ++i)
            bytes.push_back(static_cast<char>((age->second >> (8 * i)) & 255));
    }
    return bytes;
}
void GameSession::ProjectRegionalWeather(bool restoring) {
    if (!m_activeRegionsEnabled || !m_weatherSystem)
        return;
    std::vector<Systems::StormCell> storms;
    for (const auto& storm : m_fieldPages.storms())
        storms.push_back(
            {Vec2(std::bit_cast<float>(storm.center_x), std::bit_cast<float>(storm.center_z)),
             Vec2(std::bit_cast<float>(storm.velocity_x), std::bit_cast<float>(storm.velocity_z)),
             std::bit_cast<float>(storm.intensity),
             storm.spawn_tick,
             storm.lifetime_ticks,
             storm.seed_salt});
    std::vector<Systems::StrikeEvent> strikes;
    for (const auto& strike : m_fieldPages.strikes()) {
        if (restoring && strike.strike_tick <= m_worldClock.tick())
            continue;
        strikes.push_back({strike.strike_tick,
                           std::bit_cast<float>(strike.world_x),
                           std::bit_cast<float>(strike.world_z),
                           std::bit_cast<float>(strike.magnitude),
                           strike.storm_salt});
    }
    m_weatherSystem->AdoptRegionalWeather(
        m_worldClock.tick(), m_ambientFieldAnchor, std::move(storms), std::move(strikes));
}

void GameSession::AdvanceRegionalWeather(std::uint64_t tick) {
    // Weather admission is transactional without copying pages or the dense scent map.
    auto clocks = m_fieldPages.weather_clocks();
    const auto& records = m_activeRegionLedger.records();
    const auto& due_list = SortedDueRegions();
    const std::set<RegionKey> due(due_list.begin(), due_list.end());
    const auto eligible = [&records](RegionKey owner) {
        const auto found = records.find(owner);
        return found != records.end() && found->second.state != RegionState::Frozen;
    };
    const auto common = [&records, &eligible, tick](RegionKey source, RegionKey destination) {
        if (!eligible(source) || !eligible(destination))
            return false;
        const auto shift =
            std::max(records.at(source).cadence_shift, records.at(destination).cadence_shift);
        return shift < 63 && (tick & ((std::uint64_t{1} << shift) - 1)) == 0;
    };
    const auto owner_of = [](float x, float z) {
        return RegionKey{static_cast<std::int32_t>(std::floor(static_cast<double>(x) / 512.0)),
                         static_cast<std::int32_t>(std::floor(static_cast<double>(z) / 512.0))};
    };
    const auto mix = [](std::uint64_t value) {
        value += 0x9e3779b97f4a7c15ull;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31);
    };
    const auto unit = [&mix](std::uint64_t key) {
        return static_cast<float>(mix(key) >> 40) / 16777216.0f;
    };
    const auto seed =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(StringToSeed(m_metadata.seed)));
    std::map<RegionKey, std::uint64_t> work;
    for (const auto owner : due_list) {
        const auto found = clocks.find(owner);
        const auto age = found == clocks.end() ? 0 : found->second;
        if (age + 1 >= WorldClock::kTickLimit ||
            (found == clocks.end() && clocks.size() == FieldPageState::kMaxWeatherClocks))
            throw std::overflow_error("Regional weather clock admission failed");
        clocks[owner] = age + 1;
        ++work[owner];
    }
    std::vector<RegionalStrike> strikes;
    // Same-tick events have already been delivered at the saved boundary. Frozen strikes stay
    // as stored and never re-emit because their absolute provenance tick is immutable.
    for (const auto& strike : m_fieldPages.strikes())
        if (!due.contains(strike.owner))
            strikes.push_back(strike);
        else
            ++work[strike.owner];
    std::vector<RegionalStorm> storms;
    for (auto storm : m_fieldPages.storms()) {
        const auto source = storm.owner;
        if (!eligible(source)) {
            storms.push_back(storm);
            continue;
        }
        const bool interior_due = due.contains(source);
        float x = std::bit_cast<float>(storm.center_x), z = std::bit_cast<float>(storm.center_z);
        float vx = std::bit_cast<float>(storm.velocity_x),
              vz = std::bit_cast<float>(storm.velocity_z);
        if (interior_due) {
            ++storm.active_age;
            ++work[source];
            if (storm.active_age >= storm.lifetime_ticks)
                continue;
            const auto wind =
                m_windFieldSystem->SampleWind(Vec3{x, 5, z}, Systems::WindLayer::Ground);
            vx = wind.x;
            vz = wind.y;
            storm.velocity_x = std::bit_cast<std::uint32_t>(vx);
            storm.velocity_z = std::bit_cast<std::uint32_t>(vz);
            const auto remaining = storm.lifetime_ticks - storm.active_age;
            const float intensity =
                std::min(1.0f, static_cast<float>(std::min(storm.active_age, remaining)) / 45.0f);
            storm.intensity = std::bit_cast<std::uint32_t>(intensity);
        }
        float px = x + vx * (1.0f / 30.0f), pz = z + vz * (1.0f / 30.0f);
        // Discard blocked displacement; no hidden accumulated motion. World limits are sealed.
        const bool valid_position = std::isfinite(px) && std::isfinite(pz) && px >= -16777216.0f &&
                                    px < 16777216.0f && pz >= -16777216.0f && pz < 16777216.0f;
        const auto destination = valid_position ? owner_of(px, pz) : source;
        bool transfer = valid_position && destination != source && common(source, destination);
        if (transfer && destination.x != source.x && destination.z != source.z)
            transfer = common(source, {destination.x, source.z}) &&
                       common(source, {source.x, destination.z});
        if (transfer) {
            storm.owner = destination;
            ++work[source];
            ++work[destination];
        } else if (interior_due) {
            const float low_x = static_cast<float>(source.x) * 512.0f;
            const float low_z = static_cast<float>(source.z) * 512.0f;
            px = valid_position ? std::clamp(px, low_x, std::nextafter(low_x + 512.0f, low_x)) : x;
            pz = valid_position ? std::clamp(pz, low_z, std::nextafter(low_z + 512.0f, low_z)) : z;
        } else {
            px = x;
            pz = z;
        }
        storm.center_x = std::bit_cast<std::uint32_t>(px);
        storm.center_z = std::bit_cast<std::uint32_t>(pz);
        // Strike cadence follows the storm's own active age, not elapsed absolute ticks.
        const float intensity = std::bit_cast<float>(storm.intensity);
        if (interior_due && storm.active_age % 18 == 0 && intensity >= 0.45f) {
            auto key = mix((seed + 40) ^ 0x535452494b455300ull ^
                           (static_cast<std::uint64_t>(storm.seed_salt) * 0x9e3779b97f4a7c15ull));
            key = mix(key ^ ((storm.active_age / 18) * 0xff51afd7ed558ccdull));
            if (unit(key) < 0.4f * intensity) {
                const float sx = px + (unit(key ^ 0x1111222233334444ull) * 2.0f - 1.0f) * 180.0f;
                const float sz = pz + (unit(key ^ 0x5555666677778888ull) * 2.0f - 1.0f) * 180.0f;
                if (std::isfinite(sx) && std::isfinite(sz) && sx >= -16777216.0f &&
                    sx < 16777216.0f && sz >= -16777216.0f && sz < 16777216.0f) {
                    const auto target = owner_of(sx, sz);
                    if (eligible(target) &&
                        (target == storm.owner || common(storm.owner, target))) {
                        const float magnitude = std::min(
                            1.0f, 0.55f * intensity + 0.45f * unit(key ^ 0x99990000aaaabbbbull));
                        if (strikes.size() == FieldPageState::kMaxStrikes)
                            throw std::runtime_error("Regional strike capacity exhausted");
                        strikes.push_back({target,
                                           tick,
                                           std::bit_cast<std::uint32_t>(sx),
                                           std::bit_cast<std::uint32_t>(sz),
                                           std::bit_cast<std::uint32_t>(magnitude),
                                           storm.seed_salt,
                                           0});
                        ++work[storm.owner];
                        if (target != storm.owner)
                            ++work[target];
                    }
                }
            }
        }
        storms.push_back(storm);
    }
    for (const auto owner : due_list) {
        const auto age = clocks.at(owner);
        if (age % 45 != 0)
            continue;
        auto key = mix((seed + 40) ^ 0x535041574e000000ull ^ ((age / 45) * 0x100000001b3ull));
        key ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(owner.x)) * 0x9e3779b1u;
        key ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(owner.z)) * 0x85ebca77u;
        if (unit(key) >= 0.55f)
            continue;
        const float x = static_cast<float>(owner.x) * 512.0f + 256.0f +
                        (unit(key ^ 0xaaaa5555ull) * 2.0f - 1.0f) * 204.8f;
        const float z = static_cast<float>(owner.z) * 512.0f + 256.0f +
                        (unit(key ^ 0x5555aaaaull) * 2.0f - 1.0f) * 204.8f;
        const auto wind = m_windFieldSystem->SampleWind(Vec3{x, 5, z}, Systems::WindLayer::Ground);
        const auto salt = static_cast<std::uint32_t>(mix(key ^ 0xc0ffeeull));
        if (std::any_of(storms.begin(), storms.end(), [tick, salt](const auto& storm) {
                return storm.spawn_tick == tick && storm.seed_salt == salt;
            }))
            continue;
        if (storms.size() == FieldPageState::kMaxStorms)
            throw std::runtime_error("Regional storm capacity exhausted");
        storms.push_back({owner,
                          std::bit_cast<std::uint32_t>(x),
                          std::bit_cast<std::uint32_t>(z),
                          std::bit_cast<std::uint32_t>(wind.x),
                          std::bit_cast<std::uint32_t>(wind.y),
                          0,
                          tick,
                          240,
                          salt,
                          0});
        ++work[owner];
    }
    if (!m_fieldPages.replace_weather(std::move(storms), std::move(strikes), std::move(clocks)))
        throw std::runtime_error("Regional weather state admission failed");
    ProjectRegionalWeather();
    for (const auto& [owner, updates] : work)
        RecordActiveRegionWork({owner, 0, 0, 0, updates});
}

void GameSession::SetRegionSchedulerConfig(RegionSchedulerConfig config) {
    if (m_worldSystem)
        throw std::logic_error("Set region scheduler configuration before creating a world");
    m_activeRegionLedger = ActiveRegionLedger(config);
    m_groundObjects.clear();
    m_fieldPages.clear();
    m_regionSchedulerConfig = config;
}
void GameSession::LatchRegionFault(std::uint64_t tick, std::string message) {
    if (m_regionFault)
        return; // first fault wins
    m_regionFault = RegionSchedulerFault{tick, message};
    m_simulationClock.reset(tick);
    m_activeRegionLedger.acknowledge_world_clock(m_worldClock);
    m_activeRegionLedger.clear_completed_work();
    m_regionCompletedWork.clear();
    m_regionWork.clear();
    // A fault can stop the tick before the ambient fields run. These fields are
    // reconstructed from the saved clock on load, so align them with that same
    // clock now without running any consumers or discarding stateful authority.
    RestoreAmbientFieldsAtTick(tick);
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
    if (m_activeRegionsEnabled && !m_regionFault) {
        if (m_simulationBatchInProgress)
            m_regionCompletedWork.push_back(work);
        else
            m_regionWork.push_back(work);
    }
}

namespace {

std::string GroundEventPayload(const GroundObject& object) {
    return std::to_string(object.id) + "," + std::to_string(object.type) + "," +
           std::to_string(object.x_mm) + "," + std::to_string(object.y_mm) + "," +
           std::to_string(object.z_mm);
}

// Floor division for a positive divisor: negative quotients round toward minus infinity.
std::int64_t FloorDivide(std::int64_t value, std::int64_t divisor) {
    const std::int64_t quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

// (chunk x, chunk z) of every streamed chunk that is not still loading.
std::set<std::pair<int, int>>
ResidentChunkColumns(const std::vector<std::shared_ptr<Luminumbra::Chunk>>& chunks) {
    std::set<std::pair<int, int>> columns;
    for (const auto& chunk : chunks) {
        if (!chunk || chunk->get_state() == ChunkState::Loading)
            continue;
        columns.emplace(chunk->get_coords().x, chunk->get_coords().z);
    }
    return columns;
}

// Settles an unsettled object with an owner-thread downward terrain probe.
// Returns false, leaving the object unchanged, when its chunk column is not resident or the
// height is not finite.
bool SettleGroundObjectOnTerrain(GroundObject& object,
                                 const std::set<std::pair<int, int>>& columns,
                                 const Systems::SHIELD_WorldSystem& world) {
    if (object.settled())
        return true;
    const std::int64_t column_x_mm = static_cast<std::int64_t>(CHUNK_SIZE_X) * 1000;
    const std::int64_t column_z_mm = static_cast<std::int64_t>(CHUNK_SIZE_Z) * 1000;
    const std::pair<int, int> column{static_cast<int>(FloorDivide(object.x_mm, column_x_mm)),
                                     static_cast<int>(FloorDivide(object.z_mm, column_z_mm))};
    if (!columns.contains(column))
        return false;
    const auto height = world.ProbeTerrainBelow({static_cast<float>(object.x_mm) / 1000.0f,
                                                 static_cast<float>(object.y_mm) / 1000.0f,
                                                 static_cast<float>(object.z_mm) / 1000.0f});
    if (!height || !std::isfinite(*height))
        return false;
    const double y_mm = static_cast<double>(*height) * 1000.0;
    if (std::abs(y_mm) >= static_cast<double>(GroundObjectStore::kMaxVerticalMm))
        return false;
    object.y_mm = std::llround(y_mm);
    object.flags = static_cast<std::uint16_t>(object.flags | GroundObject::kSettled);
    return true;
}

} // namespace

std::uint64_t GameSession::SpawnGroundObject(std::uint32_t type,
                                             std::int64_t x_mm,
                                             std::int64_t y_mm,
                                             std::int64_t z_mm,
                                             std::uint64_t despawn_after,
                                             std::uint32_t payload) {
    if (!m_activeRegionsEnabled)
        return 0;
    if (despawn_after > std::numeric_limits<std::uint32_t>::max()) {
        ++m_regionInputRejections.ground_spawns;
        return 0;
    }
    const std::uint64_t tick = m_worldClock.tick();
    // Probe candidate: the real id is allocated only once the candidate is accepted.
    GroundObject candidate;
    candidate.id = 1;
    candidate.type = type;
    candidate.x_mm = x_mm;
    candidate.y_mm = y_mm;
    candidate.z_mm = z_mm;
    candidate.spawn_tick = tick;
    candidate.despawn_after = static_cast<std::uint32_t>(despawn_after);
    candidate.payload = payload;
    if (!m_worldSystem || m_regionFault.has_value() ||
        m_groundObjects.size() >= GroundObjectStore::kMaxObjects ||
        !GroundObjectStore::valid(candidate)) {
        ++m_regionInputRejections.ground_spawns;
        return 0;
    }
    std::uint64_t id = 0;
    if (!m_activeRegionLedger.allocate_persistent_id(id)) {
        LatchRegionFault(tick, "persistent id space exhausted");
        return 0;
    }
    // A failure from here on still consumes the id.
    candidate.id = id;
    SettleGroundObjectOnTerrain(
        candidate, ResidentChunkColumns(m_worldSystem->snapshot_streamed_chunks()), *m_worldSystem);
    if (!m_groundObjects.insert(candidate))
        return 0;
    if (!MarkGroundRegionEdited(candidate)) {
        m_groundObjects.erase(id);
        return 0;
    }
    m_simulationEventBus.publish(tick, "ground.spawned", GroundEventPayload(candidate));
    return id;
}

std::uint64_t GameSession::SpawnGroundObject(std::uint32_t type,
                                             Vec3 position,
                                             std::uint32_t despawn_after,
                                             std::uint32_t payload) {
    if (!m_activeRegionsEnabled)
        return 0;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        std::abs(static_cast<double>(position.x)) >= 16777216.0 ||
        std::abs(static_cast<double>(position.z)) >= 16777216.0 ||
        std::abs(static_cast<double>(position.y)) >= 1048576.0) {
        ++m_regionInputRejections.ground_spawns;
        return 0;
    }
    return SpawnGroundObject(type,
                             std::llround(static_cast<double>(position.x) * 1000.0),
                             std::llround(static_cast<double>(position.y) * 1000.0),
                             std::llround(static_cast<double>(position.z) * 1000.0),
                             despawn_after,
                             payload);
}

bool GameSession::MarkGroundRegionEdited(const GroundObject& object) {
    try {
        m_activeRegionLedger.mark_edited(GroundObjectStore::region_of(object), m_worldClock);
        return true;
    } catch (const std::length_error& error) {
        LatchRegionFault(m_worldClock.tick(), error.what());
    } catch (const std::invalid_argument&) {
        ++m_regionInputRejections.edits;
    }
    return false;
}

void GameSession::AdvanceGroundObjects(std::uint64_t tick) {
    const auto& due = SortedDueRegions();
    // Resident chunk columns, built on the first unsettled object of this call.
    std::optional<std::set<std::pair<int, int>>> columns;
    const auto settle = [this, &columns](GroundObject& object) {
        if (!columns.has_value())
            columns = ResidentChunkColumns(m_worldSystem->snapshot_streamed_chunks());
        return SettleGroundObjectOnTerrain(object, columns.value(), *m_worldSystem);
    };
    for (const RegionKey key : due) {
        if (!m_groundObjects.owns_region(key))
            continue;
        const auto processed = static_cast<std::uint64_t>(m_groundObjects.in_region(key).size());
        const GroundObjectStore::AgeResult result = m_groundObjects.age_region(key, settle);
        m_regionCompletedWork.push_back(RegionWork{key, 0, processed, 0, 0});
        for (const GroundObject& object : result.despawned)
            m_simulationEventBus.publish(tick, "ground.despawned", GroundEventPayload(object));
    }
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
            [this](std::string error) { LatchRegionFault(m_worldClock.tick(), std::move(error)); });
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
        return Persistence::ComputeWorldStreamingStateHash(state);
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
    std::string projection = "authoritative_chunks:v2:";
    try {
        if (!durable.empty() && !Persistence::snapshot_index_exists(directory))
            throw std::runtime_error("inconsistent snapshot: index missing");
        for (const auto key : keys) {
            const auto record = m_activeRegionLedger.records().find(key);
            const auto digest = record != m_activeRegionLedger.records().end() &&
                                        record->second.state == RegionState::Frozen
                                    ? record->second.frozen_digest
                                    : Persistence::WorldSaveService::region_simulation_digest(
                                          directory,
                                          key,
                                          Persistence::RegionDigestInputs{.live = live,
                                                                          .parked = parked,
                                                                          .durable = durable,
                                                                          .ground_object_bytes = {},
                                                                          .field_page_bytes = {}});
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
                                             m_activeRegionLedger.canonical_bytes() +
                                             m_groundObjects.canonical_bytes() +
                                             m_fieldPages.canonical_bytes())
               : ecology_hash;
}

void GameSession::RestoreWorldClock(const WorldClock& clock) {
    m_worldClock = clock;
    m_simulationClock.reset(clock.tick());
    if (m_energyFieldState)
        m_energyFieldState->ResetEmptyAtTick(clock.tick());
    RestoreAmbientFieldsAtTick(clock.tick());
}

void GameSession::RestoreAmbientFieldsAtTick(std::uint64_t tick) {
    if (m_activeRegionsEnabled) {
        // Rebuild projections from authority without advancing regional ages or consumers.
        // Regional RestoreAtTick only stamps weather's tick; it does not update wind.
        if (m_windFieldSystem)
            m_windFieldSystem->Update(tick, m_ambientFieldAnchor);
        ProjectRegionalWeather(true);
    }
    if (m_weatherSystem && m_windFieldSystem)
        m_weatherSystem->RestoreAtTick(tick, m_ambientFieldAnchor, *m_windFieldSystem);
    if (m_aetherFieldSystem)
        m_aetherFieldSystem->Update(tick, m_ambientFieldAnchor, m_windFieldSystem.get());
    if (m_activeRegionsEnabled)
        RestoreRegionalScent();
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

bool GameSession::RunInSnapshotTransaction(const std::function<bool()>& body) {
    const Persistence::ChunkValidationCache::Scope validation_scope(m_chunkValidationCache);
    const fs::path save_dir = GetWorldSaveDir();
    if (!m_activeRegionsEnabled || save_dir.empty() || m_transientWorld ||
        m_metadata.worldId.empty() || !m_worldOpenError.empty() || !IsSimulationTickBoundary() ||
        !ClockConfigurationCompatible(save_dir) ||
        Persistence::SaveTransaction::active_for(save_dir)) {
        return body(); // nothing to wrap: the writers keep their own commits
    }
    auto outer = StartOuterSave(save_dir, m_worldClock.tick(), m_activeRegionLedger.local_anchor());
    if (!outer)
        return false;
    if (!body()) {
        RecordPersistenceSaveOutcome(false, 0);
        return false; // the destructor rolls the whole snapshot back
    }
    const bool committed = outer->Commit();
    if (!committed)
        RecordPersistenceSaveOutcome(false, 0);
    return committed;
}

bool GameSession::SaveWorld() {
    return SaveWorldMetadataTo(GetWorldSaveDir());
}

bool GameSession::SaveWorldMetadataTo(const fs::path& save_dir) {
    const Persistence::ChunkValidationCache::Scope validation_scope(m_chunkValidationCache);
    if (!IsSimulationTickBoundary() || save_dir.empty() ||
        !ClockConfigurationCompatible(save_dir) || m_transientWorld || m_metadata.worldId.empty() ||
        !m_worldOpenError.empty())
        return false;
    std::optional<Ecs::EntityRegistrySnapshot> plants;
    if (m_activeRegionsEnabled) {
        try {
            if (!m_regionFault)
                PrepareRegionalFoliage();
            plants = luminumbra::foliage::BuildPlantEntitySnapshot(m_registry, true);
            std::string error;
            if (!luminumbra::foliage::ValidatePlantEntitySnapshot(
                    *plants, m_activeRegionLedger, m_groundObjects, error)) {
                LUMINUMBRA_CORE_ERROR("Plant save refused: {}", error);
                return false;
            }
        } catch (const std::exception& e) {
            LatchRegionFault(m_worldClock.tick(), e.what());
            return false;
        }
    }
    std::unique_ptr<OuterSaveTransaction> outer;
    if (m_activeRegionsEnabled && !Persistence::SaveTransaction::active_for(save_dir)) {
        outer = StartOuterSave(save_dir, m_worldClock.tick(), m_activeRegionLedger.local_anchor());
        if (!outer)
            return false;
    }
    // Using nlohmann::json for robust saving
    nlohmann::json metadata_json = {
        {"container_version", Persistence::WorldSaveService::kContainerVersion},
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

    if (m_activeRegionsEnabled) {
        m_worldClock.write_metadata(metadata_json);
        metadata_json["ambientFieldAnchor"] = {{"x", m_ambientFieldAnchor.x},
                                               {"y", m_ambientFieldAnchor.y},
                                               {"z", m_ambientFieldAnchor.z}};
        // Marks a directory that must carry a snapshot index (downgrade guard, S4). Written only
        // inside a snapshot transaction, so the marker and the index always appear together.
        metadata_json["snapshotFormat"] = 1;
    }
    if (m_activeRegionsEnabled) {
        // All production callers save synchronously on the host thread: client
        // world creation/quit/shutdown and server autosave/snapshot/shutdown.
        // Do not dispatch this pair to workers; the writer asserts non-overlap.
        // The object file joins the same transaction, so the id allocator and objects stay paired.
        if (!plants.has_value())
            return false;
        std::vector<std::string> plant_errors;
        if (!Persistence::WorldSaveService::save_plant_entities(*plants, save_dir, &plant_errors)) {
            for (const auto& error : plant_errors)
                LUMINUMBRA_CORE_ERROR("Plant save failed: {}", error);
            return false;
        }
        std::vector<std::string> ground_errors;
        if (!Persistence::WorldSaveService::save_ground_objects(
                m_groundObjects, save_dir, &ground_errors)) {
            for (const std::string& error : ground_errors)
                LUMINUMBRA_CORE_ERROR("Ground object save failed: {}", error);
            return false;
        }
        std::vector<std::string> page_errors;
        if (!Persistence::WorldSaveService::save_field_pages(
                m_fieldPages, save_dir, &page_errors)) {
            for (const std::string& error : page_errors)
                LUMINUMBRA_CORE_ERROR("Field page save failed: {}", error);
            return false;
        }
        if (!SaveEnergyFieldRecord(save_dir))
            return false;
        const auto expected_ledger = m_activeRegionLedger.canonical_bytes();
        auto saved_ledger = m_activeRegionLedger;
        saved_ledger.mark_saved(m_worldClock);
        if (!Persistence::WorldSaveService::save_metadata_and_active_regions(
                metadata_json.dump(4) + "\n", m_activeRegionLedger, save_dir))
            return false;
        if (auto* transaction = Persistence::SaveTransaction::active_for(save_dir))
            transaction->defer_until_commit(
                [this, expected_ledger, saved = std::move(saved_ledger)]() mutable {
                    if (m_activeRegionLedger.canonical_bytes() == expected_ledger)
                        m_activeRegionLedger = std::move(saved);
                });
        if (outer) {
            if (!outer->Commit())
                return false;
        }
    } else if (!Persistence::WorldSaveService::save_metadata(metadata_json.dump(4) + "\n",
                                                             save_dir)) {
        return false;
    }
    return true;
}

std::filesystem::path GameSession::GetWorldSaveDir() const {
    if (m_transientWorld || m_metadata.worldId.empty()) {
        return {};
    }
    return RuntimeRoot(m_rootPath) / "worlds" / "saves" / m_metadata.worldId;
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
                                   WorldStateSaveReport* report) {
    const Persistence::ChunkValidationCache::Scope validation_scope(m_chunkValidationCache);
    const auto save_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&save_started]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         save_started)
            .count();
    };
    WorldStateSaveReport result;
    if (report) {
        *report = result;
    }
    if (!IsSimulationTickBoundary() || !ClockConfigurationCompatible(save_dir) ||
        !m_worldOpenError.empty() || !m_worldSystem || save_dir.empty() ||
        (m_activeRegionsEnabled && (m_transientWorld || m_metadata.worldId.empty()))) {
        return false;
    }

    if (m_activeRegionsEnabled && !m_regionFault) {
        try {
            PrepareRegionalFoliage();
        } catch (const std::exception& e) {
            LatchRegionFault(m_worldClock.tick(), e.what());
            return false;
        }
    }

    // Flag on: one snapshot transaction covers the whole save (recovery, one validation, the
    // journal, and the destination directory, which Start creates only after every refusal check
    // passed). Flag off keeps today's per-writer validation.
    std::unique_ptr<OuterSaveTransaction> outer;
    if (m_activeRegionsEnabled) {
        // A caller-owned transaction (RunInSnapshotTransaction) already recovered, validated and
        // opened the journal; this save joins it and the owner commits.
        if (!Persistence::SaveTransaction::active_for(save_dir)) {
            outer =
                StartOuterSave(save_dir, m_worldClock.tick(), m_activeRegionLedger.local_anchor());
            if (!outer) {
                RecordPersistenceSaveOutcome(false, elapsed_ms());
                return false;
            }
        }
    } else if (!Persistence::WorldSaveService::validate_save(save_dir)) {
        RecordPersistenceSaveOutcome(false, elapsed_ms());
        return false;
    }

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

    std::error_code source_path_error, target_path_error;
    const auto source_directory = fs::weakly_canonical(m_regionDurableDirectory, source_path_error);
    const auto target_directory = fs::weakly_canonical(save_dir, target_path_error);
    if (m_activeRegionsEnabled && !m_regionDurableDirectory.empty() &&
        (source_path_error || target_path_error)) {
        RecordPersistenceSaveOutcome(false, elapsed_ms());
        return false;
    }
    if (m_activeRegionsEnabled && !m_regionDurableDirectory.empty() &&
        source_directory != target_directory) {
        // Save-as must transfer distant disk-only obligations before switching their backing
        // directory. Stream one chunk at a time; live/parked state supersedes the old copy.
        Persistence::WorldSaveService copier;
        std::vector<std::string> copy_errors;
        bool copied = true;
        for (const auto& coords : m_worldSystem->durable_chunk_inventory()) {
            if (state.find_chunk(Chunk::calculate_id(coords)))
                continue;
            const auto durable =
                Persistence::WorldSaveService::read_chunk(m_regionDurableDirectory, coords, true);
            if (durable.status != Persistence::WorldSaveService::ChunkReadStatus::Found) {
                copied = false;
                copy_errors.push_back(durable.error.empty()
                                          ? "expected durable chunk missing during save-as"
                                          : durable.error);
                break;
            }
            WorldStreamingState one;
            one.insert_chunk(durable.chunk);
            if (!copier.save_world(one, save_dir, &copy_errors)) {
                copied = false;
                break;
            }
        }
        if (!copied || !copier.save_world(state, save_dir, &copy_errors)) {
            for (const auto& error : copy_errors)
                LUMINUMBRA_CORE_ERROR("World save-as failed: {}", error);
            RecordPersistenceSaveOutcome(false, elapsed_ms());
            return false;
        }
    }

    //  project the live plant roster ONCE and persist it as a sibling file in
    // BOTH the dirty-empty and the normal save path (plants are independent of chunk dirtiness). An
    // empty roster writes NO file, so a no-plant save stays byte-identical (the save-less path
    // holds).
    const auto plant_snapshot =
        luminumbra::foliage::BuildPlantEntitySnapshot(m_registry, m_activeRegionsEnabled);
    if (m_activeRegionsEnabled) {
        std::string plant_error;
        if (!luminumbra::foliage::ValidatePlantEntitySnapshot(
                plant_snapshot, m_activeRegionLedger, m_groundObjects, plant_error)) {
            LUMINUMBRA_CORE_ERROR("Plant save refused: {}", plant_error);
            RecordPersistenceSaveOutcome(false, elapsed_ms());
            return false;
        }
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
    if (dirty_ids.empty()) {
        // No dirty CHUNKS. Without an existing snapshot this keeps a save-less, no-plant world
        // byte-for-byte (no chunks/ dir created); a planted world still persists its plants here.
        const bool plants_ok =
            Persistence::WorldSaveService::save_plant_entities(plant_snapshot, save_dir, nullptr);
        const bool energy_ok = SaveEnergyFieldRecord(save_dir);
        if (m_activeRegionsEnabled && (!plants_ok || !energy_ok)) {
            RecordPersistenceSaveOutcome(false, elapsed_ms());
            return false;
        }
        if (report) {
            *report = result;
        }
        bool empty_ok = !m_activeRegionsEnabled || SaveWorldMetadataTo(save_dir);
        if (empty_ok)
            defer_success();
        if (empty_ok && outer) {
            empty_ok = outer->Commit();
            if (empty_ok)
                m_regionDurableDirectory = save_dir;
        }
        if (!empty_ok)
            RecordPersistenceSaveOutcome(false, elapsed_ms());
        return empty_ok;
    }

    Persistence::WorldSaveService service;
    std::vector<std::string> errors;
    // Subsequent saves rewrite the edited regions of a supported container.
    const bool has_snapshot = m_activeRegionsEnabled
                                  ? Persistence::WorldSaveService::has_chunk_snapshot(save_dir)
                                  : Persistence::WorldSaveService::has_world_save(save_dir);

    bool ok = false;
    if (!has_snapshot) {
        // First save of this world: write the full snapshot, then clear the
        // dirty flags exactly like the incremental path does.
        ok = service.save_world(state, save_dir, &errors);
        if (ok) {
            // Inside the outer transaction the dirty bits clear only once it commits.
            if (auto* transaction = Persistence::SaveTransaction::active_for(save_dir))
                transaction->defer_until_commit(acknowledge);
            else
                acknowledge();
        }
    } else {
        const Persistence::WorldSaveDirtyReport dirty_report =
            service.save_dirty_chunks(state, save_dir, &errors);
        ok = dirty_report.saved;
    }

    // A multi-region save can commit an early atomic LMR1 replacement and then
    // report failure on a later region. Conservatively invalidate every dirty
    // chunk's affected far regions after any Phase-A attempt: this rejects a
    // worker that read an older container even across a partial-save result.
    // An extra rebuild when nothing committed is safe; missing a partial commit
    // would allow old derived bytes to integrate as current.
    for (const ChunkID id : dirty_ids) {
        if (const auto chunk = state.find_chunk(id)) {
            m_worldSystem->notify_far_lod_authority_durable(chunk->get_coords());
        }
    }

    //  persist plants alongside the chunk write (empty roster -> no file -> byte-identical).
    if (!Persistence::WorldSaveService::save_plant_entities(plant_snapshot, save_dir, &errors)) {
        ok = false;
    }

    // persist the stateful energy layer alongside (null/
    // all-zero layer -> no file -> byte-identical; the plant discipline).
    const bool energy_ok = SaveEnergyFieldRecord(save_dir);
    if (m_activeRegionsEnabled && !energy_ok)
        ok = false;

    for (const std::string& error : errors) {
        LUMINUMBRA_CORE_ERROR("World state save failed: {}", error);
    }
    if (ok && m_activeRegionsEnabled)
        ok = SaveWorldMetadataTo(save_dir);
    if (ok)
        defer_success();
    if (ok && outer) {
        ok = outer->Commit();
        if (ok)
            m_regionDurableDirectory = save_dir;
    }
    if (ok) {
        result.saved = true;
        result.chunks_saved = result.chunks_total; // whole-snapshot layout
        LUMINUMBRA_CORE_INFO("World state saved: {} chunks ({} dirty) -> {}",
                             result.chunks_total,
                             result.chunks_dirty,
                             save_dir.string());
    }
    if (m_activeRegionsEnabled) {
        if (!ok) {
            // The whole save failed after chunk dirty bits may have been acknowledged: re-arm
            // every chunk this save covered so the retry rewrites it (no revision bump).
            for (const ChunkID id : dirty_ids) {
                if (const auto chunk = state.find_chunk(id)) {
                    chunk->mark_dirty_for_save();
                }
            }
        }
        if (!ok)
            RecordPersistenceSaveOutcome(false, elapsed_ms());
    }
    if (report) {
        *report = result;
    }
    return ok;
}

bool GameSession::LoadWorldState() {
    if (m_transientWorld)
        return m_worldSystem && m_worldOpenError.empty();
    return LoadWorldStateFrom(GetWorldSaveDir());
}

bool GameSession::LoadWorldStateFrom(const std::filesystem::path& save_dir) {
    const Persistence::ChunkValidationCache::Scope validation_scope(m_chunkValidationCache);
    if (!IsSimulationTickBoundary())
        return false;
    m_lastLoadedChunkCount = 0;
    if (!m_worldOpenError.empty() || !m_worldSystem || save_dir.empty()) {
        return false;
    }
    {
        std::vector<std::string> recovery_errors;
        if (Persistence::recover_pending_journal(save_dir, &recovery_errors) ==
            Persistence::RecoveryOutcome::Failed) {
            m_worldOpenError =
                "World unavailable: an interrupted save could not be recovered: " +
                (recovery_errors.empty() ? std::string("unknown error") : recovery_errors.front());
            m_worldSystem->clear_world(m_physicsSystem.get());
            return false;
        }
    }

    Persistence::WorldSaveService service;
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    std::vector<IVec3> durable_inventory;
    std::set<ChunkID> wanted;
    if (m_activeRegionsEnabled)
        for (const auto& coords : m_worldSystem->GetInitialChunkLoadList(m_metadata.spawnPoint))
            wanted.insert(Chunk::calculate_id(coords));
    const std::function<bool(const IVec3&)> retain =
        m_activeRegionsEnabled ? std::function<bool(const IVec3&)>([&wanted](const IVec3& coords) {
            return wanted.contains(Chunk::calculate_id(coords));
        })
                               : std::function<bool(const IVec3&)>{};
    const bool snapshot_present =
        service.load_world(loaded,
                           save_dir,
                           errors,
                           {},
                           retain,
                           m_activeRegionsEnabled ? &durable_inventory : nullptr);
    if (!snapshot_present && !errors.empty()) {
        m_worldOpenError = errors.front();
        m_worldSystem->clear_world(m_physicsSystem.get());
        for (const std::string& error : errors) {
            LUMINUMBRA_CORE_ERROR("World state load failed: {}", error);
        }
        return false;
    }

    WorldClock saved_clock;
    std::optional<Vec3> ambient_anchor;
    bool requires_active_regions = false;
    std::vector<std::string> clock_errors;
    if (!Persistence::WorldSaveService::read_clock_metadata(
            save_dir, saved_clock, requires_active_regions, &clock_errors, &ambient_anchor) ||
        (requires_active_regions && !m_activeRegionsEnabled)) {
        m_worldOpenError =
            clock_errors.empty()
                ? "Incompatible configuration: this world requires sim.active_regions."
                : clock_errors.front();
        m_worldSystem->clear_world(m_physicsSystem.get());
        return false;
    }
    if (!snapshot_present && !m_activeRegionsEnabled)
        return true; // legacy fresh world
    if (m_activeRegionsEnabled) {
        const Vec3 previous_ambient_anchor = m_ambientFieldAnchor;
        m_ambientFieldAnchor = ambient_anchor.value_or(m_metadata.spawnPoint);
        ActiveRegionLedger ledger;
        if (!Persistence::WorldSaveService::load_active_regions(
                ledger, save_dir, &clock_errors, saved_clock)) {
            m_ambientFieldAnchor = previous_ambient_anchor;
            m_worldOpenError = clock_errors.empty()
                                   ? std::string(ActiveRegionLedger::kCorruptMessage)
                                   : clock_errors.front();
            m_worldSystem->clear_world(m_physicsSystem.get());
            return false;
        }
        // Validation accepted ledger.tick() <= saved_clock.tick(). An interrupted
        // pair can be strictly older. Rebase its header without scheduling work,
        // so immediate edits/save stamps at the restored clock remain decodable.
        ledger.restore_clock(saved_clock);
        // Loaded before the ledger is adopted so a refusal leaves the session state as the
        // ledger-failure path does.
        GroundObjectStore ground_objects;
        std::vector<std::string> ground_errors;
        if (!Persistence::WorldSaveService::load_ground_objects(
                ground_objects, save_dir, ledger.next_persistent_id(), &ground_errors)) {
            m_ambientFieldAnchor = previous_ambient_anchor;
            m_worldOpenError = ground_errors.empty() ? std::string("Corrupt ground objects.")
                                                     : ground_errors.front();
            m_worldSystem->clear_world(m_physicsSystem.get());
            return false;
        }
        FieldPageState field_pages;
        std::vector<std::string> page_errors;
        if (!Persistence::WorldSaveService::load_field_pages(field_pages, save_dir, &page_errors)) {
            m_ambientFieldAnchor = previous_ambient_anchor;
            m_worldOpenError =
                page_errors.empty() ? std::string("Corrupt field pages.") : page_errors.front();
            m_worldSystem->clear_world(m_physicsSystem.get());
            return false;
        }
        std::optional<luminumbra::fields::EnergyFieldState> energy;
        std::error_code energy_ec;
        const auto energy_path = save_dir / "aether_state.efs";
        const bool has_energy = fs::exists(energy_path, energy_ec);
        if (energy_ec || ((has_energy || !ledger.energy_ages().empty()) && !m_energyFieldState)) {
            m_worldOpenError = energy_ec ? "Unreadable regional energy record."
                                         : "Incompatible configuration: regional energy state "
                                           "requires sim.aether_state.";
            m_ambientFieldAnchor = previous_ambient_anchor;
            m_worldSystem->clear_world(m_physicsSystem.get());
            return false;
        }
        if (m_energyFieldState) {
            energy.emplace(m_energyFieldState->channels());
            bool valid = true;
            if (has_energy) {
                const auto length = fs::file_size(energy_path, energy_ec);
                valid = !energy_ec &&
                        length <= luminumbra::fields::EnergyFieldState::kMaxRegionalRecordBytes;
                if (valid) {
                    std::ifstream input(energy_path, std::ios::binary);
                    const std::string bytes(std::istreambuf_iterator<char>{input}, {});
                    valid = input.good() && energy->DeserializeRegionalRecord(bytes);
                }
            }
            valid = valid && energy->ValidateRegionalOwners(ledger.records(), ledger.energy_ages());
            if (!valid) {
                m_worldOpenError = "Corrupt regional energy record.";
                m_ambientFieldAnchor = previous_ambient_anchor;
                m_worldSystem->clear_world(m_physicsSystem.get());
                return false;
            }
        }
        m_groundObjects = std::move(ground_objects);
        m_fieldPages = std::move(field_pages);
        m_activeRegionLedger = std::move(ledger);
        if (!m_activeRegionLedger.local_anchor())
            m_activeRegionLedger.try_set_local_anchor(m_metadata.spawnPoint);
        m_regionDurableDirectory = save_dir;
        m_worldSystem->remember_durable_chunks(durable_inventory);
        m_regionSchedule = {};
        m_regionWork.clear();
        m_regionCompletedWork.clear();
        m_regionFault.reset();
        m_replicatedSimulationAnchors.reset();
        RestoreWorldClock(saved_clock);
        if (energy)
            *m_energyFieldState = std::move(*energy);
    }

    // Validate every chunk before adopting any state. A malformed lattice must
    // never become an invitation to regenerate over authoritative disk bytes.
    constexpr std::size_t kFullSdfLattice =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    for (const auto& chunk : loaded.snapshot_chunks()) {
        if (chunk && !chunk->sdf_data.empty() && chunk->sdf_data.size() != kFullSdfLattice) {
            m_worldOpenError = "corrupt world chunk SDF lattice";
            m_worldSystem->clear_world(m_physicsSystem.get());
            LUMINUMBRA_CORE_ERROR("World open refused: {}", m_worldOpenError);
            return false;
        }
    }

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

    // boot-time water-resolution migration: a save written at another
    // sim resolution (sim.water_high_res flipped between sessions) must converge
    // BEFORE the first live tick. Left to the live path it would resize at
    // MAX_WATER_RESIZES_PER_TICK=1 while the seam pass — which hard-gates on equal
    // resolution — walls off water at every mixed seam for thousands of ticks. One
    // uncapped pass here (the boot-settle cap-lifting idea applied to resizes) is
    // deterministic: each chunk's integer-bilinear mm resample is a pure function of
    // its loaded state + worldgen + the session resolution. Matching saves resize
    // nothing — the default path stays byte-identical.
    if (!m_activeRegionsEnabled) {
        const std::size_t migrated = m_worldSystem->MigrateWaterSimResolution();
        if (migrated > 0) {
            LUMINUMBRA_CORE_INFO(
                "Water-resolution migration: {} loaded chunks resized to {}x{} before first tick",
                migrated,
                m_worldSystem->debug_water_sim_resolution(),
                m_worldSystem->debug_water_sim_resolution());
        }
    }

    //  reload persisted plant entities into the live registry (a missing file is
    // a clean miss). Geometry rebakes from the reloaded genome+stage on the render bridge.
    {
        Luminumbra::Ecs::EntityRegistrySnapshot plant_snapshot;
        std::vector<std::string> plant_errors;
        const bool read = Persistence::WorldSaveService::load_plant_entities(
            plant_snapshot, save_dir, &plant_errors);
        std::string error;
        if (m_activeRegionsEnabled &&
            (!read || !luminumbra::foliage::ValidatePlantEntitySnapshot(
                          plant_snapshot, m_activeRegionLedger, m_groundObjects, error))) {
            m_worldOpenError =
                error.empty() ? (plant_errors.empty() ? "Corrupt plants" : plant_errors.front())
                              : error;
            m_worldSystem->clear_world(m_physicsSystem.get());
            return false;
        }
        if (read) {
            try {
                // Decode completely into a scratch registry first; malformed optional
                // components cannot leave a partially adopted live roster.
                entt::registry scratch;
                luminumbra::foliage::ApplyPlantEntitySnapshot(scratch, plant_snapshot);
                luminumbra::foliage::ApplyPlantEntitySnapshot(m_registry, plant_snapshot);
            } catch (const std::exception& e) {
                if (!m_activeRegionsEnabled)
                    throw;
                m_worldOpenError = std::string("Corrupt plant payload: ") + e.what();
                m_worldSystem->clear_world(m_physicsSystem.get());
                return false;
            }
        } else
            for (const auto& e : plant_errors)
                LUMINUMBRA_CORE_ERROR("Plant state load failed: {}", e);
    }

    // restore the stateful energy layer's record (a
    // missing file is a clean miss == all zeros by contract). Epoch-rebased
    // onto the loaded session's tick stream (-4).
    if (m_energyFieldState && !m_activeRegionsEnabled) {
        const fs::path recordPath = save_dir / "aether_state.efs";
        std::ifstream recordFile(recordPath, std::ios::binary);
        if (recordFile) {
            std::ostringstream recordBytes;
            recordBytes << recordFile.rdbuf();
            if (m_energyFieldState->DeserializeRecord(recordBytes.str(),
                                                      GetSimulationTickCount())) {
                LUMINUMBRA_CORE_INFO("Aether state record restored ({} pages).",
                                     m_energyFieldState->page_count());
            } else {
                LUMINUMBRA_CORE_WARN("Aether state record UNREADABLE at {} - starting all-zero.",
                                     recordPath.string());
            }
        }
    }

    m_lastLoadedChunkCount = adopted;
    LUMINUMBRA_CORE_INFO("World state loaded: {} chunks adopted ({} in snapshot) from {}",
                         adopted,
                         loaded.size(),
                         save_dir.string());
    return true;
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
        std::hash<std::string> hasher;
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
    return static_cast<int>(std::lround((world_x - ScentDomainOrigin(true)) / kScentCellSize));
}
int GameSession::ScentWorldToCellZ(float world_z) const {
    return static_cast<int>(std::lround((world_z - ScentDomainOrigin(false)) / kScentCellSize));
}
float GameSession::ScentCellToWorldX(int cell_x) const {
    return ScentDomainOrigin(true) + static_cast<float>(cell_x) * kScentCellSize;
}
float GameSession::ScentCellToWorldZ(int cell_z) const {
    return ScentDomainOrigin(false) + static_cast<float>(cell_z) * kScentCellSize;
}

void GameSession::InitializeScentField(const Vec3& /*anchor*/) {
    m_scentField = std::make_unique<luminumbra::ai::ScentField>(
        kScentFieldCells, kScentFieldCells, kScentFieldChannels);
    if (m_activeRegionsEnabled)
        m_scentField->BindDepositGate([this](int x, int z) { return ScentCellDue(x, z); });
    LUMINUMBRA_CORE_INFO("Scent field initialized: {}x{} cells, {} channel(s), cell_size={}",
                         kScentFieldCells,
                         kScentFieldCells,
                         kScentFieldChannels,
                         kScentCellSize);
}

float GameSession::ScentDomainOrigin(bool x_axis) const {
    const float anchor = m_activeRegionsEnabled
                             ? (x_axis ? m_ambientFieldAnchor.x : m_ambientFieldAnchor.z)
                             : (x_axis ? m_metadata.spawnPoint.x : m_metadata.spawnPoint.z);
    return ScentOriginFor(m_activeRegionsEnabled ? std::floor(anchor) : anchor);
}
RegionKey GameSession::ScentCellOwner(int x, int z) const {
    return FieldPageState::region_of_scent_cell(
        {static_cast<std::int32_t>(ScentDomainOrigin(true)) + x,
         static_cast<std::int32_t>(ScentDomainOrigin(false)) + z});
}
bool GameSession::ScentCellDue(int x, int z) const {
    if (m_regionFault || x < 0 || z < 0 || x >= kScentFieldCells || z >= kScentFieldCells)
        return false;
    const auto owner = ScentCellOwner(x, z);
    return std::find(m_regionSchedule.due.begin(), m_regionSchedule.due.end(), owner) !=
           m_regionSchedule.due.end();
}
bool GameSession::ScentEdgeOpen(int x, int z, int nx, int nz) const {
    const auto source = ScentCellOwner(x, z), destination = ScentCellOwner(nx, nz);
    if (source == destination)
        return ScentCellDue(x, z);
    const auto& records = m_activeRegionLedger.records();
    const auto a = records.find(source), b = records.find(destination);
    if (a == records.end() || b == records.end() || a->second.state == RegionState::Frozen ||
        b->second.state == RegionState::Frozen)
        return false;
    const auto shift = std::max(a->second.cadence_shift, b->second.cadence_shift);
    return shift < 63 && (m_worldClock.tick() & ((std::uint64_t{1} << shift) - 1)) == 0;
}
void GameSession::RestoreRegionalScent() {
    if (!m_activeRegionsEnabled || !m_scentField)
        return;
    for (int z = 0; z < kScentFieldCells; ++z)
        for (int x = 0; x < kScentFieldCells; ++x) {
            const ScentCellKey key{static_cast<std::int32_t>(ScentDomainOrigin(true)) + x,
                                   static_cast<std::int32_t>(ScentDomainOrigin(false)) + z};
            const auto found = m_fieldPages.scent_cells().find(key);
            for (int channel = 0; channel < kScentFieldChannels; ++channel)
                m_scentField->RestoreCell(channel,
                                          x,
                                          z,
                                          found == m_fieldPages.scent_cells().end()
                                              ? 0.0
                                              : std::bit_cast<double>(found->second[channel]));
        }
}
void GameSession::CaptureRegionalScent() {
    auto& changed = m_scentCaptureUpdates;
    changed.clear();
    std::map<RegionKey, std::uint64_t> work;
    const auto origin_x = static_cast<std::int32_t>(ScentDomainOrigin(true));
    const auto origin_z = static_cast<std::int32_t>(ScentDomainOrigin(false));
    const auto& stored = m_fieldPages.scent_cells();
    // Storage is ordered by (x, z), capture by (z, x). Walk one cursor per column
    // instead of searching the tree for every cell. Authority stays sealed until
    // update_scent commits, including when this domain has moved since last tick.
    std::array<decltype(stored.begin()), kScentFieldCells> columns;
    for (int x = 0; x < kScentFieldCells; ++x)
        columns[x] = stored.lower_bound({origin_x + x, origin_z});
    changed.reserve(kScentFieldCells * kScentFieldCells);
    for (int z = 0; z < kScentFieldCells; ++z)
        for (int x = 0; x < kScentFieldCells; ++x) {
            const ScentCellKey key{origin_x + x, origin_z + z};
            ScentCell values{};
            for (int channel = 0; channel < kScentFieldChannels; ++channel)
                values[channel] = std::bit_cast<std::uint64_t>(m_scentField->Sample(channel, x, z));
            auto& cursor = columns[x];
            const bool exists = cursor != stored.end() && cursor->first == key;
            const ScentCell old = exists ? cursor->second : ScentCell{};
            if (exists)
                ++cursor;
            if (values == old)
                continue;
            changed.emplace_back(key, values);
            ++work[FieldPageState::region_of_scent_cell(key)];
        }
    if (!m_fieldPages.update_scent(changed))
        throw std::runtime_error("Regional scent admission failed");
    for (const auto& [owner, updates] : work)
        RecordActiveRegionWork({owner, 0, 0, 0, updates});
}

void GameSession::AdvanceNearScent() {
    // 2c.: scent stigmergy write/update. Game-data opt-in:
    // no scent emitter/sensor components means no field mutation, while active
    // ecology worlds get deterministic deposit -> diffuse/evaporate -> clamp.
    const bool scent_active = HasScentParticipants();
    // ant foraging deposits trails into the SAME scent field (channels 2/3), so it runs in
    // this slot BEFORE the diffuse/evaporate Step (its deposits then diffuse + evaporate
    // this tick, like creature scent). Opt-in via ForagerComponent: a roster with none is a
    // no-op, so the canonical roster (and any scent-only world) is byte-identical.
    // Integer/id-ordered/libm-free.
    const auto foragers_view = m_registry.view<const Luminumbra::Components::ForagerComponent>();
    const bool foraging_active = std::any_of(
        foragers_view.begin(), foragers_view.end(), [this, &foragers_view](const auto entity) {
            if (!m_activeRegionsEnabled)
                return true;
            const auto& forager =
                foragers_view.get<const Luminumbra::Components::ForagerComponent>(entity);
            return ScentCellDue(forager.cell_x, forager.cell_z);
        });
    const bool scent_interior = scent_active || foraging_active;
    if ((scent_interior || HasScentBorderWork()) && m_scentField) {
        if (scent_active) {
            luminumbra::ai::RunScentDepositOnTick(m_registry,
                                                  *m_scentField,
                                                  ScentDomainOrigin(true),
                                                  ScentDomainOrigin(false),
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
        luminumbra::ai::detail::ScentStepGates gates;
        if (m_activeRegionsEnabled) {
            // This 128 m square intersects at most two 512 m owners on each axis.
            // The schedule and anchor are sealed throughout Step/Clamp. Resolve their
            // gates once per tile, rather than doing world conversions and ledger/due
            // lookups for every channel, diffusion iteration and advection path hop.
            static_assert(kScentFieldCells * kScentCellSize < 512.0f);
            gates = luminumbra::ai::detail::CacheNearScentGates(
                kScentFieldCells,
                [this](int x, int z) { return ScentCellOwner(x, z); },
                [this, scent_interior](int x, int z) {
                    return scent_interior && ScentCellDue(x, z);
                },
                [this](int x, int z, int nx, int nz) { return ScentEdgeOpen(x, z, nx, nz); });
        }
        m_scentField->Step(kScentDiffusionRate,
                           kScentDiffusionIterations,
                           kScentEvaporation,
                           wind_cx,
                           wind_cz,
                           gates.participants,
                           gates.exchange);
        m_scentField->Clamp(kScentTauMin, kScentTauMax, gates.participants);
        if (m_activeRegionsEnabled)
            CaptureRegionalScent();
    }
}

bool GameSession::HasScentBorderWork() const {
    if (!m_activeRegionsEnabled || !m_scentField || m_regionFault)
        return false;
    bool participant = HasScentParticipants(false);
    const auto foragers = m_registry.view<const Luminumbra::Components::ForagerComponent>();
    for (const auto entity : foragers) {
        const auto& ant = foragers.get<const Luminumbra::Components::ForagerComponent>(entity);
        participant =
            participant || (ant.cell_x >= 0 && ant.cell_z >= 0 && ant.cell_x < kScentFieldCells &&
                            ant.cell_z < kScentFieldCells);
    }
    if (!participant)
        return false;
    for (int z = 0; z < kScentFieldCells; ++z)
        for (int x = 0; x < kScentFieldCells; ++x)
            for (const auto& offset : {std::pair{1, 0}, std::pair{0, 1}}) {
                const int nx = x + offset.first, nz = z + offset.second;
                if (nx >= kScentFieldCells || nz >= kScentFieldCells ||
                    ScentCellOwner(x, z) == ScentCellOwner(nx, nz) || !ScentEdgeOpen(x, z, nx, nz))
                    continue;
                for (int channel = 0; channel < kScentFieldChannels; ++channel)
                    if (m_scentField->Sample(channel, x, z) !=
                        m_scentField->Sample(channel, nx, nz))
                        return true;
            }
    return false;
}

bool GameSession::HasScentParticipants(bool require_due) const {
    const auto near_due = [this, require_due](const Vec3& position) {
        if (!m_activeRegionsEnabled)
            return true;
        const float x = position.x - ScentDomainOrigin(true),
                    z = position.z - ScentDomainOrigin(false);
        return std::isfinite(x) && std::isfinite(z) && x >= 0 && z >= 0 && x < kScentFieldCells &&
               z < kScentFieldCells &&
               (!require_due || ScentCellDue(static_cast<int>(x), static_cast<int>(z)));
    };
    {
        auto emitters = m_registry.view<const Luminumbra::Components::TransformComponent,
                                        const Luminumbra::Components::SensableComponent>();
        for (auto e : emitters) {
            if (!near_due(
                    emitters.get<const Luminumbra::Components::TransformComponent>(e).position))
                continue;
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
            if (!near_due(
                    sensors.get<const Luminumbra::Components::TransformComponent>(e).position))
                continue;
            const auto& s = sensors.get<const Luminumbra::Components::ScentSenseComponent>(e);
            if (s.channel >= 0) {
                return true;
            }
        }
    }
    return false;
}

std::string GameSession::ComputeScentSubHash() const {
    if (m_activeRegionsEnabled) {
        if (m_fieldPages.scent_cells().empty())
            return {};
        std::string bytes = "regional_scent:v2:";
        for (const auto& [cell, values] : m_fieldPages.scent_cells()) {
            const auto append = [&bytes](std::uint64_t value, int count) {
                for (int i = 0; i < count; ++i)
                    bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
            };
            append(static_cast<std::uint32_t>(cell.x), 4);
            append(static_cast<std::uint32_t>(cell.z), 4);
            for (const auto value : values)
                append(value, 8);
        }
        return Persistence::StableChecksum(bytes);
    }
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

bool GameSession::SaveEnergyFieldRecord(const std::filesystem::path& save_dir) {
    if (!m_worldOpenError.empty())
        return false;
    if (!m_energyFieldState || save_dir.empty())
        return true;
    if (m_activeRegionsEnabled) {
        const auto record = m_energyFieldState->SerializeRegionalRecord();
        std::vector<std::string> errors;
        const bool saved = record.empty() ? Persistence::WorldSaveService::remove_member(
                                                save_dir, "aether_state.efs", &errors)
                                          : Persistence::WorldSaveService::write_member_bytes(
                                                save_dir, "aether_state.efs", record, &errors);
        for (const auto& error : errors)
            LUMINUMBRA_CORE_ERROR("Failed to save regional energy: {}", error);
        return saved;
    }
    if (m_energyFieldState->page_count() == 0) {
        if (!m_activeRegionsEnabled)
            return true; // Preserve legacy empty-layer save behavior.
        const auto tick = GetSimulationTickCount();
        const auto aligned_next = tick + (luminumbra::fields::kEnergyCadenceTicks -
                                          tick % luminumbra::fields::kEnergyCadenceTicks);
        if (m_energyFieldState->next_fire_tick() == aligned_next) {
            // Absence reconstructs this phase. Retire any stale nonempty record.
            std::vector<std::string> errors;
            const bool removed =
                Persistence::WorldSaveService::remove_member(save_dir, "aether_state.efs", &errors);
            for (const std::string& error : errors)
                LUMINUMBRA_CORE_ERROR("Failed to retire aether state record: {}", error);
            return removed;
        }
        // A legacy import can retain a different phase after its last unit decays.
        // The existing header alone preserves it without adding zero pages.
    }
    const std::string record = m_energyFieldState->SerializeRecord(GetSimulationTickCount());
    if (m_activeRegionsEnabled || Persistence::SaveTransaction::active_for(save_dir) ||
        Persistence::snapshot_index_exists(save_dir)) {
        // Flag-on (or indexed-directory) lane: durable binary replace inside the save transaction.
        std::vector<std::string> errors;
        const bool written = Persistence::WorldSaveService::write_member_bytes(
            save_dir, "aether_state.efs", record, &errors);
        for (const std::string& error : errors)
            LUMINUMBRA_CORE_ERROR("Failed to write aether state record: {}", error);
        return written;
    }
    std::ofstream file(save_dir / "aether_state.efs", std::ios::trunc);
    if (!file.is_open()) {
        LUMINUMBRA_CORE_ERROR("Failed to write aether state record under {}", save_dir.string());
        return false;
    }
    file << record;
    file.flush();
    return file.good();
}

std::string GameSession::ComputeAetherStateSubHash() const {
    if (!m_energyFieldState) {
        return {};
    }
    const std::string bytes = m_activeRegionsEnabled ? m_energyFieldState->CanonicalRegionalBytes()
                                                     : m_energyFieldState->CanonicalBytes();
    if (bytes.empty()) {
        return {}; // all-zero == absent (the scent/plant empty-neutral contract)
    }
    return Persistence::StableChecksum(bytes);
}

std::string GameSession::ComputePlantSubHash() const {
    if (m_activeRegionsEnabled) {
        const auto snap = luminumbra::foliage::BuildPlantEntitySnapshot(m_registry, true);
        return snap.entities.empty()
                   ? std::string{}
                   : Persistence::StableChecksum(
                         Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snap));
    }

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
