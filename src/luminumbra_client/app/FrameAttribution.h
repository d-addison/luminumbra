#pragma once

#include "../rendering/UploadBudget.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <optional>
#include <ratio>

namespace Luminumbra::Client::Attribution {

// Components mirror the quantized scatter signature for render diagnostics.
// Foliage owns rebuild admission; these values never feed simulation state.
struct ScatterSignature {
    std::uint64_t config = 0;
    std::uint64_t camera_cell = 0;
    std::uint64_t ground_revision = 0; // folded revisions of contributing support columns
    std::uint64_t scatter_chunks = 0;
};
enum RebuildReason : std::uint32_t {
    Initial = 1u << 0,
    Config = 1u << 1,
    CameraCell = 1u << 2,
    GroundRevision = 1u << 3,
    ScatterChunks = 1u << 4,
    Backlog = 1u << 5
};
std::uint32_t classify_rebuild(const ScatterSignature& previous,
                               const ScatterSignature& current,
                               bool built,
                               bool backlog);

// The private indirect-draw guard can hold capacity. Never expose that marker
// as a generated count, even if an older readback snapshot still exists.
std::optional<std::size_t> generated_instance_count(bool gpu_active,
                                                    bool readback_enabled,
                                                    bool completed_readback,
                                                    std::size_t draw_limit);

// Static tokens only: safe CSV fields, no quoting or per-frame string allocation.
enum class FoliagePath {
    NotCalled,
    ReusedGpu,
    ReusedCpu,
    Gpu,
    Cpu,
    Inactive,
    FrozenGpu,
    Mixed
};
enum class FallbackReason {
    None,
    GpuDisabled,
    MissingResources,
    ColumnCapacity,
    Mixed
};
const char* path_name(FoliagePath path);
const char* fallback_name(FallbackReason reason);
struct FoliageRebuild {
    double signature_ms = 0;
    double sort_ms = 0;
    double surface_ms = 0;
    double repack_ms = 0;
    double hash_ms = 0;
    double upload_ms = 0;
    double palette_scratch_ms = 0;
    double dispatch_ms = 0;
    // Inclusive of CPU surface misses and mapped copies; other stages are disjoint.
    double cpu_fallback_ms = 0;
    std::uint64_t rebuild_calls = 0;
    std::uint32_t reasons = 0;
    std::uint64_t admitted_columns = 0;
    std::uint64_t surface_misses = 0;
    std::uint64_t surface_samples = 0;
    std::uint64_t deferred_columns = 0;
    std::uint64_t copied_bytes = 0;
    std::uint64_t hashed_bytes = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t dirty_columns = 0;
    std::uint64_t worker_dispatched = 0;
    std::uint64_t worker_completed = 0;
    std::uint64_t worker_stale_rejected = 0;
    // Oldest outstanding or consumed job, in frames, including worker service.
    std::uint64_t worker_queue_age_frames = 0;
    std::uint64_t uploads = 0;
    std::uint64_t stale_results = 0;
    // Storage allocated with null glBufferData, separate from transferred bytes.
    std::uint64_t scratch_bytes = 0;
    bool backlog = false;
    FoliagePath path = FoliagePath::NotCalled;
    FallbackReason fallback = FallbackReason::None;
};

struct RenderUploads {
    Rendering::UploadWorkStats terrain{};
    Rendering::UploadWorkStats water{};
    std::size_t terrain_pending = 0;
    std::size_t water_pending = 0;
};
// Every rebuild and render integration on the render thread contributes,
// including preview and QA callers. Storage is constant size.
struct FoliageFrame {
    RenderUploads uploads{};
    FoliageRebuild rebuild{};
    double rebuild_ms = 0;
    std::uint64_t capacity = 0;
    bool count_known = false;
    std::uint64_t generated_count = 0;
    std::uint64_t count_generation = 0;
};
class FoliageFrameScope {
public:
    explicit FoliageFrameScope(bool enabled = false);
    ~FoliageFrameScope();
    FoliageFrameScope(const FoliageFrameScope&) = delete;
    FoliageFrameScope& operator=(const FoliageFrameScope&) = delete;
    const FoliageFrame& snapshot() const {
        return m_frame;
    }

private:
    // Rebuilds update the owned storage even when the RAII guard is const.
    mutable FoliageFrame m_frame{};
    FoliageFrame* m_previous = nullptr;
};
bool foliage_frame_active();
void record_render_uploads(const Rendering::UploadWorkStats& work, bool water, std::size_t pending);
void record_foliage_rebuild(const FoliageRebuild& rebuild,
                            double milliseconds,
                            std::uint64_t capacity,
                            std::optional<std::size_t> count,
                            std::uint64_t count_generation);

class StageTimer {
public:
    explicit StageTimer(double& milliseconds, bool enabled)
        : m_ms(milliseconds)
        , m_running(enabled) {
        if (m_running)
            m_start = std::chrono::steady_clock::now();
    }
    ~StageTimer() {
        stop();
    }
    void stop() {
        if (m_running) {
            m_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              m_start)
                        .count();
            m_running = false;
        }
    }
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;

private:
    double& m_ms;
    std::chrono::steady_clock::time_point m_start{};
    bool m_running;
};

// Remove nested foliage rebuild time from a timed render/menu scope.
double exclusive_parent_ms(double parent_ms, double nested_before_ms, double nested_after_ms);

// CPU wall time only. Nested phases are exclusive, including synchronous work
// dispatched by the window-system event pump. No GL waits are introduced.
enum class Phase : std::uint8_t {
    UiUpdate,
    UiRender,
    UiFilters,
    MenuScene,
    PreviewAdoption,
    DocumentLoad,
    TextureLoad,
    ShaderLoad,
    ShaderFirstUse,
    WorldCreateCallback,
    WorldLoadCallback,
    Count
};
constexpr std::size_t kPhaseCount = static_cast<std::size_t>(Phase::Count);
const char* phase_name(Phase phase);
class PhaseTotals {
public:
    const std::array<double, kPhaseCount>& milliseconds() const {
        return m_ms;
    }
    double sum() const;
    // Const runtime diagnostic: completed scopes, independent of clock precision.
    std::uint64_t completed_scopes(Phase phase) const {
        return m_scopes.at(static_cast<std::size_t>(phase));
    }
    // Const diagnostic for checking inclusive/exclusive phase conservation.
    double inclusive_milliseconds(Phase phase) const {
        return m_inclusive_ms.at(static_cast<std::size_t>(phase));
    }
    // Child scopes include their foliage in child_ms. Remove only foliage
    // belonging directly to this phase, preserving the existing foliage split.
    void add(Phase phase, double inclusive_ms, double child_ms, double direct_foliage_ms);

private:
    std::array<double, kPhaseCount> m_ms{};
    std::array<std::uint64_t, kPhaseCount> m_scopes{};
    std::array<double, kPhaseCount> m_inclusive_ms{};
};
class PhaseFrameScope {
public:
    explicit PhaseFrameScope(bool enabled = false);
    ~PhaseFrameScope();
    PhaseFrameScope(const PhaseFrameScope&) = delete;
    PhaseFrameScope& operator=(const PhaseFrameScope&) = delete;
    const PhaseTotals& snapshot() const {
        return m_totals;
    }
    // Const diagnostic for nested collector ownership on the calling thread.
    bool is_current() const;

private:
    // Completed phases update the owned storage even when the RAII guard is const.
    mutable PhaseTotals m_totals{};
    PhaseTotals* m_previous = nullptr;
};
// The client and static-preview host each have one owning render thread/context.
// This flag is written by that thread's frame collector only. Keep the GL fast
// path independent of TLS initialization and phase-stack access.
inline constinit bool shader_first_use_collection_enabled = false;
// Completed exclusive phases, also usable inside an open enclosing phase.
double phase_total_ms();
class PhaseScope {
public:
    explicit PhaseScope(Phase phase, bool enabled = true);
    ~PhaseScope();
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;
    // Const diagnostic for the active phase stack on the calling thread.
    bool is_current() const;

private:
    Phase m_phase;
    PhaseTotals* m_totals = nullptr;
    const PhaseScope* m_parent = nullptr;
    std::chrono::steady_clock::time_point m_start{};
    // Nested scopes update these even when the enclosing RAII guard is const.
    mutable double m_children_ms = 0;
    mutable double m_child_foliage_ms = 0;
    double m_foliage_before = 0;
};

struct FrameBuckets {
    double sim = 0;
    double stream = 0;
    double render = 0;
    double present = 0;
    double foliage = 0;
    double scatter = 0;
    double rebake = 0;
    double poll = 0;
    double menu = 0;
    double ui = 0;
    double sum() const;
    // All supplied scopes must be disjoint and inside the measured frame.
    double other(double frame_ms, const PhaseTotals& phases = {}) const {
        return frame_ms - sum() - phases.sum();
    }
};

// CSV sinks use the classic locale; the recorder configures it once on open.
void write_csv_header(std::ostream& out);
// Returns false on an unavailable or failed sink; failures remain sticky.
bool write_csv_row(std::ostream& out,
                   std::uint64_t frame,
                   double frame_ms,
                   const FrameBuckets& buckets,
                   const FoliageRebuild& foliage,
                   std::uint64_t capacity,
                   bool count_known,
                   std::uint64_t generated_count,
                   std::uint64_t count_generation,
                   const PhaseTotals& phases = {},
                   const RenderUploads& uploads = {});

// Flush and close an open CSV file, preserving write, flush, and close failures.
// Uses the default non-throwing stream exception mask, as do the CSV writers.
bool finalize_csv(std::ofstream& out);

class FrameRecorder {
public:
    FrameRecorder(bool enabled, const std::filesystem::path& logs);
    bool active() const {
        return m_out.is_open() && m_out.good();
    }
    // Call before shutdown to observe buffered write and close failures. Repeated
    // calls return the same result; a disabled recorder finishes successfully.
    bool finish();
    bool record(std::uint64_t frame,
                double frame_ms,
                const FrameBuckets& buckets,
                const FoliageRebuild& foliage,
                std::uint64_t capacity,
                bool count_known,
                std::uint64_t generated_count,
                std::uint64_t count_generation,
                const PhaseTotals& phases = {},
                const RenderUploads& uploads = {});

private:
    // Fixed 64 KiB stream buffer; frames stream to disk without a retained history.
    std::array<char, 64 * 1024> m_buffer{};
    std::ofstream m_out;
};
} // namespace Luminumbra::Client::Attribution
