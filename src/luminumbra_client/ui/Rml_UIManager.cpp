#include "ui/Rml_UIManager.h"
#include "audio/IAudioManager.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Debugger.h>
#include <utility>
#include <functional>
#include <cstdio>
#include <string>
#include <chrono>
#include <GLFW/glfw3.h>

namespace Luminumbra::Client {

// --- Helper Functions to map GLFW input to RmlUi ---
// ADDED: These functions were missing, causing 'not declared in this scope' errors.
static Rml::Input::KeyIdentifier GlfwToRmlKey(int glfw_key) {
    // clang-format off
    switch (glfw_key) {
        case GLFW_KEY_UNKNOWN: return Rml::Input::KI_UNKNOWN;
        case GLFW_KEY_SPACE: return Rml::Input::KI_SPACE;
        case GLFW_KEY_APOSTROPHE: return Rml::Input::KI_OEM_7;
        case GLFW_KEY_COMMA: return Rml::Input::KI_OEM_COMMA;
        case GLFW_KEY_MINUS: return Rml::Input::KI_OEM_MINUS;
        case GLFW_KEY_PERIOD: return Rml::Input::KI_OEM_PERIOD;
        case GLFW_KEY_SLASH: return Rml::Input::KI_OEM_2;
        case GLFW_KEY_0: return Rml::Input::KI_0;
        case GLFW_KEY_1: return Rml::Input::KI_1;
        case GLFW_KEY_2: return Rml::Input::KI_2;
        case GLFW_KEY_3: return Rml::Input::KI_3;
        case GLFW_KEY_4: return Rml::Input::KI_4;
        case GLFW_KEY_5: return Rml::Input::KI_5;
        case GLFW_KEY_6: return Rml::Input::KI_6;
        case GLFW_KEY_7: return Rml::Input::KI_7;
        case GLFW_KEY_8: return Rml::Input::KI_8;
        case GLFW_KEY_9: return Rml::Input::KI_9;
        case GLFW_KEY_SEMICOLON: return Rml::Input::KI_OEM_1;
        case GLFW_KEY_EQUAL: return Rml::Input::KI_OEM_PLUS;
        case GLFW_KEY_A: return Rml::Input::KI_A;
        case GLFW_KEY_B: return Rml::Input::KI_B;
        case GLFW_KEY_C: return Rml::Input::KI_C;
        case GLFW_KEY_D: return Rml::Input::KI_D;
        case GLFW_KEY_E: return Rml::Input::KI_E;
        case GLFW_KEY_F: return Rml::Input::KI_F;
        case GLFW_KEY_G: return Rml::Input::KI_G;
        case GLFW_KEY_H: return Rml::Input::KI_H;
        case GLFW_KEY_I: return Rml::Input::KI_I;
        case GLFW_KEY_J: return Rml::Input::KI_J;
        case GLFW_KEY_K: return Rml::Input::KI_K;
        case GLFW_KEY_L: return Rml::Input::KI_L;
        case GLFW_KEY_M: return Rml::Input::KI_M;
        case GLFW_KEY_N: return Rml::Input::KI_N;
        case GLFW_KEY_O: return Rml::Input::KI_O;
        case GLFW_KEY_P: return Rml::Input::KI_P;
        case GLFW_KEY_Q: return Rml::Input::KI_Q;
        case GLFW_KEY_R: return Rml::Input::KI_R;
        case GLFW_KEY_S: return Rml::Input::KI_S;
        case GLFW_KEY_T: return Rml::Input::KI_T;
        case GLFW_KEY_U: return Rml::Input::KI_U;
        case GLFW_KEY_V: return Rml::Input::KI_V;
        case GLFW_KEY_W: return Rml::Input::KI_W;
        case GLFW_KEY_X: return Rml::Input::KI_X;
        case GLFW_KEY_Y: return Rml::Input::KI_Y;
        case GLFW_KEY_Z: return Rml::Input::KI_Z;
        case GLFW_KEY_LEFT_BRACKET: return Rml::Input::KI_OEM_4;
        case GLFW_KEY_BACKSLASH: return Rml::Input::KI_OEM_5;
        case GLFW_KEY_RIGHT_BRACKET: return Rml::Input::KI_OEM_6;
        case GLFW_KEY_GRAVE_ACCENT: return Rml::Input::KI_OEM_3;
        case GLFW_KEY_ESCAPE: return Rml::Input::KI_ESCAPE;
        case GLFW_KEY_ENTER: return Rml::Input::KI_RETURN;
        case GLFW_KEY_TAB: return Rml::Input::KI_TAB;
        case GLFW_KEY_BACKSPACE: return Rml::Input::KI_BACK;
        case GLFW_KEY_INSERT: return Rml::Input::KI_INSERT;
        case GLFW_KEY_DELETE: return Rml::Input::KI_DELETE;
        case GLFW_KEY_RIGHT: return Rml::Input::KI_RIGHT;
        case GLFW_KEY_LEFT: return Rml::Input::KI_LEFT;
        case GLFW_KEY_DOWN: return Rml::Input::KI_DOWN;
        case GLFW_KEY_UP: return Rml::Input::KI_UP;
        case GLFW_KEY_PAGE_UP: return Rml::Input::KI_PRIOR;
        case GLFW_KEY_PAGE_DOWN: return Rml::Input::KI_NEXT;
        case GLFW_KEY_HOME: return Rml::Input::KI_HOME;
        case GLFW_KEY_END: return Rml::Input::KI_END;
        case GLFW_KEY_CAPS_LOCK: return Rml::Input::KI_CAPITAL;
        case GLFW_KEY_SCROLL_LOCK: return Rml::Input::KI_SCROLL;
        case GLFW_KEY_NUM_LOCK: return Rml::Input::KI_NUMLOCK;
        case GLFW_KEY_PRINT_SCREEN: return Rml::Input::KI_SNAPSHOT;
        case GLFW_KEY_PAUSE: return Rml::Input::KI_PAUSE;
        case GLFW_KEY_F1: return Rml::Input::KI_F1;
        case GLFW_KEY_F2: return Rml::Input::KI_F2;
        case GLFW_KEY_F3: return Rml::Input::KI_F3;
        case GLFW_KEY_F4: return Rml::Input::KI_F4;
        case GLFW_KEY_F5: return Rml::Input::KI_F5;
        case GLFW_KEY_F6: return Rml::Input::KI_F6;
        case GLFW_KEY_F7: return Rml::Input::KI_F7;
        case GLFW_KEY_F8: return Rml::Input::KI_F8;
        case GLFW_KEY_F9: return Rml::Input::KI_F9;
        case GLFW_KEY_F10: return Rml::Input::KI_F10;
        case GLFW_KEY_F11: return Rml::Input::KI_F11;
        case GLFW_KEY_F12: return Rml::Input::KI_F12;
        case GLFW_KEY_KP_0: return Rml::Input::KI_NUMPAD0;
        case GLFW_KEY_KP_1: return Rml::Input::KI_NUMPAD1;
        case GLFW_KEY_KP_2: return Rml::Input::KI_NUMPAD2;
        case GLFW_KEY_KP_3: return Rml::Input::KI_NUMPAD3;
        case GLFW_KEY_KP_4: return Rml::Input::KI_NUMPAD4;
        case GLFW_KEY_KP_5: return Rml::Input::KI_NUMPAD5;
        case GLFW_KEY_KP_6: return Rml::Input::KI_NUMPAD6;
        case GLFW_KEY_KP_7: return Rml::Input::KI_NUMPAD7;
        case GLFW_KEY_KP_8: return Rml::Input::KI_NUMPAD8;
        case GLFW_KEY_KP_9: return Rml::Input::KI_NUMPAD9;
        case GLFW_KEY_KP_DECIMAL: return Rml::Input::KI_DECIMAL;
        case GLFW_KEY_KP_DIVIDE: return Rml::Input::KI_DIVIDE;
        case GLFW_KEY_KP_MULTIPLY: return Rml::Input::KI_MULTIPLY;
        case GLFW_KEY_KP_SUBTRACT: return Rml::Input::KI_SUBTRACT;
        case GLFW_KEY_KP_ADD: return Rml::Input::KI_ADD;
        case GLFW_KEY_KP_ENTER: return Rml::Input::KI_NUMPADENTER;
        case GLFW_KEY_LEFT_SHIFT: return Rml::Input::KI_LSHIFT;
        case GLFW_KEY_LEFT_CONTROL: return Rml::Input::KI_LCONTROL;
        case GLFW_KEY_LEFT_ALT: return Rml::Input::KI_LMENU;
        case GLFW_KEY_RIGHT_SHIFT: return Rml::Input::KI_RSHIFT;
        case GLFW_KEY_RIGHT_CONTROL: return Rml::Input::KI_RCONTROL;
        case GLFW_KEY_RIGHT_ALT: return Rml::Input::KI_RMENU;
        default: break;
    }
    // clang-format on
    return Rml::Input::KI_UNKNOWN;
}

static int GlfwToRmlMods(int glfw_mods) {
    int rml_mods = 0;
    if (glfw_mods & GLFW_MOD_SHIFT) rml_mods |= Rml::Input::KM_SHIFT;
    if (glfw_mods & GLFW_MOD_CONTROL) rml_mods |= Rml::Input::KM_CTRL;
    if (glfw_mods & GLFW_MOD_ALT) rml_mods |= Rml::Input::KM_ALT;
    if (glfw_mods & GLFW_MOD_CAPS_LOCK) rml_mods |= Rml::Input::KM_CAPSLOCK;
    if (glfw_mods & GLFW_MOD_NUM_LOCK) rml_mods |= Rml::Input::KM_NUMLOCK;
    return rml_mods;
}

// --- Lambda Event Listener Wrapper ---
class LambdaEventListener : public Rml::EventListener {
public:
    using Callback = std::function<void(Rml::Event&)>;
    explicit LambdaEventListener(Callback callback) : m_callback(std::move(callback)) {}
    void ProcessEvent(Rml::Event& event) override {
        if (m_callback) m_callback(event);
    }
private:
    Callback m_callback;
};

static std::string ReadFormControlValue(Rml::Element* element, const std::string& fallback) {
    if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) {
        return control->GetValue();
    }
    return element ? element->GetAttribute<Rml::String>("value", fallback) : fallback;
}

// --- Static Instance for Callbacks ---
Rml_UIManager* Rml_UIManager::s_active_manager = nullptr;

// --- Constructor / Destructor ---
Rml_UIManager::Rml_UIManager(const std::string& asset_root_path) 
    : m_fileInterface(asset_root_path) {
    s_active_manager = this;
}

Rml_UIManager::~Rml_UIManager() {
    s_active_manager = nullptr;
}

// --- Core Functions ---
void Rml_UIManager::Init(GLFWwindow* window, IAudioManager* audioManager) {
    m_window = window;
    m_audioManager = audioManager;

    Rml::SetSystemInterface(&m_systemInterface);
    Rml::SetFileInterface(&m_fileInterface);
    Rml::SetRenderInterface(&m_renderInterface);

    if (!Rml::Initialise()) {
        LUMINUMBRA_CORE_ERROR("Failed to initialize RmlUi!");
        return;
    }
    
    // It's better to load fonts relative to the assets path specified in the file interface
    Rml::LoadFontFace("data/fonts/Lora/Lora-VariableFont_wght.ttf");
    Rml::LoadFontFace("data/fonts/Lora/Lora-Italic-VariableFont_wght.ttf");

    int width, height;
    glfwGetWindowSize(m_window, &width, &height);
    m_context = Rml::CreateContext("main", Rml::Vector2i(width, height));
    
    if (!m_context) {
        LUMINUMBRA_CORE_ERROR("Failed to create RmlUi context!");
        Rml::Shutdown();
        return;
    }

    Rml::Debugger::Initialise(m_context);
    
    glfwSetKeyCallback(m_window, Rml_UIManager::KeyCallback);
    glfwSetCharCallback(m_window, Rml_UIManager::CharCallback);
    glfwSetMouseButtonCallback(m_window, Rml_UIManager::MouseButtonCallback);
    glfwSetCursorPosCallback(m_window, Rml_UIManager::CursorPosCallback);
    glfwSetScrollCallback(m_window, Rml_UIManager::ScrollCallback);

    LUMINUMBRA_CORE_INFO("UI Manager Initialized.");
}

void Rml_UIManager::Shutdown() {
    if (m_context) {
        Rml::RemoveContext("main");
    }
    Rml::Shutdown();
    LUMINUMBRA_CORE_INFO("UI Manager Shutdown.");
}

void Rml_UIManager::Update() {
    if (m_context) {
        ProcessDocumentLoadRequest(); // Process async loads
        // T006: only push dimensions on an actual size change — a per-frame SetDimensions can
        // needlessly dirty layout even when the size is unchanged.
        int width, height;
        glfwGetWindowSize(m_window, &width, &height);
        if (width != m_lastWidth || height != m_lastHeight) {
            m_context->SetDimensions(Rml::Vector2i(width, height));
            m_lastWidth = width;
            m_lastHeight = height;
        }
        m_context->Update();
    }
}

void Rml_UIManager::Render() {
    if (m_context) {
        int width, height;
        glfwGetFramebufferSize(m_window, &width, &height);
        m_renderInterface.SetViewport(width, height);
        
        glEnable(GL_BLEND);
        // RmlUi 6.1 emits PREMULTIPLIED-alpha vertex colours + font glyphs (vendor/CMakeLists.txt:88
        // pins GIT_TAG 6.1), and the shader does texture*vertexColour — so the correct blend is
        // (GL_ONE, GL_ONE_MINUS_SRC_ALPHA), matching RmlUi's reference GL3 backend. Straight-alpha
        // (GL_SRC_ALPHA) double-darkens any semi-transparent fill/text.
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);

        // T007: time the UI draw submission (CPU side; the UI is off the deterministic sim path, so
        // wall-clock here is fine). Meaningful while the renderer is unbatched (draw-call-bound).
        const auto ui_t0 = std::chrono::high_resolution_clock::now();
        m_context->Render();
        const auto ui_t1 = std::chrono::high_resolution_clock::now();
        m_lastUiFrameMs = std::chrono::duration<double, std::milli>(ui_t1 - ui_t0).count();

        glEnable(GL_DEPTH_TEST);
    }
}

void Rml_UIManager::RequestLoadDocument(std::string path) {
    m_documentToLoad = std::move(path);
}

// REMOVED: GetContext() is now in the header.

// --- Private Implementation ---

void Rml_UIManager::ProcessDocumentLoadRequest() {
    if (!m_documentToLoad.empty()) {
        LoadDocument(m_documentToLoad);
        m_documentToLoad.clear();
    }
}

void Rml_UIManager::LoadDocument(const std::string& rml_path) {
    if (!m_context || rml_path == m_activeDocument) return;

    // Close existing documents (except the debugger)
    for (int i = m_context->GetNumDocuments() - 1; i >= 0; --i) {
        Rml::ElementDocument* doc = m_context->GetDocument(i);
        if (doc && doc->GetId().find("rmlui-debug") == Rml::String::npos) {
            doc->Close();
        }
    }
    
    Rml::ElementDocument* document = m_context->LoadDocument("data/ui/" + rml_path);
    if (!document) {
        LUMINUMBRA_CORE_ERROR("Failed to load RML document: {}", rml_path);
        return;
    }

    m_activeDocument = rml_path;
    m_selectedWorldId.clear();
    BindEventListeners(document);
    document->Show();
}

void Rml_UIManager::BindEventListeners(Rml::ElementDocument* document) {
    auto AddClickSoundListener = [this](Rml::Element* element, LambdaEventListener::Callback callback) {
        if (element) {
            element->AddEventListener("click", new LambdaEventListener([this, cb = std::move(callback)](Rml::Event& event){
                if (m_audioManager) m_audioManager->PlayOneShot2D("ui_button_click");
                if (cb) cb(event);
            }));
            element->AddEventListener("mouseover", new LambdaEventListener([this](Rml::Event&){
                 if (m_audioManager) m_audioManager->PlayOneShot2D("ui_button_hover");
            }));
        }
    };
    // The rest of your BindEventListeners implementation is fine...
    if (auto* e = document->GetElementById("new_world_btn")) AddClickSoundListener(e, [this](Rml::Event&){ this->RequestLoadDocument("world_creation.rml"); });
    if (auto* e = document->GetElementById("load_world_btn")) AddClickSoundListener(e, [this](Rml::Event&){ this->RequestLoadDocument("world_selection.rml"); });
    if (auto* e = document->GetElementById("settings_btn")) AddClickSoundListener(e, [this](Rml::Event&){ this->RequestLoadDocument("settings.rml"); });
    if (auto* e = document->GetElementById("quit_btn")) AddClickSoundListener(e, [this](Rml::Event&){ glfwSetWindowShouldClose(this->m_window, true); });
    if (auto* e = document->GetElementById("back_btn")) AddClickSoundListener(e, [this](Rml::Event&){ this->RequestLoadDocument("main_menu.rml"); });

    // settings.rml: populate widgets from current settings, then wire live change + Apply.
    BindSettingsListeners(document);

    if (auto* load_button = document->GetElementById("load_selected_btn")) {
        AddClickSoundListener(load_button, [this](Rml::Event&){
            if (m_loadWorldCallback && !m_selectedWorldId.empty()) {
                m_loadWorldCallback(m_selectedWorldId);
            }
        });
    }

    Rml::ElementList world_items;
    document->GetElementsByClassName(world_items, "list-item");
    for (Rml::Element* item : world_items) {
        if (!item) {
            continue;
        }
        AddClickSoundListener(item, [this, document](Rml::Event& event){
            Rml::Element* selected = event.GetTargetElement();
            while (selected && selected->GetAttribute<Rml::String>("data-world-id", "").empty()) {
                selected = selected->GetParentNode();
            }
            if (!selected) {
                return;
            }

            Rml::ElementList all_items;
            document->GetElementsByClassName(all_items, "list-item");
            for (Rml::Element* item_to_clear : all_items) {
                if (item_to_clear) {
                    item_to_clear->RemoveAttribute("data-selected");
                    item_to_clear->SetClass("selected", false);
                }
            }

            m_selectedWorldId = selected->GetAttribute<Rml::String>("data-world-id", "");
            selected->SetAttribute("data-selected", "true");
            selected->SetClass("selected", true);
            if (auto* load_button = document->GetElementById("load_selected_btn")) {
                load_button->RemoveAttribute("disabled");
            }
        });
    }
    
    if (auto* e = document->GetElementById("create_btn")) {
        AddClickSoundListener(e, [this](Rml::Event& event){
            if (m_worldCreationCallback) {
                auto* doc = event.GetTargetElement()->GetOwnerDocument();
                Rml::Element* name_input = doc->GetElementById("world_name");
                Rml::Element* seed_input = doc->GetElementById("world_seed");
                Rml::Element* type_select = doc->GetElementById("world_type");
                std::string name = ReadFormControlValue(name_input, "New World");
                std::string seed = ReadFormControlValue(seed_input, "");
                std::string type = ReadFormControlValue(type_select, "default");
                m_worldCreationCallback(name, seed, type);
            }
        });
    }
}

// --- settings.rml support ---
namespace {

// Set a form control's value (works for <select>, <input type=range>, etc.).
void SetControlValue(Rml::Element* element, const std::string& value) {
    if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) {
        control->SetValue(value);
    } else if (element) {
        element->SetAttribute("value", value);
    }
}

// Update the little "value" label next to a slider, if present.
void SetValueLabel(Rml::ElementDocument* doc, const std::string& label_id, const std::string& text) {
    if (auto* label = doc->GetElementById(label_id)) {
        label->SetInnerRML(text);
    }
}

std::string FormatFloat(float v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
    return buf;
}

std::string FormatPercent(float v01) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(v01 * 100.0f + 0.5f));
    return buf;
}

}  // namespace

void Rml_UIManager::PopulateSettingsForm(Rml::ElementDocument* document) {
    if (!document) return;
    const SettingsBridge& b = m_settingsBridge;

    // Video
    if (b.GetResolution) {
        SetControlValue(document->GetElementById("setting_resolution"), b.GetResolution());
    }
    if (b.GetWindowMode) {
        SetControlValue(document->GetElementById("setting_window_mode"), b.GetWindowMode());
    }
    if (b.GetVSync) {
        SetControlValue(document->GetElementById("setting_vsync"), b.GetVSync() ? "on" : "off");
    }
    if (b.GetFov) {
        const float fov = b.GetFov();
        SetControlValue(document->GetElementById("setting_fov"), FormatFloat(fov, 0));
        SetValueLabel(document, "setting_fov_value", FormatFloat(fov, 0));
    }
    if (b.GetMouseSensitivity) {
        const float s = b.GetMouseSensitivity();
        SetControlValue(document->GetElementById("setting_mouse_sensitivity"), FormatFloat(s, 3));
        SetValueLabel(document, "setting_mouse_sensitivity_value", FormatFloat(s, 3));
    }

    // Audio
    if (b.GetAudioMaster) {
        const float v = b.GetAudioMaster();
        SetControlValue(document->GetElementById("setting_audio_master"), FormatFloat(v, 2));
        SetValueLabel(document, "setting_audio_master_value", FormatPercent(v));
    }
    if (b.GetAudioSfx) {
        const float v = b.GetAudioSfx();
        SetControlValue(document->GetElementById("setting_audio_sfx"), FormatFloat(v, 2));
        SetValueLabel(document, "setting_audio_sfx_value", FormatPercent(v));
    }
    if (b.GetAudioMusic) {
        const float v = b.GetAudioMusic();
        SetControlValue(document->GetElementById("setting_audio_music"), FormatFloat(v, 2));
        SetValueLabel(document, "setting_audio_music_value", FormatPercent(v));
    }
}

void Rml_UIManager::ApplySettingFromElement(Rml::Element* element) {
    if (!element) return;
    const std::string id = element->GetId();
    const std::string value = ReadFormControlValue(element, "");
    SettingsBridge& b = m_settingsBridge;
    Rml::ElementDocument* doc = element->GetOwnerDocument();

    auto as_float = [&value](float fallback) {
        try { return std::stof(value); } catch (...) { return fallback; }
    };

    if (id == "setting_resolution") {
        if (b.SetResolution) b.SetResolution(value);
    } else if (id == "setting_window_mode") {
        if (b.SetWindowMode) b.SetWindowMode(value);
    } else if (id == "setting_vsync") {
        if (b.SetVSync) b.SetVSync(value == "on" || value == "1" || value == "true");
    } else if (id == "setting_fov") {
        const float f = as_float(45.0f);
        if (b.SetFov) b.SetFov(f);
        if (doc) SetValueLabel(doc, "setting_fov_value", FormatFloat(f, 0));
    } else if (id == "setting_mouse_sensitivity") {
        const float f = as_float(0.025f);
        if (b.SetMouseSensitivity) b.SetMouseSensitivity(f);
        if (doc) SetValueLabel(doc, "setting_mouse_sensitivity_value", FormatFloat(f, 3));
    } else if (id == "setting_audio_master") {
        const float f = as_float(1.0f);
        if (b.SetAudioMaster) b.SetAudioMaster(f);
        if (doc) SetValueLabel(doc, "setting_audio_master_value", FormatPercent(f));
    } else if (id == "setting_audio_sfx") {
        const float f = as_float(1.0f);
        if (b.SetAudioSfx) b.SetAudioSfx(f);
        if (doc) SetValueLabel(doc, "setting_audio_sfx_value", FormatPercent(f));
    } else if (id == "setting_audio_music") {
        const float f = as_float(1.0f);
        if (b.SetAudioMusic) b.SetAudioMusic(f);
        if (doc) SetValueLabel(doc, "setting_audio_music_value", FormatPercent(f));
    }
}

void Rml_UIManager::BindSettingsListeners(Rml::ElementDocument* document) {
    if (!document) return;
    // Only wire the settings screen.
    if (!document->GetElementById("setting_resolution") &&
        !document->GetElementById("apply_settings_btn")) {
        return;
    }

    // Seed widgets from the current settings.
    PopulateSettingsForm(document);

    // Live-apply every control on "change" (sliders, selects).
    static const char* kControlIds[] = {
        "setting_resolution", "setting_window_mode", "setting_vsync",
        "setting_fov", "setting_mouse_sensitivity",
        "setting_audio_master", "setting_audio_sfx", "setting_audio_music",
    };
    for (const char* control_id : kControlIds) {
        if (auto* el = document->GetElementById(control_id)) {
            el->AddEventListener("change", new LambdaEventListener([this](Rml::Event& event) {
                this->ApplySettingFromElement(event.GetTargetElement());
            }));
        }
    }

    // Apply & Save button: flush every control then persist the overlay.
    if (auto* apply = document->GetElementById("apply_settings_btn")) {
        apply->AddEventListener("click", new LambdaEventListener([this, document](Rml::Event&) {
            if (m_audioManager) m_audioManager->PlayOneShot2D("ui_button_click");
            for (const char* control_id : kControlIds) {
                this->ApplySettingFromElement(document->GetElementById(control_id));
            }
            bool ok = true;
            if (m_settingsBridge.Save) ok = m_settingsBridge.Save();
            if (auto* note = document->GetElementById("notification")) {
                note->SetClass("hidden", false);
                if (auto* txt = document->GetElementById("notification_text")) {
                    txt->SetInnerRML(ok ? "Settings saved" : "Save failed");
                }
            }
        }));
        apply->AddEventListener("mouseover", new LambdaEventListener([this](Rml::Event&) {
            if (m_audioManager) m_audioManager->PlayOneShot2D("ui_button_hover");
        }));
    }
}

// --- Static GLFW Callbacks ---
void Rml_UIManager::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (!s_active_manager || !s_active_manager->m_context) return;

    if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
        Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
        return;
    }

    Rml::Input::KeyIdentifier rml_key = GlfwToRmlKey(key);
    int rml_mods = GlfwToRmlMods(mods);

    if (rml_key != Rml::Input::KI_UNKNOWN) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            s_active_manager->m_context->ProcessKeyDown(rml_key, rml_mods);
        } else if (action == GLFW_RELEASE) {
            s_active_manager->m_context->ProcessKeyUp(rml_key, rml_mods);
        }
    }
}
void Rml_UIManager::CharCallback(GLFWwindow*, unsigned int codepoint) {
    if (s_active_manager && s_active_manager->m_context) {
        s_active_manager->m_context->ProcessTextInput(codepoint);
    }
}
void Rml_UIManager::MouseButtonCallback(GLFWwindow*, int button, int action, int mods) {
    if (s_active_manager && s_active_manager->m_context) {
        int rml_mods = GlfwToRmlMods(mods);
        if (action == GLFW_PRESS) s_active_manager->m_context->ProcessMouseButtonDown(button, rml_mods);
        else s_active_manager->m_context->ProcessMouseButtonUp(button, rml_mods);
    }
}
void Rml_UIManager::CursorPosCallback(GLFWwindow*, double xpos, double ypos) {
    if (s_active_manager && s_active_manager->m_context) {
        s_active_manager->m_context->ProcessMouseMove((int)xpos, (int)ypos, GlfwToRmlMods(0));
    }
}
void Rml_UIManager::ScrollCallback(GLFWwindow*, double xoffset, double yoffset) {
    if (s_active_manager && s_active_manager->m_context) {
        s_active_manager->m_context->ProcessMouseWheel((float)-yoffset, GlfwToRmlMods(0));
    }
}

} // namespace Luminumbra::Client
