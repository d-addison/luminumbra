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
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Luminumbra::Net {

namespace ReplicationDeltaDetail {
struct IndexedEntity {
    const ReplEntityState* entity;
    std::size_t ordinal;
};

[[nodiscard]] inline std::vector<IndexedEntity>
LastEntitiesById(const std::vector<ReplEntityState>& entities) {
    std::vector<IndexedEntity> index;
    // Scratch is bounded by the participating input records, never the world set.
    index.reserve(entities.size());
    for (std::size_t ordinal = 0; ordinal < entities.size(); ++ordinal)
        index.push_back({&entities[ordinal], ordinal});
    std::sort(index.begin(), index.end(), [](const IndexedEntity& a, const IndexedEntity& b) {
        if (a.entity->entity_id != b.entity->entity_id)
            return a.entity->entity_id < b.entity->entity_id;
        return a.ordinal > b.ordinal; // keep the last input record for each id
    });
    index.erase(std::unique(index.begin(),
                            index.end(),
                            [](const IndexedEntity& a, const IndexedEntity& b) {
                                return a.entity->entity_id == b.entity->entity_id;
                            }),
                index.end());
    return index;
}

[[nodiscard]] inline auto FindEntity(const std::vector<IndexedEntity>& index, std::uint32_t id) {
    return std::lower_bound(
        index.begin(), index.end(), id, [](const IndexedEntity& entry, std::uint32_t entity_id) {
            return entry.entity->entity_id < entity_id;
        });
}
} // namespace ReplicationDeltaDetail

// Diff `current` against `baseline`: include an entity iff it is NEW or CHANGED; list the
// ids present in `baseline` but absent from `current` as removed. Header fields come from
// `current`. Pure + deterministic (id-ordered).
[[nodiscard]] inline SnapshotMsg MakeSnapshotDelta(const SnapshotMsg& baseline,
                                                   const SnapshotMsg& current) {
    const auto base = ReplicationDeltaDetail::LastEntitiesById(baseline.entities);

    SnapshotMsg delta;
    delta.server_tick = current.server_tick;
    delta.snapshot_seq = current.snapshot_seq;
    delta.acked_usercmd_tick = current.acked_usercmd_tick;
    const auto changed = [&base](const ReplEntityState& entity) {
        const auto it = ReplicationDeltaDetail::FindEntity(base, entity.entity_id);
        return it == base.end() || it->entity->entity_id != entity.entity_id ||
               !(*it->entity == entity);
    };
    std::vector<std::uint32_t> seen;
    // Membership scratch holds one id per current input record.
    seen.reserve(current.entities.size());
    std::size_t changed_count = 0;
    for (const ReplEntityState& e : current.entities) {
        seen.push_back(e.entity_id);
        if (changed(e))
            ++changed_count;
    }
    // Output storage is bounded by emitted records, not the full input snapshots.
    delta.entities.reserve(changed_count);
    for (const ReplEntityState& e : current.entities) {
        if (changed(e)) {
            delta.entities.push_back(e); // new or changed
        }
    }
    std::sort(seen.begin(), seen.end());
    std::size_t removed_count = 0;
    for (const ReplEntityState& e : baseline.entities) {
        if (!std::binary_search(seen.begin(), seen.end(), e.entity_id))
            ++removed_count;
    }
    // Include explicit removals before deduplication; check the sum before allocating.
    if (removed_count > delta.removed_ids.max_size() ||
        current.removed_ids.size() > delta.removed_ids.max_size() - removed_count)
        throw std::length_error("snapshot removal count exceeds capacity");
    delta.removed_ids.reserve(removed_count + current.removed_ids.size());
    for (const ReplEntityState& e : baseline.entities) {
        if (!std::binary_search(seen.begin(), seen.end(), e.entity_id))
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
// Bound reconstructed state as well as individual delta frames. Scratch indices
// are bounded by their input record counts; retained state stays within one frame.
[[nodiscard]] inline std::optional<SnapshotMsg> TryApplySnapshotDelta(const SnapshotMsg& baseline,
                                                                      const SnapshotMsg& delta,
                                                                      std::size_t max_frame_bytes) {
    const auto base_bytes = SnapshotEncodedSize(baseline.entities.size(), 0);
    const auto overhead = SnapshotEncodedSize(0, delta.removed_ids.size());
    if (!base_bytes || *base_bytes > max_frame_bytes || !overhead || *overhead > max_frame_bytes)
        return std::nullopt;
    const auto entity_limit = (max_frame_bytes - *overhead) / 23u;
    auto base = ReplicationDeltaDetail::LastEntitiesById(baseline.entities);
    auto removed = delta.removed_ids;
    std::sort(removed.begin(), removed.end());
    base.erase(std::remove_if(base.begin(),
                              base.end(),
                              [&removed](const ReplicationDeltaDetail::IndexedEntity& entry) {
                                  return std::binary_search(
                                      removed.begin(), removed.end(), entry.entity->entity_id);
                              }),
               base.end());
    if (base.size() > entity_limit)
        return std::nullopt;
    const auto updates = ReplicationDeltaDetail::LastEntitiesById(delta.entities);
    std::size_t entity_count = base.size();
    for (const auto& entry : updates) {
        const auto existing = ReplicationDeltaDetail::FindEntity(base, entry.entity->entity_id);
        if (existing == base.end() || existing->entity->entity_id != entry.entity->entity_id) {
            // Check before incrementing or reserving, including at SIZE_MAX.
            if (entity_count >= entity_limit)
                return std::nullopt;
            ++entity_count;
        }
    }

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
    full.entities.reserve(entity_count);
    auto old = base.begin();
    auto update = updates.begin();
    while (old != base.end() || update != updates.end()) {
        if (update == updates.end() ||
            (old != base.end() && old->entity->entity_id < update->entity->entity_id)) {
            full.entities.push_back(*old->entity);
            ++old;
        } else {
            if (old != base.end() && old->entity->entity_id == update->entity->entity_id)
                ++old;
            full.entities.push_back(*update->entity);
            ++update;
        }
    }
    return full;
}

// In-memory callers retain the original pure reconstruction contract. Network
// admission uses TryApplySnapshotDelta with the transport's complete-frame cap.
[[nodiscard]] inline SnapshotMsg ApplySnapshotDelta(const SnapshotMsg& baseline,
                                                    const SnapshotMsg& delta) {
    return TryApplySnapshotDelta(baseline, delta, (std::numeric_limits<std::size_t>::max)())
        .value_or(SnapshotMsg{});
}

} // namespace Luminumbra::Net
