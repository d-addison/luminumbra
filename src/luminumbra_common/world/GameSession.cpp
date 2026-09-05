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
#include "../components/SoilComponents.h"
#include "../components/TerritoryComponents.h"
#include "../core/JobSystem.h"
#include "../core/Log.h"
#include "../fields/EnergyFieldState.h"
#include "../persistence/PlantPersistence.h"
#include "../persistence/WorldPersistenceRoundtrip.h" // Persistence::StableChecksum (ComputeScentSubHash)
#include "../persistence/WorldSaveService.h"
#include "../scripting/LuaState.h"
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
#include "nlohmann/json.hpp" // For parsing JSON
#include <cmath>             // std::floor (energy-layer anchor quantization)

#include <algorithm> // explicit: stabilise std template instantiation for entt storage helpers
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator> // (GCC-15 vague-linkage: must be visible at the GrazeableComponent storage
#include <memory>   //  instantiation point, else std::__advance/iter_move get externalized)
#include <random>
#include <sstream>
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
    // Destructor
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
    const std::uint32_t ticks_executed = m_simulationClock.advance(frame_dt);
    if (ticks_executed == 0) {
        return 0;
    }

    // Tick ids are 1-based; the clock already advanced past this frame's
    // ticks, so recover the id of the first one.
    const std::uint64_t first_tick = m_simulationClock.tick_count() - ticks_executed + 1;
    for (std::uint32_t i = 0; i < ticks_executed; ++i) {
        const std::uint64_t current_tick = first_tick + i;

        // Deterministic per-tick system order. The ordered event bus drains everything
        // published for this tick after simulation systems finish (tick -> lane -> sequence).

        // 1. Animation pose sampling: FIRST in the tick order.
        luminumbra::animation::SamplePosesOnTick(m_registry, m_simulationClock.fixed_dt());

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

        // 4.  : weather core update. Runs AFTER wind so storm cells
        // advect by the freshly-updated wind grid. Deterministic (DeterministicMath
        // + FastNoise batch path; no wall-clock/RNG, no std::random). The weather
        // state (category map + storm cells + precip field) feeds the world_hash
        // `weather` sub-hash; the update runs every tick so run==replay and resim
        // agree on the state at every checkpoint.
        if (m_weatherSystem) {
            m_weatherSystem->Update(current_tick, m_metadata.spawnPoint, m_windFieldSystem.get());
        }

        // 5.: Aether scalar field update. Runs AFTER weather so it
        // advects its emission source by the freshly-updated wind grid, keeping
        // same-tick weather coupling current. Deterministic
        // (pure function of seed+14, tick, origin[, wind]; DeterministicMath +
        // FastNoise batch path; no wall-clock/RNG). The cell values feed the
        // world_hash `aether` sub-hash; the update runs every tick so run==replay
        // and resim agree on the field at every checkpoint.
        if (m_aetherFieldSystem) {
            m_aetherFieldSystem->Update(
                current_tick, m_metadata.spawnPoint, m_windFieldSystem.get());
        }

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
            if (!m_registry.view<Comp::SoilFeederComponent>().empty()) {
                if (!m_soilGrid)
                    m_soilGrid =
                        std::make_unique<fol::SoilGrid>(kFoliageFieldCells, kFoliageFieldCells);
                fol::RunSoilNutrientOnTick(
                    m_registry, *m_soilGrid, foliageOriginX, foliageOriginZ, kFoliageFieldCell);
            }
        }

        // 6. : deterministic plant GROWTH. Game-data opt-in (PlantTag):
        // no plants -> the system never runs and world_hash stays byte-identical
        // (same discipline as scent). The environment is ATMOSPHERIC — moisture is
        // driven by the freshly-updated weather precip field (rain -> growth), so
        // growth runs AFTER weather. Integer/fixed-point + id-ordered = run==replay.
        // Temperature, light, soil, weather, and irrigation are sampled below.
        if (HasPlantParticipants()) {
            luminumbra::foliage::EnvSampler plant_env =
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
                    const float tod01 = static_cast<float>(current_tick % kTicksPerDay) /
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
                    const float season01 = static_cast<float>(current_tick % kTicksPerYear) /
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

            if (!m_registry.view<Comp::PlantGenomeComponent>().empty())
                fol::RunPollinationOnTick(m_registry, current_tick, windXZ, 0, m_plantMutationRate);
            if (!m_registry.view<Comp::PlantHealthComponent>().empty())
                fol::RunPlantDiseaseOnTick(m_registry, current_tick);
            //  germination: ripe plants senesce -> annual dies + reseeds, perennial
            // resets + reseeds (child genome = the pollination cross, computed just above, or
            // self). Runs AFTER pollination so next_genome is fresh. Opt-in via
            // CropLifecycleComponent.
            if (!m_registry.view<Comp::CropLifecycleComponent>().empty())
                fol::RunCropLifecycleOnTick(m_registry, current_tick);
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
            if (!m_registry.view<Comp::GrazeableComponent>().empty())
                luminumbra::ai::RunWildlifeFoliageOnTick(
                    m_registry, current_tick, m_wildlifeFoliageTuning);
            if (!m_registry.view<Comp::MortalComponent>().empty())
                luminumbra::ai::RunLifespanOnTick(m_registry, current_tick);
            // Herd alarm: propagate collective vigilance among same-role creatures. The brain
            // (slot 2e) sets AlarmComponent.alarmed when a prey senses a threat and reads the
            // propagated level to flee with the herd; this maintains the field.
            if (!m_registry.view<Comp::AlarmComponent>().empty())
                luminumbra::ai::RunHerdAlarmOnTick(
                    m_registry, static_cast<float>(m_simulationClock.fixed_dt()));
            // Decomposition: dead creatures decay + release nutrient (consumed by the soil loop).
            if (!m_registry.view<Comp::DecayComponent>().empty())
                luminumbra::ai::RunDecompositionOnTick(m_registry, current_tick);
            // Circadian: diurnal/nocturnal activity from a tick-derived day fraction.
            if (!m_registry.view<Comp::CircadianComponent>().empty()) {
                constexpr std::uint64_t kTicksPerDay = 30ull * 60ull * 20ull; // 20-min day @30Hz
                const float tod01 = static_cast<float>(current_tick % kTicksPerDay) /
                                    static_cast<float>(kTicksPerDay);
                luminumbra::ai::RunCircadianOnTick(m_registry, tod01, m_circadianAmplitude);
            }
            // Territory: claim home + emit a homing bias (movement blends it like mate-seeking).
            if (!m_registry.view<Comp::TerritoryComponent>().empty())
                luminumbra::ai::RunTerritoryOnTick(m_registry, current_tick);
            // Predator pack: flanking coordination (a pack surrounds the prey).
            if (!m_registry.view<Comp::PackHunterComponent>().empty())
                luminumbra::ai::RunPredatorPackOnTick(m_registry, current_tick);
            // Migration: seasonal drive toward a moving target (tick-derived year fraction).
            if (!m_registry.view<Comp::MigratoryComponent>().empty()) {
                constexpr std::uint64_t kTicksPerYear = 30ull * 60ull * 60ull; // ~1h year @30Hz
                const float season01 = static_cast<float>(current_tick % kTicksPerYear) /
                                       static_cast<float>(kTicksPerYear);
                luminumbra::ai::RunMigrationOnTick(m_registry, season01);
            }
        }

        m_simulationEventBus.drain(current_tick);
    }
    return ticks_executed;
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

    // Set up metadata
    m_metadata.name = name;
    m_metadata.seed = seed.empty() ? std::to_string(std::random_device{}()) : seed;
    m_metadata.worldType = worldType;
    m_metadata.worldId = GenerateWorldId();
    m_metadata.creationTime = std::time(nullptr);
    m_metadata.spawnPoint = Vec3(0, 100, 0);

    // Create world directory
    std::string worldPath = m_rootPath + "worlds/saves/" + m_metadata.worldId;
    try {
        fs::create_directories(worldPath);
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_ERROR("Failed to create world directory '{}': {}", worldPath, e.what());
        return false;
    }

    // Initialize Physics System
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    // Customized world: embed the resolved preset in THIS world's own save dir and generate from
    // it, so the world is self-contained (no global custom files / dangling references).
    fs::path preset_to_load = validation.preset_path;
    if (customPresetJson) {
        const fs::path world_preset = fs::path(worldPath) / "preset.json";
        std::ofstream pf(world_preset, std::ios::binary);
        if (pf) {
            pf << *customPresetJson;
            if (pf.good()) {
                preset_to_load = world_preset;
                LUMINUMBRA_CORE_INFO("Custom world preset embedded in save: {}",
                                     world_preset.string());
            } else {
                LUMINUMBRA_CORE_ERROR("Embedded preset write failed; using base preset '{}'",
                                      worldType);
            }
        } else {
            LUMINUMBRA_CORE_ERROR(
                "Could not open embedded preset for write; using base preset '{}'", worldType);
        }
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
    if (!SaveWorld()) {
        LUMINUMBRA_CORE_ERROR("Failed to save world metadata");
        return false;
    }
    return true;
}

bool GameSession::LoadWorld(const std::string& worldId) {
    m_worldOpenError = "World metadata or configuration could not be opened.";
    m_lastLoadedChunkCount = 0;
    std::vector<std::string> open_errors;
    if (!Persistence::WorldSaveService::validate_save(
            fs::path(m_rootPath + "worlds/saves/" + worldId), &open_errors)) {
        m_worldOpenError = open_errors.front();
        if (m_worldSystem)
            m_worldSystem->clear_world(m_physicsSystem.get());
        LUMINUMBRA_CORE_ERROR("World open refused: {}", m_worldOpenError);
        return false;
    }
    if (!m_jobSystem) {
        LUMINUMBRA_CORE_ERROR("JobSystem not set before loading world");
        return false;
    }

    std::string worldPath = m_rootPath + "worlds/saves/" + worldId;
    std::string metadataPath = worldPath + "/world_info.json";

    if (!fs::exists(metadataPath)) {
        LUMINUMBRA_CORE_ERROR("World not found: {}", worldId);
        return false;
    }

    // --- Load Metadata from world_info.json ---
    std::ifstream metadata_file(metadataPath);
    nlohmann::json metadata_json;
    // restored after the world systems exist (see below). Old saves -> 0.
    std::size_t water_sim_cursor = 0;
    try {
        metadata_json = nlohmann::json::parse(metadata_file);
        m_metadata.name = metadata_json.value("name", "Unnamed World");
        m_metadata.seed = metadata_json.value("seed", "0");
        m_metadata.worldType = metadata_json.value("worldType", "default");
        m_metadata.worldId = worldId;
        m_metadata.creationTime = metadata_json.value("creationTime", 0);
        //  (minimal additive hook): restore the persisted spawn point
        // so a headless host booting an existing save anchors its chunk
        // streaming where the world was created, not at the origin. Saves
        // written before spawnPoint existed fall through to the terrain
        // sample below.
        if (metadata_json.contains("spawnPoint")) {
            const nlohmann::json& spawn_json = metadata_json.at("spawnPoint");
            m_metadata.spawnPoint = Vec3(spawn_json.value("x", 0.0f),
                                         spawn_json.value("y", 0.0f),
                                         spawn_json.value("z", 0.0f));
        }
        water_sim_cursor = metadata_json.value("waterSimCursor", std::size_t{0});
    } catch (const nlohmann::json::parse_error& e) {
        LUMINUMBRA_CORE_ERROR(
            "Failed to parse world metadata file '{}': {}", metadataPath, e.what());
        return false;
    }

    const WorldConfigValidationResult validation =
        ValidateWorldConfig(m_rootPath, m_metadata.worldType, m_requiredClientAssets);
    if (!validation.ok) {
        for (const std::string& error : validation.errors) {
            LUMINUMBRA_CORE_ERROR("World config validation failed: {}", error);
        }
        return false;
    }

    // --- Load Generation Preset ---
    // Prefer this world's OWN embedded preset (custom worlds) over the named global preset, so a
    // copied/shared save reproduces its exact terrain regardless of the curated presets dir.
    fs::path preset_to_load = validation.preset_path;
    const fs::path embedded_preset = fs::path(worldPath) / "preset.json";
    if (fs::exists(embedded_preset)) {
        preset_to_load = embedded_preset;
        LUMINUMBRA_CORE_INFO("Loading embedded world preset: {}", embedded_preset.string());
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

    // sim.water_high_res: raise the solver to High BEFORE any chunk is adopted
    // or seeded (default-OFF = untouched Medium = byte-identical). A loaded save
    // at another resolution migrates in LoadWorldStateFrom's boot pass.
    ApplyWaterResolutionWiring();

    // the wind field is a pure function of the world seed, so a
    // loaded world reconstructs the identical field (the heavy-oracle/replay
    // resim reaches the same wind sub-hash at the same tick).
    m_windFieldSystem = std::make_unique<Systems::WindFieldSystem>(world_seed);

    // weather is likewise a pure function of (world seed, tick,
    // anchor). A loaded world reconstructs the identical weather core. NOTE: like
    // wind, the loaded session's tick counter starts at 0, so the loaded session
    // reproduces the SAME-TICK state, not the original's absolute-tick state --
    // the heavy-oracle cross-phase compare excludes weather for this reason
    // (documented in main_server.cpp AuthoritativeStateEqual), exactly as wind is.
    m_weatherSystem = std::make_unique<Systems::WeatherSystem>(world_seed);

    // aether is likewise a pure function of (world seed, tick, anchor);
    // a loaded world reconstructs the identical field at the same tick (heavy-
    // oracle cross-phase compare excludes it for the same loaded-tick-zero reason
    // as wind/weather).
    m_aetherFieldSystem = std::make_unique<Systems::AetherFieldSystem>(world_seed);

    // unlike the re-derivable trio above, the stateful
    // energy layer is AUTHORITATIVE state — when enabled, a persisted record
    // restores it (epoch-rebased onto the loaded tick base of 0); an absent
    // record is all-zeros by contract. OFF -> null -> byte-identical.
    InitializeEnergyFieldState();
    if (m_energyFieldState) {
        const std::string recordPath = worldPath + "/aether_state.efs";
        std::ifstream recordFile(recordPath);
        if (recordFile) {
            std::ostringstream recordBytes;
            recordBytes << recordFile.rdbuf();
            if (m_energyFieldState->DeserializeRecord(recordBytes.str(), 0)) {
                LUMINUMBRA_CORE_INFO("Aether state record restored ({} pages).",
                                     m_energyFieldState->page_count());
            } else {
                LUMINUMBRA_CORE_WARN("Aether state record UNREADABLE at {} - starting all-zero.",
                                     recordPath);
            }
        }
    }

    // Legacy saves without a persisted spawnPoint: derive it from terrain
    // height exactly like CreateWorld does (pure function of seed/params).
    if (!metadata_json.contains("spawnPoint")) {
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
        m_worldSystem->SetWaterSimWindowCursor(water_sim_cursor);
    }

    // same weather-rain wiring as CreateWorld (default-OFF = no-op).
    ApplyWeatherRainWiring();

    m_worldOpenError.clear();
    if (!LoadWorldState())
        return false;
    LUMINUMBRA_CORE_INFO("World loaded successfully: {}", m_metadata.name);
    return true;
}

bool GameSession::SaveWorld() {
    if (!m_worldOpenError.empty() ||
        !Persistence::WorldSaveService::validate_save(GetWorldSaveDir()))
        return false;
    std::string worldPath = m_rootPath + "worlds/saves/" + m_metadata.worldId;
    std::string metadataPath = worldPath + "/world_info.json";

    std::ofstream file(metadataPath);
    if (!file.is_open()) {
        LUMINUMBRA_CORE_ERROR("Failed to create world metadata file: {}", metadataPath);
        return false;
    }

    // Using nlohmann::json for robust saving
    nlohmann::json metadata_json = {
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

    file << std::setw(4) << metadata_json << std::endl;
    file.close();
    return true;
}

std::filesystem::path GameSession::GetWorldSaveDir() const {
    if (m_metadata.worldId.empty()) {
        return {};
    }
    return fs::path(m_rootPath + "worlds/saves/" + m_metadata.worldId);
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
    WorldStateSaveReport result;
    if (report) {
        *report = result;
    }
    if (!m_worldOpenError.empty() || !m_worldSystem || save_dir.empty()) {
        return false;
    }

    if (!Persistence::WorldSaveService::validate_save(save_dir))
        return false;

    // Quiesce in-flight generation/promotion/meshing so chunk data is stable
    // on disk — WITHOUT publishing: a save must never be an
    // activation event. Generated chunks are already live (generation writes
    // live fields); only unpublished mesh/promotion staging stays out, which
    // the next legitimate publish point (the barrier today, the activation
    // queue after activation queue) delivers on its own schedule. On the per-tick-
    // quiesced server paths everything is already drained and published here,
    // so the saved bytes are identical to the old publishing barrier's.
    m_worldSystem->quiesce_streaming_jobs_for_save();

    WorldStreamingState state;
    for (const auto& chunk : m_worldSystem->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }
    result.chunks_total = state.size();

    //  project the live plant roster ONCE and persist it as a sibling file in
    // BOTH the dirty-empty and the normal save path (plants are independent of chunk dirtiness). An
    // empty roster writes NO file, so a no-plant save stays byte-identical (the save-less path
    // holds).
    const auto plant_snapshot = luminumbra::foliage::BuildPlantEntitySnapshot(m_registry);

    const std::vector<ChunkID> dirty_ids = state.dirty_chunk_ids();
    result.chunks_dirty = dirty_ids.size();
    if (dirty_ids.empty()) {
        // No dirty CHUNKS. Without an existing snapshot this keeps a save-less, no-plant world
        // byte-for-byte (no chunks/ dir created); a planted world still persists its plants here.
        Persistence::WorldSaveService::save_plant_entities(plant_snapshot, save_dir, nullptr);
        SaveEnergyFieldRecord(save_dir);
        if (report) {
            *report = result;
        }
        return true;
    }

    Persistence::WorldSaveService service;
    std::vector<std::string> errors;
    // Subsequent saves rewrite the edited regions of a supported container.
    const bool has_snapshot = Persistence::WorldSaveService::has_world_save(save_dir);

    bool ok = false;
    if (!has_snapshot) {
        // First save of this world: write the full snapshot, then clear the
        // dirty flags exactly like the incremental path does.
        ok = service.save_world(state, save_dir, &errors);
        if (ok) {
            for (const ChunkID id : dirty_ids) {
                if (const auto chunk = state.find_chunk(id)) {
                    chunk->clear_voxel_data_dirty();
                }
            }
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
    SaveEnergyFieldRecord(save_dir);

    for (const std::string& error : errors) {
        LUMINUMBRA_CORE_ERROR("World state save failed: {}", error);
    }
    if (ok) {
        result.saved = true;
        result.chunks_saved = result.chunks_total; // whole-snapshot layout
        LUMINUMBRA_CORE_INFO("World state saved: {} chunks ({} dirty) -> {}",
                             result.chunks_total,
                             result.chunks_dirty,
                             save_dir.string());
    }
    if (report) {
        *report = result;
    }
    return ok;
}

bool GameSession::LoadWorldState() {
    return LoadWorldStateFrom(GetWorldSaveDir());
}

bool GameSession::LoadWorldStateFrom(const std::filesystem::path& save_dir) {
    m_lastLoadedChunkCount = 0;
    if (!m_worldOpenError.empty() || !m_worldSystem || save_dir.empty()) {
        return false;
    }

    Persistence::WorldSaveService service;
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    if (!service.load_world(loaded, save_dir, errors)) {
        if (errors.empty())
            return true; // fresh world
        m_worldOpenError = errors.front();
        m_worldSystem->clear_world(m_physicsSystem.get());
        for (const std::string& error : errors) {
            LUMINUMBRA_CORE_ERROR("World state load failed: {}", error);
        }
        return false;
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

    //  reload persisted plant entities into the live registry (a missing file is
    // a clean miss). Geometry rebakes from the reloaded genome+stage on the render bridge.
    {
        Luminumbra::Ecs::EntityRegistrySnapshot plant_snapshot;
        std::vector<std::string> plant_errors;
        if (Persistence::WorldSaveService::load_plant_entities(
                plant_snapshot, save_dir, &plant_errors)) {
            luminumbra::foliage::ApplyPlantEntitySnapshot(m_registry, plant_snapshot);
        } else {
            for (const std::string& e : plant_errors)
                LUMINUMBRA_CORE_ERROR("Plant state load failed: {}", e);
        }
    }

    // restore the stateful energy layer's record (a
    // missing file is a clean miss == all zeros by contract). Epoch-rebased
    // onto the loaded session's tick stream (-4).
    if (m_energyFieldState) {
        const fs::path recordPath = save_dir / "aether_state.efs";
        std::ifstream recordFile(recordPath);
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

void GameSession::SaveEnergyFieldRecord(const std::filesystem::path& save_dir) {
    if (!m_worldOpenError.empty())
        return;
    if (!m_energyFieldState || m_energyFieldState->page_count() == 0 || save_dir.empty()) {
        return; // null/all-zero layer -> no file -> byte-identical saves
    }
    const std::string record = m_energyFieldState->SerializeRecord(GetSimulationTickCount());
    std::ofstream file(save_dir / "aether_state.efs", std::ios::trunc);
    if (!file.is_open()) {
        LUMINUMBRA_CORE_ERROR("Failed to write aether state record under {}", save_dir.string());
        return;
    }
    file << record;
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
