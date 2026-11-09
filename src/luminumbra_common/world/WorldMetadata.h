#pragma once

#include "luminumbra/core/Types.h"

#include <cstdint>
#include <ctime>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace Luminumbra::world {

struct WorldMetadata {
    std::string name;
    std::string seed;
    std::string worldType;
    std::string worldId;
    std::time_t creationTime = 0;
    Vec3 spawnPoint{};
    // Zero means both seed fields were absent on disk: retain legacy conversion.
    std::uint32_t seedConversionVersion = 0;
    std::uint32_t resolvedSeed = 0;
};

inline constexpr std::uint32_t kSeedConversionVersion = 1;

// Version 0 preserves std::hash; version 1 uses FNV-1a 64 over UTF-8 bytes,
// XORing the high and low 32-bit halves. Both retain std::stoull numeric parsing
// and the random-device path for empty input. Unknown versions throw.
std::uint32_t ResolveWorldSeed(const std::string& seed, std::uint32_t conversion_version);

// Validate the optional seedConversionVersion/resolvedSeed pair without changing
// metadata or resolving an absent legacy seed. An empty result means admission.
std::string ValidateWorldSeedMetadata(const nlohmann::json& metadata);

} // namespace Luminumbra::world
