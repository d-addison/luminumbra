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

// Shared network/replay localization after a world-hash mismatch. Derived mesh
// has no checkpoint slot and falls through to the world-level diagnostic.
[[nodiscard]] inline const char* CheckpointMismatchSection(const HashCheckpoint& local,
                                                           const HashCheckpoint& peer) {
    if (local.terrain != peer.terrain)
        return "terrain";
    if (local.water != peer.water)
        return "water";
    if (local.entities != peer.entities)
        return "entities";
    return "world_hash";
}

} // namespace Luminumbra
