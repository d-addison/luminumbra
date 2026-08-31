#pragma once

#include "Rml_Interfaces.h"         // The one true source for interface definitions
#include "gl3/RmlUi_Renderer_GL3.h" // RmlUi 6.1 reference backend: real blur/box-shadow/layers
#include "world/WorldgenOverride.h" // WorldGenParam transport (engine-owned, not UI)
#include <RmlUi/Core.h>
#include <filesystem> // gallery fixture capture-source override
#include <functional>
#include <memory>
#include <string>
#include <utility> // For std::move
#include <vector>

struct GLFWwindow;

namespace Luminumbra::Client {

class IAudioManager;

// --- Callbacks for UI interaction ---
// WorldGenParam (the customize-form param transport) lives in the world layer, not here.
using WorldCreationCallback = std::function<void(const std::string& name,
                                                 const std::string& seed,
                                                 const std::string& worldType,
                                                 const std::vector<WorldGenParam>& params)>;
// Read a generation parameter's current value from a preset (for seeding the customize form
// when a preset chip is selected). Returns "" if the preset doesn't set that key.
using WorldParamGetter =
    std::function<std::string(const std::string& worldType, const std::string& path)>;
// Save the current customize-form config (base preset + overrides) as a reusable, named user
// preset; returns the new world-type id (e.g. "user_my_canyon") on success, "" on failure.
using WorldPresetSaver = std::function<std::string(const std::string& displayName,
                                                   const std::string& baseType,
                                                   const std::vector<WorldGenParam>& params)>;
// List saved user presets as (display name, world-type id) so create-world can offer them as
// selectable starting points alongside the curated presets.
using WorldPresetList = std::function<std::vector<std::pair<std::string, std::string>>()>;
// Returns true if saving under `displayName` would collide with an existing user preset
// (same derived slug). The UI uses this to gate the silently-overwriting saver behind an
// explicit "overwrite?" confirm. Optional — if unwired, saves proceed without a confirm.
using WorldPresetExists = std::function<bool(const std::string& displayName)>;
// Delete a saved user preset by its world-type id (e.g. "user_my_canyon"); true on success.
using WorldPresetDeleter = std::function<bool(const std::string& worldType)>;
// Rename a saved user preset (worldType) to `newDisplayName`; returns the (possibly new)
// world-type id on success, "" on failure. The id may change because it's slug-derived.
using WorldPresetRenamer =
    std::function<std::string(const std::string& worldType, const std::string& newDisplayName)>;
using LoadWorldCallback = std::function<void(const std::string&)>;
// Pause-menu actions ("resume" / "quit") routed back to main_client, which owns game state +
// cursor.
using PauseActionCallback = std::function<void(const std::string&)>;

// --- Settings bridge ---
// The UI layer must not depend on luminumbra_common's SystemConfig directly (it lives in
// main_client, which owns the global g_systemConfig). Instead the host wires this small POD
// of callbacks: the Settings screen (settings.rml) reads initial values via the getters,
// pushes live changes via the setters, and persists via Save. Every field is optional —
// any null callback is simply skipped, so the screen degrades gracefully if unwired.
struct SettingsBridge {
    // Video
    std::function<std::string()> GetResolution; // "" = native/default, else "WxH"
    std::function<void(const std::string&)> SetResolution;
    std::function<std::string()> GetWindowMode; // "windowed" | "borderless" | "fullscreen"
    std::function<void(const std::string&)> SetWindowMode;
    std::function<bool()> GetVSync;
    std::function<void(bool)> SetVSync;
    std::function<float()> GetFov;
    std::function<void(float)> SetFov;
    std::function<float()> GetMouseSensitivity;
    std::function<void(float)> SetMouseSensitivity;
    std::function<float()> GetUiScale; // HUD/UI density-independent-pixel ratio (0.5..2.5)
    std::function<void(float)> SetUiScale;
    // Audio (0..1)
    std::function<float()> GetAudioMaster;
    std::function<void(float)> SetAudioMaster;
    std::function<float()> GetAudioSfx;
    std::function<void(float)> SetAudioSfx;
    std::function<float()> GetAudioMusic;
    std::function<void(float)> SetAudioMusic;
    // Controls: current binding label for a logical action (e.g. "MoveForward" -> "W"), and a
    // request to capture the next key press as that action's new binding. Optional/skippable.
    std::function<std::string(const std::string&)> GetKeybind;
    std::function<void(const std::string&)> BeginRebind;
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

    // Last UI-pass submit time in ms. This is deliberately a CPU draw-submission
    // metric; whole-frame GPU timing is owned by the render-pipeline profiler.
    double GetLastUiFrameMs() const {
        return m_lastUiFrameMs;
    }

    void RequestLoadDocument(std::string path);

    // Hot reload : clear RmlUi's stylesheet/template caches and reload the active document
    // so.rml/.rcss edits show without a restart. Called from the UIHotReload watcher callback.
    void ReloadActiveDocument();

    // Fixed: GetContext is now defined inline here, solving the redefinition error.
    Rml::Context* GetContext() {
        return m_context;
    }

    //  the host reads this each frame to drive the live preview
    // diorama. `active` is true only while world_creation.rml is the loaded,
    // visible document AND its #preview_pane exists. The pane rect is in PIXELS
    // (top-left origin, matching glfw framebuffer coords). worldType + params are
    // the CURRENT form state (so sliding a knob re-derives the candidate world).
    // weather is the selected pill ("clear"|"rain"|"snow"|"fog"|"storm"), tod the
    // time-of-day slider [0,1]. The host blits its preview FBO into pane_{x,y,w,h}
    // and routes mouse drag/scroll over that rect to the orbit controller.
    struct PreviewState {
        bool active = false;
        std::string worldType = "default";
        std::vector<WorldGenParam> params;
        int pane_x = 0;
        int pane_y = 0;
        int pane_w = 0;
        int pane_h = 0;
        std::string weather = "clear";
        float tod = 0.24f;
    };
    PreviewState GetWorldCreationPreviewState() const;
    // Returns true once after the reset-view control was clicked, clearing the
    // pending marker (the host then re-centers the orbit camera).
    bool ConsumeWorldCreationResetView();

    void SetWorldCreationCallback(WorldCreationCallback callback) {
        m_worldCreationCallback = std::move(callback);
    }
    void SetWorldParamGetter(WorldParamGetter getter) {
        m_worldParamGetter = std::move(getter);
    }
    void SetWorldPresetSaver(WorldPresetSaver saver) {
        m_worldPresetSaver = std::move(saver);
    }
    void SetWorldPresetList(WorldPresetList lister) {
        m_worldPresetList = std::move(lister);
    }
    void SetWorldPresetExists(WorldPresetExists exists) {
        m_worldPresetExists = std::move(exists);
    }
    void SetWorldPresetDeleter(WorldPresetDeleter deleter) {
        m_worldPresetDeleter = std::move(deleter);
    }
    void SetWorldPresetRenamer(WorldPresetRenamer renamer) {
        m_worldPresetRenamer = std::move(renamer);
    }
    void SetLoadWorldCallback(LoadWorldCallback callback) {
        m_loadWorldCallback = std::move(callback);
    }
    void SetSettingsBridge(SettingsBridge bridge) {
        m_settingsBridge = std::move(bridge);
    }
    void SetPauseActionCallback(PauseActionCallback cb) {
        m_pauseActionCallback = std::move(cb);
    }

    //  (, --ui-fixtures): point the gallery at a deterministic
    // capture source. `capture_dir` is the ABSOLUTE directory PopulateGallery
    // enumerates for cap_<N>.tga; `img_prefix` is the document-relative <img src>
    // prefix those ids resolve through (gallery.rml lives in data/ui/, so the
    // default live dir <root>/data/ui/captures pairs with "captures/"). Unset =
    // the live shutter-capture path.
    void SetGalleryCaptureSource(std::filesystem::path capture_dir, std::string img_prefix) {
        m_galleryCaptureDir = std::move(capture_dir);
        m_galleryImgPrefix = std::move(img_prefix);
    }

    // Static GLFW callbacks that forward to the active manager instance
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void CharCallback(GLFWwindow* window, unsigned int codepoint);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

private:
    // This is now a public function on the manager, called by Update.
    void ProcessDocumentLoadRequest();
    void BindEventListeners(Rml::ElementDocument* document);
    void LoadDocument(const std::string& rml_path);

    // settings.rml support: populate widgets from the bridge on load, and push a single
    // changed widget's value back through the bridge live.
    // gallery.rml: populate the grid with the player's real captures (newest first),
    // each thumbnail loaded from data/ui/captures/cap_<N>.tga written at shutter time.
    void PopulateGallery(Rml::ElementDocument* document);
    void PopulateSettingsForm(Rml::ElementDocument* document);
    void ApplySettingFromElement(Rml::Element* element);
    void BindSettingsListeners(Rml::ElementDocument* document);

    // world_creation.rml: seed every.worldgen-param control from the given preset (via the
    // WorldParamGetter), updating slider values + toggle states + value labels.
    void SeedWorldGenParams(Rml::ElementDocument* document, const std::string& worldType);

    // world_creation.rml: enumerate saved user presets and (re)build a selectable chip for each
    // in #user_presets_row, wired like the curated preset chips.
    void PopulateUserPresets(Rml::ElementDocument* document);

    // world_creation.rml: commit the current customize config as a named user preset via the
    // saver, then refresh the chip row + notification. Factored so the direct save path and the
    // overwrite-confirm path share one implementation.
    void CommitSavePreset(Rml::ElementDocument* document, const std::string& name);

    // Interfaces are now members, their lifetime is tied to the manager.
    RmlSystem m_systemInterface;
    RmlFileInterface m_fileInterface;
    // RmlUi's reference GL3 backend (replaces the hand-rolled RmlRenderer): implements the
    // layered render API (PushLayer/CompositeLayers/CompileFilter) so frosted glass, soft
    // shadows, and the live blurred backdrop actually render. Built in this ctor, so GL/glad
    // must already be initialised when the manager is constructed (main_client: glad at the
    // GLFW context, manager constructed right after).
    RenderInterface_GL3 m_renderInterface;

    Rml::Context* m_context = nullptr;
    GLFWwindow* m_window = nullptr;
    IAudioManager* m_audioManager = nullptr;

    WorldCreationCallback m_worldCreationCallback;
    WorldParamGetter m_worldParamGetter;
    WorldPresetSaver m_worldPresetSaver;
    WorldPresetList m_worldPresetList;
    WorldPresetExists m_worldPresetExists;
    WorldPresetDeleter m_worldPresetDeleter;
    WorldPresetRenamer m_worldPresetRenamer;
    LoadWorldCallback m_loadWorldCallback;
    SettingsBridge m_settingsBridge;
    PauseActionCallback m_pauseActionCallback;

    std::string m_documentToLoad;
    std::string m_activeDocument;
    std::string m_selectedWorldId;

    // The asset root the file interface resolves against (kept here too so the
    // gallery enumerates the SAME root its <img src> paths load through — the
    // old CWD-relative enumeration only worked when CWD == root).: an
    // explicit capture source overrides the live shutter path.
    std::string m_assetRoot;
    std::filesystem::path m_galleryCaptureDir;    // empty = <root>/data/ui/captures
    std::string m_galleryImgPrefix = "captures/"; // document-relative img prefix

    //  cache the last pushed context size so SetDimensions only fires on an actual resize
    // (a per-frame SetDimensions can needlessly dirty layout). -1 forces the first push.
    int m_lastWidth = -1;
    int m_lastHeight = -1;
    //  last UI-pass CPU submit time (ms).
    double m_lastUiFrameMs = 0.0;

    // Static pointer to the active instance for callbacks
    static Rml_UIManager* s_active_manager;
};

} // namespace Luminumbra::Client
