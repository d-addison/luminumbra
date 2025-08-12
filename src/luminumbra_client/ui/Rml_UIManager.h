#pragma once

#include "Rml_Interfaces.h" // The one true source for interface definitions
#include <RmlUi/Core.h>
#include <string>
#include <functional>
#include <memory>
#include <utility> // For std::move

struct GLFWwindow;

namespace Luminumbra::Client {

class IAudioManager;

// --- Callbacks for UI interaction ---
using WorldCreationCallback = std::function<void(const std::string&, const std::string&, const std::string&)>;
using LoadWorldCallback = std::function<void(const std::string&)>;

class Rml_UIManager {
public:
    Rml_UIManager(const std::string& asset_root_path);
    ~Rml_UIManager();

    // Prevent copying
    Rml_UIManager(const Rml_UIManager&) = delete;
    Rml_UIManager& operator=(const Rml_UIManager&) = delete;

    void Init(GLFWwindow* window, IAudioManager* audioManager);
    void Shutdown();

    void Update();
    void Render();

    void RequestLoadDocument(std::string path);
    
    // Fixed: GetContext() is now defined inline here, solving the redefinition error.
    Rml::Context* GetContext() { return m_context; }
    
    void SetWorldCreationCallback(WorldCreationCallback callback) { m_worldCreationCallback = std::move(callback); }
    void SetLoadWorldCallback(LoadWorldCallback callback) { m_loadWorldCallback = std::move(callback); }

    // Static GLFW callbacks that forward to the active manager instance
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void CharCallback(GLFWwindow* window, unsigned int codepoint);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

private:
    // This is now a public function on the manager, called by Update().
    void ProcessDocumentLoadRequest();
    void BindEventListeners(Rml::ElementDocument* document);
    void LoadDocument(const std::string& rml_path);

    // Interfaces are now members, their lifetime is tied to the manager.
    RmlSystem m_systemInterface;
    RmlFileInterface m_fileInterface;
    RmlRenderer m_renderInterface;
    
    Rml::Context* m_context = nullptr;
    GLFWwindow* m_window = nullptr;
    IAudioManager* m_audioManager = nullptr;
    
    WorldCreationCallback m_worldCreationCallback;
    LoadWorldCallback m_loadWorldCallback;
    
    std::string m_documentToLoad;
    std::string m_activeDocument;

    // Static pointer to the active instance for callbacks
    static Rml_UIManager* s_active_manager;
};

} // namespace Luminumbra::Client