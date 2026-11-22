#pragma once

#include <luminumbra/abi/luminumbra_abi.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace Luminumbra::Mods {

enum class NativePluginResult {
    Loaded,
    Untrusted,
    AlreadyLoaded,
    InvalidHost,
    InvalidPin,
    FileRefused,
    HashMismatch,
    LoadFailed,
    MissingSymbol,
    QueryFailed,
    IncompatibleAbi,
    InitFailed
};

// Explicitly opt-in, session-scoped owner; there are no default engine callers.
// Trust must authorize this exact binary pin. Native mods remain platform locked.
// Loading is one-shot: shutdown stops the context, destruction releases the module.
// Use on the owning thread, after quiescing all callbacks before shutdown/destruction.
class NativePluginLoader {
public:
    NativePluginLoader();
    ~NativePluginLoader();
    NativePluginLoader(const NativePluginLoader&) = delete;
    NativePluginLoader& operator=(const NativePluginLoader&) = delete;
    NativePluginLoader(NativePluginLoader&&) = delete;
    NativePluginLoader& operator=(NativePluginLoader&&) = delete;

    NativePluginResult load(const std::filesystem::path& path,
                            std::string_view expected_sha256,
                            bool trust_granted,
                            const LumHostApi& host);
    void shutdown() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace Luminumbra::Mods
