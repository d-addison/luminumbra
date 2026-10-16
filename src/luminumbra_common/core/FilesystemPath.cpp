#include "FilesystemPath.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <stdio.h>
#endif

namespace Luminumbra::Filesystem {

ReplacementSafeInputFile::~ReplacementSafeInputFile() {
    close();
}
ReplacementSafeInputFile::ReplacementSafeInputFile(ReplacementSafeInputFile&& other) noexcept
    : m_file(std::exchange(other.m_file, nullptr)) {}
ReplacementSafeInputFile&
ReplacementSafeInputFile::operator=(ReplacementSafeInputFile&& other) noexcept {
    if (this != &other) {
        close();
        m_file = std::exchange(other.m_file, nullptr);
    }
    return *this;
}
bool ReplacementSafeInputFile::open(const std::filesystem::path& path) {
    close();
#if defined(_WIN32) && !defined(__CYGWIN__)
    const HANDLE handle = CreateFileW(path.c_str(),
                                      GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const int descriptor =
        _open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        CloseHandle(handle);
        return false;
    }
    m_file = _fdopen(descriptor, "rb");
    if (!m_file)
        _close(descriptor);
#else
    m_file = std::fopen(path.c_str(), "rb");
#endif
    return is_open();
}
void ReplacementSafeInputFile::close() {
    if (m_file)
        std::fclose(std::exchange(m_file, nullptr));
}
std::uint64_t ReplacementSafeInputFile::size(std::error_code& error) const {
    error.clear();
    if (!m_file) {
        error = std::make_error_code(std::errc::bad_file_descriptor);
        return 0;
    }
#if defined(_WIN32) && !defined(__CYGWIN__)
    struct _stat64 info {};
    const int result = _fstat64(_fileno(m_file), &info);
#else
    struct stat info {};
    const int result = fstat(fileno(m_file), &info);
#endif
    if (result != 0 || info.st_size < 0) {
        error = std::error_code(result != 0 ? errno : EINVAL, std::generic_category());
        return 0;
    }
    return static_cast<std::uint64_t>(info.st_size);
}
bool ReplacementSafeInputFile::seek(std::uint64_t offset) {
    if (!m_file)
        return false;
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<__int64>::max()))
        return false;
    return _fseeki64(m_file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return false;
    return fseeko(m_file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
bool ReplacementSafeInputFile::read(char* destination, std::size_t bytes) {
    return m_file && std::fread(destination, 1, bytes, m_file) == bytes;
}

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
