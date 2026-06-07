#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "core/Log.h"
#include "core/Debug.h"
#include "core/GameState.h"
#include "player/PlayerController.h"
#include "rendering/Camera.h"
#include "rendering/RenderPipeline.h"
#include "rendering/WorldLoadingVisualizer.h"
#include "ui/Rml_UIManager.h"
#include "audio/AudioManagerFactory.h"
#include "audio/IAudioManager.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/core/JobSystem.h"
#include "debug/WorldGenViewer.h"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <memory>
#include <algorithm>
#include <filesystem>
#include "luminumbra_common/components/CoreComponents.h"

// --- Global Pointers ---
std::unique_ptr<Luminumbra::Rendering::Camera> g_camera;
std::unique_ptr<Luminumbra::Client::PlayerController> g_playerController;
std::unique_ptr<Luminumbra::Client::Rml_UIManager> g_uiManager;
std::unique_ptr<Luminumbra::Client::WorldLoadingVisualizer> g_loading_visualizer;

// --- Forward Declarations ---
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void GLAPIENTRY GLDebugMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);
void GLFWErrorCallback(int error, const char* description);
void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState);

// --- Global State ---
float lastX = 1280 / 2.0f;
float lastY = 720 / 2.0f;
bool firstMouse = true;
bool show_worldgen_viewer = false;
bool wireframe_mode = false;

std::vector<Luminumbra::IVec3> g_initial_chunks_to_load;
int g_generation_dispatch_index = 0;
Luminumbra::JobHandle g_world_gen_handle;

struct WindowState {
    bool isFullscreen = false;
    int windowedX = 100, windowedY = 100;
    int windowedWidth = 1280, windowedHeight = 720;
};
WindowState g_windowState;

void ToggleFullscreen(GLFWwindow* window, WindowState& state) {
    if (state.isFullscreen) {
        glfwSetWindowMonitor(window, nullptr, state.windowedX, state.windowedY, state.windowedWidth, state.windowedHeight, 0);
    } else {
        glfwGetWindowPos(window, &state.windowedX, &state.windowedY);
        glfwGetWindowSize(window, &state.windowedWidth, &state.windowedHeight);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    }
    state.isFullscreen = !state.isFullscreen;
}

int main(int argc, char* argv[]) {
    Log::Init();
    std::filesystem::path exe_path = argv[0];
    std::filesystem::path root_dir = exe_path.parent_path().parent_path().parent_path();
    std::string root_path_str = root_dir.string() + "/";

    glfwSetErrorCallback(GLFWErrorCallback);
    if (!glfwInit()) { LUMINUMBRA_CORE_ERROR("FATAL: Failed to initialize GLFW!"); return -1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef LUMINUMBRA_DEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
    #endif

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Luminumbra", nullptr, nullptr);
    LUMINUMBRA_ASSERT(window, "Failed to create GLFW window!");
    glfwMakeContextCurrent(window);
    // g_windowState.isFullscreen = false;
    // ToggleFullscreen(window, g_windowState);

    int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    LUMINUMBRA_ASSERT(status, "Failed to initialize GLAD!");
    
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    #ifdef LUMINUMBRA_DEBUG
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(GLDebugMessageCallback, nullptr);
    #endif

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    GameStateManager gameStateManager;
    Luminumbra::JobSystem jobSystem;
    jobSystem.startup();

    auto gameSession = std::make_unique<Luminumbra::world::GameSession>();
    gameSession->SetJobSystem(&jobSystem);
    gameSession->SetRootPath(root_path_str);

    auto audioManager = Luminumbra::Client::CreateAudioManager(root_path_str);
    audioManager->Init();
    audioManager->LoadBank("data/audio/sfx_main.bank.json");
    audioManager->LoadBank("data/audio/music.bank.json");

    g_uiManager = std::make_unique<Luminumbra::Client::Rml_UIManager>(root_path_str);
    g_uiManager->Init(window, audioManager.get());

    Luminumbra::Rendering::RenderPipeline renderPipeline;
    renderPipeline.startup(framebufferWidth, framebufferHeight, root_dir);
    glfwSetWindowUserPointer(window, &renderPipeline);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    g_loading_visualizer = std::make_unique<Luminumbra::Client::WorldLoadingVisualizer>();
    g_loading_visualizer->Startup(root_dir, framebufferWidth, framebufferHeight);

    g_uiManager->SetWorldCreationCallback([&](const std::string& name, const std::string& seed, const std::string& worldType) {
        // 1. Synchronously create the world systems and metadata. This is fast.
        if (gameSession->CreateWorld(name, seed, worldType)) {
            // Hide the main menu UI
            for (int i = 0; i < g_uiManager->GetContext()->GetNumDocuments(); ++i) {
                if (auto* doc = g_uiManager->GetContext()->GetDocument(i)) { doc->Hide(); }
            }
            if (Rml::Element* focused_element = g_uiManager->GetContext()->GetFocusElement()) {
                focused_element->Blur();
            }

            // 2. Switch to the loading state
            SetGameState(window, gameStateManager, GameState::WORLD_LOADING);
            
            // 3. Get the list of chunks to generate and start the visualizer
            auto* world_system = gameSession->GetWorldSystem();
            // Use the actual spawn position for initial chunk loading
            Luminumbra::Vec3 spawn_pos = gameSession->GetMetadata().spawnPoint;
            g_initial_chunks_to_load = world_system->GetInitialChunkLoadList(spawn_pos);

            std::sort(g_initial_chunks_to_load.begin(), g_initial_chunks_to_load.end(), [](const auto& a, const auto& b) {
                if (a.y != b.y) return a.y < b.y;
                if (a.x != b.x) return a.x < b.x;
                return a.z < b.z;
            });

            g_generation_dispatch_index = 0;
            g_loading_visualizer->BeginVisualization(g_initial_chunks_to_load);

        } else {
            LUMINUMBRA_CORE_ERROR("Failed to create world!");
        }
    });

    glfwSetKeyCallback(window, key_callback);
    SetGameState(window, gameStateManager, GameState::MAIN_MENU);
    audioManager->PlayMusic("music_main_menu");
    g_uiManager->RequestLoadDocument("main_menu.rml");

    auto worldGenViewer = std::make_unique<Luminumbra::Client::WorldGenViewer>();

    float lastFrame = 0.0f;
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = (float)glfwGetTime();
        float deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        glfwPollEvents();
        audioManager->Update();

        if (g_uiManager) {
            g_uiManager->Update();
        }
        
        GameState currentState = gameStateManager.GetCurrentState();
        
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        switch (currentState) {
            case GameState::WORLD_LOADING:
            {
                const int JOBS_PER_FRAME = 64; // Dispatch more jobs per frame for a faster visual fill
                if (g_generation_dispatch_index < g_initial_chunks_to_load.size()) {
                    std::vector<Luminumbra::IVec3> batch_to_generate;
                    int dispatched_this_frame = 0;
                    while (g_generation_dispatch_index < g_initial_chunks_to_load.size() && dispatched_this_frame < JOBS_PER_FRAME) {
                        const auto& coords = g_initial_chunks_to_load[g_generation_dispatch_index];
                        batch_to_generate.push_back(coords);
                        g_loading_visualizer->UpdateChunkState(coords, Luminumbra::Client::ChunkLoadVisualState::DISPATCHED);
                        g_generation_dispatch_index++;
                        dispatched_this_frame++;
                    }
                    if(!batch_to_generate.empty()) {
                        gameSession->GetWorldSystem()->dispatch_generation_jobs(batch_to_generate);
                    }
                }

                float progress = g_initial_chunks_to_load.empty() ? 1.0f : static_cast<float>(g_generation_dispatch_index) / g_initial_chunks_to_load.size();
                
                glClearColor(0.01f, 0.02f, 0.05f, 1.0f); // Dark blue background
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                g_loading_visualizer->UpdateAndRender(deltaTime, "CONSTRUCTING WORLD GEOMETRY...", progress);

                if (g_generation_dispatch_index >= g_initial_chunks_to_load.size()) {
                    LUMINUMBRA_CORE_INFO("World generation phase complete. Entering world.");
                    audioManager->StopMusic();
                    audioManager->PlayOneShot2D("ui_world_loaded"); // Play a sound on completion
                    g_loading_visualizer->EndVisualization();
                    g_camera = std::make_unique<Luminumbra::Rendering::Camera>(gameSession->GetMetadata().spawnPoint);
                    g_playerController = std::make_unique<Luminumbra::Client::PlayerController>(window, g_camera.get(), gameSession->GetPhysicsSystem());
                    // Clear any cached render data to ensure fresh uploads after our fixes
                    renderPipeline.clear_all_chunk_data();
                    SetGameState(window, gameStateManager, GameState::IN_GAME);
                }
                break;
            }

            case GameState::IN_GAME:
                if (g_playerController) g_playerController->Update(deltaTime);
                if (auto* physics = gameSession->GetPhysicsSystem()) physics->update(deltaTime);
                if (gameSession->GetWorldSystem() && g_playerController) {
                    gameSession->GetWorldSystem()->update(
                        gameSession->GetRegistry(),
                        g_playerController->GetPosition(),
                        gameSession->GetPhysicsSystem()
                    );
                }
                break;
            case GameState::MAIN_MENU:
                break;
            case GameState::EXITING:
                glfwSetWindowShouldClose(window, true);
                break;
            default:
                break;
        }

        if (currentState != GameState::WORLD_LOADING) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (currentState == GameState::IN_GAME) {
                if (gameSession->GetWorldSystem() && g_camera) {
                    renderPipeline.render_frame(gameSession->GetRegistry(), *gameSession->GetWorldSystem(), *g_camera, deltaTime, wireframe_mode);
                }
            } else { // Main Menu, etc.
                g_uiManager->Render();
            }
        }
        
        if (currentState == GameState::IN_GAME) {
            if (g_playerController) {
                g_playerController->RenderDebugUI();
            }
            if (show_worldgen_viewer) {
                worldGenViewer->UpdateAndRender(show_worldgen_viewer, gameSession->GetWorldSystem());
            }
        }
        
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    g_playerController.reset();
    g_camera.reset();
    g_uiManager->Shutdown();
    g_loading_visualizer->Shutdown(); // <<< NEW
    audioManager->Shutdown();
    jobSystem.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key == GLFW_KEY_F7 && action == GLFW_PRESS) {
        show_worldgen_viewer = !show_worldgen_viewer;
        if (show_worldgen_viewer) {
             glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        return;
    }
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
        ToggleFullscreen(window, g_windowState);
        return;
    }
    if (key == GLFW_KEY_F9 && action == GLFW_PRESS) {
        wireframe_mode = !wireframe_mode;
        return;
    }

    if (g_playerController) {
        g_playerController->ProcessKeyInput(key, action);
    }
    
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        return;
    }

    // Always give RmlUi a chance to process key events.
    if (g_uiManager) {
        g_uiManager->KeyCallback(window, key, scancode, action, mods);
        // If RmlUi has a focused input element, it should consume the event.
        if (g_uiManager->GetContext() && g_uiManager->GetContext()->GetFocusElement()) {
             return;
        }
    }
}

void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState) {
    gameStateManager.SetState(newState);
    bool cursorDisabled = (newState == GameState::IN_GAME);

    if (cursorDisabled) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        // Set game-related callbacks
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        // Mouse buttons could be set here for game actions if needed
        glfwSetMouseButtonCallback(window, nullptr);
        firstMouse = true;
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        // Set UI-related callbacks
        glfwSetCursorPosCallback(window, Luminumbra::Client::Rml_UIManager::CursorPosCallback);
        glfwSetScrollCallback(window, Luminumbra::Client::Rml_UIManager::ScrollCallback);
        glfwSetMouseButtonCallback(window, Luminumbra::Client::Rml_UIManager::MouseButtonCallback);
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (firstMouse) {
        lastX = (float)xpos;
        lastY = (float)ypos;
        firstMouse = false;
    }
    float xoffset = (float)xpos - lastX;
    float yoffset = lastY - (float)ypos;
    lastX = (float)xpos;
    lastY = (float)ypos;
    if (g_camera) g_camera->ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    if (ImGui::GetIO().WantCaptureMouse || (g_uiManager && g_uiManager->GetContext()->GetHoverElement() != nullptr)) {
        return;
    }
    if (g_camera) g_camera->ProcessMouseScroll((float)yoffset);
    if (g_playerController) g_playerController->ProcessMouseScroll(yoffset);
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    auto* pipeline = static_cast<Luminumbra::Rendering::RenderPipeline*>(glfwGetWindowUserPointer(window));
    if (pipeline) pipeline->on_resize(width, height);
}

void GLFWErrorCallback(int error, const char* description) {
    LUMINUMBRA_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
}

void GLAPIENTRY GLDebugMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
    if(id == 131169 || id == 131185 || id == 131218 || id == 131204) return; 
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:         LUMINUMBRA_CORE_ERROR("OpenGL: {0}", message); break;
        case GL_DEBUG_SEVERITY_MEDIUM:       LUMINUMBRA_CORE_ERROR("OpenGL: {0}", message);    break;
        case GL_DEBUG_SEVERITY_LOW:          LUMINUMBRA_CORE_WARN("OpenGL: {0}", message);     break;
        case GL_DEBUG_SEVERITY_NOTIFICATION: LUMINUMBRA_CORE_TRACE("OpenGL: {0}", message);    break;
    }
}