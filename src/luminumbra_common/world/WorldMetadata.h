#pragma once

#include "luminumbra/core/Types.h"

#include <cstdint>
#include <ctime>
#include <nlohmann/json_fwd.hpp>
#include <optional>
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

#if defined(_WIN32)
// Read a --seed value from the native Windows command line, independently of the
// narrow argv code page. The line is split with the C runtime's argv rules, so the
// indices match the narrow argv. Refuses invalid UTF-16, argument mismatches, and
// UTF-8 output over the saved-world seed limit. Empty seed text remains valid.
std::optional<std::string>
WorldSeedFromWindowsCommandLine(const wchar_t* command_line, int argument_count, int seed_index);
#endif

// Validate the optional seedConversionVersion/resolvedSeed pair without changing
// metadata or resolving an absent legacy seed. An empty result means admission.
std::string ValidateWorldSeedMetadata(const nlohmann::json& metadata);

} // namespace Luminumbra::world
