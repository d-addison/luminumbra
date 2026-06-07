#pragma once

#include "UIStateManager.h"
#include "UIComponent.h"
#include "../Rml_Interfaces.h"
#include "../components/common/Button.h"
#include "../components/common/Input.h"
#include "../components/common/Panel.h"
#include "UIHotReload.h"

#include <RmlUi/Core.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>

struct GLFWwindow;

namespace Luminumbra::Client {
class IAudioManager;
}

namespace Luminumbra::Client::UI {

/**
 * Enhanced UI Manager that integrates the new component system
 * with the existing RmlUi infrastructure
 */
class UIManager {
public:
    UIManager(const std::string& asset_root_path);
    ~UIManager();

    // Prevent copying
    UIManager(const UIManager&) = delete;
    UIManager& operator=(const UIManager&) = delete;

    // Core lifecycle
    void Init(GLFWwindow* window, IAudioManager* audioManager);
    void Shutdown();
    void Update(float deltaTime);
    void Render();

    // Document management
    void LoadDocument(const std::string& rml_path);
    void RequestLoadDocument(const std::string& path);
    Rml::Context* GetContext() { return m_context; }

    // Component management
    template<typename T, typename... Args>
    std::shared_ptr<T> CreateComponent(const std::string& elementId, Args&&... args);
    
    std::shared_ptr<UIComponent> GetComponent(const std::string& elementId);
    void RemoveComponent(const std::string& elementId);
    void ClearComponents();

    // Convenience methods for common components
    std::shared_ptr<Button> CreateButton(const std::string& elementId);
    std::shared_ptr<Input> CreateInput(const std::string& elementId);
    std::shared_ptr<Panel> CreatePanel(const std::string& elementId);

    // Event system integration
    using WorldCreationCallback = std::function<void(const std::string&, const std::string&, const std::string&)>;
    using LoadWorldCallback = std::function<void(const std::string&)>;
    
    void SetWorldCreationCallback(WorldCreationCallback callback) { m_worldCreationCallback = std::move(callback); }
    void SetLoadWorldCallback(LoadWorldCallback callback) { m_loadWorldCallback = std::move(callback); }

    // Navigation helpers
    void NavigateToMainMenu();
    void NavigateToWorldCreation();
    void NavigateToWorldSelection();
    void ShowModal(const std::string& modalId);
    void HideModal();

    // Notification system
    void ShowNotification(const std::string& message, float timeout = 3.0f);
    void ClearNotification();

    // Theme management
    void LoadTheme(const std::string& themeName);
    void ReloadCurrentTheme();

    // Hot-reload support
    void EnableHotReload(bool enabled);
    bool IsHotReloadEnabled() const { return m_hotReloadEnabled; }

    // Static GLFW callbacks (unchanged for compatibility)
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void CharCallback(GLFWwindow* window, unsigned int codepoint);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

private:
    // RmlUi interfaces
    RmlSystem m_systemInterface;
    RmlFileInterface m_fileInterface;
    RmlRenderer m_renderInterface;
    
    Rml::Context* m_context = nullptr;
    GLFWwindow* m_window = nullptr;
    IAudioManager* m_audioManager = nullptr;
    
    // Component management
    std::unordered_map<std::string, std::shared_ptr<UIComponent>> m_components;
    
    // Document management
    std::string m_documentToLoad;
    std::string m_activeDocument;
    std::string m_currentTheme = "quantum";
    
    // Callbacks
    WorldCreationCallback m_worldCreationCallback;
    LoadWorldCallback m_loadWorldCallback;
    
    // Hot-reload support
    bool m_hotReloadEnabled = false;
    std::unique_ptr<UIHotReload> m_hotReload;
    
    // State management
    UIStateManager* m_stateManager = nullptr;
    
    // Static instance for callbacks
    static UIManager* s_active_manager;
    
    // Internal methods
    void ProcessDocumentLoadRequest();
    void BindEventListeners(Rml::ElementDocument* document);
    void InitializeComponents(Rml::ElementDocument* document);
    void CheckHotReload();
    void UpdateNotifications(float deltaTime);
    
    // Component event handlers
    void HandleWorldCreation();
    void HandleWorldLoad(const std::string& worldId);
    void HandleQuit();
    void HandleNavigation(const std::string& target);
    
    // Utility methods
    void AddClickSoundToElement(Rml::Element* element);
    std::shared_ptr<UIComponent> FindOrCreateComponent(const std::string& elementId, Rml::ElementDocument* document);
};

// Template implementation
template<typename T, typename... Args>
std::shared_ptr<T> UIManager::CreateComponent(const std::string& elementId, Args&&... args) {
    auto component = std::make_shared<T>(elementId, std::forward<Args>(args)...);
    m_components[elementId] = component;
    
    // Initialize component with current document if available
    if (m_context && m_context->GetNumDocuments() > 0) {
        for (int i = 0; i < m_context->GetNumDocuments(); ++i) {
            if (auto* doc = m_context->GetDocument(i)) {
                if (doc->GetElementById(elementId)) {
                    component->Initialize(doc);
                    break;
                }
            }
        }
    }
    
    return component;
}

} // namespace Luminumbra::Client::UI