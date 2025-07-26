// include/luminumbra/core/Engine.h
#pragma once

// Forward declare GLFWwindow to avoid including glfw3.h in the header
struct GLFWwindow;

namespace Luminumbra::Core {

class Engine {
public:
    Engine(int width, int height, const char* title);
    ~Engine();

    // Delete copy constructor and assignment operator to prevent copying
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void run();

private:
    void init();
    void shutdown();
    
    GLFWwindow* m_Window = nullptr;
};

} // namespace Luminumbra::Core