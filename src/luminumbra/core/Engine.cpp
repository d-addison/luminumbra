// src/luminumbra/core/Engine.cpp
#include "luminumbra/core/Engine.h"
#include "luminumbra/rendering/Shader.h" // Include our new shader class
#include "luminumbra/rendering/Camera.h"

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
    
    // Associate this Engine instance with the GLFW window
    glfwSetWindowUserPointer(m_Window, this);
    initInput(); // Call new init function

    m_Camera = std::make_unique<Luminumbra::Rendering::Camera>((float)width, (float)height);
    initRendering();
}

// --- Destructor ---
Engine::~Engine() {
    shutdown();
    std::cout << "Shutting down." << std::endl;
    glfwDestroyWindow(m_Window);
    glfwTerminate();
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
    // Create the shader program
    m_BasicShader = std::make_unique<Luminumbra::Rendering::Shader>("res/shaders/basic.vert", "res/shaders/basic.frag");

    // Define the vertices for a triangle
    float vertices[] = {
        -0.5f, -0.5f, 0.0f, // left  
         0.5f, -0.5f, 0.0f, // right 
         0.0f,  0.5f, 0.0f  // top   
    };

    // 1. Create VAO and VBO
    glGenVertexArrays(1, &m_TriangleVAO);
    glGenBuffers(1, &m_TriangleVBO);

    // 2. Bind VAO first, then bind and set VBO(s), and then configure vertex attributes(s).
    glBindVertexArray(m_TriangleVAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_TriangleVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 3. Tell OpenGL how to interpret the vertex data
    // Corresponds to 'layout (location = 0)' in the vertex shader
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Unbind the VBO and VAO
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

// --- NEW: render ---
void Engine::render() {
    glClearColor(0.28f, 0.24f, 0.55f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_BasicShader->use();

    // Set up MVP matrices
    glm::mat4 model = glm::mat4(1.0f); // Identity matrix
    // You could rotate the triangle over time like this:
    // model = glm::rotate(model, (float)glfwGetTime(), glm::vec3(0.0f, 1.0f, 0.0f));

    m_BasicShader->setMat4("projection", m_Camera->getProjectionMatrix());
    m_BasicShader->setMat4("view", m_Camera->getViewMatrix());
    m_BasicShader->setMat4("model", model);

    glBindVertexArray(m_TriangleVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// --- NEW: shutdown ---
void Engine::shutdown() {
    glDeleteVertexArrays(1, &m_TriangleVAO);
    glDeleteBuffers(1, &m_TriangleVBO);
    // m_BasicShader is cleaned up automatically by unique_ptr
}

// --- run loop ---
void Engine::run() {
    while (!glfwWindowShouldClose(m_Window)) {
        glfwPollEvents();
        processInput();

        render();

        glfwSwapBuffers(m_Window);
    }
}

} // namespace Luminumbra::Core