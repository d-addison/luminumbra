#pragma once

#include <filesystem>

namespace Luminumbra::Rendering {

// Derive the companion atlas filename without treating dots in its directory as extensions.
inline std::filesystem::path ImpostorNormalAtlasPath(const std::filesystem::path& atlasPath) {
    auto filename = atlasPath.stem();
    filename += "_normal";
    filename += atlasPath.extension();
    return atlasPath.parent_path() / filename;
}

} // namespace Luminumbra::Rendering
