#include "FilesystemPath.h"

#include <algorithm>
#include <utility>

#if defined(_WIN32) && !defined(__CYGWIN__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Luminumbra::Filesystem {

bool IsWindowsUncPath(const std::filesystem::path& path) noexcept {
#if defined(_WIN32) && !defined(__CYGWIN__)
    const auto& native = path.native();
    const auto separator = [](wchar_t c) {
        return c == L'/' || c == L'\\';
    };
    return native.size() >= 3 && separator(native[0]) && separator(native[1]) &&
           !separator(native[2]);
#else
    (void)path;
    return false;
#endif
}

bool IsAbsolutePath(const std::filesystem::path& path) noexcept {
    return IsWindowsUncPath(path) || path.is_absolute();
}

namespace {

#if defined(_WIN32) && !defined(__CYGWIN__)
std::filesystem::path NormalizeUncPath(const std::filesystem::path& path, std::error_code& error) {
    // GCC 15's absolute(), lexically_normal() and generic_string() can discard
    // the UNC prefix. Native Win32 normalization accepts both slash directions,
    // resolves dot components and preserves the server/share without file I/O.
    // Upstream correction: https://gcc.gnu.org/pipermail/gcc-patches/2025-May/683185.html
    DWORD capacity = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (capacity == 0) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return {};
    }
    std::wstring normalized;
    for (;;) {
        normalized.resize(capacity);
        const DWORD length = GetFullPathNameW(path.c_str(), capacity, normalized.data(), nullptr);
        if (length == 0) {
            error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            return {};
        }
        if (length < capacity) {
            normalized.resize(length);
            error.clear();
            return std::filesystem::path(std::move(normalized));
        }
        capacity = length; // Insufficient-buffer result includes the terminator.
    }
}
#endif

} // namespace

std::filesystem::path AbsolutePath(const std::filesystem::path& path, std::error_code& error) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (IsWindowsUncPath(path)) {
        return NormalizeUncPath(path, error);
    }
#endif
    return std::filesystem::absolute(path, error);
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = AbsolutePath(path, error);
    if (error) {
        throw std::filesystem::filesystem_error("absolute resource path", path, error);
    }
    return absolute;
}

std::filesystem::path LexicallyNormalPath(const std::filesystem::path& path) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (IsWindowsUncPath(path)) {
        return AbsolutePath(path); // A UNC path is already absolute.
    }
#endif
    return path.lexically_normal();
}

std::string GenericPathString(const std::filesystem::path& path) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (IsWindowsUncPath(path)) {
        std::string native = path.string();
        std::replace(native.begin(), native.end(), '\\', '/');
        return native;
    }
#endif
    return path.generic_string();
}

} // namespace Luminumbra::Filesystem
