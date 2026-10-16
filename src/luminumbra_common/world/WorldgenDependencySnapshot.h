#pragma once

#include "BiomeTable.h"
#include "StructurePlacement.h"

#include <string>
#include <vector>

namespace Luminumbra::World {

// Published only through shared_ptr<const ...> after the loader finishes.
// Empty/rejected objects preserve the exact fallback decisions made at load time;
// generation must never consult the filesystem when this snapshot is supplied.
struct WorldgenDependencySnapshot {
    BiomeTable biome_table;
    std::vector<StructureTemplatePool> structure_pools; // includes rejected pools
    // Same UTF-8-sorted records hashed into the preset identity: logical path,
    // optional exact-byte hash, then absence/acceptance/rejection/fallback markers.
    std::vector<std::vector<std::string>> manifest;
};

} // namespace Luminumbra::World
