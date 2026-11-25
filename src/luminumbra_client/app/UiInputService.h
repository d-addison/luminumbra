#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace Luminumbra::Client::App {
enum class InputOwner {
    Gameplay,
    Developer,
    RmlUi,
    Unfocused
};
enum class CursorMode {
    Visible,
    Captured
};
struct InputContextPolicy {
    InputOwner owner = InputOwner::Developer;
    CursorMode cursor = CursorMode::Visible;
    bool text_entry = false;
    bool operator==(const InputContextPolicy&) const = default;
};
struct InputContextId {
    // IDs belong to the service that registered them; zero denotes refusal/absence.
    std::uint64_t value = 0;
    bool operator==(const InputContextId&) const = default;
};

// Main-thread service. No window, renderer or game dependencies.
class UiInputService {
public:
    // Fixed storage bounds context operations and key tracking without allocations.
    static constexpr std::size_t kMaxContexts = 32;
    static constexpr std::size_t kKeyCount = 512;
    using KeySet = std::bitset<kKeyCount>;

    UiInputService() = default;
    UiInputService(const UiInputService&) = delete;
    UiInputService& operator=(const UiInputService&) = delete;
    InputContextId Register(InputContextPolicy policy);
    bool Unregister(InputContextId id);
    bool Update(InputContextId id, InputContextPolicy policy);
    // Focus promotes an existing entry, so repeated activation never duplicates it.
    bool Focus(InputContextId id);
    bool Unfocus(InputContextId id);
    // Back retains the root entry and restores the preceding live context.
    bool Back();
    InputContextId Focused() const;
    InputOwner Owner() const;
    CursorMode Cursor() const;
    bool TextEntry() const;
    std::size_t RegisteredCount() const;
    void SetWindowFocused(bool focused);
    void KeyDown(int key);
    void KeyUp(int key);
    bool KeyQuarantined(int key) const;
    bool GameplayKeyAllowed(int key) const;
    // Hosts clear queued actions on Revision changes; synthetic releases are also available.
    void ReleaseHeldInput();
    KeySet TakeReleasedKeys();
    std::uint64_t Revision() const;

private:
    struct Slot {
        InputContextId id;
        InputContextPolicy policy;
    };
    std::size_t Find(InputContextId id) const;
    void RemoveFromStack(std::size_t slot);
    const InputContextPolicy* ActivePolicy() const;
    std::array<Slot, kMaxContexts> m_slots{};
    std::array<std::size_t, kMaxContexts> m_stack{};
    std::size_t m_depth = 0;
    std::size_t m_registered = 0;
    std::uint64_t m_nextId = 1;
    std::uint64_t m_revision = 0;
    bool m_windowFocused = true;
    KeySet m_held;
    KeySet m_quarantined;
    KeySet m_released;
};

// The service must outlive its scoped registrations. Destruction removes even a
// covered context from navigation; IDs are never reused within a service lifetime.
class ScopedInputContext {
public:
    ScopedInputContext(UiInputService& service, InputContextPolicy policy);
    ~ScopedInputContext();
    ScopedInputContext(const ScopedInputContext&) = delete;
    ScopedInputContext& operator=(const ScopedInputContext&) = delete;
    InputContextId Id() const;

private:
    UiInputService& m_service;
    InputContextId m_id;
};
} // namespace Luminumbra::Client::App
