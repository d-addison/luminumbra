// LREC1 session replay stream implementation. See ReplayStream.h for
// the format rationale and binding invariants. All multi-byte integers are
// written little-endian with explicit per-byte shifts (no memcpy of native
// integers, no struct padding) so the on-wire bytes are identical on every
// platform/compiler. Strings are length-prefixed (u32 LE length + raw bytes).

#include "replay/ReplayStream.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/JobSystem.h"
#include "core/Log.h"
#include "world/SurfaceWorkBudget.h"

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
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) {
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
        return ok && pos <= size && n <= size - pos;
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
        v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[pos]) |
                                       (static_cast<std::uint16_t>(data[pos + 1]) << 8));
        pos += 2;
        return true;
    }
    bool GetU32(std::uint32_t& v) {
        if (!Remaining(4)) {
            ok = false;
            return false;
        }
        v = 0;
        for (std::size_t i = 0; i < 4; ++i) {
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
        for (std::size_t i = 0; i < 8; ++i) {
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
//   engine_version, system_config_identity, preset_content_hash (v3).
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
    PutString(out, h.system_config_identity);
    PutString(out, h.preset_content_hash);
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
    if (h.version != kLrec1Version && h.version != kLrec1ElidedVersion)
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
    if (!World::SurfaceRadiiSupported(h.surface_radius, h.collision_radius))
        return false;
    if (!c.GetString(h.seed_string))
        return false;
    if (!c.GetString(h.preset))
        return false;
    if (!c.GetString(h.start_world_hash))
        return false;
    if (!c.GetString(h.engine_version))
        return false;
    if (!c.GetString(h.system_config_identity))
        return false;
    if (!c.GetString(h.preset_content_hash))
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
    for (const char character : text) {
        const auto ch = static_cast<unsigned char>(character);
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL; // FNV prime
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

// --- ReplayWriter ------------------------------------------------------------

bool ReplayWriter::Open(const std::string& path, const ReplayHeader& header) {
    if (!World::SurfaceRadiiSupported(header.surface_radius, header.collision_radius))
        return false;
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
    for (std::size_t i = 0; i < 8; ++i) {
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

// --- BoundedReplayRecorder ---------------------------------------------------

BoundedReplayRecorder::~BoundedReplayRecorder() {
    Close();
}

bool BoundedReplayRecorder::Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks) {
    if (!m_directory.empty())
        return false;
    const auto size_max = (std::numeric_limits<std::size_t>::max)();
    const auto segment_bytes = std::max(kSegmentBytes, max_tick_bytes);
    if (max_tick_bytes < 17 || segment_bytes > size_max / 3 ||
        unconfirmed_ticks > (size_max - 3 * segment_bytes) / max_tick_bytes)
        return false;
    m_max_tick_bytes = max_tick_bytes;
    m_byte_limit = static_cast<std::size_t>(unconfirmed_ticks) * max_tick_bytes + 3 * segment_bytes;
    return true;
}

bool BoundedReplayRecorder::Open(const std::string& destination, const ReplayHeader& header) {
    if (m_byte_limit == 0 || !m_directory.empty())
        return false;
    // Header allocation is bounded independently of the tick/segment budget.
    std::size_t remaining = kSegmentBytes - 66; // fixed fields + six u32 lengths
    for (const auto* part : {&header.seed_string,
                             &header.preset,
                             &header.start_world_hash,
                             &header.engine_version,
                             &header.system_config_identity,
                             &header.preset_content_hash}) {
        if (part->size() > remaining)
            return false;
        remaining -= part->size();
    }
    std::error_code ec;
    const fs::path out_path(destination);
    if (out_path.has_parent_path()) {
        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            LUMINUMBRA_CORE_ERROR("Replay recorder: cannot create parent '{}': {} ({})",
                                  out_path.parent_path().string(),
                                  ec.message(),
                                  ec.value());
            return false;
        }
    }
    // Exclusive directory creation avoids collisions between concurrent sessions,
    // including processes using the same default dump destination.
    static std::atomic<std::uint64_t> next_directory{0};
    for (std::size_t attempt = 0; attempt < kSegmentTicks; ++attempt) {
        const auto candidate =
            (out_path.parent_path() /
             ("lockstep-recording-" + std::to_string(next_directory.fetch_add(1))))
                .string();
        if (fs::create_directory(candidate, ec)) {
            m_directory = candidate;
            m_header = header;
            m_digest = 1469598103934665603ULL;
            m_elided_tick = 0;
            m_elided_digest = m_digest;
            m_last_tick = 0;
            m_output.clear();
            return true;
        }
        if (ec) {
            LUMINUMBRA_CORE_ERROR("Replay recorder: cannot reserve directory '{}': {} ({})",
                                  candidate,
                                  ec.message(),
                                  ec.value());
            return false;
        }
    }
    LUMINUMBRA_CORE_ERROR("Replay recorder: exhausted {} directory candidates beside '{}'",
                          kSegmentTicks,
                          destination);
    return false;
}

bool BoundedReplayRecorder::Prune(std::uint64_t tick, std::uint64_t confirmed_tick) {
    const auto oldest = tick > kSegmentTicks ? tick - kSegmentTicks : 0;
    // Each segment is examined/removed once. Never elide an unconfirmed tick:
    // a delayed mismatching checkpoint must still have its inputs in the dump.
    while (m_segments.size() > 1) {
        const auto& segment = m_segments.front();
        if (segment.last_tick > confirmed_tick ||
            (segment.last_tick > oldest && m_bytes - segment.bytes < kSegmentBytes))
            break;
        std::error_code ec;
        if (!fs::remove(segment.path, ec) || ec)
            return false;
        m_elided_tick = segment.last_tick;
        m_elided_digest = segment.digest;
        m_bytes -= segment.bytes;
        m_segments.pop_front();
    }
    return true;
}

bool BoundedReplayRecorder::RecordTick(std::uint64_t tick,
                                       const std::vector<std::uint8_t>& inputs,
                                       const std::array<std::string_view, 4>* hashes,
                                       std::uint64_t confirmed_tick) {
    if (m_directory.empty() || tick <= m_last_tick || m_max_tick_bytes < 17 ||
        inputs.size() > m_max_tick_bytes - 17 ||
        inputs.size() > (std::numeric_limits<std::uint32_t>::max)() - 4)
        return false;
    std::size_t bytes = 17 + inputs.size();
    if (hashes) {
        if (m_max_tick_bytes - bytes < 29)
            return false;
        bytes += 29; // checkpoint frame + four u32 lengths
        for (const auto part : *hashes) {
            if (part.size() > m_max_tick_bytes - bytes ||
                part.size() > (std::numeric_limits<std::uint32_t>::max)() - 16)
                return false;
            bytes += part.size();
        }
        if (bytes - 17 - inputs.size() - 13 > (std::numeric_limits<std::uint32_t>::max)())
            return false;
    }
    if (!Prune(tick, confirmed_tick) || bytes > m_byte_limit - m_bytes)
        return false;

    // One encoding buffer, bounded by the validated maximum merged input plus
    // one checkpoint (three negotiated transport frames). No history copies.
    std::vector<std::uint8_t> encoded;
    encoded.reserve(bytes);
    PutU8(encoded, static_cast<std::uint8_t>(RecordType::Input));
    PutU64(encoded, tick);
    PutU32(encoded, static_cast<std::uint32_t>(4 + inputs.size()));
    PutBlob(encoded, inputs);
    if (hashes) {
        PutU8(encoded, static_cast<std::uint8_t>(RecordType::Checkpoint));
        PutU64(encoded, tick);
        PutU32(encoded, static_cast<std::uint32_t>(bytes - 17 - inputs.size() - 13));
        for (const auto part : *hashes) {
            PutU32(encoded, static_cast<std::uint32_t>(part.size()));
            encoded.insert(encoded.end(), part.begin(), part.end());
        }
    }
    const bool rotate = !m_segments.empty() &&
                        (m_segments.back().ticks == kSegmentTicks ||
                         bytes > kSegmentBytes - std::min(kSegmentBytes, m_segments.back().bytes));
    if (rotate) {
        m_output.close();
        if (m_output.fail())
            return false;
    }
    if (m_segments.empty() || rotate) {
        const auto path = (fs::path(m_directory) / std::to_string(tick)).string();
        m_output.open(path, std::ios::binary | std::ios::trunc);
        if (!m_output.is_open())
            return false;
        m_segments.push_back({.path = path, .last_tick = 0, .digest = 0, .bytes = 0, .ticks = 0});
    }
    m_output.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
    if (!m_output.good())
        return false;
    for (const auto byte : encoded) {
        m_digest ^= byte;
        m_digest *= 1099511628211ULL;
    }
    auto& segment = m_segments.back();
    segment.last_tick = tick;
    segment.digest = m_digest;
    segment.bytes += bytes;
    ++segment.ticks;
    m_bytes += bytes;
    m_last_tick = tick;
    return Prune(tick, confirmed_tick);
}

bool BoundedReplayRecorder::Finalize(const std::string& destination,
                                     std::uint64_t tick_count,
                                     std::uint64_t divergence_tick) {
    if (m_directory.empty() || divergence_tick <= m_elided_tick)
        return false;
    if (m_output.is_open()) {
        m_output.close();
        if (m_output.fail())
            return false;
    }
    std::vector<std::uint8_t> header;
    if (m_elided_tick != 0)
        m_header.version = kLrec1ElidedVersion;
    AppendHeader(header, m_header);
    for (std::size_t i = 0; i < 8; ++i)
        header[kTickCountOffset + i] = static_cast<std::uint8_t>((tick_count >> (8 * i)) & 0xFF);
    if (m_elided_tick != 0) {
        PutU8(header, static_cast<std::uint8_t>(RecordType::ElidedPrefix));
        PutU64(header, m_elided_tick);
        PutU32(header, 8);
        PutU64(header, m_elided_digest);
    }
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    if (!out.write(reinterpret_cast<const char*>(header.data()),
                   static_cast<std::streamsize>(header.size())))
        return false;
    std::uint64_t input_count = 0;
    std::uint64_t checkpoint_count = 0;
    // Finalization streams through a fixed 64 KiB scratch buffer; the replay
    // artifact is never assembled in memory, regardless of the negotiated bound.
    std::array<char, std::size_t{64} * 1024> scratch{};
    for (const auto& segment : m_segments) {
        std::ifstream in(segment.path, std::ios::binary);
        if (!in.is_open())
            return false;
        std::size_t remaining = segment.bytes;
        while (remaining != 0) {
            std::array<std::uint8_t, 13> frame{};
            if (remaining < frame.size() || !in.read(reinterpret_cast<char*>(frame.data()),
                                                     static_cast<std::streamsize>(frame.size())))
                return false;
            Cursor cursor{frame.data(), frame.size(), 0, true};
            std::uint8_t type = 0;
            std::uint64_t tick = 0;
            std::uint32_t payload = 0;
            if (!cursor.GetU8(type) || !cursor.GetU64(tick) || !cursor.GetU32(payload) ||
                payload > remaining - frame.size())
                return false;
            const bool keep = type != static_cast<std::uint8_t>(RecordType::Checkpoint) ||
                              tick <= divergence_tick;
            if (type == static_cast<std::uint8_t>(RecordType::Input))
                ++input_count;
            else if (type == static_cast<std::uint8_t>(RecordType::Checkpoint))
                checkpoint_count += keep;
            else
                return false;
            if (keep && !out.write(reinterpret_cast<const char*>(frame.data()),
                                   static_cast<std::streamsize>(frame.size())))
                return false;
            remaining -= frame.size() + payload;
            std::size_t pending = payload;
            while (pending != 0) {
                const auto count = std::min(pending, scratch.size());
                if (!in.read(scratch.data(), static_cast<std::streamsize>(count)))
                    return false;
                if (keep && !out.write(scratch.data(), static_cast<std::streamsize>(count)))
                    return false;
                pending -= count;
            }
        }
    }
    std::vector<std::uint8_t> trailer;
    trailer.insert(trailer.end(), kLrec1Trailer, kLrec1Trailer + sizeof(kLrec1Trailer));
    PutU64(trailer, tick_count);
    PutU64(trailer, input_count);
    PutU64(trailer, checkpoint_count);
    out.write(reinterpret_cast<const char*>(trailer.data()),
              static_cast<std::streamsize>(trailer.size()));
    out.close();
    return out.good();
}

void BoundedReplayRecorder::Close() {
    // Windows forbids deleting open files; close before removing the segments.
    if (m_output.is_open())
        m_output.close();
    if (!m_directory.empty()) {
        std::error_code ec;
        fs::remove_all(m_directory, ec);
        m_directory.clear();
    }
    m_segments.clear();
    m_bytes = 0;
}

// --- QueuedReplayRecorder ----------------------------------------------------

struct QueuedReplayRecorder::State {
    explicit State(std::unique_ptr<ReplayTickSink> destination)
        : sink(std::move(destination)) {}

    bool
    Configure(std::size_t tick_byte_limit, std::uint64_t unconfirmed_ticks, QueueLimits limits);
    bool Open(const std::string& destination, const ReplayHeader& header);
    bool RecordTick(std::uint64_t tick,
                    std::shared_ptr<const std::vector<std::uint8_t>> inputs,
                    std::shared_ptr<const CheckpointRecord> checkpoint,
                    std::uint64_t confirmed_tick);
    bool Flush();
    bool Finalize(const std::string& destination,
                  std::uint64_t tick_count,
                  std::uint64_t divergence_tick);
    void Close();
    [[nodiscard]] const char* FailureReason() const;
    [[nodiscard]] std::size_t RetainedBytes() const;
    [[nodiscard]] std::size_t ByteLimit() const;
    [[nodiscard]] std::size_t SegmentCount() const;
    [[nodiscard]] std::size_t PendingTicks() const;
    [[nodiscard]] std::size_t PendingBytes() const;

private:
    enum class Failure : std::uint8_t {
        None,
        QueueFull,
        InvalidTick,
        Sink,
        Finalization
    };
    struct Tick {
        std::uint64_t tick = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> inputs;
        std::shared_ptr<const CheckpointRecord> checkpoint;
        std::uint64_t confirmed = 0;
        std::size_t bytes = 0;
    };

    // Requires mutex. Preserve the first failure even while draining accepted work.
    void Fail(Failure reason, std::uint64_t tick) {
        if (failure == Failure::None) {
            failure = reason;
            failure_tick = tick;
        }
    }

    void Run() {
        for (;;) {
            Tick next;
            {
                std::unique_lock lock(mutex);
                waiting = true;
                ready.wait(lock, [this] { return stopping || queued_ticks != 0; });
                waiting = false;
                if (queued_ticks == 0)
                    return;
                next = std::move(queue[queue_head]);
                queue[queue_head] = Tick{};
                queue_head = (queue_head + 1) % queue.size();
                --queued_ticks;
            }
            // The queue mutex is never held across sink I/O, encoding or digesting.
            // Virtual dispatch is once per whole tick, exclusively on this worker.
            bool recorded = false;
            try {
                std::array<std::string_view, 4> hashes{};
                if (next.checkpoint) {
                    const auto& hash = *next.checkpoint;
                    hashes = {hash.world_hash, hash.terrain, hash.water, hash.entities};
                }
                recorded = sink->RecordTick(
                    next.tick, *next.inputs, next.checkpoint ? &hashes : nullptr, next.confirmed);
            } catch (...) {
                // JobSystem catches exceptions too, but the lifecycle waiters must
                // always be notified and this artifact must be marked incomplete.
                recorded = false;
            }
            retained_bytes.store(sink->RetainedBytes(), std::memory_order_relaxed);
            segments.store(sink->SegmentCount(), std::memory_order_relaxed);
            const auto completed_bytes = next.bytes;
            const auto completed_tick = next.tick;
            // Release payload ownership before reporting the capacity available.
            next = Tick{};
            {
                const std::lock_guard lock(mutex);
                if (!recorded)
                    Fail(Failure::Sink, completed_tick);
                pending_bytes -= completed_bytes;
                --pending_ticks;
                if (pending_ticks == 0)
                    drained.notify_all();
            }
        }
    }

    void Stop() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        // JobSystem drains its accepted job and joins; Run drains its FIFO first.
        jobs.shutdown();
    }

    std::unique_ptr<ReplayTickSink> sink;
    JobSystem jobs;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable drained;
    // Preallocated at setup: at most 4096 fixed-size slots. Enqueue never allocates.
    std::vector<Tick> queue;
    std::size_t queue_head = 0;
    std::size_t queued_ticks = 0;
    std::size_t max_tick_bytes = 0;
    std::size_t queue_byte_limit = 0;
    std::size_t queue_tick_limit = 0;
    std::size_t pending_bytes = 0;
    std::size_t pending_ticks = 0;
    std::size_t byte_limit = 0;
    std::uint64_t last_tick = 0;
    std::uint64_t failure_tick = 0;
    std::atomic<std::size_t> retained_bytes{0};
    std::atomic<std::size_t> segments{0};
    Failure failure = Failure::None;
    bool open = false;
    bool stopping = false;
    bool waiting = false;
};

QueuedReplayRecorder::QueuedReplayRecorder()
    : QueuedReplayRecorder(std::make_unique<BoundedReplayRecorder>()) {}

QueuedReplayRecorder::QueuedReplayRecorder(std::unique_ptr<ReplayTickSink> sink)
    : m_state(std::make_unique<State>(std::move(sink))) {}

QueuedReplayRecorder::~QueuedReplayRecorder() {
    Close();
}

bool QueuedReplayRecorder::Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks) {
    return Configure(max_tick_bytes, unconfirmed_ticks, QueueLimits{});
}

bool QueuedReplayRecorder::State::Configure(std::size_t tick_byte_limit,
                                            std::uint64_t unconfirmed_ticks,
                                            QueueLimits limits) {
    auto& state = *this;
    if (state.open || !state.sink || limits.ticks == 0 || limits.ticks > kQueueTicks ||
        !state.sink->Configure(tick_byte_limit, unconfirmed_ticks))
        return false;
    state.max_tick_bytes = tick_byte_limit;
    state.queue_byte_limit =
        limits.bytes == 0 ? std::max(kQueueBytes, tick_byte_limit) : limits.bytes;
    state.queue_tick_limit = limits.ticks;
    state.queue.resize(limits.ticks);
    state.queue_head = 0;
    state.queued_ticks = 0;
    state.byte_limit = state.sink->ByteLimit();
    return true;
}

bool QueuedReplayRecorder::State::Open(const std::string& destination, const ReplayHeader& header) {
    auto& state = *this;
    if (state.open || state.max_tick_bytes == 0 || !state.sink->Open(destination, header))
        return false;
    state.stopping = false;
    state.failure = State::Failure::None;
    state.failure_tick = 0;
    state.last_tick = 0;
    state.jobs.startup(1);
    state.open = true;
    // One persistent consumer uses the existing engine worker infrastructure.
    // No std::function construction/dispatch occurs per tick.
    state.jobs.dispatch([&state] { state.Run(); });
    return true;
}

bool QueuedReplayRecorder::State::RecordTick(
    std::uint64_t tick,
    std::shared_ptr<const std::vector<std::uint8_t>> inputs,
    std::shared_ptr<const CheckpointRecord> checkpoint,
    std::uint64_t confirmed_tick) {
    auto& state = *this;
    // Validate exact wire size before retaining anything. Checkpoint validation is
    // four constant-time size checks, with no scans/copies of the encoded payload.
    bool valid = inputs && state.max_tick_bytes >= 17 &&
                 inputs->size() <= state.max_tick_bytes - 17 &&
                 inputs->size() <= (std::numeric_limits<std::uint32_t>::max)() - 4;
    std::size_t bytes = valid ? 17 + inputs->size() : 0;
    std::size_t storage = valid ? inputs->capacity() : 0;
    if (valid && checkpoint) {
        valid = checkpoint->tick == tick && state.max_tick_bytes - bytes >= 29;
        if (valid) {
            bytes += 29;
            std::size_t payload = 16;
            for (const auto* part : {&checkpoint->world_hash,
                                     &checkpoint->terrain,
                                     &checkpoint->water,
                                     &checkpoint->entities}) {
                if (part->size() > state.max_tick_bytes - bytes ||
                    part->size() > (std::numeric_limits<std::uint32_t>::max)() - payload ||
                    part->capacity() > (std::numeric_limits<std::size_t>::max)() - storage) {
                    valid = false;
                    break;
                }
                bytes += part->size();
                payload += part->size();
                storage += part->capacity();
            }
        }
    }
    // Charge capacity as well as wire size: a small view of a large allocation
    // must not evade the queue's byte budget. Fixed metadata is count-bounded.
    const auto charged = std::max(bytes, storage);
    const std::lock_guard lock(state.mutex);
    if (!state.open || state.stopping || state.failure != State::Failure::None)
        return false;
    if (!valid || tick <= state.last_tick) {
        state.Fail(State::Failure::InvalidTick, tick);
        return false;
    }
    if (state.pending_ticks == state.queue_tick_limit ||
        charged > state.queue_byte_limit - state.pending_bytes) {
        state.Fail(State::Failure::QueueFull, tick);
        return false;
    }
    const bool wake = state.waiting && state.queued_ticks == 0;
    const auto slot = (state.queue_head + state.queued_ticks) % state.queue.size();
    state.queue[slot] = {.tick = tick,
                         .inputs = std::move(inputs),
                         .checkpoint = std::move(checkpoint),
                         .confirmed = confirmed_tick,
                         .bytes = charged};
    ++state.queued_ticks;
    state.pending_bytes += charged;
    ++state.pending_ticks;
    state.last_tick = tick;
    if (wake)
        state.ready.notify_one();
    return true;
}

bool QueuedReplayRecorder::State::Flush() {
    auto& state = *this;
    std::unique_lock lock(state.mutex);
    state.drained.wait(lock, [&state] { return state.pending_ticks == 0; });
    return state.failure == State::Failure::None;
}

bool QueuedReplayRecorder::State::Finalize(const std::string& destination,
                                           std::uint64_t tick_count,
                                           std::uint64_t divergence_tick) {
    auto& state = *this;
    if (!state.open)
        return false;
    state.Stop();
    if (state.failure == State::Failure::None &&
        state.sink->Finalize(destination, tick_count, divergence_tick)) {
        // A later successful recording at the same destination supersedes any
        // earlier incomplete companion. All handles are closed before removal.
        std::error_code ec;
        fs::remove(destination + ".incomplete", ec);
        if (!ec)
            return true;
    }
    {
        const std::lock_guard lock(state.mutex);
        state.Fail(State::Failure::Finalization, divergence_tick);
    }
    // Never advertise a partial recording as a successful LREC1. The companion
    // is deliberately outside the versioned format and includes an explicit cause.
    std::ofstream reason(destination + ".incomplete", std::ios::trunc);
    reason << "Incomplete replay at tick " << state.failure_tick << ": " << FailureReason() << '\n';
    reason.close();
    LUMINUMBRA_CORE_ERROR("Replay recording incomplete at tick {}: {} ({})",
                          state.failure_tick,
                          FailureReason(),
                          destination);
    return false;
}

void QueuedReplayRecorder::State::Close() {
    auto& state = *this;
    if (!state.open)
        return;
    state.Stop();
    if (state.failure != State::Failure::None)
        LUMINUMBRA_CORE_ERROR(
            "Replay recording incomplete at tick {}: {}", state.failure_tick, FailureReason());
    state.sink->Close();
    state.retained_bytes.store(0, std::memory_order_relaxed);
    state.segments.store(0, std::memory_order_relaxed);
    state.open = false;
}

const char* QueuedReplayRecorder::State::FailureReason() const {
    const std::lock_guard lock(mutex);
    switch (failure) {
        case State::Failure::None:
            return "";
        case State::Failure::QueueFull:
            return "recorder queue capacity exceeded";
        case State::Failure::InvalidTick:
            return "invalid or oversized recorder tick";
        case State::Failure::Sink:
            return "recorder sink write failed";
        case State::Failure::Finalization:
            return "recorder finalization failed";
    }
    return "recorder failure";
}

std::size_t QueuedReplayRecorder::State::RetainedBytes() const {
    return retained_bytes.load(std::memory_order_relaxed);
}

std::size_t QueuedReplayRecorder::State::ByteLimit() const {
    return byte_limit;
}

std::size_t QueuedReplayRecorder::State::SegmentCount() const {
    return segments.load(std::memory_order_relaxed);
}

std::size_t QueuedReplayRecorder::State::PendingTicks() const {
    const std::lock_guard lock(mutex);
    return pending_ticks;
}

std::size_t QueuedReplayRecorder::State::PendingBytes() const {
    const std::lock_guard lock(mutex);
    return pending_bytes;
}

bool QueuedReplayRecorder::Configure(std::size_t max_tick_bytes,
                                     std::uint64_t unconfirmed_ticks,
                                     QueueLimits limits) {
    return m_state->Configure(max_tick_bytes, unconfirmed_ticks, limits);
}

bool QueuedReplayRecorder::Open(const std::string& destination, const ReplayHeader& header) {
    return m_state->Open(destination, header);
}

bool QueuedReplayRecorder::RecordTick(std::uint64_t tick,
                                      std::shared_ptr<const std::vector<std::uint8_t>> inputs,
                                      std::shared_ptr<const CheckpointRecord> checkpoint,
                                      std::uint64_t confirmed_tick) {
    return m_state->RecordTick(tick, std::move(inputs), std::move(checkpoint), confirmed_tick);
}

bool QueuedReplayRecorder::Flush() {
    return m_state->Flush();
}

bool QueuedReplayRecorder::Finalize(const std::string& destination,
                                    std::uint64_t tick_count,
                                    std::uint64_t divergence_tick) {
    return m_state->Finalize(destination, tick_count, divergence_tick);
}

void QueuedReplayRecorder::Close() {
    m_state->Close();
}

const char* QueuedReplayRecorder::FailureReason() const {
    return m_state->FailureReason();
}

std::size_t QueuedReplayRecorder::RetainedBytes() const {
    return m_state->RetainedBytes();
}

std::size_t QueuedReplayRecorder::ByteLimit() const {
    return m_state->ByteLimit();
}

std::size_t QueuedReplayRecorder::SegmentCount() const {
    return m_state->SegmentCount();
}

std::size_t QueuedReplayRecorder::PendingTicks() const {
    return m_state->PendingTicks();
}

std::size_t QueuedReplayRecorder::PendingBytes() const {
    return m_state->PendingBytes();
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
        } else if (type == RecordType::ElidedPrefix &&
                   contents.header.version == kLrec1ElidedVersion && contents.inputs.empty() &&
                   contents.checkpoints.empty() && contents.elided_through_tick == 0 && tick != 0 &&
                   payload_len == 8) {
            if (!payload.GetU64(contents.elided_prefix_digest)) {
                contents.truncated = true;
                break;
            }
            contents.elided_through_tick = tick;
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
