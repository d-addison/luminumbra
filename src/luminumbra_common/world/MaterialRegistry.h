#pragma once

#include "../../../include/luminumbra/core/Types.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace Luminumbra::World {

// Name-to-id bridge for byte-sized biome palettes. IDs are supplied explicitly;
// registration order never assigns or reassigns them. This does not activate
// custom materials in simulation or provide a persistent save palette.
class MaterialRegistry {
public:
    MaterialRegistry();

    // Custom keys have exactly two nonempty parts separated by ':', using
    // lowercase ASCII letters, digits, '_', '-' or '.'. At most 128 bytes.
    // IDs 0..7 are reserved. Duplicate names/IDs and IDs above 255 are refused
    // without changing the registry. A registered ID cannot be recycled.
    [[nodiscard]] bool Register(std::string_view name, u32 id);
    [[nodiscard]] std::optional<u8> Resolve(std::string_view name) const;

private:
    // Fixed bound: all 256 IDs representable by BiomeSurfacePalette's u8 slots.
    std::array<std::string, 256> m_names;
};

} // namespace Luminumbra::World
