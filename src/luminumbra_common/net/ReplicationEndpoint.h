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

    // T-I6 P3.2: area-of-interest radius (mm). When > 0, each client's snapshot is
    // SCOPED to entities within this radius of THAT client's own avatar (the entity
    // whose id == client_id), so a 20-player world does not send everyone to
    // everyone (research mp-interest-management.md). 0 (default) = disabled = full
    // set. The client's own avatar is ALWAYS included.
    void SetAoiRadiusMm(std::int64_t radius_mm) { m_aoi_radius_mm = radius_mm; }
    [[nodiscard]] std::int64_t aoi_radius_mm() const { return m_aoi_radius_mm; }

    // Builds a SnapshotMsg from the authoritative entity set and sends it to every
    // connected client (each its own monotonically increasing seq + acked_usercmd_
    // tick). When AOI is enabled the per-client `entities` is filtered to that
    // client's area of interest. (Delta-vs-acked compression is a later step.)
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
    std::int64_t m_aoi_radius_mm = 0;              // 0 = AOI disabled (full set)
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

// T-I6 P3.3: client-side REMOTE-ENTITY INTERPOLATION (research mp-prediction-
// reconciliation.md). Snapshots arrive at ~15-20 Hz; the client RENDERS remote
// entities at a time slightly in the past (render-behind) and LERPs between the
// two bracketing snapshots, so motion is smooth between updates. The time axis is
// the server_tick the snapshot carries (no wall-clock here -- the caller supplies
// the render tick-time, typically newest_tick - interp_delay_ticks). No
// EXTRAPOLATION past the newest snapshot (clamp) -- walking avatars change
// direction abruptly, so extrapolation overshoots (research default).
class SnapshotInterpolator {
public:
    explicit SnapshotInterpolator(std::size_t max_buffer = 32) : m_max(max_buffer) {}

    // Buffer a snapshot (kept sorted ascending by server_tick; duplicates by tick
    // replace; oldest evicted past max_buffer). Snapshots should already be most-
    // recent-wins de-duped upstream (SnapshotReceiver) but out-of-order pushes are
    // tolerated.
    void Push(const SnapshotMsg& snap);

    // Interpolated entity states at fractional server tick `tick_time`. Entities in
    // BOTH bracketing snapshots are position/yaw-lerped; entities in only one are
    // passed through. Clamps to the nearest snapshot outside the buffered range.
    [[nodiscard]] std::vector<ReplEntityState> Sample(double tick_time) const;

    [[nodiscard]] bool empty() const { return m_buf.empty(); }
    [[nodiscard]] std::size_t buffered() const { return m_buf.size(); }
    // Newest buffered server_tick (0 if empty) -- the caller subtracts the interp
    // delay from this to get the render tick-time.
    [[nodiscard]] std::uint64_t newest_tick() const {
        return m_buf.empty() ? 0u : m_buf.back().server_tick;
    }

private:
    std::vector<SnapshotMsg> m_buf; // ascending by server_tick
    std::size_t m_max;
};

} // namespace Luminumbra::Net
