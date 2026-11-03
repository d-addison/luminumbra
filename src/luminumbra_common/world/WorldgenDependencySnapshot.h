#pragma once

#include "BiomeTable.h"
#include "StructurePlacement.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace luminumbra::ai {
class CreatureSpeciesRegistry;
}

namespace Luminumbra::World {

// Encoding validation only: preserve all Unicode scalar values and their spelling.
// Call before inserting a logical path into the manifest or loading its species.
inline bool IsValidDependencyPathUtf8(std::string_view text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto lead = static_cast<unsigned char>(text[offset++]);
        if (lead <= 0x7fu)
            continue;
        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            continuation_count = 1;
            codepoint = lead & 0x1fu;
            minimum = 0x80u;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            continuation_count = 2;
            codepoint = lead & 0x0fu;
            minimum = 0x800u;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            continuation_count = 3;
            codepoint = lead & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (continuation_count > text.size() - offset)
            return false;
        for (std::size_t index = 0; index < continuation_count; ++index) {
            const auto continuation = static_cast<unsigned char>(text[offset++]);
            if ((continuation & 0xc0u) != 0x80u)
                return false;
            codepoint = (codepoint << 6u) | (continuation & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu))
            return false;
    }
    return true;
}

// Published only through shared_ptr<const ...> after the loader finishes.
// Empty/rejected objects preserve the exact fallback decisions made at load time;
// generation must never consult the filesystem when this snapshot is supplied.
struct WorldgenDependencySnapshot {
    BiomeTable biome_table;
    std::vector<StructureTemplatePool> structure_pools; // includes rejected pools
    // Session content, captured by the same reader/manifest before world adoption.
    std::shared_ptr<const luminumbra::ai::CreatureSpeciesRegistry> species_table;
    // Same UTF-8-sorted records hashed into the preset identity: logical path,
    // optional exact-byte hash, then absence/acceptance/rejection/fallback markers.
    std::vector<std::vector<std::string>> manifest;
};

} // namespace Luminumbra::World
