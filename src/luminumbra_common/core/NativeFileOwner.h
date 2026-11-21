#pragma once

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
#include <unistd.h>
#endif

namespace Luminumbra::Filesystem {

// Adopt without allocating, including while a containing object is being constructed.
// Move into shared storage only after its allocation succeeds.
class NativeFileOwner {
public:
#if defined(_WIN32)
    using NativeFile = HANDLE;
#else
    using NativeFile = int;
#endif

    explicit NativeFileOwner(NativeFile file) noexcept
        : m_file(file) {}
    ~NativeFileOwner() {
#if defined(_WIN32)
        if (m_file != INVALID_HANDLE_VALUE && m_file != nullptr)
            ::CloseHandle(m_file);
#else
        if (m_file >= 0)
            ::close(m_file);
#endif
    }
    NativeFileOwner(const NativeFileOwner&) = delete;
    NativeFileOwner& operator=(const NativeFileOwner&) = delete;
    NativeFileOwner(NativeFileOwner&& other) noexcept
        : m_file(other.release()) {}
    NativeFileOwner& operator=(NativeFileOwner&&) = delete;

    [[nodiscard]] NativeFile get() const noexcept {
        return m_file;
    }
    NativeFile release() noexcept {
#if defined(_WIN32)
        return std::exchange(m_file, INVALID_HANDLE_VALUE);
#else
        return std::exchange(m_file, -1);
#endif
    }

private:
    NativeFile m_file;
};

} // namespace Luminumbra::Filesystem
