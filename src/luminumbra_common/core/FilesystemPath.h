#pragma once

#include <filesystem>
#include <string>
#include <string_view>
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
// Expand Windows short names in an absolute, lexically normalized path without
// resolving links. Retain missing suffixes; return the input unchanged on POSIX.
std::filesystem::path LongNamePath(const std::filesystem::path& path, std::error_code& error);
// Compare components with Windows ordinal case folding, or byte-for-byte on POSIX.
// Separators are compared through path components; this does not resolve aliases.
bool PathSpellingEqual(const std::filesystem::path& left, const std::filesystem::path& right);
/// Produces generic separators while retaining both leading UNC separators.
std::string GenericPathString(const std::filesystem::path& path);

// Resolve links through existing ancestors, including NTFS junctions and short names.
// A missing suffix is retained; other lookup failures are reported.
std::filesystem::path WeaklyCanonicalPath(const std::filesystem::path& path,
                                          std::error_code& error);
std::filesystem::path WeaklyCanonicalPath(const std::filesystem::path& path);

// Explicit UTF-8 conversion, independent of the Windows active code page.
std::filesystem::path PathFromUtf8(std::string_view text);
std::string GenericUtf8PathString(const std::filesystem::path& path);

} // namespace Luminumbra::Filesystem
