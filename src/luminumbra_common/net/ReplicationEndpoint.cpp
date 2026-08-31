#include "ReplicationEndpoint.h"

#include <algorithm>
#include <cmath>
#include <cstdlib> // std::llabs
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Luminumbra::Net {

void ReplicationServer::AddClient(std::uint32_t client_id, ILockstepTransport* transport) {
    ClientLink link;
    link.transport = transport;
    m_clients[client_id] = link;
}

void ReplicationServer::RemoveClient(std::uint32_t client_id) {
    m_clients.erase(client_id);
}

std::vector<std::uint32_t> ReplicationServer::PruneDisconnectedClients() {
    std::vector<std::uint32_t> removed;
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        if (it->second.transport && !it->second.transport->IsPeerConnected()) {
            removed.push_back(it->first);
            //  polish: PRUNE-INTO-TICK. Enqueue the leaver's avatar id (id ==
            // client_id) so the very next BroadcastSnapshot folds it into removed_ids
            // and tells surviving clients to despawn the ghost -- in the same tick the
            // disconnect was detected, repeated for unreliable-delivery robustness.
            m_pending_removed_ids[it->first] = kRemovalRepeatBroadcasts;
            it = m_clients.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

void ReplicationServer::BroadcastSnapshot(std::uint64_t server_tick,
                                          const std::vector<ReplEntityState>& entities,
                                          const std::vector<std::uint32_t>& removed_ids) {
    m_last_broadcast_total_bytes = 0;
    m_last_broadcast_max_client_bytes = 0;
    m_disconnected_this_broadcast.clear();
    // hopeless clients the policy decided to drop this broadcast. Collected
    // in-loop (so the broadcast iterator stays valid) and erased after the loop.
    std::vector<std::uint32_t> to_disconnect;

    //  polish: PRUNE-INTO-TICK. Merge the caller's explicit despawns (e.g. a
    // spent arrow) with the pending leaver despawns enqueued by
    // PruneDisconnectedClients, deduped + sorted for a deterministic wire order.
    std::vector<std::uint32_t> merged_removed;
    {
        std::set<std::uint32_t> ids(removed_ids.begin(), removed_ids.end());
        for (const auto& [id, count] : m_pending_removed_ids) {
            if (count > 0)
                ids.insert(id);
        }
        merged_removed.assign(ids.begin(), ids.end()); // std::set -> sorted ascending
    }

    //  polish: CHUNK-INDEX AOI. Bucket every entity by its horizontal (X/Z)
    // streaming chunk ONCE, so each client's scope is a cheap neighbourhood gather
    // instead of an all-entities distance scan. floor-divide handles negative
    // coordinates so chunk boundaries are stable across the origin.
    const bool chunk_aoi = m_aoi_chunk_radius >= 0 && m_aoi_chunk_size_mm > 0;
    auto chunk_idx = [](std::int64_t v, std::int64_t cs) -> std::int64_t {
        return (v >= 0) ? (v / cs) : -((-v + cs - 1) / cs);
    };
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<const ReplEntityState*>> buckets;
    if (chunk_aoi) {
        for (const ReplEntityState& e : entities) {
            const std::pair<std::int64_t, std::int64_t> key{
                chunk_idx(e.px_mm, m_aoi_chunk_size_mm), chunk_idx(e.pz_mm, m_aoi_chunk_size_mm)};
            buckets[key].push_back(&e);
        }
    }

    for (auto& [client_id, link] : m_clients) {
        if (!link.transport)
            continue;

        // OUTBOUND BACKPRESSURE POLICY (default-OFF -> this whole block is
        // skipped and the send path below is byte-identical to the pre-policy code). Assess the
        // client's LIVE queue depth BEFORE producing anything: a backed-up client is escalated
        // (throttle -> keyframe -> disconnect) instead of having another frame piled on. backed_up
        // latches at the high-water mark and clears only once the queue fully drains (hysteresis).
        bool force_keyframe = false;
        if (m_backpressure_policy) {
            const std::uint32_t depth = static_cast<std::uint32_t>(link.outbound.size());
            if (!link.backed_up && depth >= kThrottleHighWaterMark) {
                link.backed_up = true;
                link.over_hwm_streak = 0;
            } else if (link.backed_up && depth <= kBackpressureLowWaterMark) {
                link.backed_up = false;
                link.over_hwm_streak = 0;
            }
            if (link.backed_up) {
                ++link.over_hwm_streak;
                if (link.over_hwm_streak >= kDisconnectDeadlineStreak) {
                    // (3) DISCONNECT the hopeless: never drained past the deadline. Signal the peer
                    // (Close) and mark for server-side removal after the loop. On loopback the
                    // server's own IsPeerConnected would stay true, so the POLICY -- not
                    // PruneDisconnectedClients -- owns this drop.
                    link.transport->Close();
                    to_disconnect.push_back(client_id);
                    continue;
                }
                if (link.over_hwm_streak == kKeyframeResyncStreak) {
                    // (2) DROP TO KEYFRAME: discard the piled (undeliverable) delta backlog -- it
                    // is superseded by a single full resync frame -- and fall through to build that
                    // frame as a FULL snapshot (force_keyframe below).
                    link.dropped_frames += link.outbound.size();
                    link.outbound.clear();
                    force_keyframe = true;
                } else {
                    // (1) THROTTLE: skip producing a new snapshot to space this client's cadence;
                    // still try to drain what is queued. Freezing next_snapshot_seq here (nothing
                    // was sent) is deliberate -- SnapshotAge must not climb on frames we withheld.
                    ++link.throttled_frames;
                    while (!link.outbound.empty() &&
                           link.transport->SendFrame(link.outbound.front(),
                                                     FrameDelivery::Unreliable)) {
                        link.outbound.pop_front();
                    }
                    const std::uint32_t d = static_cast<std::uint32_t>(link.outbound.size());
                    if (d > link.peak_queue_depth)
                        link.peak_queue_depth = d;
                    continue;
                }
            }
        }

        SnapshotMsg snap;
        snap.server_tick = server_tick;
        snap.snapshot_seq = link.next_snapshot_seq++;
        snap.acked_usercmd_tick = link.inbound.has_command() ? link.inbound.latest().tick : 0u;

        // Locate this client's own avatar (entity_id == client_id) -- the AOI centre;
        // it is ALWAYS included even if scoping would exclude it.
        const ReplEntityState* center = nullptr;
        for (const ReplEntityState& e : entities) {
            if (e.entity_id == client_id) {
                center = &e;
                break;
            }
        }

        if (center != nullptr && chunk_aoi) {
            // Gather the (2r+1)^2 chunk neighbourhood of the centre, then sort by
            // entity_id to reproduce the canonical (network-id) wire order.
            const std::int64_t ccx = chunk_idx(center->px_mm, m_aoi_chunk_size_mm);
            const std::int64_t ccz = chunk_idx(center->pz_mm, m_aoi_chunk_size_mm);
            const int r = m_aoi_chunk_radius;
            std::vector<const ReplEntityState*> gathered;
            for (int dx = -r; dx <= r; ++dx) {
                for (int dz = -r; dz <= r; ++dz) {
                    const auto it = buckets.find({ccx + dx, ccz + dz});
                    if (it == buckets.end())
                        continue;
                    for (const ReplEntityState* p : it->second)
                        gathered.push_back(p);
                }
            }
            std::sort(gathered.begin(),
                      gathered.end(),
                      [](const ReplEntityState* a, const ReplEntityState* b) {
                          return a->entity_id < b->entity_id;
                      });
            snap.entities.reserve(gathered.size());
            for (const ReplEntityState* p : gathered)
                snap.entities.push_back(*p);
        } else if (center != nullptr && m_aoi_radius_mm > 0) {
            //  mm-radius AOI. Box-cull before the squared compare to keep
            // the int64 distance math overflow-safe at large world coordinates.
            const std::int64_t rr = m_aoi_radius_mm;
            const std::int64_t r2 = rr * rr;
            for (const ReplEntityState& e : entities) {
                const bool is_self = e.entity_id == client_id;
                const std::int64_t dx = static_cast<std::int64_t>(e.px_mm) - center->px_mm;
                const std::int64_t dy = static_cast<std::int64_t>(e.py_mm) - center->py_mm;
                const std::int64_t dz = static_cast<std::int64_t>(e.pz_mm) - center->pz_mm;
                if (is_self || (std::llabs(dx) <= rr && std::llabs(dy) <= rr &&
                                std::llabs(dz) <= rr && dx * dx + dy * dy + dz * dz <= r2)) {
                    snap.entities.push_back(e);
                }
            }
        } else {
            snap.entities = entities; // AOI disabled, or this client has no avatar yet
        }
        snap.removed_ids = merged_removed;

        //  ack-driven delta-vs-acked compression. `snap` is this client's
        // FULL post-AOI set at this seq. In delta mode we send only what changed since
        // the baseline the client last ACKed (MakeSnapshotDelta auto-derives removed_ids
        // from the baseline diff, so despawns ride along); the FULL set is retained as
        // the next baseline. With no usable acked baseline yet we send a full snapshot
        // (delta_from_seq == 0). Off -> wire-identical to .
        SnapshotMsg outgoing;
        if (force_keyframe) {
            // forced FULL keyframe (delta_from_seq==0) resyncs a persistently-behind
            // client -- a standalone frame it can apply without any baseline. Retain it as the new
            // delta baseline so normal delta-vs-acked resumes once the client catches up.
            outgoing = snap;
            outgoing.delta_from_seq = 0;
            if (m_delta) {
                link.sent_history[snap.snapshot_seq] = snap;
                while (link.sent_history.size() > kServerHistoryCap) {
                    link.sent_history.erase(link.sent_history.begin());
                }
            }
            ++link.forced_keyframes;
        } else if (m_delta) {
            const std::uint32_t acked = link.inbound.acked_snapshot_seq();
            const auto base_it =
                (acked != 0) ? link.sent_history.find(acked) : link.sent_history.end();
            if (base_it != link.sent_history.end()) {
                outgoing = MakeSnapshotDelta(base_it->second, snap); // header from snap
                outgoing.delta_from_seq = acked;
            } else {
                outgoing = snap; // no baseline the client can apply -> full
                outgoing.delta_from_seq = 0;
            }
            link.sent_history[snap.snapshot_seq] = snap;
            for (auto it = link.sent_history.begin(); it != link.sent_history.end();) {
                if (it->first < acked)
                    it = link.sent_history.erase(it);
                else
                    ++it;
            }
            while (link.sent_history.size() > kServerHistoryCap) {
                link.sent_history.erase(link.sent_history.begin());
            }
        } else {
            outgoing = snap; //  full-snapshot mode
        }

        // State snapshots are UNRELIABLE: a dropped one is superseded by the next
        // (most-recent-wins). Over Steam this maps to k_nSteamNetworkingSend_Unreliable.
        // route the produced frame through this client's OUTBOUND queue
        // (the backpressure substrate), then flush as far as the transport accepts. A
        // connected loopback/TCP peer accepts immediately, so the queue drains fully and
        // the wire bytes are byte-identical to the prior direct-send path; a would-block /
        // gone peer leaves frames buffered -> outbound.size is the live queue-depth
        // metric. On overflow drop the OLDEST (unreliable / most-recent-wins) and count it.
        std::vector<std::uint8_t> frame = EncodeSnapshot(outgoing);
        m_last_broadcast_total_bytes += frame.size();
        if (frame.size() > m_last_broadcast_max_client_bytes) {
            m_last_broadcast_max_client_bytes = frame.size();
        }
        link.outbound.push_back(std::move(frame));
        while (link.outbound.size() > kOutboundQueueCap) {
            link.outbound.pop_front();
            ++link.dropped_frames;
        }
        while (!link.outbound.empty() &&
               link.transport->SendFrame(link.outbound.front(), FrameDelivery::Unreliable)) {
            link.outbound.pop_front();
        }
        const std::uint32_t depth = static_cast<std::uint32_t>(link.outbound.size());
        if (depth > link.peak_queue_depth)
            link.peak_queue_depth = depth;
    }

    // Decay the pending despawns: each was just sent to every surviving client this
    // broadcast; drop the repeat count and erase exhausted ids. Repeating across a
    // few unreliable snapshots makes a dropped despawn vanishingly unlikely.
    for (auto it = m_pending_removed_ids.begin(); it != m_pending_removed_ids.end();) {
        if (--it->second <= 0) {
            it = m_pending_removed_ids.erase(it);
        } else {
            ++it;
        }
    }

    // finalize policy disconnects AFTER the decay pass, so a freshly-dropped
    // client's avatar-despawn gets the FULL kRemovalRepeatBroadcasts repeats (not decremented
    // this tick). Removing the client here (not in-loop) kept the broadcast iterator valid; the
    // caller reads DisconnectedClientsLastBroadcast to free the player's server-side state.
    for (std::uint32_t id : to_disconnect) {
        m_pending_removed_ids[id] = kRemovalRepeatBroadcasts;
        m_disconnected_this_broadcast.push_back(id);
        m_clients.erase(id);
    }
}

void ReplicationServer::PumpInbound() {
    for (auto& [client_id, link] : m_clients) {
        (void)client_id;
        if (!link.transport)
            continue;
        std::vector<std::uint8_t> frame;
        while (link.transport->TryReceiveFrame(frame)) {
            ReplMessageType type;
            if (!PeekReplMessageType(frame, type))
                continue;
            if (type == ReplMessageType::Usercmd) {
                UsercmdMsg cmd;
                if (DecodeUsercmd(frame, cmd))
                    link.inbound.Receive(cmd);
            } else if (type == ReplMessageType::Ack) {
                AckMsg ack;
                if (DecodeAck(frame, ack))
                    link.inbound.ApplyAck(ack);
            }
            // Snapshot frames are server->client only; ignore if echoed back.
        }
    }
}

const UsercmdMsg* ReplicationServer::LatestUsercmd(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    if (it == m_clients.end() || !it->second.inbound.has_command())
        return nullptr;
    return &it->second.inbound.latest();
}

std::uint32_t ReplicationServer::AckedSnapshotSeq(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    return it == m_clients.end() ? 0u : it->second.inbound.acked_snapshot_seq();
}

// backpressure + snapshot-aging telemetry. All derived from live
// transport/ack state -- additive, world_hash-neutral.
namespace {
// last_sent_seq (next_snapshot_seq-1) - acked_seq, floored at 0. Shared form so the
// per-client getter and the p95 gather agree exactly.
std::uint32_t SeqAge(std::uint32_t next_snapshot_seq, std::uint32_t acked) {
    const std::uint32_t last_sent = next_snapshot_seq > 0 ? next_snapshot_seq - 1u : 0u;
    return last_sent >= acked ? last_sent - acked : 0u;
}
// Nearest-rank p95 over an unsorted value set (sorts a copy); 0 for an empty set.
std::uint32_t Percentile95U32(std::vector<std::uint32_t> v) {
    if (v.empty())
        return 0u;
    std::sort(v.begin(), v.end());
    const std::size_t idx = (v.size() - 1) * 95 / 100;
    return v[idx];
}
} // namespace

std::uint32_t ReplicationServer::OutboundQueueDepth(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    return it == m_clients.end() ? 0u : static_cast<std::uint32_t>(it->second.outbound.size());
}

std::uint32_t ReplicationServer::SnapshotAge(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    if (it == m_clients.end())
        return 0u;
    return SeqAge(it->second.next_snapshot_seq, it->second.inbound.acked_snapshot_seq());
}

std::uint32_t ReplicationServer::PeakOutboundQueueDepth(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    return it == m_clients.end() ? 0u : it->second.peak_queue_depth;
}

std::uint64_t ReplicationServer::DroppedFrames(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    return it == m_clients.end() ? 0u : it->second.dropped_frames;
}

std::uint64_t ReplicationServer::ThrottledFrames(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    return it == m_clients.end() ? 0u : it->second.throttled_frames;
}

std::uint64_t ReplicationServer::ForcedKeyframes(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    return it == m_clients.end() ? 0u : it->second.forced_keyframes;
}

std::uint32_t ReplicationServer::QueueDepthP95() const {
    std::vector<std::uint32_t> depths;
    depths.reserve(m_clients.size());
    for (const auto& [id, link] : m_clients) {
        (void)id;
        depths.push_back(static_cast<std::uint32_t>(link.outbound.size()));
    }
    return Percentile95U32(std::move(depths));
}

std::uint32_t ReplicationServer::SnapshotAgeP95() const {
    std::vector<std::uint32_t> ages;
    ages.reserve(m_clients.size());
    for (const auto& [id, link] : m_clients) {
        (void)id;
        ages.push_back(SeqAge(link.next_snapshot_seq, link.inbound.acked_snapshot_seq()));
    }
    return Percentile95U32(std::move(ages));
}

void ReplicationClient::SendUsercmd(const UsercmdMsg& cmd) {
    if (!m_transport)
        return;
    // Usercmds are UNRELIABLE: newest tick wins on the server (UsercmdReceiver),
    // a dropped one is superseded by the next tick's input.
    m_transport->SendFrame(EncodeUsercmd(cmd), FrameDelivery::Unreliable);
    m_latest_usercmd_tick = cmd.tick;
    m_sent_any_usercmd = true;
}

void SnapshotInterpolator::Push(const SnapshotMsg& snap) {
    // Insert keeping the buffer sorted ascending by server_tick; replace on an
    // equal tick (newest wins for that tick).
    auto it = std::lower_bound(
        m_buf.begin(), m_buf.end(), snap.server_tick, [](const SnapshotMsg& s, std::uint64_t t) {
            return s.server_tick < t;
        });
    if (it != m_buf.end() && it->server_tick == snap.server_tick) {
        *it = snap;
    } else {
        m_buf.insert(it, snap);
    }
    // Evict the oldest beyond the cap.
    while (m_buf.size() > m_max) {
        m_buf.erase(m_buf.begin());
    }
}

std::vector<ReplEntityState> SnapshotInterpolator::Sample(double tick_time) const {
    if (m_buf.empty())
        return {};
    // Clamp outside the buffered range (no extrapolation).
    if (tick_time <= static_cast<double>(m_buf.front().server_tick))
        return m_buf.front().entities;
    if (tick_time >= static_cast<double>(m_buf.back().server_tick))
        return m_buf.back().entities;

    // Find the bracketing pair a.tick <= tick_time < b.tick.
    std::size_t bi = 0;
    while (bi < m_buf.size() && static_cast<double>(m_buf[bi].server_tick) <= tick_time)
        ++bi;
    const SnapshotMsg& a = m_buf[bi - 1];
    const SnapshotMsg& b = m_buf[bi];
    const double span = static_cast<double>(b.server_tick) - static_cast<double>(a.server_tick);
    const double frac = span > 0.0 ? (tick_time - static_cast<double>(a.server_tick)) / span : 0.0;

    auto lerp_i32 = [frac](std::int32_t lo, std::int32_t hi) {
        return static_cast<std::int32_t>(std::llround(
            static_cast<double>(lo) + frac * (static_cast<double>(hi) - static_cast<double>(lo))));
    };
    auto lerp_i16 = [frac](std::int16_t lo, std::int16_t hi) {
        return static_cast<std::int16_t>(std::llround(
            static_cast<double>(lo) + frac * (static_cast<double>(hi) - static_cast<double>(lo))));
    };

    // Lerp entities present in BOTH; pass through entities only in `b` (newer).
    std::vector<ReplEntityState> out;
    out.reserve(b.entities.size());
    for (const ReplEntityState& be : b.entities) {
        const ReplEntityState* ae = nullptr;
        for (const ReplEntityState& cand : a.entities) {
            if (cand.entity_id == be.entity_id) {
                ae = &cand;
                break;
            }
        }
        if (ae == nullptr) {
            out.push_back(be);
            continue;
        }
        ReplEntityState e = be;
        e.px_mm = lerp_i32(ae->px_mm, be.px_mm);
        e.py_mm = lerp_i32(ae->py_mm, be.py_mm);
        e.pz_mm = lerp_i32(ae->pz_mm, be.pz_mm);
        // NOTE: linear yaw lerp (no shortest-arc wrap); fine for the small per-
        // snapshot deltas at 15-20 Hz, revisit if a wrap glitch ever shows.
        e.yaw_mrad = lerp_i16(ae->yaw_mrad, be.yaw_mrad);
        out.push_back(e);
    }
    return out;
}

void ReplicationClient::PumpInbound() {
    if (!m_transport)
        return;
    bool got_new_snapshot = false;
    std::vector<std::uint8_t> frame;
    while (m_transport->TryReceiveFrame(frame)) {
        ReplMessageType type;
        if (!PeekReplMessageType(frame, type))
            continue;
        if (type == ReplMessageType::Snapshot) {
            //  reconstruct the full set. delta_from_seq==0 is a complete
            // snapshot (use directly); otherwise apply the delta onto the baseline we
            // reconstructed at that seq. A missing baseline (e.g. heavy loss pruned it)
            // means we can't apply this frame -- drop it; the server keeps deltaing
            // against the last-acked baseline, so a later frame re-converges us.
            SnapshotMsg raw;
            if (!DecodeSnapshot(frame, raw))
                continue;
            SnapshotMsg full;
            if (raw.delta_from_seq == 0) {
                full = std::move(raw);
            } else {
                const auto it = m_recon_history.find(raw.delta_from_seq);
                if (it == m_recon_history.end())
                    continue;
                full = ApplySnapshotDelta(it->second, raw);
            }
            if (m_receiver.Receive(full)) {
                got_new_snapshot = true;
                m_recon_history[full.snapshot_seq] = full;
                while (m_recon_history.size() > kClientHistoryCap) {
                    m_recon_history.erase(m_recon_history.begin());
                }
            }
        }
        // Usercmd/Ack are client->server only; ignore if echoed back.
    }
    // Ack the newest snapshot applied (carries the newest usercmd we produced so
    // the server can reconcile). Only bother once we have something to report.
    if (got_new_snapshot || m_sent_any_usercmd) {
        // Acks are UNRELIABLE: the newest ack supersedes (monotonic on the server).
        m_transport->SendFrame(EncodeAck(m_receiver.make_ack(m_latest_usercmd_tick)),
                               FrameDelivery::Unreliable);
    }
}

} // namespace Luminumbra::Net
