// src/luminumbra/core/Engine.cpp
#include "luminumbra/core/Engine.h"

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>

// Private free function for the error callback
void error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error: %s\n", description);
}

namespace Luminumbra::Core {

Engine::Engine(int width, int height, const char* title) {
    std::cout << "Luminumbra Engine Initializing..." << std::endl;

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
}

Engine::~Engine() {
    std::cout << "Shutting down." << std::endl;
    glfwDestroyWindow(m_Window);
    glfwTerminate();
}

void Engine::run() {
    while (!glfwWindowShouldClose(m_Window)) {
        // --- Input ---
        glfwPollEvents();
        if (glfwGetKey(m_Window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(m_Window, true);
        }

        // --- Logic Update (for later) ---
        // update(deltaTime);

        // --- Rendering ---
        // Clear the screen to a color from the Umbra palette (#483D8B)
        glClearColor(0.28f, 0.24f, 0.55f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // render();

        // --- Swap Buffers ---
        glfwSwapBuffers(m_Window);
    }
}

} // namespace Luminumbra::Core