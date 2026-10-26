#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
// Type, length, and bytes come from one open handle. Leaf symlinks and special
// files are refusals; only a missing entry is a clean miss.
class SavedInputFile {
public:
    explicit SavedInputFile(const std::filesystem::path& path) {
#if defined(_WIN32)
        m_handle = ::CreateFileW(path.c_str(),
                                 GENERIC_READ,
                                 FILE_SHARE_READ,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                 nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError();
            m_missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
#else
        // NONBLOCK prevents a FIFO open from waiting for a writer; NOFOLLOW
        // refuses even dangling symlinks rather than mistaking them for absence.
        m_descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        m_missing = m_descriptor < 0 && errno == ENOENT;
#endif
    }

    SavedInputFile(const SavedInputFile&) = delete;
    SavedInputFile& operator=(const SavedInputFile&) = delete;

    ~SavedInputFile() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(m_handle);
#else
        if (m_descriptor >= 0)
            ::close(m_descriptor);
#endif
    }

    [[nodiscard]] bool missing() const {
        return m_missing;
    }

    [[nodiscard]] bool length(std::uint64_t& size) const {
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

    [[nodiscard]] bool read(std::string& bytes, std::uint64_t maximum) const {
        std::uint64_t size = 0;
        if (!length(size) || size > maximum || size > bytes.max_size())
            return false;
        // Allocation is bounded by the named compatibility limit supplied by the caller.
        std::string candidate(static_cast<std::size_t>(size), '\0');
        if (!read_exact(candidate.data(), candidate.size()) || !at_end())
            return false;
        bytes = std::move(candidate);
        return true;
    }

    [[nodiscard]] bool read_exact(char* destination, std::size_t size) const {
        constexpr std::size_t kReadBlockSize = std::size_t{64} * 1024;
        std::size_t offset = 0;
        while (offset < size) {
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
            const auto received = ::read(m_descriptor, destination + offset, count);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0)
                return false;
#endif
            offset += static_cast<std::size_t>(received);
        }
        return true;
    }

    [[nodiscard]] bool rewind() const {
#if defined(_WIN32)
        LARGE_INTEGER origin{};
        return ::SetFilePointerEx(m_handle, origin, nullptr, FILE_BEGIN) != 0;
#else
        return ::lseek(m_descriptor, 0, SEEK_SET) == 0;
#endif
    }

    [[nodiscard]] bool at_end() const {
        char trailing = 0;
#if defined(_WIN32)
        DWORD received = 0;
        return ::ReadFile(m_handle, &trailing, 1, &received, nullptr) && received == 0;
#else
        return ::read(m_descriptor, &trailing, 1) == 0;
#endif
    }

private:
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
    bool m_missing = false;
};

} // namespace Luminumbra::Persistence
