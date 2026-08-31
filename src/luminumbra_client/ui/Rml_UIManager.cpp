#include "ui/Rml_UIManager.h"
#include "audio/IAudioManager.h"
#include <GLFW/glfw3.h>
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Debugger.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

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
    if (glfw_mods & GLFW_MOD_SHIFT)
        rml_mods |= Rml::Input::KM_SHIFT;
    if (glfw_mods & GLFW_MOD_CONTROL)
        rml_mods |= Rml::Input::KM_CTRL;
    if (glfw_mods & GLFW_MOD_ALT)
        rml_mods |= Rml::Input::KM_ALT;
    if (glfw_mods & GLFW_MOD_CAPS_LOCK)
        rml_mods |= Rml::Input::KM_CAPSLOCK;
    if (glfw_mods & GLFW_MOD_NUM_LOCK)
        rml_mods |= Rml::Input::KM_NUMLOCK;
    return rml_mods;
}

// --- Lambda Event Listener Wrapper ---
class LambdaEventListener : public Rml::EventListener {
public:
    using Callback = std::function<void(Rml::Event&)>;
    explicit LambdaEventListener(Callback callback)
        : m_callback(std::move(callback)) {}
    void ProcessEvent(Rml::Event& event) override {
        if (m_callback)
            m_callback(event);
    }
    void OnDetach(Rml::Element*) override {
        delete this;
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

// Gather every.worldgen-param control in the create-world customize form into the override
// list the host merges onto the base preset. Bool params read their toggle.on class; numeric
// params read their form-control value.
static std::vector<WorldGenParam> CollectWorldGenParams(Rml::ElementDocument* document) {
    std::vector<WorldGenParam> out;
    if (!document)
        return out;
    //  the SEMANTIC KNOBS travel first, as WorldGenParam entries
    // with path "knob.<id>" + type "knob" (value in [0,1]). The host splits these
    // off and feeds them to the engine-side KnobLayer; the raw.worldgen-param
    // entries below are the sparse advanced-panel OVERRIDE diff overlaid on top.
    Rml::ElementList knobs;
    document->GetElementsByClassName(knobs, "worldgen-knob");
    for (Rml::Element* el : knobs) {
        if (!el)
            continue;
        const std::string id = el->GetAttribute<Rml::String>("data-knob", "");
        if (id.empty())
            continue;
        WorldGenParam p;
        p.path = "knob." + id;
        p.type = "knob";
        p.value = ReadFormControlValue(el, "0.5");
        out.push_back(std::move(p));
    }
    Rml::ElementList controls;
    document->GetElementsByClassName(controls, "worldgen-param");
    for (Rml::Element* el : controls) {
        if (!el)
            continue;
        WorldGenParam p;
        p.path = el->GetAttribute<Rml::String>("data-path", "");
        p.type = el->GetAttribute<Rml::String>("data-type", "float");
        if (p.path.empty())
            continue;
        if (p.type == "bool") {
            p.value = el->IsClassSet("on") ? "true" : "false";
        } else {
            p.value = ReadFormControlValue(el, "");
            if (p.value.empty())
                continue;
        }
        out.push_back(std::move(p));
    }
    return out;
}

// --- Static Instance for Callbacks ---
Rml_UIManager* Rml_UIManager::s_active_manager = nullptr;

Rml_UIManager::PreviewState Rml_UIManager::GetWorldCreationPreviewState() const {
    PreviewState st;
    if (!m_context)
        return st;
    // Find the loaded, VISIBLE create-world document. The live diorama renders
    // FULL-SCREEN behind the form (cinematic backdrop), so the form itself — not a
    // bounded preview box — is the active marker.
    Rml::ElementDocument* doc = nullptr;
    for (int i = 0; i < m_context->GetNumDocuments(); ++i) {
        Rml::ElementDocument* d = m_context->GetDocument(i);
        if (!d || !d->IsVisible())
            continue;
        if (d->GetElementById("world_creation_form")) {
            doc = d;
            break;
        }
    }
    if (!doc)
        return st;

    st.active = true;
    st.params = CollectWorldGenParams(doc);
    st.worldType = ReadFormControlValue(doc->GetElementById("world_type"), "default");

    // Selected weather pill (defaults to clear).
    Rml::ElementList pills;
    doc->GetElementsByClassName(pills, "preview-weather");
    for (Rml::Element* p : pills) {
        if (p && p->IsClassSet("selected")) {
            st.weather = p->GetAttribute<Rml::String>("data-weather", "clear");
            break;
        }
    }
    // Time-of-day slider.
    if (Rml::Element* tod = doc->GetElementById("preview_tod")) {
        const std::string v = ReadFormControlValue(tod, "0.24");
        try {
            st.tod = std::stof(v);
        } catch (...) {
            st.tod = 0.24f;
        }
    }

    // The diorama is full-screen, so there is no bounded pane rect to report; the
    // host orbits when the cursor is over the world backdrop (outside the form
    // panel), not over a fixed box. Leave pane_* at 0.
    return st;
}

bool Rml_UIManager::ConsumeWorldCreationResetView() {
    if (!m_context)
        return false;
    for (int i = 0; i < m_context->GetNumDocuments(); ++i) {
        Rml::ElementDocument* d = m_context->GetDocument(i);
        if (!d || !d->IsVisible())
            continue;
        if (Rml::Element* btn = d->GetElementById("preview_reset_btn")) {
            if (btn->IsClassSet("reset-pending")) {
                btn->SetClass("reset-pending", false);
                return true;
            }
        }
    }
    return false;
}

// --- Constructor / Destructor ---
Rml_UIManager::Rml_UIManager(const std::string& asset_root_path)
    : m_fileInterface(asset_root_path)
    , m_assetRoot(asset_root_path) {
    s_active_manager = this;
}

Rml_UIManager::~Rml_UIManager() {
    s_active_manager = nullptr;
}

// --- Core Functions ---
void Rml_UIManager::Init(GLFWwindow* window, IAudioManager* audioManager) {
    m_window = window;
    m_audioManager = audioManager;

    if (!static_cast<bool>(m_renderInterface)) {
        LUMINUMBRA_CORE_ERROR("RmlUi GL3 render interface failed to construct (GL not ready?).");
        return;
    }

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
        //  only push dimensions on an actual size change — a per-frame SetDimensions can
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
    if (!m_context)
        return;

    int width, height;
    glfwGetFramebufferSize(m_window, &width, &height);
    // BeginFrame asserts a >=1 viewport; skip minimised frames.
    if (width < 1 || height < 1)
        return;

    m_renderInterface.SetViewport(width, height);

    // The GL3 backend's BeginFrame/EndFrame fully manage GL state (blend, depth, stencil,
    // scissor, viewport, FBO bindings) and restore the caller's state on EndFrame — so the
    // world and ImGui passes around this call are unaffected. It composites the UI (with its
    // layer/filter stack) into its own MSAA framebuffer and blits the result onto the default
    // backbuffer with premultiplied-alpha blend, so the world shows through transparent UI.
    //  time the UI draw submission (CPU side; the UI is off the deterministic sim path).
    const auto ui_t0 = std::chrono::high_resolution_clock::now();
    m_renderInterface.BeginFrame();
    m_context->Render();
    m_renderInterface.EndFrame();
    const auto ui_t1 = std::chrono::high_resolution_clock::now();
    m_lastUiFrameMs = std::chrono::duration<double, std::milli>(ui_t1 - ui_t0).count();
}

void Rml_UIManager::RequestLoadDocument(std::string path) {
    m_documentToLoad = std::move(path);
}

void Rml_UIManager::ReloadActiveDocument() {
    if (!m_context || m_activeDocument.empty())
        return;
    // Drop cached stylesheets/templates so edited.rcss/.rml is re-read from disk.
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    // LoadDocument early-returns when the path equals the active document; clear it to force
    // a genuine reload of the same screen.
    const std::string doc = m_activeDocument;
    m_activeDocument.clear();
    LoadDocument(doc);
    LUMINUMBRA_CORE_INFO("UI hot-reloaded: {}", doc);
}

// REMOVED: GetContext is now in the header.

// --- Private Implementation ---

void Rml_UIManager::ProcessDocumentLoadRequest() {
    if (!m_documentToLoad.empty()) {
        LoadDocument(m_documentToLoad);
        m_documentToLoad.clear();
    }
}

void Rml_UIManager::LoadDocument(const std::string& rml_path) {
    if (!m_context || rml_path == m_activeDocument)
        return;

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
    if (document->GetId() == "gallery")
        PopulateGallery(document);
    document->Show();
}

void Rml_UIManager::BindEventListeners(Rml::ElementDocument* document) {
    auto AddClickSoundListener = [this](Rml::Element* element,
                                        LambdaEventListener::Callback callback) {
        if (element) {
            element->AddEventListener(
                "click",
                new LambdaEventListener([this, cb = std::move(callback)](Rml::Event& event) {
                    if (m_audioManager)
                        m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                    if (cb)
                        cb(event);
                }));
            element->AddEventListener("mouseover", new LambdaEventListener([this](Rml::Event&) {
                                          if (m_audioManager)
                                              m_audioManager->PlayOneShot2D("ui_button_hover",
                                                                            BusId::Ui);
                                      }));
        }
    };
    // The rest of your BindEventListeners implementation is fine...
    if (auto* e = document->GetElementById("new_world_btn"))
        AddClickSoundListener(
            e, [this](Rml::Event&) { this->RequestLoadDocument("world_creation.rml"); });
    if (auto* e = document->GetElementById("load_world_btn"))
        AddClickSoundListener(
            e, [this](Rml::Event&) { this->RequestLoadDocument("world_selection.rml"); });
    if (auto* e = document->GetElementById("settings_btn"))
        AddClickSoundListener(e,
                              [this](Rml::Event&) { this->RequestLoadDocument("settings.rml"); });
    if (auto* e = document->GetElementById("gallery_btn"))
        AddClickSoundListener(e, [this](Rml::Event&) { this->RequestLoadDocument("gallery.rml"); });
    if (auto* e = document->GetElementById("quit_btn"))
        AddClickSoundListener(
            e, [this](Rml::Event&) { glfwSetWindowShouldClose(this->m_window, true); });
    if (auto* e = document->GetElementById("back_btn"))
        AddClickSoundListener(e, [this, document](Rml::Event&) {
            // Leaving settings persists the live-applied changes (sliders apply on change; the
            // overlay is saved here since there's no explicit apply button in the new design).
            if (document->GetId() == "settings" && m_settingsBridge.Save)
                m_settingsBridge.Save();
            this->RequestLoadDocument("main_menu.rml");
        });
    // Pause menu (pause.rml): route resume / quit-to-menu back to main_client (it owns cursor +
    // state).
    if (auto* e = document->GetElementById("resume_btn"))
        AddClickSoundListener(e, [this](Rml::Event&) {
            if (m_pauseActionCallback)
                m_pauseActionCallback("resume");
        });
    if (auto* e = document->GetElementById("quit_menu_btn"))
        AddClickSoundListener(e, [this](Rml::Event&) {
            if (m_pauseActionCallback)
                m_pauseActionCallback("quit");
        });

    // settings.rml: populate widgets from current settings, then wire live change + Apply.
    BindSettingsListeners(document);

    if (auto* load_button = document->GetElementById("load_selected_btn")) {
        AddClickSoundListener(load_button, [this](Rml::Event&) {
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
        AddClickSoundListener(item, [this, document](Rml::Event& event) {
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
        AddClickSoundListener(e, [this](Rml::Event& event) {
            if (m_worldCreationCallback) {
                auto* doc = event.GetTargetElement()->GetOwnerDocument();
                Rml::Element* name_input = doc->GetElementById("world_name");
                Rml::Element* seed_input = doc->GetElementById("world_seed");
                Rml::Element* type_select = doc->GetElementById("world_type");
                std::string name = ReadFormControlValue(name_input, "New World");
                std::string seed = ReadFormControlValue(seed_input, "");
                std::string type = ReadFormControlValue(type_select, "default");
                // Gather the customize-form overrides; the host merges them onto the base preset.
                m_worldCreationCallback(name, seed, type, CollectWorldGenParams(doc));
            }
        });
    }

    // Landscape preset chips (world_creation): clicking a chip selects it, drives the hidden
    // #world_type select that create_btn reads, and re-seeds the customize form from that preset.
    {
        Rml::ElementList chips;
        document->GetElementsByClassName(chips, "preset-chip");
        for (Rml::Element* chip : chips) {
            AddClickSoundListener(chip, [this, document](Rml::Event& event) {
                Rml::Element* c = event.GetTargetElement();
                while (c && c->GetAttribute<Rml::String>("data-preset", "").empty())
                    c = c->GetParentNode();
                if (!c)
                    return;
                Rml::ElementList all;
                document->GetElementsByClassName(all, "preset-chip");
                for (Rml::Element* x : all)
                    x->SetClass("selected", false);
                c->SetClass("selected", true);
                const std::string preset = c->GetAttribute<Rml::String>("data-preset", "default");
                if (auto* sel = document->GetElementById("world_type")) {
                    if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(sel))
                        fc->SetValue(preset);
                }
                this->SeedWorldGenParams(document, preset);
            });
        }
    }

    // Customize section: expand/collapse the advanced worldgen params.
    if (auto* toggle = document->GetElementById("customize_toggle")) {
        toggle->AddEventListener(
            "click", new LambdaEventListener([this, document](Rml::Event&) {
                if (m_audioManager)
                    m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                auto* body = document->GetElementById("customize_body");
                const bool collapsed = body && body->IsClassSet("collapsed");
                if (body)
                    body->SetClass("collapsed", !collapsed);
                if (auto* caret = document->GetElementById("customize_caret")) {
                    caret->SetInnerRML(collapsed ? "&#8211;"
                                                 : "+"); // "–" when open, "+" when closed
                }
            }));
    }

    // advanced-param TAB strip. Clicking a tab chip activates its pane
    // (and only its pane), so the long advanced column is split into terrain /
    // water / biomes / features groups shown one at a time (cuts the scroll).
    {
        Rml::ElementList tabs;
        document->GetElementsByClassName(tabs, "param-tab");
        for (Rml::Element* tab : tabs) {
            AddClickSoundListener(tab, [this, document](Rml::Event& event) {
                Rml::Element* t = event.GetTargetElement();
                while (t && t->GetAttribute<Rml::String>("data-tab", "").empty())
                    t = t->GetParentNode();
                if (!t)
                    return;
                const std::string which = t->GetAttribute<Rml::String>("data-tab", "");
                Rml::ElementList all_tabs;
                document->GetElementsByClassName(all_tabs, "param-tab");
                for (Rml::Element* x : all_tabs)
                    x->SetClass("active", x->GetAttribute<Rml::String>("data-tab", "") == which);
                Rml::ElementList panes;
                document->GetElementsByClassName(panes, "param-pane");
                for (Rml::Element* p : panes)
                    p->SetClass("active", p->GetAttribute<Rml::String>("data-pane", "") == which);
            });
        }
    }

    // Worldgen sliders: live-update the adjacent.param-value label as they move.
    {
        Rml::ElementList sliders;
        document->GetElementsByClassName(sliders, "worldgen-param");
        for (Rml::Element* el : sliders) {
            if (el->GetAttribute<Rml::String>("data-type", "") == "bool")
                continue;
            el->AddEventListener("change", new LambdaEventListener([](Rml::Event& ev) {
                                     Rml::Element* slider = ev.GetTargetElement();
                                     if (!slider || !slider->GetParentNode())
                                         return;
                                     Rml::ElementList vals;
                                     slider->GetParentNode()->GetElementsByClassName(vals,
                                                                                     "param-value");
                                     if (!vals.empty())
                                         vals[0]->SetInnerRML(ReadFormControlValue(slider, ""));
                                 }));
        }
    }

    //  semantic-knob sliders — live-update the adjacent
    //.knob-value label (the host reads the knob positions each frame to drive
    // the engine KnobLayer + the live preview rebuild).
    {
        Rml::ElementList kslid;
        document->GetElementsByClassName(kslid, "worldgen-knob");
        for (Rml::Element* el : kslid) {
            el->AddEventListener("change", new LambdaEventListener([](Rml::Event& ev) {
                                     Rml::Element* slider = ev.GetTargetElement();
                                     if (!slider || !slider->GetParentNode())
                                         return;
                                     Rml::ElementList vals;
                                     slider->GetParentNode()->GetElementsByClassName(vals,
                                                                                     "knob-value");
                                     if (!vals.empty())
                                         vals[0]->SetInnerRML(ReadFormControlValue(slider, "0.5"));
                                 }));
        }
    }

    // Save the current config as a named, reusable user preset. If the name's slug already
    // has a saved preset, WorldPresetSaver silently overwrites — so gate it behind an explicit
    // overwrite-confirm modal instead of clobbering the existing preset without warning.
    if (auto* save_btn = document->GetElementById("save_preset_btn")) {
        AddClickSoundListener(save_btn, [this, document](Rml::Event&) {
            if (!m_worldPresetSaver)
                return;
            const std::string name =
                ReadFormControlValue(document->GetElementById("save_preset_name"), "");
            if (m_worldPresetExists && m_worldPresetExists(name)) {
                // Show the overwrite-confirm modal; the confirm button commits the save.
                if (auto* modal = document->GetElementById("preset_overwrite_modal"))
                    modal->SetClass("hidden", false);
                if (auto* label = document->GetElementById("preset_overwrite_name"))
                    label->SetInnerRML(name);
                return;
            }
            this->CommitSavePreset(document, name);
        });
    }

    // Overwrite-confirm modal: confirm commits the (overwriting) save; cancel just dismisses.
    if (auto* confirm = document->GetElementById("confirm_overwrite_btn")) {
        AddClickSoundListener(confirm, [this, document](Rml::Event&) {
            if (auto* modal = document->GetElementById("preset_overwrite_modal"))
                modal->SetClass("hidden", true);
            const std::string name =
                ReadFormControlValue(document->GetElementById("save_preset_name"), "");
            this->CommitSavePreset(document, name);
        });
    }
    if (auto* cancel = document->GetElementById("cancel_overwrite_btn")) {
        AddClickSoundListener(cancel, [document](Rml::Event&) {
            if (auto* modal = document->GetElementById("preset_overwrite_modal"))
                modal->SetClass("hidden", true);
        });
    }

    // Rename the currently-selected USER preset to the typed name. Curated presets have no
    // user-type id, so the renamer no-ops on them. Confirm/cancel reuse the rename flow inline.
    if (auto* rename_btn = document->GetElementById("rename_preset_btn")) {
        AddClickSoundListener(rename_btn, [this, document](Rml::Event&) {
            if (!m_worldPresetRenamer)
                return;
            const std::string worldType =
                ReadFormControlValue(document->GetElementById("world_type"), "default");
            const std::string newName =
                ReadFormControlValue(document->GetElementById("save_preset_name"), "");
            auto note = [&](const std::string& msg) {
                if (auto* n = document->GetElementById("notification")) {
                    n->SetClass("hidden", false);
                    if (auto* t = document->GetElementById("notification_text"))
                        t->SetInnerRML(msg);
                }
            };
            if (worldType.rfind("user_", 0) != 0) {
                note("Select a saved preset to rename");
                return;
            }
            if (newName.empty()) {
                note("Enter a new name");
                return;
            }
            const std::string renamed = m_worldPresetRenamer(worldType, newName);
            if (renamed.empty()) {
                note("Could not rename preset");
                return;
            }
            note("Preset renamed");
            // Point #world_type at the (possibly new) id and refresh the chip row.
            if (auto* sel = document->GetElementById("world_type")) {
                if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(sel))
                    fc->SetValue(renamed);
            }
            this->PopulateUserPresets(document);
        });
    }

    // Delete-confirm modal: confirm deletes the pending preset; cancel dismisses.
    if (auto* confirm = document->GetElementById("confirm_delete_preset_btn")) {
        AddClickSoundListener(confirm, [this, document](Rml::Event&) {
            Rml::Element* modal = document->GetElementById("preset_delete_modal");
            const std::string worldType =
                modal ? modal->GetAttribute<Rml::String>("data-pending", "") : "";
            if (modal)
                modal->SetClass("hidden", true);
            if (!m_worldPresetDeleter || worldType.empty())
                return;
            const bool ok = m_worldPresetDeleter(worldType);
            if (auto* n = document->GetElementById("notification")) {
                n->SetClass("hidden", false);
                if (auto* t = document->GetElementById("notification_text"))
                    t->SetInnerRML(ok ? "Preset deleted" : "Could not delete preset");
            }
            if (ok)
                this->PopulateUserPresets(document);
        });
    }
    if (auto* cancel = document->GetElementById("cancel_delete_preset_btn")) {
        AddClickSoundListener(cancel, [document](Rml::Event&) {
            if (auto* modal = document->GetElementById("preset_delete_modal"))
                modal->SetClass("hidden", true);
        });
    }

    //  live-preview weather pills. Clicking one selects it (the
    // host reads.preview-weather.selected each frame and drives set_weather).
    {
        Rml::ElementList pills;
        document->GetElementsByClassName(pills, "preview-weather");
        for (Rml::Element* pill : pills) {
            AddClickSoundListener(pill, [document](Rml::Event& event) {
                Rml::Element* p = event.GetTargetElement();
                while (p && p->GetAttribute<Rml::String>("data-weather", "").empty())
                    p = p->GetParentNode();
                if (!p)
                    return;
                Rml::ElementList all;
                document->GetElementsByClassName(all, "preview-weather");
                for (Rml::Element* x : all)
                    x->SetClass("selected", false);
                p->SetClass("selected", true);
            });
        }
    }
    // Live-preview reset-view: a marker class the host polls + clears (orbit reset
    // lives host-side in WorldgenPreview). Toggling.reset-pending signals it.
    if (auto* reset = document->GetElementById("preview_reset_btn")) {
        AddClickSoundListener(reset, [](Rml::Event& event) {
            if (Rml::Element* e = event.GetTargetElement())
                e->SetClass("reset-pending", true);
        });
    }

    // If this is the create-world screen, seed the customize form from the selected preset and
    // list any saved user presets as chips.
    if (document->GetElementById("customize_body")) {
        std::string preset = "default";
        Rml::ElementList chips;
        document->GetElementsByClassName(chips, "preset-chip");
        for (Rml::Element* c : chips) {
            if (c->IsClassSet("selected")) {
                preset = c->GetAttribute<Rml::String>("data-preset", "default");
                break;
            }
        }
        SeedWorldGenParams(document, preset);
        PopulateUserPresets(document);
    }
}

void Rml_UIManager::SeedWorldGenParams(Rml::ElementDocument* document,
                                       const std::string& worldType) {
    if (!document || !m_worldParamGetter)
        return;

    //  seed the semantic knobs from the preset's persisted knob
    // layer (path convention "knob.<id>"). A curated preset has none -> the
    // getter returns "" and the knob keeps its NEUTRAL 0.5 (never inverse-lerped).
    Rml::ElementList knobs;
    document->GetElementsByClassName(knobs, "worldgen-knob");
    for (Rml::Element* el : knobs) {
        const std::string id = el->GetAttribute<Rml::String>("data-knob", "");
        if (id.empty())
            continue;
        const std::string value = m_worldParamGetter(worldType, "knob." + id);
        const std::string v = value.empty() ? std::string("0.5") : value;
        if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(el)) {
            fc->SetValue(v);
            if (el->GetParentNode()) {
                Rml::ElementList vals;
                el->GetParentNode()->GetElementsByClassName(vals, "knob-value");
                if (!vals.empty())
                    vals[0]->SetInnerRML(v);
            }
        }
    }

    Rml::ElementList controls;
    document->GetElementsByClassName(controls, "worldgen-param");
    for (Rml::Element* el : controls) {
        const std::string path = el->GetAttribute<Rml::String>("data-path", "");
        const std::string type = el->GetAttribute<Rml::String>("data-type", "float");
        if (path.empty())
            continue;
        const std::string value = m_worldParamGetter(worldType, path);
        if (value.empty())
            continue; // preset doesn't set this key -> keep the control's default
        if (type == "bool") {
            el->SetClass("on", value == "true" || value == "1");
        } else if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(el)) {
            fc->SetValue(value);
            // Mirror into the adjacent value label.
            if (el->GetParentNode()) {
                Rml::ElementList vals;
                el->GetParentNode()->GetElementsByClassName(vals, "param-value");
                if (!vals.empty())
                    vals[0]->SetInnerRML(value);
            }
        }
    }
}

void Rml_UIManager::PopulateUserPresets(Rml::ElementDocument* document) {
    if (!document || !m_worldPresetList)
        return;
    Rml::Element* row = document->GetElementById("user_presets_row");
    if (!row)
        return;
    // Rebuild from scratch (avoids duplicates on re-entry).
    while (row->GetNumChildren() > 0)
        row->RemoveChild(row->GetChild(0));

    for (const auto& [name, type] : m_worldPresetList()) {
        Rml::ElementPtr chip = document->CreateElement("span");
        if (!chip)
            continue;
        chip->SetClassNames("preset-chip user-preset-chip");
        chip->SetAttribute("data-preset", type);
        chip->SetInnerRML(name);
        // A small delete affordance per user chip (curated chips have none). The id encodes
        // the preset type so the e2e can target a specific chip's delete control.
        Rml::ElementPtr del = document->CreateElement("span");
        if (del) {
            del->SetClassNames("preset-delete");
            del->SetId("del_" + type);
            del->SetAttribute("data-delete", type);
            del->SetInnerRML("&#10005;"); // ✕
            chip->AppendChild(std::move(del));
        }
        Rml::Element* added = row->AppendChild(std::move(chip));
        // Same behaviour as the curated chips: select + drive #world_type + re-seed the form.
        // A click on the inner delete control opens the delete-confirm modal instead.
        added->AddEventListener(
            "click", new LambdaEventListener([this, document](Rml::Event& event) {
                if (m_audioManager)
                    m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                Rml::Element* target = event.GetTargetElement();
                // Delete affordance: walk up looking for a data-delete before the chip's
                // data-preset.
                for (Rml::Element* t = target; t; t = t->GetParentNode()) {
                    const std::string del = t->GetAttribute<Rml::String>("data-delete", "");
                    if (!del.empty()) {
                        std::string disp = del;
                        if (m_worldPresetList) {
                            for (const auto& [n, ty] : m_worldPresetList())
                                if (ty == del) {
                                    disp = n;
                                    break;
                                }
                        }
                        if (auto* modal = document->GetElementById("preset_delete_modal")) {
                            modal->SetClass("hidden", false);
                            modal->SetAttribute("data-pending", del);
                        }
                        if (auto* lbl = document->GetElementById("preset_delete_name"))
                            lbl->SetInnerRML(disp);
                        return;
                    }
                    if (!t->GetAttribute<Rml::String>("data-preset", "").empty())
                        break;
                }
                Rml::Element* c = target;
                while (c && c->GetAttribute<Rml::String>("data-preset", "").empty())
                    c = c->GetParentNode();
                if (!c)
                    return;
                Rml::ElementList all;
                document->GetElementsByClassName(all, "preset-chip");
                for (Rml::Element* x : all)
                    x->SetClass("selected", false);
                c->SetClass("selected", true);
                const std::string preset = c->GetAttribute<Rml::String>("data-preset", "default");
                if (auto* sel = document->GetElementById("world_type")) {
                    if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(sel))
                        fc->SetValue(preset);
                }
                this->SeedWorldGenParams(document, preset);
            }));
    }
}

void Rml_UIManager::CommitSavePreset(Rml::ElementDocument* document, const std::string& name) {
    if (!document || !m_worldPresetSaver)
        return;
    const std::string baseType =
        ReadFormControlValue(document->GetElementById("world_type"), "default");
    const std::string saved = m_worldPresetSaver(name, baseType, CollectWorldGenParams(document));
    if (auto* note = document->GetElementById("notification")) {
        note->SetClass("hidden", false);
        if (auto* txt = document->GetElementById("notification_text"))
            txt->SetInnerRML(saved.empty() ? "Could not save preset" : "Preset saved");
    }
    if (!saved.empty())
        this->PopulateUserPresets(document);
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
void SetValueLabel(Rml::ElementDocument* doc,
                   const std::string& label_id,
                   const std::string& text) {
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

} // namespace

void Rml_UIManager::PopulateGallery(Rml::ElementDocument* document) {
    if (!document)
        return;
    Rml::Element* grid = document->GetElementById("gallery_grid");
    if (!grid)
        return;

    namespace fs = std::filesystem;
    std::error_code ec;

    // Each shutter writes <root>/data/ui/captures/cap_<N>.tga; collect them
    // newest-first.: enumerate against the ASSET ROOT (the same root the
    // emitted <img src> resolves through — the old CWD-relative lookup silently
    // found nothing when CWD != root), unless a fixture source was injected
    // (SetGalleryCaptureSource / --ui-fixtures).
    std::vector<int> ids;
    const fs::path thumbs_dir = m_galleryCaptureDir.empty()
                                    ? fs::path(m_assetRoot) / "data" / "ui" / "captures"
                                    : m_galleryCaptureDir;
    if (fs::exists(thumbs_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(thumbs_dir, ec)) {
            if (entry.path().extension() != ".tga")
                continue;
            const std::string stem = entry.path().stem().string(); // "cap_<N>"
            if (stem.rfind("cap_", 0) != 0)
                continue;
            try {
                ids.push_back(std::stoi(stem.substr(4)));
            } catch (...) {}
        }
    }
    std::sort(ids.begin(), ids.end(), std::greater<int>());

    if (ids.empty()) {
        grid->SetInnerRML(
            "<p class=\"gallery-empty\">No photos yet \xE2\x80\x94 enter a world, raise the "
            "viewfinder, and press the shutter. Your shots appear here.</p>");
        return;
    }

    std::string html;
    int shown = 0;
    for (int id : ids) {
        if (shown++ >= 12)
            break; // one page of the most recent captures
        // Star rating from the sidecar (<root>/photos/photo-<N>.photo.json), if it's there.
        int stars = 0;
        std::ifstream sf(fs::path(m_assetRoot) / "photos" /
                         ("photo-" + std::to_string(id) + ".photo.json"));
        if (sf) {
            try {
                nlohmann::json j;
                sf >> j;
                if (j.contains("stars") && j["stars"].is_number_integer())
                    stars = j["stars"].get<int>();
            } catch (...) {}
        }
        html += "<div class=\"photo-card\"><img class=\"photo-thumb\" src=\"" + m_galleryImgPrefix +
                "cap_" + std::to_string(id) + ".tga\"/>";
        if (stars >= 4)
            html += "<span class=\"photo-fav\">\xE2\x98\x85</span>";
        html += "</div>";
    }
    grid->SetInnerRML(html);
}

void Rml_UIManager::PopulateSettingsForm(Rml::ElementDocument* document) {
    if (!document)
        return;
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
    if (b.GetUiScale) {
        const float s = b.GetUiScale();
        SetControlValue(document->GetElementById("setting_ui_scale"), FormatFloat(s, 2));
        SetValueLabel(document, "setting_ui_scale_value", FormatPercent(s));
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

    // Custom widgets mirror the hidden controls: vsync toggle.on state, window-mode stepper
    // label, and each keybind chip filled from the live bindings.
    if (b.GetVSync) {
        if (auto* toggle = document->GetElementById("vsync_toggle"))
            toggle->SetClass("on", b.GetVSync());
    }
    if (b.GetWindowMode) {
        if (auto* val = document->GetElementById("window_mode_value"))
            val->SetInnerRML(b.GetWindowMode());
    }
    if (b.GetKeybind) {
        Rml::ElementList rows;
        document->GetElementsByClassName(rows, "keybind-rebind");
        for (Rml::Element* row : rows) {
            const std::string action = row->GetAttribute<Rml::String>("data-action", "");
            if (action.empty())
                continue;
            if (auto* chip = document->GetElementById("kb_" + action)) {
                const std::string label = b.GetKeybind(action);
                if (!label.empty())
                    chip->SetInnerRML(label);
            }
        }
    }
}

void Rml_UIManager::ApplySettingFromElement(Rml::Element* element) {
    if (!element)
        return;
    const std::string id = element->GetId();
    const std::string value = ReadFormControlValue(element, "");
    SettingsBridge& b = m_settingsBridge;
    Rml::ElementDocument* doc = element->GetOwnerDocument();

    auto as_float = [&value](float fallback) {
        try {
            return std::stof(value);
        } catch (...) {
            return fallback;
        }
    };

    if (id == "setting_resolution") {
        if (b.SetResolution)
            b.SetResolution(value);
    } else if (id == "setting_window_mode") {
        if (b.SetWindowMode)
            b.SetWindowMode(value);
    } else if (id == "setting_vsync") {
        if (b.SetVSync)
            b.SetVSync(value == "on" || value == "1" || value == "true");
    } else if (id == "setting_fov") {
        const float f = as_float(45.0f);
        if (b.SetFov)
            b.SetFov(f);
        if (doc)
            SetValueLabel(doc, "setting_fov_value", FormatFloat(f, 0));
    } else if (id == "setting_mouse_sensitivity") {
        const float f = as_float(0.025f);
        if (b.SetMouseSensitivity)
            b.SetMouseSensitivity(f);
        if (doc)
            SetValueLabel(doc, "setting_mouse_sensitivity_value", FormatFloat(f, 3));
    } else if (id == "setting_ui_scale") {
        const float f = as_float(1.0f);
        if (b.SetUiScale)
            b.SetUiScale(f);
        if (doc)
            SetValueLabel(doc, "setting_ui_scale_value", FormatPercent(f));
    } else if (id == "setting_audio_master") {
        const float f = as_float(1.0f);
        if (b.SetAudioMaster)
            b.SetAudioMaster(f);
        if (doc)
            SetValueLabel(doc, "setting_audio_master_value", FormatPercent(f));
    } else if (id == "setting_audio_sfx") {
        const float f = as_float(1.0f);
        if (b.SetAudioSfx)
            b.SetAudioSfx(f);
        if (doc)
            SetValueLabel(doc, "setting_audio_sfx_value", FormatPercent(f));
    } else if (id == "setting_audio_music") {
        const float f = as_float(1.0f);
        if (b.SetAudioMusic)
            b.SetAudioMusic(f);
        if (doc)
            SetValueLabel(doc, "setting_audio_music_value", FormatPercent(f));
    }
}

void Rml_UIManager::BindSettingsListeners(Rml::ElementDocument* document) {
    if (!document)
        return;
    // Only wire the settings screen.
    if (!document->GetElementById("setting_resolution") &&
        !document->GetElementById("apply_settings_btn")) {
        return;
    }

    // Seed widgets from the current settings.
    PopulateSettingsForm(document);

    // Live-apply every control on "change" (sliders, selects).
    static const char* kControlIds[] = {
        "setting_resolution",
        "setting_window_mode",
        "setting_vsync",
        "setting_fov",
        "setting_mouse_sensitivity",
        "setting_ui_scale",
        "setting_audio_master",
        "setting_audio_sfx",
        "setting_audio_music",
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
        apply->AddEventListener(
            "click", new LambdaEventListener([this, document](Rml::Event&) {
                if (m_audioManager)
                    m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                for (const char* control_id : kControlIds) {
                    this->ApplySettingFromElement(document->GetElementById(control_id));
                }
                bool ok = true;
                if (m_settingsBridge.Save)
                    ok = m_settingsBridge.Save();
                if (auto* note = document->GetElementById("notification")) {
                    note->SetClass("hidden", false);
                    if (auto* txt = document->GetElementById("notification_text")) {
                        txt->SetInnerRML(ok ? "Settings saved" : "Save failed");
                    }
                }
            }));
        apply->AddEventListener("mouseover", new LambdaEventListener([this](Rml::Event&) {
                                    if (m_audioManager)
                                        m_audioManager->PlayOneShot2D("ui_button_hover", BusId::Ui);
                                }));
    }

    // --- Custom widgets that drive the hidden form controls (settings apply live + persist) ---
    auto persist = [this]() {
        if (m_settingsBridge.Save)
            m_settingsBridge.Save();
    };

    // vsync toggle: flip.on, mirror to hidden #setting_vsync, apply + save.
    if (auto* toggle = document->GetElementById("vsync_toggle")) {
        toggle->AddEventListener(
            "click", new LambdaEventListener([this, document, persist](Rml::Event&) {
                if (m_audioManager)
                    m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                auto* t = document->GetElementById("vsync_toggle");
                const bool now_on = !(t && t->IsClassSet("on"));
                if (t)
                    t->SetClass("on", now_on);
                if (auto* sel = document->GetElementById("setting_vsync")) {
                    SetControlValue(sel, now_on ? "on" : "off");
                    this->ApplySettingFromElement(sel);
                }
                persist();
            }));
    }

    // window-mode stepper: cycle windowed/borderless/fullscreen, mirror to hidden select.
    auto cycle_window = [this, document, persist](int dir) {
        static const char* kModes[] = {"windowed", "borderless", "fullscreen"};
        auto* val = document->GetElementById("window_mode_value");
        std::string cur = val ? val->GetInnerRML() : std::string("borderless");
        int idx = 1;
        for (int i = 0; i < 3; ++i)
            if (cur == kModes[i])
                idx = i;
        idx = (idx + dir + 3) % 3;
        const std::string next = kModes[idx];
        if (val)
            val->SetInnerRML(next);
        if (auto* sel = document->GetElementById("setting_window_mode")) {
            SetControlValue(sel, next);
            this->ApplySettingFromElement(sel);
        }
        persist();
    };
    if (auto* prev = document->GetElementById("window_mode_prev")) {
        prev->AddEventListener("click", new LambdaEventListener([this, cycle_window](Rml::Event&) {
                                   if (m_audioManager)
                                       m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                                   cycle_window(-1);
                               }));
    }
    if (auto* next = document->GetElementById("window_mode_next")) {
        next->AddEventListener("click", new LambdaEventListener([this, cycle_window](Rml::Event&) {
                                   if (m_audioManager)
                                       m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                                   cycle_window(+1);
                               }));
    }

    // keybind rows: click to capture the next key as that action's binding (chip updates on the
    // next settings open). Highlights the active row and shows a prompt.
    Rml::ElementList kb_rows;
    document->GetElementsByClassName(kb_rows, "keybind-rebind");
    for (Rml::Element* row : kb_rows) {
        row->AddEventListener(
            "click", new LambdaEventListener([this, document](Rml::Event& ev) {
                if (m_audioManager)
                    m_audioManager->PlayOneShot2D("ui_button_click", BusId::Ui);
                Rml::Element* r = ev.GetTargetElement();
                while (r && r->GetAttribute<Rml::String>("data-action", "").empty())
                    r = r->GetParentNode();
                if (!r)
                    return;
                const std::string action = r->GetAttribute<Rml::String>("data-action", "");
                Rml::ElementList all;
                document->GetElementsByClassName(all, "keybind-rebind");
                for (Rml::Element* x : all)
                    x->SetClass("selected", false);
                r->SetClass("selected", true);
                if (auto* chip = document->GetElementById("kb_" + action))
                    chip->SetInnerRML("press a key");
                if (m_settingsBridge.BeginRebind)
                    m_settingsBridge.BeginRebind(action);
            }));
    }
}

// --- Static GLFW Callbacks ---
void Rml_UIManager::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (!s_active_manager || !s_active_manager->m_context)
        return;

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
        if (action == GLFW_PRESS)
            s_active_manager->m_context->ProcessMouseButtonDown(button, rml_mods);
        else
            s_active_manager->m_context->ProcessMouseButtonUp(button, rml_mods);
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
