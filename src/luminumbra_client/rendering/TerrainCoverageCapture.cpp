#include "RenderPipeline.h"

#include "AttributedGl.h"
#include "Camera.h"
#include "FarLodSystem.h"
#include "Shader.h"
#include "UploadBudget.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "passes/AerialPass.h"
#include "passes/FroxelPass.h"
#include "passes/LightingPass.h"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
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
    GLint image_height = 0, skip_images = 0;
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
        glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &image_height);
        glGetIntegerv(GL_PACK_SKIP_IMAGES, &skip_images);
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
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
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
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, image_height);
        glPixelStorei(GL_PACK_SKIP_IMAGES, skip_images);
    }
};
// Read existing pass inputs without rebinding textures or rerunning the scene.
// One plane at a time: at most 16M pixels / 256 MiB and 64 volume slices.
bool CaptureTexture(const std::filesystem::path& directory,
                    const char* name,
                    GLuint texture,
                    GLenum format,
                    int channels,
                    Json& sources) {
    if (texture == 0) {
        sources[name] = {{"status", "unavailable"}};
        return true;
    }
    GLint width = 0, height = 0, layers = 0;
    glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
    glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_DEPTH, &layers);
    constexpr std::size_t max_pixels = 16u * 1024u * 1024u;
    if (glGetError() != GL_NO_ERROR || width <= 0 || height <= 0 || layers < 1 || layers > 64 ||
        static_cast<std::size_t>(width) > max_pixels / static_cast<std::size_t>(height)) {
        return false;
    }
    if ((channels != 1 && channels != 3 && channels != 4) || (channels == 4 && format != GL_RGBA)) {
        return false;
    }
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                       static_cast<std::size_t>(channels);
    std::vector<float> values(count);
    const int output_channels = format == GL_RGBA ? 1 : channels;
    auto files = Json::array();
    for (int layer = 0; layer < layers; ++layer) {
        values.resize(count);
        glGetTextureSubImage(texture,
                             0,
                             0,
                             0,
                             layer,
                             width,
                             height,
                             1,
                             format,
                             GL_FLOAT,
                             static_cast<GLsizei>(count * sizeof(float)),
                             values.data());
        if (format == GL_RGBA) {
            const auto plane_pixels = count / 4u;
            for (std::size_t pixel = 0; pixel < plane_pixels; ++pixel) {
                values[pixel] = values[pixel * 4u + 3u];
            }
            values.resize(plane_pixels);
        }
        const std::string file = std::string(name) + "-" + std::to_string(layer) + ".pfm";
        if (glGetError() != GL_NO_ERROR || !WriteFloatPlane(directory / file,
                                                            static_cast<u32>(width),
                                                            static_cast<u32>(height),
                                                            output_channels,
                                                            values)) {
            return false;
        }
        files.push_back(file);
    }
    sources[name] = {{"status", "captured"},
                     {"dimensions", {width, height, layers}},
                     {"channels", output_channels},
                     {"files", std::move(files)}};
    return true;
}

// Replay only the existing fog fragment program into private storage, using
// its unchanged uploaded uniforms and current-frame inputs. No scene advancement.
struct FogReadback {
    GLint draw_fbo = 0, program = 0, vao = 0, active = 0;
    std::array<GLint, 4> viewport{}, texture_2d{}, texture_3d{};
    std::array<GLboolean, 4> mask{};
    std::array<GLint, 2> polygon_mode{};
    static constexpr std::array<GLenum, 4> capabilities{
        GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD};
    std::array<GLboolean, capabilities.size()> enabled{};
    GLboolean blend = GL_FALSE;
    GLuint texture = 0, fbo = 0;
    FogReadback() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, mask.data());
        blend = glIsEnabledi(GL_BLEND, 0);
        glDisablei(GL_BLEND, 0);
        glGetIntegerv(GL_POLYGON_MODE, polygon_mode.data());
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        for (std::size_t unit = 0; unit < texture_2d.size(); ++unit) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d[unit]);
            glGetIntegerv(GL_TEXTURE_BINDING_3D, &texture_3d[unit]);
        }
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            enabled[index] = glIsEnabled(capabilities[index]);
            glDisable(capabilities[index]);
        }
        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    ~FogReadback() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
        AttributedGl::UseProgram(glUseProgram, static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vao));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glColorMaski(0, mask[0], mask[1], mask[2], mask[3]);
        if (blend) {
            glEnablei(GL_BLEND, 0);
        }
        glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(polygon_mode[0]));
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            if (enabled[index]) {
                glEnable(capabilities[index]);
            }
        }
        for (std::size_t unit = 0; unit < texture_2d.size(); ++unit) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_2d[unit]));
            glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(texture_3d[unit]));
        }
        glActiveTexture(static_cast<GLenum>(active));
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
    }
};

bool CaptureFog(const std::filesystem::path& directory,
                const Shader& shader,
                const RenderContext& context,
                GLuint froxel,
                Json& sources) {
    FogReadback restore;
    // The caller has admitted at most 16M pixels. Private RGBA32F <= 256 MiB.
    glCreateTextures(GL_TEXTURE_2D, 1, &restore.texture);
    glTextureStorage2D(restore.texture,
                       1,
                       GL_RGBA32F,
                       static_cast<GLsizei>(context.internal_w()),
                       static_cast<GLsizei>(context.internal_h()));
    glCreateFramebuffers(1, &restore.fbo);
    glNamedFramebufferTexture(restore.fbo, GL_COLOR_ATTACHMENT0, restore.texture, 0);
    glNamedFramebufferDrawBuffer(restore.fbo, GL_COLOR_ATTACHMENT0);
    if (glGetError() != GL_NO_ERROR ||
        glCheckNamedFramebufferStatus(restore.fbo, GL_DRAW_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, restore.fbo);
    glViewport(0,
               0,
               static_cast<GLsizei>(context.internal_w()),
               static_cast<GLsizei>(context.internal_h()));
    const std::array<GLuint, 3> inputs{
        context.gbuffer_depth.id, context.sky_view_lut.id, context.transmittance_lut.id};
    for (std::size_t unit = 0; unit < inputs.size(); ++unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, inputs[unit]);
    }
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, froxel);
    AttributedGl::UseProgram(glUseProgram, shader.Id());
    glBindVertexArray(context.screen_quad_vao);
    AttributedGl::Submit(glDrawArrays, GLenum{GL_TRIANGLE_STRIP}, 0, 4);
    return glGetError() == GL_NO_ERROR &&
           CaptureTexture(directory, "aerial-inscatter", restore.texture, GL_RGB, 3, sources) &&
           CaptureTexture(directory, "aerial-opacity", restore.texture, GL_RGBA, 4, sources);
}

// Values are the uniforms already uploaded for this frame, including optional
// canopy controls and fog defaults. Sampler values identify units, not images.
Json CaptureUniforms(const Shader* shader) {
    Json uniforms = Json::object();
    if (!shader || !shader->IsValid()) {
        return uniforms;
    }
    GLint count = 0;
    glGetProgramiv(shader->Id(), GL_ACTIVE_UNIFORMS, &count);
    // Production programs are small; refuse oversized reflection explicitly.
    if (count < 0 || count > 512) {
        return {{"status", "reflection_limit"}};
    }
    for (GLint index = 0; index < count; ++index) {
        std::array<char, 256> name{};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(shader->Id(),
                           static_cast<GLuint>(index),
                           static_cast<GLsizei>(name.size()),
                           &length,
                           &size,
                           &type,
                           name.data());
        if (length <= 0 || length >= static_cast<GLsizei>(name.size() - 1) || size < 1 ||
            size > 64) {
            uniforms["status"] = "reflection_limit";
            return uniforms;
        }
        const int components = type == GL_FLOAT_MAT4   ? 16
                               : type == GL_FLOAT_VEC4 ? 4
                               : type == GL_FLOAT_VEC3 ? 3
                               : type == GL_FLOAT_VEC2 ? 2
                               : type == GL_FLOAT      ? 1
                                                       : 0;
        const std::string base(name.data(), static_cast<std::size_t>(length));
        for (GLint element = 0; element < size; ++element) {
            std::string key = base;
            if (size > 1) {
                const auto bracket = key.find("[0]");
                if (bracket == std::string::npos) {
                    continue;
                }
                key.replace(bracket, 3, "[" + std::to_string(element) + "]");
            }
            const GLint location = glGetUniformLocation(shader->Id(), key.c_str());
            if (location < 0) {
                continue; // Uniform-block members have no default-block location.
            }
            if (components != 0) {
                std::array<float, 16> values{};
                glGetUniformfv(shader->Id(), location, values.data());
                uniforms[key] = std::vector<float>(values.begin(), values.begin() + components);
            } else if (type == GL_INT || type == GL_BOOL || type == GL_SAMPLER_2D ||
                       type == GL_SAMPLER_3D || type == GL_SAMPLER_2D_ARRAY) {
                GLint value = 0;
                glGetUniformiv(shader->Id(), location, &value);
                uniforms[key] = value;
            }
        }
    }
    return uniforms;
}
} // namespace

nlohmann::json RenderPipeline::terrain_peak_diagnostics(const glm::vec3& target) const {
    Json out = {{"status", "no_prepared_frame"},
                {"centre", nullptr},
                {"target", nullptr},
                {"ownership_scope", "terrain_fragment_policy_not_prop_draw_identity"}};
    const auto* world = m_frame_prepared.world_system;
    if (!m_frame_prepared.valid || !world || !gbuffer().fbo_id || !m_internal_width ||
        !m_internal_height) {
        return out;
    }
    out["far_mode"] = FarWorldModeName(m_frame_far_selection.effective);
    out["far_draws"] = m_last_render_pass_stats.far_region_draws;
    if (const auto* legacy = legacy_far_compatibility(); legacy && legacy->preview_anchored()) {
        out["status"] = "preview_ownership_unavailable";
        return out;
    }
    const auto& ownership = m_far_world->LiveOwnership();
    out["live_ownership_complete"] = ownership.complete();
    const auto inverse_view = glm::inverse(glm::dmat4(m_frame_prepared.view));
    const auto describe = [&](const std::array<double, 3>& position) {
        Json point = {{"position", position},
                      {"lod", nullptr},
                      {"sample_step_m", nullptr},
                      {"mesh_path", "unknown"},
                      {"mesh_metadata_status", "no_snapshot"},
                      {"live_upload_present", false},
                      {"live_upload_current", false},
                      {"terrain_owner", "unresolved"}};
        FarWorldLiveCell cell;
        if (!FarWorldChunkCell(position, cell)) {
            point["mesh_metadata_status"] = "invalid_position";
            return point;
        }
        point["chunk"] = {cell.x, cell.y, cell.z};
        bool presented = false;
        // Capture-only: at most two searches of the admitted 8192 live cells.
        // Do not copy meshes or traverse the world's requested/resident store.
        const auto& snapshots = m_frame_prepared.renderable_chunk_snapshots;
        const auto count = std::min(snapshots.size(), kFarWorldLiveCellLimit);
        point["snapshot_search_truncated"] = snapshots.size() > count;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& snapshot = snapshots[index];
            if (snapshot.coords != IVec3(cell.x, cell.y, cell.z)) {
                continue;
            }
            const auto found = m_chunk_render_data.find(snapshot.id);
            presented = found != m_chunk_render_data.end() &&
                        m_chunk_geometry_pool.valid_allocation(found->second.pool_handle,
                                                               found->second.element_count) &&
                        found->second.element_count != 0;
            const bool current = presented && found->second.mesh_version == snapshot.mesh_version;
            point["live_upload_present"] = presented;
            point["live_upload_current"] = current;
            point["snapshot_mesh_version"] = snapshot.mesh_version;
            const auto* source = snapshot.source_chunk;
            if (!current || !source ||
                source->mesh_version.load(std::memory_order_acquire) != snapshot.mesh_version) {
                point["mesh_metadata_status"] = "mesh_version_unavailable";
                break;
            }
            if (source->pending_lod.load(std::memory_order_acquire) >= 0 ||
                source->get_state() != ChunkState::Ready) {
                point["mesh_metadata_status"] = "mesh_publication_pending";
                break;
            }
            const int lod = source->current_lod.load(std::memory_order_acquire);
            point["mesh_metadata_status"] = "unknown_lod";
            for (const auto& level : world->get_lod_levels()) {
                if (level.level != lod) {
                    continue;
                }
                point["lod"] = lod;
                point["sample_step_m"] = level.step;
                constexpr std::size_t full_lattice =
                    (CHUNK_SIZE_X + 1u) * (CHUNK_SIZE_Y + 1u) * (CHUNK_SIZE_Z + 1u);
                point["sdf_samples"] = source->sdf_data.size();
                if (source->sdf_data.size() == full_lattice) {
                    point["mesh_path"] = "sdf";
                    point["mesh_metadata_status"] = "current";
                } else if (source->sdf_data.empty() && level.step > 1) {
                    point["mesh_path"] = "heightfield";
                    point["mesh_metadata_status"] = "current";
                } else {
                    point["mesh_metadata_status"] = "invalid_lattice";
                }
                break;
            }
            break;
        }
        const glm::dvec3 relative =
            glm::dvec3(position[0], position[1], position[2]) - glm::dvec3(inverse_view[3]);
        const bool inner =
            glm::dot(relative, relative) < kFarWorldInnerRadiusMeters * kFarWorldInnerRadiusMeters;
        const bool ladder_owned = ownership.complete() && ownership.Owns(position);
        point["far_excluded_by_inner_radius"] = inner;
        point["ladder_live_owned"] = ladder_owned;
        // Legacy has depth-biased overlap rather than a cell mask. Outside the
        // inner sphere, a live upload alone cannot prove which surface won.
        const bool exclusive_live =
            inner || m_last_render_pass_stats.far_region_draws == 0 ||
            (m_frame_far_selection.effective == FarWorldMode::Ladder && ladder_owned);
        if (presented && exclusive_live) {
            point["terrain_owner"] = "live";
        } else if (!presented && snapshots.size() == count && !inner &&
                   m_last_render_pass_stats.far_region_draws != 0 &&
                   (m_frame_far_selection.effective != FarWorldMode::Ladder ||
                    (ownership.complete() && !ladder_owned))) {
            point["terrain_owner"] = "far";
        }
        return point;
    };
    out["target"] = describe({target.x, target.y, target.z});
    float depth = std::numeric_limits<float>::quiet_NaN();
    const u32 pixel_x = m_internal_width / 2u;
    const u32 pixel_y = m_internal_height / 2u;
    {
        const ReadbackState restore(gbuffer().fbo_id);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer().fbo_id);
        glReadPixels(static_cast<GLint>(pixel_x),
                     static_cast<GLint>(pixel_y),
                     1,
                     1,
                     GL_DEPTH_COMPONENT,
                     GL_FLOAT,
                     &depth);
    }
    // Leave GL errors available to the existing gates. A failed one-pixel read
    // leaves the NaN sentinel; no error-queue drain can hide a frame failure.
    out["status"] = "invalid_depth";
    if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f) {
        return out;
    }
    out["centre_depth"] = depth;
    out["centre_pixel"] = {pixel_x, pixel_y};
    if (depth == 0.0f) {
        out["status"] = "background";
        return out;
    }
    auto projection = m_frame_prepared.projection;
    projection[2][0] += m_taau_jitter_ndc.x;
    projection[2][1] += m_taau_jitter_ndc.y;
    const glm::dvec4 clip(2.0 * (static_cast<double>(pixel_x) + 0.5) / m_internal_width - 1.0,
                          2.0 * (static_cast<double>(pixel_y) + 0.5) / m_internal_height - 1.0,
                          depth,
                          1.0);
    const auto homogeneous = inverse_view * glm::inverse(glm::dmat4(projection)) * clip;
    if (!std::isfinite(homogeneous.w) || homogeneous.w == 0.0) {
        return out;
    }
    const auto position = glm::dvec3(homogeneous) / homogeneous.w;
    out["centre"] = describe({position.x, position.y, position.z});
    out["status"] = "observed";
    return out;
}

void RenderPipeline::set_terrain_coverage_diagnostics_enabled(bool enabled,
                                                              bool bypass_camera_region_guard) {
    m_terrain_coverage_enabled = enabled;
    if (auto* legacy = legacy_far_compatibility()) {
        legacy->set_coverage_diagnostics_enabled(enabled);
        legacy->set_coverage_camera_region_guard_bypass(bypass_camera_region_guard);
    }
}

nlohmann::json RenderPipeline::terrain_coverage_diagnostics(bool include_live_chunks) const {
    Json out = {{"enabled", m_terrain_coverage_enabled},
                {"render_frame", m_terrain_coverage_frame}};
    if (!m_terrain_coverage_enabled)
        return out;
    if (const auto* legacy = legacy_far_compatibility()) {
        const auto& d = legacy->coverage_diagnostics();
        const auto& s = legacy->stats();
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
    } else if (far_world_selection().effective == FarWorldMode::Ladder) {
        out["far"] = FarWorldDiagnosticJson(far_world_diagnostic());
    } else {
        out["far"] = nullptr;
    }
    const auto& ownership = m_far_world->LiveOwnership();
    out["live_ownership"] = {{"frame", ownership.frame()},
                             {"complete", ownership.complete()},
                             {"cells", ownership.cells().size()},
                             {"limit", kFarWorldLiveCellLimit}};
    const auto& uploads = get_last_mesh_upload_stats();
    const auto upload_work = [](const UploadWorkStats& work) {
        return Json{{"attempts", work.attempts},
                    {"adopted", work.adopted},
                    {"bytes", work.bytes},
                    {"byte_overshoot", work.byte_overshoot},
                    {"elapsed_ms", work.elapsed_ms},
                    {"overshoot_ms", work.overshoot_ms}};
    };
    out["live"]["render_uploads_settled"] = render_uploads_settled();
    out["live"]["terrain_upload_budget"] = upload_work(uploads.terrain_budget);
    out["live"]["water_upload_budget"] = upload_work(uploads.water_budget);
    out["live"]["streaming"] = {{"terrain_uploads_pending", uploads.terrain_uploads_deferred},
                                {"water_uploads_pending", uploads.water_uploads_deferred}};
    if (const auto* world = m_frame_prepared.world_system; m_frame_prepared.valid && world) {
        const auto& s = world->get_last_streaming_budget_stats();
        out["live"]["streaming"].update({{"requested_radius_chunks", s.requested_render_radius},
                                         {"target_radius_chunks", s.target_render_radius},
                                         {"generation_pending", s.deferred_generation},
                                         {"meshing_pending", s.deferred_meshing}});
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
        const bool allocated =
            found != m_chunk_render_data.end() &&
            found->second.pool_handle != ChunkRenderData::kInvalidPoolHandle &&
            m_chunk_geometry_pool.allocation(found->second.pool_handle).live &&
            m_chunk_geometry_pool.allocation(found->second.pool_handle).index_count ==
                found->second.element_count;
        const FarWorldLiveUpload upload{
            {chunk.coords.x, chunk.coords.y, chunk.coords.z, 0},
            chunk.mesh_version,
            found != m_chunk_render_data.end() ? found->second.mesh_version : 0,
            found != m_chunk_render_data.end() ? found->second.element_count : 0,
            allocated};
        const bool uploaded = IsFarWorldLiveUploadCurrent(upload);
        const bool visible = m_terrain_coverage_visible.count(chunk.id) != 0;
        Json row = {{"id", chunk.id},
                    {"coords", {chunk.coords.x, chunk.coords.y, chunk.coords.z}},
                    {"snapshot_mesh_version", chunk.mesh_version},
                    {"snapshot_indices", chunk.terrain_index_count},
                    {"pool_resident", uploaded},
                    {"pool_allocation_valid", allocated},
                    {"current_uploaded_coverage", uploaded},
                    {"culler_visible", visible},
                    {"submitted",
                     m_terrain_coverage_live_submit_observed && visible && allocated &&
                         upload.uploaded_indices != 0}};
        if (chunk.source_chunk)
            row["source_lod_observed_at_capture"] = chunk.source_chunk->current_lod.load();
        if (found != m_chunk_render_data.end()) {
            row["uploaded_mesh_version"] = found->second.mesh_version;
            row["uploaded_indices"] = found->second.element_count;
        }
        rows.push_back(std::move(row));
    }
    out["live"].update({{"submit_observed", m_terrain_coverage_live_submit_observed},
                        {"snapshot_count", snapshots.size()},
                        {"truncated", snapshots.size() > limit},
                        {"scope", "prepared_renderable_snapshots_not_all_requested_chunks"},
                        {"chunks", std::move(rows)}});
    return out;
}

nlohmann::json RenderPipeline::terrain_coverage_camera_metadata() const {
    Json metadata;
    // Describe the stored scene depth, not the conventional shadow-map depth.
    metadata["depth_convention"] = {{"schema", "luminumbra.depth_convention.v1"},
                                    {"direction", "reversed"},
                                    {"clip_depth_range", "zero_to_one"},
                                    {"clear_value", 0.0f},
                                    {"near_plane_depth", 1.0f},
                                    {"far_plane_depth", 0.0f},
                                    {"near_plane_m", NEAR_PLANE},
                                    {"far_plane_m", m_frame_prepared.far_plane},
                                    {"storage_format", "GL_DEPTH_COMPONENT32F"}};
    metadata["view"] = std::vector<float>(glm::value_ptr(m_frame_prepared.view),
                                          glm::value_ptr(m_frame_prepared.view) + 16);
    auto projection = m_frame_prepared.projection;
    projection[2][0] += m_taau_jitter_ndc.x;
    projection[2][1] += m_taau_jitter_ndc.y;
    metadata["gbuffer_projection"] =
        std::vector<float>(glm::value_ptr(projection), glm::value_ptr(projection) + 16);
    metadata["jitter_ndc"] = {m_taau_jitter_ndc.x, m_taau_jitter_ndc.y};
    metadata["taau_enabled"] = m_taau_enabled;
    return metadata;
}

bool RenderPipeline::capture_terrain_coverage(const std::filesystem::path& out_dir) const {
    // A capture is immutable. Failure leaves no complete manifest, and reruns
    // must choose a new directory. Bound memory/I/O before any readback.
    constexpr std::size_t max_pixels = 16u * 1024u * 1024u;
    if (m_internal_height == 0 || m_screen_height == 0 ||
        static_cast<std::size_t>(m_internal_width) > max_pixels / m_internal_height ||
        static_cast<std::size_t>(m_screen_width) > max_pixels / m_screen_height) {
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(m_internal_width) * m_internal_height;
    const std::size_t color_pixels = static_cast<std::size_t>(m_screen_width) * m_screen_height;
    if (!m_terrain_coverage_enabled || !m_frame_prepared.valid || !render_uploads_settled() ||
        m_offscreen_target_active || !gbuffer().fbo_id || !pixels || pixels > max_pixels ||
        !color_pixels || color_pixels > max_pixels)
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
    receipt.update(terrain_coverage_camera_metadata());
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
        glReadPixels(0,
                     0,
                     static_cast<GLsizei>(m_screen_width),
                     static_cast<GLsizei>(m_screen_height),
                     GL_RGB,
                     GL_UNSIGNED_BYTE,
                     color.data());
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
            glReadPixels(0,
                         0,
                         static_cast<GLsizei>(m_internal_width),
                         static_cast<GLsizei>(m_internal_height),
                         format,
                         GL_FLOAT,
                         values.data());
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
        glReadPixels(0,
                     0,
                     static_cast<GLsizei>(m_internal_width),
                     static_cast<GLsizei>(m_internal_height),
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     encoded.data());
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
        // These are source attachments, not independent additive lighting AOVs.
        // Fog is replayed separately; canopy visibility has no standalone target.
        const auto& context = m_frame_lighting_ctx;
        Json sources = Json::object();
        if (!CaptureTexture(out_dir, "lit-scene", context.lit_scene_color.id, GL_RGB, 3, sources) ||
            !CaptureTexture(out_dir, "opaque-scene", context.opaque_scene.id, GL_RGB, 3, sources) ||
            !CaptureTexture(out_dir, "sky-view", context.sky_view_lut.id, GL_RGB, 3, sources) ||
            !CaptureTexture(
                out_dir, "transmittance", context.transmittance_lut.id, GL_RGB, 3, sources) ||
            !CaptureTexture(out_dir, "ambient-ssao", context.ssao_blur.id, GL_RED, 1, sources) ||
            !CaptureTexture(out_dir,
                            "shadow-depth",
                            context.shadow_depth_array.id,
                            GL_DEPTH_COMPONENT,
                            1,
                            sources) ||
            !CaptureTexture(
                out_dir, "shadow-tint", context.shadow_tint_array.id, GL_RGB, 3, sources) ||
            !CaptureTexture(out_dir,
                            "fog-inscatter",
                            m_froxel_pass ? m_froxel_pass->integrated_texture() : 0,
                            GL_RGB,
                            3,
                            sources) ||
            !CaptureTexture(out_dir,
                            "fog-transmittance",
                            m_froxel_pass ? m_froxel_pass->integrated_texture() : 0,
                            GL_RGBA,
                            4,
                            sources)) {
            return false;
        }
        const bool aerial_available =
            m_aerial_pass && m_aerial_pass->shader() && m_aerial_pass->shader()->IsValid() &&
            m_sky_lut.ready() && context.screen_quad_vao != 0 &&
            m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Aerial);
        if (aerial_available) {
            if (!CaptureFog(out_dir,
                            *m_aerial_pass->shader(),
                            context,
                            m_froxel_pass ? m_froxel_pass->integrated_texture() : 0,
                            sources)) {
                return false;
            }
        } else {
            sources["aerial"] = {{"status", "unavailable"}};
        }
        receipt["lighting_sources"] = std::move(sources);
        receipt["lighting_uniforms"] =
            CaptureUniforms(m_lighting_pass ? m_lighting_pass->shader().get() : nullptr);
        receipt["fog_uniforms"] =
            CaptureUniforms(m_aerial_pass ? m_aerial_pass->shader().get() : nullptr);
        if (receipt["lighting_uniforms"].contains("status") ||
            receipt["fog_uniforms"].contains("status")) {
            return false;
        }
        receipt["volumetric_quality"] = m_volumetric_quality;
        receipt["ambient_color"] = Vec(context.sky_ambient_color);
        receipt["clouds"] = {
            {"enabled", m_cloud_state.enabled},
            {"coverage", m_cloud_state.coverage_amount},
            {"scroll_offset", {m_cloud_state.scroll_offset.x, m_cloud_state.scroll_offset.y}}};
        receipt["weather"] = {{"driven", m_weather_state.driven},
                              {"rain_intensity", m_weather_state.rain_intensity},
                              {"fog_density", m_weather_state.fog_density}};
        receipt["lighting_source_scope"] = "raw_pass_inputs_and_composites_not_additive_terms";
        receipt["unavailable_terms"] = {"canopy_visibility_no_standalone_target"};
        if (!WriteFloatPlane(
                out_dir / "normal.pfm", m_internal_width, m_internal_height, 3, normals) ||
            !WriteBytePlane(out_dir / "material.pgm", m_internal_width, m_internal_height, 1, ids))
            return false;
    }
    if (glGetError() != GL_NO_ERROR)
        return false;
    receipt["attachments"] = {
        {"color.ppm", "RGB8 final display output; top-down; no additional transfer conversion"},
        {"depth.pfm",
         "float32 reversed-Z OpenGL window depth [0,1], near=1, far/clear=0; bottom-up"},
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
