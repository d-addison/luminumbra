#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace Luminumbra::Core {

// Returns a process-environment value with ownership independent of the C
// runtime's environment storage. MSVC deprecates direct getenv access, while
// other supported toolchains provide it as the portable process API.
inline std::optional<std::string> ReadEnvironment(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
        std::free(value);
        return std::nullopt;
    }
    std::string owned(value);
    std::free(value);
    return owned;
#else
    if (const char* value = std::getenv(name)) {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

} // namespace Luminumbra::Core
