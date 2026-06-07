#include "GameSession.h"
#include "../systems/SHIELD_WorldSystem.h" // This includes TerrainGenParams
#include "../core/JobSystem.h"
#include "nlohmann/json.hpp" // For parsing JSON
#include "../systems/PhysicsSystem.h"
#include "../systems/WaterSystem.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>
#include <filesystem>
#include <chrono>
#include "../core/Log.h"

namespace fs = std::filesystem;

// A using-declaration to make the code cleaner. This brings the struct into the current scope.
using Luminumbra::Systems::TerrainGenParams;

namespace Luminumbra::world {

GameSession::GameSession() {
    // Constructor
}

GameSession::~GameSession() {
    // Destructor
}

bool GameSession::CreateWorld(const std::string& name, const std::string& seed, const std::string& worldType) {
    if (!m_jobSystem) {
        std::cerr << "Error: JobSystem not set before creating world!" << std::endl;
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
        std::cerr << "Failed to create world directory: " << e.what() << std::endl;
        return false;
    }

    // Initialize Physics System
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    // Load the generation preset based on worldType
    std::string presetPath = m_rootPath + "worlds/atlas/presets/" + worldType + ".json";
    std::ifstream f(presetPath);
    if (!f.is_open()) {
        std::cerr << "Error: Failed to open world preset file: " << presetPath << std::endl;
        return false;
    }

    nlohmann::json data;
    try {
        data = nlohmann::json::parse(f);
    } catch (const nlohmann::json::parse_error& e) {
        // Use your logger here if available, otherwise cerr is fine.
        std::cerr << "FATAL: Failed to parse world preset JSON '" << presetPath << "'. Error: " << e.what() << std::endl;
        return false; // Return false to prevent the game from entering a broken state
    }

    TerrainGenParams params; // FIX: This now works because of the 'using' declaration above

    // Safely parse values from JSON
    auto& gen_params = data["generation_params"];
    params.base_frequency = gen_params["terrain"].value("base_frequency", 0.01f);
    params.base_amplitude = gen_params["terrain"].value("base_amplitude", 50.0f);
    params.octaves = gen_params["terrain"].value("octaves", 4);
    params.persistence = gen_params["terrain"].value("persistence", 0.5f);
    params.lacunarity = gen_params["terrain"].value("lacunarity", 2.0f);
    params.height_offset = gen_params["terrain"].value("height_offset", 0.0f);
    params.island_mask_enabled = gen_params["terrain"].value("island_mask_enabled", false);
    params.island_mask_frequency = gen_params["terrain"].value("island_mask_frequency", 0.004f);
    params.caves_enabled = gen_params["features"].value("caves_enabled", true);
    params.cave_frequency = gen_params["features"].value("cave_frequency", 0.02f);
    
    int world_seed = StringToSeed(m_metadata.seed);
    LUMINUMBRA_CORE_INFO("Loaded world preset '{}': height_offset={}, amplitude={}, caves={}", 
        worldType, params.height_offset, params.base_amplitude, params.caves_enabled);

     // 1. Create the World System
    m_worldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(m_jobSystem, nullptr, params, world_seed);

    // 2. Create the Water System
    m_waterSystem = std::make_unique<Systems::WaterSystem>(m_jobSystem, m_worldSystem.get());

    // 3. Link them together
    m_worldSystem->SetWaterSystem(m_waterSystem.get());

    // Calculate appropriate spawn point based on actual terrain height
    float spawn_x = 0.0f;
    float spawn_z = 0.0f;
    float terrain_height = m_worldSystem->GetTerrainHeightAt(spawn_x, spawn_z);
    
    // TEMP: Spawn much higher to see terrain from above and debug visibility
    m_metadata.spawnPoint = Vec3(spawn_x, terrain_height + 50.0f, spawn_z);
    LUMINUMBRA_CORE_WARN("SPAWN DEBUG: Moving player to Y={} (terrain={} + 50)", 
        m_metadata.spawnPoint.y, terrain_height);
    
    LUMINUMBRA_CORE_INFO("World created successfully: {} (ID: {})", m_metadata.name, m_metadata.worldId);
    LUMINUMBRA_CORE_INFO("Spawn point set to ({}, {}, {}) - terrain height: {}", 
        m_metadata.spawnPoint.x, m_metadata.spawnPoint.y, m_metadata.spawnPoint.z, terrain_height);
    
    // Save world metadata
    if (!SaveWorld()) {
        std::cerr << "Failed to save world metadata!" << std::endl;
        return false;
    }
    return true;
}

bool GameSession::LoadWorld(const std::string& worldId) {
    if (!m_jobSystem) {
        // FIX: Replaced logging macro with std::cerr
        std::cerr << "Error: JobSystem not set before loading world!" << std::endl;
        return false;
    }

    std::string worldPath = m_rootPath + "worlds/saves/" + worldId;
    std::string metadataPath = worldPath + "/world_info.json";

    if (!fs::exists(metadataPath)) {
        // FIX: Replaced logging macro with std::cerr
        std::cerr << "Error: World not found: " << worldId << std::endl;
        return false;
    }

    // Initialize Physics System
    m_physicsSystem = std::make_unique<Systems::PhysicsSystem>();
    m_physicsSystem->startup();

    // --- Load Metadata from world_info.json ---
    std::ifstream metadata_file(metadataPath);
    nlohmann::json metadata_json;
    try {
        metadata_json = nlohmann::json::parse(metadata_file);
        m_metadata.name = metadata_json.value("name", "Unnamed World");
        m_metadata.seed = metadata_json.value("seed", "0");
        m_metadata.worldType = metadata_json.value("worldType", "default");
        m_metadata.creationTime = metadata_json.value("creationTime", 0);
    } catch (const nlohmann::json::parse_error& e) {
        // FIX: Replaced logging macro with std::cerr
        std::cerr << "Error: Failed to parse world metadata file: " << e.what() << std::endl;
        return false;
    }
    
    // --- Load Generation Preset ---
    
    std::string presetPath = m_rootPath + "worlds/atlas/presets/" + m_metadata.worldType + ".json";
    std::ifstream preset_file(presetPath);
    if (!preset_file.is_open()) {
        // FIX: Replaced logging macro with std::cerr
        std::cerr << "Error: Failed to open world preset file for loaded world: " << presetPath << std::endl;
        return false;
    }
    
    nlohmann::json preset_json;
    try {
        preset_json = nlohmann::json::parse(preset_file);
    } catch (const nlohmann::json::parse_error& e) {
        // FIX: Replaced logging macro with std::cerr
        std::cerr << "Error: Failed to parse world preset file '" << presetPath << "': " << e.what() << std::endl;
        return false;
    }

    TerrainGenParams params; // FIX: This now works because of the 'using' declaration
    auto& gen_params = preset_json["generation_params"];
    params.base_frequency = gen_params["terrain"].value("base_frequency", 0.01f);
    params.base_amplitude = gen_params["terrain"].value("base_amplitude", 50.0f);
    params.octaves = gen_params["terrain"].value("octaves", 4);
    params.persistence = gen_params["terrain"].value("persistence", 0.5f);
    params.lacunarity = gen_params["terrain"].value("lacunarity", 2.0f);
    params.height_offset = gen_params["terrain"].value("height_offset", 0.0f);
    params.island_mask_enabled = gen_params["terrain"].value("island_mask_enabled", false);
    params.island_mask_frequency = gen_params["terrain"].value("island_mask_frequency", 0.004f);
    params.caves_enabled = gen_params["features"].value("caves_enabled", true);
    params.cave_frequency = gen_params["features"].value("cave_frequency", 0.02f);

    int world_seed = StringToSeed(m_metadata.seed);

    m_worldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(m_jobSystem, nullptr, params, world_seed);
    m_waterSystem = std::make_unique<Systems::WaterSystem>(m_jobSystem, m_worldSystem.get());
    m_worldSystem->SetWaterSystem(m_waterSystem.get());

    // FIX: Replaced logging macro with std::cout
    std::cout << "World loaded successfully: " << m_metadata.name << std::endl;
    return true;
}

bool GameSession::SaveWorld() {
    std::string worldPath = m_rootPath + "worlds/saves/" + m_metadata.worldId;
    std::string metadataPath = worldPath + "/world_info.json";

    std::ofstream file(metadataPath);
    if (!file.is_open()) {
        std::cerr << "Failed to create world metadata file!" << std::endl;
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