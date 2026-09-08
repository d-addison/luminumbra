#include "app/RuntimeRoot.h"
#include "luminumbra_common/core/FilesystemPath.h"

#include <system_error>
#include <vector>

namespace Luminumbra::Client::App {

namespace {

bool HasRuntimeAssets(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path / "res/shaders/g_buffer.vert", ec) &&
           std::filesystem::exists(path / "data/audio/music.bank.json", ec) &&
           std::filesystem::exists(path / "worlds/atlas/presets/default.json", ec);
}

void AddAncestorCandidates(std::vector<std::filesystem::path>& candidates,
                           std::filesystem::path path) {
    std::error_code ec;
    path = Filesystem::AbsolutePath(path, ec);
    if (path.empty()) {
        return;
    }

    while (!path.empty()) {
        candidates.push_back(path);
        const std::filesystem::path parent = Filesystem::IsWindowsUncPath(path)
                                                 ? Filesystem::LexicallyNormalPath(path / "..")
                                                 : path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
}

} // namespace

std::filesystem::path ResolveRuntimeRoot(const char* argv0) {
    std::vector<std::filesystem::path> candidates;

    std::error_code ec;
    AddAncestorCandidates(candidates, std::filesystem::current_path(ec));
    if (argv0 && argv0[0] != '\0') {
        AddAncestorCandidates(candidates, std::filesystem::path(argv0).parent_path());
    }

    for (const std::filesystem::path& candidate : candidates) {
        if (HasRuntimeAssets(candidate)) {
            // The UNC candidate is already normalized by Win32. Older MinGW
            // weakly_canonical() loses its share root; keep the usable path.
            if (Filesystem::IsWindowsUncPath(candidate)) {
                return candidate;
            }
            std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate : canonical;
        }
    }

    return std::filesystem::current_path(ec);
}

std::string RuntimeRootString(const std::filesystem::path& root_dir) {
    std::string root_path = Filesystem::GenericPathString(root_dir);
    if (!root_path.empty() && root_path.back() != '/') {
        root_path.push_back('/');
    }
    return root_path;
}

} // namespace Luminumbra::Client::App
