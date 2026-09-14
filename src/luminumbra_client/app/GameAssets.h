#pragma once

#include <filesystem>
#include <string>

namespace Luminumbra::Client::App {

inline constexpr char kTreePackDirectory[] = "game-assets/tree-small-02/1.0.0";
inline constexpr char kTreeMeshPrefix[] =
    "game-assets/tree-small-02/1.0.0/data/models/trees/tree_small_02_";

// Application content policy. Engine and headless startup do not require this pack.
// Returns an actionable setup error, or an empty string after verifying every file.
std::string VerifyGameAssets(const std::filesystem::path& root);

// Streaming SHA-256 used to verify the acquisition manifest's exact file identities.
std::string AssetFileSha256(const std::filesystem::path& path);

} // namespace Luminumbra::Client::App
