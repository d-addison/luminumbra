#pragma once

// Authoritative-server state replication protocol.
//
// The authoritative dedicated server sends each client an entity-state snapshot;
// clients send their per-tick USERCMD upstream and ACK the latest snapshot.
// This header defines the WIRE MESSAGES + their encode/decode only (the first
// tested sub-phase). It carries opaque framed bytes over the EXISTING
// ILockstepTransport seam (LoopbackTransport for tests now; a UDP socket
// transport), so no transport code is duplicated here.
//
// Wire discipline (identical to LockstepSession): fixed-width little-endian,
// length-prefixed strings/blobs, NO struct padding on the wire, NO float bit-
// twiddling -- floats are QUANTIZED to fixed-point integers (positions to mm,
// angles to milli-radians) so the wire form is compact and exactly reproducible.
//
// Determinism note: replication is server-authoritative, so it does NOT require
// cross-machine bit-exactness (the heavy lockstep tax is gone). Quantization
// here is for bandwidth + a stable wire form, not for a determinism hash.

#include <cstdint>
#include <string>
#include <vector>

namespace Luminumbra::Net {

inline constexpr char kReplMagic[5] = {'R', 'E', 'P', 'L', '1'};
inline constexpr std::uint16_t kReplProtocolVersion = 1;

// Fixed-point quantization scales (encode = round(value * scale)).
inline constexpr float kReplPosScale = 1000.0f;   // metres -> millimetres
inline constexpr float kReplAngleScale = 1000.0f; // radians -> milli-radians

enum class ReplMessageType : std::uint8_t {
    Usercmd = 0x01,  // client -> server: one tick of player input
    Snapshot = 0x02, // server -> client: authoritative entity-state set
    Ack = 0x03,      // client -> server: latest snapshot received + latest usercmd produced
};

// One tick of a player's input (the opaque blob the server applies to that
// player's avatar). Movement axes are quantized [-1,1] -> [-32767,32767]; look
// yaw is carried for facing (render-side, never a sim-hash input).
struct UsercmdMsg {
    std::uint64_t tick = 0;
    std::uint32_t player_id = 0;
    std::int16_t move_x = 0;
    std::int16_t move_z = 0;
    std::int16_t yaw_mrad = 0;
    std::uint8_t action_bits = 0; // bit0 jump, bit1 crouch,... (game-defined)

    bool operator==(const UsercmdMsg& o) const {
        return tick == o.tick && player_id == o.player_id && move_x == o.move_x &&
               move_z == o.move_z && yaw_mrad == o.yaw_mrad && action_bits == o.action_bits;
    }
};

// One entity's replicated transform + class inside a snapshot (quantized).
// type_id lets the client instantiate the right mesh or behaviour (player,
// animal / NPC / projectile -- like a Quake3 eType / Source server-class index);
// anim_state is the current clip enum and anim_phase its normalized time
// (0..255 -> 0..1) -- NEVER joint data. All game-defined; the engine only carries them.
struct ReplEntityState {
    std::uint32_t entity_id = 0;
    std::int32_t px_mm = 0;
    std::int32_t py_mm = 0;
    std::int32_t pz_mm = 0;
    std::int16_t yaw_mrad = 0;
    std::uint8_t flags = 0;      // bit0 grounded, bit1 owned/predicted,... (game-defined)
    std::uint16_t type_id = 0;   // archetype/class (0 = default/player avatar)
    std::uint8_t anim_state = 0; // current clip enum (game-defined)
    std::uint8_t anim_phase = 0; // normalized clip time, 0..255

    bool operator==(const ReplEntityState& o) const {
        return entity_id == o.entity_id && px_mm == o.px_mm && py_mm == o.py_mm &&
               pz_mm == o.pz_mm && yaw_mrad == o.yaw_mrad && flags == o.flags &&
               type_id == o.type_id && anim_state == o.anim_state && anim_phase == o.anim_phase;
    }
};

// The server's authoritative entity-state set for one client at one tick.
// is a FULL set (baseline); delta-vs-acked compression + AOI scoping land in
// / -- the message already carries the seq + acked-usercmd fields they need.
struct SnapshotMsg {
    std::uint64_t server_tick = 0;
    std::uint32_t snapshot_seq = 0; // monotonically increasing per receiver
    //  the baseline seq this snapshot was DELTA-compressed against (Quake3
    // "deltaFrom"). 0 = a FULL snapshot (no baseline; decode it standalone). When
    // non-zero the receiver reconstructs the full set via ApplySnapshotDelta against
    // the snapshot it previously reconstructed at this seq. Ack-driven: the server
    // only ever deltas against a seq the client has ACKed, so a dropped delta never
    // strands the client (the next delta is still against the same acked baseline).
    std::uint32_t delta_from_seq = 0;
    std::uint64_t acked_usercmd_tick =
        0; // newest usercmd the server has folded in (reconciliation)
    std::vector<ReplEntityState> entities;
    //  explicit RELIABLE despawn -- ids that LEFT this client's set (died /
    // expired / left AOI). Full snapshots drop absent entities implicitly, but a
    // transient (a spent arrow) needs an explicit removal so a lost update can't
    // strand a ghost (Halo's "cosmetic one-shot events get lost" lesson).
    std::vector<std::uint32_t> removed_ids;
};

// Client -> server acknowledgement: the newest snapshot the client has applied
// (so the server can delta against it) + the newest usercmd the client produced.
struct AckMsg {
    std::uint32_t snapshot_seq = 0;
    std::uint64_t usercmd_tick = 0;
};

// --- Encode: each returns a complete framed message [u8 type][u32 len][payload]. ---
std::vector<std::uint8_t> EncodeUsercmd(const UsercmdMsg& m);
std::vector<std::uint8_t> EncodeSnapshot(const SnapshotMsg& m);
std::vector<std::uint8_t> EncodeAck(const AckMsg& m);

// --- Decode: false on a short/garbled frame (truncation-robust). ---
bool PeekReplMessageType(const std::vector<std::uint8_t>& frame, ReplMessageType& out_type);
bool DecodeUsercmd(const std::vector<std::uint8_t>& frame, UsercmdMsg& out);
bool DecodeSnapshot(const std::vector<std::uint8_t>& frame, SnapshotMsg& out);
bool DecodeAck(const std::vector<std::uint8_t>& frame, AckMsg& out);

// --- Quantization helpers (so the server/client agree on the mapping). ---
inline std::int32_t ReplQuantPos(float metres) {
    return static_cast<std::int32_t>(metres * kReplPosScale + (metres >= 0.0f ? 0.5f : -0.5f));
}
inline float ReplDequantPos(std::int32_t mm) {
    return static_cast<float>(mm) / kReplPosScale;
}
inline std::int16_t ReplQuantAngle(float radians) {
    const float v = radians * kReplAngleScale;
    return static_cast<std::int16_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}
inline float ReplDequantAngle(std::int16_t mrad) {
    return static_cast<float>(mrad) / kReplAngleScale;
}

// ---------------------------------------------------------------------------
//  unreliable-delivery reliability layer (what makes UDP usable).
// State replication runs over UNRELIABLE UDP (research/mp-replication.md): a
// datagram can arrive late, out of order, or duplicated, and an OLD snapshot is
// superseded by a newer one (most-recent-wins -- TCP head-of-line blocking is
// exactly the wrong behaviour here). These tiny endpoints encode that semantics
// independent of the socket, so they are unit-testable without real ports (the
// raw winsock UDP ILockstepTransport is a separate owner-LAN-validated step).
// ---------------------------------------------------------------------------

// CLIENT side: keeps only the NEWEST snapshot seen (by snapshot_seq); discards
// any stale/duplicate datagram. Generates the Ack the client sends upstream.
class SnapshotReceiver {
public:
    // Returns true if accepted (strictly newer seq), false if stale/duplicate.
    bool Receive(const SnapshotMsg& snap) {
        if (m_has && snap.snapshot_seq <= m_current.snapshot_seq)
            return false;
        m_current = snap;
        m_has = true;
        return true;
    }
    [[nodiscard]] bool has_snapshot() const {
        return m_has;
    }
    [[nodiscard]] const SnapshotMsg& current() const {
        return m_current;
    }
    // The Ack to send upstream: newest snapshot applied + newest usercmd produced.
    [[nodiscard]] AckMsg make_ack(std::uint64_t latest_usercmd_tick) const {
        return AckMsg{m_has ? m_current.snapshot_seq : 0u, latest_usercmd_tick};
    }

private:
    SnapshotMsg m_current;
    bool m_has = false;
};

// SERVER side, per connected client: keeps the NEWEST usercmd seen (by tick;
// UDP may reorder) and tracks the latest snapshot_seq that client has acked (so
// the server can delta against it in ).
class UsercmdReceiver {
public:
    // Returns true if accepted (strictly newer tick), false if stale/duplicate.
    bool Receive(const UsercmdMsg& cmd) {
        if (m_has && cmd.tick <= m_latest.tick)
            return false;
        m_latest = cmd;
        m_has = true;
        return true;
    }
    [[nodiscard]] bool has_command() const {
        return m_has;
    }
    [[nodiscard]] const UsercmdMsg& latest() const {
        return m_latest;
    }

    // Record an Ack from this client (monotonic: a stale ack never lowers state).
    void ApplyAck(const AckMsg& ack) {
        if (ack.snapshot_seq > m_acked_snapshot_seq)
            m_acked_snapshot_seq = ack.snapshot_seq;
    }
    [[nodiscard]] std::uint32_t acked_snapshot_seq() const {
        return m_acked_snapshot_seq;
    }

private:
    UsercmdMsg m_latest;
    bool m_has = false;
    std::uint32_t m_acked_snapshot_seq = 0;
};

} // namespace Luminumbra::Net
