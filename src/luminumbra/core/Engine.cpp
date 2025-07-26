// src/luminumbra/core/Engine.cpp
#include "luminumbra/core/Engine.h"
#include <glad/gl.h> 
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/player/Player.h"
#include "luminumbra/world/Chunk.h"
#include "luminumbra/world/World.h"
#include "luminumbra/core/Debug.h"
#include "luminumbra/ui/UIManager.h"
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

    m_Player = std::make_unique<Luminumbra::Player::Player>((float)width, (float)height);
    LOG("Engine::Constructor - Player created.");
    initRendering();
    LOG("Engine::Constructor - Rendering initialized.");
    
    // Initialize UI Manager
    m_UIManager = std::make_unique<Luminumbra::UI::UIManager>(m_Window);
    LOG("Engine::Constructor - UIManager initialized.");
    
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

        engine->m_Player->processMouseMovement(xoffset, yoffset);
    };

    glfwSetCursorPosCallback(m_Window, cursor_pos_callback);

    // Optional: Hide and lock the cursor for a better camera feel
    // glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

// --- NEW: Input Processing ---
void Engine::processInput() {
    static bool escapeWasPressed = false;
    
    if (glfwGetKey(m_Window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        if (!escapeWasPressed) {
            m_ShowMenu = !m_ShowMenu;
            
            // Toggle cursor visibility based on menu state
            if (m_ShowMenu) {
                glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            } else {
                glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }
            
            escapeWasPressed = true;
        }
    } else {
        escapeWasPressed = false;
    }
    
    // Toggle debug info with F3
    static bool f3WasPressed = false;
    if (glfwGetKey(m_Window, GLFW_KEY_F3) == GLFW_PRESS) {
        if (!f3WasPressed) {
            m_ShowDebugInfo = !m_ShowDebugInfo;
            f3WasPressed = true;
        }
    } else {
        f3WasPressed = false;
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
    
m_Debug = std::make_unique<Luminumbra::Debug::Debug>();
    LOG("Engine::initRendering - Debug created.");

    // Enable depth testing so the terrain draws correctly
    glEnable(GL_DEPTH_TEST);
    LOG("Engine::initRendering - Depth Test enabled.");
    LOG("Engine::initRendering - Finish");
}

// --- NEW: render ---
void Engine::render() {
    glClearColor(0.28f, 0.24f, 0.55f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const auto& camera = m_Player->getCamera();

    m_BasicShader->use();
    // Set lighting uniforms (we only need to do this once per frame)
    m_BasicShader->setVec3("lightPos", glm::vec3(16.0f, 50.0f, 16.0f)); // A light high up in the "sky"
    m_BasicShader->setVec3("viewPos", camera.getPosition());
    m_BasicShader->setVec3("lightColor", glm::vec3(1.0f, 1.0f, 1.0f));
    m_BasicShader->setVec3("objectColor", glm::vec3(0.8f, 0.8f, 0.9f)); // A pale rock color

    m_BasicShader->setMat4("projection", camera.getProjectionMatrix());
    m_BasicShader->setMat4("view", camera.getViewMatrix());
    if (m_World) {
        m_World->render(*m_BasicShader); // Tell the world to render itself
    }

    if (m_Debug) {
        m_Debug->drawAxes(camera.getProjectionMatrix(), camera.getViewMatrix());
    }
    
    // Start ImGui frame
    m_UIManager->NewFrame();
    
    // Show UI elements
    if (m_ShowMenu) {
        m_UIManager->ShowMainMenu();
        m_UIManager->ShowSettingsWindow();
    }
    
    if (m_ShowDebugInfo && !m_ShowMenu) {
        glm::vec3 playerPos = m_Player->getPosition();
        glm::vec3 playerVel = m_Player->getVelocity();
        const char* mode = m_Player->isNoClipMode() ? "NoClip" : "Normal";
        // Pass the horizontal speed (magnitude of XZ velocity) to the overlay
        m_UIManager->ShowDebugOverlay(playerPos.x, playerPos.y, playerPos.z, glm::length(glm::vec2(playerVel.x, playerVel.z)), m_Player->getGravity(), mode);
    }
    
    // Show stamina bar when not in menu and not in noclip mode
    if (!m_ShowMenu && !m_Player->isNoClipMode()) {
        m_UIManager->ShowStaminaBar(m_Player->getStamina(), m_Player->getMaxStamina(), m_Player->isSprinting());
    }
    
    // Render ImGui
    m_UIManager->Render();
}

// --- NEW: shutdown ---
void Engine::shutdown() {

}

// --- run loop ---
void Engine::run() {
    LOG("Engine::run - Starting main loop.");
    float lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(m_Window)) {
        float currentTime = glfwGetTime();
        float deltaTime = currentTime - lastTime;
        lastTime = currentTime;

        glfwPollEvents();
        processInput();
        
        // Only update player if menu is not shown
        if (!m_ShowMenu) {
            m_Player->update(m_Window, deltaTime, *m_World);
        }

        render();

        glfwSwapBuffers(m_Window);
    }
    LOG("Engine::run - Main loop finished.");
}

} // namespace Luminumbra::Core
