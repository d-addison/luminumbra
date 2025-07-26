// src/luminumbra/core/Engine.cpp
#include "luminumbra/core/Engine.h"
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/rendering/Camera.h"
#include "luminumbra/world/Chunk.h"
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

// --- NEW: Input Initialization ---
void Engine::initInput() {
    // Set cursor position callback
    auto cursor_pos_callback = [](GLFWwindow* window, double xpos, double ypos) {
        Engine* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
        engine->m_LastMouseX = xpos;
        engine->m_LastMouseY = ypos;
    };
    glfwSetCursorPosCallback(m_Window, cursor_pos_callback);
}

// --- NEW: Input Processing ---
void Engine::processInput() {
    if (glfwGetKey(m_Window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(m_Window, true);
    }
    
    // Normalize mouse coordinates to [-1, 1] range for camera control
    float normMouseX = (2.0f * (float)m_LastMouseX) / m_ScreenWidth - 1.0f;
    float normMouseY = 1.0f - (2.0f * (float)m_LastMouseY) / m_ScreenHeight;
    m_Camera->update(normMouseX, normMouseY);
}

// --- NEW: initRendering ---
void Engine::initRendering() {
    LOG("Engine::initRendering - Start");
    // Create the shader program
    m_BasicShader = std::make_unique<Luminumbra::Rendering::Shader>("res/shaders/basic.vert", "res/shaders/basic.frag");

    LOG("Engine::initRendering - Shader created.");
    m_TestChunk = std::make_unique<Luminumbra::World::Chunk>(glm::ivec3(0, 0, 0));
    LOG("Engine::initRendering - Chunk created.");
    
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

    // Set up MVP matrices
    m_BasicShader->setMat4("projection", m_Camera->getProjectionMatrix());
    m_BasicShader->setMat4("view", m_Camera->getViewMatrix());
    m_BasicShader->setMat4("model", m_TestChunk->getModelMatrix());

    m_TestChunk->render();
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