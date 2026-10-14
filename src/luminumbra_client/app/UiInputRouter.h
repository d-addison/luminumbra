#pragma once

#include <cstdint>
#include <set>

namespace Luminumbra::Client::App {
enum class InputOwner {
    Gameplay,
    Developer,
    RmlUi,
    Unfocused
};
enum class KeyRoute {
    Capture,
    ConsoleToggle,
    DeveloperEscape,
    DeveloperShortcut,
    RmlUi,
    Gameplay,
    Ignore
};
struct UiInputOwnership {
    bool in_world = false;
    bool window_focused = true;
    bool binding_capture = false;
    bool console_open = false;
    bool interactive_panel = false;
    bool rml_interaction = false;
    bool rml_debugger = false;
    bool rml_text_input = false;
    bool imgui_keyboard = false;
    bool imgui_mouse = false;
    bool operator==(const UiInputOwnership&) const = default;
};
// No window/UI dependencies: the host updates explicit ownership within each event batch.
class UiInputRouter {
public:
    bool SetOwnership(UiInputOwnership ownership);
    InputOwner Owner() const;
    KeyRoute Key(int key,
                 int action,
                 bool console_toggle,
                 bool developer_shortcut,
                 bool panel_shortcut = false);
    InputOwner Character(unsigned int codepoint) const;
    bool GameplayAllowed() const;
    bool GameplayKeyAllowed(int key) const;
    bool GameplayMouseAllowed() const;
    bool CursorCaptured() const;
    static bool TimeShortcutAllowed(bool photo_mode, int key);
    std::uint64_t Revision() const;

private:
    UiInputOwnership m_ownership;
    std::set<int> m_held;
    std::set<int> m_quarantined;
    std::set<int> m_consumedToggleKeys;
    std::uint64_t m_revision = 0;
    bool m_suppressCharacter = false;
};
} // namespace Luminumbra::Client::App
