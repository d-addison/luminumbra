#pragma once

#include "../core/ResidencyContract.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace luminumbra::mods {

// Startup-only input limits; checked before parsing or allocating graph storage.
inline constexpr std::size_t kMaxManifestBytes = 65536;
inline constexpr std::size_t kMaxModRelations = 128;
inline constexpr std::size_t kMaxModCount = 1024;

enum class ModTier {
    ClientCosmetic,
    SimData,
    SimScript,
    SimNative,
    ClientNative
};

struct ModDependency {
    std::string id;
    std::string version;
    bool optional = false;
};

struct ModManifest {
    std::string id;
    std::string version;
    ModTier tier = ModTier::SimData;
    std::vector<ModDependency> dependencies;
    std::vector<ModDependency> incompatible_with;
    std::vector<std::string> load_after;
    std::vector<std::string> load_before;

    [[nodiscard]] core::ResidencyClass residency() const noexcept;
    // Also validates programmatically constructed manifests. Success clears error.
    [[nodiscard]] bool Validate(std::string& error) const;
    // Schema v1 subset: id, version, tier, dependencies, incompatible_with,
    // load_order.after/before. Other fields are ignored. Failure preserves out.
    [[nodiscard]] static bool Parse(std::string_view json, ModManifest& out, std::string& error);
};

// Strict SemVer 2.0 versions (uint64 components). Ranges support *, full versions,
// =, <, <=, >, >=, ^, ~, whitespace conjunctions and || alternatives.
// Prereleases require a comparator naming the same prerelease core in that clause.
// Unsupported or malformed range syntax refuses, including partial versions.
[[nodiscard]] bool ModVersionMatches(std::string_view version, std::string_view range);

} // namespace luminumbra::mods
