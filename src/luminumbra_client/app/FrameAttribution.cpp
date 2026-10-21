#include "FrameAttribution.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <locale>
#include <optional>
#include <ostream>
#include <ratio>
#include <system_error>

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
FoliageFrame*& current_foliage_frame() {
    thread_local FoliageFrame* frame = nullptr;
    return frame;
}
} // namespace
FoliageFrameScope::FoliageFrameScope(bool enabled)
    : m_previous(current_foliage_frame()) {
    current_foliage_frame() = enabled ? &m_frame : nullptr;
}
FoliageFrameScope::~FoliageFrameScope() {
    current_foliage_frame() = m_previous;
}
bool foliage_frame_active() {
    return current_foliage_frame() != nullptr;
}
void record_foliage_rebuild(const FoliageRebuild& rebuild,
                            double milliseconds,
                            std::uint64_t capacity,
                            std::optional<std::size_t> count,
                            std::uint64_t count_generation) {
    auto* current_frame = current_foliage_frame();
    if (!current_frame)
        return;
    auto& frame = *current_frame;
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
    total.surface_samples += rebuild.surface_samples;
    total.deferred_columns += rebuild.deferred_columns;
    total.copied_bytes += rebuild.copied_bytes;
    total.hashed_bytes += rebuild.hashed_bytes;
    total.uploaded_bytes += rebuild.uploaded_bytes;
    total.uploads += rebuild.uploads;
    total.stale_results += rebuild.stale_results;
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
namespace {
// Frame guards own the totals; each thread only borrows its active guard's
// mutable storage. Keep the stack pointers local to their accessors, like foliage.
PhaseTotals*& current_phases() {
    thread_local PhaseTotals* totals = nullptr;
    return totals;
}
const PhaseScope*& current_phase_scope() {
    thread_local const PhaseScope* scope = nullptr;
    return scope;
}
double foliage_ms() {
    const auto* frame = current_foliage_frame();
    return frame ? frame->rebuild_ms : 0;
}
constexpr std::array<const char*, kPhaseCount> kPhaseNames{"ui_update",
                                                           "ui_render",
                                                           "ui_filters",
                                                           "menu_scene",
                                                           "preview_adoption",
                                                           "document_load",
                                                           "texture_load",
                                                           "shader_load",
                                                           "shader_first_use",
                                                           "world_create_callback",
                                                           "world_load_callback"};
} // namespace
const char* phase_name(Phase phase) {
    return kPhaseNames.at(static_cast<std::size_t>(phase));
}
double PhaseTotals::sum() const {
    double result = 0;
    for (const double value : m_ms)
        result += value;
    return result;
}
void PhaseTotals::add(Phase phase, double inclusive_ms, double child_ms, double direct_foliage_ms) {
    m_ms.at(static_cast<std::size_t>(phase)) += inclusive_ms - child_ms - direct_foliage_ms;
    ++m_scopes.at(static_cast<std::size_t>(phase));
    m_inclusive_ms.at(static_cast<std::size_t>(phase)) += inclusive_ms;
}
PhaseFrameScope::PhaseFrameScope(bool enabled)
    : m_previous(current_phases()) {
    current_phases() = enabled ? &m_totals : nullptr;
    shader_first_use_collection_enabled = current_phases() != nullptr;
}
PhaseFrameScope::~PhaseFrameScope() {
    current_phases() = m_previous;
    shader_first_use_collection_enabled = current_phases() != nullptr;
}
bool PhaseFrameScope::is_current() const {
    return current_phases() == &m_totals;
}
double phase_total_ms() {
    const auto* totals = current_phases();
    return totals ? totals->sum() : 0;
}
PhaseScope::PhaseScope(Phase phase, bool enabled)
    : m_phase(phase) {
    if (!enabled || !current_phases())
        return;
    m_totals = current_phases();
    m_parent = current_phase_scope();
    current_phase_scope() = this;
    m_foliage_before = foliage_ms();
    m_start = std::chrono::steady_clock::now();
}
PhaseScope::~PhaseScope() {
    if (!m_totals)
        return;
    const double elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start)
            .count();
    const double foliage = foliage_ms() - m_foliage_before;
    m_totals->add(m_phase, elapsed, m_children_ms, foliage - m_child_foliage_ms);
    if (m_parent && m_parent->m_totals == m_totals) {
        m_parent->m_children_ms += elapsed;
        m_parent->m_child_foliage_ms += foliage;
    }
    current_phase_scope() = m_parent;
}
bool PhaseScope::is_current() const {
    return m_totals && m_totals == current_phases() && current_phase_scope() == this;
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
           "instance_capacity,generated_count,count_generation";
    for (std::size_t index = 0; index < kPhaseCount; ++index)
        out << ',' << phase_name(static_cast<Phase>(index)) << "_ms";
    out << '\n';
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
                   const PhaseTotals& phases) {
    if (!out.good())
        return false;
    out << std::fixed << std::setprecision(6) << frame << ',' << frame_ms << ',' << buckets.sim
        << ',' << buckets.stream << ',' << buckets.render << ',' << buckets.present << ','
        << buckets.foliage << ',' << buckets.scatter << ',' << buckets.rebake << ',' << buckets.poll
        << ',' << buckets.menu << ',' << buckets.ui << ',' << buckets.other(frame_ms, phases) << ','
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
    for (const double value : phases.milliseconds())
        out << ',' << value;
    out << '\n';
    return out.good();
}
bool finalize_csv(std::ofstream& out) {
    out.flush();
    out.close();
    return out.good();
}
FrameRecorder::FrameRecorder(bool enabled, const std::filesystem::path& logs) {
    if (!enabled)
        return;
    std::error_code error;
    std::filesystem::create_directories(logs, error);
    if (error) {
        m_out.setstate(std::ios::failbit);
        return;
    }
    m_out.rdbuf()->pubsetbuf(m_buffer.data(), static_cast<std::streamsize>(m_buffer.size()));
    m_out.imbue(std::locale::classic());
    m_out.open(logs / "frame-attribution.csv", std::ios::out | std::ios::trunc | std::ios::binary);
    if (active())
        write_csv_header(m_out);
}
bool FrameRecorder::finish() {
    if (!m_out.is_open())
        return m_out.good();
    return finalize_csv(m_out);
}
bool FrameRecorder::record(std::uint64_t frame,
                           double frame_ms,
                           const FrameBuckets& buckets,
                           const FoliageRebuild& foliage,
                           std::uint64_t capacity,
                           bool count_known,
                           std::uint64_t generated_count,
                           std::uint64_t count_generation,
                           const PhaseTotals& phases) {
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
                         phases);
}
} // namespace Luminumbra::Client::Attribution
