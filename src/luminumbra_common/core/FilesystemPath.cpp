#include "FilesystemPath.h"
#include "CaptureContentPaths.h"
#include "FilesystemPathDiagnostics.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <algorithm>
#include <array>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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
std::size_t& LongNameExpansionCountStorage() noexcept {
    static thread_local std::size_t count = 0;
    return count;
}

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
    return LongNameExpansionCountStorage();
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
        ++LongNameExpansionCountStorage();
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

void CaptureContentPaths::refuse() const {
    if (active_)
        refused_.store(true, std::memory_order_relaxed);
}

namespace {
class MissingContentFile : public std::runtime_error {
public:
    MissingContentFile()
        : std::runtime_error("optional capture content is absent") {}
};

bool AllowsMissingContent(const std::filesystem::path& path, const CaptureContentPaths& policy) {
    // Only absent components may form a suffix. A dangling link, non-directory,
    // denied lookup or redirect is never an optional miss. The 32767-character
    // path bound also bounds this ancestor walk and its retained suffix.
    auto ancestor = AbsolutePath(path);
    if (ancestor.native().size() > 32767)
        return false;
    std::filesystem::path suffix;
    while (!ancestor.empty()) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(ancestor, error);
        if (status.type() != std::filesystem::file_type::not_found) {
            if (error || !std::filesystem::is_directory(status) || suffix.empty())
                return false;
            const VerifiedFileInput opened(ancestor, VerifiedFileInput::Kind::Directory);
            return policy.allows_resolved(path, opened.resolved_path() / suffix);
        }
        if (error && error != std::errc::no_such_file_or_directory)
            return false;
        const auto parent = ancestor.parent_path();
        if (parent == ancestor)
            return false;
        suffix = suffix.empty() ? ancestor.filename() : ancestor.filename() / suffix;
        ancestor = parent;
    }
    return false;
}
} // namespace

struct VerifiedFileInput::State {
    explicit State(const std::filesystem::path& path, Kind kind = Kind::File) {
        // Match the extended Windows path bound on both platforms; this also
        // bounds the POSIX component walk used to verify the checked resolution.
        if (path.native().size() > 32767)
            throw std::runtime_error("workload path exceeds bounds");
#if defined(_WIN32) && !defined(__CYGWIN__)
        handle_ = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto failure = GetLastError();
            throw std::filesystem::filesystem_error(
                "open workload content",
                path,
                std::error_code(static_cast<int>(failure), std::system_category()));
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle_ == INVALID_HANDLE_VALUE || GetFileType(handle_) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) !=
                (kind == Kind::Directory)) {
            close();
            throw std::runtime_error("unsupported workload save file");
        }
        // Resolve the owning handle, including intermediate junctions and 8.3
        // aliases. The fixed buffer includes the extended-path terminator.
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetFinalPathNameByHandleW(
            handle_, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
        if (length == 0 || length >= buffer.size()) {
            close();
            throw std::runtime_error("cannot resolve opened workload path");
        }
        try {
            std::wstring resolved(buffer.data(), length);
            if (resolved.starts_with(LR"(\\?\UNC\)"))
                resolved = LR"(\\)" + resolved.substr(8);
            else if (resolved.starts_with(LR"(\\?\)"))
                resolved.erase(0, 4);
            resolved_ = std::filesystem::path(std::move(resolved));
        } catch (...) {
            close();
            throw;
        }
        size_ = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
        resolved_ = std::filesystem::canonical(path);
        if (resolved_.native().size() > 32767)
            throw std::runtime_error("workload path exceeds bounds");
        handle_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
        struct stat info {};
        if (handle_ < 0 || ::fstat(handle_, &info) != 0 ||
            (kind == Kind::Directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) ||
            info.st_size < 0) {
            close();
            throw std::runtime_error("unsupported workload save file");
        }
        // A no-follow leaf alone does not cover replaced parent symlinks.
        // Independently anchor every component of the checked canonical path,
        // then compare that descriptor's device/inode with the reader's handle.
        int checked = -1;
        try {
            checked = open_resolved(resolved_);
        } catch (...) {
            close();
            throw;
        }
        struct stat expected {};
        const bool matches = checked >= 0 && ::fstat(checked, &expected) == 0 &&
                             expected.st_dev == info.st_dev && expected.st_ino == info.st_ino;
        if (checked >= 0)
            ::close(checked);
        if (!matches) {
            close();
            throw std::runtime_error("opened workload path changed resolution");
        }
        size_ = static_cast<std::uint64_t>(info.st_size);
#endif
    }
    ~State() {
        close();
    }
    // This spelling is bound to the opened object, never a fresh path lookup.
    const std::filesystem::path& resolved_path() const {
        return resolved_;
    }
    std::uint64_t size() const {
        return size_;
    }
    std::size_t read(std::array<char, 65536>& buffer) {
#if defined(_WIN32) && !defined(__CYGWIN__)
        DWORD count = 0;
        if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            throw std::runtime_error("cannot read workload save");
        return count;
#else
        ssize_t count = 0;
        do {
            count = ::read(handle_, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            throw std::runtime_error("cannot read workload save");
        return static_cast<std::size_t>(count);
#endif
    }

#if !defined(_WIN32) || defined(__CYGWIN__)
    static int open_resolved(const std::filesystem::path& path) {
        // Search-only descriptors avoid requiring directory read permission.
#if defined(O_PATH)
        constexpr int access = O_PATH;
#elif defined(O_SEARCH)
        constexpr int access = O_SEARCH;
#else
        constexpr int access = O_RDONLY;
#endif
        constexpr int directory_flags = access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        int parent = ::open(path.root_path().c_str(), directory_flags);
        if (parent < 0)
            return -1;
        try {
            const auto relative = path.relative_path();
            for (auto component = relative.begin(); component != relative.end(); ++component) {
                auto next = component;
                ++next;
                const int flags = next == relative.end()
                                      ? O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC
                                      : directory_flags;
                const int child = ::openat(parent, component->c_str(), flags);
                ::close(parent);
                parent = child;
                if (parent < 0)
                    return -1;
            }
        } catch (...) {
            ::close(parent);
            throw;
        }
        return parent;
    }
#endif
    void close() {
#if defined(_WIN32) && !defined(__CYGWIN__)
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#else
        if (handle_ >= 0)
            ::close(handle_);
        handle_ = -1;
#endif
    }
#if defined(_WIN32) && !defined(__CYGWIN__)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
    std::uint64_t size_ = 0;
    std::filesystem::path resolved_;
};

VerifiedFileInput::VerifiedFileInput(const std::filesystem::path& path,
                                     Kind kind,
                                     const CaptureContentPaths* policy,
                                     MissingContent missing) {
    try {
        state_ = std::make_unique<State>(path, kind);
        if (policy &&
            !policy->allows_resolved(path, state_->resolved_path(), kind == Kind::Directory))
            throw std::runtime_error("capture content reference escapes inventoried trees");
    } catch (const std::filesystem::filesystem_error& error) {
        bool optional_absence = false;
        try {
            optional_absence = policy && kind == Kind::File &&
                               missing == MissingContent::Optional &&
                               error.code() == std::errc::no_such_file_or_directory &&
                               AllowsMissingContent(path, *policy);
        } catch (...) {
            if (policy)
                policy->refuse();
            throw;
        }
        if (optional_absence)
            throw MissingContentFile();
        if (policy)
            policy->refuse();
        throw;
    } catch (...) {
        if (policy)
            policy->refuse();
        throw;
    }
}
VerifiedFileInput::~VerifiedFileInput() = default;
const std::filesystem::path& VerifiedFileInput::resolved_path() const {
    return state_->resolved_path();
}
std::uint64_t VerifiedFileInput::size() const {
    return state_->size();
}
std::size_t VerifiedFileInput::read(std::array<char, 65536>& buffer) {
    return state_->read(buffer);
}
bool VerifiedFileInput::seek(std::uint64_t offset) {
    if (offset > size())
        return false;
#if defined(_WIN32) && !defined(__CYGWIN__)
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(state_->handle_, distance, nullptr, FILE_BEGIN) != 0;
#else
    return ::lseek(state_->handle_, static_cast<off_t>(offset), SEEK_SET) >= 0;
#endif
}

bool CaptureContentPaths::read_file(const std::filesystem::path& path,
                                    std::string& bytes,
                                    std::size_t max_bytes,
                                    MissingContent missing) const {
    try {
        VerifiedFileInput input(path, VerifiedFileInput::Kind::File, this, missing);
        std::string candidate;
        if (input.size() > max_bytes || input.size() > candidate.max_size())
            throw std::runtime_error("capture content exceeds byte budget");
        // Allocation and total copied bytes are bounded by the owning size,
        // the caller's byte budget and string::max_size, checked before reserve.
        candidate.reserve(static_cast<std::size_t>(input.size()));
        std::array<char, 65536> buffer{}; // At most 64 KiB per handle read.
        for (;;) {
            const auto count = input.read(buffer);
            if (count == 0)
                break;
            if (count > input.size() - candidate.size())
                throw std::runtime_error("capture content grew while reading");
            candidate.append(buffer.data(), count);
        }
        if (candidate.size() != input.size())
            throw std::runtime_error("capture content truncated while reading");
        bytes.swap(candidate);
        return true;
    } catch (const MissingContentFile&) {
        return false;
    } catch (...) {
        refuse();
        return false;
    }
}

namespace {
class VerifiedContentBuffer : public std::streambuf {
public:
    explicit VerifiedContentBuffer(const std::filesystem::path& path)
        : input_(path,
                 VerifiedFileInput::Kind::File,
                 &capture_content_paths(),
                 MissingContent::Optional) {}
    std::uint64_t size() const {
        return input_.size();
    }

protected:
    int_type underflow() override {
        try {
            const auto count = input_.read(buffer_);
            if (count > input_.size() - consumed_ || (count == 0 && consumed_ != input_.size()))
                throw std::runtime_error("capture content changed while reading");
            consumed_ += count;
            setg(buffer_.data(), buffer_.data(), buffer_.data() + count);
            return count == 0 ? traits_type::eof() : traits_type::to_int_type(*gptr());
        } catch (...) {
            capture_content_paths().refuse();
            throw;
        }
    }
    pos_type seekoff(off_type offset, std::ios::seekdir origin, std::ios::openmode mode) override {
        if ((mode & std::ios::in) == 0)
            return pos_type(off_type(-1));
        const auto buffered = gptr() ? static_cast<std::uint64_t>(egptr() - gptr()) : 0;
        const auto base = origin == std::ios::beg   ? 0
                          : origin == std::ios::cur ? consumed_ - buffered
                                                    : input_.size();
        // Avoid negating the minimum signed offset or overflowing addition.
        const auto distance = offset < 0 ? static_cast<std::uint64_t>(-(offset + 1)) + 1
                                         : static_cast<std::uint64_t>(offset);
        if ((offset < 0 && distance > base) || (offset >= 0 && distance > input_.size() - base))
            return pos_type(off_type(-1));
        const auto target = offset < 0 ? base - distance : base + distance;
        if (target > static_cast<std::uint64_t>(std::numeric_limits<off_type>::max()) ||
            !input_.seek(target))
            return pos_type(off_type(-1));
        consumed_ = target;
        setg(nullptr, nullptr, nullptr);
        return pos_type(static_cast<off_type>(target));
    }
    pos_type seekpos(pos_type position, std::ios::openmode mode) override {
        return seekoff(static_cast<off_type>(position), std::ios::beg, mode);
    }

private:
    VerifiedFileInput input_;
    std::uint64_t consumed_ = 0;
    std::array<char, 65536> buffer_{}; // Fixed 64 KiB; independent of asset size.
};
} // namespace

struct ContentInput::State {
    std::ifstream ordinary;
    std::unique_ptr<VerifiedContentBuffer> verified;
    std::streambuf* buffer() {
        if (verified)
            return verified.get();
        return ordinary.rdbuf();
    }
};
ContentInput::ContentInput()
    : std::istream(nullptr) {}
ContentInput::ContentInput(const std::filesystem::path& path, std::ios::openmode mode)
    : std::istream(nullptr)
    , state_(std::make_unique<State>()) {
    if (!capture_content_paths().active()) {
        state_->ordinary.open(path, mode);
        rdbuf(state_->ordinary.rdbuf());
        clear(state_->ordinary.rdstate());
        return;
    }
    try {
        state_->verified = std::make_unique<VerifiedContentBuffer>(path);
        rdbuf(state_->verified.get());
        if ((mode & std::ios::ate) != 0)
            seekg(0, std::ios::end);
    } catch (const MissingContentFile&) {
        setstate(std::ios::failbit);
    } catch (...) {
        capture_content_paths().refuse();
        setstate(std::ios::failbit);
    }
}
ContentInput::ContentInput(ContentInput&& other) noexcept
    : ContentInput() {
    std::istream::swap(other);
    state_.swap(other.state_);
    set_rdbuf(state_ ? state_->buffer() : nullptr);
    other.set_rdbuf(nullptr);
}
ContentInput::~ContentInput() = default;
bool ContentInput::is_open() const {
    return state_ && (state_->verified || state_->ordinary.is_open());
}
std::uint64_t ContentInput::verified_size() const {
    return state_ && state_->verified ? state_->verified->size() : 0;
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

CaptureContentPaths& capture_content_paths() {
    static CaptureContentPaths paths;
    return paths;
}

} // namespace Luminumbra::Filesystem
