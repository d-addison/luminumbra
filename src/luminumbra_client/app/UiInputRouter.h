#pragma once

#include "app/UiInputService.h"

#include <set>

namespace Luminumbra::Client::App {
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
    bool imgui_text_input = false;
    bool imgui_keyboard = false;
    bool imgui_mouse = false;
    bool operator==(const UiInputOwnership&) const = default;
};
// No window/UI dependencies: the host updates explicit ownership within each event batch.
class UiInputRouter {
public:
    UiInputRouter();
    // Reserves two registrations for the legacy host and console. External scoped
    // contexts must be destroyed before this router.
    UiInputService& Contexts();
    bool SetOwnership(UiInputOwnership ownership);
    InputOwner Owner() const;
    KeyRoute Key(int key, int action, bool console_toggle, bool developer_shortcut);
    InputOwner Character(unsigned int codepoint) const;
    bool GameplayAllowed() const;
    // Developer panels retain world bindings; menus and modal world screens use global ones.
    bool WorldShortcutsAllowed() const;
    bool GameplayKeyAllowed(int key) const;
    bool GameplayMouseAllowed() const;
    bool CursorCaptured() const;
    std::uint64_t Revision() const;

private:
    UiInputOwnership m_ownership;
    UiInputService m_contexts;
    ScopedInputContext m_legacyContext;
    ScopedInputContext m_consoleContext;
    std::set<int> m_consumedShortcutKeys;
    std::uint64_t m_seenRevision = 0;
    bool m_suppressCharacter = false;
};
} // namespace Luminumbra::Client::App
