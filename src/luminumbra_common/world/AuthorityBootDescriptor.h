#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace Luminumbra::world {

// Complete effective authority settings, complementing the replay header's
// preset/content identity, original seed, tick rate and streaming radii. This
// describes a fresh boot, not a saved world or a mid-session snapshot. Runtime
// paths, workers, telemetry, autosave and test-only streaming caps are excluded.
struct AuthorityBootDescriptor {
    static constexpr std::uint16_t kVersion = 1;
    static constexpr std::size_t kIdentityBytes = 256;
    // Frozen SysKey/SysParam ordinal slots from SystemConfigRegistry.gen.h.
    // New registry slots require an explicit descriptor version, never resizing
    // this wire layout in place. Gates 11/12 select high water/active regions.
    static constexpr std::size_t kSystemGateCount = 25;
    static constexpr std::size_t kSystemParamCount = 46;
    // Includes magic, version, lengths/counts and the maximum identity string.
    static constexpr std::size_t kMaxEncodedBytes =
        6 + 12 + 4 + kIdentityBytes + 2 + kSystemGateCount + 2 + kSystemParamCount * (8 + 3 * 4) +
        1 + 4 + 8 + 4 + 1 + 20 + 8 + 4 + 4;
    static constexpr std::uint32_t kMaxAvatars = 1024;

    std::uint32_t authority_rules_revision = 0;
    std::uint32_t resolved_world_seed = 0;
    std::uint32_t seed_conversion_version = 1;
    std::string system_config_identity;
    std::array<std::uint8_t, kSystemGateCount> system_gates{}; // each exactly 0 or 1
    // Effective SimulationIdentityValues: IEEE binary64 scalars and binary32
    // vec3 components as integer bits, including resolved compiled fallbacks.
    // No native floating-point representation or structure padding is serialized.
    std::array<std::uint64_t, kSystemParamCount> scalar_bits{};
    std::array<std::array<std::uint32_t, 3>, kSystemParamCount> vector_bits{};
    bool finite_hydrology = true;
    std::int32_t weather_rain_scale_mm = 25;
    std::uint64_t region_work_limit = (std::numeric_limits<std::uint64_t>::max)();
    std::uint32_t region_hold_ticks = 30;
    std::uint8_t region_reduced_shift = 1;
    std::uint32_t day_length_ticks = 162000;
    std::uint32_t days_per_year = 8;
    std::uint32_t daylight_numerator = 2;
    std::uint32_t daylight_denominator = 3;
    std::uint32_t lunar_cycle_days = 8;
    std::uint64_t parked_bytes_limit = 256ull * 1024ull * 1024ull;
    std::uint32_t avatar_count = 0;
    bool ecology_roster = false;
    bool planted_roster = false;
    bool moving_anchor = false;
    bool water_smoke = false;

    bool operator==(const AuthorityBootDescriptor&) const = default;
};

// Wire order is declaration order above, preceded by ASCII AUBD and LE u16
// version 1. Integers use their declared width in LE; booleans are u8 0/1;
// identity is u32 length + bytes; gate/scalar arrays have u16 exact counts,
// followed by their elements (vectors share the scalar count). Signed rain uses
// two's complement u32 bits. There are no optional fields or extension bytes.
// Refusal supplies an error and leaves the caller's output unchanged.
bool EncodeAuthorityBootDescriptor(const AuthorityBootDescriptor& descriptor,
                                   std::vector<std::uint8_t>& output,
                                   std::string& error);
bool DecodeAuthorityBootDescriptor(std::span<const std::uint8_t> bytes,
                                   AuthorityBootDescriptor& output,
                                   std::string& error);

} // namespace Luminumbra::world
