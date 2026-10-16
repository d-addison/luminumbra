#include "FrameAttribution.h"

#include <iomanip>
#include <locale>
#include <ostream>

namespace Luminumbra::Client::Attribution {
std::uint32_t classify_rebuild(const ScatterSignature& previous,
                               const ScatterSignature& current,
                               bool built,
                               bool backlog) {
    std::uint32_t reasons = built ? 0u : static_cast<std::uint32_t>(Initial);
    if (built) {
        if (previous.config != current.config)
            reasons |= Config;
        if (previous.camera_cell != current.camera_cell)
            reasons |= CameraCell;
        if (previous.ground_revision != current.ground_revision)
            reasons |= GroundRevision;
        if (previous.scatter_chunks != current.scatter_chunks)
            reasons |= ScatterChunks;
    }
    if (backlog)
        reasons |= Backlog;
    return reasons;
}
std::optional<std::size_t> generated_instance_count(bool gpu_active,
                                                    bool readback_enabled,
                                                    bool completed_readback,
                                                    std::size_t draw_limit) {
    if (gpu_active && (!readback_enabled || !completed_readback))
        return std::nullopt;
    return draw_limit;
}
const char* path_name(FoliagePath path) {
    switch (path) {
        case FoliagePath::NotCalled:
            return "not_called";
        case FoliagePath::ReusedGpu:
            return "reused_gpu";
        case FoliagePath::ReusedCpu:
            return "reused_cpu";
        case FoliagePath::Gpu:
            return "gpu";
        case FoliagePath::Cpu:
            return "cpu";
        case FoliagePath::Inactive:
            return "inactive";
        case FoliagePath::FrozenGpu:
            return "frozen_gpu";
        case FoliagePath::Mixed:
            return "mixed";
    }
    return "unknown";
}
const char* fallback_name(FallbackReason reason) {
    switch (reason) {
        case FallbackReason::None:
            return "none";
        case FallbackReason::GpuDisabled:
            return "gpu_disabled";
        case FallbackReason::MissingResources:
            return "missing_resources";
        case FallbackReason::ColumnCapacity:
            return "column_capacity";
        case FallbackReason::Mixed:
            return "mixed";
    }
    return "unknown";
}
namespace {
thread_local FoliageFrame* current_foliage_frame = nullptr;
}
FoliageFrameScope::FoliageFrameScope(bool enabled)
    : m_previous(current_foliage_frame) {
    current_foliage_frame = enabled ? &m_frame : nullptr;
}
FoliageFrameScope::~FoliageFrameScope() {
    current_foliage_frame = m_previous;
}
bool foliage_frame_active() {
    return current_foliage_frame != nullptr;
}
void record_render_uploads(const Rendering::UploadWorkStats& work,
                           bool water,
                           std::size_t pending) {
    if (!current_foliage_frame)
        return;
    auto& uploads = current_foliage_frame->uploads;
    auto& total = water ? uploads.water : uploads.terrain;
    total.attempts += work.attempts;
    total.adopted += work.adopted;
    total.bytes += work.bytes;
    total.byte_overshoot += work.byte_overshoot;
    total.elapsed_ms += work.elapsed_ms;
    total.overshoot_ms += work.overshoot_ms;
    (water ? uploads.water_pending : uploads.terrain_pending) += pending;
}
void record_foliage_rebuild(const FoliageRebuild& rebuild,
                            double milliseconds,
                            std::uint64_t capacity,
                            std::optional<std::size_t> count,
                            std::uint64_t count_generation) {
    if (!current_foliage_frame)
        return;
    auto& frame = *current_foliage_frame;
    auto& total = frame.rebuild;
    total.path =
        total.rebuild_calls == 0 || total.path == rebuild.path ? rebuild.path : FoliagePath::Mixed;
    total.fallback = total.rebuild_calls == 0 || total.fallback == rebuild.fallback
                         ? rebuild.fallback
                         : FallbackReason::Mixed;
    ++total.rebuild_calls;
    total.signature_ms += rebuild.signature_ms;
    total.sort_ms += rebuild.sort_ms;
    total.surface_ms += rebuild.surface_ms;
    total.repack_ms += rebuild.repack_ms;
    total.hash_ms += rebuild.hash_ms;
    total.upload_ms += rebuild.upload_ms;
    total.palette_scratch_ms += rebuild.palette_scratch_ms;
    total.dispatch_ms += rebuild.dispatch_ms;
    total.cpu_fallback_ms += rebuild.cpu_fallback_ms;
    total.reasons |= rebuild.reasons;
    total.admitted_columns += rebuild.admitted_columns;
    total.surface_misses += rebuild.surface_misses;
    total.deferred_columns += rebuild.deferred_columns;
    total.copied_bytes += rebuild.copied_bytes;
    total.hashed_bytes += rebuild.hashed_bytes;
    total.uploaded_bytes += rebuild.uploaded_bytes;
    total.scratch_bytes += rebuild.scratch_bytes;
    total.backlog = total.backlog || rebuild.backlog;
    frame.rebuild_ms += milliseconds;
    frame.capacity = capacity;
    frame.count_known = count.has_value();
    frame.generated_count = count.value_or(0);
    frame.count_generation = count_generation;
}
double exclusive_parent_ms(double parent_ms, double nested_before_ms, double nested_after_ms) {
    return parent_ms - (nested_after_ms - nested_before_ms);
}
double FrameBuckets::sum() const {
    return sim + stream + render + present + foliage + scatter + rebake + poll + menu + ui;
}
void write_csv_header(std::ostream& out) {
    out << "frame,frame_ms,sim_ms,stream_ms,render_ms,present_ms,foliage_ms,scatter_ms,"
           "rebake_ms,poll_ms,menu_ms,ui_ms,other_ms,signature_ms,sort_ms,surface_ms,"
           "repack_ms,hash_ms,upload_ms,palette_scratch_ms,dispatch_ms,cpu_fallback_ms,"
           "rebuild_calls,reason_mask,admitted_columns,surface_misses,deferred_columns,copied_"
           "bytes,"
           "hashed_bytes,uploaded_bytes,scratch_bytes,backlog,path,fallback,"
           "instance_capacity,generated_count,count_generation,"
           "terrain_work_ms,terrain_overshoot_ms,terrain_attempts,terrain_adopted,terrain_work_"
           "bytes,"
           "terrain_byte_overshoot,terrain_pending,water_work_ms,water_overshoot_ms,water_attempts,"
           "water_adopted,water_work_bytes,water_byte_overshoot,water_pending\n";
}
bool write_csv_row(std::ostream& out,
                   std::uint64_t frame,
                   double frame_ms,
                   const FrameBuckets& buckets,
                   const FoliageRebuild& foliage,
                   std::uint64_t capacity,
                   bool count_known,
                   std::uint64_t generated_count,
                   std::uint64_t count_generation,
                   const RenderUploads& uploads) {
    if (!out.good())
        return false;
    out << std::fixed << std::setprecision(6) << frame << ',' << frame_ms << ',' << buckets.sim
        << ',' << buckets.stream << ',' << buckets.render << ',' << buckets.present << ','
        << buckets.foliage << ',' << buckets.scatter << ',' << buckets.rebake << ',' << buckets.poll
        << ',' << buckets.menu << ',' << buckets.ui << ',' << buckets.other(frame_ms) << ','
        << foliage.signature_ms << ',' << foliage.sort_ms << ',' << foliage.surface_ms << ','
        << foliage.repack_ms << ',' << foliage.hash_ms << ',' << foliage.upload_ms << ','
        << foliage.palette_scratch_ms << ',' << foliage.dispatch_ms << ','
        << foliage.cpu_fallback_ms << ',' << foliage.rebuild_calls << ',' << foliage.reasons << ','
        << foliage.admitted_columns << ',' << foliage.surface_misses << ','
        << foliage.deferred_columns << ',' << foliage.copied_bytes << ',' << foliage.hashed_bytes
        << ',' << foliage.uploaded_bytes << ',' << foliage.scratch_bytes << ','
        << (foliage.backlog ? 1 : 0) << ',' << path_name(foliage.path) << ','
        << fallback_name(foliage.fallback) << ',' << capacity << ',';
    if (count_known)
        out << generated_count;
    out << ',';
    if (count_known)
        out << count_generation;
    const auto append_uploads = [&](const Rendering::UploadWorkStats& work, std::size_t pending) {
        out << ',' << work.elapsed_ms << ',' << work.overshoot_ms << ',' << work.attempts << ','
            << work.adopted << ',' << work.bytes << ',' << work.byte_overshoot << ',' << pending;
    };
    append_uploads(uploads.terrain, uploads.terrain_pending);
    append_uploads(uploads.water, uploads.water_pending);
    out << '\n';
    return out.good();
}
FrameRecorder::FrameRecorder(bool enabled, const std::filesystem::path& logs) {
    if (!enabled)
        return;
    std::error_code error;
    std::filesystem::create_directories(logs, error);
    if (error)
        return;
    m_out.rdbuf()->pubsetbuf(m_buffer.data(), static_cast<std::streamsize>(m_buffer.size()));
    m_out.imbue(std::locale::classic());
    m_out.open(logs / "frame-attribution.csv", std::ios::out | std::ios::trunc | std::ios::binary);
    if (active())
        write_csv_header(m_out);
}
bool FrameRecorder::record(std::uint64_t frame,
                           double frame_ms,
                           const FrameBuckets& buckets,
                           const FoliageRebuild& foliage,
                           std::uint64_t capacity,
                           bool count_known,
                           std::uint64_t generated_count,
                           std::uint64_t count_generation,
                           const RenderUploads& uploads) {
    if (!active())
        return false;
    return write_csv_row(m_out,
                         frame,
                         frame_ms,
                         buckets,
                         foliage,
                         capacity,
                         count_known,
                         generated_count,
                         count_generation,
                         uploads);
}
} // namespace Luminumbra::Client::Attribution
