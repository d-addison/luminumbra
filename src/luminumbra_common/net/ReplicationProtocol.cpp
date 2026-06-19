#include "ReplicationProtocol.h"

namespace Luminumbra::Net {

namespace {

// Little-endian fixed-width append helpers (the only on-wire encoders; identical
// discipline to LockstepSession's encoders).
void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void PutU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void PutI16(std::vector<std::uint8_t>& out, std::int16_t v) { PutU16(out, static_cast<std::uint16_t>(v)); }
void PutI32(std::vector<std::uint8_t>& out, std::int32_t v) { PutU32(out, static_cast<std::uint32_t>(v)); }

// Cursor reader (truncation-robust: every Get returns false past the end).
class Cursor {
public:
    explicit Cursor(const std::vector<std::uint8_t>& buf) : m_buf(buf) {}
    bool GetU8(std::uint8_t& v) {
        if (m_pos + 1 > m_buf.size()) return false;
        v = m_buf[m_pos++];
        return true;
    }
    bool GetU16(std::uint16_t& v) {
        if (m_pos + 2 > m_buf.size()) return false;
        v = static_cast<std::uint16_t>(m_buf[m_pos]) |
            (static_cast<std::uint16_t>(m_buf[m_pos + 1]) << 8);
        m_pos += 2;
        return true;
    }
    bool GetU32(std::uint32_t& v) {
        if (m_pos + 4 > m_buf.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(m_buf[m_pos + i]) << (8 * i);
        m_pos += 4;
        return true;
    }
    bool GetU64(std::uint64_t& v) {
        if (m_pos + 8 > m_buf.size()) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(m_buf[m_pos + i]) << (8 * i);
        m_pos += 8;
        return true;
    }
    bool GetI16(std::int16_t& v) {
        std::uint16_t u;
        if (!GetU16(u)) return false;
        v = static_cast<std::int16_t>(u);
        return true;
    }
    bool GetI32(std::int32_t& v) {
        std::uint32_t u;
        if (!GetU32(u)) return false;
        v = static_cast<std::int32_t>(u);
        return true;
    }

private:
    const std::vector<std::uint8_t>& m_buf;
    std::size_t m_pos = 0;
};

// Frames a payload as [u8 type][u32 len][payload].
std::vector<std::uint8_t> Frame(ReplMessageType type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(5 + payload.size());
    PutU8(out, static_cast<std::uint8_t>(type));
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Validates the frame header + type and returns a cursor positioned at the payload.
bool OpenFrame(const std::vector<std::uint8_t>& frame, ReplMessageType expected, Cursor& cursor) {
    std::uint8_t type = 0;
    std::uint32_t len = 0;
    if (!cursor.GetU8(type) || !cursor.GetU32(len)) return false;
    if (static_cast<ReplMessageType>(type) != expected) return false;
    return frame.size() == static_cast<std::size_t>(5) + len;
}

} // namespace

std::vector<std::uint8_t> EncodeUsercmd(const UsercmdMsg& m) {
    std::vector<std::uint8_t> p;
    PutU64(p, m.tick);
    PutU32(p, m.player_id);
    PutI16(p, m.move_x);
    PutI16(p, m.move_z);
    PutI16(p, m.yaw_mrad);
    PutU8(p, m.action_bits);
    return Frame(ReplMessageType::Usercmd, p);
}

std::vector<std::uint8_t> EncodeSnapshot(const SnapshotMsg& m) {
    std::vector<std::uint8_t> p;
    PutU64(p, m.server_tick);
    PutU32(p, m.snapshot_seq);
    PutU32(p, m.delta_from_seq);
    PutU64(p, m.acked_usercmd_tick);
    PutU32(p, static_cast<std::uint32_t>(m.entities.size()));
    for (const ReplEntityState& e : m.entities) {
        PutU32(p, e.entity_id);
        PutI32(p, e.px_mm);
        PutI32(p, e.py_mm);
        PutI32(p, e.pz_mm);
        PutI16(p, e.yaw_mrad);
        PutU8(p, e.flags);
        PutU16(p, e.type_id);
        PutU8(p, e.anim_state);
        PutU8(p, e.anim_phase);
    }
    PutU32(p, static_cast<std::uint32_t>(m.removed_ids.size()));
    for (std::uint32_t id : m.removed_ids) {
        PutU32(p, id);
    }
    return Frame(ReplMessageType::Snapshot, p);
}

std::vector<std::uint8_t> EncodeAck(const AckMsg& m) {
    std::vector<std::uint8_t> p;
    PutU32(p, m.snapshot_seq);
    PutU64(p, m.usercmd_tick);
    return Frame(ReplMessageType::Ack, p);
}

bool PeekReplMessageType(const std::vector<std::uint8_t>& frame, ReplMessageType& out_type) {
    if (frame.empty()) return false;
    out_type = static_cast<ReplMessageType>(frame[0]);
    return true;
}

bool DecodeUsercmd(const std::vector<std::uint8_t>& frame, UsercmdMsg& out) {
    Cursor c(frame);
    if (!OpenFrame(frame, ReplMessageType::Usercmd, c)) return false;
    return c.GetU64(out.tick) && c.GetU32(out.player_id) && c.GetI16(out.move_x) &&
           c.GetI16(out.move_z) && c.GetI16(out.yaw_mrad) && c.GetU8(out.action_bits);
}

bool DecodeSnapshot(const std::vector<std::uint8_t>& frame, SnapshotMsg& out) {
    Cursor c(frame);
    if (!OpenFrame(frame, ReplMessageType::Snapshot, c)) return false;
    std::uint32_t count = 0;
    if (!c.GetU64(out.server_tick) || !c.GetU32(out.snapshot_seq) ||
        !c.GetU32(out.delta_from_seq) || !c.GetU64(out.acked_usercmd_tick) || !c.GetU32(count)) {
        return false;
    }
    out.entities.clear();
    out.entities.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ReplEntityState e;
        if (!c.GetU32(e.entity_id) || !c.GetI32(e.px_mm) || !c.GetI32(e.py_mm) ||
            !c.GetI32(e.pz_mm) || !c.GetI16(e.yaw_mrad) || !c.GetU8(e.flags) ||
            !c.GetU16(e.type_id) || !c.GetU8(e.anim_state) || !c.GetU8(e.anim_phase)) {
            return false;
        }
        out.entities.push_back(e);
    }
    std::uint32_t removed_count = 0;
    if (!c.GetU32(removed_count)) return false;
    out.removed_ids.clear();
    out.removed_ids.reserve(removed_count);
    for (std::uint32_t i = 0; i < removed_count; ++i) {
        std::uint32_t id = 0;
        if (!c.GetU32(id)) return false;
        out.removed_ids.push_back(id);
    }
    return true;
}

bool DecodeAck(const std::vector<std::uint8_t>& frame, AckMsg& out) {
    Cursor c(frame);
    if (!OpenFrame(frame, ReplMessageType::Ack, c)) return false;
    return c.GetU32(out.snapshot_seq) && c.GetU64(out.usercmd_tick);
}

} // namespace Luminumbra::Net
