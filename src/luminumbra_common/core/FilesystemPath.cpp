#include "FilesystemPath.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
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

std::filesystem::path
FinalWindowsPath(HANDLE handle, bool require_directory, std::error_code& error) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        const DWORD information_failure = GetLastError();
        CloseHandle(handle);
        error = std::error_code(static_cast<int>(information_failure), std::system_category());
        return {};
    }
    if (require_directory && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(handle);
        error = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    // Extended Win32 paths, including the terminator, fit in 32768 wide characters.
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    const DWORD failure = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) {
        error = std::error_code(static_cast<int>(failure), std::system_category());
        return {};
    }
    std::wstring resolved(buffer.data(), length);
    if (resolved.starts_with(LR"(\\?\UNC\)"))
        resolved = LR"(\\)" + resolved.substr(8);
    else if (resolved.starts_with(LR"(\\?\)"))
        resolved.erase(0, 4);
    error.clear();
    return std::filesystem::path(resolved);
}

#endif

} // namespace

std::filesystem::path WeaklyCanonicalPath(const std::filesystem::path& path,
                                          std::error_code& error) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (path.native().size() > 32767) {
        error = std::make_error_code(std::errc::filename_too_long);
        return {};
    }
    auto ancestor = AbsolutePath(path, error);
    if (error)
        return {};
    ancestor = LexicallyNormalPath(ancestor);
    ancestor.make_preferred();
    if (ancestor.native().size() > 32767) {
        error = std::make_error_code(std::errc::filename_too_long);
        return {};
    }
    std::filesystem::path suffix;
    // Each failed lookup removes one component. Win32 paths are bounded to 32767 characters.
    while (!ancestor.empty()) {
        const auto handle = CreateFileW(ancestor.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            const auto canonical = FinalWindowsPath(handle, !suffix.empty(), error);
            if (error)
                return {};
            return suffix.empty() ? canonical : LexicallyNormalPath(canonical / suffix);
        }
        const DWORD failure = GetLastError();
        const auto parent = ancestor.parent_path();
        if ((failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) ||
            parent == ancestor) {
            error = std::error_code(static_cast<int>(failure), std::system_category());
            return {};
        }
        suffix = suffix.empty() ? ancestor.filename() : ancestor.filename() / suffix;
        ancestor = parent;
    }
    error = std::make_error_code(std::errc::no_such_file_or_directory);
    return {};
#else
    auto ancestor = AbsolutePath(path, error);
    if (error)
        return {};
    std::filesystem::path suffix;
    // Each failed lookup removes one path component. Only a missing component may
    // form a suffix: libstdc++ weakly_canonical also accepts ENOTDIR as a miss.
    while (!ancestor.empty()) {
        const auto canonical = std::filesystem::canonical(ancestor, error);
        if (!error) {
            if (!suffix.empty() && !std::filesystem::is_directory(canonical, error)) {
                if (!error)
                    error = std::make_error_code(std::errc::not_a_directory);
                return {};
            }
            if (error)
                return {};
            return suffix.empty() ? canonical : LexicallyNormalPath(canonical / suffix);
        }
        const auto parent = ancestor.parent_path();
        if (error != std::errc::no_such_file_or_directory || parent == ancestor)
            return {};
        suffix = suffix.empty() ? ancestor.filename() : ancestor.filename() / suffix;
        ancestor = parent;
    }
    error = std::make_error_code(std::errc::no_such_file_or_directory);
    return {};
#endif
}

std::filesystem::path WeaklyCanonicalPath(const std::filesystem::path& path) {
    std::error_code error;
    auto resolved = WeaklyCanonicalPath(path, error);
    if (error)
        throw std::filesystem::filesystem_error("resolve content path", path, error);
    return resolved;
}

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

std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string GenericUtf8PathString(const std::filesystem::path& path) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (IsWindowsUncPath(path)) {
        const auto utf8 = path.u8string();
        std::string native(utf8.begin(), utf8.end());
        std::replace(native.begin(), native.end(), '\\', '/');
        return native;
    }
#endif
    const auto utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

} // namespace Luminumbra::Filesystem
