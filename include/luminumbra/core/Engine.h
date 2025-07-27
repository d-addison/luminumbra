#pragma once

#include <memory>
#include <string>
#include <vector>
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/core/InputManager.h"

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