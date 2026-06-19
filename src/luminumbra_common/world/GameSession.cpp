#include "GameSession.h"
#include "../ai/InstinctSystem.h"
#include "../ai/CreatureBrainSystem.h"
#include "../ai/CreatureReproductionSystem.h"  // Track (a): generational evolution (+16)
#include "../ai/InstinctLocomotionSystem.h"
#include "../ai/PerceptionSystem.h"
#include "../ai/ScentDepositSystem.h"
#include "../ai/ScentField.h"
#include "../ai/ScentSteeringSystem.h"
#include "../ai/StimulusChannels.h"
#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"
#include "../components/PlantComponents.h"          // I9-FOLIAGE plant pillar
#include "../systems/PlantGrowthSystem.h"           // I9-FOLIAGE growth tick
#include "../animation/AnimationRuntime.h"
#include "../systems/SHIELD_WorldSystem.h" // This includes TerrainGenParams
#include "../core/JobSystem.h"
#include "nlohmann/json.hpp" // For parsing JSON
#include "../systems/PhysicsSystem.h"
#include "../systems/WaterSystem.h"
#include "../systems/WindFieldSystem.h"
#include "../systems/WeatherSystem.h"
#include "../systems/AetherFieldSystem.h"
#include "../core/Log.h"
#include "../persistence/WorldSaveService.h"
#include "../persistence/WorldPersistenceRoundtrip.h" // Persistence::StableChecksum (ComputeScentSubHash)
#include "TerrainPresetLoader.h"
#include "WorldStreamingState.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>
#include <filesystem>
#include <chrono>
#include <utility>

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

void AddValidationError(Luminumbra::world::WorldConfigValidationResult& result, std::string error) {
    result.errors.push_back(std::move(error));
    result.ok = false;
}

bool IsSafeWorldType(const std::string& world_type) {
    return !world_type.empty() &&
           world_type.find("..") == std::string::npos &&
           world_type.find('/') == std::string::npos &&
           world_type.find('\\') == std::string::npos;
}

fs::path RuntimeRoot(const std::string& root_path) {
    return root_path.empty() ? fs::path(".") : fs::path(root_path);
}

fs::path PresetPathFor(const std::string& root_path, const std::string& world_type) {
    return RuntimeRoot(root_path) / "worlds" / "atlas" / "presets" / (world_type + ".json");
}

float ScentOriginFor(float anchor) {
    return anchor - (static_cast<float>(kScentFieldCells) * kScentCellSize * 0.5f);
}
}

namespace Luminumbra::world {

GameSession::GameSession() {
    // Constructor
}

GameSession::~GameSession() {
    // Destructor
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

        // Deterministic per-tick system order (design-decisions.md §1).
        // Remaining placeholder slots until the owning iteration-3/4 tasks
        // land:
        //   3. Field budget (iteration 4)
        //   4. Queued world edits
        // Finally, the ordered event bus drains everything published for
        // this tick (tick -> lane -> sequence order).

        // 1. Animation pose sampling (T-I3-15): FIRST in the tick order.
        luminumbra::animation::SamplePosesOnTick(m_registry, m_simulationClock.fixed_dt());

        // 2. Instinct planning (T-I3-17): need growth + deterministic
        // replanning over the registry.
        //
        // T-I5b-2 (E1): the ecology stimulus-channel registry is supplied ONLY
        // when at least one creature has OPTED IN via a game-data
        // StimulusSubscriptionComponent. The canonical roster carries none, so
        // the `view.empty()` guard keeps the call BYTE-IDENTICAL to the pre-5b
        // path (nullptr context) -- world_hash stays d950a6afc12a5cdc (critique
        // F1 / §0). When subscribers exist, the context reads the replicated
        // weather state (one-way; weather updated on the PREVIOUS tick, slot 4)
        // and the per-tick time-of-day/season/light channels derived from the
        // tick. The engine still names no creature behavior.
        if (m_registry.view<const Luminumbra::Components::StimulusSubscriptionComponent>().empty()) {
            luminumbra::ai::RunInstinctSystemOnTick(m_registry, current_tick);
        } else {
            luminumbra::ai::StimulusContext stimulus_context;
            stimulus_context.tick = current_tick;
            stimulus_context.sample_position = m_metadata.spawnPoint;
            stimulus_context.weather = m_weatherSystem.get();
            const luminumbra::ai::StimulusChannelRegistry stimulus_registry(stimulus_context);
            luminumbra::ai::RunInstinctSystemOnTick(m_registry, current_tick, &stimulus_registry);
        }

        // 2b. T-I9-AI E1: perception fusion. Updates each perceiving creature's
        // awareness (vision cone + hearing audiogram over Sensable entities of
        // other factions -> detection meter/state + last-known memory). Runs after
        // planning so awareness is fresh for the NEXT plan/locomotion. Operates
        // ONLY on entities carrying PerceptionComponent+AwarenessComponent; the
        // canonical headless roster carries none, so this is a no-op there and
        // world_hash is UNCHANGED (byte-identical) -- same discipline as the
        // stimulus-channel opt-in above. No RNG/wall-clock; id-ordered.
        luminumbra::ai::RunPerceptionSystemOnTick(m_registry, m_simulationClock.fixed_dt());

        // 2c. T-I7-ECO-RENDER: scent stigmergy write/update. Game-data opt-in:
        // no scent emitter/sensor components means no field mutation, while active
        // ecology worlds get deterministic deposit -> diffuse/evaporate -> clamp.
        const bool scent_active = HasScentParticipants();
        if (scent_active && m_scentField) {
            luminumbra::ai::RunScentDepositOnTick(
                m_registry, *m_scentField, ScentOriginFor(m_metadata.spawnPoint.x),
                ScentOriginFor(m_metadata.spawnPoint.z), kScentCellSize);
            m_scentField->Step(kScentDiffusionRate, kScentDiffusionIterations, kScentEvaporation);
            m_scentField->Clamp(kScentTauMin, kScentTauMax);
        }

        // 2d. T-I7-ECO-RENDER: action-plan locomotion, then scent gradient bias.
        // The executor writes only LocomotionIntentComponent; physics/render owners
        // consume that intent in their existing lanes.
        luminumbra::ai::RunInstinctLocomotionOnTick(m_registry);
        if (scent_active && m_scentField) {
            luminumbra::ai::RunScentSteeringOnTick(
                m_registry, *m_scentField, ScentOriginFor(m_metadata.spawnPoint.x),
                ScentOriginFor(m_metadata.spawnPoint.z), kScentCellSize);
        }

        // 2e. I9-ECO: Utility-AI creature brain (predator/prey movement). Per-entity opt-in
        // via CreatureComponent -> a world with none is a no-op (canonical roster byte-identical,
        // same discipline as plants/scent). Deterministic (id-ordered, libm-free).
        {
            auto creatures = m_registry.view<const Luminumbra::Components::CreatureComponent>();
            if (creatures.begin() != creatures.end()) {
                luminumbra::ai::RunCreatureBrainSystemOnTick(
                    m_registry, static_cast<float>(m_simulationClock.fixed_dt()));

                // 2e-mate: SEXUAL reproduction, phase A. Ready creatures (mature, well-fed,
                // off cooldown) carrying a CreatureGenomeComponent steer toward the nearest
                // ready OPPOSITE-SEX mate by overriding the brain's wish velocity (unless
                // fleeing) — so the physics bridge below actually walks them together. Opt-in
                // (genome component); no genome -> untouched.
                luminumbra::ai::RunMateSeekingOnTick(m_registry);

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
                            const auto& cp = phys.get<Luminumbra::Components::CreaturePhysicsComponent>(e);
                            m_physicsSystem->set_avatar_wish_velocity(cp.avatar_index,
                                                                      glm::vec2(cr.wish_x, cr.wish_z));
                        }
                        m_physicsSystem->update_avatars(static_cast<float>(m_simulationClock.fixed_dt()));
                        for (auto e : pe) {
                            const auto& cp = phys.get<Luminumbra::Components::CreaturePhysicsComponent>(e);
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
                const auto repro = luminumbra::ai::RunMatingResolveOnTick(m_registry, current_tick);
                if (repro.born > 0) {
                    LUMINUMBRA_CORE_INFO("I9-EVO: {} offspring born (sexual) at tick {}", repro.born, current_tick);
                }
            }
        }

        // 3. T-I5a-2 (A2): wind field update. Deterministic (DeterministicMath +
        // FastNoise batch path; no wall-clock/RNG). Anchored on the spawn/stream
        // anchor so the streamed-region grid follows it. The wind cell values
        // feed the world_hash `wind` sub-hash; the update must run every tick so
        // run==replay and resim agree on the field at every checkpoint.
        if (m_windFieldSystem) {
            m_windFieldSystem->Update(current_tick, m_metadata.spawnPoint);
        }

        // 4. T-I5a-3 (B1): weather core update. Runs AFTER wind so storm cells
        // advect by the freshly-updated wind grid. Deterministic (DeterministicMath
        // + FastNoise batch path; no wall-clock/RNG, no std::random). The weather
        // state (category map + storm cells + precip field) feeds the world_hash
        // `weather` sub-hash; the update runs every tick so run==replay and resim
        // agree on the state at every checkpoint.
        if (m_weatherSystem) {
            m_weatherSystem->Update(current_tick, m_metadata.spawnPoint, m_windFieldSystem.get());
        }

        // 5. T-I6-A1: Aetheric scalar field update. Runs AFTER weather so it
        // advects its emission source by the freshly-updated wind grid (and so
        // any future weather coupling reads the current weather). Deterministic
        // (pure function of seed+14, tick, origin[, wind]; DeterministicMath +
        // FastNoise batch path; no wall-clock/RNG). The cell values feed the
        // world_hash `aether` sub-hash; the update runs every tick so run==replay
        // and resim agree on the field at every checkpoint.
        if (m_aetherFieldSystem) {
            m_aetherFieldSystem->Update(current_tick, m_metadata.spawnPoint, m_windFieldSystem.get());
        }

        // 6. I9-FOLIAGE: deterministic plant GROWTH. Game-data opt-in (PlantTag):
        // no plants -> the system never runs and world_hash stays byte-identical
        // (same discipline as scent). The environment is ATMOSPHERIC — moisture is
        // driven by the freshly-updated weather precip field (rain -> growth), so
        // growth runs AFTER weather. Integer/fixed-point + id-ordered = run==replay.
        // (temperature/light/soil coupling are follow-ups; neutral for now.)
        if (HasPlantParticipants()) {
            luminumbra::foliage::EnvSampler plant_env =
                [this](const Luminumbra::Components::TransformComponent& tf) {
                    luminumbra::foliage::PlantEnvSample s;
                    const Luminumbra::Vec3 p = tf.position;
                    // Moisture: weather precipitation (rain -> growth).
                    const float precip = m_weatherSystem ? m_weatherSystem->PrecipitationAt(p) : 0.0f;
                    s.moisture = luminumbra::foliage::clamp01(0.30f + precip * 0.70f);
                    // Soil + temperature from the TERRAIN: surface-material favourability
                    // (grass/soil rich, sand/stone poor) and an altitude lapse (higher
                    // ground is colder -> alpine vs lowland growth). The real atmospheric/
                    // terrain coupling. (Only runs for opt-in sim plants, so the per-plant
                    // world queries are bounded.)
                    float soil = 0.5f, temp = 0.55f;
                    if (m_worldSystem) {
                        const float th = m_worldSystem->GetTerrainHeightAt(p.x, p.z);
                        switch (m_worldSystem->SurfaceVertexMaterial(p.x, p.z, th)) {
                            case Luminumbra::MaterialType::Grass: soil = 0.95f; break;
                            case Luminumbra::MaterialType::Soil:  soil = 0.85f; break;
                            case Luminumbra::MaterialType::Sand:  soil = 0.45f; break;
                            case Luminumbra::MaterialType::Stone: soil = 0.30f; break;
                            default:                              soil = 0.25f; break;
                        }
                        const float altitude = th - static_cast<float>(Luminumbra::SEA_LEVEL);
                        temp = luminumbra::foliage::clamp01(
                            0.60f - (altitude > 0.0f ? altitude : 0.0f) * 0.00045f);
                    }
                    s.soil_quality = soil;
                    s.temperature = temp;
                    s.light = 0.75f; // season/time-of-day coupling is a render-side follow-up
                    return s;
                };
            luminumbra::foliage::RunPlantGrowthSystemOnTick(m_registry, current_tick, plant_env);
        }

        m_simulationEventBus.drain(current_tick);
    }
    return ticks_executed;
}

WorldConfigValidationResult GameSession::ValidateWorldConfig(
    const std::string& root_path,
    const std::string& worldType,
    const std::vector<std::filesystem::path>& required_assets) {
    WorldConfigValidationResult result;
    result.ok = true;

    if (!IsSafeWorldType(worldType)) {
        AddValidationError(result, "world type must be non-empty and must not contain path separators: " + worldType);
        return result;
    }

    const fs::path root = RuntimeRoot(root_path);
    result.preset_path = PresetPathFor(root_path, worldType);
    if (!fs::exists(result.preset_path)) {
        AddValidationError(result, "missing world preset: " + result.preset_path.string());
    }

    // T-I3-6: simulation needs only the preset. Any further runtime assets
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

bool GameSession::CreateWorld(const std::string& name, const std::string& seed, const std::string& worldType) {
    if (!m_jobSystem) {
        LUMINUMBRA_CORE_ERROR("JobSystem not set before creating world");
        return false;
    }

    const WorldConfigValidationResult validation = ValidateWorldConfig(m_rootPath, worldType, m_requiredClientAssets);
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

    const TerrainPresetLoadResult preset = LoadTerrainPreset(validation.preset_path);
    if (!preset.ok) {
        for (const std::string& error : preset.errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return false;
    }
    const TerrainGenParams& params = preset.params;

    int world_seed = StringToSeed(m_metadata.seed);
    LUMINUMBRA_CORE_INFO("Loaded world preset '{}': height_offset={}, amplitude={}, caves={}", 
        worldType, params.height_offset, params.base_amplitude, params.caves_enabled);

     // 1. Create the World System
    m_worldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(m_jobSystem, nullptr, params, world_seed);
    LUMINUMBRA_CORE_INFO("World system initialized for seed {}.", world_seed);

    // 2. Create the Water System
    m_waterSystem = std::make_unique<Systems::WaterSystem>(m_jobSystem, m_worldSystem.get());
    LUMINUMBRA_CORE_INFO("Water system initialized.");

    // 3. Link them together
    m_worldSystem->SetWaterSystem(m_waterSystem.get());
    LUMINUMBRA_CORE_INFO("World and water systems linked.");

    // 4. T-I5a-2 (A2): the deterministic wind field. Pure function of the world
    //    seed (uses seed+11 for its base-direction noise); updated per tick.
    m_windFieldSystem = std::make_unique<Systems::WindFieldSystem>(world_seed);
    LUMINUMBRA_CORE_INFO("Wind field system initialized.");

    // 5. T-I5a-3 (B1): the deterministic weather core. Pure function of the world
    //    seed (uses seed+12 for its pressure/climate noise + storm schedule);
    //    updated per tick AFTER wind (advects storm cells by the wind grid).
    m_weatherSystem = std::make_unique<Systems::WeatherSystem>(world_seed);
    LUMINUMBRA_CORE_INFO("Weather system initialized.");

    // 6. T-I6-A1: the deterministic Aetheric scalar field. Pure function of the
    //    world seed (uses seed+14 for its emission noise); updated per tick AFTER
    //    weather, advected by the wind grid. Feeds the world_hash `aether` slot.
    m_aetherFieldSystem = std::make_unique<Systems::AetherFieldSystem>(world_seed);
    LUMINUMBRA_CORE_INFO("Aether field system initialized.");

    // Calculate appropriate spawn point based on actual terrain height
    float spawn_x = 8.0f;
    float spawn_z = 8.0f;
    float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
    LUMINUMBRA_CORE_INFO("Initial terrain height sampled at spawn: {}.", terrain_height);
    
    m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + kSpawnEyeHeight, spawn_z);
    InitializeScentField(m_metadata.spawnPoint);
    
    LUMINUMBRA_CORE_INFO("World created successfully: {} (ID: {})", m_metadata.name, m_metadata.worldId);
    LUMINUMBRA_CORE_INFO("Spawn point set to ({}, {}, {}) - terrain height: {}", 
        m_metadata.spawnPoint.x, m_metadata.spawnPoint.y, m_metadata.spawnPoint.z, terrain_height);
    
    // Save world metadata
    if (!SaveWorld()) {
        LUMINUMBRA_CORE_ERROR("Failed to save world metadata");
        return false;
    }
    return true;
}

bool GameSession::LoadWorld(const std::string& worldId) {
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
    try {
        metadata_json = nlohmann::json::parse(metadata_file);
        m_metadata.name = metadata_json.value("name", "Unnamed World");
        m_metadata.seed = metadata_json.value("seed", "0");
        m_metadata.worldType = metadata_json.value("worldType", "default");
        m_metadata.worldId = worldId;
        m_metadata.creationTime = metadata_json.value("creationTime", 0);
        // T-I3-13 (minimal additive hook): restore the persisted spawn point
        // so a headless host booting an existing save anchors its chunk
        // streaming where the world was created, not at the origin. Saves
        // written before spawnPoint existed fall through to the terrain
        // sample below.
        if (metadata_json.contains("spawnPoint")) {
            const nlohmann::json& spawn_json = metadata_json.at("spawnPoint");
            m_metadata.spawnPoint = Vec3(
                spawn_json.value("x", 0.0f),
                spawn_json.value("y", 0.0f),
                spawn_json.value("z", 0.0f));
        }
    } catch (const nlohmann::json::parse_error& e) {
        LUMINUMBRA_CORE_ERROR("Failed to parse world metadata file '{}': {}", metadataPath, e.what());
        return false;
    }

    const WorldConfigValidationResult validation = ValidateWorldConfig(m_rootPath, m_metadata.worldType, m_requiredClientAssets);
    if (!validation.ok) {
        for (const std::string& error : validation.errors) {
            LUMINUMBRA_CORE_ERROR("World config validation failed: {}", error);
        }
        return false;
    }
    
    // --- Load Generation Preset ---
    const TerrainPresetLoadResult preset = LoadTerrainPreset(validation.preset_path);
    if (!preset.ok) {
        for (const std::string& error : preset.errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return false;
    }
    const TerrainGenParams& params = preset.params;

    int world_seed = StringToSeed(m_metadata.seed);

    // Initialize Physics System after config and preset validation passes.
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    m_worldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(m_jobSystem, nullptr, params, world_seed);
    m_waterSystem = std::make_unique<Systems::WaterSystem>(m_jobSystem, m_worldSystem.get());
    m_worldSystem->SetWaterSystem(m_waterSystem.get());

    // T-I5a-2 (A2): the wind field is a pure function of the world seed, so a
    // loaded world reconstructs the identical field (the heavy-oracle/replay
    // resim reaches the same wind sub-hash at the same tick).
    m_windFieldSystem = std::make_unique<Systems::WindFieldSystem>(world_seed);

    // T-I5a-3 (B1): weather is likewise a pure function of (world seed, tick,
    // anchor). A loaded world reconstructs the identical weather core. NOTE: like
    // wind, the loaded session's tick counter starts at 0, so the loaded session
    // reproduces the SAME-TICK state, not the original's absolute-tick state --
    // the heavy-oracle cross-phase compare excludes weather for this reason
    // (documented in main_server.cpp AuthoritativeStateEqual), exactly as wind is.
    m_weatherSystem = std::make_unique<Systems::WeatherSystem>(world_seed);

    // T-I6-A1: aether is likewise a pure function of (world seed, tick, anchor);
    // a loaded world reconstructs the identical field at the same tick (heavy-
    // oracle cross-phase compare excludes it for the same loaded-tick-zero reason
    // as wind/weather).
    m_aetherFieldSystem = std::make_unique<Systems::AetherFieldSystem>(world_seed);

    // Legacy saves without a persisted spawnPoint: derive it from terrain
    // height exactly like CreateWorld does (pure function of seed/params).
    if (!metadata_json.contains("spawnPoint")) {
        const float spawn_x = 8.0f;
        const float spawn_z = 8.0f;
        const float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
        m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + kSpawnEyeHeight, spawn_z);
    }
    InitializeScentField(m_metadata.spawnPoint);

    LUMINUMBRA_CORE_INFO("World loaded successfully: {}", m_metadata.name);
    return true;
}

bool GameSession::SaveWorld() {
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
        {"spawnPoint", {
            {"x", m_metadata.spawnPoint.x},
            {"y", m_metadata.spawnPoint.y},
            {"z", m_metadata.spawnPoint.z}
        }}
    };

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

bool GameSession::SaveWorldStateTo(const std::filesystem::path& save_dir, WorldStateSaveReport* report) {
    WorldStateSaveReport result;
    if (report) {
        *report = result;
    }
    if (!m_worldSystem || save_dir.empty()) {
        return false;
    }

    // Quiesce in-flight generation/meshing so chunk data is stable on disk.
    m_worldSystem->wait_for_streaming_jobs();

    WorldStreamingState state;
    for (const auto& chunk : m_worldSystem->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }
    result.chunks_total = state.size();

    const std::vector<ChunkID> dirty_ids = state.dirty_chunk_ids();
    result.chunks_dirty = dirty_ids.size();
    if (dirty_ids.empty()) {
        // Nothing to persist. Without an existing snapshot this keeps a
        // save-less world byte-for-byte on the fresh-world path (the chunks/
        // directory is never created).
        if (report) {
            *report = result;
        }
        return true;
    }

    Persistence::WorldSaveService service;
    std::vector<std::string> errors;
    // T-I3-7: detects both the legacy v1 snapshot and the v2 LMR1 region
    // container so subsequent saves take the O(edited regions) path.
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
        const Persistence::WorldSaveDirtyReport dirty_report = service.save_dirty_chunks(state, save_dir, &errors);
        ok = dirty_report.saved;
    }

    for (const std::string& error : errors) {
        LUMINUMBRA_CORE_ERROR("World state save failed: {}", error);
    }
    if (ok) {
        result.saved = true;
        result.chunks_saved = result.chunks_total; // whole-snapshot layout
        LUMINUMBRA_CORE_INFO("World state saved: {} chunks ({} dirty) -> {}",
            result.chunks_total, result.chunks_dirty, save_dir.string());
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
    if (!m_worldSystem || save_dir.empty()) {
        return false;
    }

    Persistence::WorldSaveService service;
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    if (!service.load_world(loaded, save_dir, errors)) {
        // Missing snapshot is a clean miss: errors stays empty and the world
        // proceeds on the untouched fresh-generation path.
        for (const std::string& error : errors) {
            LUMINUMBRA_CORE_ERROR("World state load failed: {}", error);
        }
        return false;
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

    m_lastLoadedChunkCount = adopted;
    LUMINUMBRA_CORE_INFO("World state loaded: {} chunks adopted ({} in snapshot) from {}",
        adopted, loaded.size(), save_dir.string());
    return true;
}

std::string GameSession::GenerateWorldId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    std::stringstream ss;
    ss << "world_" << timestamp << "_" << dis(gen);
    return ss.str();
}

uint32_t GameSession::StringToSeed(const std::string& seedStr) {
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

void GameSession::InitializeScentField(const Vec3& /*anchor*/) {
    m_scentField = std::make_unique<luminumbra::ai::ScentField>(
        kScentFieldCells, kScentFieldCells, kScentFieldChannels);
    LUMINUMBRA_CORE_INFO(
        "Scent field initialized: {}x{} cells, {} channel(s), cell_size={}",
        kScentFieldCells, kScentFieldCells, kScentFieldChannels, kScentCellSize);
}

bool GameSession::HasScentParticipants() const {
    {
        auto emitters =
            m_registry.view<const Luminumbra::Components::TransformComponent,
                            const Luminumbra::Components::SensableComponent>();
        for (auto e : emitters) {
            const auto& s = emitters.get<
                const Luminumbra::Components::SensableComponent>(e);
            if (s.scent_channel >= 0 && s.scent_deposit > 0.0f) {
                return true;
            }
        }
    }
    {
        auto sensors =
            m_registry.view<const Luminumbra::Components::TransformComponent,
                            const Luminumbra::Components::ScentSenseComponent>();
        for (auto e : sensors) {
            const auto& s = sensors.get<
                const Luminumbra::Components::ScentSenseComponent>(e);
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
    bytes << "scent:v1:"
          << m_scentField->width() << ':'
          << m_scentField->height() << ':'
          << m_scentField->channels() << ':'
          << std::setprecision(17);
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

std::string GameSession::ComputePlantSubHash() const {
    if (!HasPlantParticipants()) {
        return {};
    }
    // Hash the id-ordered sequence of integer growth state (geometry is visual-only;
    // THIS is the sim truth). id-robust: we hash the ordered STATE, not raw ids.
    auto view = m_registry.view<const Luminumbra::Components::PlantTag,
                                const Luminumbra::Components::PlantGrowthComponent>();
    std::vector<entt::entity> ents;
    for (auto e : view) ents.push_back(e);
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
