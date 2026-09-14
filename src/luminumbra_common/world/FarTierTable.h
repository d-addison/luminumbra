#pragma once

// Volumetric far-range authority; see docs/distant-world.md. This declaration
// does not yet drive the legacy F1/F2 heightfield scheduler or camera clipping.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Luminumbra::World {

struct FarTierDimensions {
    std::uint32_t sample_spacing_meters;
    std::uint32_t brick_edge_meters;
    std::uint32_t tile_edge_meters;
    std::uint32_t outer_radius_meters;
};

inline constexpr std::size_t kFarTierCount = 5;
inline constexpr std::array<FarTierDimensions, kFarTierCount> kFarTierTable = [] {
    std::array<FarTierDimensions, kFarTierCount> tiers{};
    for (std::size_t index = 0; index < tiers.size(); ++index) {
        const std::uint32_t spacing = 4u << index;
        tiers[index] = {spacing, 4u * spacing, 128u * spacing, 256u * spacing};
    }
    return tiers;
}();

// Tier numbers are one-based: 1 means V1, 5 means V5. Invalid numbers refuse
// with nullopt; they never index outside the table or clamp to a valid tier.
constexpr std::optional<FarTierDimensions> FarTierAt(std::size_t tier_number) {
    if (tier_number == 0 || tier_number > kFarTierCount) {
        return std::nullopt;
    }
    return kFarTierTable[tier_number - 1];
}

inline constexpr std::uint32_t kFarOuterRadiusMeters = kFarTierTable.back().outer_radius_meters;

// Nominal band owner for the horizontal (XZ) camera-to-nearest-tile distance.
// V1 owns [0, V1 radius]; later tiers own (previous radius, own radius].
// Negative, non-finite and beyond-horizon distances have no owner. Actual
// live coverage, arrival fallback and one-brick overlap are separate policies.
constexpr std::optional<std::size_t> FarTierForHorizontalDistance(double distance_meters) {
    if (!(distance_meters >= 0.0 && distance_meters <= kFarOuterRadiusMeters)) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < kFarTierCount; ++index) {
        if (distance_meters <= kFarTierTable[index].outer_radius_meters) {
            return index + 1;
        }
    }
    return std::nullopt;
}

// Integer cross-multiplication proves the exact 1/256 ratio without rounding.
static_assert([] {
    for (const auto& tier : kFarTierTable) {
        if (256u * tier.sample_spacing_meters != tier.outer_radius_meters) {
            return false;
        }
    }
    return true;
}());

static_assert([] {
    for (std::size_t index = 1; index < kFarTierCount; ++index) {
        if (kFarTierTable[index].outer_radius_meters !=
            2u * kFarTierTable[index - 1].outer_radius_meters) {
            return false;
        }
    }
    return true;
}());

static_assert([] {
    for (const auto& tier : kFarTierTable) {
        if (tier.brick_edge_meters == 0 || tier.tile_edge_meters % tier.brick_edge_meters != 0) {
            return false;
        }
    }
    return true;
}());

static_assert(kFarOuterRadiusMeters == 16384u);

} // namespace Luminumbra::World
