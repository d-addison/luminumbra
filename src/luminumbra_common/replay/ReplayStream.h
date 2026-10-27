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

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../HashCheckpoint.h"

namespace Luminumbra::Replay {

// On-wire constants. Bump kLrec1Version on ANY incompatible layout change and
// move the gate goldens deliberately in the same commit (determinism discipline
// section 13: replay LREC1 is versioned).
inline constexpr char kLrec1Magic[5] = {'L', 'R', 'E', 'C', '1'};
inline constexpr std::uint16_t kLrec1Version = 3;
// Version 3 remains byte-identical for complete streams. Only diagnostic
// streams with ElidedPrefix require version 4; readers accept both layouts.
inline constexpr std::uint16_t kLrec1ElidedVersion = 4;
inline constexpr char kLrec1Trailer[8] = {'L', 'R', 'E', 'C', 'E', 'N', 'D', '1'};

// Record type tags (u8 on the wire).
enum class RecordType : std::uint8_t {
    Input = 0x01,        // per-tick input set (may be empty -- valid + compact)
    Checkpoint = 0x02,   // world_hash + authoritative sub-hashes at a tick
    ElidedPrefix = 0x03, // version 4 diagnostic suffix: tick and digest of removed prefix
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
    std::string system_config_identity;
    // Canonical resolved preset plus sorted data_root-relative dependency paths
    // and exact-byte file hashes (TerrainPresetLoadResult::preset_content_hash).
    std::string preset_content_hash;
};

// Required in addition to the existing start/checkpoint world-hash comparisons.
[[nodiscard]] inline bool ConfigurationMatches(const ReplayHeader& header,
                                               const std::string& live_identity,
                                               const std::string& live_preset_hash) {
    return (header.version == kLrec1Version || header.version == kLrec1ElidedVersion) &&
           !live_identity.empty() && !header.system_config_identity.empty() &&
           header.system_config_identity == live_identity && !live_preset_hash.empty() &&
           !header.preset_content_hash.empty() && header.preset_content_hash == live_preset_hash;
}

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
using CheckpointRecord = HashCheckpoint;

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

// Automatic lockstep diagnostics. Buffered disk segments retain all unconfirmed
// ticks and a rolling suffix (1024 ticks or 64 MiB, whichever is reached first).
// Segments contain at most 1024 ticks / 64 MiB, except a single larger tick.
// No simulation callback or per-record flush is needed. Segments are closed
// before deletion on every platform; destruction removes the working directory.
// Finalize writes a normal LREC1, with an explicit ElidedPrefix record when the
// seed alone cannot reconstruct the suffix. Such an artifact is diagnostic only.
// A whole-tick sink. Calls are serialized by QueuedReplayRecorder; implementations
// may perform blocking I/O here, never in the simulation's enqueue operation.
class ReplayTickSink {
public:
    virtual ~ReplayTickSink() = default;
    virtual bool Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks) = 0;
    virtual bool Open(const std::string& destination, const ReplayHeader& header) = 0;
    virtual bool RecordTick(std::uint64_t tick,
                            const std::vector<std::uint8_t>& inputs,
                            const std::array<std::string_view, 4>* hashes,
                            std::uint64_t confirmed_tick) = 0;
    virtual bool Finalize(const std::string& destination,
                          std::uint64_t tick_count,
                          std::uint64_t divergence_tick) = 0;
    virtual void Close() = 0;
    [[nodiscard]] virtual std::size_t RetainedBytes() const = 0;
    [[nodiscard]] virtual std::size_t ByteLimit() const = 0;
    [[nodiscard]] virtual std::size_t SegmentCount() const = 0;
    // Bounded text naming the step that refused the most recent Open and the OS
    // error it saw. Empty before the first Open and after every successful Open.
    [[nodiscard]] virtual std::string_view OpenRefusal() const {
        return {};
    }
};

class BoundedReplayRecorder final : public ReplayTickSink {
public:
    static constexpr std::size_t kSegmentTicks = 1024;
    static constexpr std::size_t kSegmentBytes = std::size_t{64} * 1024 * 1024;
    // Fixed capacity of the Open refusal text, including its terminator. Paths
    // contribute only their tail so one refusal never allocates or grows.
    static constexpr std::size_t kOpenRefusalBytes = 256;

    BoundedReplayRecorder() = default;
    ~BoundedReplayRecorder() override;
    BoundedReplayRecorder(const BoundedReplayRecorder&) = delete;
    BoundedReplayRecorder& operator=(const BoundedReplayRecorder&) = delete;

    // Checks the disk bound before any recording allocation: unconfirmed_ticks
    // * max_tick_bytes + three segments (a segment may hold one maximum tick).
    bool Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks) override;
    bool Open(const std::string& destination, const ReplayHeader& header) override;
    bool RecordTick(std::uint64_t tick,
                    const std::vector<std::uint8_t>& inputs,
                    const std::array<std::string_view, 4>* hashes,
                    std::uint64_t confirmed_tick) override;
    bool Finalize(const std::string& destination,
                  std::uint64_t tick_count,
                  std::uint64_t divergence_tick) override;
    void Close() override;

    [[nodiscard]] std::size_t RetainedBytes() const override {
        return m_bytes;
    }
    [[nodiscard]] std::size_t ByteLimit() const override {
        return m_byte_limit;
    }
    [[nodiscard]] std::size_t SegmentCount() const override {
        return m_segments.size();
    }
    [[nodiscard]] std::string_view OpenRefusal() const override {
        return {m_open_refusal.data(), m_open_refusal_length};
    }

private:
    struct Segment {
        std::string path;
        std::uint64_t last_tick = 0;
        std::uint64_t digest = 0;
        std::size_t bytes = 0;
        std::size_t ticks = 0;
    };
    bool Prune(std::uint64_t tick, std::uint64_t confirmed_tick);
    // Records the length of the refusal text just formatted into m_open_refusal
    // and returns false so Open can refuse in one statement.
    bool Refused(int formatted);

    ReplayHeader m_header;
    std::string m_directory;
    std::ofstream m_output;
    std::deque<Segment> m_segments;
    std::size_t m_max_tick_bytes = 0;
    std::size_t m_byte_limit = 0;
    std::size_t m_bytes = 0;
    std::uint64_t m_digest = 1469598103934665603ULL;
    std::uint64_t m_elided_tick = 0;
    std::uint64_t m_elided_digest = 1469598103934665603ULL;
    std::uint64_t m_last_tick = 0;
    std::array<char, kOpenRefusalBytes> m_open_refusal{};
    std::size_t m_open_refusal_length = 0;
};

// Ordered recording on one existing JobSystem worker. Open is a setup operation;
// RecordTick only retains immutable, already-encoded inputs and checkpoint strings.
// Outstanding work (including the active tick) is bounded by both 4096 ticks and
// max(64 MiB, maximum encoded tick) bytes, plus fixed-size queue metadata. Full
// queues latch failure without waiting: simulation never depends on disk progress.
// Finalize drains accepted ticks and reports failure with a .incomplete companion
// containing the first rejected/failed tick and reason. Close drains then removes
// working files; an in-sync shutdown does not publish an artifact.
class QueuedReplayRecorder {
public:
    static constexpr std::size_t kQueueTicks = 4096;
    static constexpr std::size_t kQueueBytes = std::size_t{64} * 1024 * 1024;
    struct QueueLimits {
        std::size_t bytes = 0;
        std::size_t ticks = kQueueTicks;
    };

    QueuedReplayRecorder();
    explicit QueuedReplayRecorder(std::unique_ptr<ReplayTickSink> sink);
    ~QueuedReplayRecorder();
    QueuedReplayRecorder(const QueuedReplayRecorder&) = delete;
    QueuedReplayRecorder& operator=(const QueuedReplayRecorder&) = delete;

    // Optional smaller queue budgets are useful for callers with tighter memory
    // limits. Zero bytes selects the default above; ticks must be in [1,4096].
    bool Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks);
    bool Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks, QueueLimits limits);
    bool Open(const std::string& destination, const ReplayHeader& header);
    bool RecordTick(std::uint64_t tick,
                    std::shared_ptr<const std::vector<std::uint8_t>> inputs,
                    std::shared_ptr<const CheckpointRecord> checkpoint,
                    std::uint64_t confirmed_tick);
    // Explicit blocking lifecycle barrier, never called by the tick path.
    bool Flush();
    bool Finalize(const std::string& destination,
                  std::uint64_t tick_count,
                  std::uint64_t divergence_tick);
    void Close();

    [[nodiscard]] const char* FailureReason() const;
    // Why the most recent Open was refused: this recorder's own precondition or
    // the sink's bounded refusal text. Empty before the first and after a
    // successful Open. Read it on the thread that called Open.
    [[nodiscard]] std::string_view OpenRefusal() const;
    // Const snapshots published after each whole-tick sink operation. Flush
    // before comparing them with files on disk.
    [[nodiscard]] std::size_t RetainedBytes() const;
    [[nodiscard]] std::size_t ByteLimit() const;
    [[nodiscard]] std::size_t SegmentCount() const;
    [[nodiscard]] std::size_t PendingTicks() const;
    [[nodiscard]] std::size_t PendingBytes() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
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
    std::uint64_t elided_through_tick = 0;     // nonzero => diagnostic suffix only
    std::uint64_t elided_prefix_digest = 0;    // FNV-1a over elided record bytes
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
