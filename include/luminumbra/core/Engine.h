// include/luminumbra/core/Engine.h
#pragma once

#include <memory> // For std::unique_ptr

// Forward declarations
struct GLFWwindow;
namespace Luminumbra::Rendering { 
    class Shader;
}
namespace Luminumbra::Player { class Player; }
namespace Luminumbra::World { class World; }
namespace Luminumbra::Debug { class Debug; }
namespace Luminumbra::UI { class UIManager; }

namespace Luminumbra::Core {

class Engine {
public:
    Engine(int width, int height, const char* title);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void run();

private:
    void initRendering();
    void render();
    void shutdown();
    void initInput();
    void processInput();
    
    GLFWwindow* m_Window = nullptr;
    int m_ScreenWidth, m_ScreenHeight; // Store window dimensions

    // Mouse position
    double m_LastMouseX = 0.0, m_LastMouseY = 0.0;

    // Rendering resources
    std::unique_ptr<Luminumbra::Rendering::Shader> m_BasicShader;
    std::unique_ptr<Luminumbra::Player::Player> m_Player;
    std::unique_ptr<Luminumbra::World::World> m_World; 
    std::unique_ptr<Luminumbra::Debug::Debug> m_Debug;
    std::unique_ptr<Luminumbra::UI::UIManager> m_UIManager;
    
    // UI state
    bool m_ShowMenu = true;
    bool m_ShowDebugInfo = true;
};

} // namespace Luminumbra::Core
