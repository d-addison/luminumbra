#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Luminumbra {

inline constexpr std::string_view kWaterCursorHashLabel = "|waterSimCursor:v1:";

// The water checkpoint is a legacy 16-character chunk checksum, optionally
// followed by the labelled 16-character cursor checksum. Keep the components
// separate inside the existing string slot so old readers can still compare it.
inline std::string_view WaterHashDifferenceSection(std::string_view a, std::string_view b) {
    const auto known_shape = [](std::string_view value) {
        return value.size() == 16 ||
               (value.size() == 32 + kWaterCursorHashLabel.size() &&
                value.substr(16, kWaterCursorHashLabel.size()) == kWaterCursorHashLabel);
    };
    if (known_shape(a) && known_shape(b) && a != b && a.substr(0, 16) == b.substr(0, 16))
        return "water/cursor";
    return "water";
}

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
        return WaterHashDifferenceSection(local.water, peer.water).data();
    if (local.entities != peer.entities)
        return "entities";
    return "world_hash";
}

} // namespace Luminumbra
