#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Luminumbra::StationDiscoveryCli {

#ifdef _WIN32
using ArgumentChar = wchar_t;
#else
using ArgumentChar = char;
#endif
using ArgumentView = std::basic_string_view<ArgumentChar>;

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Paths retain the native argv spelling, including UTF-16 on Windows and
// arbitrary non-NUL bytes on POSIX. Never pass them through ArgumentText.
inline std::filesystem::path ArgumentPath(ArgumentView text) {
    return std::filesystem::path(text);
}

// Windows CLI switches, kinds and integers are ASCII; only paths accept Unicode.
// POSIX keeps the original narrow argv bytes and parsing behaviour.
inline std::string ArgumentText(ArgumentView text) {
#ifdef _WIN32
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        if (static_cast<unsigned int>(character) > 127U) {
            throw UsageError("expected an ASCII switch, kind or integer");
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
#else
    return std::string(text);
#endif
}

} // namespace Luminumbra::StationDiscoveryCli
