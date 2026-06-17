#pragma once

// T-I6 P3.1: authoritative-server replication ENDPOINTS. Wires the P3.0 wire
// protocol (Usercmd / Snapshot / Ack) + the P3.0b reliability layer
// (SnapshotReceiver / UsercmdReceiver) onto the engine's ILockstepTransport seam
// (LoopbackTransport for tests; the deferred UDP socket transport drops in
// unchanged). This is the server<->client message loop, independent of the sim:
//
//   ReplicationServer  — one per dedicated server. Holds a per-client link
//     (transport + inbound UsercmdReceiver + outbound snapshot seq). Builds and
//     broadcasts a SnapshotMsg from an authoritative entity-state set the sim
//     supplies, and drains inbound Usercmd/Ack (most-recent-wins per client).
//
//   ReplicationClient  — one per connected player. Sends its per-tick Usercmd
//     upstream and applies inbound snapshots most-recent-wins (SnapshotReceiver),
//     auto-acking the newest. Exposes the current authoritative snapshot for the
//     renderer to interpolate remote avatars from (P3.3).
//
// Engine-generic + world_hash-neutral: this is render/transport-side glue; the
// authoritative sim state is supplied to BroadcastSnapshot and consumed from
// LatestUsercmd by the caller (the server tick / the client input+render).

#include <cstdint>
#include <map>
#include <vector>

#include "LockstepSession.h"        // ILockstepTransport
#include "ReplicationProtocol.h"

namespace Luminumbra::Net {

class ReplicationServer {
public:
    // Registers a connected client on its transport. client_id is the player id.
    void AddClient(std::uint32_t client_id, ILockstepTransport* transport);
    void RemoveClient(std::uint32_t client_id);
    [[nodiscard]] std::size_t client_count() const { return m_clients.size(); }

    // Builds ONE SnapshotMsg from the authoritative entity set and sends it to
    // every connected client (each gets its own monotonically increasing seq).
    // P3.1 sends the full set; P3.2 will scope `entities` per client (AOI) and
    // P3.1b will delta against each client's acked seq. acked_usercmd_tick per
    // client is taken from that client's newest received usercmd.
    void BroadcastSnapshot(std::uint64_t server_tick, const std::vector<ReplEntityState>& entities);

    // Drains all currently-available inbound frames from every client: Usercmd
    // (newest-wins) and Ack (monotonic). Non-blocking.
    void PumpInbound();

    // The newest usercmd received from a client (for the sim to apply), or null.
    [[nodiscard]] const UsercmdMsg* LatestUsercmd(std::uint32_t client_id) const;
    [[nodiscard]] std::uint32_t AckedSnapshotSeq(std::uint32_t client_id) const;

private:
    struct ClientLink {
        ILockstepTransport* transport = nullptr;
        UsercmdReceiver inbound;
        std::uint32_t next_snapshot_seq = 1;
    };
    std::map<std::uint32_t, ClientLink> m_clients; // ordered -> deterministic broadcast order
};

class ReplicationClient {
public:
    ReplicationClient(std::uint32_t player_id, ILockstepTransport* transport)
        : m_player_id(player_id), m_transport(transport) {}

    // Sends one tick of input upstream (records the latest tick for the ack).
    void SendUsercmd(const UsercmdMsg& cmd);

    // Drains inbound snapshots (most-recent-wins) and sends an Ack for the newest.
    void PumpInbound();

    [[nodiscard]] bool has_snapshot() const { return m_receiver.has_snapshot(); }
    [[nodiscard]] const SnapshotMsg& snapshot() const { return m_receiver.current(); }
    [[nodiscard]] std::uint32_t player_id() const { return m_player_id; }

private:
    std::uint32_t m_player_id = 0;
    ILockstepTransport* m_transport = nullptr;
    SnapshotReceiver m_receiver;
    std::uint64_t m_latest_usercmd_tick = 0;
    bool m_sent_any_usercmd = false;
};

} // namespace Luminumbra::Net
