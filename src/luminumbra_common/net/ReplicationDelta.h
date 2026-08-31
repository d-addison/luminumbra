#pragma once

//  delta-vs-acked snapshot compression (the explicit next replication slice).
// The server sends a client only what CHANGED since the snapshot that client last ACKed,
// instead of a full entity set every tick -- the bandwidth win that makes 20-32+ players
// affordable (memory multiplayer-scale-testing).
//
// These are PURE functions over SnapshotMsg (no wire-format change): MakeSnapshotDelta
// diffs current vs an acked baseline -> a SnapshotMsg carrying only new/changed entities +
// removed ids; ApplySnapshotDelta reconstructs the full snapshot from (baseline + delta).
// Deltaing only against an ACKED baseline keeps it loss-tolerant: a dropped delta never
// strands the client because the server keeps deltaing against the last-acked set until a
// newer ack arrives. Entities are emitted in canonical entity_id order so encode is stable.

#include "ReplicationProtocol.h"

#include <algorithm>
#include <map>
#include <vector>

namespace Luminumbra::Net {

// Diff `current` against `baseline`: include an entity iff it is NEW or CHANGED; list the
// ids present in `baseline` but absent from `current` as removed. Header fields come from
// `current`. Pure + deterministic (id-ordered).
[[nodiscard]] inline SnapshotMsg MakeSnapshotDelta(const SnapshotMsg& baseline,
                                                   const SnapshotMsg& current) {
    std::map<std::uint32_t, const ReplEntityState*> base;
    for (const ReplEntityState& e : baseline.entities)
        base[e.entity_id] = &e;

    SnapshotMsg delta;
    delta.server_tick = current.server_tick;
    delta.snapshot_seq = current.snapshot_seq;
    delta.acked_usercmd_tick = current.acked_usercmd_tick;

    std::map<std::uint32_t, bool> seen;
    for (const ReplEntityState& e : current.entities) {
        seen[e.entity_id] = true;
        const auto it = base.find(e.entity_id);
        if (it == base.end() || !(*it->second == e)) {
            delta.entities.push_back(e); // new or changed
        }
    }
    for (const ReplEntityState& e : baseline.entities) {
        if (seen.find(e.entity_id) == seen.end())
            delta.removed_ids.push_back(e.entity_id);
    }
    // ALSO carry the caller-stamped despawns on `current` (explicit/pending removals -- e.g. a
    // spent transient or a disconnected client's avatar whose id is not in the acked baseline's
    // replicated set). Without this, delta mode silently drops those removals that full mode
    // delivers, leaving a stale ghost on the client. Union + dedup keeps full==delta parity.
    for (std::uint32_t id : current.removed_ids)
        delta.removed_ids.push_back(id);
    std::sort(delta.entities.begin(),
              delta.entities.end(),
              [](const ReplEntityState& a, const ReplEntityState& b) {
                  return a.entity_id < b.entity_id;
              });
    std::sort(delta.removed_ids.begin(), delta.removed_ids.end());
    delta.removed_ids.erase(std::unique(delta.removed_ids.begin(), delta.removed_ids.end()),
                            delta.removed_ids.end());
    return delta;
}

// Reconstruct the full snapshot from (baseline + delta): keep baseline entities, drop the
// delta's removed ids, then overwrite/insert the delta's entities. Header fields come from
// `delta`. Pure + deterministic (id-ordered output).
[[nodiscard]] inline SnapshotMsg ApplySnapshotDelta(const SnapshotMsg& baseline,
                                                    const SnapshotMsg& delta) {
    std::map<std::uint32_t, ReplEntityState> set;
    for (const ReplEntityState& e : baseline.entities)
        set[e.entity_id] = e;
    for (std::uint32_t id : delta.removed_ids)
        set.erase(id);
    for (const ReplEntityState& e : delta.entities)
        set[e.entity_id] = e;

    SnapshotMsg full;
    full.server_tick = delta.server_tick;
    full.snapshot_seq = delta.snapshot_seq;
    full.acked_usercmd_tick = delta.acked_usercmd_tick;
    // Surface this frame's despawns on the reconstructed snapshot, exactly as full-snapshot mode
    // does (ReplicationEndpoint stamps snap.removed_ids). A TRANSIENT removal -- an id never in
    // the entity set (a spent projectile, a just-joined leaver) -- has no entity to drop, so
    // removed_ids is the ONLY signal the consumer gets; dropping it here would leave a stale
    // ghost and break full==delta parity.
    full.removed_ids = delta.removed_ids;
    full.entities.reserve(set.size());
    for (const auto& [id, e] : set)
        full.entities.push_back(e); // map -> ascending id
    return full;
}

} // namespace Luminumbra::Net
