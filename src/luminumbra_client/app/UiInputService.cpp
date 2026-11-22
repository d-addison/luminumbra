#include "app/UiInputService.h"

#include <limits>

namespace Luminumbra::Client::App {
std::size_t UiInputService::Find(InputContextId id) const {
    if (id.value != 0) {
        for (std::size_t index = 0; index < kMaxContexts; ++index) {
            if (m_slots[index].id == id)
                return index;
        }
    }
    return kMaxContexts;
}
InputContextId UiInputService::Register(InputContextPolicy policy) {
    if (m_registered == kMaxContexts || m_nextId == std::numeric_limits<std::uint64_t>::max())
        return {};
    for (auto& slot : m_slots) {
        if (slot.id.value == 0) {
            slot = {{m_nextId++}, policy};
            ++m_registered;
            return slot.id;
        }
    }
    return {};
}
void UiInputService::RemoveFromStack(std::size_t slot) {
    for (std::size_t position = 0; position < m_depth; ++position) {
        if (m_stack[position] == slot) {
            for (std::size_t next = position + 1; next < m_depth; ++next)
                m_stack[next - 1] = m_stack[next];
            --m_depth;
            return;
        }
    }
}
bool UiInputService::Unregister(InputContextId id) {
    const auto slot = Find(id);
    if (slot == kMaxContexts)
        return false;
    const bool was_focused = Focused() == id;
    RemoveFromStack(slot);
    m_slots[slot] = {};
    --m_registered;
    if (was_focused)
        ReleaseHeldInput();
    return true;
}
bool UiInputService::Update(InputContextId id, InputContextPolicy policy) {
    const auto slot = Find(id);
    if (slot == kMaxContexts)
        return false;
    if (m_slots[slot].policy != policy) {
        m_slots[slot].policy = policy;
        if (Focused() == id)
            ReleaseHeldInput();
    }
    return true;
}
bool UiInputService::Focus(InputContextId id) {
    const auto slot = Find(id);
    if (slot == kMaxContexts)
        return false;
    if (Focused() == id)
        return true;
    RemoveFromStack(slot);
    m_stack[m_depth++] = slot;
    ReleaseHeldInput();
    return true;
}
bool UiInputService::Back() {
    if (m_depth <= 1)
        return false;
    --m_depth;
    ReleaseHeldInput();
    return true;
}
bool UiInputService::Unfocus(InputContextId id) {
    const auto slot = Find(id);
    if (slot == kMaxContexts)
        return false;
    const auto depth = m_depth;
    const bool was_focused = Focused() == id;
    RemoveFromStack(slot);
    if (was_focused)
        ReleaseHeldInput();
    return depth != m_depth;
}
InputContextId UiInputService::Focused() const {
    return m_depth == 0 ? InputContextId{} : m_slots[m_stack[m_depth - 1]].id;
}
const InputContextPolicy* UiInputService::ActivePolicy() const {
    return m_depth == 0 ? nullptr : &m_slots[m_stack[m_depth - 1]].policy;
}
InputOwner UiInputService::Owner() const {
    const auto* policy = ActivePolicy();
    if (!m_windowFocused || !policy)
        return InputOwner::Unfocused;
    // Even an accidentally permissive text context cannot feed gameplay.
    return policy->text_entry && policy->owner == InputOwner::Gameplay ? InputOwner::Developer
                                                                       : policy->owner;
}
CursorMode UiInputService::Cursor() const {
    const auto* policy = ActivePolicy();
    return m_windowFocused && policy ? policy->cursor : CursorMode::Visible;
}
bool UiInputService::TextEntry() const {
    const auto* policy = ActivePolicy();
    return m_windowFocused && policy && policy->text_entry;
}
std::size_t UiInputService::RegisteredCount() const {
    return m_registered;
}
void UiInputService::SetWindowFocused(bool focused) {
    if (m_windowFocused != focused) {
        m_windowFocused = focused;
        ReleaseHeldInput();
    }
}
void UiInputService::KeyDown(int key) {
    if (key >= 0 && static_cast<std::size_t>(key) < kKeyCount)
        m_held.set(static_cast<std::size_t>(key));
}
void UiInputService::KeyUp(int key) {
    if (key >= 0 && static_cast<std::size_t>(key) < kKeyCount) {
        m_held.reset(static_cast<std::size_t>(key));
        m_quarantined.reset(static_cast<std::size_t>(key));
    }
}
bool UiInputService::GameplayKeyAllowed(int key) const {
    return Owner() == InputOwner::Gameplay && !KeyQuarantined(key);
}
bool UiInputService::KeyQuarantined(int key) const {
    return key < 0 || static_cast<std::size_t>(key) >= kKeyCount ||
           m_quarantined.test(static_cast<std::size_t>(key));
}
void UiInputService::ReleaseHeldInput() {
    m_quarantined |= m_held;
    m_released |= m_held;
    ++m_revision;
}
UiInputService::KeySet UiInputService::TakeReleasedKeys() {
    const auto released = m_released;
    m_released.reset();
    return released;
}
std::uint64_t UiInputService::Revision() const {
    return m_revision;
}
ScopedInputContext::ScopedInputContext(UiInputService& service, InputContextPolicy policy)
    : m_service(service)
    , m_id(service.Register(policy)) {}
ScopedInputContext::~ScopedInputContext() {
    m_service.Unregister(m_id);
}
InputContextId ScopedInputContext::Id() const {
    return m_id;
}
} // namespace Luminumbra::Client::App
