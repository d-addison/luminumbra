// include/luminumbra/core/Engine.h
#pragma once

#include <memory> // For std::unique_ptr

// Forward declarations
struct GLFWwindow;
namespace Luminumbra::Rendering { class Shader; }

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
    
    GLFWwindow* m_Window = nullptr;

    // Rendering resources
    std::unique_ptr<Luminumbra::Rendering::Shader> m_BasicShader;
    unsigned int m_TriangleVAO = 0;
    unsigned int m_TriangleVBO = 0;
};

} // namespace Luminumbra::Core