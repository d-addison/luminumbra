#pragma once

#include "FilesystemPath.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace Luminumbra::Filesystem {

inline constexpr std::array kCaptureContentTrees = {
    "data", "res", "config", "game-assets", "assets/audio"};
inline constexpr std::string_view kOptionalCaptureContentTree = "assets/audio";

// Configured once on the host thread before starting capture loaders/workers.
// Only file-load boundaries perform filesystem work; inactive checks are O(1).
// A refusal survives callers that normally recover from missing optional assets.
class CaptureContentPaths {
public:
    void enable(const std::filesystem::path& root) {
        root_ = LexicallyNormalPath(AbsolutePath(root));
        root_.make_preferred();
        const auto canonical_root = WeaklyCanonicalPath(root_);
        // The shared resolver uses Win32 handles for junctions, short names and UNC roots.
        for (std::size_t index = 0; index < trees_.size(); ++index) {
            const auto tree = (root_ / kCaptureContentTrees[index]).make_preferred();
            const bool absent_audio = kCaptureContentTrees[index] == kOptionalCaptureContentTree &&
                                      !std::filesystem::exists(tree);
            if (std::filesystem::is_symlink(tree) ||
                (!absent_audio && !std::filesystem::is_directory(tree)))
                throw std::runtime_error("unsupported capture content tree");
            trees_[index] = WeaklyCanonicalPath(tree);
            lexical_trees_[index] = tree;
            if (trees_[index] != canonical_root / kCaptureContentTrees[index])
                throw std::runtime_error("redirected capture content tree");
        }
        active_ = true;
    }

    bool active() const {
        return active_;
    }
    const std::filesystem::path& root() const {
        return root_;
    }
    bool refused() const {
        return refused_.load(std::memory_order_relaxed);
    }

    bool allows(const std::filesystem::path& path) const {
        if (!active_)
            return true;
        std::error_code error;
        const auto absolute = AbsolutePath(path, error);
        if (!error) {
            const auto lexical = LexicallyNormalPath(absolute);
            const auto resolved = WeaklyCanonicalPath(absolute, error);
            if (!error) {
                for (std::size_t index = 0; index < trees_.size(); ++index)
                    // Retain the selected root's spelling as well as its
                    // canonical spelling (including Windows short-name aliases).
                    if ((within(lexical, lexical_trees_[index]) ||
                         within(lexical, trees_[index])) &&
                        within(resolved, trees_[index]))
                        return true;
            }
        }
        refused_.store(true, std::memory_order_relaxed);
        return false;
    }

private:
    static bool within(const std::filesystem::path& path, const std::filesystem::path& tree) {
        auto component = path.begin();
        for (const auto& parent : tree) {
            if (component == path.end() || *component != parent)
                return false;
            ++component;
        }
        return component != path.end();
    }

    std::filesystem::path root_;
    std::array<std::filesystem::path, kCaptureContentTrees.size()> trees_{};
    std::array<std::filesystem::path, kCaptureContentTrees.size()> lexical_trees_{};
    mutable std::atomic<bool> refused_{false};
    bool active_ = false;
};

inline CaptureContentPaths capture_content_paths;

} // namespace Luminumbra::Filesystem
