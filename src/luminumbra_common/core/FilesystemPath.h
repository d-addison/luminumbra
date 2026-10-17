#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace Luminumbra::Filesystem {

// An opened regular file image; directories and devices are refused without
// reading, and FIFOs are opened without blocking. Windows readers share deletion so atomic
// replacement can publish a new image while existing readers finish the old one.
// POSIX readers have the same semantics through their open descriptor.
class ReplacementSafeInputFile {
public:
    ReplacementSafeInputFile() = default;
    ~ReplacementSafeInputFile();
    ReplacementSafeInputFile(const ReplacementSafeInputFile&) = delete;
    ReplacementSafeInputFile& operator=(const ReplacementSafeInputFile&) = delete;
    ReplacementSafeInputFile(ReplacementSafeInputFile&& other) noexcept;
    ReplacementSafeInputFile& operator=(ReplacementSafeInputFile&& other) noexcept;
    bool open(const std::filesystem::path& path);
    void close();
    [[nodiscard]] bool is_open() const {
        return m_file != nullptr;
    }
    [[nodiscard]] std::uint64_t size(std::error_code& error) const;
    bool seek(std::uint64_t offset);
    bool read(char* destination, std::size_t bytes);

private:
    std::FILE* m_file = nullptr;
};

// MinGW libstdc++ before GCC 16 does not recognize UNC root names. Keep the
// native path intact rather than converting a network share to the current drive.
/// Recognizes native Windows network/device prefixes; always false on POSIX.
bool IsWindowsUncPath(const std::filesystem::path& path) noexcept;
/// Includes UNC paths that older MinGW path::is_absolute() misclassifies.
bool IsAbsolutePath(const std::filesystem::path& path) noexcept;
/// Resolves absolute spelling without discarding a Windows UNC root.
std::filesystem::path AbsolutePath(const std::filesystem::path& path, std::error_code& error);
/// Throwing counterpart of AbsolutePath(path, error).
std::filesystem::path AbsolutePath(const std::filesystem::path& path);
/// Normalizes dot/separator components without resolving filesystem links.
std::filesystem::path LexicallyNormalPath(const std::filesystem::path& path);
/// Produces generic separators while retaining both leading UNC separators.
std::string GenericPathString(const std::filesystem::path& path);

// Explicit UTF-8 conversion, independent of the Windows active code page.
std::filesystem::path PathFromUtf8(std::string_view text);
std::string GenericUtf8PathString(const std::filesystem::path& path);

} // namespace Luminumbra::Filesystem
