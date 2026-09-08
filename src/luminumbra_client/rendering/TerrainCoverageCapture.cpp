#include "RenderPipeline.h"

#include "FarLodSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <fstream>
#include <tuple>

namespace Luminumbra::Rendering {
namespace {
using Json = nlohmann::json;

Json Vec(const glm::vec3& v) {
    return {v.x, v.y, v.z};
}

// PFM preserves the readback floats without display mapping. Its row order is
// bottom-up, unlike the paired top-down PPM/PGM. The scale sign declares endian.
bool WriteFloatPlane(const std::filesystem::path& path,
                     u32 width,
                     u32 height,
                     int channels,
                     const std::vector<float>& values) {
    std::ofstream out(path, std::ios::binary);
    out << (channels == 3 ? "PF\n" : "Pf\n") << width << ' ' << height << '\n'
        << (std::endian::native == std::endian::little ? "-1.0\n" : "1.0\n");
    out.write(reinterpret_cast<const char*>(values.data()),
              static_cast<std::streamsize>(values.size() * sizeof(float)));
    out.close();
    return !out.fail();
}

bool WriteBytePlane(const std::filesystem::path& path,
                    u32 width,
                    u32 height,
                    int channels,
                    const std::vector<unsigned char>& values) {
    std::ofstream out(path, std::ios::binary);
    out << (channels == 3 ? "P6\n" : "P5\n") << width << ' ' << height << "\n255\n";
    const auto stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    for (u32 row = height; row > 0; --row)
        out.write(reinterpret_cast<const char*>(values.data() + (row - 1) * stride),
                  static_cast<std::streamsize>(stride));
    out.close();
    return !out.fail();
}

// Restore all pixel-pack state, including PBO and non-default row strides. Save
// read-buffer selection for BOTH FBOs we touch, not just the current binding.
struct ReadbackState {
    GLint fbo = 0, read_buffer = 0, gbuffer_read = 0, back_read = 0, pbo = 0;
    GLint alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0, swap_bytes = 0;
    GLuint gbuffer = 0;
    explicit ReadbackState(GLuint id)
        : gbuffer(id) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
        glGetIntegerv(GL_PACK_SWAP_BYTES, &swap_bytes);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer);
        glGetIntegerv(GL_READ_BUFFER, &gbuffer_read);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glGetIntegerv(GL_READ_BUFFER, &back_read);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    }
    ~ReadbackState() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer);
        glReadBuffer(static_cast<GLenum>(gbuffer_read));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(static_cast<GLenum>(back_read));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(fbo));
        glReadBuffer(static_cast<GLenum>(read_buffer));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pbo));
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
        glPixelStorei(GL_PACK_SWAP_BYTES, swap_bytes);
    }
};
} // namespace

void RenderPipeline::set_terrain_coverage_diagnostics_enabled(bool enabled,
                                                              bool bypass_camera_region_guard) {
    m_terrain_coverage_enabled = enabled;
    if (m_farlod) {
        m_farlod->set_coverage_diagnostics_enabled(enabled);
        m_farlod->set_coverage_camera_region_guard_bypass(bypass_camera_region_guard);
    }
}

nlohmann::json RenderPipeline::terrain_coverage_diagnostics(bool include_live_chunks) const {
    Json out = {{"enabled", m_terrain_coverage_enabled},
                {"render_frame", m_terrain_coverage_frame}};
    if (!m_terrain_coverage_enabled)
        return out;
    if (m_farlod) {
        const auto& d = m_farlod->coverage_diagnostics();
        const auto& s = m_farlod->stats();
        auto rows = Json::array();
        for (const auto& r : d.neighbourhood) {
            Json row = {{"region", {r.rx, r.rz}},
                        {"wanted", r.wanted},
                        {"wanted_tier", static_cast<int>(r.wanted_tier)},
                        {"pending", r.pending},
                        {"resident", r.resident},
                        {"current", r.current},
                        {"authority_revision", r.authority_revision},
                        {"terrain_decision", r.terrain_decision},
                        {"water_decision", r.water_decision}};
            if (r.resident) {
                row["resident_tier"] = static_cast<int>(r.resident_tier);
                row["resident_authority_revision"] = r.resident_authority_revision;
                row["persistence_pending"] = r.persistence_pending;
                row["has_edited_samples"] =
                    r.edited_samples_observed ? Json(r.has_edited_samples) : Json(nullptr);
                row["aabb_min"] = Vec(r.aabb_min);
                row["aabb_max"] = Vec(r.aabb_max);
                row["terrain_indices"] = r.terrain_indices;
                row["water_indices"] = r.water_indices;
            }
            rows.push_back(std::move(row));
        }
        out["far"] = {{"update_frame", d.update_frame},
                      {"enabled", s.enabled},
                      {"draw_observed", d.draw_observed},
                      {"camera", Vec(d.camera)},
                      {"wanted", d.wanted},
                      {"resident_wanted", d.resident},
                      {"missing_after_eviction", d.missing},
                      {"stale_after_eviction", d.stale},
                      {"pending", d.pending},
                      {"resident_bytes", s.resident_bytes},
                      {"draws", s.region_draws},
                      {"indices", s.indices_drawn},
                      {"builds_failed_total", s.builds_failed_total},
                      {"authority_build_failures", s.authority_build_failures},
                      {"stale_results_rejected", s.stale_results_rejected},
                      {"camera_region_guard_bypassed", d.camera_region_guard_bypassed},
                      {"preview_anchored", d.preview_anchored},
                      {"preview_anchor", Vec(d.preview_anchor)},
                      {"preview_inner_radius_m", d.preview_inner_radius},
                      {"clip_inner_radius_m", FarLodSystem::kFarClipInnerRadiusMeters},
                      {"clip_outer_radius_m", FarLodSystem::kFarClipOuterRadiusMeters},
                      {"camera_neighbourhood", std::move(rows)}};
    } else {
        out["far"] = nullptr;
    }
    if (!include_live_chunks || !m_frame_prepared.valid)
        return out;
    // Bound output and sort independently of unordered-map/culler iteration.
    constexpr std::size_t limit = 8192;
    auto snapshots = m_frame_prepared.renderable_chunk_snapshots;
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) {
        return std::tie(a.coords.z, a.coords.y, a.coords.x) <
               std::tie(b.coords.z, b.coords.y, b.coords.x);
    });
    auto rows = Json::array();
    for (const auto& chunk : snapshots) {
        if (rows.size() == limit)
            break;
        const auto found = m_chunk_render_data.find(chunk.id);
        const bool uploaded = found != m_chunk_render_data.end() &&
                              found->second.pool_handle != ChunkRenderData::kInvalidPoolHandle &&
                              found->second.element_count != 0;
        const bool visible = m_terrain_coverage_visible.count(chunk.id) != 0;
        Json row = {{"id", chunk.id},
                    {"coords", {chunk.coords.x, chunk.coords.y, chunk.coords.z}},
                    {"snapshot_mesh_version", chunk.mesh_version},
                    {"snapshot_indices", chunk.terrain_index_count},
                    {"pool_resident", uploaded},
                    {"culler_visible", visible},
                    {"submitted", m_terrain_coverage_live_submit_observed && visible && uploaded}};
        if (chunk.source_chunk)
            row["source_lod_observed_at_capture"] = chunk.source_chunk->current_lod.load();
        if (found != m_chunk_render_data.end()) {
            row["uploaded_mesh_version"] = found->second.mesh_version;
            row["uploaded_indices"] = found->second.element_count;
        }
        rows.push_back(std::move(row));
    }
    out["live"] = {{"submit_observed", m_terrain_coverage_live_submit_observed},
                   {"snapshot_count", snapshots.size()},
                   {"truncated", snapshots.size() > limit},
                   {"scope", "prepared_renderable_snapshots_not_all_requested_chunks"},
                   {"chunks", std::move(rows)}};
    if (const auto* world = m_frame_prepared.world_system) {
        const auto& s = world->get_last_streaming_budget_stats();
        out["live"]["streaming"] = {
            {"requested_radius_chunks", s.requested_render_radius},
            {"target_radius_chunks", s.target_render_radius},
            {"generation_pending", s.deferred_generation},
            {"meshing_pending", s.deferred_meshing},
            {"terrain_uploads_pending", get_last_mesh_upload_stats().terrain_uploads_deferred}};
    }
    return out;
}

bool RenderPipeline::capture_terrain_coverage(const std::filesystem::path& out_dir) const {
    // A capture is immutable. Failure leaves no complete manifest, and reruns
    // must choose a new directory. Bound memory/I/O before any readback.
    constexpr std::size_t max_pixels = 16u * 1024u * 1024u;
    const std::size_t pixels = static_cast<std::size_t>(m_internal_width) * m_internal_height;
    const std::size_t color_pixels = static_cast<std::size_t>(m_screen_width) * m_screen_height;
    if (!m_terrain_coverage_enabled || !m_frame_prepared.valid || m_offscreen_target_active ||
        !gbuffer().fbo_id || !pixels || pixels > max_pixels || !color_pixels ||
        color_pixels > max_pixels)
        return false;
    std::error_code ec;
    if (!std::filesystem::create_directory(out_dir, ec) || ec)
        return false;
    Json receipt = terrain_coverage_diagnostics(true);
    receipt["schema"] = "luminumbra.terrain_coverage_capture.v1";
    receipt["capture_phase"] = "same_rendered_frame_after_postprocessing_before_present_unmeasured";
    receipt["color_dimensions"] = {m_screen_width, m_screen_height};
    receipt["gbuffer_dimensions"] = {m_internal_width, m_internal_height};
    receipt["time_of_day"] = get_time_of_day();
    receipt["matrix_layout"] = "column_major";
    receipt["view"] = std::vector<float>(glm::value_ptr(m_frame_prepared.view),
                                         glm::value_ptr(m_frame_prepared.view) + 16);
    auto projection = m_frame_prepared.projection;
    projection[2][0] += m_taau_jitter_ndc.x;
    projection[2][1] += m_taau_jitter_ndc.y;
    receipt["gbuffer_projection"] =
        std::vector<float>(glm::value_ptr(projection), glm::value_ptr(projection) + 16);
    receipt["jitter_ndc"] = {m_taau_jitter_ndc.x, m_taau_jitter_ndc.y};
    receipt["taau_enabled"] = m_taau_enabled;
    receipt["limitations"] = {"G-buffer coverage is not final transparent-surface coverage",
                              "color may include overlays; use --no-ui for a scene-only capture",
                              "TAAU color includes temporal history when taau_enabled; disable it "
                              "for spatial coverage comparisons",
                              "residency and draw submission do not prove per-pixel coverage",
                              "raw readback and file writes are excluded from measured frames"};
    if (glGetError() != GL_NO_ERROR)
        return false;
    {
        ReadbackState state(gbuffer().fbo_id);
        glReadBuffer(GL_BACK);
        std::vector<unsigned char> color(color_pixels * 3);
        glReadPixels(0, 0, m_screen_width, m_screen_height, GL_RGB, GL_UNSIGNED_BYTE, color.data());
        if (glGetError() != GL_NO_ERROR ||
            !WriteBytePlane(out_dir / "color.ppm", m_screen_width, m_screen_height, 3, color))
            return false;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer().fbo_id);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        const auto plane = [&](const char* name, GLenum format, int channels, GLenum attachment) {
            if (format != GL_DEPTH_COMPONENT)
                glReadBuffer(attachment);
            std::vector<float> values(pixels * static_cast<std::size_t>(channels));
            glReadPixels(
                0, 0, m_internal_width, m_internal_height, format, GL_FLOAT, values.data());
            return glGetError() == GL_NO_ERROR &&
                   WriteFloatPlane(
                       out_dir / name, m_internal_width, m_internal_height, channels, values);
        };
        if (!plane("depth.pfm", GL_DEPTH_COMPONENT, 1, GL_NONE) ||
            !plane("position.pfm", GL_RGB, 3, GL_COLOR_ATTACHMENT0) ||
            !plane("albedo.pfm", GL_RGB, 3, GL_COLOR_ATTACHMENT2))
            return false;
        std::vector<unsigned char> encoded(pixels * 4), ids(pixels);
        std::vector<float> normals(pixels * 3);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(
            0, 0, m_internal_width, m_internal_height, GL_RGBA, GL_UNSIGNED_BYTE, encoded.data());
        if (glGetError() != GL_NO_ERROR)
            return false;
        for (std::size_t i = 0; i < pixels; ++i) {
            ids[i] = encoded[i * 4 + 3];
            const glm::vec2 e =
                glm::vec2(encoded[i * 4], encoded[i * 4 + 1]) / 255.0f * 2.0f - 1.0f;
            glm::vec3 n(e, 1.0f - std::abs(e.x) - std::abs(e.y));
            if (n.z < 0.0f) {
                n.x = (1.0f - std::abs(e.y)) * (e.x >= 0.0f ? 1.0f : -1.0f);
                n.y = (1.0f - std::abs(e.x)) * (e.y >= 0.0f ? 1.0f : -1.0f);
            }
            n = glm::normalize(n);
            for (int c = 0; c < 3; ++c)
                normals[i * 3 + static_cast<std::size_t>(c)] = n[c];
        }
        if (!WriteFloatPlane(
                out_dir / "normal.pfm", m_internal_width, m_internal_height, 3, normals) ||
            !WriteBytePlane(out_dir / "material.pgm", m_internal_width, m_internal_height, 1, ids))
            return false;
    }
    if (glGetError() != GL_NO_ERROR)
        return false;
    receipt["attachments"] = {
        {"color.ppm", "RGB8 final display output; top-down; no additional transfer conversion"},
        {"depth.pfm", "float32 OpenGL window depth [0,1], clear=1; bottom-up"},
        {"position.pfm", "float32 RGB view-space metres expanded from RGB16F; bottom-up"},
        {"normal.pfm",
         "float32 RGB view-space unit normal decoded from octahedral RG8; bottom-up; ignore "
         "clear-depth pixels"},
        {"albedo.pfm", "float32 RGB linear albedo expanded from RGBA8; bottom-up"},
        {"material.pgm", "uint8 material ID from normal attachment alpha; top-down; clear=255"}};
    receipt["complete"] = true;
    const auto temporary = out_dir / "manifest.json.tmp";
    std::ofstream out(temporary, std::ios::binary);
    out << receipt.dump(2) << '\n';
    out.close();
    if (out.fail())
        return false;
    std::filesystem::rename(temporary, out_dir / "manifest.json", ec);
    return !ec;
}
} // namespace Luminumbra::Rendering
