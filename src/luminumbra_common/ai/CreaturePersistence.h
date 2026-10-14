#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include "world/ActiveRegionLedger.h"
#include "world/GroundObjectStore.h"

namespace Luminumbra::Ecs {
struct EntityRegistrySnapshot;
}

namespace luminumbra::ai {

inline constexpr std::size_t kMaxCreatures = 16384;
// Preserve the legacy payload budget; only controller stamping earns extra bytes.
inline constexpr std::size_t kMaxCreatureBytes = 64u * 1024u * 1024u;
// The largest compact stamp uses false (one byte longer than true), plus its comma.
inline constexpr std::size_t kCreatureControllerStampBytes =
    sizeof(R"(,"Controller":{"data":{"physics_controlled":false},"version":1})") - 1;
// Callers must first bound count by kMaxCreatures. Only present compact metadata earns credit.
inline constexpr std::size_t CreatureByteLimit(std::size_t count, std::size_t controller_bytes) {
    const auto allowance = count * kCreatureControllerStampBytes;
    return kMaxCreatureBytes + (controller_bytes < allowance ? controller_bytes : allowance);
}
inline constexpr std::size_t kMaxStampedCreatureBytes =
    CreatureByteLimit(kMaxCreatures, (kMaxCreatures * kCreatureControllerStampBytes));
inline constexpr std::size_t kMaxCreatureString = 1024;
inline constexpr std::size_t kMaxCreatureVector = 256;
// Objects use the vector collection budget, which covers the complete component inventory.
inline constexpr std::size_t kMaxCreatureObjectKeys = kMaxCreatureVector;
inline constexpr std::size_t kMaxCreatureNodes = 4u * 1024u * 1024u;
// Controller member key, two objects, three field keys, version and participation values.
inline constexpr std::size_t kCreatureControllerStampNodes = 8;
inline constexpr std::size_t CreatureNodeLimit(std::size_t count, std::size_t controller_nodes) {
    const auto allowance = count * kCreatureControllerStampNodes;
    return kMaxCreatureNodes + (controller_nodes < allowance ? controller_nodes : allowance);
}
// A u32 rate shifted by 32 or more is zero: ring 31 is the last useful ring.
inline constexpr int kMaxCreatureEmitterRadius = 31;
inline constexpr const char* kCreatureSchema = "luminumbra.creature_entities.v1";

struct CreatureRecord {
    std::uint64_t id = 0;
    // Sorted names; each value is {version: 1, data: {...}}. Presence is authoritative.
    nlohmann::json components = nlohmann::json::object();
};
struct CreatureSnapshot {
    std::vector<CreatureRecord> entities;
};

// Throws on unadmitted entities, unsupported references or invalid component payloads.
CreatureSnapshot BuildCreatureSnapshot(const entt::registry& registry);
std::string SerializeCreatureSnapshot(const CreatureSnapshot& snapshot);
// Strict, bounded, transactional decode; out changes only on success.
bool LoadCreatureSnapshot(std::string_view bytes, CreatureSnapshot& out, std::string& error);
bool ValidateCreatureSnapshot(const CreatureSnapshot& snapshot,
                              const Luminumbra::world::ActiveRegionLedger& ledger,
                              const Luminumbra::world::GroundObjectStore& ground,
                              const Luminumbra::Ecs::EntityRegistrySnapshot& plants,
                              std::string& error);
// Decodes and resolves all refusal-producing input in scratch state before destination mutation.
// Durable plant targets must already be present in registry. No runtime avatars are created.
void ApplyCreatureSnapshot(entt::registry& registry, const CreatureSnapshot& snapshot);
std::string CreatureRegionProjection(const entt::registry& registry,
                                     Luminumbra::world::RegionKey key);

} // namespace luminumbra::ai
