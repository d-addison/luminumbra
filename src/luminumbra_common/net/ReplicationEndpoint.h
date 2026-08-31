#pragma once

// Authoritative-server replication endpoints. Wires the wire protocol
// (Usercmd / Snapshot / Ack) and reliability layer
// (SnapshotReceiver / UsercmdReceiver) onto the engine's ILockstepTransport seam
// (LoopbackTransport for tests and socket transports in production). This is
// the server<->client message loop, independent of the sim:
//
//   ReplicationServer  — one per dedicated server. Holds a per-client link
//     (transport + inbound UsercmdReceiver + outbound snapshot seq). Builds and
//     broadcasts a SnapshotMsg from an authoritative entity-state set the sim
//     supplies, and drains inbound Usercmd/Ack (most-recent-wins per client).
//
//   ReplicationClient  — one per connected player. Sends its per-tick Usercmd
//     upstream and applies inbound snapshots most-recent-wins (SnapshotReceiver),
//     auto-acking the newest. Exposes the current authoritative snapshot for the
//     renderer to interpolate remote avatars.
//
// Engine-generic + world_hash-neutral: this is render/transport-side glue; the
// authoritative sim state is supplied to BroadcastSnapshot and consumed from
// LatestUsercmd by the caller (the server tick / the client input+render).
//
// SCALE PATH: server-authoritative delta replication here -- NOT
// lockstep -- is THE 20-32+ player session path. Delta-vs-acked (default at scale) +
// chunk-AOI bound per-client egress by local density, not headcount; one slow/leaving
// client never shared-fate-stalls the others. Architecture: docs/networking-scale-
// architecture.md. Lockstep (LockstepSession.h) is the determinism oracle / small co-op.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "LockstepSession.h"  // ILockstepTransport
#include "ReplicationDelta.h" // MakeSnapshotDelta / ApplySnapshotDelta
#include "ReplicationProtocol.h"

namespace Luminumbra::Net {

class ReplicationServer {
public:
    // Registers a connected client on its transport. client_id is the player id.
    void AddClient(std::uint32_t client_id, ILockstepTransport* transport);
    void RemoveClient(std::uint32_t client_id);
    [[nodiscard]] std::size_t client_count() const {
        return m_clients.size();
    }
    [[nodiscard]] bool has_client(std::uint32_t client_id) const {
        return m_clients.count(client_id) != 0;
    }

    //  persistent-server lifecycle. Removes any client whose transport peer
    // has cleanly disconnected (IsPeerConnected==false) and returns the removed
    // client ids, so the caller can despawn those players' avatars. Call after
    // PumpInbound (so a peer's queued frames are drained before it's pruned).
    // Surviving clients are untouched. Drain-then-prune = a clean leave, not a desync.
    std::vector<std::uint32_t> PruneDisconnectedClients();

    //  area-of-interest radius (mm). When > 0, each client's snapshot is
    // SCOPED to entities within this radius of THAT client's own avatar (the entity
    // whose id == client_id), so a 20-player world does not send everyone to
    // everyone (research mp-interest-management.md). 0 (default) = disabled = full
    // set. The client's own avatar is ALWAYS included.
    void SetAoiRadiusMm(std::int64_t radius_mm) {
        m_aoi_radius_mm = radius_mm;
    }
    [[nodiscard]] std::int64_t aoi_radius_mm() const {
        return m_aoi_radius_mm;
    }

    //  polish: CHUNK-INDEX area of interest (research mp-interest-management.md
    // "AOI reuses the existing chunk index"). Buckets entities by their horizontal
    // (X/Z) streaming chunk ONCE per broadcast, then sends each client only the
    // entities within `chunk_radius` chunks (Chebyshev) of its OWN avatar's chunk --
    // aligned with the grid the world actually streams on. Scales as
    // O(E + clients * (2r+1)^2) instead of the mm-radius path's O(clients * E), so a
    // 20+ player world with many entities stays cheap. Takes PRECEDENCE over the mm
    // radius when enabled. chunk_radius < 0 (default) = chunk AOI off.
    void SetAoiChunkRadius(int chunk_radius, std::int64_t chunk_size_mm) {
        m_aoi_chunk_radius = chunk_radius;
        m_aoi_chunk_size_mm = chunk_size_mm;
    }
    [[nodiscard]] int aoi_chunk_radius() const {
        return m_aoi_chunk_radius;
    }

    //  ack-driven DELTA-vs-acked snapshot compression (the bandwidth win
    // that makes 20-32+ players affordable). OFF by default -> full snapshots, wire-
    // identical to  (the canonical baselines hold). When ON, each client is sent
    // only what CHANGED since the snapshot it last ACKed (MakeSnapshotDelta), tagged
    // with that baseline's seq (delta_from_seq); the client reconstructs the full set
    // (ApplySnapshotDelta). Loss-tolerant by construction: the server keeps deltaing
    // against the last-ACKED baseline until a newer ack arrives, so a dropped delta is
    // recovered by the next one (no stranded client). Validate over NetworkSim
    // (injected loss/jitter) -- the single-PC unblocker for this slice.
    // SCALE PATH: default-OFF keeps the canonical full-snapshot
    // baselines bit-exact, but the 20-32 player session ENABLES this -- delta-vs-acked
    // is the bandwidth win that makes scale affordable.
    void SetDeltaCompression(bool on) {
        m_delta = on;
    }
    [[nodiscard]] bool delta_compression() const {
        return m_delta;
    }

    // OUTBOUND BACKPRESSURE POLICY. Default-OFF (like delta + AOI):
    // while off, BroadcastSnapshot is byte-identical to the pre-policy send path (the canonical
    // baselines + every existing backpressure/soak test hold, and ThrottledFrames stays 0).
    // When ON, a client whose per-client outbound queue crosses the high-water mark is
    // ESCALATED rather than piled onto:
    //   (1) THROTTLE  -- stop producing new snapshots for it (space the cadence), bumping
    //       ThrottledFrames, while still draining whatever is already queued;
    //   (2) KEYFRAME  -- if it stays behind, drop its piled delta backlog and resync it with a
    //       single FULL snapshot (delta_from_seq==0) it can apply standalone, not more deltas;
    //   (3) DISCONNECT -- if it never drains past the deadline, kick it (Close + remove from the
    //       client set) and enqueue its avatar despawn so surviving clients are unaffected.
    // The 20-32 player session ENABLES this on the server send path (docs/networking-scale-
    // architecture.md), exactly as it enables delta compression. Thresholds are named + tunable
    // (kThrottleHighWaterMark etc.); their exact values await  soak calibration.
    void SetBackpressurePolicy(bool on) {
        m_backpressure_policy = on;
    }
    [[nodiscard]] bool backpressure_policy() const {
        return m_backpressure_policy;
    }

    // Builds a SnapshotMsg from the authoritative entity set and sends it to every
    // connected client (each its own monotonically increasing seq + acked_usercmd_
    // tick). When AOI is enabled the per-client `entities` is filtered to that
    // client's area of interest. (Delta-vs-acked compression is a later step.)
    void BroadcastSnapshot(std::uint64_t server_tick,
                           const std::vector<ReplEntityState>& entities,
                           const std::vector<std::uint32_t>& removed_ids = {});

    // Drains all currently-available inbound frames from every client: Usercmd
    // (newest-wins) and Ack (monotonic). Non-blocking.
    void PumpInbound();

    // The newest usercmd received from a client (for the sim to apply), or null.
    [[nodiscard]] const UsercmdMsg* LatestUsercmd(std::uint32_t client_id) const;
    [[nodiscard]] std::uint32_t AckedSnapshotSeq(std::uint32_t client_id) const;

    //  bandwidth telemetry from the LAST BroadcastSnapshot -- total bytes
    // sent across all clients + the largest single per-client snapshot (the
    // bound that matters per connection). Lets the gate report MEASURED bytes
    // (vs the research's estimate) and show AOI's effect.
    [[nodiscard]] std::size_t last_broadcast_total_bytes() const {
        return m_last_broadcast_total_bytes;
    }
    [[nodiscard]] std::size_t last_broadcast_max_client_bytes() const {
        return m_last_broadcast_max_client_bytes;
    }

    // per-client BACKPRESSURE + SNAPSHOT-AGING telemetry for the 32-client
    // soak gate (the p95 early-warning that a real large session is degrading). All values
    // are derived from transport/ack state -- none feeds world_hash (transport-side glue).
    //
    //  OutboundQueueDepth(id)  -- frames produced for this client but NOT yet flushed to
    //    its transport (a would-block / gone peer leaves them buffered). 0 while the peer
    //    keeps up. This is the endpoint-level backpressure signal; the bounded send queue
    // drains/drops it. PeakOutboundQueueDepth is its session high-water; a frame
    //    dropped on queue overflow bumps DroppedFrames (snapshots are unreliable, so the
    //    OLDEST is dropped -- most-recent-wins).
    //  SnapshotAge(id)         -- how many seqs behind this client's last-ACKed baseline is
    //    (last_sent_seq - acked_seq, ). 0 means it acked the latest; a climbing
    //    value flags a falling-behind client before it desyncs or stalls.
    //  QueueDepthP95 / SnapshotAgeP95 -- the across-connected-clients p95 the soak gate
    //    FAILs on if either exceeds budget. 0 when there are no clients.
    [[nodiscard]] std::uint32_t OutboundQueueDepth(std::uint32_t client_id) const;
    [[nodiscard]] std::uint32_t SnapshotAge(std::uint32_t client_id) const;
    [[nodiscard]] std::uint32_t PeakOutboundQueueDepth(std::uint32_t client_id) const;
    [[nodiscard]] std::uint64_t DroppedFrames(std::uint32_t client_id) const;
    [[nodiscard]] std::uint64_t ThrottledFrames(std::uint32_t client_id) const;
    [[nodiscard]] std::uint32_t QueueDepthP95() const;
    [[nodiscard]] std::uint32_t SnapshotAgeP95() const;

    // cumulative FORCED KEYFRAMES the backpressure policy sent to this client
    // (each replaces a piled delta backlog with one standalone full resync frame). 0 while the
    // policy is off or the client keeps up.
    [[nodiscard]] std::uint64_t ForcedKeyframes(std::uint32_t client_id) const;

    // client ids the backpressure policy DISCONNECTED during the most recent
    // BroadcastSnapshot (hopeless peers that never drained past the deadline). Consume this
    // exactly like PruneDisconnectedClients' return -- despawn/free the player's server-side
    // state. Empty when the policy is off or nobody was dropped.
    [[nodiscard]] const std::vector<std::uint32_t>& DisconnectedClientsLastBroadcast() const {
        return m_disconnected_this_broadcast;
    }

private:
    struct ClientLink {
        ILockstepTransport* transport = nullptr;
        UsercmdReceiver inbound;
        std::uint32_t next_snapshot_seq = 1;
        //  per-client FULL (post-AOI) snapshots we have sent, keyed by seq,
        // so a delta can be computed against whichever one the client last ACKed.
        // Pruned below the acked seq (older baselines can never be referenced again).
        std::map<std::uint32_t, SnapshotMsg> sent_history;
        // per-client OUTBOUND send queue (backpressure substrate) + cumulative
        // telemetry. Each broadcast pushes the produced frame here, then flushes as far as the
        // transport accepts; a would-block / gone peer leaves frames buffered, so outbound.size
        // is the live queue-depth metric. Bounded by kOutboundQueueCap: overflow drops the OLDEST
        // (snapshots are unreliable / most-recent-wins) and increments dropped_frames. A connected
        // loopback/TCP peer accepts immediately, so the queue drains fully and the wire bytes stay
        // byte-identical to the prior direct-send path (existing baselines + determinism hold).
        std::deque<std::vector<std::uint8_t>> outbound;
        std::uint32_t peak_queue_depth = 0;
        std::uint64_t dropped_frames = 0;
        //   backpressure-policy state + metrics (active only when
        // m_backpressure_policy). throttled_frames: broadcasts whose new snapshot was SKIPPED to
        // space this client's cadence. forced_keyframes: full resync frames sent to it.
        // backed_up / over_hwm_streak: the escalation state machine -- backed_up latches when the
        // queue crosses the high-water mark and clears once it fully drains (hysteresis); the
        // streak counts consecutive backed-up broadcasts and drives keyframe -> disconnect.
        std::uint64_t throttled_frames = 0;
        std::uint64_t forced_keyframes = 0;
        bool backed_up = false;
        int over_hwm_streak = 0;
    };
    std::map<std::uint32_t, ClientLink> m_clients; // ordered -> deterministic broadcast order
    bool m_delta = false;               // delta-vs-acked compression (off = full snapshots)
    bool m_backpressure_policy = false; //  policy default-OFF (byte-identical fast path)
    static constexpr std::size_t kServerHistoryCap = 256; // bound per-client baseline retention
    static constexpr std::size_t kOutboundQueueCap =
        256; // bound per-client unflushed send backlog ()
    // Backpressure thresholds are calibrated against the 32-client loopback soak. A queue
    // depth above the observed p95 budget (16) starts throttling; sustained pressure forces a
    // keyframe after four broadcasts and disconnects after sixteen. High/low water marks give
    // the enter/leave hysteresis, and streaks count consecutive backed-up broadcasts.
    static constexpr std::uint32_t kThrottleHighWaterMark =
        16; // queue depth that starts throttling
    static constexpr std::uint32_t kBackpressureLowWaterMark =
        0;                                          // fully drained -> leave backed-up state
    static constexpr int kKeyframeResyncStreak = 4; // backed-up broadcasts -> force a full keyframe
    static constexpr int kDisconnectDeadlineStreak =
        16; // backed-up broadcasts w/o draining -> disconnect
    std::vector<std::uint32_t> m_disconnected_this_broadcast; //  drops from the last broadcast
    std::int64_t m_aoi_radius_mm = 0;     // 0 = mm-radius AOI disabled (full set)
    int m_aoi_chunk_radius = -1;          // < 0 = chunk AOI disabled
    std::int64_t m_aoi_chunk_size_mm = 0; // chunk edge length (mm) for chunk AOI
    std::size_t m_last_broadcast_total_bytes = 0;
    std::size_t m_last_broadcast_max_client_bytes = 0;
    //  polish: PENDING despawns. PruneDisconnectedClients enqueues the
    // leaver's avatar id here; each BroadcastSnapshot folds the pending ids into
    // removed_ids and REPEATS them across a few snapshots (the snapshot is
    // unreliable, so a single despawn could be dropped -> a ghost entity). The
    // value is the remaining repeat count; entries decay to 0 and are erased.
    std::map<std::uint32_t, int> m_pending_removed_ids;
    static constexpr int kRemovalRepeatBroadcasts = 3;
};

class ReplicationClient {
public:
    ReplicationClient(std::uint32_t player_id, ILockstepTransport* transport)
        : m_player_id(player_id)
        , m_transport(transport) {}

    // Sends one tick of input upstream (records the latest tick for the ack).
    void SendUsercmd(const UsercmdMsg& cmd);

    // Drains inbound snapshots (most-recent-wins) and sends an Ack for the newest.
    void PumpInbound();

    [[nodiscard]] bool has_snapshot() const {
        return m_receiver.has_snapshot();
    }
    [[nodiscard]] const SnapshotMsg& snapshot() const {
        return m_receiver.current();
    }
    [[nodiscard]] std::uint32_t player_id() const {
        return m_player_id;
    }

private:
    std::uint32_t m_player_id = 0;
    ILockstepTransport* m_transport = nullptr;
    SnapshotReceiver m_receiver;
    std::uint64_t m_latest_usercmd_tick = 0;
    bool m_sent_any_usercmd = false;
    //  reconstructed FULL snapshots, keyed by seq, so an incoming delta can
    // be applied against the exact baseline it was deltaed from (delta_from_seq). A
    // full snapshot (delta_from_seq==0) is used directly. Bounded; oldest evicted.
    std::map<std::uint32_t, SnapshotMsg> m_recon_history;
    static constexpr std::size_t kClientHistoryCap = 256;
};

//  client-side REMOTE-ENTITY INTERPOLATION (research mp-prediction-
// reconciliation.md). Snapshots arrive at ~15-20 Hz; the client RENDERS remote
// entities at a time slightly in the past (render-behind) and LERPs between the
// two bracketing snapshots, so motion is smooth between updates. The time axis is
// the server_tick the snapshot carries (no wall-clock here -- the caller supplies
// the render tick-time, typically newest_tick - interp_delay_ticks). No
// EXTRAPOLATION past the newest snapshot (clamp) -- walking avatars change
// direction abruptly, so extrapolation overshoots (research default).
class SnapshotInterpolator {
public:
    explicit SnapshotInterpolator(std::size_t max_buffer = 32)
        : m_max(max_buffer) {}

    // Buffer a snapshot (kept sorted ascending by server_tick; duplicates by tick
    // replace; oldest evicted past max_buffer). Snapshots should already be most-
    // recent-wins de-duped upstream (SnapshotReceiver) but out-of-order pushes are
    // tolerated.
    void Push(const SnapshotMsg& snap);

    // Interpolated entity states at fractional server tick `tick_time`. Entities in
    // BOTH bracketing snapshots are position/yaw-lerped; entities in only one are
    // passed through. Clamps to the nearest snapshot outside the buffered range.
    [[nodiscard]] std::vector<ReplEntityState> Sample(double tick_time) const;

    [[nodiscard]] bool empty() const {
        return m_buf.empty();
    }
    [[nodiscard]] std::size_t buffered() const {
        return m_buf.size();
    }
    // Newest buffered server_tick (0 if empty) -- the caller subtracts the interp
    // delay from this to get the render tick-time.
    [[nodiscard]] std::uint64_t newest_tick() const {
        return m_buf.empty() ? 0u : m_buf.back().server_tick;
    }

private:
    std::vector<SnapshotMsg> m_buf; // ascending by server_tick
    std::size_t m_max;
};

//  LOCAL-PLAYER prediction + reconciliation (research mp-prediction-
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
    struct Pos {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    LocalPlayerPredictor(float speed_ms = 4.0f, float dt_s = 1.0f / 30.0f)
        : m_speed(speed_ms)
        , m_dt(dt_s) {}

    void SetPosition(float x, float y, float z) {
        m_pos = {x, y, z};
    }

    // Apply this tick's input immediately (predict) and buffer it for reconcile.
    void RecordInput(std::uint64_t tick, float move_x, float move_z) {
        Step(m_pos, move_x, move_z);
        m_buffer.push_back({tick, move_x, move_z});
    }

    // Authoritative correction: drop inputs the server has folded in (tick <=
    // acked_tick), snap to the authoritative position, replay the rest on top.
    void Reconcile(float ax, float ay, float az, std::uint64_t acked_tick) {
        m_buffer.erase(std::remove_if(m_buffer.begin(),
                                      m_buffer.end(),
                                      [acked_tick](const Cmd& c) { return c.tick <= acked_tick; }),
                       m_buffer.end());
        m_pos = {ax, ay, az};
        for (const Cmd& c : m_buffer)
            Step(m_pos, c.move_x, c.move_z);
    }

    [[nodiscard]] Pos predicted() const {
        return m_pos;
    }
    [[nodiscard]] std::size_t pending_inputs() const {
        return m_buffer.size();
    }

private:
    struct Cmd {
        std::uint64_t tick;
        float move_x;
        float move_z;
    };
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
