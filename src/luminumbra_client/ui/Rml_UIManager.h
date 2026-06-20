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

// --- Settings bridge ---
// The UI layer must not depend on luminumbra_common's SystemConfig directly (it lives in
// main_client, which owns the global g_systemConfig). Instead the host wires this small POD
// of callbacks: the Settings screen (settings.rml) reads initial values via the getters,
// pushes live changes via the setters, and persists via Save(). Every field is optional —
// any null callback is simply skipped, so the screen degrades gracefully if unwired.
struct SettingsBridge {
    // Video
    std::function<std::string()> GetResolution;        // "" = native/default, else "WxH"
    std::function<void(const std::string&)> SetResolution;
    std::function<std::string()> GetWindowMode;        // "windowed" | "borderless" | "fullscreen"
    std::function<void(const std::string&)> SetWindowMode;
    std::function<bool()> GetVSync;
    std::function<void(bool)> SetVSync;
    std::function<float()> GetFov;
    std::function<void(float)> SetFov;
    std::function<float()> GetMouseSensitivity;
    std::function<void(float)> SetMouseSensitivity;
    // Audio (0..1)
    std::function<float()> GetAudioMaster;
    std::function<void(float)> SetAudioMaster;
    std::function<float()> GetAudioSfx;
    std::function<void(float)> SetAudioSfx;
    std::function<float()> GetAudioMusic;
    std::function<void(float)> SetAudioMusic;
    // Persist the user overlay (returns true on success).
    std::function<bool()> Save;
};

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

    // T007: last UI-pass submit time in ms (CPU draw-submission cost; meaningful while the UI
    // renderer is unbatched). A GPU timer-query refinement is deferred to the perf-hardening phase.
    double GetLastUiFrameMs() const { return m_lastUiFrameMs; }

    void RequestLoadDocument(std::string path);
    
    // Fixed: GetContext() is now defined inline here, solving the redefinition error.
    Rml::Context* GetContext() { return m_context; }
    
    void SetWorldCreationCallback(WorldCreationCallback callback) { m_worldCreationCallback = std::move(callback); }
    void SetLoadWorldCallback(LoadWorldCallback callback) { m_loadWorldCallback = std::move(callback); }
    void SetSettingsBridge(SettingsBridge bridge) { m_settingsBridge = std::move(bridge); }

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

    // settings.rml support: populate widgets from the bridge on load, and push a single
    // changed widget's value back through the bridge live.
    void PopulateSettingsForm(Rml::ElementDocument* document);
    void ApplySettingFromElement(Rml::Element* element);
    void BindSettingsListeners(Rml::ElementDocument* document);

    // Interfaces are now members, their lifetime is tied to the manager.
    RmlSystem m_systemInterface;
    RmlFileInterface m_fileInterface;
    RmlRenderer m_renderInterface;
    
    Rml::Context* m_context = nullptr;
    GLFWwindow* m_window = nullptr;
    IAudioManager* m_audioManager = nullptr;
    
    WorldCreationCallback m_worldCreationCallback;
    LoadWorldCallback m_loadWorldCallback;
    SettingsBridge m_settingsBridge;
    
    std::string m_documentToLoad;
    std::string m_activeDocument;
    std::string m_selectedWorldId;

    // T006: cache the last pushed context size so SetDimensions only fires on an actual resize
    // (a per-frame SetDimensions can needlessly dirty layout). -1 forces the first push.
    int m_lastWidth = -1;
    int m_lastHeight = -1;
    // T007: last UI-pass CPU submit time (ms).
    double m_lastUiFrameMs = 0.0;

    // Static pointer to the active instance for callbacks
    static Rml_UIManager* s_active_manager;
};

} // namespace Luminumbra::Client
