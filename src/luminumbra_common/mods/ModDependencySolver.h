#pragma once

#include "ModManifest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace luminumbra::mods {

struct ModPackage {
    ModManifest manifest;
    // Caller-supplied SHA-256 of verified resolved content, including referenced
    // data. Never a digest of the raw mod.json text or an installation path.
    std::array<std::uint8_t, 32> content_hash{};
};

using ModSetIdentity = std::uint64_t;

struct ModSolution {
    // Indices into the supplied package span, in deterministic solver order.
    std::vector<std::size_t> order;
    ModSetIdentity identity = 14695981039346656037ULL;
};

class ModDependencySolver {
public:
    // Required and present optional dependencies constrain order. Missing optional
    // dependencies and absent load hints are ignored. Present optional versions
    // must match. Every declared incompatibility is checked in both directions.
    // Failure preserves out; success clears error. No session or filesystem I/O.
    [[nodiscard]] static bool
    Solve(std::span<const ModPackage> packages, ModSolution& out, std::string& error);
};

} // namespace luminumbra::mods
