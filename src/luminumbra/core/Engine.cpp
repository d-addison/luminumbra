#include "luminumbra/core/Engine.h"
#include "luminumbra/core/Debug.h"
#include "luminumbra/core/GLError.h"
#include "luminumbra/player/Player.h" // Still needed for enums & types
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/rendering/Skybox.h"
#include "luminumbra/rendering/CloudManager.h"
#include "luminumbra/ui/UIManager.h"
#include "luminumbra/world/World.h"
#include "luminumbra/world/Chunk.h"
#include "luminumbra/core/SaveData.h"
#include "luminumbra/core/InputManager.h"
#include <glad/gl.h> 
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <iostream>
#include <stdexcept>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <filesystem>
#include <fstream>
#include "json.hpp"

void error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error: %s\n", description);
}

namespace Luminumbra::Core {

Engine::Engine(int width, int height, const char* title) : m_ScreenWidth(width), m_ScreenHeight(height) {
    LOG("Engine::Constructor - Start");
    if (!glfwInit()) throw std::runtime_error("Failed to initialize GLFW");
    glfwSetErrorCallback(error_callback);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_Window = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!m_Window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(m_Window);
    if (!gladLoadGL(glfwGetProcAddress)) {
        glfwTerminate();
        throw std::runtime_error("Failed to initialize GLAD");
    }
    m_InputManager = std::make_unique<InputManager>(m_Window);
    glViewport(0, 0, width, height);

    glfwSetWindowUserPointer(m_Window, this);
    initInput();
    initRendering();
    
    m_UIManager = std::make_unique<Luminumbra::UI::UIManager>(m_Window);
    LOG("Engine::Constructor - Finish");
}

Engine::~Engine() {
    LOG("Engine::Destructor - Start");
    m_World.reset(); // Explicitly destroy world before subsystems
    glfwDestroyWindow(m_Window);
    glfwTerminate();
    LOG("Engine::Destructor - Finish");
}

void Engine::SaveGame() const {
    if (!m_World || !m_World->getPlayer() || m_World->getSlotName().empty()) {
        LOG("SaveGame failed: No active world or player.");
        return;
    }

    const std::string slotName = m_World->getSlotName();

    LOG("SaveGame: Starting save for slot: " + slotName);

    // 1. Create a JSON object to hold all save data
    nlohmann::json saveFileJson;

    // 2. Add metadata
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");
    saveFileJson["metadata"]["saveName"] = slotName;
    saveFileJson["metadata"]["timestamp"] = ss.str();

    // 3. Serialize and add world and player data
    saveFileJson["world"] = m_World->serialize();
    saveFileJson["player"] = m_World->getPlayer()->serialize();

    // 4. Write to file
    try {
        if (!std::filesystem::exists("saves")) {
            std::filesystem::create_directory("saves");
        }
        std::ofstream file("saves/" + slotName + ".json");
        file << saveFileJson.dump(4); // Use .dump(4) for pretty-printing
        file.close();
        LOG("SaveGame: Successfully saved to saves/" + slotName + ".json");
    } catch (const std::exception& e) {
        LOG("SaveGame Error: " + std::string(e.what()));
    }
}

std::vector<std::string> Engine::ListSaveGames() const {
    std::vector<std::string> saveFiles;
    const std::string savesPath = "saves";
    if (!std::filesystem::exists(savesPath)) {
        return saveFiles; // Return empty vector if directory doesn't exist
    }

    for (const auto& entry : std::filesystem::directory_iterator(savesPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            saveFiles.push_back(entry.path().stem().string()); // Add filename without extension
        }
    }
    return saveFiles;
}

void Engine::LoadGame(const std::string& slotName) {
    LOG("LoadGame: Loading from slot: " + slotName);
    const std::string filePath = "saves/" + slotName + ".json";

    try {
        std::ifstream file(filePath);
        nlohmann::json saveFileJson;
        file >> saveFileJson;

        // Deserialize data from JSON
        auto worldData = saveFileJson.at("world").get<Core::WorldSaveData>();
        auto playerData = saveFileJson.at("player").get<Core::PlayerSaveData>();

        // Create a new world with the saved seed
        m_World = std::make_unique<World::World>(worldData.seed, worldData.slotName, m_ScreenWidth, m_ScreenHeight);
        
        // Apply the loaded state
        m_World->setTimeOfDay(worldData.timeOfDay);
        m_World->getPlayer()->applySaveData(playerData);
        
        // Transition to the game
        m_GameState = GameState::InGame;
        m_ShowMenu = false;
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        LOG("LoadGame: Success.");

    } catch (const std::exception& e) {
        LOG("LoadGame Error: " + std::string(e.what()));
        m_GameState = GameState::MainMenu; // Go back to menu on failure
    }
}

void Engine::initInput() {
    glfwGetCursorPos(m_Window, &m_LastMouseX, &m_LastMouseY);

    auto cursor_pos_callback = [](GLFWwindow* window, double xpos, double ypos) {
        auto* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
        if (engine->m_World && engine->m_World->getPlayer()) {
            float xoffset = xpos - engine->m_LastMouseX;
            float yoffset = engine->m_LastMouseY - ypos;
            engine->m_LastMouseX = xpos;
            engine->m_LastMouseY = ypos;
            engine->m_World->getPlayer()->processMouseMovement(xoffset, yoffset);
        }
    };
    glfwSetCursorPosCallback(m_Window, cursor_pos_callback);

    auto scroll_callback = [](GLFWwindow* window, double xoffset, double yoffset) {
        auto* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
        if (engine->m_World && engine->m_World->getPlayer()) {
            engine->m_World->getPlayer()->processMouseScroll(yoffset);
        }
    };
    glfwSetScrollCallback(m_Window, scroll_callback);
}

void Engine::initRendering() {
    LOG("Engine::initRendering - Start");
    m_BasicShader = std::make_unique<Rendering::Shader>("res/shaders/basic.vert", "res/shaders/basic.frag");
    m_CelestialShader = std::make_unique<Rendering::Shader>("res/shaders/celestial.vert", "res/shaders/celestial.frag");
    m_FoliageShader = std::make_unique<Rendering::Shader>("res/shaders/foliage.vert", "res/shaders/foliage.frag");
    m_ParticleShader = std::make_unique<Rendering::Shader>("res/shaders/particle.vert", "res/shaders/particle.frag");
    m_WaterShader = std::make_unique<Rendering::Shader>("res/shaders/water.vert", "res/shaders/water.frag");
    m_Debug = std::make_unique<Debug::Debug>();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    LOG("Engine::initRendering - Finish");
}

void Engine::processInput() {
    // --- UI and Menu Toggles ---
    if (m_InputManager->isActionPressed(GameAction::Pause)) {
        if (m_GameState == GameState::InGame) {
            m_GameState = GameState::Paused;
            glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (m_GameState == GameState::Paused) {
            m_GameState = GameState::InGame;
            glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }
    
    if (m_ShowMenu || m_GameState != GameState::InGame) return; // Block game input if menu is open or not in-game

    // --- Gameplay Input ---
    Player::Player* player = m_World ? m_World->getPlayer() : nullptr;
    if (!player) return;

    // Handle continuous movement
    std::vector<Player::Movement> directions;
    if (m_InputManager->isActionHeld(GameAction::MoveForward)) directions.push_back(Player::Movement::Forward);
    if (m_InputManager->isActionHeld(GameAction::MoveBackward)) directions.push_back(Player::Movement::Backward);
    if (m_InputManager->isActionHeld(GameAction::MoveLeft)) directions.push_back(Player::Movement::Left);
    if (m_InputManager->isActionHeld(GameAction::MoveRight)) directions.push_back(Player::Movement::Right);
    
    if (player->isNoClipMode()) {
        if (m_InputManager->isActionHeld(GameAction::MoveUp)) directions.push_back(Player::Movement::Up);
        if (m_InputManager->isActionHeld(GameAction::MoveDown)) directions.push_back(Player::Movement::Down);
    }
    player->processMovement(directions, m_DeltaTime);

    // Handle state toggles (sprint, crouch)
    player->setSprinting(m_InputManager->isActionHeld(GameAction::Sprint));
    player->setCrouching(m_InputManager->isActionHeld(GameAction::Crouch), *m_World);

    // Handle single-press actions
    if (m_InputManager->isActionPressed(GameAction::Jump)) {
        player->processAction(Player::Action::Jump, *m_World);
    }
    if (m_InputManager->isActionPressed(GameAction::ToggleNoclip)) {
        player->processAction(Player::Action::ToggleNoclip, *m_World);
    }
    if (m_InputManager->isActionPressed(GameAction::ToggleGlide)) {
        player->processAction(Player::Action::ToggleGlide, *m_World);
    }

    // Debug Toggles - Now they are toggles, not holds, which is usually better UX
    if (m_InputManager->isActionPressed(GameAction::ShowDebug)) {
        LOG("Toggling debug info: " + std::to_string(m_ShowDebugInfo));
        m_ShowDebugInfo = !m_ShowDebugInfo;
    }
    if (m_InputManager->isActionPressed(GameAction::ToggleWireframe)) {
        LOG("Toggling wireframe mode: " + std::to_string(m_WireframeMode));
        m_WireframeMode = !m_WireframeMode;
    }

    if (m_InputManager->isActionPressed(GameAction::ToggleWeather)) { // Using raw key for simplicity
        static bool isRainy = false;
        isRainy = !isRainy;
        m_World->setWeather(isRainy ? World::World::WeatherType::Rainy : World::World::WeatherType::Clear);
    }

    if (m_InputManager->isActionPressed(GameAction::StartFire)) {
        m_World->startFireNearPlayer();
    }
}

void Engine::update(float deltaTime) {
    switch (m_GameState) {
        case GameState::SplashScreen:
            m_SplashTime += deltaTime;
            if (m_SplashTime > 2.0f) m_GameState = GameState::MainMenu;
            break;

        case GameState::Loading:
            // In a real game, you would check if crucial chunks are loaded.
            // For now, we'll use a simple delay to ensure the loading screen is visible.
            m_SplashTime += deltaTime; // Re-using splash time as a generic timer
            if (m_SplashTime > 5.0f) { // Wait for 3 seconds
                m_GameState = GameState::InGame;
            }
            break;

        case GameState::InGame:
            if (m_World) {
                // The player and world are updated in a specific order
                Player::Player* player = m_World->getPlayer();
                if (player) player->update(deltaTime, *m_World);
                m_World->update(deltaTime);
            }
            break;

        default:
            // MainMenu, NewGameSetup, etc. are state-only, no updates needed.
            break;
    }
}

void Engine::render() {
    if ((m_GameState == GameState::InGame || m_GameState == GameState::Paused) && m_World && m_World->getPlayer()) {
        // 1. Get the dynamic sky color from the world
        glm::vec3 skyColor = m_World->getSkyColor();

        // 2. Use this color to clear the screen
        glClearColor(skyColor.r, skyColor.g, skyColor.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Render the world from the player's perspective
        Player::Player* player = m_World->getPlayer();
        const auto& camera = player->getCamera();
        glPolygonMode(GL_FRONT_AND_BACK, m_WireframeMode ? GL_LINE : GL_FILL);

        // --- Render Terrain ---
        Rendering::Shader& terrainShader = m_WireframeMode ? m_Debug->getShader() : *m_BasicShader;
        terrainShader.use();
        terrainShader.setMat4("projection", camera.getProjectionMatrix());
        terrainShader.setMat4("view", camera.getViewMatrix());
        terrainShader.setVec3("sunDirection", m_World->getSunDirection());
        terrainShader.setVec3("viewPos", camera.getPosition());
        terrainShader.setVec3("fogColor", skyColor);
        m_World->renderTerrain(terrainShader, camera.getPosition());

        // 3b. Foliage
        if (!m_WireframeMode) {
             m_FoliageShader->use();
             m_FoliageShader->setVec3("sunDirection", m_World->getSunDirection());
             m_FoliageShader->setVec3("viewPos", camera.getPosition());
             m_FoliageShader->setVec3("fogColor", skyColor);
             m_World->renderFoliage(*m_FoliageShader);
        }

        // 4. Render Transparent Geometry
        if (!m_WireframeMode) {
            // 4a. Water (re-uses the terrain shader)
            m_WaterShader->use();
            m_WaterShader->setMat4("projection", camera.getProjectionMatrix());
            m_WaterShader->setMat4("view", camera.getViewMatrix());
            m_World->renderWater(*m_WaterShader, camera.getPosition());
            
            // 4b. Particles
            m_ParticleShader->use();
            m_ParticleShader->setMat4("u_Projection", camera.getProjectionMatrix());
            m_World->getParticleSystem()->render(*m_ParticleShader, camera.getViewMatrix());
        }
        
        if (m_Debug) m_Debug->drawAxes(camera.getProjectionMatrix(), camera.getViewMatrix());

    } else {
        // Default clear for menus
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    
    // --- UI Rendering ---
    m_UIManager->NewFrame();
    switch (m_GameState) {
        case GameState::SplashScreen: m_UIManager->ShowSplashScreen(); break;
        case GameState::MainMenu:
            m_UIManager->ShowMainMenu(
                [&](){ m_GameState = GameState::NewGameSetup; },
                [&](){ m_GameState = GameState::LoadGameMenu; },
                [&](){ QuitGame(); }
            );
            break;
        case GameState::NewGameSetup:
            m_UIManager->ShowNewGameWindow(
                m_LaunchGame,
                m_SaveName,
                sizeof(m_SaveName),
                m_Seed,
                sizeof(m_Seed)
            );
            break;
        case GameState::LoadGameMenu:
            {
                auto saves = ListSaveGames();
                m_UIManager->ShowLoadGameWindow(saves,
                    [&](const std::string& slotName) { LoadGame(slotName); },
                    [&]() { m_GameState = GameState::MainMenu; }
                );
            }
            break;
        case GameState::Paused:
            // Keep rendering the game world in the background to show it's paused
            if (m_World && m_World->getPlayer()) {
                Player::Player* player = m_World->getPlayer();
                if (m_ShowDebugInfo && !m_ShowMenu) {
                    m_UIManager->ShowDebugOverlay(player->getPosition(), player->getVelocity(), player->getGravity(),
                                                  player->isNoClipMode(), *m_World, *m_BasicShader);
                }
                if (!m_ShowMenu && !player->isNoClipMode()) {
                    m_UIManager->ShowStaminaBar(player->getStamina(), player->getMaxStamina(), player->isSprinting());
                }
            }

            // Draw the pause menu over the top
            m_UIManager->ShowPauseMenu(
                [&]() { m_GameState = GameState::InGame; glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); },
                [&]() { /* TODO: Show settings window */ },
                [&]() { SaveGame(); },
                [&]() { m_World.reset(); m_GameState = GameState::MainMenu; glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); }
            );
            break;
        case GameState::InGame:
            if (m_World && m_World->getPlayer()) {
                Player::Player* player = m_World->getPlayer();
                if (m_ShowDebugInfo && !m_ShowMenu) {
                    m_UIManager->ShowDebugOverlay(player->getPosition(), player->getVelocity(), player->getGravity(),
                                                  player->isNoClipMode(), *m_World, *m_BasicShader);
                }
                if (!m_ShowMenu && !player->isNoClipMode()) {
                    m_UIManager->ShowStaminaBar(player->getStamina(), player->getMaxStamina(), player->isSprinting());
                }
            }
            break;
        default: break;
    }
    m_UIManager->Render();
}

void Engine::StartNewGame(const std::string& saveName, const std::string& seed) {
    LOG("Engine::StartNewGame - Starting new game '" + saveName + "'...");
    m_ShowMenu = false;
    // Pass the saveName as the slotName to the World constructor
    m_World = std::make_unique<World::World>(saveName, seed, m_ScreenWidth, m_ScreenHeight);
    
    m_SplashTime = 0.0f; // Reset loading timer
    glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void Engine::QuitGame() {
    glfwSetWindowShouldClose(m_Window, true);
}

void Engine::run() {
    LOG("Engine::run - Starting main loop.");
    double lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(m_Window)) {
        double currentTime = glfwGetTime();
        m_DeltaTime = static_cast<float>(currentTime - lastTime);
        lastTime = currentTime;

        glfwPollEvents();
        m_InputManager->update();
        processInput();

        if (m_LaunchGame) {
            std::string seed_str = m_Seed;
            if (seed_str.empty()) seed_str = "luminumbra";
            
            // Set state to loading FIRST
            m_GameState = GameState::Loading;
            
            // Then start the world creation process
            StartNewGame(m_SaveName, seed_str);

            m_LaunchGame = false; // Reset the flag
        }

        update(m_DeltaTime);
        render();

        glfwSwapBuffers(m_Window);
    }
    LOG("Engine::run - Main loop finished.");
}

} // namespace Luminumbra::Core