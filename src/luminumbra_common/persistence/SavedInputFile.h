#pragma once

#include "RegionFileWriteGuard.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Luminumbra::Persistence {
#if !defined(_WIN32)
namespace detail {
// Share the POSIX retry rule between payload reads and the final EOF probe.
// The callable is inlined; each attempt performs exactly one read syscall.
template<typename Read>
auto ReadSavedInputRetryingInterrupt(Read&& read, const std::stop_token& stop = {}) {
    for (;;) {
        if (stop.stop_requested())
            return decltype(read()){-1};
        const auto received = read();
        if (received >= 0 || errno != EINTR)
            return received;
    }
}
} // namespace detail
#endif

// Type, length, and bytes come from one open handle. Special files are refusals;
// only a missing entry is a clean miss. Leaf links are refused by default.
class SavedInputFile {
public:
    // Checked rename follows a source leaf to verify its expected target bytes,
    // then retires only the leaf. Saved-world admission always uses Refuse.
    enum class LeafSymlinks {
        Refuse,
        Follow
    };

    explicit SavedInputFile(const std::filesystem::path& path,
                            LeafSymlinks leaf_symlinks = LeafSymlinks::Refuse);

    SavedInputFile(const SavedInputFile&) = delete;
    SavedInputFile& operator=(const SavedInputFile&) = delete;

    ~SavedInputFile();

    // Keep the native object alive without retaining this reader's publication
    // stripe. Prevents native identity reuse during a transaction's replacements.
    [[nodiscard]] std::shared_ptr<const void> retain_identity() const;

    [[nodiscard]] bool missing() const {
        return m_missing;
    }

    // Native object identity from the admitted handle, used by the recovery journal.
    [[nodiscard]] bool identity(std::array<std::uint64_t, 3>& result) const {
        if (!m_location_admitted)
            return false;
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION info{};
        if (m_handle == INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(m_handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) !=
                0 ||
            (info.nFileIndexHigh == 0 && info.nFileIndexLow == 0))
            return false;
        result = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
#else
        struct stat info {};
        if (m_descriptor < 0 || ::fstat(m_descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_ino == 0)
            return false;
        result = {
            static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), 0};
#endif
        return true;
    }

    // Native identity plus size and high-resolution write/change times, without owning a handle.
    using Version = std::array<std::uint64_t, 8>;
#if defined(_WIN32)
    using NativeFile = HANDLE;
#else
    using NativeFile = int;
#endif
    [[nodiscard]] static bool native_version(NativeFile file, Version& result) {
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION identity_info{};
        FILE_BASIC_INFO times{};
        if (file == INVALID_HANDLE_VALUE || ::GetFileType(file) != FILE_TYPE_DISK ||
            !::GetFileInformationByHandle(file, &identity_info) ||
            (identity_info.dwFileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            (identity_info.nFileIndexHigh == 0 && identity_info.nFileIndexLow == 0) ||
            !::GetFileInformationByHandleEx(
                file, FileBasicInfo, &times, static_cast<DWORD>(sizeof(times))))
            return false;
        result = {identity_info.dwVolumeSerialNumber,
                  identity_info.nFileIndexHigh,
                  identity_info.nFileIndexLow,
                  (static_cast<std::uint64_t>(identity_info.nFileSizeHigh) << 32) |
                      identity_info.nFileSizeLow,
                  static_cast<std::uint64_t>(times.LastWriteTime.QuadPart),
                  0,
                  static_cast<std::uint64_t>(times.ChangeTime.QuadPart),
                  0};
#else
        struct stat info {};
        if (file < 0 || ::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_ino == 0 ||
            info.st_size < 0)
            return false;
#if defined(__APPLE__)
        const auto modified = info.st_mtimespec;
        const auto changed = info.st_ctimespec;
#else
        const auto modified = info.st_mtim;
        const auto changed = info.st_ctim;
#endif
        result = {static_cast<std::uint64_t>(info.st_dev),
                  static_cast<std::uint64_t>(info.st_ino),
                  0,
                  static_cast<std::uint64_t>(info.st_size),
                  static_cast<std::uint64_t>(modified.tv_sec),
                  static_cast<std::uint64_t>(modified.tv_nsec),
                  static_cast<std::uint64_t>(changed.tv_sec),
                  static_cast<std::uint64_t>(changed.tv_nsec)};
#endif
        return true;
    }

    [[nodiscard]] bool version(Version& result) const {
        if (!m_location_admitted)
            return false;
#if defined(_WIN32)
        return native_version(m_handle, result);
#else
        return native_version(m_descriptor, result);
#endif
    }

    [[nodiscard]] bool length(std::uint64_t& size) const {
        if (!m_location_admitted)
            return false;
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION information{};
        LARGE_INTEGER length{};
        if (m_handle == INVALID_HANDLE_VALUE || ::GetFileType(m_handle) != FILE_TYPE_DISK ||
            !::GetFileInformationByHandle(m_handle, &information) ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            !::GetFileSizeEx(m_handle, &length) || length.QuadPart < 0)
            return false;
        size = static_cast<std::uint64_t>(length.QuadPart);
#else
        struct stat information {};
        if (m_descriptor < 0 || ::fstat(m_descriptor, &information) != 0 ||
            !S_ISREG(information.st_mode) || information.st_size < 0)
            return false;
        size = static_cast<std::uint64_t>(information.st_size);
#endif
        return true;
    }

    [[nodiscard]] bool
    read(std::string& bytes, std::uint64_t maximum, const std::stop_token& stop = {}) const {
        std::uint64_t size = 0;
        if (stop.stop_requested() || !length(size) || size > maximum || size > bytes.max_size())
            return false;
        // Allocation is bounded by the named compatibility limit supplied by the caller.
        std::string candidate(static_cast<std::size_t>(size), '\0');
        if (!read_exact(candidate.data(), candidate.size(), stop) || !at_end(stop))
            return false;
        if (stop.stop_requested())
            return false;
        bytes = std::move(candidate);
        return true;
    }

    [[nodiscard]] bool
    read_exact(char* destination, std::size_t size, const std::stop_token& stop = {}) const {
        if (!m_location_admitted || stop.stop_requested())
            return false;
        constexpr std::size_t kReadBlockSize = std::size_t{64} * 1024;
        std::size_t offset = 0;
        while (offset < size) {
            if (stop.stop_requested())
                return false;
            const auto count = std::min(kReadBlockSize, size - offset);
#if defined(_WIN32)
            DWORD received = 0;
            if (!::ReadFile(m_handle,
                            destination + offset,
                            static_cast<DWORD>(count),
                            &received,
                            nullptr) ||
                received == 0)
                return false;
#else
            const auto received = detail::ReadSavedInputRetryingInterrupt(
                [&] { return ::read(m_descriptor, destination + offset, count); }, stop);
            if (received <= 0)
                return false;
#endif
            offset += static_cast<std::size_t>(received);
        }
        return true;
    }

    [[nodiscard]] bool seek(std::uint64_t offset) const {
        std::uint64_t size = 0;
        if (!length(size) || offset > size)
            return false;
#if defined(_WIN32)
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        return ::SetFilePointerEx(m_handle, position, nullptr, FILE_BEGIN) != 0;
#else
        return ::lseek(m_descriptor, static_cast<off_t>(offset), SEEK_SET) ==
               static_cast<off_t>(offset);
#endif
    }

    [[nodiscard]] bool rewind() const {
#if defined(_WIN32)
        LARGE_INTEGER origin{};
        return ::SetFilePointerEx(m_handle, origin, nullptr, FILE_BEGIN) != 0;
#else
        return ::lseek(m_descriptor, 0, SEEK_SET) == 0;
#endif
    }

    [[nodiscard]] bool at_end(const std::stop_token& stop = {}) const {
        if (!m_location_admitted || stop.stop_requested())
            return false;
        char trailing = 0;
#if defined(_WIN32)
        DWORD received = 0;
        return ::ReadFile(m_handle, &trailing, 1, &received, nullptr) && received == 0;
#else
        return detail::ReadSavedInputRetryingInterrupt(
                   [&] { return ::read(m_descriptor, &trailing, 1); }, stop) == 0;
#endif
    }

private:
    // Conventional saved-region inputs must resolve within their owning save
    // root. Verification is against the open payload, before any bytes are read.
    bool is_within_save_root() const;

    // Lock before opening; the destructor closes the handle before releasing it.
    detail::FilePublicationReadGuard m_publication;
    std::filesystem::path m_path;
#if defined(_WIN32)
    static bool MissingAncestorsAreDirectories(const std::filesystem::path& path) {
        auto parent = path.parent_path();
        // Only a failed open reaches this walk. No file is subsequently opened
        // based on these attributes; a present non-directory ancestor is invalid.
        while (!parent.empty()) {
            const auto attributes = ::GetFileAttributesW(parent.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
                return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const auto error = ::GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                return false;
            const auto next = parent.parent_path();
            if (next == parent)
                return false;
            parent = next;
        }
        return true;
    }
#endif
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
    bool m_missing = false;
    bool m_location_admitted = true;
};

} // namespace Luminumbra::Persistence
