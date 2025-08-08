#include "core/Log.h"
#include "core/Debug.h"
#include "core/GameState.h"
#include "../luminumbra_common/systems/SHIELD_WorldSystem.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "ui/Rml_UIManager.h"
#include "audio/AudioManagerFactory.h"
#include "audio/IAudioManager.h"
#include "rendering/RenderPipeline.h"
#include "rendering/Camera.h"
#include "../luminumbra_common/world/GameSession.h"
#include "../luminumbra_common/core/JobSystem.h"

#include "debug/WorldGenViewer.h"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <memory> // for std::unique_ptr
#include <filesystem>

// Forward declaration for our OpenGL debug callback
void GLAPIENTRY GLDebugMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);

// GLFW error callback
void GLFWErrorCallback(int error, const char* description) {
    LUMINUMBRA_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
}

void processInput(GLFWwindow *window, Luminumbra::Rendering::Camera& camera, float deltaTime)
{
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.ProcessKeyboard(Luminumbra::Rendering::FORWARD, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.ProcessKeyboard(Luminumbra::Rendering::BACKWARD, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.ProcessKeyboard(Luminumbra::Rendering::LEFT, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.ProcessKeyboard(Luminumbra::Rendering::RIGHT, deltaTime);
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);

void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState);

Luminumbra::Rendering::Camera camera(glm::vec3(16.0f, 50.0f, 16.0f), glm::vec3(0.0f, 1.0f, 0.0f), -135.0f, -45.0f);
float lastX = 1280 / 2.0f;
float lastY = 720 / 2.0f;
bool firstMouse = true;

int main(int argc, char* argv[]) {
    // --- 1. Initialize Core Systems (Logging First!) ---
    Log::Init();
    LUMINUMBRA_CORE_INFO("Logger Initialized.");

    std::filesystem::path exe_path = argv[0];
    // Go up three levels from build/bin/luminumbra_client_app.exe to the project root
    std::filesystem::path root_dir = exe_path.parent_path().parent_path().parent_path();
    std::string root_path_str = root_dir.string() + "/";
    LUMINUMBRA_CORE_INFO("Application Root Path: {0}", root_path_str);

    // Set the error callback *before* initializing GLFW
    glfwSetErrorCallback(GLFWErrorCallback);

    // --- 2. Initialize GLFW and Window Hints ---
    LUMINUMBRA_CORE_INFO("Initializing GLFW...");
    if (!glfwInit()) {
        LUMINUMBRA_CORE_CRITICAL("FATAL: Failed to initialize GLFW!");
        // You might want to add a getchar() here in some environments to see the message before the console closes.
        return -1;
    }

    // Request a modern OpenGL 4.5 Core context
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Request a debug context to get detailed error messages
    #ifdef LUMINUMBRA_DEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
    #endif

    // --- 3. Create Window ---
    LUMINUMBRA_CORE_INFO("Creating Window...");
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Luminumbra", nullptr, nullptr);
    LUMINUMBRA_ASSERT(window, "Failed to create GLFW window!");

    // --- 4. Create and Bind OpenGL Context ---
    LUMINUMBRA_CORE_INFO("Making OpenGL Context Current...");
    glfwMakeContextCurrent(window);

    // --- 5. Initialize GLAD ---
    // THIS MUST BE AFTER the context is made current.
    LUMINUMBRA_CORE_INFO("Initializing GLAD...");
    int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    LUMINUMBRA_ASSERT(status, "Failed to initialize GLAD!");
    LUMINUMBRA_CORE_INFO("OpenGL Version: {0}", (const char*)glGetString(GL_VERSION));
    LUMINUMBRA_CORE_INFO("GPU: {0}", (const char*)glGetString(GL_RENDERER));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // --- 6. Setup OpenGL Debug Callback (if in debug mode) ---
    #ifdef LUMINUMBRA_DEBUG
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS); // For easier debugging
        glDebugMessageCallback(GLDebugMessageCallback, nullptr);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, NULL, GL_FALSE);
        LUMINUMBRA_CORE_INFO("OpenGL Debug Context Initialized.");
    #endif

    // --- 7. LATE INITIALIZATION: Construct Rendering Objects ---
    // NOW it is safe to create objects that make OpenGL calls.
    // The RmlRenderer, AssetManager, etc., should be created here.
    LUMINUMBRA_CORE_INFO("Initializing rendering subsystems...");
    
    // Initialize game state manager
    GameStateManager gameStateManager;
    gameStateManager.SetState(GameState::MAIN_MENU);
    
    // Initialize job system for world generation
    Luminumbra::JobSystem jobSystem;
    jobSystem.startup();
    
    // Initialize game session
    auto gameSession = std::make_unique<Luminumbra::world::GameSession>();
    gameSession->SetJobSystem(&jobSystem);
    gameSession->SetRootPath(root_path_str); 
    
    auto audioManager = Luminumbra::Client::CreateAudioManager(root_path_str);
    audioManager->Init();

    // Load audio banks *after* manager is initialized
    audioManager->LoadBank("data/audio/sfx_main.bank.json");
    audioManager->LoadBank("data/audio/music.bank.json");

    Luminumbra::Client::Rml_UIManager uiManager(root_path_str);
    uiManager.Init(window, audioManager.get());

    // MODIFIED: Pass the root path to the render pipeline
    Luminumbra::Rendering::RenderPipeline renderPipeline;
    renderPipeline.startup(1920, 1080, root_path_str);
    
    // Track the current music handle for stopping later
    Luminumbra::Common::AudioEventHandle currentMusicHandle;
    
    // Set up world creation callback
    uiManager.SetWorldCreationCallback([&](const std::string& name, const std::string& seed, const std::string& worldType) {
        LUMINUMBRA_CORE_INFO("Creating world: {0}, Seed: {1}, Type: {2}", name, seed, worldType);
        if (gameSession->CreateWorld(name, seed, worldType)) {
            audioManager->StopMusic();
            if (uiManager.GetContext()) {
                for (int i = uiManager.GetContext()->GetNumDocuments() - 1; i >= 0; --i) {
                    auto* doc = uiManager.GetContext()->GetDocument(i);
                    if (doc && doc->GetId().find("rmlui-debug") == Rml::String::npos) {
                        doc->Hide();
                    }
                }
            }
            LUMINUMBRA_CORE_INFO("World created successfully! Entering game...");
            // Use our new function to switch to the in-game state
            SetGameState(window, gameStateManager, GameState::IN_GAME);
        } else {
            LUMINUMBRA_CORE_ERROR("Failed to create world!");
        }
    });

    // --- 8. Set Initial Game State ---
    SetGameState(window, gameStateManager, GameState::MAIN_MENU);

    // --- 9. Load Initial UI Document ---
    LUMINUMBRA_CORE_INFO("Loading Main Menu...");
    uiManager.LoadDocument("main_menu.rml");

    // --- 10. Play Music ---
    audioManager->PlayMusic("music_main_menu");

    auto worldGenViewer = std::make_unique<Luminumbra::Client::WorldGenViewer>();


    // --- Main Loop ---
    float lastFrame = 0.0f;
    while (!glfwWindowShouldClose(window)) {
        // --- Timing ---
        float currentFrame = glfwGetTime();
        float deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        // --- Input ---
        glfwPollEvents();
        processInput(window, camera, deltaTime);

        // --- Update ---
        audioManager->Update();
        // JobSystem doesn't need explicit update - it processes jobs asynchronously
        
        GameState currentState = gameStateManager.GetCurrentState();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        switch (currentState) {
            case GameState::MAIN_MENU:
                uiManager.Update();
                // Add a way to enter the viewer, e.g., a button or key press
                if (glfwGetKey(window, GLFW_KEY_F7) == GLFW_PRESS) {
                    SetGameState(window, gameStateManager, GameState::WORLD_GEN_VIEWER);
                }
                break;
            
            case GameState::WORLD_GEN_VIEWER:
                worldGenViewer->UpdateAndRender(); // This draws the ImGui window
                if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                    SetGameState(window, gameStateManager, GameState::MAIN_MENU);
                }
                break;
                
            case GameState::IN_GAME:
                // Update world systems
                if (gameSession->GetWorldSystem()) {
                    gameSession->GetWorldSystem()->update(gameSession->GetRegistry(), camera.Position);
                }
                break;
                
            case GameState::EXITING:
                glfwSetWindowShouldClose(window, true);
                break;
        }

        // --- Rendering ---
        if (currentState == GameState::IN_GAME) {
            // In-game rendering
            glClearColor(0.2f, 0.3f, 0.5f, 1.0f); // Sky blue for in-game
        } else {
            // Menu rendering
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Dark grey for menu
        }
        
        // Clear the color and depth buffers
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Render based on state
        switch (currentState) {
            case GameState::MAIN_MENU:
            case GameState::WORLD_GEN_VIEWER:
                 uiManager.Render();
                 break;
                
            case GameState::IN_GAME:
                if (gameSession->GetWorldSystem()) {
                    renderPipeline.render_frame(*gameSession->GetWorldSystem(), camera);
                }
                
                // Allow pressing ESC to return to menu
                if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                    gameStateManager.SetState(GameState::MAIN_MENU);
                    uiManager.LoadDocument("main_menu.rml");
                }
                break;
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // --- Swap Buffers ---
        // Swaps the front and back buffers of the window
        glfwSwapBuffers(window);
    }

    // --- Shutdown ---
    LUMINUMBRA_CORE_INFO("Shutting down.");
    
    // Save world if in-game
    if (gameStateManager.GetCurrentState() == GameState::IN_GAME && gameSession) {
        gameSession->SaveWorld();
    }
    
    uiManager.Shutdown();
    audioManager->Shutdown();
    jobSystem.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState) {
    gameStateManager.SetState(newState);

    if (newState == GameState::MAIN_MENU) {
        // Show cursor and set UI callbacks (Rml_UIManager's Init already did this)
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursorPosCallback(window, Luminumbra::Client::Rml_UIManager::CursorPosCallback);
        // Let RmlUi handle mouse buttons
        glfwSetMouseButtonCallback(window, Luminumbra::Client::Rml_UIManager::MouseButtonCallback);
        glfwSetScrollCallback(window, Luminumbra::Client::Rml_UIManager::ScrollCallback);
    } 
    else if (newState == GameState::IN_GAME) {
        // Hide cursor and set camera callbacks
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwSetCursorPosCallback(window, mouse_callback);
        // Game logic would handle clicks, so we can unset the UI's callback for now
        glfwSetMouseButtonCallback(window, nullptr); 
        glfwSetScrollCallback(window, scroll_callback);

        // Reset mouse position tracking for the camera to prevent a jump
        firstMouse = true;
    }
}


// Implementation of the debug callback function
void GLAPIENTRY GLDebugMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
    // ignore non-significant error/warning codes
    if(id == 131169 || id == 131185 || id == 131218 || id == 131204) return; 

    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:         LUMINUMBRA_CORE_CRITICAL("OpenGL Debug: {0}", message); break;
        case GL_DEBUG_SEVERITY_MEDIUM:       LUMINUMBRA_CORE_ERROR("OpenGL Debug: {0}", message);    break;
        case GL_DEBUG_SEVERITY_LOW:          LUMINUMBRA_CORE_WARN("OpenGL Debug: {0}", message);     break;
        case GL_DEBUG_SEVERITY_NOTIFICATION: LUMINUMBRA_CORE_TRACE("OpenGL Debug: {0}", message);    break;
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    if (firstMouse)
    {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }

    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos; // reversed since y-coordinates go from bottom to top

    lastX = xpos;
    lastY = ypos;

    camera.ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    camera.ProcessMouseScroll(yoffset);
}
