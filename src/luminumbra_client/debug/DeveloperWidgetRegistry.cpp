#include "debug/DeveloperWidgetRegistry.h"

#include <algorithm>
#include <charconv>

namespace Luminumbra::Client::Debug {
bool IsModifierKey(int key) {
    return key >= 340 && key <= 347;
}
bool ValidChord(KeyChord chord) {
    const int key = chord.key;
    const bool known = key == 32 || key == 39 || (key >= 44 && key <= 57) || key == 59 ||
                       key == 61 || (key >= 65 && key <= 93) || key == 96 || key == 161 ||
                       key == 162 || (key >= 256 && key <= 269) || (key >= 280 && key <= 284) ||
                       (key >= 290 && key <= 314) || (key >= 320 && key <= 336) || key == 348;
    return known && !IsModifierKey(key) && (chord.mods & ~15) == 0;
}
std::optional<KeyChord> ParseKeyChord(const std::string& text) {
    if (text.empty())
        return std::nullopt;
    KeyChord chord;
    std::string key = text;
    for (std::size_t p; (p = key.find('+')) != std::string::npos;) {
        const auto mod = key.substr(0, p);
        const int bit = mod == "Shift"   ? 1
                        : mod == "Ctrl"  ? 2
                        : mod == "Alt"   ? 4
                        : mod == "Super" ? 8
                                         : 0;
        if (!bit || (chord.mods & bit))
            return std::nullopt;
        chord.mods |= bit;
        key.erase(0, p + 1);
    }
    if (key.empty())
        return std::nullopt;
    if (key.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(key[0]);
        chord.key = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
    } else if (key == "Space")
        chord.key = 32;
    else if (key == "Enter")
        chord.key = 257;
    else if (key == "Escape")
        chord.key = 256;
    else {
        const bool function = key[0] == 'F';
        const char* start = key.data() + (function ? 1 : 0);
        int number = 0;
        const auto result = std::from_chars(start, key.data() + key.size(), number);
        if (result.ec != std::errc{} || result.ptr != key.data() + key.size())
            return std::nullopt;
        if (function && (number < 1 || number > 25))
            return std::nullopt;
        chord.key = function ? 289 + number : number;
    }
    return ValidChord(chord) ? std::optional<KeyChord>(chord) : std::nullopt;
}
std::string FormatKeyChord(std::optional<KeyChord> chord) {
    if (!chord)
        return "Unbound";
    std::string result;
    if (chord->mods & 2)
        result += "Ctrl+";
    if (chord->mods & 1)
        result += "Shift+";
    if (chord->mods & 4)
        result += "Alt+";
    if (chord->mods & 8)
        result += "Super+";
    const int key = chord->key;
    if (key >= 290 && key <= 314)
        result += "F" + std::to_string(key - 289);
    else if (key == 32)
        result += "Space";
    else if (key >= 33 && key <= 126)
        result += static_cast<char>(key);
    else
        result += std::to_string(key);
    return result;
}
bool DeveloperBindings::Contains(const std::string& id) const {
    return std::any_of(
        m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.def.id == id; });
}
bool DeveloperBindings::Register(BindingDef def) {
    if (m_sealed || def.id.empty() || Contains(def.id) ||
        (def.default_binding && !ValidChord(*def.default_binding)))
        return false;
    if (def.default_binding) {
        for (const auto& entry : m_entries)
            if (entry.chord == def.default_binding && (entry.def.scopes & def.scopes))
                return false;
    }
    m_entries.push_back({def, def.default_binding});
    return true;
}
void DeveloperBindings::Reserve(KeyChord chord, unsigned scopes, std::string reason) {
    m_reserved.push_back({chord, scopes, std::move(reason)});
}
bool DeveloperBindings::Rebind(const std::string& id,
                               std::optional<KeyChord> chord,
                               std::string& error) {
    auto it = std::find_if(
        m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.def.id == id; });
    if (it == m_entries.end()) {
        error = "Unknown action: " + id;
        return false;
    }
    if (chord) {
        if (!ValidChord(*chord)) {
            error = "Invalid key chord";
            return false;
        }
        if (external_conflict) {
            error = external_conflict(*chord, it->def.scopes);
            if (!error.empty())
                return false;
        }
        for (const auto& r : m_reserved)
            if (r.chord == *chord && (r.scopes & it->def.scopes)) {
                error = "Reserved: " + r.reason;
                return false;
            }
        for (const auto& e : m_entries)
            if (e.def.id != id && EffectiveBinding(e.def.id) == chord &&
                (e.def.scopes & it->def.scopes)) {
                error = "Conflicts with " + e.def.id;
                return false;
            }
    }
    if (m_drawing) {
        m_pending.emplace_back(id, chord);
        return true;
    }
    if (it->chord != chord) {
        it->chord = chord;
        if (changed)
            changed();
    }
    return true;
}
std::optional<KeyChord> DeveloperBindings::Get(const std::string& id) const {
    for (const auto& e : m_entries)
        if (e.def.id == id)
            return e.chord;
    return std::nullopt;
}
std::vector<BindingDef> DeveloperBindings::List() const {
    std::vector<BindingDef> result;
    for (const auto& e : m_entries)
        result.push_back(e.def);
    return result;
}
std::optional<std::string>
DeveloperBindings::Match(KeyChord chord, int action, unsigned scope) const {
    if (action != 1)
        return std::nullopt; // GLFW press, never repeat.
    chord.mods &= 15;
    for (const auto& e : m_entries)
        if (e.chord == chord && (e.def.scopes & scope))
            return e.def.id;
    return std::nullopt;
}
void DeveloperBindings::Seal() {
    m_sealed = true;
}
void DeveloperBindings::BeginDraw() {
    m_sealed = true;
    m_drawing = true;
}
std::optional<KeyChord> DeveloperBindings::EffectiveBinding(const std::string& id) const {
    auto value = Get(id);
    for (const auto& [pending_id, chord] : m_pending)
        if (id == pending_id)
            value = chord;
    return value;
}
void DeveloperBindings::EndDraw(bool apply) {
    m_drawing = false;
    bool mutated = false;
    if (apply) {
        for (const auto& [id, chord] : m_pending)
            for (auto& entry : m_entries)
                if (entry.def.id == id && entry.chord != chord) {
                    entry.chord = chord;
                    mutated = true;
                }
    }
    m_pending.clear();
    if (apply)
        mutated = ResolveConflicts() || mutated;
    if (mutated && changed)
        changed();
}
void DeveloperBindings::RestoreDefaults() {
    if (m_drawing) {
        for (const auto& e : m_entries)
            m_pending.emplace_back(e.def.id, e.def.default_binding);
        return;
    }
    for (auto& e : m_entries)
        e.chord = e.def.default_binding;
    ResolveConflicts();
    if (changed)
        changed();
}

bool DeveloperBindings::ResolveConflicts() {
    bool mutated = false;
    // Registration order decides which developer action wins; gameplay always wins.
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (!it->chord)
            continue;
        std::string error = external_conflict ? external_conflict(*it->chord, it->def.scopes) : "";
        for (const auto& reservation : m_reserved)
            if (error.empty() && reservation.chord == *it->chord &&
                (reservation.scopes & it->def.scopes))
                error = "Reserved: " + reservation.reason;
        for (auto previous = m_entries.begin(); error.empty() && previous != it; ++previous)
            if (previous->chord == it->chord && (previous->def.scopes & it->def.scopes))
                error = "Conflicts with " + previous->def.id;
        if (!error.empty()) {
            const auto message = "Developer binding " + it->def.id + " (" +
                                 FormatKeyChord(it->chord) + ") unbound: " + error;
            it->chord.reset();
            mutated = true;
            if (conflict_reported)
                conflict_reported(message);
        }
    }
    return mutated;
}
void DeveloperBindings::ApplySaved(
    const std::map<std::string, luminumbra::core::DeveloperBindingSetting>& saved) {
    for (const auto& entry : m_entries) {
        if (saved.contains(entry.def.id)) {
            std::string error;
            Rebind(entry.def.id, std::nullopt, error);
        }
    }
    for (const auto& entry : m_entries) {
        const auto record = saved.find(entry.def.id);
        if (record == saved.end())
            continue;
        const auto& binding = record->second;
        const auto chord = binding.key == -1
                               ? std::nullopt
                               : std::optional<KeyChord>(KeyChord{binding.key, binding.mods});
        std::string error;
        if (!Rebind(entry.def.id, chord, error) && conflict_reported)
            conflict_reported("Developer binding " + entry.def.id + " unbound: " + error);
    }
    Validate();
}
void DeveloperBindings::Validate() {
    if (ResolveConflicts() && changed)
        changed();
}

std::vector<WidgetDef> BuiltinDeveloperWidgets() {
    std::vector<WidgetDef> result;
    auto add = [&](const char* id,
                   const char* title,
                   const char* category,
                   const char* description,
                   bool visible,
                   int key,
                   bool passive,
                   std::vector<BindingDef> shortcuts = {},
                   bool action_only = false) {
        WidgetDef def;
        def.id = id;
        def.title = title;
        def.category = category;
        def.description = description;
        def.default_visible = visible;
        def.default_binding = key < 0 ? std::nullopt : std::optional<KeyChord>(KeyChord{key, 0});
        def.persistence_key = id;
        def.presentation = action_only ? Presentation::ActionOnly
                           : passive   ? Presentation::PassiveOverlay
                                       : Presentation::ConsolePanel;
        def.focus_policy = passive || action_only ? FocusPolicy::Passive : FocusPolicy::Interactive;
        def.available = [] {
            return Availability{false, "Service unavailable"};
        };
        def.shortcuts = std::move(shortcuts);
        result.push_back(std::move(def));
    };
    add("perf.hud",
        "Frame performance",
        "Performance",
        "Frame timing, UI cost and terrain draws",
        true,
        -1,
        true);
    add("perf.gpu", "GPU profiler", "Performance", "GPU timings by render pass", false, 292, false);
    add("render.shaders",
        "Shaders",
        "Rendering",
        "Inspect shaders, reload status and uniforms",
        false,
        299,
        false);
    add("shader.reload",
        "Shader reload",
        "Rendering",
        "Reload all shaders with rollback on failure",
        false,
        -1,
        false,
        {{"shader.reload", KeyChord{294, 0}}},
        true);
    add("render.debug_views",
        "Debug views",
        "Rendering",
        "Cycle G-buffer inspection views",
        false,
        -1,
        false,
        {{"render.debug-cycle", KeyChord{295, 0}}});
    add("render.wireframe",
        "Wireframe",
        "Rendering",
        "Toggle the wireframe rendering effect",
        false,
        -1,
        false,
        {{"render.wireframe-toggle", KeyChord{298, 0}}});
    add("world.generation",
        "World generation",
        "World",
        "Inspect and edit an independent world preview",
        false,
        296,
        false);
    add("client.settings",
        "Client settings",
        "Client",
        "Video, audio, controls and developer preferences",
        false,
        297,
        false);
    add("runtime.time",
        "Time controls",
        "Runtime",
        "Slow, speed up or reset simulation time",
        false,
        -1,
        false,
        {{"time.slower", KeyChord{91, 0}, Gameplay},
         {"time.faster", KeyChord{93, 0}, Gameplay},
         {"time.reset", KeyChord{92, 0}, Gameplay | Photo}});
    add("runtime.time_indicator",
        "Time indicator",
        "Runtime",
        "Show the active non-default time scale",
        true,
        -1,
        true);
    add("player.state",
        "Player state",
        "Inspection",
        "Inspect current player state",
        false,
        -1,
        false);
    add("farm.debug",
        "Farm diagnostics",
        "Inspection",
        "Inspect farming state and interactions",
        false,
        -1,
        false);
    add("world.creature_labels",
        "Creature labels",
        "Inspection",
        "Label visible creatures in the world",
        false,
        -1,
        true);
    add("ui.data_stream", "UI data stream", "UI", "Inspect UI stream snapshots", false, -1, false);
    add("ui.debugger",
        "RmlUi debugger",
        "UI",
        "Inspect UI documents and elements",
        false,
        -1,
        false);
    add("console.help", "Help", "Console", "Command syntax and resolved hotkeys", false, -1, false);
    return result;
}
DeveloperWidgetRegistry::DeveloperWidgetRegistry(DeveloperBindings& bindings)
    : m_bindings(bindings) {}
bool DeveloperWidgetRegistry::Register(WidgetDef def) {
    if (m_sealed || m_drawing || def.id.empty() || Find(def.id))
        return false;
    if (def.persistence_key.empty())
        def.persistence_key = def.id;
    const auto bindings = def.BindingDefinitions();
    auto candidate = m_bindings;
    for (const auto& binding : bindings)
        if (!candidate.Register(binding))
            return false;
    for (const auto& binding : bindings)
        m_bindings.Register(binding);
    if (def.presentation != Presentation::ActionOnly)
        m_visible[def.id] = def.default_visible;
    const auto id = def.id;
    m_definitions.push_back(std::move(def));
    if (Visible(id))
        FocusInteraction(id);
    return true;
}
void DeveloperWidgetRegistry::Seal() {
    m_sealed = true;
    m_bindings.Seal();
}
void DeveloperWidgetRegistry::Shutdown() {
    changed = {};
    for (const auto& def : m_definitions)
        if (Visible(def.id) && def.visibility_changed)
            def.visibility_changed(false);
    m_definitions.clear();
    m_interactions.clear();
    m_visible.clear();
    m_layouts.clear();
    m_deferred.clear();
}
const std::vector<WidgetDef>& DeveloperWidgetRegistry::List() const {
    return m_definitions;
}
const WidgetDef* DeveloperWidgetRegistry::Find(const std::string& id) const {
    for (const auto& def : m_definitions)
        if (def.id == id)
            return &def;
    return nullptr;
}
bool DeveloperWidgetRegistry::Visible(const std::string& id) const {
    const auto it = m_visible.find(id);
    return it != m_visible.end() && it->second;
}
bool DeveloperWidgetRegistry::SetVisible(const std::string& id, bool visible) {
    const auto* def = Find(id);
    if (!def || def->presentation == Presentation::ActionOnly)
        return false;
    if (m_drawing) {
        m_deferred.emplace_back(id, visible);
        return true;
    }
    if (Visible(id) != visible) {
        m_visible[id] = visible;
        if (auto layout = m_layouts.find(id); layout != m_layouts.end())
            layout->second.open = visible;
        if (def->visibility_changed)
            def->visibility_changed(visible);
        if (visible)
            FocusInteraction(id);
        else
            m_interactions.erase(id);
        if (changed)
            changed();
    }
    return true;
}
bool DeveloperWidgetRegistry::Toggle(const std::string& id) {
    bool value = Visible(id);
    for (const auto& pending : m_deferred)
        if (pending.first == id)
            value = pending.second;
    return SetVisible(id, !value);
}
std::optional<DeveloperPanelLayout> DeveloperWidgetRegistry::Layout(const std::string& id) const {
    const auto it = m_layouts.find(id);
    return it == m_layouts.end() ? std::nullopt : std::optional<DeveloperPanelLayout>(it->second);
}
bool DeveloperWidgetRegistry::SetLayout(const std::string& id, DeveloperPanelLayout layout) {
    const auto* def = Find(id);
    if (!def || def->presentation != Presentation::ConsolePanel ||
        def->focus_policy != FocusPolicy::Interactive || !ValidDeveloperPanelLayout(layout))
        return false;
    layout.open = Visible(id);
    if (Layout(id) != layout) {
        m_layouts[id] = layout;
        if (changed)
            changed();
    }
    return true;
}
bool DeveloperWidgetRegistry::Rebind(const std::string& id,
                                     std::optional<KeyChord> chord,
                                     std::string& error) {
    if (!Find(id)) {
        error = "Unknown widget: " + id;
        return false;
    }
    return m_bindings.Rebind(id, chord, error);
}
void DeveloperWidgetRegistry::RestoreDefaults() {
    for (const auto& def : m_definitions)
        SetVisible(def.id, def.default_visible);
    m_bindings.RestoreDefaults();
}
void DeveloperWidgetRegistry::DrawVisible(DeveloperFrameAdapter& frame, Presentation presentation) {
    if (m_drawing)
        return;
    m_sealed = true;
    m_drawing = true;
    m_bindings.BeginDraw();
    try {
        for (const auto& def : m_definitions)
            if (def.presentation == presentation && Visible(def.id) && def.draw &&
                (!def.available || def.available().available))
                def.draw(frame);
    } catch (...) {
        m_drawing = false;
        m_deferred.clear();
        m_bindings.EndDraw(false);
        throw;
    }
    m_drawing = false;
    auto pending = std::move(m_deferred);
    m_deferred.clear();
    for (const auto& [id, visible] : pending)
        SetVisible(id, visible);
    m_bindings.EndDraw(true);
}
bool DeveloperWidgetRegistry::HasInteractivePanel(const std::string& except) const {
    for (const auto& def : m_definitions)
        if (def.id != except && Visible(def.id) && def.focus_policy == FocusPolicy::Interactive &&
            (!def.available || def.available().available))
            return true;
    return false;
}
std::uint64_t DeveloperWidgetRegistry::NextInteractionSequence() {
    return ++m_interactionSequence;
}
void DeveloperWidgetRegistry::FocusInteraction(const std::string& id) {
    const auto* def = Find(id);
    if (def && Visible(id) && def->focus_policy == FocusPolicy::Interactive)
        m_interactions[id] = NextInteractionSequence();
}
std::uint64_t DeveloperWidgetRegistry::TopInteractionSequence() const {
    std::uint64_t sequence = 0;
    for (const auto& def : m_definitions) {
        if (!Visible(def.id) || def.focus_policy != FocusPolicy::Interactive ||
            (def.available && !def.available().available))
            continue;
        const auto it = m_interactions.find(def.id);
        if (it != m_interactions.end())
            sequence = std::max(sequence, it->second);
    }
    return sequence;
}
bool DeveloperWidgetRegistry::CloseTopInteraction() {
    const auto sequence = TopInteractionSequence();
    if (!sequence)
        return false;
    for (const auto& [id, active] : m_interactions)
        if (active == sequence)
            return SetVisible(id, false);
    return false;
}
} // namespace Luminumbra::Client::Debug
