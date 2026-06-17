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

#include <algorithm>
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
    [[nodiscard]] bool has_client(std::uint32_t client_id) const { return m_clients.count(client_id) != 0; }

    // T-I6 P4: persistent-server lifecycle. Removes any client whose transport peer
    // has cleanly disconnected (IsPeerConnected()==false) and returns the removed
    // client ids, so the caller can despawn those players' avatars. Call after
    // PumpInbound (so a peer's queued frames are drained before it's pruned).
    // Surviving clients are untouched. Drain-then-prune = a clean leave, not a desync.
    std::vector<std::uint32_t> PruneDisconnectedClients();

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

    // T-I6 P5: bandwidth telemetry from the LAST BroadcastSnapshot -- total bytes
    // sent across all clients + the largest single per-client snapshot (the
    // bound that matters per connection). Lets the gate report MEASURED bytes
    // (vs the research's estimate) and show AOI's effect.
    [[nodiscard]] std::size_t last_broadcast_total_bytes() const { return m_last_broadcast_total_bytes; }
    [[nodiscard]] std::size_t last_broadcast_max_client_bytes() const { return m_last_broadcast_max_client_bytes; }

private:
    struct ClientLink {
        ILockstepTransport* transport = nullptr;
        UsercmdReceiver inbound;
        std::uint32_t next_snapshot_seq = 1;
    };
    std::map<std::uint32_t, ClientLink> m_clients; // ordered -> deterministic broadcast order
    std::int64_t m_aoi_radius_mm = 0;              // 0 = AOI disabled (full set)
    std::size_t m_last_broadcast_total_bytes = 0;
    std::size_t m_last_broadcast_max_client_bytes = 0;
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

// T-I6 P3.3: LOCAL-PLAYER prediction + reconciliation (research mp-prediction-
// reconciliation.md). The client applies its OWN input immediately (predict, no
// wait for the server) and buffers each unacked usercmd. When an authoritative
// snapshot arrives (carrying acked_usercmd_tick), the client drops acked inputs,
// SNAPS its predicted position to the authoritative one, and REPLAYS the still-
// unacked inputs on top -- so the local avatar stays responsive yet converges to
// the server. Horizontal-only kinematic model (move axes in [-1,1] * speed);
// vertical is server-authoritative (gravity/terrain), not predicted. NOTE: the
// server steps Jolt CharacterVirtual, so a perfect match isn't guaranteed; the
// per-snapshot snap-then-replay bounds the error (render-side smoothing of the
// residual is a renderer concern). Engine-generic, world_hash-neutral.
class LocalPlayerPredictor {
public:
    struct Pos { float x = 0.0f; float y = 0.0f; float z = 0.0f; };

    LocalPlayerPredictor(float speed_ms = 4.0f, float dt_s = 1.0f / 30.0f)
        : m_speed(speed_ms), m_dt(dt_s) {}

    void SetPosition(float x, float y, float z) { m_pos = {x, y, z}; }

    // Apply this tick's input immediately (predict) and buffer it for reconcile.
    void RecordInput(std::uint64_t tick, float move_x, float move_z) {
        Step(m_pos, move_x, move_z);
        m_buffer.push_back({tick, move_x, move_z});
    }

    // Authoritative correction: drop inputs the server has folded in (tick <=
    // acked_tick), snap to the authoritative position, replay the rest on top.
    void Reconcile(float ax, float ay, float az, std::uint64_t acked_tick) {
        m_buffer.erase(std::remove_if(m_buffer.begin(), m_buffer.end(),
                                      [acked_tick](const Cmd& c) { return c.tick <= acked_tick; }),
                       m_buffer.end());
        m_pos = {ax, ay, az};
        for (const Cmd& c : m_buffer) Step(m_pos, c.move_x, c.move_z);
    }

    [[nodiscard]] Pos predicted() const { return m_pos; }
    [[nodiscard]] std::size_t pending_inputs() const { return m_buffer.size(); }

private:
    struct Cmd { std::uint64_t tick; float move_x; float move_z; };
    void Step(Pos& p, float move_x, float move_z) const {
        p.x += move_x * m_speed * m_dt;
        p.z += move_z * m_speed * m_dt;
    }
    std::vector<Cmd> m_buffer; // unacked inputs, ascending tick
    Pos m_pos;
    float m_speed;
    float m_dt;
};

} // namespace Luminumbra::Net
