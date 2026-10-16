#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <optional>

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
    std::uint64_t uploads = 0;
    std::uint64_t stale_results = 0;
    // Storage allocated with null glBufferData, separate from transferred bytes.
    std::uint64_t scratch_bytes = 0;
    bool backlog = false;
    FoliagePath path = FoliagePath::NotCalled;
    FallbackReason fallback = FallbackReason::None;
};

// Every rebuild on the render thread contributes, including preview and QA
// callers. Storage is constant size; paths are Mixed when calls differ.
struct FoliageFrame {
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
    FoliageFrame m_frame{};
    FoliageFrame* m_previous = nullptr;
};
bool foliage_frame_active();
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
    double other(double frame_ms) const {
        return frame_ms - sum();
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
                   std::uint64_t count_generation);

class FrameRecorder {
public:
    FrameRecorder(bool enabled, const std::filesystem::path& logs);
    bool active() const {
        return m_out.is_open() && m_out.good();
    }
    bool record(std::uint64_t frame,
                double frame_ms,
                const FrameBuckets& buckets,
                const FoliageRebuild& foliage,
                std::uint64_t capacity,
                bool count_known,
                std::uint64_t generated_count,
                std::uint64_t count_generation);

private:
    // Fixed 64 KiB stream buffer; frames stream to disk without a retained history.
    std::array<char, 64 * 1024> m_buffer{};
    std::ofstream m_out;
};
} // namespace Luminumbra::Client::Attribution
