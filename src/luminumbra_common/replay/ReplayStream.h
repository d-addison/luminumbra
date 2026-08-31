#pragma once

// LREC1 session replay stream (engine-generic; NO client/game deps).
//
// WHY THIS EXISTS (research Area 2 takeaway 8): deterministic lockstep makes
// session replay nearly free -- a recording is just (world boot parameters +
// the per-tick input stream + periodic world_hash checkpoints). The checkpoints
// make a replay SELF-VERIFYING: replaying the recorded inputs from the recorded
// boot parameters must reproduce the exact same world_hash at every checkpoint;
// the first mismatch localizes a desync to a tick (and, via the  sub-
// hashes, to a subsystem). This is precisely Factorio's primary desync-repro
// tool, and  (lockstep transport) will dump LREC1 streams on desync.
//
// DESIGN INVARIANTS (binding, from the deterministic runtime contract sections 7 + 13):
//  - TICK-INDEXED records, NEVER frame-indexed (regression review): the simulation
//    clock's wall-clock catch-up / frame-drop telemetry is excluded by design.
//    A record's tick index is the authoritative simulation tick it applies to.
//  - Deterministic serialization: fixed-width little-endian integers only, no
//    platform-dependent types (no size_t/long on the wire, no struct padding,
//    no float bit-twiddling -- hashes travel as their hex strings).
//  - Append-on-record, sequential read on playback.
//  - Truncation-robust: every record is length-framed AND the stream carries a
//    trailer; a truncated tail is DETECTED (a short/again-truncated read fails
//    loudly) rather than silently accepted.
//  - Versioned: LREC1 streams stamp the engine version; playback refuses a
//    version mismatch loudly (Factorio replays break silently across versions;
//    we refuse instead).
//
// The format is engine-generic: it knows about ticks, opaque per-tick input
// blobs, and hash checkpoints. It knows NOTHING about voxels, biomes, cameras,
// or any game concept.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Luminumbra::Replay {

// On-wire constants. Bump kLrec1Version on ANY incompatible layout change and
// move the gate goldens deliberately in the same commit (determinism discipline
// section 13: replay LREC1 is versioned).
inline constexpr char kLrec1Magic[5] = {'L', 'R', 'E', 'C', '1'};
inline constexpr std::uint16_t kLrec1Version = 1;
inline constexpr char kLrec1Trailer[8] = {'L', 'R', 'E', 'C', 'E', 'N', 'D', '1'};

// Record type tags (u8 on the wire).
enum class RecordType : std::uint8_t {
    Input = 0x01,      // per-tick input set (may be empty -- valid + compact)
    Checkpoint = 0x02, // world_hash + authoritative sub-hashes at a tick
};

// Session boot parameters -- everything ServerWorldRunner boots from, so a
// replay can reconstruct the identical session. Stored in the LREC1 header.
struct ReplayHeader {
    std::uint16_t version = kLrec1Version;
    std::uint16_t tick_rate_hz = 30; // canonical fixed tick rate
    // Numeric seed (GameSession::StringToSeed of seed_string) -- the value the
    // worldgen RNG is actually seeded with. seed_string is also stored so the
    // boot path (which takes the string) is reproduced exactly.
    std::uint64_t seed = 0;
    std::string seed_string;
    std::string preset;            // preset name (worlds/atlas/presets/<preset>.json)
    std::uint64_t preset_hash = 0; // fnv1a64 of the preset name (provenance; bump-detect)
    std::string start_world_hash;  // world_hash at tick 0 (post-boot, pre-tick)
    std::uint32_t surface_radius = 4;
    std::uint32_t collision_radius = 2;
    std::string engine_version; // luminumbra::core::GetEngineVersionString()
};

// One per-tick input record. The headless smoke has no player inputs today, so
// `inputs` is typically empty -- the format still carries the (empty) set so the
// stream shape is identical once  feeds real inputs. `inputs` is an
// OPAQUE engine-generic blob: the input encoding is the transport's concern.
struct InputRecord {
    std::uint64_t tick = 0;
    std::vector<std::uint8_t> inputs; // opaque; empty is valid and compact
};

// One checkpoint record: the world_hash + authoritative sub-hashes at a tick.
// Mesh is EXCLUDED per the  caveat (it is a derived render artifact that
// legitimately differs across save/load; authoritative state = terrain/water/
// entities). Hashes are stored as their hex strings (no float/endianness risk).
struct CheckpointRecord {
    std::uint64_t tick = 0;
    std::string world_hash;
    std::string terrain;
    std::string water;
    std::string entities;
};

// --- Writer: append-on-record. ----------------------------------------------
// Usage: construct with the header (writes it immediately), RecordInput once per
// tick, RecordCheckpoint at checkpoint ticks, then Finalize to patch the tick
// count and append the trailer. Recording IO is buffered in memory and flushed
// to disk by Finalize/Close so it never sits on the simulation hot path.
class ReplayWriter {
public:
    ReplayWriter() = default;

    // Begins a new stream into `path`, writing the header. Returns false on IO
    // failure. The path's parent directory is created if needed.
    bool Open(const std::string& path, const ReplayHeader& header);

    // Appends a per-tick input record. `inputs` empty is valid.
    void RecordInput(std::uint64_t tick, const std::vector<std::uint8_t>& inputs);

    // Appends a checkpoint record.
    void RecordCheckpoint(const CheckpointRecord& checkpoint);

    // Patches the header tick_count, appends the trailer, and flushes to disk.
    // Returns false on IO failure. After Finalize the writer is closed.
    bool Finalize(std::uint64_t tick_count);

    [[nodiscard]] std::uint64_t InputRecordCount() const {
        return m_input_count;
    }
    [[nodiscard]] std::uint64_t CheckpointRecordCount() const {
        return m_checkpoint_count;
    }

private:
    std::string m_path;
    std::vector<std::uint8_t> m_buffer; // header + records, flushed on Finalize
    std::uint64_t m_input_count = 0;
    std::uint64_t m_checkpoint_count = 0;
    bool m_open = false;
};

// --- Reader: sequential read on playback. -----------------------------------
// Parses the whole stream up front (replay files are small: ~tens of bytes per
// tick) and exposes the header, the ordered records, and a truncation flag.
struct ReplayContents {
    ReplayHeader header;
    std::vector<InputRecord> inputs;           // tick-ordered as recorded
    std::vector<CheckpointRecord> checkpoints; // tick-ordered as recorded
    std::uint64_t tick_count = 0;              // from the trailer
    bool truncated = false;                    // tail did not parse cleanly
    bool trailer_present = false;              // a valid trailer was found
};

// Reads and parses an LREC1 stream. Returns nullopt only if the file cannot be
// opened or the HEADER is malformed (not a valid LREC1 stream at all). A stream
// whose RECORD region is truncated still parses what it can and sets
// `truncated=true` / `trailer_present=false` -- callers MUST check those.
std::optional<ReplayContents> ReadReplay(const std::string& path);

// Returns the input record for `tick`, or nullptr if none was recorded. O(n)
// scan; replay drives ticks in order so callers typically iterate `inputs`.
const InputRecord* FindInput(const ReplayContents& contents, std::uint64_t tick);

// Returns the checkpoint record for `tick`, or nullptr if none was recorded.
const CheckpointRecord* FindCheckpoint(const ReplayContents& contents, std::uint64_t tick);

// fnv1a-64 over bytes, returned as a 16-char lowercase hex string. Shared so the
// header preset_hash uses the same stable algorithm as the world hashes.
std::string Fnv1a64Hex(const std::string& text);

} // namespace Luminumbra::Replay
