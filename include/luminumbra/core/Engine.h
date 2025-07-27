#pragma once

#include <memory>
#include <string>
#include <vector>
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/core/InputManager.h"
#include <glad/gl.h>  // Add this for GLuint type
#include "luminumbra/rendering/Camera.h"
#include <glm/glm.hpp>
#include "luminumbra/core/PostProcessSettings.h"

// Forward declarations
struct GLFWwindow;
namespace Luminumbra::Rendering { class Shader; }
namespace Luminumbra::World { class World; }
namespace Luminumbra::Debug { class Debug; }
namespace Luminumbra::UI { class UIManager; }

namespace Luminumbra::Core {

enum class GameState {
    SplashScreen,
    MainMenu,
    NewGameSetup,
    LoadGameMenu,
    Loading,
    InGame,
    Paused
};

class Engine {
public:
    Engine(int width, int height, const char* title);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void run();

    glm::vec3 getPosition() const;
    glm::vec3 getFront() const;

private:
    void initRendering();
    void initInput();
    void processInput();
    void update(float deltaTime);
    void render();
    
    void StartNewGame(const std::string& saveName, const std::string& seed);
    void SaveGame() const;
    void LoadGame(const std::string& slotName); // Add this
    std::vector<std::string> ListSaveGames() const; // Add this
    void QuitGame();
    
    // --- Core Components ---
    GLFWwindow* m_Window = nullptr;
    int m_ScreenWidth, m_ScreenHeight;
    std::unique_ptr<Luminumbra::UI::UIManager> m_UIManager;
    std::unique_ptr<Luminumbra::World::World> m_World; // The currently active world

    // --- Rendering & Debug ---
    std::unique_ptr<Luminumbra::Rendering::Shader> m_BasicShader;
    std::unique_ptr<Luminumbra::Debug::Debug> m_Debug;
    std::unique_ptr<Rendering::Shader> m_CelestialShader;
    std::unique_ptr<Rendering::Shader> m_FoliageShader;
    std::unique_ptr<Rendering::Shader> m_ParticleShader;
    std::unique_ptr<Rendering::Shader> m_WaterShader;

    // Post-processing shaders
    std::unique_ptr<Rendering::Shader> m_BloomShader;
    std::unique_ptr<Rendering::Shader> m_BlurShader;
    std::unique_ptr<Rendering::Shader> m_DofShader;
    std::unique_ptr<Rendering::Shader> m_GodRaysShader;
    std::unique_ptr<Rendering::Shader> m_FinalPassShader;

    // Framebuffer objects and textures
    GLuint m_SceneFramebuffer = 0;
    GLuint m_SceneTexture = 0;
    GLuint m_DepthTexture = 0;
    GLuint m_PingPongFBO;
    GLuint m_PingPongTextures[2];
    
    // Post-processing framebuffers
    GLuint m_PostProcessFBO = 0;
    GLuint m_PostProcessTextures[2] = {0, 0};

    void initFramebuffers();
    void deleteFramebuffers();
    void bindSceneFramebuffer() const;
    void renderPostProcess(GLuint sourceTexture, GLuint depthTexture = 0);
    void renderFullscreenQuad();
    Rendering::Shader* m_CurrentShader = nullptr;
    void renderScene(const Luminumbra::Rendering::Camera& camera);
    float calculateFocusDepth() const;

    static constexpr int SHADOW_MAP_SIZE = 2048;
    int m_CurrentPostProcessBuffer = 0;
    PostProcessSettings m_PostProcessSettings;
    
    // --- State Management ---
    GameState m_GameState = GameState::SplashScreen;
    float m_DeltaTime = 0.0f;
    double m_LastMouseX = 0.0, m_LastMouseY = 0.0;
    float m_SplashTime = 0.0f;

    // --- UI State ---
    bool m_ShowMenu = true;
    bool m_LaunchGame = false;
    char m_SaveName[128] = "My World";
    char m_Seed[128] = "luminumbra";
    bool m_ShowDebugInfo = true;
    bool m_WireframeMode = false;
    bool m_InCameraView = false;
    std::string m_InteractionPrompt = "";

    // --- Game Settings ---
    struct GameSettings {
        bool vsync = true;
        int shadowQuality = 2;
        int textureFiltering = 1;
        float masterVolume = 1.0f;
        float musicVolume = 0.7f;
        float effectsVolume = 0.9f;
        float mouseSensitivity = 0.1f;
        bool invertY = false;
    };
    GameSettings m_Settings;
    std::unique_ptr<InputManager> m_InputManager;
};

} // namespace Luminumbra::Core