#include "GameSession.h"
#include "../systems/SHIELD_WorldSystem.h" // This includes TerrainGenParams
#include "../core/JobSystem.h"
#include "nlohmann/json.hpp" // For parsing JSON
#include "../systems/PhysicsSystem.h"
#include "../systems/WaterSystem.h"
#include "../core/Log.h"
#include "../persistence/WorldSaveService.h"
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

bool JsonHasObject(const nlohmann::json& data, const char* key) {
    return data.contains(key) && data[key].is_object();
}

bool LoadTerrainParamsFromPreset(const fs::path& preset_path, TerrainGenParams& params, std::vector<std::string>& errors) {
    std::ifstream file(preset_path);
    if (!file.is_open()) {
        errors.push_back("failed to open world preset: " + preset_path.string());
        return false;
    }

    nlohmann::json data;
    try {
        data = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        errors.push_back("failed to parse world preset JSON '" + preset_path.string() + "': " + e.what());
        return false;
    }

    if (!JsonHasObject(data, "generation_params")) {
        errors.push_back("world preset is missing object generation_params: " + preset_path.string());
        return false;
    }

    const auto& gen_params = data["generation_params"];
    if (!JsonHasObject(gen_params, "terrain")) {
        errors.push_back("world preset is missing object generation_params.terrain: " + preset_path.string());
        return false;
    }
    if (!JsonHasObject(gen_params, "features")) {
        errors.push_back("world preset is missing object generation_params.features: " + preset_path.string());
        return false;
    }

    const auto& terrain = gen_params["terrain"];
    const auto& features = gen_params["features"];
    for (const char* key : {"base_frequency", "base_amplitude", "octaves", "persistence", "lacunarity", "height_offset"}) {
        if (!terrain.contains(key) || !terrain[key].is_number()) {
            errors.push_back(std::string("world preset terrain field must be numeric: ") + key);
        }
    }
    if (!features.contains("caves_enabled") || !features["caves_enabled"].is_boolean()) {
        errors.push_back("world preset feature caves_enabled must be boolean");
    }
    if (!features.contains("cave_frequency") || !features["cave_frequency"].is_number()) {
        errors.push_back("world preset feature cave_frequency must be numeric");
    }

    if (!errors.empty()) {
        return false;
    }

    params.base_frequency = terrain.value("base_frequency", 0.01f);
    params.base_amplitude = terrain.value("base_amplitude", 50.0f);
    params.octaves = terrain.value("octaves", 4);
    params.persistence = terrain.value("persistence", 0.5f);
    params.lacunarity = terrain.value("lacunarity", 2.0f);
    params.height_offset = terrain.value("height_offset", 0.0f);
    params.island_mask_enabled = terrain.value("island_mask_enabled", false);
    params.island_mask_frequency = terrain.value("island_mask_frequency", 0.004f);
    params.caves_enabled = features.value("caves_enabled", true);
    params.cave_frequency = features.value("cave_frequency", 0.02f);
    return true;
}
}

namespace Luminumbra::world {

GameSession::GameSession() {
    // Constructor
}

GameSession::~GameSession() {
    // Destructor
}

WorldConfigValidationResult GameSession::ValidateWorldConfig(const std::string& root_path, const std::string& worldType) {
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

    const std::vector<fs::path> required_assets = {
        root / "res" / "shaders" / "basic.vert",
        root / "res" / "shaders" / "g_buffer.frag",
        root / "res" / "shaders" / "sdf_generation.compute",
        root / "data" / "ui" / "main_menu.rml",
        root / "data" / "fonts" / "Lora" / "static" / "Lora-Regular.ttf",
    };

    for (const fs::path& path : required_assets) {
        if (!fs::exists(path)) {
            AddValidationError(result, "missing required runtime asset: " + path.string());
        }
    }

    if (result.ok) {
        TerrainGenParams params;
        std::vector<std::string> parse_errors;
        if (!LoadTerrainParamsFromPreset(result.preset_path, params, parse_errors)) {
            for (const std::string& error : parse_errors) {
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

    const WorldConfigValidationResult validation = ValidateWorldConfig(m_rootPath, worldType);
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

    TerrainGenParams params;
    std::vector<std::string> parse_errors;
    if (!LoadTerrainParamsFromPreset(validation.preset_path, params, parse_errors)) {
        for (const std::string& error : parse_errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return false;
    }
    
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

    // Calculate appropriate spawn point based on actual terrain height
    float spawn_x = 8.0f;
    float spawn_z = 8.0f;
    float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
    LUMINUMBRA_CORE_INFO("Initial terrain height sampled at spawn: {}.", terrain_height);
    
    m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + kSpawnEyeHeight, spawn_z);
    
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
    } catch (const nlohmann::json::parse_error& e) {
        LUMINUMBRA_CORE_ERROR("Failed to parse world metadata file '{}': {}", metadataPath, e.what());
        return false;
    }

    const WorldConfigValidationResult validation = ValidateWorldConfig(m_rootPath, m_metadata.worldType);
    if (!validation.ok) {
        for (const std::string& error : validation.errors) {
            LUMINUMBRA_CORE_ERROR("World config validation failed: {}", error);
        }
        return false;
    }
    
    // --- Load Generation Preset ---
    TerrainGenParams params;
    std::vector<std::string> parse_errors;
    if (!LoadTerrainParamsFromPreset(validation.preset_path, params, parse_errors)) {
        for (const std::string& error : parse_errors) {
            LUMINUMBRA_CORE_ERROR("World preset load failed: {}", error);
        }
        return false;
    }

    int world_seed = StringToSeed(m_metadata.seed);

    // Initialize Physics System after config and preset validation passes.
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    m_worldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(m_jobSystem, nullptr, params, world_seed);
    m_waterSystem = std::make_unique<Systems::WaterSystem>(m_jobSystem, m_worldSystem.get());
    m_worldSystem->SetWaterSystem(m_waterSystem.get());

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
    std::error_code exists_error;
    const bool has_snapshot =
        fs::exists(Persistence::WorldSaveService::world_state_path(save_dir), exists_error) && !exists_error;

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

} // namespace Luminumbra::world
