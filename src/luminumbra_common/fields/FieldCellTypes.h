#pragma once
#include "../world/ActiveRegionLedger.h"
namespace Luminumbra::world {
struct FieldPageKey {
    std::int32_t x = 0;
    std::int32_t z = 0;
    auto operator<=>(const FieldPageKey&) const = default;
};

struct ScentCellKey {
    std::int32_t x = 0, z = 0; // Existing scent geometry: one-metre world cells.
    auto operator<=>(const ScentCellKey&) const = default;
};
} // namespace Luminumbra::world
