// src/luminumbra/core/Engine.cpp
#include "luminumbra/core/Engine.h"
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/rendering/Camera.h"
#include "luminumbra/world/Chunk.h"
#include "luminumbra/world/World.h"
#include "luminumbra/core/Debug.h"

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Private free function for the error callback
void error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error: %s\n", description);
}

namespace Luminumbra::Core {

// --- Constructor ---
Engine::Engine(int width, int height, const char* title) : m_ScreenWidth(width), m_ScreenHeight(height) {
    LOG("Engine::Constructor - Start");

    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwSetErrorCallback(error_callback);

    // Request OpenGL 3.3 Core Profile
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

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
    glViewport(0, 0, width, height);
    LOG("Engine::Constructor - GLFW and GLAD initialized.");
    
    // Associate this Engine instance with the GLFW window
    glfwSetWindowUserPointer(m_Window, this);
    initInput(); // Call new init function
    LOG("Engine::Constructor - Input initialized.");

    m_Camera = std::make_unique<Luminumbra::Rendering::Camera>((float)width, (float)height);
    LOG("Engine::Constructor - Camera created.");
    initRendering();
    LOG("Engine::Constructor - Rendering initialized.");
    LOG("Engine::Constructor - Finish");
}

// --- Destructor ---
Engine::~Engine() {
    LOG("Engine::Destructor - Start");
    shutdown();
    LOG("Engine::Destructor - Shutdown complete.");
    glfwDestroyWindow(m_Window);
    glfwTerminate();
    LOG("Engine::Destructor - Finish");
}

void Engine::initInput() {
    // We need to store the last position to calculate the offset
    glfwGetCursorPos(m_Window, &m_LastMouseX, &m_LastMouseY);

    auto cursor_pos_callback = [](GLFWwindow* window, double xpos, double ypos) {
        Engine* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
        
        float xoffset = xpos - engine->m_LastMouseX;
        float yoffset = engine->m_LastMouseY - ypos; // Reversed since y-coordinates go from top to bottom
        
        engine->m_LastMouseX = xpos;
        engine->m_LastMouseY = ypos;

        engine->m_Camera->processMouseMovement(xoffset, yoffset);
    };

    auto scroll_callback = [](GLFWwindow* window, double xoffset, double yoffset) {
        Engine* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
        engine->m_Camera->processMouseScroll(yoffset);
    };

    glfwSetCursorPosCallback(m_Window, cursor_pos_callback);
    glfwSetScrollCallback(m_Window, scroll_callback);

    // Optional: Hide and lock the cursor for a better camera feel
    // glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

// --- NEW: Input Processing ---
void Engine::processInput() {
    if (glfwGetKey(m_Window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(m_Window, true);
    }
}

// --- NEW: initRendering ---
void Engine::initRendering() {
    LOG("Engine::initRendering - Start");
    // Create the shader program
    m_BasicShader = std::make_unique<Luminumbra::Rendering::Shader>("res/shaders/basic.vert", "res/shaders/basic.frag");

    LOG("Engine::initRendering - Shader created.");
    m_World = std::make_unique<Luminumbra::World::World>();
    LOG("Engine::initRendering - World created.");
    
    // Enable depth testing so the terrain draws correctly
    glEnable(GL_DEPTH_TEST);
    LOG("Engine::initRendering - Depth Test enabled.");
    LOG("Engine::initRendering - Finish");
}

// --- NEW: render ---
void Engine::render() {
    glClearColor(0.28f, 0.24f, 0.55f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_BasicShader->use();
    // Set lighting uniforms (we only need to do this once per frame)
    m_BasicShader->setVec3("lightPos", glm::vec3(16.0f, 50.0f, 16.0f)); // A light high up in the "sky"
    m_BasicShader->setVec3("viewPos", m_Camera->getPosition());
    m_BasicShader->setVec3("lightColor", glm::vec3(1.0f, 1.0f, 1.0f));
    m_BasicShader->setVec3("objectColor", glm::vec3(0.8f, 0.8f, 0.9f)); // A pale rock color

    m_BasicShader->setMat4("projection", m_Camera->getProjectionMatrix());
    m_BasicShader->setMat4("view", m_Camera->getViewMatrix());
    if (m_World) {
        m_World->render(*m_BasicShader); // Tell the world to render itself
    }
}

// --- NEW: shutdown ---
void Engine::shutdown() {

}

// --- run loop ---
void Engine::run() {
    LOG("Engine::run - Starting main loop.");
    while (!glfwWindowShouldClose(m_Window)) {
        glfwPollEvents();
        processInput();

        render();

        glfwSwapBuffers(m_Window);
    }
    LOG("Engine::run - Main loop finished.");
}

} // namespace Luminumbra::Core
