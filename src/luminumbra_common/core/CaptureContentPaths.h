#pragma once

#include "FilesystemPath.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <string>
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

    // Path-only guard for enumeration/reference discovery; never authorizes a read.
    bool allows(const std::filesystem::path& path) const;
    // Record an open/read refusal even when an optional loader recovers.
    void refuse() const;
    // Read only through a verified owning handle. Output is unchanged on failure.
    bool
    read_file(const std::filesystem::path& path, std::string& bytes, std::size_t max_bytes) const;

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

// The same no-follow reader is used for content hashes, saves and capture loaders.
// Resolution is bound to the owning handle (anchored device/inode on POSIX,
// reparse rejection and final-path-from-handle on Windows). No path is reopened
// to consume bytes. Paths/component walks are bounded to 32767 native characters.
class VerifiedFileInput {
public:
    enum class Kind : std::uint8_t {
        File,
        Directory
    };
    explicit VerifiedFileInput(const std::filesystem::path& path,
                               Kind kind = Kind::File,
                               const CaptureContentPaths* policy = nullptr);
    VerifiedFileInput(const VerifiedFileInput&) = delete;
    VerifiedFileInput& operator=(const VerifiedFileInput&) = delete;
    ~VerifiedFileInput();
    const std::filesystem::path& resolved_path() const;
    std::uint64_t size() const;
    std::size_t read(std::array<char, 65536>& buffer);
    bool seek(std::uint64_t offset);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Inactive: the original ifstream, mode and filebuf, without policy syscalls.
// Active: a seekable stream over the verified handle, with at most 64 KiB buffered.
// Open/read failures set the policy's sticky refusal flag and the stream state.
class ContentInput : public std::istream {
public:
    ContentInput();
    explicit ContentInput(const std::filesystem::path& path,
                          std::ios::openmode mode = std::ios::in);
    ContentInput(ContentInput&& other) noexcept;
    ~ContentInput() override;
    bool is_open() const;
    // Active only: size recorded from the owning handle, without a pathname stat.
    std::uint64_t verified_size() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Process-wide policy; configure on the host thread before starting workers.
CaptureContentPaths& capture_content_paths();

} // namespace Luminumbra::Filesystem
