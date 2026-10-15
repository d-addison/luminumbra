#include "app/UiInputRouter.h"

namespace Luminumbra::Client::App {
bool UiInputRouter::SetOwnership(UiInputOwnership ownership) {
    if (m_ownership == ownership)
        return false;
    // Include ownership changes between two different developer panels or menu/world states.
    m_quarantined.insert(m_held.begin(), m_held.end());
    m_ownership = ownership;
    ++m_revision;
    return true;
}
InputOwner UiInputRouter::Owner() const {
    if (!m_ownership.window_focused)
        return InputOwner::Unfocused;
    if (m_ownership.binding_capture || m_ownership.console_open || m_ownership.interactive_panel)
        return InputOwner::Developer;
    if (m_ownership.rml_text_input)
        return InputOwner::RmlUi;
    if (m_ownership.imgui_text_input || m_ownership.imgui_keyboard || m_ownership.imgui_mouse)
        return InputOwner::Developer;
    if (m_ownership.rml_interaction || !m_ownership.in_world)
        return InputOwner::RmlUi;
    return InputOwner::Gameplay;
}
KeyRoute UiInputRouter::Key(int key, int action, bool console_toggle, bool developer_shortcut) {
    m_suppressCharacter = false;
    if (action == 0) {
        m_held.erase(key);
        m_quarantined.erase(key);
        m_consumedShortcutKeys.erase(key);
        return KeyRoute::Ignore;
    }
    m_held.insert(key);
    if (!m_ownership.window_focused)
        return KeyRoute::Ignore;
    if (m_ownership.binding_capture)
        return KeyRoute::Capture;
    if (action == 2 && m_consumedShortcutKeys.contains(key)) {
        m_suppressCharacter = true;
        return KeyRoute::Ignore;
    }
    const bool printable = (key >= 32 && key <= 162) || (key >= 320 && key <= 334);
    if (m_ownership.rml_text_input && Owner() == InputOwner::RmlUi && printable)
        return KeyRoute::RmlUi;
    if (console_toggle) {
        // GLFW sends character callbacks after key callbacks, including on repeat.
        m_suppressCharacter = true;
        if (action == 1)
            m_consumedShortcutKeys.insert(key);
        return action == 1 ? KeyRoute::ConsoleToggle : KeyRoute::Ignore;
    }
    if (action == 1 && key == 256 && (Owner() == InputOwner::Developer || m_ownership.rml_debugger))
        return KeyRoute::DeveloperEscape;
    // Panel ownership blocks gameplay, but resolved developer bindings remain usable.
    // Text entry blocks actions for every key, including function keys.
    const bool text_capture = m_ownership.imgui_text_input || m_ownership.rml_text_input;
    if (action == 1 && developer_shortcut && !text_capture &&
        (Owner() == InputOwner::Developer || m_ownership.rml_debugger)) {
        m_suppressCharacter = true;
        m_consumedShortcutKeys.insert(key);
        return KeyRoute::DeveloperShortcut;
    }
    if (Owner() == InputOwner::Developer)
        return KeyRoute::Ignore;
    if (m_quarantined.contains(key))
        return KeyRoute::Ignore;
    if (Owner() == InputOwner::RmlUi)
        return KeyRoute::RmlUi;
    if (action == 1 && developer_shortcut && !text_capture) {
        m_suppressCharacter = true;
        m_consumedShortcutKeys.insert(key);
        return KeyRoute::DeveloperShortcut;
    }
    return KeyRoute::Gameplay;
}
InputOwner UiInputRouter::Character(unsigned int codepoint) const {
    if (m_suppressCharacter || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return InputOwner::Unfocused;
    return Owner();
}
bool UiInputRouter::GameplayAllowed() const {
    return Owner() == InputOwner::Gameplay;
}
bool UiInputRouter::GameplayKeyAllowed(int key) const {
    return GameplayAllowed() && !m_quarantined.contains(key);
}
bool UiInputRouter::GameplayMouseAllowed() const {
    return GameplayAllowed();
}
bool UiInputRouter::CursorCaptured() const {
    return GameplayMouseAllowed();
}
bool UiInputRouter::TimeShortcutAllowed(bool photo_mode, int key) {
    return !photo_mode || (key != 91 && key != 93);
}
std::uint64_t UiInputRouter::Revision() const {
    return m_revision;
}
} // namespace Luminumbra::Client::App
