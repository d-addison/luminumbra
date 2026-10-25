#pragma once

#include "FilesystemPath.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <string_view>

namespace Luminumbra::Filesystem {

// Expand Windows short names in an absolute, lexically normalized path without
// resolving links. Retain missing suffixes; return the input unchanged on POSIX.
std::filesystem::path LongNamePath(const std::filesystem::path& path, std::error_code& error);
// Compare components with Windows ordinal case folding, or byte-for-byte on POSIX.
// Separators are compared through path components; this does not resolve aliases.
bool PathSpellingEqual(const std::filesystem::path& left, const std::filesystem::path& right);
// Resolve links through existing ancestors, including NTFS junctions and short names.
// A missing suffix is retained; other lookup failures are reported.
std::filesystem::path WeaklyCanonicalPath(const std::filesystem::path& path,
                                          std::error_code& error);
std::filesystem::path WeaklyCanonicalPath(const std::filesystem::path& path);

inline constexpr std::array kCaptureContentTrees = {
    "data", "res", "config", "game-assets", "assets/audio"};
inline constexpr std::string_view kOptionalCaptureContentTree = "assets/audio";

// Configured once on the host thread before starting capture loaders/workers.
// Only file-load boundaries perform filesystem work; inactive checks are O(1).
// A refusal survives callers that normally recover from missing optional assets.
class CaptureContentPaths {
public:
    void enable(const std::filesystem::path& root);

    bool active() const;
    const std::filesystem::path& root() const;
    bool refused() const;

    bool allows(const std::filesystem::path& path) const;

    // The caller supplies a resolution verified against its owning open handle.
    // Do not resolve that pathname again: it may have been replaced since open.
    // Inventory scans may include the tree root itself; loaders require a child.
    bool allows_resolved(const std::filesystem::path& path,
                         const std::filesystem::path& resolved,
                         bool include_tree_root = false) const;

private:
    static bool within(const std::filesystem::path& path,
                       const std::filesystem::path& tree,
                       bool include_tree_root);

    std::filesystem::path root_;
    std::array<std::filesystem::path, kCaptureContentTrees.size()> trees_{};
    std::array<std::filesystem::path, kCaptureContentTrees.size()> lexical_trees_{};
    mutable std::atomic<bool> refused_{false};
    bool active_ = false;
};

extern CaptureContentPaths capture_content_paths;

} // namespace Luminumbra::Filesystem
