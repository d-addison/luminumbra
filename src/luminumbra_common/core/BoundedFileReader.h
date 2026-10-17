#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <istream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <streambuf>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// A prior non-lean Windows include can already have pulled in winspool.h.
// Do not let its ANSI/Unicode alias rename Jolt's job-system methods.
#ifdef AddJob
#undef AddJob
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Luminumbra::Filesystem {

// Open without following the final symlink/reparse point. Verify type and length
// on the same handle used for every read; a FIFO must never block during open.
class RegularFileHandle {
public:
    explicit RegularFileHandle(const std::filesystem::path& path) {
#ifdef _WIN32
        m_handle = CreateFileW(path.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (m_handle == INVALID_HANDLE_VALUE || GetFileType(m_handle) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(m_handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            Close();
            throw std::runtime_error("cannot open regular save file: " + path.string());
        }
        m_size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
#else
        m_handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat info {};
        if (m_handle < 0 || ::fstat(m_handle, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0) {
            Close();
            throw std::runtime_error("cannot open regular save file: " + path.string());
        }
        m_size = static_cast<std::uint64_t>(info.st_size);
#endif
    }
    ~RegularFileHandle() {
        Close();
    }
    RegularFileHandle(const RegularFileHandle&) = delete;
    RegularFileHandle& operator=(const RegularFileHandle&) = delete;
    std::uint64_t size() const {
        return m_size;
    }
    std::size_t read(char* data, std::size_t count) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(m_handle, data, static_cast<DWORD>(count), &got, nullptr))
            throw std::runtime_error("unreadable save file");
        return got;
#else
        ssize_t got = 0;
        do {
            got = ::read(m_handle, data, count);
        } while (got < 0 && errno == EINTR);
        if (got < 0)
            throw std::runtime_error("unreadable save file");
        return static_cast<std::size_t>(got);
#endif
    }

private:
    void Close() {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_handle >= 0) {
            ::close(m_handle);
            m_handle = -1;
        }
#endif
    }
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_handle = -1;
#endif
    std::uint64_t m_size = 0;
};

// The stream owns a fixed 64 KiB buffer. Check size through this same opened handle
// before any content allocation; reads stay within that size plus one EOF probe byte.
class BoundedFileInput : private std::streambuf {
public:
    BoundedFileInput(const std::filesystem::path& path,
                     std::size_t limit,
                     std::stop_token stop = {})
        : m_file(path)
        , m_stop(std::move(stop))
        , m_input(this) {
        m_input.exceptions(std::ios::badbit);
        if (m_stop.stop_requested())
            throw std::runtime_error("Saved-world validation cancelled.");
        const auto length = m_file.size();
        if (length > limit || length > std::string{}.max_size() ||
            length > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            throw std::runtime_error("save file exceeds size cap: " + path.string());
        m_size = static_cast<std::size_t>(length);
    }

    std::istream& stream() {
        return m_input;
    }
    std::size_t size() const {
        return m_size;
    }
    std::size_t bytes_read() const {
        return m_read;
    }

protected:
    int_type underflow() override {
        if (gptr() != egptr())
            return traits_type::to_int_type(*gptr());
        if (m_stop.stop_requested())
            throw std::runtime_error("Saved-world validation cancelled.");
        if (m_read == m_size) {
            char probe = 0;
            if (m_file.read(&probe, 1) != 0)
                throw std::runtime_error("save file grew or read failed");
            return traits_type::eof();
        }
        const auto count = std::min(m_buffer.size(), m_size - m_read);
        std::size_t received = 0;
        while (received < count) {
            const auto got = m_file.read(m_buffer.data() + received, count - received);
            m_read += got;
            if (got == 0)
                throw std::runtime_error("short or unreadable save file");
            received += got;
        }
        setg(m_buffer.data(), m_buffer.data(), m_buffer.data() + count);
        return traits_type::to_int_type(*gptr());
    }

private:
    RegularFileHandle m_file;
    std::stop_token m_stop;
    std::istream m_input;
    std::array<char, 65536> m_buffer{};
    std::size_t m_size = 0;
    std::size_t m_read = 0;
};

// Read only the size checked through the opened file. Refuse short reads and growth.
inline bool ReadBoundedFile(const std::filesystem::path& path,
                            std::string& bytes,
                            std::size_t limit,
                            const std::stop_token& stop = {}) {
    bytes.clear();
    BoundedFileInput input(path, limit, stop);
    bytes.resize(input.size());
    auto& stream = input.stream();
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("cannot read complete save file");
    return true;
}

} // namespace Luminumbra::Filesystem
