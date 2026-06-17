#include "ReplicationEndpoint.h"

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

void ReplicationServer::BroadcastSnapshot(std::uint64_t server_tick,
                                          const std::vector<ReplEntityState>& entities) {
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
        // State snapshots are UNRELIABLE: a dropped one is superseded by the next
        // (most-recent-wins). Over Steam this maps to k_nSteamNetworkingSend_Unreliable.
        link.transport->SendFrame(EncodeSnapshot(snap), FrameDelivery::Unreliable);
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
