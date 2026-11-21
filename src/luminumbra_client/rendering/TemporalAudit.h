#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::Rendering {

enum class TemporalAuditSpan {
    DayNight,
    WeatherCycle,
    MultiDay
};
enum class TemporalAuditControl {
    Live,
    Noon,
    Night
};
enum class TemporalAuditFrameKind {
    Sequence,
    SteadyHold,
    NightPresenceHold
};

inline constexpr std::uint64_t kTemporalMaxTicks = 10'000'000;
inline constexpr std::size_t kTemporalMaxFrames = 4096;
inline constexpr std::uint32_t kTemporalPresentationHz = 60;
inline constexpr std::uint32_t kTemporalBurstFrames = 120;

struct TemporalAuditRequest {
    TemporalAuditSpan span = TemporalAuditSpan::DayNight;
    std::uint64_t start_tick = 0;
    std::uint64_t ticks_per_day = 0;
    std::uint32_t ticks_per_second = 0;
    // Resolved calendar crossings, as offsets within a midnight-origin day.
    std::uint64_t dawn_tick = 0;
    std::uint64_t dusk_tick = 0;
    // Weather-only offset from start, in the storm interval [50T, 90T).
    std::uint64_t strike_tick = 0;
    std::uint32_t night_moon_phase_milli = 1000;
    // Callers may lower these limits, never raise the hard ceilings.
    std::uint64_t tick_limit = kTemporalMaxTicks;
    std::size_t frame_limit = kTemporalMaxFrames;
    bool operator==(const TemporalAuditRequest&) const = default;
};

struct TemporalAuditFrame {
    std::uint64_t target_tick = 0;
    // Exact fraction of a simulation tick, denominator 60. Never rounded to dt.
    std::uint32_t sub_tick_60 = 0;
    TemporalAuditFrameKind kind = TemporalAuditFrameKind::Sequence;
    TemporalAuditControl control = TemporalAuditControl::Live;
    bool dense = false;
    // Holds keep target_tick fixed and present at hold_frame / 60 seconds.
    std::uint32_t hold_frame = 0;
    bool operator==(const TemporalAuditFrame&) const = default;
};

struct TemporalAuditSettling {
    std::uint32_t warmup_frames = 90;
    std::uint32_t initial_readiness_retries = 300;
    std::uint32_t sample_settle_frames = 3;
    std::uint32_t sample_readiness_retries = 60;
    // Warmup uses 1/60 s steps with simulation frozen. Readiness and sample
    // settling hold both simulation and presentation phase. Dense frames retain
    // history and omit sample settling; retries never advance/age producers.
    bool operator==(const TemporalAuditSettling&) const = default;
};

struct TemporalAuditPlan {
    TemporalAuditRequest request;
    std::uint64_t end_tick = 0;
    std::uint64_t sparse_interval_ticks = 0;
    std::vector<std::uint64_t> boundary_ticks;
    // At most 4096 entries. Admission checks candidate counts before allocation.
    std::vector<TemporalAuditFrame> frames;
    TemporalAuditSettling settling;
    // Upper bounds include unsaved presentation substeps, warmup, settling and
    // every readiness retry, including the two labelled control holds.
    std::uint64_t presentation_steps = 0;
    std::uint64_t max_render_attempts = 0;
    bool operator==(const TemporalAuditPlan&) const = default;
};

struct TemporalAuditAdmission {
    std::optional<TemporalAuditPlan> plan;
    std::string reason;
};

// Pure plan admission: no world, weather, file, clock or render side effects.
TemporalAuditAdmission BuildTemporalAuditPlan(const TemporalAuditRequest& request);

// O(1) exact advancement query. Execute every returned tick through normal sim;
// frame pacing supplies no additional ticks. Refuses overshoot, invalid indices
// and clocks preceding initialization. The sequence protocol below owns retries
// and single-tick progression; the app supplies simulation and capture transport.
std::optional<std::uint64_t> TemporalAuditTicksToFrame(const TemporalAuditPlan& plan,
                                                       std::size_t frame_index,
                                                       std::uint64_t current_tick);

inline constexpr std::uint64_t kTemporalMaxOutputBytes = 512ULL * 1024 * 1024 * 1024;

// The transport must supply and enforce upper bounds covering every frame's
// pixels, diagnostic bundle and metadata, plus the final sequence manifest.
struct TemporalSequenceBudget {
    std::uint64_t frame_bytes = 0;
    std::uint64_t manifest_bytes = 0;
    std::uint64_t output_bytes = kTemporalMaxOutputBytes;
};

enum class TemporalSequenceAction {
    Warmup,
    InitialReadiness,
    AdvanceTick,
    Present,
    Settle,
    Readiness,
    Capture,
    Publish,
    Complete,
    Refused
};

struct TemporalSequenceStep {
    TemporalSequenceAction action = TemporalSequenceAction::Refused;
    // AdvanceTick requests exactly this next tick; all other actions freeze it.
    std::uint64_t simulation_tick = 0;
    std::uint32_t sub_tick_60 = 0;
    std::size_t frame_index = 0;
    double presentation_delta_seconds = 0;
    // Midnight-origin calendar fraction. The owner resolves solar TOD through
    // its WorldClock, then applies the explicit noon/night render-only control.
    double day_phase = 0;
    TemporalAuditControl control = TemporalAuditControl::Live;
    std::uint64_t byte_limit = 0;
    bool operator==(const TemporalSequenceStep&) const = default;
};

// GL-free execution protocol. Admit before calling any transport/recorder. The
// owner executes each step once, acknowledges the actual simulation clock, and
// keeps payloads unpublished until Publish. Capture uses the most recent held
// readiness render through the completed-frame adapter. On refusal the owner
// abandons its reservation. The basic query/acknowledgement protocol performs no
// I/O and is O(1), with no per-step allocation. The optional file transport below
// hashes bounded payloads at capture and serializes once, at publication.
class TemporalAuditSequence {
public:
    TemporalAuditSequence(const TemporalAuditPlan& plan, const TemporalSequenceBudget& budget);
    TemporalSequenceStep next(std::uint64_t actual_tick);
    void acknowledge(std::uint64_t actual_tick,
                     bool success,
                     bool ready = true,
                     std::uint64_t written_bytes = 0);
    void refuse();
    // Optional filesystem transport for completed-frame captures. Names are
    // lowercase ASCII leaves beginning "frame-<frame_index>."; at most 12 nonempty payloads,
    // each <= 128 MiB, within the already admitted aggregate frame reservation.
    // Hashes come from the same verified open handle as the payload bytes.
    // All captures and the manifest must share one immutable directory.
    // Failure is sticky; no files are created by capture acknowledgement.
    bool acknowledgeCaptureFiles(std::uint64_t actual_tick,
                                 const std::filesystem::path& directory,
                                 std::span<const std::string> filenames);
    // Schema/version 1; empty until every capture has a successful file-backed
    // acknowledgement, or after refusal/cancellation. No wall-clock fields.
    [[nodiscard]] std::string completionManifest() const;
    // Only a pending Publish can write. Manifest names are lowercase ASCII
    // leaves <= 128 bytes, excluding device stems; paths cannot contain NUL.
    // Existing parent required; exclusively
    // create a sibling .tmp, close it, then rename without replacing old output.
    // A failed publication leaves no newly published complete manifest.
    bool publishCompletionManifest(const std::filesystem::path& path);
    [[nodiscard]] std::string_view refusal() const {
        return refusal_;
    }
    [[nodiscard]] bool complete() const {
        return stage_ == TemporalSequenceAction::Complete;
    }
    [[nodiscard]] std::size_t captured_frames() const {
        return frame_index_;
    }

private:
    struct Payload {
        std::string path;
        std::string sha256;
        std::uint64_t bytes = 0;
    };
    void beginFrame();
    void beginSample();
    void fail(std::string_view reason);
    TemporalSequenceStep step(TemporalSequenceAction action,
                              std::uint64_t tick,
                              std::uint64_t phase_60,
                              double delta) const;

    TemporalAuditPlan plan_;
    TemporalSequenceBudget budget_;
    TemporalSequenceAction stage_ = TemporalSequenceAction::Refused;
    std::optional<TemporalSequenceStep> pending_;
    std::string_view refusal_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t phase_60_ = 0; // elapsed tick numerator; denominator is 60
    std::uint64_t next_phase_60_ = 0;
    std::uint64_t render_attempts_ = 0;
    std::size_t frame_index_ = 0;
    std::uint32_t attempts_ = 0;
    // At most kTemporalMaxFrames entries, each containing at most 12 payloads.
    std::vector<std::vector<Payload>> payloads_;
    std::filesystem::path payload_directory_;
};

} // namespace Luminumbra::Rendering
