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
#include "luminumbra/audio/AudioManager.h"
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
#include "luminumbra/rendering/WaterRenderer.h"
#include "luminumbra/rendering/TextureLoader.h"

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
    Audio::AudioManager::getInstance().init(); 
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
    m_UniversalDepthShader = std::make_unique<Rendering::Shader>("res/shaders/universal_depth.vert", "res/shaders/universal_depth.frag");

    // Initialize water renderer and load textures
    m_WaterRenderer = std::make_unique<Rendering::WaterRenderer>();
    m_WaterDudvMap = Rendering::loadTexture("../res/textures/water_dudv.png");
    m_WaterNormalMap = Rendering::loadTexture("../res/textures/water_normal.png");

    glGenFramebuffers(1, &m_DepthMapFBO);

    glGenTextures(1, &m_DepthMapTexture);
    glBindTexture(GL_TEXTURE_2D, m_DepthMapTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, m_DepthMapFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_DepthMapTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

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

    if (m_InputManager->isActionPressed(GameAction::ToggleWeather)) {
        // This logic cycles through the weather types
        auto currentType = m_World->getWeatherManager()->getCurrentWeatherType();
        int nextTypeIndex = (static_cast<int>(currentType) + 1) % 4; // 4 weather types
        Luminumbra::World::WeatherType nextType = static_cast<Luminumbra::World::WeatherType>(nextTypeIndex);
        m_World->setWeather(nextType);
    }

    if (m_InputManager->isActionPressed(GameAction::StartFire)) {
        m_World->startFireNearPlayer();
    }
}

void Engine::update(float deltaTime) {
    if (m_GameState == GameState::InGame && m_World && m_World->getPlayer()) {
        auto& camera = m_World->getPlayer()->getCamera();
        Audio::AudioManager::getInstance().setListenerPosition(camera.getPosition(), camera.getFront(), camera.getUp());
        
        // Pass settings to the audio manager
        Audio::AudioManager::getInstance().setGroupVolume(Audio::SoundGroup::Master, m_Settings.masterVolume);
        // Note: Music volume is handled in playMusic for simplicity, but could be grouped too.
        Audio::AudioManager::getInstance().setGroupVolume(Audio::SoundGroup::Player, m_Settings.effectsVolume);
        Audio::AudioManager::getInstance().setGroupVolume(Audio::SoundGroup::SFX, m_Settings.effectsVolume);
        Audio::AudioManager::getInstance().setGroupVolume(Audio::SoundGroup::Ambience, m_Settings.effectsVolume * 0.6f); // Ambience is quieter
    }
    Audio::AudioManager::getInstance().update();

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
    // --- 1. Main Scene Framebuffer (Full Resolution) ---
    // Renders the main 3D world in HDR.
    glGenFramebuffers(1, &m_SceneFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFramebuffer);

    // Color attachment (HDR)
    glGenTextures(1, &m_SceneTexture);
    glBindTexture(GL_TEXTURE_2D, m_SceneTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_ScreenWidth, m_ScreenHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SceneTexture, 0);

    // Depth attachment
    glGenTextures(1, &m_DepthTexture);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_ScreenWidth, m_ScreenHeight, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_DepthTexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Scene Framebuffer is not complete!");
    }

    // --- 2. Post-Processing Framebuffers (Full Resolution) ---
    // Used for full-res effects like Depth of Field.
    glGenFramebuffers(1, &m_PostProcessFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_PostProcessFBO);
    glGenTextures(2, m_PostProcessTextures);

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, m_PostProcessTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_ScreenWidth, m_ScreenHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    // Attach one texture so the FBO is valid, it will be changed during rendering
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_PostProcessTextures[0], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Post-Process Framebuffer is not complete!");
    }

    // --- 3. Bloom Ping-Pong Framebuffers (Full Resolution) ---
    // Used specifically for the multi-pass Gaussian blur for the bloom effect.
    glGenFramebuffers(1, &m_PingPongFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_PingPongFBO);
    glGenTextures(2, m_PingPongTextures);

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, m_PingPongTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_ScreenWidth, m_ScreenHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    // Attach one texture so the FBO is valid
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_PingPongTextures[0], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Bloom Ping-Pong Framebuffer is not complete!");
    }

    // --- 4. Half-Resolution Framebuffer (for expensive effects) ---
    // A smaller FBO to run expensive shaders like God Rays for a huge performance gain.
    glGenFramebuffers(1, &m_HalfResFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_HalfResFBO);
    glGenTextures(2, m_HalfResTextures); // Two textures for ping-ponging if needed

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, m_HalfResTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_ScreenWidth / 2, m_ScreenHeight / 2, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    // Attach one texture so the FBO is valid
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_HalfResTextures[0], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Half-Resolution Framebuffer is not complete!");
    }

    // Unbind the framebuffer to return to the default one
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

void Engine::renderScene(const Luminumbra::Rendering::Camera& camera, const glm::vec4& clipPlane) {
    const glm::mat4& view = camera.getViewMatrix();
    const glm::mat4& projection = camera.getProjectionMatrix();
    const glm::vec3 sunDirection = m_World->getSunDirection();
    const float gameTime = static_cast<float>(glfwGetTime());

    // Opaque Terrain
    m_BasicShader->use();
    m_BasicShader->setMat4("view", view);
    m_BasicShader->setMat4("projection", projection);
    m_BasicShader->setVec3("viewPos", camera.getPosition());
    m_BasicShader->setVec3("sunDirection", sunDirection);
    m_BasicShader->setVec4("u_ClipPlane", clipPlane);
    m_World->renderTerrain(*m_BasicShader, camera.getPosition());

    // Opaque Foliage
    m_FoliageShader->use();
    m_FoliageShader->setVec3("viewPos", camera.getPosition());
    m_FoliageShader->setVec3("sunDirection", sunDirection);
    m_FoliageShader->setFloat("time", gameTime);
    m_FoliageShader->setVec4("u_ClipPlane", clipPlane);
    m_World->renderFoliage(*m_FoliageShader);
}

void Engine::renderWorld(const Luminumbra::Rendering::Camera& camera, const glm::mat4& lightSpaceMatrix, const glm::vec4& clipPlane) {
    const glm::mat4& view = camera.getViewMatrix();
    const glm::mat4& projection = camera.getProjectionMatrix();
    const glm::vec3 sunDirection = m_World->getSunDirection();
    const float gameTime = static_cast<float>(glfwGetTime());
    const glm::vec3 skyColor = m_World->getSkyColor();

    // --- Opaque Terrain ---
    m_BasicShader->use();

    // === NEW: Set Height-Based Coloring Uniforms ===
    // Set the Y-levels for each biome layer. These are based on the water level for context.
    const float waterLevel = Luminumbra::World::Chunk::WATER_LEVEL;
    m_BasicShader->setFloat("u_sandLevel", waterLevel + 2.0f);
    m_BasicShader->setFloat("u_grassLevel", waterLevel + 25.0f);
    m_BasicShader->setFloat("u_rockLevel", waterLevel + 55.0f);
    
    // Set the colors for each layer
    m_BasicShader->setVec3("u_sandColor", glm::vec3(0.85f, 0.75f, 0.55f));  // Sandy yellow
    m_BasicShader->setVec3("u_grassColor", glm::vec3(0.45f, 0.65f, 0.25f)); // Grassy green
    m_BasicShader->setVec3("u_rockColor", glm::vec3(0.5f, 0.5f, 0.5f));      // Rocky grey
    m_BasicShader->setVec3("u_snowColor", glm::vec3(0.95f, 0.95f, 1.0f));     // Bright white for snow
    m_BasicShader->setFloat("u_Wetness", m_World->getWeatherManager()->getWetness());

    // Set the blend sharpness between layers. Higher values = sharper transitions.
    m_BasicShader->setFloat("u_blendRange", 8.0f); 

    m_BasicShader->setMat4("u_view", view);
    m_BasicShader->setMat4("u_projection", projection);
    m_BasicShader->setVec3("viewPos", camera.getPosition());
    m_BasicShader->setVec3("sunDirection", sunDirection);
    m_BasicShader->setVec4("u_ClipPlane", clipPlane);
    m_BasicShader->setMat4("u_lightSpaceMatrix", lightSpaceMatrix);
    m_BasicShader->setVec3("fogColor", skyColor);
    
    // Bind the shadow map texture to a free texture unit
    glActiveTexture(GL_TEXTURE4); 
    glBindTexture(GL_TEXTURE_2D, m_DepthMapTexture);
    m_BasicShader->setInt("shadowMap", 4);

    m_World->renderTerrain(*m_BasicShader, camera.getPosition());

    // --- Opaque Foliage ---
    m_FoliageShader->use();
    m_FoliageShader->setMat4("view", view);
    m_FoliageShader->setMat4("projection", projection);
    m_FoliageShader->setVec3("viewPos", camera.getPosition());
    m_FoliageShader->setVec3("sunDirection", sunDirection);
    m_FoliageShader->setFloat("time", gameTime);
    m_FoliageShader->setVec4("u_ClipPlane", clipPlane);
    m_FoliageShader->setMat4("lightSpaceMatrix", lightSpaceMatrix);
    m_FoliageShader->setVec3("objectColor", glm::vec3(0.1f, 0.5f, 0.15f));
    m_FoliageShader->setVec3("fogColor", skyColor);
    m_FoliageShader->setFloat("u_Wetness", m_World->getWeatherManager()->getWetness());
    
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_DepthMapTexture);
    m_FoliageShader->setInt("shadowMap", 4);
    
    m_World->renderFoliage(*m_FoliageShader);
}

void Engine::render() {
    // --- 1. HANDLE NON-GAME STATES & UI-ONLY RENDERING ---
    // If we aren't in a playable game state, we only need to render the UI.
    if (!m_World || (m_GameState != GameState::InGame && m_GameState != GameState::Paused)) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_ScreenWidth, m_ScreenHeight);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
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
                        m_UIManager->ShowDebugOverlay(m_DeltaTime, *player, *m_World, m_PostProcessSettings);
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
                        m_UIManager->ShowDebugOverlay(m_DeltaTime, *player, *m_World, m_PostProcessSettings);
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

    // --- RENDER PREPARATION ---
    glEnable(GL_CLIP_DISTANCE0);
    Player::Player* player = m_World->getPlayer();
    Rendering::Camera& camera = player->getCamera();
    const float gameTime = static_cast<float>(glfwGetTime());
    const glm::vec3 sunDirection = m_World->getSunDirection();
    const glm::vec3 skyColor = m_World->getSkyColor();

    // --- 2. SHADOW MAPPING PASS ---
    const float shadowOrthoSize = 150.0f;
    glm::mat4 lightProjection = glm::ortho(-shadowOrthoSize, shadowOrthoSize, -shadowOrthoSize, shadowOrthoSize, 1.0f, 400.0f);
    glm::vec3 lightPos = player->getPosition() - (sunDirection * 100.0f);
    glm::mat4 lightView = glm::lookAt(lightPos, player->getPosition(), glm::vec3(0.0, 1.0, 0.0));
    glm::mat4 lightSpaceMatrix = lightProjection * lightView;
    
    glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
    glBindFramebuffer(GL_FRAMEBUFFER, m_DepthMapFBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT); // Prevent peter-panning artifacts

    m_UniversalDepthShader->use();
    m_UniversalDepthShader->setMat4("u_LightSpaceMatrix", lightSpaceMatrix);

    // Render terrain (not instanced)
    m_UniversalDepthShader->setBool("u_IsInstanced", false);
    m_World->renderTerrain(*m_UniversalDepthShader, camera.getPosition());

    // Render foliage (instanced)
    m_UniversalDepthShader->setBool("u_IsInstanced", true);
    m_World->renderFoliage(*m_UniversalDepthShader);

    glCullFace(GL_BACK); // Reset culling
    m_BasicShader->use();
    m_FoliageShader->use();

    // --- 3. WATER REFLECTION & REFRACTION PASSES ---
    // Reflection Pass (render upside-down)
    m_WaterRenderer->bindReflectionFBO();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    float distance = 2.0f * (camera.getPosition().y - World::Chunk::WATER_LEVEL);
    camera.setPosition(camera.getPosition() - glm::vec3(0.0f, distance, 0.0f));
    camera.setPitch(-camera.getPitch());
    renderWorld(camera, lightSpaceMatrix, glm::vec4(0.0f, 1.0f, 0.0f, -World::Chunk::WATER_LEVEL + 0.1f));
    m_World->renderSkyboxAndClouds(camera.getViewMatrix(), camera.getProjectionMatrix());
    camera.setPosition(camera.getPosition() + glm::vec3(0.0f, distance, 0.0f)); // Restore camera
    camera.setPitch(-camera.getPitch());

    // Refraction Pass (render normally, clipped at water level)
    m_WaterRenderer->bindRefractionFBO();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderWorld(camera, lightSpaceMatrix, glm::vec4(0.0f, -1.0f, 0.0f, World::Chunk::WATER_LEVEL));

    // --- 4. MAIN SCENE PASS (to HDR Framebuffer) ---
    bindSceneFramebuffer();
    glClearColor(skyColor.r, skyColor.g, skyColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_World->renderSkyboxAndClouds(camera.getViewMatrix(), camera.getProjectionMatrix());
    m_World->renderCelestials(*m_CelestialShader, camera.getViewMatrix(), camera.getProjectionMatrix());
    camera.updateFrustum();
    renderWorld(camera, lightSpaceMatrix); // Render main world with shadows

    // Render water surface
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    m_WaterShader->use();
    m_WaterShader->setMat4("u_View", camera.getViewMatrix());
    m_WaterShader->setMat4("u_Projection", camera.getProjectionMatrix());
    m_WaterShader->setVec3("u_CameraPosition", camera.getPosition());
    m_WaterShader->setVec3("u_LightDirection", m_World->getSunDirection());
    m_WaterShader->setFloat("u_Time", gameTime);
    m_WaterShader->setFloat("u_NearPlane", camera.getNearPlane());
    m_WaterShader->setFloat("u_FarPlane", camera.getFarPlane());
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_WaterRenderer->getReflectionTexture());   m_WaterShader->setInt("u_ReflectionTexture", 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_WaterRenderer->getRefractionTexture());  m_WaterShader->setInt("u_RefractionTexture", 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_WaterDudvMap);                          m_WaterShader->setInt("u_DudvMap", 2);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_WaterNormalMap);                        m_WaterShader->setInt("u_NormalMap", 3);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, m_WaterRenderer->getRefractionDepthTexture()); m_WaterShader->setInt("u_RefractionDepthTexture", 4);
    m_World->renderWater(*m_WaterShader, camera.getPosition());
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    // Render particles
    if (m_World->getParticleSystem()) {
        m_World->getParticleSystem()->render(*m_ParticleShader, camera);
    }
    
    // --- 5. POST-PROCESSING CHAIN ---
    glDisable(GL_DEPTH_TEST);
    GLuint currentSourceTexture = m_SceneTexture;
    
    // Pass A: Depth of Field (optional)
    if (m_PostProcessSettings.enableDof) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_PostProcessFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_PostProcessTextures[0], 0);

        m_DofShader->use();
        m_DofShader->setFloat("focusDepth", calculateFocusDepth()); // Automatic focus
        m_DofShader->setFloat("focusScale", m_PostProcessSettings.dofFocalRange);
        m_DofShader->setFloat("nearPlane", camera.getNearPlane());
        m_DofShader->setFloat("farPlane", camera.getFarPlane());
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, currentSourceTexture);
        m_DofShader->setInt("screenTexture", 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_DepthTexture); // Use main scene's depth texture
        m_DofShader->setInt("depthTexture", 1);

        renderFullscreenQuad();
        currentSourceTexture = m_PostProcessTextures[0]; // The result of this pass is now the source for the next
    }

    // Pass B: God Rays
    GLuint godRayResultTexture = 0; // Will hold the handle to the final low-res texture
    if (m_PostProcessSettings.enableGodRays) {
        
        // --- A: Downsample the scene to a half-resolution texture ---
        glBindFramebuffer(GL_FRAMEBUFFER, m_HalfResFBO);
        // Set the render target to the FIRST half-res texture
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_HalfResTextures[0], 0);

        glViewport(0, 0, m_ScreenWidth / 2, m_ScreenHeight / 2); // Set viewport to half size

        // Use a simple passthrough shader to copy and downsample
        m_FinalPassShader->use(); 
        m_FinalPassShader->setFloat("exposure", 1.0f); // Use neutral exposure for a clean copy
        m_FinalPassShader->setBool("useBloom", false);
        m_FinalPassShader->setBool("useGodRays", false); // Make sure this is also false

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, currentSourceTexture); // Input is the full-res scene
        m_FinalPassShader->setInt("screenTexture", 0);
        renderFullscreenQuad(); // The downsampled scene is now in m_HalfResTextures[0]

        
        // --- B: Run the expensive God Rays shader on the SMALL texture ---
        // Set the render target to the SECOND half-res texture to avoid read/write conflict
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_HalfResTextures[1], 0);

        m_GodRaysShader->use();
        // Project sun's world position to screen space for the shader
        glm::vec4 sunClipSpace = camera.getProjectionMatrix() * camera.getViewMatrix() * glm::vec4(sunDirection * -1000.0f, 1.0f);
        glm::vec2 sunNDC = glm::vec2(sunClipSpace.x, sunClipSpace.y) / sunClipSpace.w;
        glm::vec2 sunScreenPos = sunNDC * 0.5f + 0.5f;

        // Set all required uniforms
        m_GodRaysShader->setVec2("lightScreenPos", sunScreenPos);
        m_GodRaysShader->setFloat("density", m_PostProcessSettings.godRaysDensity);
        m_GodRaysShader->setFloat("exposure", 0.25f);
        m_GodRaysShader->setFloat("decay", 0.95f);
        m_GodRaysShader->setFloat("weight", 0.6f);
        m_GodRaysShader->setInt("samples", 40); // Use a reasonable sample count

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_HalfResTextures[0]); // INPUT is the downsampled texture
        m_GodRaysShader->setInt("screenTexture", 0);
        renderFullscreenQuad(); // The god rays effect is now in m_HalfResTextures[1]


        // --- C: Prepare for the final composite pass ---
        // The result of our work is the second half-res texture
        godRayResultTexture = m_HalfResTextures[1];

        // Restore the viewport to full resolution for the next passes
        glViewport(0, 0, m_ScreenWidth, m_ScreenHeight);
    }

    // Pass C: Bloom (optional)
    GLuint bloomTexture = 0; // Will hold the final blurred texture
    if (m_PostProcessSettings.enableBloom) {
        // C.1: Extract bright colors
        glBindFramebuffer(GL_FRAMEBUFFER, m_PingPongFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_PingPongTextures[0], 0);
        
        m_BloomShader->use();
        m_BloomShader->setFloat("threshold", m_PostProcessSettings.bloomThreshold);
        m_BloomShader->setFloat("intensity", m_PostProcessSettings.bloomIntensity);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, currentSourceTexture); // Use result from previous pass
        m_BloomShader->setInt("screenTexture", 0);
        renderFullscreenQuad();

        // C.2: Blur the bright texture with a two-pass Gaussian blur
        m_BlurShader->use();
        bool horizontal = true;
        bool first_iteration = true;
        for (unsigned int i = 0; i < 10; i++) { // 5 blur iterations (10 passes)
            glBindFramebuffer(GL_FRAMEBUFFER, m_PingPongFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_PingPongTextures[horizontal], 0);
            
            m_BlurShader->setInt("horizontal", horizontal);
            m_BlurShader->setInt("image", 0);
            
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, first_iteration ? m_PingPongTextures[0] : m_PingPongTextures[!horizontal]);
            
            renderFullscreenQuad();
            horizontal = !horizontal;
            if (first_iteration) first_iteration = false;
        }
        bloomTexture = m_PingPongTextures[!horizontal]; // The final blurred texture
    }

    // --- 6. FINAL COMPOSITE PASS (to screen) ---
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Bind back to default framebuffer
    glViewport(0, 0, m_ScreenWidth, m_ScreenHeight);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_FinalPassShader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, currentSourceTexture); // Bind the result of the post-processing chain
    m_FinalPassShader->setInt("screenTexture", 0);

    m_FinalPassShader->setBool("useBloom", m_PostProcessSettings.enableBloom);
    if (m_PostProcessSettings.enableBloom) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomTexture);
        m_FinalPassShader->setInt("bloomTexture", 1);
    }
    m_FinalPassShader->setFloat("exposure", m_PostProcessSettings.exposure);
    
    // Draw the final image BEFORE enabling depth testing
    renderFullscreenQuad();
    
    // =================== THIS LINE WAS MOVED ===================
    // Re-enable depth testing for the UI and any potential 3D debug overlays.
    glEnable(GL_DEPTH_TEST); 
    // =========================================================

    // --- 7. UI RENDERING ---
    // Render UI on top of the final scene
    m_UIManager->NewFrame();
    if (m_GameState == GameState::Paused) {
        m_UIManager->ShowPauseMenu(
            [&]() { m_GameState = GameState::InGame; glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); },
            [&]() { /* TODO: Show settings window */ },
            [&]() { SaveGame(); },
            [&]() { m_World.reset(); m_GameState = GameState::MainMenu; glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); }
        );
    }
    if (m_ShowDebugInfo) {
        m_UIManager->ShowDebugOverlay(m_DeltaTime, *player, *m_World, m_PostProcessSettings);
    }
    if (!player->isNoClipMode()) {
        m_UIManager->ShowStaminaBar(player->getStamina(), player->getMaxStamina(), player->isSprinting());
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