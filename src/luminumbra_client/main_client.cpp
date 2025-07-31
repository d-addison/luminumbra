#include <iostream>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "audio/IAudioManager.h"
#include "ui/Rml_UIManager.h"

// Globals for our simple client
const unsigned int SCR_WIDTH = 1280;
const unsigned int SCR_HEIGHT = 720;
GLFWwindow* g_window = nullptr;
Luminumbra::Client::Rml_UIManager* g_uiManager = nullptr;
std::unique_ptr<Luminumbra::Client::IAudioManager> g_audioManager;

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

void InitSystems() {
    // GLFW
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    g_window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "Luminumbra", NULL, NULL);
    if (g_window == NULL) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        exit(-1);
    }
    glfwMakeContextCurrent(g_window);
    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_callback);

    // GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        exit(-1);
    }

    // Audio Manager
    g_audioManager = Luminumbra::Client::CreateAudioManager();
    if (!g_audioManager || !g_audioManager->Init()) {
        std::cerr << "Failed to initialize Audio Manager!" << std::endl;
        // Handle error
    }

    // UI Manager
    g_uiManager = new Luminumbra::Client::Rml_UIManager();
    g_uiManager->Init(g_window, g_audioManager.get()); 
}

void ShutdownSystems() {
    g_uiManager->Shutdown();
    delete g_uiManager;
    
    if (g_audioManager) {
        g_audioManager->Shutdown();
    }

    glfwDestroyWindow(g_window);
    glfwTerminate();
}

int main() {
    InitSystems();

    // --- Load Game Content ---
    g_uiManager->LoadDocument("main_menu.rml");
    g_audioManager->LoadBank("data/audio/music.bank.json");
    g_audioManager->LoadBank("data/audio/sfx_main.bank.json");

    // Create a handle and play the music
    Luminumbra::Client::AudioEventHandle musicHandle;
    g_audioManager->PlayEvent("music_main_menu", musicHandle);
    
    // --- Main Loop ---
    while (!glfwWindowShouldClose(g_window)) {
        // Input
        glfwPollEvents();

        // Update
        if (g_audioManager) g_audioManager->Update();
        g_uiManager->Update();

        // Render
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        g_uiManager->Render();

        // Swap buffers
        glfwSwapBuffers(g_window);
    }

    ShutdownSystems();

    return 0;
}