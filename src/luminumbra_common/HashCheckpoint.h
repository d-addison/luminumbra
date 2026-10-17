#pragma once

#include <cstdint>
#include <string>

namespace Luminumbra {

// Shared immutable value for network and replay checkpoints. Mesh is derived
// state; only world, terrain, water and entity hashes participate in the oracle.
struct HashCheckpoint {
    std::uint64_t tick = 0;
    std::string world_hash;
    std::string terrain;
    std::string water;
    std::string entities;
};

} // namespace Luminumbra
