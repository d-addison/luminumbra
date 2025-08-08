#include "ui/Rml_UIManager.h"
#include "audio/IAudioManager.h" 
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Debugger.h>
#include <RmlUi/Core/StyleTypes.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <cassert>
#include <iostream>
#include <unordered_map>

namespace Luminumbra::Client {

// Forward declarations
class NewWorldButtonListener;
class BackButtonListener;
class CreateWorldButtonListener;

// --- Rml_UIManager Implementation ---

Rml::Context* Rml_UIManager::s_activeContext = nullptr;

Rml_UIManager::Rml_UIManager(const std::string& root_path) : m_fileInterface(root_path) {}
Rml_UIManager::~Rml_UIManager() {}

void Rml_UIManager::Init(GLFWwindow* window, IAudioManager* audioManager) {
    m_window = window;
    m_audioManager = audioManager;
    std::cout << "UI Manager Initializing..." << std::endl;

    Rml::SetSystemInterface(&m_systemInterface);
    Rml::SetFileInterface(&m_fileInterface);
    Rml::SetRenderInterface(&m_renderInterface);

    Rml::Initialise();

    // Load fonts - This makes the entire "Lora" family available to CSS
    bool fonts_loaded = true;
    fonts_loaded &= Rml::LoadFontFace("data/fonts/Lora/Lora-VariableFont_wght.ttf");
    fonts_loaded &= Rml::LoadFontFace("data/fonts/Lora/Lora-Italic-VariableFont_wght.ttf");

    if (!fonts_loaded) {
        std::cerr << "FATAL ERROR: One or more fonts failed to load." << std::endl;
    }

    int width, height;
    glfwGetWindowSize(m_window, &width, &height);

    m_context = Rml::CreateContext("main", Rml::Vector2i(width, height));
    s_activeContext = m_context; // For static callbacks
    Rml::Debugger::Initialise(m_context);

    // Set up GLFW callbacks for RmlUi
    glfwSetKeyCallback(m_window, Rml_UIManager::KeyCallback);
    glfwSetCharCallback(m_window, Rml_UIManager::CharCallback);
    glfwSetMouseButtonCallback(m_window, Rml_UIManager::MouseButtonCallback);
    glfwSetCursorPosCallback(m_window, Rml_UIManager::CursorPosCallback);
    glfwSetScrollCallback(m_window, Rml_UIManager::ScrollCallback);

    std::cout << "UI Manager Initialized." << std::endl;
}

void Rml_UIManager::Shutdown() {
    Rml::Shutdown();
    s_activeContext = nullptr;
    std::cout << "UI Manager Shutdown." << std::endl;
}

void Rml_UIManager::Update() {
    if (m_context) {
        int width, height;
        glfwGetWindowSize(m_window, &width, &height);
        m_context->SetDimensions(Rml::Vector2i(width, height));
        m_renderInterface.SetViewport(width, height);
        m_context->Update();
    }
}

void Rml_UIManager::Render() {
    if (m_context) {
        m_context->Render();
    }
}

Rml::Context* Rml_UIManager::GetContext() {
    return m_context;
}

class QuitButtonListener : public Rml::EventListener {
public:
    QuitButtonListener(GLFWwindow* window) : m_window(window) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "click") {
            glfwSetWindowShouldClose(m_window, true);
        }
    }
private:
    GLFWwindow* m_window;
};

class NewWorldButtonListener : public Rml::EventListener {
public:
    NewWorldButtonListener(Rml_UIManager* uiManager, IAudioManager* audioManager) 
        : m_uiManager(uiManager), m_audioManager(audioManager) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "click") {
            std::cout << "New World button clicked!" << std::endl;
            if (m_audioManager) {
                m_audioManager->PlayOneShot2D("ui_button_click");
            }
            // Load the world creation screen
            m_uiManager->LoadDocument("world_creation.rml");
        }
    }
private:
    Rml_UIManager* m_uiManager;
    IAudioManager* m_audioManager;
};

class LoadWorldButtonListener : public Rml::EventListener {
public:
    LoadWorldButtonListener(IAudioManager* audioManager) : m_audioManager(audioManager) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "click") {
            std::cout << "Load World button clicked!" << std::endl;
            // TODO: Load world selection screen
            // For now, just log and play a sound
            if (m_audioManager) {
                m_audioManager->PlayOneShot2D("ui_button_click");
            }
            std::cout << "Load World not yet implemented - would show world selection screen here" << std::endl;
        }
    }
private:
    IAudioManager* m_audioManager;
};

class ButtonSoundListener : public Rml::EventListener {
public:
    ButtonSoundListener(IAudioManager* audio) : m_audio(audio) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "mouseover" && m_audio) {
            m_audio->PlayOneShot2D("ui_button_hover");
        }
    }
private:
    IAudioManager* m_audio;
};

class BackButtonListener : public Rml::EventListener {
public:
    BackButtonListener(Rml_UIManager* uiManager, IAudioManager* audioManager) 
        : m_uiManager(uiManager), m_audioManager(audioManager) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "click") {
            std::cout << "Back button clicked!" << std::endl;
            if (m_audioManager) {
                m_audioManager->PlayOneShot2D("ui_button_click");
            }
            // Go back to main menu
            m_uiManager->LoadDocument("main_menu.rml");
        }
    }
private:
    Rml_UIManager* m_uiManager;
    IAudioManager* m_audioManager;
};

class CreateWorldButtonListener : public Rml::EventListener {
public:
    CreateWorldButtonListener(Rml_UIManager* uiManager, IAudioManager* audioManager)
        : m_uiManager(uiManager), m_audioManager(audioManager) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "click") {
            std::cout << "Create World button clicked!" << std::endl;
            if (m_audioManager) {
                m_audioManager->PlayOneShot2D("ui_button_click");
            }
            
            // Get the world creation form data
            Rml::ElementDocument* document = event.GetTargetElement()->GetOwnerDocument();
            if (document) {
                // Get world name
                Rml::Element* nameInput = document->GetElementById("world_name");
                std::string worldName = "New World";
                if (nameInput) {
                    const Rml::Variant* value = nameInput->GetAttribute("value");
                    if (value && value->GetType() == Rml::Variant::STRING) {
                        worldName = value->Get<Rml::String>();
                    }
                }
                
                // Get seed
                Rml::Element* seedInput = document->GetElementById("world_seed");
                std::string seed = "";
                if (seedInput) {
                    const Rml::Variant* value = seedInput->GetAttribute("value");
                    if (value && value->GetType() == Rml::Variant::STRING) {
                        seed = value->Get<Rml::String>();
                    }
                }
                
                // Get world type
                Rml::Element* typeSelect = document->GetElementById("world_type");
                std::string worldType = "default";
                if (typeSelect) {
                    const Rml::Variant* value = typeSelect->GetAttribute("value");
                    if (value && value->GetType() == Rml::Variant::STRING) {
                        worldType = value->Get<Rml::String>();
                    }
                }
                
                std::cout << "Creating world:" << std::endl;
                std::cout << "  Name: " << worldName << std::endl;
                std::cout << "  Seed: " << (seed.empty() ? "(random)" : seed) << std::endl;
                std::cout << "  Type: " << worldType << std::endl;
                
                // Call the world creation callback if set
                if (m_uiManager->m_worldCreationCallback) {
                    m_uiManager->m_worldCreationCallback(worldName, seed, worldType);
                } else {
                    std::cout << "Warning: No world creation callback set!" << std::endl;
                }
            }
        }
    }
private:
    Rml_UIManager* m_uiManager;
    IAudioManager* m_audioManager;
};

void Rml_UIManager::LoadDocument(const std::string& rml_path) {
    if (!m_context) return;

    // Remove previous documents (non-debug) and their listeners
    // IMPORTANT: Clear listeners BEFORE destroying docs so no dangling calls can happen.
    m_activeListeners.clear();

    for (int i = m_context->GetNumDocuments() - 1; i >= 0; --i) {
        Rml::ElementDocument* doc = m_context->GetDocument(i);
        if (doc && doc->GetId().find("rmlui-debug") == Rml::String::npos) {
            doc->Close();
        }
    }

    Rml::ElementDocument* document = m_context->LoadDocument("data/ui/" + rml_path);
    if (!document) {
        std::cerr << "UI ERROR: Could not load document: " << rml_path << std::endl;
        assert(document != nullptr && "RML DOCUMENT FAILED TO LOAD!");
        return;
    }

    document->Show();

    if (rml_path == "main_menu.rml" && m_audioManager) {
        m_audioManager->PlayMusic("music_main_menu");
    }

    if (rml_path == "main_menu.rml") {
        if (auto* new_world_button = document->GetElementById("new_world_btn")) {
            new_world_button->AddEventListener("click", MakeListener<NewWorldButtonListener>(this, m_audioManager));
        }
        if (auto* load_world_button = document->GetElementById("load_world_btn")) {
            load_world_button->AddEventListener("click", MakeListener<LoadWorldButtonListener>(m_audioManager));
        }
        if (auto* quit_button = document->GetElementById("quit_btn")) {
            quit_button->AddEventListener("click", MakeListener<QuitButtonListener>(m_window));
        }
    } else if (rml_path == "world_creation.rml") {
        if (auto* back_button = document->GetElementById("back_btn")) {
            back_button->AddEventListener("click", MakeListener<BackButtonListener>(this, m_audioManager));
        }
        if (auto* create_button = document->GetElementById("create_btn")) {
            create_button->AddEventListener("click", MakeListener<CreateWorldButtonListener>(this, m_audioManager));
        }
    }

    // Add sound to all buttons
    Rml::ElementList buttons;
    document->GetElementsByTagName(buttons, "button");
    for (Rml::Element* button : buttons) {
        button->AddEventListener("mouseover", MakeListener<ButtonSoundListener>(m_audioManager));
    }
}

// GLFW Input Callbacks Implementation
Rml::Input::KeyIdentifier GlfwToRmlKey(int key) { /* Maps GLFW keys to RmlUi keys, implementation omitted for brevity but is required */ return Rml::Input::KI_UNKNOWN; }
int GlfwToRmlMods(int mods) { /* Maps GLFW mods, implementation omitted for brevity */ return 0; }

void Rml_UIManager::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if(!s_activeContext) return;
    // ... (Full implementation would map GLFW keys to RmlUi keys)
    if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
         Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
    }
}
void Rml_UIManager::CharCallback(GLFWwindow* window, unsigned int codepoint) {
    if (s_activeContext) s_activeContext->ProcessTextInput(codepoint);
}
void Rml_UIManager::MouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    if (s_activeContext) {
        if (action == GLFW_PRESS) s_activeContext->ProcessMouseButtonDown(button, GlfwToRmlMods(mods));
        else s_activeContext->ProcessMouseButtonUp(button, GlfwToRmlMods(mods));
    }
}
void Rml_UIManager::CursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    if (s_activeContext) s_activeContext->ProcessMouseMove( (int)xpos, (int)ypos, 0);
}
void Rml_UIManager::ScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    if (s_activeContext) s_activeContext->ProcessMouseWheel( (float)-yoffset, 0);
}


} // namespace Luminumbra::Client
