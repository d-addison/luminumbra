#pragma once

// T-I6 P1 (multiplayer): the server-authoritative PLAYER AVATAR — engine-generic
// sim state for one connected player. A stable per-player id + a kinematic
// transform. The authoritative dedicated server (MULTIPLAYER-BLOCKER-SPEC.md v2)
// owns the avatar list; avatar positions feed the multi-anchor chunk-streaming
// vector (each player streams their own near surface), and the avatars fold
// DETERMINISTICALLY into the ECS entity snapshot / `entities` sub-hash.
//
// Scope split (per the v2 phase plan):
//   * P1 (here): the avatar struct, a deterministic spawn layout, and the
//     entity-snapshot projection. Avatars are kinematic transforms only.
//   * P2: give avatars Jolt physics bodies (server-authoritative collision).
//   * P3: network connections drive the avatar list + per-tick input/movement.
//
// DETERMINISM: the spawn layout is a PURE function of player_id (no RNG, no
// wall-clock). The snapshot quantizes positions to fixed-point millimetres so the
// `entities` sub-hash is robust to float formatting and bit-reproducible run to
// run. RENDER/sim one-way: avatars are NOT in the top-level world_hash
// composition (chunk+wind+weather+aether) — they populate the separate `entities`
// desync sub-hash, which is empty (byte-identical to the pre-P1 headless lane)
// whenever no avatars are present.

#include "../../../include/luminumbra/core/Types.h"
#include "../ecs/EntitySnapshot.h"
#include "../net/ReplicationProtocol.h"

#include <cstdint>
#include <vector>

namespace Luminumbra::World {

struct PlayerAvatar {
    std::uint32_t player_id = 0;
    Vec3 position{0.0f};   // world-space ground anchor
    float facing = 0.0f;   // yaw radians
    Vec3 velocity{0.0f};   // world units / second (kinematic until P2 physics)
};

// Deterministic spawn OFFSET (world XZ, Y left 0) for player N, as a phyllotaxis
// (sunflower) ring around the world spawn so N players fan out reproducibly and
// without overlap. PURE function of player_id. The caller adds the world spawn
// and terrain-clamps Y. player_id 0 -> the spawn point itself (zero offset).
Vec3 DeterministicAvatarSpawnOffset(std::uint32_t player_id);

// Projects a set of avatars into the canonical ECS entity snapshot (one record
// per avatar, entity_id = player_id, a single "PlayerAvatar" component carrying
// the quantized transform). Ordered by the snapshot contract (entity_id
// ascending). An EMPTY avatar list yields an EMPTY snapshot — byte-identical to
// the pre-P1 headless lane, so the default world_hash/entities sub-hash is
// unchanged when no players are connected.
Ecs::EntityRegistrySnapshot BuildAvatarEntitySnapshot(const std::vector<PlayerAvatar>& avatars);

// T-I6 P3.1b: project avatars into the network replication entity set the
// authoritative server broadcasts (ReplicationServer::BroadcastSnapshot). Each
// avatar becomes one ReplEntityState (entity_id = player_id; position quantized
// to mm, facing to milli-radians; flags bit0 = grounded). This is the bridge
// from the SIM avatar list to the WIRE form -- the same positions that feed the
// streaming anchors + the entities sub-hash, now sent to clients.
std::vector<Net::ReplEntityState> BuildAvatarReplStates(const std::vector<PlayerAvatar>& avatars);

} // namespace Luminumbra::World
