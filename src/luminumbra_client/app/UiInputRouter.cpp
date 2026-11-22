#include "app/UiInputRouter.h"

namespace Luminumbra::Client::App {
namespace {
InputContextPolicy LegacyPolicy(const UiInputOwnership& ownership) {
    InputOwner owner = InputOwner::Gameplay;
    if (ownership.binding_capture || ownership.interactive_panel)
        owner = InputOwner::Developer;
    else if (ownership.rml_text_input)
        owner = InputOwner::RmlUi;
    else if (ownership.imgui_text_input || ownership.imgui_keyboard || ownership.imgui_mouse)
        owner = InputOwner::Developer;
    else if (ownership.rml_interaction || !ownership.in_world)
        owner = InputOwner::RmlUi;
    return {owner,
            owner == InputOwner::Gameplay ? CursorMode::Captured : CursorMode::Visible,
            ownership.rml_text_input || ownership.imgui_text_input};
}
} // namespace
UiInputRouter::UiInputRouter()
    : m_legacyContext(m_contexts, LegacyPolicy(m_ownership))
    , m_consoleContext(m_contexts, {}) {
    m_contexts.Focus(m_legacyContext.Id());
    m_seenRevision = m_contexts.Revision();
}
UiInputService& UiInputRouter::Contexts() {
    return m_contexts;
}
bool UiInputRouter::SetOwnership(UiInputOwnership ownership) {
    const bool changed = m_ownership != ownership;
    if (changed) {
        // Legacy hosts still clear queued actions for changes between UI backends.
        m_contexts.ReleaseHeldInput();
        m_contexts.SetWindowFocused(ownership.window_focused);
        m_contexts.Update(m_legacyContext.Id(), LegacyPolicy(ownership));
        if (ownership.console_open && !m_ownership.console_open)
            m_contexts.Focus(m_consoleContext.Id());
        else if (!ownership.console_open && m_ownership.console_open)
            m_contexts.Unfocus(m_consoleContext.Id());
        m_ownership = ownership;
    }
    const bool capture_changed = m_seenRevision != m_contexts.Revision();
    m_seenRevision = m_contexts.Revision();
    return changed || capture_changed;
}
InputOwner UiInputRouter::Owner() const {
    return m_contexts.Owner();
}
KeyRoute UiInputRouter::Key(int key, int action, bool console_toggle, bool developer_shortcut) {
    m_suppressCharacter = false;
    if (action == 0) {
        m_contexts.KeyUp(key);
        m_consumedShortcutKeys.erase(key);
        return KeyRoute::Ignore;
    }
    m_contexts.KeyDown(key);
    if (Owner() == InputOwner::Unfocused)
        return KeyRoute::Ignore;
    if (m_ownership.binding_capture)
        return KeyRoute::Capture;
    if (action == 2 && m_consumedShortcutKeys.contains(key)) {
        m_suppressCharacter = true;
        return KeyRoute::Ignore;
    }
    const bool printable = (key >= 32 && key <= 162) || (key >= 320 && key <= 334);
    const bool text_capture =
        m_ownership.imgui_text_input || m_ownership.rml_text_input || m_contexts.TextEntry();
    // ImGui already receives key callbacks; RmlUi needs an explicit route for typing.
    if (text_capture && printable)
        return Owner() == InputOwner::RmlUi ? KeyRoute::RmlUi : KeyRoute::Ignore;
    if (console_toggle) {
        // GLFW sends character callbacks after key callbacks, including on repeat.
        m_suppressCharacter = true;
        if (action == 1)
            m_consumedShortcutKeys.insert(key);
        return action == 1 ? KeyRoute::ConsoleToggle : KeyRoute::Ignore;
    }
    if (action == 1 && key == 256 && (Owner() == InputOwner::Developer || m_ownership.rml_debugger))
        return KeyRoute::DeveloperEscape;
    // UI ownership blocks gameplay, but resolved developer bindings remain usable.
    // Text entry blocks other actions for every key, including function keys.
    if (action == 1 && developer_shortcut && !text_capture &&
        (Owner() != InputOwner::Gameplay || !m_contexts.KeyQuarantined(key))) {
        m_suppressCharacter = true;
        m_consumedShortcutKeys.insert(key);
        return KeyRoute::DeveloperShortcut;
    }
    if (Owner() == InputOwner::Developer)
        return KeyRoute::Ignore;
    if (m_contexts.KeyQuarantined(key))
        return KeyRoute::Ignore;
    if (Owner() == InputOwner::RmlUi)
        return KeyRoute::RmlUi;
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
bool UiInputRouter::WorldShortcutsAllowed() const {
    return m_ownership.in_world && !m_ownership.rml_interaction;
}
bool UiInputRouter::GameplayKeyAllowed(int key) const {
    return m_contexts.GameplayKeyAllowed(key);
}
bool UiInputRouter::GameplayMouseAllowed() const {
    return GameplayAllowed();
}
bool UiInputRouter::CursorCaptured() const {
    return m_contexts.Cursor() == CursorMode::Captured;
}
std::uint64_t UiInputRouter::Revision() const {
    return m_contexts.Revision();
}
} // namespace Luminumbra::Client::App
