// LREC1 session replay stream implementation. See ReplayStream.h for
// the format rationale and binding invariants. All multi-byte integers are
// written little-endian with explicit per-byte shifts (no memcpy of native
// integers, no struct padding) so the on-wire bytes are identical on every
// platform/compiler. Strings are length-prefixed (u32 LE length + raw bytes).

#include "replay/ReplayStream.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/Log.h"

namespace Luminumbra::Replay {
namespace {

namespace fs = std::filesystem;

// --- Little-endian fixed-width append helpers (the only on-wire encoders). ---
void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

// Length-prefixed string: u32 LE byte count + raw bytes.
void PutString(std::vector<std::uint8_t>& out, const std::string& s) {
    PutU32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

// Length-prefixed byte blob.
void PutBlob(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& b) {
    PutU32(out, static_cast<std::uint32_t>(b.size()));
    out.insert(out.end(), b.begin(), b.end());
}

// --- Sequential cursor reader over an in-memory byte buffer. -----------------
// Every Get* returns false (and sets the cursor past the end) on a short read,
// so a truncated tail is detected rather than reading garbage.
struct Cursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    bool ok = true;

    bool Remaining(std::size_t n) const {
        return ok && pos + n <= size;
    }

    bool GetU8(std::uint8_t& v) {
        if (!Remaining(1)) {
            ok = false;
            return false;
        }
        v = data[pos++];
        return true;
    }
    bool GetU16(std::uint16_t& v) {
        if (!Remaining(2)) {
            ok = false;
            return false;
        }
        v = static_cast<std::uint16_t>(data[pos]) |
            (static_cast<std::uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return true;
    }
    bool GetU32(std::uint32_t& v) {
        if (!Remaining(4)) {
            ok = false;
            return false;
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data[pos + i]) << (8 * i);
        }
        pos += 4;
        return true;
    }
    bool GetU64(std::uint64_t& v) {
        if (!Remaining(8)) {
            ok = false;
            return false;
        }
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(data[pos + i]) << (8 * i);
        }
        pos += 8;
        return true;
    }
    bool GetString(std::string& s) {
        std::uint32_t len = 0;
        if (!GetU32(len))
            return false;
        if (!Remaining(len)) {
            ok = false;
            return false;
        }
        s.assign(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return true;
    }
    bool GetBlob(std::vector<std::uint8_t>& b) {
        std::uint32_t len = 0;
        if (!GetU32(len))
            return false;
        if (!Remaining(len)) {
            ok = false;
            return false;
        }
        b.assign(data + pos, data + pos + len);
        pos += len;
        return true;
    }
    bool GetBytes(void* dst, std::size_t n) {
        if (!Remaining(n)) {
            ok = false;
            return false;
        }
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }
};

// Header layout. The tick_count field is written as a backpatch slot and finalized
// in-place by Finalize once the run length is known (its byte offset is fixed
// because everything before it is fixed-width).
//
//   [0..4]   magic "LREC1"            (5 bytes)
//   [5]      reserved u8 (0)
//   [6..7]   version u16
//   [8..9]   tick_rate_hz u16
//   [10..17] tick_count u64           (PATCHED by Finalize)  <- offset 10
//   [18..25] seed u64
//   [26..33] preset_hash u64
//   [34..37] surface_radius u32
//   [38..41] collision_radius u32
//   then length-prefixed strings: seed_string, preset, start_world_hash,
//   engine_version.
constexpr std::size_t kTickCountOffset = 10;

void AppendHeader(std::vector<std::uint8_t>& out, const ReplayHeader& h) {
    out.insert(out.end(), kLrec1Magic, kLrec1Magic + sizeof(kLrec1Magic));
    PutU8(out, 0); // reserved
    PutU16(out, h.version);
    PutU16(out, h.tick_rate_hz);
    PutU64(out, 0); // tick_count backpatch slot -- finalized in Finalize
    PutU64(out, h.seed);
    PutU64(out, h.preset_hash);
    PutU32(out, h.surface_radius);
    PutU32(out, h.collision_radius);
    PutString(out, h.seed_string);
    PutString(out, h.preset);
    PutString(out, h.start_world_hash);
    PutString(out, h.engine_version);
}

bool ParseHeader(Cursor& c, ReplayHeader& h) {
    char magic[sizeof(kLrec1Magic)] = {0};
    if (!c.GetBytes(magic, sizeof(magic)))
        return false;
    if (std::memcmp(magic, kLrec1Magic, sizeof(kLrec1Magic)) != 0)
        return false;
    std::uint8_t reserved = 0;
    if (!c.GetU8(reserved))
        return false;
    if (!c.GetU16(h.version))
        return false;
    // Refuse a version mismatch LOUDLY (the header's binding contract): a future/unknown LREC1
    // version would have a different record layout, so parsing it under today's assumptions
    // would silently mis-decode into garbage. Reject instead (Factorio breaks silently; we don't).
    if (h.version != kLrec1Version)
        return false;
    if (!c.GetU16(h.tick_rate_hz))
        return false;
    std::uint64_t header_tick_count = 0; // read separately (also in trailer)
    if (!c.GetU64(header_tick_count))
        return false;
    if (!c.GetU64(h.seed))
        return false;
    if (!c.GetU64(h.preset_hash))
        return false;
    if (!c.GetU32(h.surface_radius))
        return false;
    if (!c.GetU32(h.collision_radius))
        return false;
    if (!c.GetString(h.seed_string))
        return false;
    if (!c.GetString(h.preset))
        return false;
    if (!c.GetString(h.start_world_hash))
        return false;
    if (!c.GetString(h.engine_version))
        return false;
    return c.ok;
}

// One record frame: u8 type + u64 tick + u32 payload-length + payload. The
// explicit payload length lets a reader skip an unknown/partial record and lets
// truncation be detected (a frame claiming N payload bytes that are not present
// is a truncated tail).
void AppendRecordFrame(std::vector<std::uint8_t>& out,
                       RecordType type,
                       std::uint64_t tick,
                       const std::vector<std::uint8_t>& payload) {
    PutU8(out, static_cast<std::uint8_t>(type));
    PutU64(out, tick);
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

std::string Fnv1a64Hex(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    for (unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL; // FNV prime
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

// --- ReplayWriter ------------------------------------------------------------

bool ReplayWriter::Open(const std::string& path, const ReplayHeader& header) {
    m_path = path;
    m_buffer.clear();
    m_input_count = 0;
    m_checkpoint_count = 0;
    AppendHeader(m_buffer, header);
    m_open = true;
    return true;
}

void ReplayWriter::RecordInput(std::uint64_t tick, const std::vector<std::uint8_t>& inputs) {
    if (!m_open)
        return;
    // Payload: just the opaque input blob (length-prefixed). An empty input set
    // is 4 bytes (a zero length) -- compact, as the contract requires.
    std::vector<std::uint8_t> payload;
    PutBlob(payload, inputs);
    AppendRecordFrame(m_buffer, RecordType::Input, tick, payload);
    ++m_input_count;
}

void ReplayWriter::RecordCheckpoint(const CheckpointRecord& checkpoint) {
    if (!m_open)
        return;
    std::vector<std::uint8_t> payload;
    PutString(payload, checkpoint.world_hash);
    PutString(payload, checkpoint.terrain);
    PutString(payload, checkpoint.water);
    PutString(payload, checkpoint.entities);
    AppendRecordFrame(m_buffer, RecordType::Checkpoint, checkpoint.tick, payload);
    ++m_checkpoint_count;
}

bool ReplayWriter::Finalize(std::uint64_t tick_count) {
    if (!m_open)
        return false;

    // Patch the header tick_count in place (fixed offset; see AppendHeader).
    for (int i = 0; i < 8; ++i) {
        m_buffer[kTickCountOffset + i] = static_cast<std::uint8_t>((tick_count >> (8 * i)) & 0xFF);
    }

    // Trailer: magic + tick_count + input_count + checkpoint_count. A reader
    // that reaches the trailer knows the stream was finalized (not truncated).
    m_buffer.insert(m_buffer.end(), kLrec1Trailer, kLrec1Trailer + sizeof(kLrec1Trailer));
    PutU64(m_buffer, tick_count);
    PutU64(m_buffer, m_input_count);
    PutU64(m_buffer, m_checkpoint_count);

    std::error_code ec;
    const fs::path out_path(m_path);
    if (out_path.has_parent_path()) {
        fs::create_directories(out_path.parent_path(), ec);
    }
    std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LUMINUMBRA_CORE_ERROR("ReplayWriter: cannot open '{}' for write", m_path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(m_buffer.data()),
              static_cast<std::streamsize>(m_buffer.size()));
    out.close();
    m_open = false;
    return out.good();
}

// --- Reader ------------------------------------------------------------------

std::optional<ReplayContents> ReadReplay(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return std::nullopt;
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    }

    Cursor c{bytes.data(), bytes.size(), 0, true};
    ReplayContents contents;
    if (!ParseHeader(c, contents.header)) {
        // Not a valid LREC1 stream at all (bad magic / header too short).
        return std::nullopt;
    }

    // Record region: parse frames until the trailer magic appears, the buffer
    // is exhausted, or a frame is truncated. The trailer's first byte cannot
    // collide with a record type tag (Input=0x01/Checkpoint=0x02 vs 'L'=0x4C),
    // so peeking the type byte disambiguates trailer-vs-record cleanly.
    while (c.ok && c.pos < c.size) {
        // Trailer probe: does the remaining buffer start with the trailer magic?
        if (c.Remaining(sizeof(kLrec1Trailer)) &&
            std::memcmp(c.data + c.pos, kLrec1Trailer, sizeof(kLrec1Trailer)) == 0) {
            c.pos += sizeof(kLrec1Trailer);
            std::uint64_t trailer_ticks = 0;
            std::uint64_t trailer_inputs = 0;
            std::uint64_t trailer_checkpoints = 0;
            if (c.GetU64(trailer_ticks) && c.GetU64(trailer_inputs) &&
                c.GetU64(trailer_checkpoints)) {
                contents.tick_count = trailer_ticks;
                contents.trailer_present = true;
                // Trailer record counts must match what we parsed, else the
                // body was truncated/corrupted before the trailer.
                if (trailer_inputs != contents.inputs.size() ||
                    trailer_checkpoints != contents.checkpoints.size()) {
                    contents.truncated = true;
                }
            } else {
                contents.truncated = true; // trailer itself was short
            }
            break;
        }

        std::uint8_t type_byte = 0;
        std::uint64_t tick = 0;
        std::uint32_t payload_len = 0;
        if (!c.GetU8(type_byte) || !c.GetU64(tick) || !c.GetU32(payload_len)) {
            contents.truncated = true;
            break;
        }
        if (!c.Remaining(payload_len)) {
            contents.truncated = true; // frame claims more payload than exists
            break;
        }
        Cursor payload{c.data + c.pos, payload_len, 0, true};
        c.pos += payload_len;

        const auto type = static_cast<RecordType>(type_byte);
        if (type == RecordType::Input) {
            InputRecord rec;
            rec.tick = tick;
            if (!payload.GetBlob(rec.inputs)) {
                contents.truncated = true;
                break;
            }
            contents.inputs.push_back(std::move(rec));
        } else if (type == RecordType::Checkpoint) {
            CheckpointRecord rec;
            rec.tick = tick;
            if (!payload.GetString(rec.world_hash) || !payload.GetString(rec.terrain) ||
                !payload.GetString(rec.water) || !payload.GetString(rec.entities)) {
                contents.truncated = true;
                break;
            }
            contents.checkpoints.push_back(std::move(rec));
        } else {
            // Unknown record type: the explicit payload length let us skip it,
            // but an unknown type in a stream we wrote is corruption -- flag it.
            contents.truncated = true;
            break;
        }
    }

    // Reaching end-of-buffer WITHOUT a trailer is a truncated tail.
    if (!contents.trailer_present) {
        contents.truncated = true;
    }
    return contents;
}

const InputRecord* FindInput(const ReplayContents& contents, std::uint64_t tick) {
    for (const InputRecord& rec : contents.inputs) {
        if (rec.tick == tick)
            return &rec;
    }
    return nullptr;
}

const CheckpointRecord* FindCheckpoint(const ReplayContents& contents, std::uint64_t tick) {
    for (const CheckpointRecord& rec : contents.checkpoints) {
        if (rec.tick == tick)
            return &rec;
    }
    return nullptr;
}

} // namespace Luminumbra::Replay
