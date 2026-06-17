#include "ReplicationEndpoint.h"

#include <algorithm>
#include <cmath>
#include <cstdlib> // std::llabs

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
    for (auto& [client_id, link] : m_clients) {
        if (!link.transport) continue;
        SnapshotMsg snap;
        snap.server_tick = server_tick;
        snap.snapshot_seq = link.next_snapshot_seq++;
        snap.acked_usercmd_tick = link.inbound.has_command() ? link.inbound.latest().tick : 0u;

        // T-I6 P3.2: AOI scoping. With a radius set, scope this client's entity set
        // to entities within `radius` of its OWN avatar (entity_id == client_id);
        // the own avatar is always included. Box-cull before the squared compare to
        // keep the int64 distance math overflow-safe at large world coordinates.
        const ReplEntityState* center = nullptr;
        if (m_aoi_radius_mm > 0) {
            for (const ReplEntityState& e : entities) {
                if (e.entity_id == client_id) { center = &e; break; }
            }
        }
        if (center == nullptr) {
            snap.entities = entities; // AOI disabled, or this client has no avatar yet
        } else {
            const std::int64_t r = m_aoi_radius_mm;
            const std::int64_t r2 = r * r;
            for (const ReplEntityState& e : entities) {
                const bool is_self = e.entity_id == client_id;
                const std::int64_t dx = static_cast<std::int64_t>(e.px_mm) - center->px_mm;
                const std::int64_t dy = static_cast<std::int64_t>(e.py_mm) - center->py_mm;
                const std::int64_t dz = static_cast<std::int64_t>(e.pz_mm) - center->pz_mm;
                if (is_self ||
                    (std::llabs(dx) <= r && std::llabs(dy) <= r && std::llabs(dz) <= r &&
                     dx * dx + dy * dy + dz * dz <= r2)) {
                    snap.entities.push_back(e);
                }
            }
        }
        // P6: explicit despawns (e.g. a spent arrow). Passed through to every client
        // (AOI-scoped despawn filtering is a later refinement; the set is tiny).
        snap.removed_ids = removed_ids;
        // State snapshots are UNRELIABLE: a dropped one is superseded by the next
        // (most-recent-wins). Over Steam this maps to k_nSteamNetworkingSend_Unreliable.
        const std::vector<std::uint8_t> frame = EncodeSnapshot(snap);
        m_last_broadcast_total_bytes += frame.size();
        if (frame.size() > m_last_broadcast_max_client_bytes) {
            m_last_broadcast_max_client_bytes = frame.size();
        }
        link.transport->SendFrame(frame, FrameDelivery::Unreliable);
    }
}

void ReplicationServer::PumpInbound() {
    for (auto& [client_id, link] : m_clients) {
        (void)client_id;
        if (!link.transport) continue;
        std::vector<std::uint8_t> frame;
        while (link.transport->TryReceiveFrame(frame)) {
            ReplMessageType type;
            if (!PeekReplMessageType(frame, type)) continue;
            if (type == ReplMessageType::Usercmd) {
                UsercmdMsg cmd;
                if (DecodeUsercmd(frame, cmd)) link.inbound.Receive(cmd);
            } else if (type == ReplMessageType::Ack) {
                AckMsg ack;
                if (DecodeAck(frame, ack)) link.inbound.ApplyAck(ack);
            }
            // Snapshot frames are server->client only; ignore if echoed back.
        }
    }
}

const UsercmdMsg* ReplicationServer::LatestUsercmd(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    if (it == m_clients.end() || !it->second.inbound.has_command()) return nullptr;
    return &it->second.inbound.latest();
}

std::uint32_t ReplicationServer::AckedSnapshotSeq(std::uint32_t client_id) const {
    const auto it = m_clients.find(client_id);
    return it == m_clients.end() ? 0u : it->second.inbound.acked_snapshot_seq();
}

void ReplicationClient::SendUsercmd(const UsercmdMsg& cmd) {
    if (!m_transport) return;
    // Usercmds are UNRELIABLE: newest tick wins on the server (UsercmdReceiver),
    // a dropped one is superseded by the next tick's input.
    m_transport->SendFrame(EncodeUsercmd(cmd), FrameDelivery::Unreliable);
    m_latest_usercmd_tick = cmd.tick;
    m_sent_any_usercmd = true;
}

void SnapshotInterpolator::Push(const SnapshotMsg& snap) {
    // Insert keeping the buffer sorted ascending by server_tick; replace on an
    // equal tick (newest wins for that tick).
    auto it = std::lower_bound(m_buf.begin(), m_buf.end(), snap.server_tick,
                               [](const SnapshotMsg& s, std::uint64_t t) { return s.server_tick < t; });
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
    if (m_buf.empty()) return {};
    // Clamp outside the buffered range (no extrapolation).
    if (tick_time <= static_cast<double>(m_buf.front().server_tick)) return m_buf.front().entities;
    if (tick_time >= static_cast<double>(m_buf.back().server_tick)) return m_buf.back().entities;

    // Find the bracketing pair a.tick <= tick_time < b.tick.
    std::size_t bi = 0;
    while (bi < m_buf.size() && static_cast<double>(m_buf[bi].server_tick) <= tick_time) ++bi;
    const SnapshotMsg& a = m_buf[bi - 1];
    const SnapshotMsg& b = m_buf[bi];
    const double span = static_cast<double>(b.server_tick) - static_cast<double>(a.server_tick);
    const double frac = span > 0.0 ? (tick_time - static_cast<double>(a.server_tick)) / span : 0.0;

    auto lerp_i32 = [frac](std::int32_t lo, std::int32_t hi) {
        return static_cast<std::int32_t>(std::llround(static_cast<double>(lo) +
                                                      frac * (static_cast<double>(hi) - static_cast<double>(lo))));
    };
    auto lerp_i16 = [frac](std::int16_t lo, std::int16_t hi) {
        return static_cast<std::int16_t>(std::llround(static_cast<double>(lo) +
                                                      frac * (static_cast<double>(hi) - static_cast<double>(lo))));
    };

    // Lerp entities present in BOTH; pass through entities only in `b` (newer).
    std::vector<ReplEntityState> out;
    out.reserve(b.entities.size());
    for (const ReplEntityState& be : b.entities) {
        const ReplEntityState* ae = nullptr;
        for (const ReplEntityState& cand : a.entities) {
            if (cand.entity_id == be.entity_id) { ae = &cand; break; }
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
    if (!m_transport) return;
    bool got_new_snapshot = false;
    std::vector<std::uint8_t> frame;
    while (m_transport->TryReceiveFrame(frame)) {
        ReplMessageType type;
        if (!PeekReplMessageType(frame, type)) continue;
        if (type == ReplMessageType::Snapshot) {
            SnapshotMsg snap;
            if (DecodeSnapshot(frame, snap) && m_receiver.Receive(snap)) {
                got_new_snapshot = true;
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
