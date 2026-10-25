#include "FilesystemPath.h"
#include "CaptureContentPaths.h"
#include "FilesystemPathDiagnostics.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

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
thread_local std::size_t long_name_expansion_count = 0;

std::filesystem::path OrdinaryWindowsPath(std::wstring resolved) {
    if (resolved.starts_with(LR"(\\?\UNC\)"))
        resolved = LR"(\\)" + resolved.substr(8);
    else if (resolved.starts_with(LR"(\\?\)"))
        resolved.erase(0, 4);
    return std::filesystem::path(std::move(resolved));
}

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
    error.clear();
    return OrdinaryWindowsPath(std::wstring(buffer.data(), length));
}

#endif

} // namespace

std::size_t LongNameExpansionCount() noexcept {
#if defined(_WIN32) && !defined(__CYGWIN__)
    return long_name_expansion_count;
#else
    return 0;
#endif
}

std::filesystem::path LongNamePath(const std::filesystem::path& path, std::error_code& error) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (path.native().size() > 32767) {
        error = std::make_error_code(std::errc::filename_too_long);
        return {};
    }
    // Only components containing '~' can be 8.3 aliases. Capture policies cache
    // expanded root spellings at enable(); ordinary loads need no expansion I/O.
    // This caches no candidate validation: callers still resolve links on every
    // load, so replacing a previously allowed path with a redirect is detected.
    if (!path.empty() && path.native().find(L'~') == std::wstring::npos) {
        auto spelling = path;
        spelling.make_preferred();
        error.clear();
        return OrdinaryWindowsPath(spelling.native());
    }
    auto ancestor = path;
    ancestor.make_preferred();
    std::filesystem::path suffix;
    // Extended Win32 path bound, including the terminator. Each missing lookup
    // removes one component; every intermediate spelling is bounded to 32767 characters.
    std::array<wchar_t, 32768> buffer{};
    while (!ancestor.empty()) {
        ++long_name_expansion_count;
        const DWORD length =
            GetLongPathNameW(ancestor.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < buffer.size()) {
            const auto expanded = OrdinaryWindowsPath(std::wstring(buffer.data(), length));
            if (!suffix.empty()) {
                const auto separator = expanded.has_filename() ? 1u : 0u;
                const auto remaining = 32767 - expanded.native().size();
                if (suffix.native().size() > remaining ||
                    separator > remaining - suffix.native().size()) {
                    error = std::make_error_code(std::errc::filename_too_long);
                    return {};
                }
            }
            error.clear();
            return suffix.empty() ? expanded : LexicallyNormalPath(expanded / suffix);
        }
        const DWORD failure = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
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
    error.clear();
    return path;
#endif
}

bool PathSpellingEqual(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    auto component = left.begin();
    for (const auto& other : right) {
        if (component == left.end())
            return false;
        const auto& first = component->native();
        const auto& second = other.native();
        constexpr auto max_length = static_cast<std::size_t>(std::numeric_limits<int>::max());
        if (first != second && (first.size() > max_length || second.size() > max_length ||
                                CompareStringOrdinal(first.data(),
                                                     static_cast<int>(first.size()),
                                                     second.data(),
                                                     static_cast<int>(second.size()),
                                                     TRUE) != CSTR_EQUAL))
            return false;
        ++component;
    }
    return component == left.end();
#else
    return left == right;
#endif
}

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

void CaptureContentPaths::enable(const std::filesystem::path& root) {
    root_ = LexicallyNormalPath(AbsolutePath(root));
    root_.make_preferred();
    const auto canonical_root = WeaklyCanonicalPath(root_);
    std::error_code error;
    const auto lexical_root = LongNamePath(root_, error);
    if (error)
        throw std::filesystem::filesystem_error("expand content root names", root_, error);
    // The shared resolver uses Win32 handles for junctions, short names and UNC roots.
    for (std::size_t index = 0; index < trees_.size(); ++index) {
        const auto tree = (root_ / kCaptureContentTrees[index]).make_preferred();
        const bool absent_audio = kCaptureContentTrees[index] == kOptionalCaptureContentTree &&
                                  !std::filesystem::exists(tree);
        if (std::filesystem::is_symlink(tree) ||
            (!absent_audio && !std::filesystem::is_directory(tree)))
            throw std::runtime_error("unsupported capture content tree");
        trees_[index] = WeaklyCanonicalPath(tree);
        lexical_trees_[index] = lexical_root / kCaptureContentTrees[index];
        if (!PathSpellingEqual(trees_[index], canonical_root / kCaptureContentTrees[index]))
            throw std::runtime_error("redirected capture content tree");
    }
    active_ = true;
}

bool CaptureContentPaths::active() const {
    return active_;
}

const std::filesystem::path& CaptureContentPaths::root() const {
    return root_;
}

bool CaptureContentPaths::refused() const {
    return refused_.load(std::memory_order_relaxed);
}

bool CaptureContentPaths::allows(const std::filesystem::path& path) const {
    if (!active_)
        return true;
    std::error_code error;
    const auto resolved = WeaklyCanonicalPath(path, error);
    if (!error)
        return allows_resolved(path, resolved);
    refused_.store(true, std::memory_order_relaxed);
    return false;
}

bool CaptureContentPaths::allows_resolved(const std::filesystem::path& path,
                                          const std::filesystem::path& resolved,
                                          bool include_tree_root) const {
    if (!active_)
        return true;
    std::error_code error;
    const auto absolute = AbsolutePath(path, error);
    if (!error) {
        const auto lexical = LongNamePath(LexicallyNormalPath(absolute), error);
        if (error) {
            refused_.store(true, std::memory_order_relaxed);
            return false;
        }
        for (std::size_t index = 0; index < trees_.size(); ++index)
            // Expand short names on both lexical sides without resolving
            // links: an external redirect into an allowed tree still fails.
            if ((within(lexical, lexical_trees_[index], include_tree_root) ||
                 within(lexical, trees_[index], include_tree_root)) &&
                within(resolved, trees_[index], include_tree_root))
                return true;
    }
    refused_.store(true, std::memory_order_relaxed);
    return false;
}

bool CaptureContentPaths::within(const std::filesystem::path& path,
                                 const std::filesystem::path& tree,
                                 bool include_tree_root) {
    auto component = path.begin();
    for (const auto& parent : tree) {
        if (component == path.end() || !PathSpellingEqual(*component, parent))
            return false;
        ++component;
    }
    return component != path.end() || include_tree_root;
}

CaptureContentPaths capture_content_paths;

} // namespace Luminumbra::Filesystem
