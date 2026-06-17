#pragma once

// T-I6 P3.0: authoritative-server STATE REPLICATION protocol (multiplayer).
//
// Per MULTIPLAYER-BLOCKER-SPEC.md v2 + research/mp-replication.md: the
// authoritative dedicated server sends each client an entity-state SNAPSHOT;
// clients send their per-tick USERCMD upstream and ACK the latest snapshot.
// This header defines the WIRE MESSAGES + their encode/decode only (the first
// tested sub-phase). It carries opaque framed bytes over the EXISTING
// ILockstepTransport seam (LoopbackTransport for tests now; a UDP socket
// transport is the next sub-step), so no transport code is duplicated here.
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
    Usercmd  = 0x01, // client -> server: one tick of player input
    Snapshot = 0x02, // server -> client: authoritative entity-state set
    Ack      = 0x03, // client -> server: latest snapshot received + latest usercmd produced
};

// One tick of a player's input (the opaque blob the server applies to that
// player's avatar). Movement axes are quantized [-1,1] -> [-32767,32767]; look
// yaw is carried for facing (render-side, never a sim-hash input).
struct UsercmdMsg {
    std::uint64_t tick = 0;
    std::uint32_t player_id = 0;
    std::int16_t  move_x = 0;
    std::int16_t  move_z = 0;
    std::int16_t  yaw_mrad = 0;
    std::uint8_t  action_bits = 0; // bit0 jump, bit1 crouch, ... (game-defined)

    bool operator==(const UsercmdMsg& o) const {
        return tick == o.tick && player_id == o.player_id && move_x == o.move_x &&
               move_z == o.move_z && yaw_mrad == o.yaw_mrad && action_bits == o.action_bits;
    }
};

// One entity's replicated transform inside a snapshot (quantized).
struct ReplEntityState {
    std::uint32_t entity_id = 0;
    std::int32_t  px_mm = 0;
    std::int32_t  py_mm = 0;
    std::int32_t  pz_mm = 0;
    std::int16_t  yaw_mrad = 0;
    std::uint8_t  flags = 0; // bit0 grounded, ... (game-defined)

    bool operator==(const ReplEntityState& o) const {
        return entity_id == o.entity_id && px_mm == o.px_mm && py_mm == o.py_mm &&
               pz_mm == o.pz_mm && yaw_mrad == o.yaw_mrad && flags == o.flags;
    }
};

// The server's authoritative entity-state set for one client at one tick. P3.0
// is a FULL set (baseline); delta-vs-acked compression + AOI scoping land in
// P3.1/P3.2 -- the message already carries the seq + acked-usercmd fields they need.
struct SnapshotMsg {
    std::uint64_t server_tick = 0;
    std::uint32_t snapshot_seq = 0;        // monotonically increasing per receiver
    std::uint64_t acked_usercmd_tick = 0;  // newest usercmd the server has folded in (reconciliation)
    std::vector<ReplEntityState> entities;
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
inline float ReplDequantPos(std::int32_t mm) { return static_cast<float>(mm) / kReplPosScale; }
inline std::int16_t ReplQuantAngle(float radians) {
    const float v = radians * kReplAngleScale;
    return static_cast<std::int16_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}
inline float ReplDequantAngle(std::int16_t mrad) { return static_cast<float>(mrad) / kReplAngleScale; }

} // namespace Luminumbra::Net
