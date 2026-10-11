#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace Luminumbra::Filesystem {

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

} // namespace Luminumbra::Filesystem
