#pragma once

#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/PlayerAvatar.h"
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace luminumbra::fields {
class EnergyFieldState;
}
namespace luminumbra::simulation {
class OrderedEventBus;
}
namespace Luminumbra::world {
class GameSession;
}
namespace Luminumbra::Server {

struct HeavyReferenceDecodeLimits {
    static constexpr std::size_t kIndexBytes = 16u * 1024u * 1024u;
    static constexpr std::size_t kRecordBytes = 64u * 1024u * 1024u;
    static constexpr std::size_t kRegionRecords = 65'535;
    static constexpr std::size_t kMembers = 1'048'576;
    static constexpr std::size_t kJsonDepth = 32;
    static constexpr std::size_t kCollection = 1'048'576;
    static constexpr std::size_t kJsonNodes = 2'097'152;
    static constexpr std::uint64_t kRegionBytes = 1024ull * 1024ull * 1024ull;
    static constexpr std::uint64_t kRetainedChunkBytes = 512ull * 1024ull * 1024ull;
};

// Offline, owner-thread observations. No production hash or canonical projection is an input.
struct HeavyReferenceInputs {
    std::filesystem::path directory;
    struct ChunkInventory {
        std::vector<std::shared_ptr<const Chunk>> live;
        std::vector<std::shared_ptr<const Chunk>> parked;
        std::vector<IVec3> durable;
    };
    ChunkInventory chunks;
    const entt::registry& registry;
    const world::WorldClock& clock;
    const world::ActiveRegionLedger& ledger;
    const world::GroundObjectStore& ground;
    const world::FieldPageState& fields;
    const luminumbra::fields::EnergyFieldState* energy = nullptr;
    const luminumbra::simulation::OrderedEventBus* events = nullptr;
    std::span<const World::PlayerAvatar> avatars = {};
    Vec3 ambient_anchor{};
};

struct HeavyAuthorityReference {
    // Raw, independently framed fields, not checksums. Ordered domain names also localize loss.
    std::map<std::string, std::string> domains;
    bool operator==(const HeavyAuthorityReference&) const = default;
    [[nodiscard]] std::string Difference(const HeavyAuthorityReference& other) const;
    [[nodiscard]] static HeavyAuthorityReference Capture(const HeavyReferenceInputs& inputs);
    [[nodiscard]] static HeavyAuthorityReference
    Capture(const world::GameSession& session, std::span<const World::PlayerAvatar> avatars = {});
};
} // namespace Luminumbra::Server
