#include "luminumbra/core/Engine.h"
#include "luminumbra/core/PostProcessSettings.h" 
#include "luminumbra/core/Debug.h"
#include "luminumbra/core/GLError.h"
#include "luminumbra/player/Player.h"
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
    initRendering();
    initFramebuffers();
    initInput();
    
    m_UIManager = std::make_unique<Luminumbra::UI::UIManager>(m_Window);
    LOG("Engine::Constructor - Finish");
}

Engine::~Engine() {
    LOG("Engine::Destructor - Start");
    deleteFramebuffers();
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
    // Initialize shaders
    m_BasicShader = std::make_unique<Rendering::Shader>("res/shaders/basic.vert", "res/shaders/basic.frag");
    m_CelestialShader = std::make_unique<Rendering::Shader>("res/shaders/celestial.vert", "res/shaders/celestial.frag");
    m_FoliageShader = std::make_unique<Rendering::Shader>("res/shaders/foliage.vert", "res/shaders/foliage.frag");
    m_ParticleShader = std::make_unique<Rendering::Shader>("res/shaders/particle.vert", "res/shaders/particle.frag");
    m_WaterShader = std::make_unique<Rendering::Shader>("res/shaders/water.vert", "res/shaders/water.frag");
    m_Debug = std::make_unique<Debug::Debug>();
    // Post-processing shaders
    m_BloomShader = std::make_unique<Rendering::Shader>("res/shaders/post_process.vert", "res/shaders/bloom.frag");
    m_BlurShader = std::make_unique<Rendering::Shader>("res/shaders/post_process.vert", "res/shaders/blur.frag");
    m_DofShader = std::make_unique<Rendering::Shader>("res/shaders/post_process.vert", "res/shaders/dof.frag");
    m_GodRaysShader = std::make_unique<Rendering::Shader>("res/shaders/post_process.vert", "res/shaders/god_rays.frag");
    m_FinalPassShader = std::make_unique<Rendering::Shader>("res/shaders/post_process.vert", "res/shaders/final_pass.frag");

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

void Engine::initFramebuffers() {
    // Scene framebuffer
    glGenFramebuffers(1, &m_SceneFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFramebuffer);

    // Color attachment
    glGenTextures(1, &m_SceneTexture);
    glBindTexture(GL_TEXTURE_2D, m_SceneTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_ScreenWidth, m_ScreenHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SceneTexture, 0);

    // Depth attachment
    glGenTextures(1, &m_DepthTexture);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_ScreenWidth, m_ScreenHeight, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_DepthTexture, 0);

    // Ping-pong FBOs for post-processing
    glGenFramebuffers(1, &m_PingPongFBO);
    glGenTextures(2, m_PingPongTextures);

    // Post-processing framebuffer
    glGenFramebuffers(1, &m_PostProcessFBO);
    glGenTextures(2, m_PostProcessTextures);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, m_PostProcessTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_ScreenWidth, m_ScreenHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Framebuffer is not complete!");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Engine::deleteFramebuffers() {
    glDeleteFramebuffers(1, &m_SceneFramebuffer);
    glDeleteFramebuffers(1, &m_PostProcessFBO);
    glDeleteTextures(1, &m_SceneTexture);
    glDeleteTextures(1, &m_DepthTexture);
    glDeleteFramebuffers(1, &m_PingPongFBO);
    glDeleteTextures(2, m_PingPongTextures);
    glDeleteTextures(2, m_PostProcessTextures);
}

void Engine::bindSceneFramebuffer() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFramebuffer);
    glViewport(0, 0, m_ScreenWidth, m_ScreenHeight);
}

void Engine::renderPostProcess(GLuint sourceTexture, GLuint depthTexture) {
    // 1. Bind the post-processing FBO to render to our off-screen textures
    glBindFramebuffer(GL_FRAMEBUFFER, m_PostProcessFBO);

    // 2. Attach the output texture, alternating between the two available textures (ping-pong)
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_PostProcessTextures[1 - m_CurrentPostProcessBuffer], 0);

    // 3. Bind input textures for the shader
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    m_CurrentShader->setInt("screenTexture", 0); // All post-process shaders should use "screenTexture" for consistency

    if (depthTexture > 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, depthTexture);
    }

    // 4. Set shader-specific uniforms
    if (m_CurrentShader == m_DofShader.get()) {
        const auto& camera = m_World->getPlayer()->getCamera(); // Get camera reference
        m_DofShader->setInt("depthTexture", 1);
        m_DofShader->setFloat("focusDepth", m_PostProcessSettings.dofFocalDistance);
        m_DofShader->setFloat("focusScale", m_PostProcessSettings.dofFocalRange);
        m_DofShader->setFloat("nearPlane", camera.getNearPlane());
        m_DofShader->setFloat("farPlane", camera.getFarPlane());
    }
    else if (m_CurrentShader == m_GodRaysShader.get()) {
        // NEW: Calculate sun position in screen space for god rays
        const auto& camera = m_World->getPlayer()->getCamera();
        // Project the sun's world position into normalized device coordinates (NDC)
        glm::vec4 sunClipSpace = camera.getProjectionMatrix() * camera.getViewMatrix() * glm::vec4(m_World->getSunDirection() * -1.0f, 0.0f);
        glm::vec2 sunNDC = glm::vec2(sunClipSpace.x, sunClipSpace.y) / sunClipSpace.w;
        // Convert NDC [-1, 1] to screen space [0, 1] for texture coordinates
        glm::vec2 sunScreenPos = sunNDC * 0.5f + 0.5f;

        m_GodRaysShader->setVec2("lightScreenPos", sunScreenPos);
        m_GodRaysShader->setFloat("density", m_PostProcessSettings.godRaysDensity);
    }

    // 5. Render a quad that covers the entire screen
    glDisable(GL_DEPTH_TEST);
    renderFullscreenQuad();
    glEnable(GL_DEPTH_TEST);

    // 6. Swap the current buffer index for the next pass
    m_CurrentPostProcessBuffer = 1 - m_CurrentPostProcessBuffer;
}

void Engine::renderFullscreenQuad() {
    static GLuint quadVAO = 0;
    if (quadVAO == 0) {
        float quadVertices[] = {
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f
        };
        
        GLuint quadVBO;
        glGenVertexArrays(1, &quadVAO);
        glGenBuffers(1, &quadVBO);
        glBindVertexArray(quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    }
    
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
}

float Engine::calculateFocusDepth() const {
    if (!m_World || !m_World->getPlayer()) return 10.0f;
    
    // Ray cast from camera to get focus distance
    const auto& camera = m_World->getPlayer()->getCamera();
    glm::vec3 rayStart = camera.getPosition();
    glm::vec3 rayDir = camera.getFront();
    
    for (float dist = 0.0f; dist < 100.0f; dist += 0.5f) {
        glm::vec3 checkPos = rayStart + rayDir * dist;
        if (m_World->isSolid(checkPos)) {
            return dist;
        }
    }
    
    return m_PostProcessSettings.dofFocalDistance;
}

void Engine::renderScene(const Luminumbra::Rendering::Camera& camera) {
    const glm::mat4& view = camera.getViewMatrix();
    const glm::mat4& projection = camera.getProjectionMatrix();
    const glm::vec3 sunDirection = m_World->getSunDirection();
    const float gameTime = static_cast<float>(glfwGetTime());

    // 1. Render all opaque objects first
    
    // Opaque Terrain
    m_BasicShader->use();
    m_BasicShader->setMat4("view", view);
    m_BasicShader->setMat4("projection", projection);
    m_BasicShader->setVec3("viewPos", camera.getPosition());
    m_BasicShader->setVec3("sunDirection", sunDirection); // Use consistent uniform name
    m_World->renderTerrain(*m_BasicShader, camera.getPosition());

    // Opaque Foliage
    m_FoliageShader->use();
    m_FoliageShader->setVec3("viewPos", camera.getPosition());
    m_FoliageShader->setVec3("sunDirection", sunDirection);
    m_FoliageShader->setFloat("time", gameTime); // For wind effects
    m_World->renderFoliage(*m_FoliageShader);

    // 2. Render the skybox and celestials. Depth testing ensures they are drawn behind opaque objects.
    m_World->renderSkyboxAndClouds(view, projection);
    m_CelestialShader->use();
    m_CelestialShader->setMat4("view", view);
    m_CelestialShader->setMat4("projection", projection);
    m_World->renderCelestials(*m_CelestialShader, view, projection);


    // 3. Render all transparent objects last, from back to front if possible.

    // Transparent Water
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Standard alpha blending
    m_WaterShader->use();
    m_WaterShader->setMat4("view", view);
    m_WaterShader->setMat4("projection", projection);
    m_WaterShader->setVec3("viewPos", camera.getPosition());
    m_WaterShader->setVec3("sunDirection", sunDirection);
    m_WaterShader->setFloat("time", gameTime);
    m_World->renderWater(*m_WaterShader, camera.getPosition());
    glDisable(GL_BLEND);

    // Transparent Particles
    glEnable(GL_BLEND);
    // Additive blending is great for fire, sparks, and magic effects
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); 
    m_ParticleShader->use();
    if (m_World->getParticleSystem()) {
        // Pass the camera object itself, as the particle system needs its vectors for billboarding
        m_World->getParticleSystem()->render(*m_ParticleShader, camera);
    }
    glDisable(GL_BLEND);
    
    // Reset blend function to default
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// In Engine.cpp
void Engine::render() {
    if (m_GameState != GameState::InGame && m_GameState != GameState::Paused) {
        // For menus, just clear the screen and render UI
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
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
                        m_UIManager->ShowDebugOverlay(player->getPosition(), player->getVelocity(), player->getGravity(), player->isNoClipMode(), *m_World, *m_BasicShader, m_PostProcessSettings);
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
                        m_UIManager->ShowDebugOverlay(player->getPosition(), player->getVelocity(), player->getGravity(), player->isNoClipMode(), *m_World, *m_BasicShader, m_PostProcessSettings);
                    }
                    if (!m_ShowMenu && !player->isNoClipMode()) {
                        m_UIManager->ShowStaminaBar(player->getStamina(), player->getMaxStamina(), player->isSprinting());
                    }
                }
                break;
            default: break;
        }
        m_UIManager->Render();
        return;
    }

    const auto& camera = m_World->getPlayer()->getCamera();

    // 1. Render scene to primary HDR framebuffer
    bindSceneFramebuffer();
    glm::vec3 skyColor = m_World->getSkyColor();
    glClearColor(skyColor.r, skyColor.g, skyColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderScene(camera);

    // --- Post-Processing Chain ---
    GLuint sceneResultTexture = m_SceneTexture;
    GLuint bloomTexture = 0; // Will hold the final blurred bloom texture

    // 2. BLOOM EFFECT
    if (m_PostProcessSettings.enableBloom) {
        // Pass 1: Extract bright colors from the scene into ping-pong texture 0
        glBindFramebuffer(GL_FRAMEBUFFER, m_PingPongFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_PingPongTextures[0], 0);
        m_BloomShader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_SceneTexture);
        renderFullscreenQuad();

        // Pass 2 & 3: Blur the bright texture using a two-pass Gaussian blur
        m_BlurShader->use();
        bool horizontal = true;
        bool first_iteration = true;
        unsigned int amount = 10; // Number of blur iterations
        for (unsigned int i = 0; i < amount; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_PingPongFBO);
            // Output to the other texture
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_PingPongTextures[horizontal], 0);
            
            m_BlurShader->setInt("horizontal", horizontal);
            
            glActiveTexture(GL_TEXTURE0);
            // Input from the previous pass
            glBindTexture(GL_TEXTURE_2D, first_iteration ? m_PingPongTextures[0] : m_PingPongTextures[!horizontal]);
            
            renderFullscreenQuad();
            horizontal = !horizontal;
            if (first_iteration) first_iteration = false;
        }
        bloomTexture = m_PingPongTextures[!horizontal]; // The final blurred texture
    }

    // (DoF and God Rays would go here, operating on m_SceneTexture)
    // For simplicity, we will skip them for this example and combine directly.

    // 3. FINAL COMPOSITE
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Bind back to default framebuffer
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_FinalPassShader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneResultTexture); // The original scene
    m_FinalPassShader->setInt("screenTexture", 0);

    if (bloomTexture != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomTexture); // The blurred bright spots
        m_FinalPassShader->setInt("bloomTexture", 1);
        m_FinalPassShader->setBool("useBloom", true);
    } else {
        m_FinalPassShader->setBool("useBloom", false);
    }
    
    m_FinalPassShader->setFloat("exposure", m_PostProcessSettings.exposure);
    renderFullscreenQuad();

    // UI Rendering
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
                    m_UIManager->ShowDebugOverlay(player->getPosition(), player->getVelocity(), player->getGravity(), player->isNoClipMode(), *m_World, *m_BasicShader, m_PostProcessSettings);
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
                    m_UIManager->ShowDebugOverlay(player->getPosition(), player->getVelocity(), player->getGravity(), player->isNoClipMode(), *m_World, *m_BasicShader, m_PostProcessSettings);
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