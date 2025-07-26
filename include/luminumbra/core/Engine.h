// include/luminumbra/core/Engine.h
#pragma once

#include <memory> // For std::unique_ptr

// Forward declarations
struct GLFWwindow;
namespace Luminumbra::Rendering { 
    class Shader; 
    class Camera;
}

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
    std::unique_ptr<Luminumbra::Rendering::Camera> m_Camera;
    unsigned int m_TriangleVAO = 0;
    unsigned int m_TriangleVBO = 0;
};

} // namespace Luminumbra::Core