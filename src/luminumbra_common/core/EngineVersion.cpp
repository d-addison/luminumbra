#include "core/EngineVersion.h"

namespace luminumbra::core {

std::string_view GetEngineVersion() noexcept {
    return kEngineVersion;
}

std::string_view GetEngineGitSha() noexcept {
    return kEngineGitSha;
}

std::string_view GetEngineVersionString() noexcept {
    return kEngineVersionString;
}

} // namespace luminumbra::core
