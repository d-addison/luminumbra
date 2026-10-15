#pragma once

#include "core/DeveloperPanelLayout.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Luminumbra::Client::Debug {
using luminumbra::core::DeveloperPanelLayout;

struct KeyChord {
    int key = -1;
    int mods = 0;
    bool operator==(const KeyChord&) const = default;
};
std::optional<KeyChord> ParseKeyChord(const std::string& text);
std::string FormatKeyChord(std::optional<KeyChord> chord);
bool IsModifierKey(int key);
bool ValidChord(KeyChord chord);

enum BindingScope : unsigned {
    Global = 1,
    Gameplay = 2,
    Photo = 4
};
struct BindingDef {
    std::string id;
    std::optional<KeyChord> default_binding;
    unsigned scopes = Global | Gameplay | Photo;
};

// Both registries use this one authority. Reserved gameplay bindings are read-only.
class DeveloperBindings {
public:
    bool Register(BindingDef def);
    void Reserve(KeyChord chord, unsigned scopes, std::string reason);
    bool Rebind(const std::string& id, std::optional<KeyChord> chord, std::string& error);
    std::optional<KeyChord> Get(const std::string& id) const;
    bool Contains(const std::string& id) const;
    std::vector<BindingDef> List() const;
    std::optional<std::string> Match(KeyChord chord, int action, unsigned scope) const;
    void RestoreDefaults();
    void Validate();
    void Seal();
    std::function<void()> changed;
    std::function<void(const std::string&)> conflict_reported;
    std::function<std::string(KeyChord, unsigned)> external_conflict;

private:
    friend class DeveloperWidgetRegistry;
    bool ResolveConflicts();
    void BeginDraw();
    void EndDraw(bool apply);
    std::optional<KeyChord> EffectiveBinding(const std::string& id) const;
    bool m_sealed = false;
    bool m_drawing = false;
    std::vector<std::pair<std::string, std::optional<KeyChord>>> m_pending;
    struct Entry {
        BindingDef def;
        std::optional<KeyChord> chord;
    };
    struct Reservation {
        KeyChord chord;
        unsigned scopes;
        std::string reason;
    };
    std::vector<Entry> m_entries;
    std::vector<Reservation> m_reserved;
};

struct Availability {
    bool available = true;
    std::string reason;
};
struct DeveloperFrameAdapter;
using AvailabilityFn = std::function<Availability()>;
using DrawFn = std::function<void(DeveloperFrameAdapter&)>;
enum class Presentation {
    ConsolePanel,
    PassiveOverlay,
    ActionOnly
};
enum class FocusPolicy {
    Interactive,
    Passive
};
struct WidgetDef {
    std::string id;
    std::string title;
    std::string category;
    bool default_visible = false;
    std::optional<KeyChord> default_binding;
    std::string persistence_key;
    Presentation presentation = Presentation::ConsolePanel;
    FocusPolicy focus_policy = FocusPolicy::Interactive;
    AvailabilityFn available = {};
    DrawFn draw = {};
    // External presentations (such as the RmlUi debugger) mirror registry mutations.
    std::function<void(bool)> visibility_changed = {};
    std::string description;
    // Effect bindings belong to this module; they do not toggle panel visibility.
    std::vector<BindingDef> shortcuts;
    std::vector<BindingDef> BindingDefinitions() const {
        if (!shortcuts.empty())
            return shortcuts;
        if (presentation == Presentation::ActionOnly)
            return {};
        return {{id, default_binding}};
    }
};

std::vector<WidgetDef> BuiltinDeveloperWidgets();

class DeveloperWidgetRegistry {
public:
    explicit DeveloperWidgetRegistry(DeveloperBindings& bindings);
    bool Register(WidgetDef def);
    void Seal();
    void Shutdown();
    const std::vector<WidgetDef>& List() const;
    const WidgetDef* Find(const std::string& id) const;
    bool Visible(const std::string& id) const;
    bool SetVisible(const std::string& id, bool visible);
    bool Toggle(const std::string& id);
    std::optional<DeveloperPanelLayout> Layout(const std::string& id) const;
    bool SetLayout(const std::string& id, DeveloperPanelLayout layout);
    bool Rebind(const std::string& id, std::optional<KeyChord> chord, std::string& error);
    void RestoreDefaults();
    void DrawVisible(DeveloperFrameAdapter& frame, Presentation presentation);
    bool HasInteractivePanel(const std::string& except = {}) const;
    bool CloseTopInteraction();
    void FocusInteraction(const std::string& id);
    std::uint64_t NextInteractionSequence();
    std::uint64_t TopInteractionSequence() const;
    std::function<void()> changed;

private:
    DeveloperBindings& m_bindings;
    std::vector<WidgetDef> m_definitions;
    std::map<std::string, bool> m_visible;
    std::map<std::string, DeveloperPanelLayout> m_layouts;
    std::vector<std::pair<std::string, bool>> m_deferred;
    std::map<std::string, std::uint64_t> m_interactions;
    std::uint64_t m_interactionSequence = 0;
    bool m_sealed = false;
    bool m_drawing = false;
};
} // namespace Luminumbra::Client::Debug
