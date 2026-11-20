#include "DebugViewPass.h"
#include "core/Profiler.h"
#include "rendering/AttributedGl.h"

#include "../DiagnosticDrawTable.h"
#include "../DiagnosticTargets.h"
#include "../FarWorldReplayGl.h"
#include "../GBufferTargets.h"

#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "../Camera.h"            // near/far planes for Depth-mode linearization
#include "../PassShaderLayouts.h" // enumerable ExpectedLayout registry
#include "../RenderContext.h"     // the pass reads the G-buffer via ctx handles
#include "../Shader.h"
#include "PassGlHelpers.h"

namespace Luminumbra::Rendering {

namespace {
nlohmann::json JoinRecord(const nlohmann::json& table, const DiagnosticId& id) {
    if (!ValidDiagnosticId(id) || id[0] == 0 || !table.is_object() || !table.contains("draws") ||
        !table.contains("meshes") || !table["draws"].is_array() || !table["meshes"].is_array() ||
        table.value("schema", nlohmann::json{}) != 1 || table["draws"].size() > 65536 ||
        table["meshes"].size() > 65536 || id[0] > table["draws"].size() ||
        id[1] > table["meshes"].size()) {
        return {};
    }
    const auto& draw = table["draws"][id[0] - 1];
    const auto& mesh = table["meshes"][id[1] - 1];
    if (!draw.is_object() || !mesh.is_object() || !draw.contains("supported") ||
        !draw["supported"].is_boolean() || !draw.contains("shader_variant") ||
        !draw["shader_variant"].is_string() || !draw.contains("chunk") ||
        !draw["chunk"].is_array() || draw["chunk"].size() != 3 || !mesh.contains("path") ||
        !mesh["path"].is_string() || !mesh.contains("provenance") ||
        !mesh["provenance"].is_number_unsigned()) {
        return {};
    }
    for (const auto& axis : draw["chunk"]) {
        if (!axis.is_number_integer() || axis < std::numeric_limits<std::int32_t>::min() ||
            axis > std::numeric_limits<std::int32_t>::max()) {
            return {};
        }
    }
    const nlohmann::json missing;
    if (draw.value("id", missing) != id[0] || draw.value("mesh", missing) != id[1] ||
        draw.value("lod", missing) != id[2] || mesh.value("id", missing) != id[1] ||
        draw.value("ownership", missing) != (id[3] & 255U) ||
        draw.value("pass", missing) != (id[3] >> 8U)) {
        return {};
    }
    return {{"draw", draw}, {"mesh", mesh}};
}

struct InspectorStorageBinding {
    GLint storage = 0;
    GLint indexed_storage = 0;
    GLint64 storage_start = 0;
    GLint64 storage_size = 0;
    InspectorStorageBinding() {
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &storage);
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &indexed_storage);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 7, &storage_start);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 7, &storage_size);
    }
    ~InspectorStorageBinding() {
        if (indexed_storage != 0 && storage_size > 0) {
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER,
                              7,
                              static_cast<GLuint>(indexed_storage),
                              static_cast<GLintptr>(storage_start),
                              static_cast<GLsizeiptr>(storage_size));
        } else {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, static_cast<GLuint>(indexed_storage));
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(storage));
    }
};

// Snapshot operations affect six texture units and one SSBO binding only.
struct InspectorBindings {
    InspectorStorageBinding storage_binding;
    GLint framebuffer = 0;
    std::array<GLfloat, 4> viewport{};
    GLint program = 0;
    GLint vao = 0;
    GLint active = 0;
    GLint texture_buffer = 0;
    std::array<GLint, 6> textures{};
    std::array<GLint, 2> polygon{};
    std::array<GLboolean, 4> color_mask{};
    static constexpr std::array<GLenum, 6> capabilities = {GL_DEPTH_TEST,
                                                           GL_CULL_FACE,
                                                           GL_RASTERIZER_DISCARD,
                                                           GL_DITHER,
                                                           GL_FRAMEBUFFER_SRGB,
                                                           GL_COLOR_LOGIC_OP};
    static constexpr std::array<GLenum, 2> indexed_capabilities = {GL_BLEND, GL_SCISSOR_TEST};
    std::array<GLboolean, capabilities.size()> enabled{};
    std::array<GLboolean, indexed_capabilities.size()> indexed_enabled{};
    InspectorBindings() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetFloati_v(GL_VIEWPORT, 0, viewport.data());
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_TEXTURE_BUFFER_BINDING, &texture_buffer);
        glGetIntegerv(GL_POLYGON_MODE, polygon.data());
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, color_mask.data());
        for (std::size_t index = 0; index < textures.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
            glGetIntegerv(index == 5 ? GL_TEXTURE_BINDING_BUFFER : GL_TEXTURE_BINDING_2D,
                          &textures[index]);
        }
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            enabled[index] = glIsEnabled(capabilities[index]);
        }
        for (std::size_t index = 0; index < indexed_capabilities.size(); ++index) {
            indexed_enabled[index] = glIsEnabledi(indexed_capabilities[index], 0);
        }
    }
    ~InspectorBindings() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewportIndexedf(0, viewport[0], viewport[1], viewport[2], viewport[3]);
        AttributedGl::UseProgram(glUseProgram, static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vao));
        glBindBuffer(GL_TEXTURE_BUFFER, static_cast<GLuint>(texture_buffer));
        for (std::size_t index = 0; index < textures.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
            glBindTexture(index == 5 ? GL_TEXTURE_BUFFER : GL_TEXTURE_2D,
                          static_cast<GLuint>(textures[index]));
        }
        glActiveTexture(static_cast<GLenum>(active));
        glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(polygon[0]));
        glColorMaski(0, color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            if (enabled[index] == GL_TRUE) {
                glEnable(capabilities[index]);
            } else {
                glDisable(capabilities[index]);
            }
        }
        for (std::size_t index = 0; index < indexed_capabilities.size(); ++index) {
            if (indexed_enabled[index] == GL_TRUE) {
                glEnablei(indexed_capabilities[index], 0);
            } else {
                glDisablei(indexed_capabilities[index], 0);
            }
        }
    }
};
} // namespace

bool DebugViewPass::select_pixel(Controls& control,
                                 double window_x,
                                 double window_y,
                                 const Viewport& viewport) {
    if (!control.enabled || !control.inspector_open || !control.armed || !std::isfinite(window_x) ||
        !std::isfinite(window_y) || !std::isfinite(viewport.window_width) ||
        !std::isfinite(viewport.window_height) || !std::isfinite(viewport.left) ||
        !std::isfinite(viewport.top) || !std::isfinite(viewport.width) ||
        !std::isfinite(viewport.height) || viewport.window_width <= 0 ||
        viewport.window_height <= 0 || viewport.framebuffer.width == 0 ||
        viewport.framebuffer.height == 0 || viewport.width <= 0 || viewport.height <= 0 ||
        viewport.left < 0 || viewport.top < 0 ||
        viewport.width > viewport.framebuffer.width - viewport.left ||
        viewport.height > viewport.framebuffer.height - viewport.top || control.extent.width == 0 ||
        control.extent.height == 0 || control.extent.width > 16384 ||
        control.extent.height > 16384) {
        return false;
    }
    const double image_x =
        window_x * viewport.framebuffer.width / viewport.window_width - viewport.left;
    const double image_y =
        window_y * viewport.framebuffer.height / viewport.window_height - viewport.top;
    if (!std::isfinite(image_x) || !std::isfinite(image_y) || image_x < 0 || image_y < 0 ||
        image_x >= viewport.width || image_y >= viewport.height) {
        return false;
    }
    control.x = static_cast<int>(std::floor(image_x / viewport.width * control.extent.width));
    control.y = static_cast<int>(std::floor(image_y / viewport.height * control.extent.height));
    request_inspection(control);
    return true;
}

void DebugViewPass::invalidate_selection(Controls& control) {
    control.x = -1;
    control.y = -1;
    control.frozen = false;
    control.refresh = false;
    control.inspect_requested = false;
    control.frame_invalidated = true;
    control.zoom_texture = 0;
    control.image_texture = 0;
    control.probe_json.clear();
    control.result.clear();
    control.status = "Resize invalidated selection and frozen frame; refresh to capture";
}

std::string DebugViewPass::pixel_json(const DiagnosticExtent& extent,
                                      int x,
                                      int y,
                                      const InspectorPixel& pixel) {
    std::uint32_t material = 0;
    if (x < 0 || y < 0 || !decode_material(pixel.albedo[3], material)) {
        return {};
    }
    return BuildDiagnosticProbe(
        extent,
        static_cast<std::uint32_t>(x),
        static_cast<std::uint32_t>(y),
        {.id = pixel.id,
         .albedo = {pixel.albedo[0], pixel.albedo[1], pixel.albedo[2]},
         .normal = {pixel.normal_depth[0], pixel.normal_depth[1], pixel.normal_depth[2]},
         .depth = pixel.normal_depth[3],
         .material = material});
}

struct DebugViewPass::Snapshot {
    std::unique_ptr<Shader> shader;
    DiagnosticTargets targets;
    DiagnosticDrawTable draws;
    nlohmann::json table;
    GLuint metadata = 0;
    GLuint metadata_texture = 0;
    GLuint sample_buffer = 0;
    GLuint sample_fbo = 0;
    GLuint sample_texture = 0;
    bool ready = false;
    bool images_ready = false;
    bool inspect_requested = false;
    bool freeze_requested = false;
    std::array<GLuint, 3> images{}; // normal/material, albedo, hardware depth; one frame only.
    std::uint64_t frame = 0;
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    void reset_sample() {
        glDeleteBuffers(1, &sample_buffer);
        glDeleteFramebuffers(1, &sample_fbo);
        glDeleteTextures(1, &sample_texture);
        sample_buffer = 0;
        sample_fbo = 0;
        sample_texture = 0;
    }
    ~Snapshot() {
        glDeleteBuffers(1, &metadata);
        glDeleteTextures(1, &metadata_texture);
        reset_sample();
        glDeleteTextures(static_cast<GLsizei>(images.size()), images.data());
    }
};

class DebugViewPass::Frame {
public:
    explicit Frame(DebugViewPass& pass)
        : pass_(pass)
        , capture_(std::make_unique<GBufferDiagnosticCapture>(pass.m_snapshot->targets,
                                                              pass.m_snapshot->draws)) {}
    ~Frame() {
        if (capture_) {
            AttributedGl::UseProgram(glUseProgram, 0U);
            controls().status = controls().result = "Snapshot aborted";
        }
    }
    void finish(const RenderContext& ctx) {
        if (!capture_) {
            return;
        }
        const bool complete = capture_->complete();
        // Retire the current variant before deletion so the gather saves a valid binding.
        AttributedGl::UseProgram(glUseProgram, 0U);
        capture_.reset();
        if (!complete) {
            controls().status = controls().result =
                "Snapshot refused: incomplete or unsupported coverage";
            return;
        }
        try {
            pass_.finish_frame(ctx);
        } catch (const std::exception&) {
            controls().status = "Snapshot unavailable";
            pass_.m_snapshot->ready = false;
        }
    }

private:
    DebugViewPass& pass_;
    std::unique_ptr<GBufferDiagnosticCapture> capture_;
};

std::string DebugViewPass::join(std::string_view table, const DiagnosticId& id) {
    // The producing registry caps its schema-1 JSON at 16 MiB.
    if (table.size() > 16 * 1024 * 1024) {
        return {};
    }
    const auto document = nlohmann::json::parse(table, nullptr, false);
    if (document.is_discarded()) {
        return {};
    }
    const auto record = JoinRecord(document, id);
    return record.is_null() ? std::string{} : record.dump();
}

std::string_view DebugViewPass::contribution_legend(int mode) {
    if (DiagnosticContributions::surface_mode(mode)) {
        return DiagnosticContributions::legend(mode);
    }
    switch (mode) {
        case ShadowCascade:
            return "Production cascade: 0=red, 1=green, 2=blue, 3=yellow; UV modulates tint; "
                   "white=outside every cascade, orange=outside selected cascade; "
                   "magenta hatch=bypassed/unavailable";
        case ShadowFootprint:
            return "Production shadow footprint: RG=shadow UV; B=log2(texels/pixel)/16+0.5, "
                   "clamped [0,1]; 5x5 PCF taps at +/-2 shadow texels; "
                   "white=outside every cascade, orange=outside selected cascade";
        case Overdraw:
            return "Overdraw: additive opaque deferred fragment count after material discard; "
                   "depth test OFF, no depth writes; 0=black, 1..15=blue-to-yellow bands, "
                   ">=16=cyan saturation; authored/impostor/forward/sky unavailable";
        case Wireframe:
            return "Wireframe over shaded: depth-tested (reversed-Z GEQUAL), no depth writes; "
                   "opaque G-buffer draws only; authored/impostor producers unavailable";
        case DirectSun:
            return "Deferred direct sun: linear BRDF, shadow, cloud and transmission; RGB x/(1+x)";
        case DirectMoon:
            return "Deferred direct moon: wrapped diffuse + specular, shadow and lunar tint; RGB "
                   "x/(1+x)";
        case Ambient:
            return "Deferred sky ambient: diffuse + environment specular, AO and sky visibility; "
                   "RGB x/(1+x)";
        case ShadowFactor:
            return "Deferred key shadow factor: sun by day, softened moon at night; white=lit";
        case Ao:
            return "Deferred applied AO: SSAO, multiplied by material AO only for authored "
                   "surfaces; white=unoccluded";
        case Emissive:
            return "Deferred emissive: crystal + field + marker + authored emission; RGB x/(1+x)";
        case Specular:
            return "Deferred specular: sun + tinted moon + points + environment; RGB x/(1+x)";
        case Roughness:
            return "Deferred effective roughness: clamped and snow-adjusted; black=0, white=1";
        case Metallic:
            return "Deferred metallic: black=dielectric, white=metal";
        case NormalView:
            return "Deferred decoded view normal: RGB = XYZ * 0.5 + 0.5";
        case NormalWorld:
            return "Deferred transformed world normal: RGB = XYZ * 0.5 + 0.5";
        case SkyVisibility:
            return "Deferred screen-space sky visibility: white=open; 1 when disabled";
        case MaterialAo:
            return "Deferred material AO: decoded signed far-surface tag; white=unoccluded";
        case Lighting:
            return "Pre-albedo sun + moon + point diffuse irradiance / PI, before BRDF and lunar "
                   "tint; RGB x/(1+x)";
        case Fog:
            return "Fog / atmosphere contribution: unsupported (producer not instrumented)";
        case ForwardPlane:
            return "Forward contributor plane: unsupported (producer not instrumented)";
        case SkyPlane:
            return "Sky contributor plane: unsupported (producer not instrumented)";
        case ShadowPlane:
            return "Shadow contributor plane: unsupported (producer not instrumented)";
        default:
            return "Unsupported contribution mode";
    }
}

void DebugViewPass::set_mode(int mode) {
    m_mode = mode;
    if (m_snapshot && !controls().enabled && !controls().wireframe) {
        invalidate_selection(controls());
        controls().status = "Identity diagnostics disabled";
        m_snapshot.reset();
    }
}

// Surface instrumentation shares this pass's lifetime and existing native target.
// No new translation unit or source-list registration is required.
struct DebugViewPass::SurfaceState {
    struct Variant {
        const Shader* source = nullptr;
        std::uint64_t generation = 0;
        bool ladder = false;
        std::unique_ptr<Shader> shader;
    };
    // Three vertex stages times two backends; no unbounded per-frame cache.
    std::array<Variant, 6> variants{};
    GLuint texture = 0;
    std::unique_ptr<ReplayStateAccess> replay_access;
    std::unique_ptr<ReplayGlGuard> replay_guard;
    DiagnosticExtent extent{};
    int view = None;
    GLuint framebuffer = 0;
    GLint buffer_count = 0;
    std::array<GLenum, 32> buffers{};
    std::array<GLboolean, 4> mask{};
    GLboolean blend = GL_FALSE;
    bool attached = false;
    bool ready = false;
    bool supported = false;

    ~SurfaceState() {
        detach();
        if (texture != 0) {
            glDeleteTextures(1, &texture);
        }
    }
    bool storage(const DiagnosticExtent& requested) {
        if (!contribution_extent(requested.width, requested.height)) {
            return false;
        }
        if (texture != 0 && extent.width == requested.width && extent.height == requested.height) {
            return true;
        }
        GLint maximum = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        if (maximum <= 0 || requested.width > static_cast<unsigned int>(maximum) ||
            requested.height > static_cast<unsigned int>(maximum)) {
            return false;
        }
        // RGBA32F, <=16M pixels / 256 MiB; transactional resize peaks at 512 MiB.
        GLuint replacement = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &replacement);
        if (replacement == 0) {
            return false;
        }
        glTextureStorage2D(replacement,
                           1,
                           GL_RGBA32F,
                           static_cast<GLsizei>(requested.width),
                           static_cast<GLsizei>(requested.height));
        glTextureParameteri(replacement, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(replacement, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLint allocated = 0;
        glGetTextureLevelParameteriv(replacement, 0, GL_TEXTURE_WIDTH, &allocated);
        if (allocated != static_cast<GLint>(requested.width)) {
            glDeleteTextures(1, &replacement);
            return false;
        }
        if (texture != 0) {
            glDeleteTextures(1, &texture);
        }
        texture = replacement;
        extent = requested;
        return true;
    }
    void attach(const RenderContext& ctx, const GBuffer& gbuffer) {
        ready = false;
        supported = false;
        if (attached || gbuffer.fbo_id == 0) {
            return;
        }
        GLint bound = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        if (static_cast<GLuint>(bound) != gbuffer.fbo_id) {
            return;
        }
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &buffer_count);
        GLint attachments = 0;
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &attachments);
        if (buffer_count < 7 || buffer_count > static_cast<GLint>(buffers.size()) ||
            attachments < 7) {
            return;
        }
        GLint occupied = GL_NONE;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                              &occupied);
        if (occupied != GL_NONE || !storage({ctx.internal_w(), ctx.internal_h()})) {
            return;
        }
        if (replay_mode(view)) {
            // Existing replay journal restores bindings, raster state and mutated objects,
            // including every indexed blend function/mask and separate framebuffer bindings.
            replay_access = MakeFarWorldReplayGlAccess();
            replay_guard = std::make_unique<ReplayGlGuard>(*replay_access);
        }
        for (GLint index = 0; index < buffer_count; ++index) {
            GLint value = GL_NONE;
            glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(index), &value);
            buffers[static_cast<std::size_t>(index)] = static_cast<GLenum>(value);
        }
        glGetBooleani_v(GL_COLOR_WRITEMASK, 6, mask.data());
        blend = glIsEnabledi(GL_BLEND, 6);
        glClearTexImage(texture, 0, GL_RGBA, GL_FLOAT, nullptr);
        framebuffer = gbuffer.fbo_id;
        attached = true;
        glFramebufferTexture2D(
            GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, texture, 0);
        auto outputs = buffers;
        if (replay_mode(view)) {
            outputs.fill(GL_NONE); // replay can never write a production colour attachment
        }
        outputs[6] = GL_COLOR_ATTACHMENT6;
        glDrawBuffers(buffer_count, outputs.data());
        glColorMaski(6, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisablei(GL_BLEND, 6);
        if (replay_mode(view)) {
            glPolygonMode(GL_FRONT_AND_BACK, view == Overdraw ? GL_FILL : GL_LINE);
            glLineWidth(1.0f); // supported on every core-profile implementation
            DebugViewPass::replay_depth_policy();
            if (view == Overdraw) {
                // Float32 addition is integer-exact through 2^24; larger values remain
                // saturated in the display (cap 16) and inspector, never an exact claim.
                glEnablei(GL_BLEND, 6);
                glBlendFuncSeparatei(6, GL_ONE, GL_ONE, GL_ONE, GL_ONE);
                glBlendEquationSeparatei(6, GL_FUNC_ADD, GL_FUNC_ADD);
            }
        }
        supported = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!supported) {
            detach();
        }
    }
    void detach() {
        if (!attached) {
            return;
        }
        GLint previous = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
        glDrawBuffers(buffer_count, buffers.data());
        glColorMaski(6, mask[0], mask[1], mask[2], mask[3]);
        if (blend == GL_TRUE) {
            glEnablei(GL_BLEND, 6);
        } else {
            glDisablei(GL_BLEND, 6);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous));
        attached = false;
        ready = supported;
        replay_guard.reset();
        replay_access.reset();
    }
    Shader* variant(Shader* base, bool ladder) {
        if (!attached) {
            return base;
        }
        if (!base || !base->IsValid()) {
            supported = false;
            glColorMaski(6, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            return base;
        }
        for (auto& entry : variants) {
            if ((entry.source == base && entry.ladder == ladder) || entry.source == nullptr) {
                if (entry.source != base || entry.generation != base->ProgramGeneration() ||
                    entry.ladder != ladder) {
                    std::vector<std::string> defines{"LUMIN_DIAG_SURFACE 1"};
                    if (ladder) {
                        defines.emplace_back("LUMINUMBRA_FAR_WORLD_LADDER 1");
                    }
                    entry.shader = std::make_unique<Shader>(base->VertexPath().c_str(),
                                                            base->FragmentPath().c_str(),
                                                            std::vector<std::string>{},
                                                            defines);
                    entry.source = base;
                    entry.generation = base->ProgramGeneration();
                    entry.ladder = ladder;
                }
                if (entry.shader && entry.shader->IsValid()) {
                    entry.shader->use();
                    // Raw-finite already emits the actual raster depth for every surviving
                    // fragment. Wire coverage needs only that alpha channel; keep the
                    // shipping G-buffer source and its scene-evidence identity intact.
                    entry.shader->setInt("u_surfaceMode", view == Wireframe ? RawFinite : view);
                    glColorMaski(6, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    return entry.shader.get();
                }
                break;
            }
        }
        supported = false;
        glColorMaski(6, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        return base;
    }
};

DebugViewPass::SurfaceFrame::SurfaceFrame(const RenderContext& ctx,
                                          const GBuffer& gbuffer,
                                          bool replay) {
    auto& control = controls();
    if (!geometry_requested() || replay_mode(geometry_view()) != replay || ctx.frozen_replay ||
        GBufferDiagnosticCapture::Current() || !control.surface_owner) {
        return;
    }
    if (control.surface_owner->m_surface && control.surface_owner->m_surface->attached) {
        return;
    }
    owner_ = control.surface_owner;
    exceptions_ = std::uncaught_exceptions();
    if (!owner_->m_surface) {
        owner_->m_surface = std::make_unique<SurfaceState>();
    }
    owner_->m_surface->view = geometry_view();
    owner_->m_surface->attach(ctx, gbuffer);
}
bool DebugViewPass::SurfaceFrame::ready() const {
    return owner_ && owner_->m_surface && owner_->m_surface->attached;
}
bool DebugViewPass::replaying() {
    const auto* owner = controls().surface_owner;
    return owner && owner->m_surface && owner->m_surface->attached &&
           replay_mode(owner->m_surface->view);
}
void DebugViewPass::replay_depth_policy() {
    if (replaying()) {
        if (controls().surface_owner->m_surface->view == Overdraw) {
            glDisable(GL_DEPTH_TEST);
        } else {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GEQUAL);
        }
        glDepthMask(GL_FALSE);
    }
}
DebugViewPass::SurfaceFrame::~SurfaceFrame() {
    if (owner_) {
        if (std::uncaught_exceptions() > exceptions_) {
            owner_->m_surface->supported = false;
        }
        owner_->m_surface->detach();
    }
}
Shader* DebugViewPass::surface_variant(Shader* base, bool ladder) {
    const auto& control = controls();
    if (geometry_requested() && control.surface_owner && control.surface_owner->m_surface) {
        return control.surface_owner->m_surface->variant(base, ladder);
    }
    return base;
}
void DebugViewPass::unsupported_surface() {
    const auto& control = controls();
    if (geometry_requested() && control.surface_owner && control.surface_owner->m_surface &&
        control.surface_owner->m_surface->attached) {
        control.surface_owner->m_surface->supported = false;
        glColorMaski(6, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    }
}

bool DebugViewPass::prepare_inspector_storage() {
    auto& control = controls();
    if (!m_snapshot) {
        m_snapshot = std::make_unique<Snapshot>();
    }
    {
        InspectorStorageBinding bindings;
        if (m_snapshot->sample_buffer == 0) {
            glGenBuffers(1, &m_snapshot->sample_buffer);
            if (m_snapshot->sample_buffer == 0) {
                control.status = control.result = "Inspector storage unavailable";
                return false;
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_snapshot->sample_buffer);
            // One bounded 48-byte record is required even with inspection closed.
            const InspectorPixel empty{};
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(InspectorPixel), &empty, GL_STREAM_READ);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_snapshot->sample_buffer);
        GLint64 sample_bytes = 0;
        glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &sample_bytes);
        if (sample_bytes != sizeof(InspectorPixel)) {
            m_snapshot->reset_sample();
            control.status = control.result = "Inspector storage unavailable";
            return false;
        }
    }
    return true;
}

std::shared_ptr<DebugViewPass::Frame> DebugViewPass::begin_frame(const RenderContext& ctx) {
    auto& control = controls();
    control.surface_owner = this;
    if (m_surface) {
        m_surface->ready = false;
    }
    if (geometry_requested()) {
        if (geometry_view() == Overdraw) {
            const DiagnosticExtent render{ctx.internal_w(), ctx.internal_h()};
            if (control.extent.width != render.width || control.extent.height != render.height) {
                invalidate_selection(control);
            }
            control.extent = render;
            control.output = {ctx.screen_width, ctx.screen_height};
            control.frozen = false; // Counts and selected-pixel values describe this live replay.
            control.image_texture = control.zoom_texture = 0;
            control.probe_json.clear();
        }
        control.refresh = false;
        return {};
    }
    if (!control.enabled || ctx.frozen_replay) {
        return {};
    }
    if (std::exchange(control.frame_invalidated, false)) {
        m_snapshot.reset();
    }
    const DiagnosticExtent render{ctx.internal_w(), ctx.internal_h()};
    const DiagnosticExtent output{ctx.screen_width, ctx.screen_height};
    if ((control.extent.width != 0 &&
         (control.extent.width != render.width || control.extent.height != render.height)) ||
        (control.output.width != 0 &&
         (control.output.width != output.width || control.output.height != output.height))) {
        invalidate();
    }
    control.extent = render;
    control.output = output;
    if (!control.inspector_open) {
        control.inspect_requested = false;
        control.zoom_texture = 0;
    }
    if (control.frozen && m_snapshot && m_snapshot->ready && m_snapshot->images_ready) {
        if (std::exchange(control.inspect_requested, false)) {
            inspect_snapshot(ctx);
        }
        control.refresh = false;
        return {};
    }
    if (!control.refresh) {
        return {};
    }
    if (contribution_mode(control.view)) {
        // Live lighting terms need no identity capture, geometry replay or readback.
        control.refresh = false;
        return {};
    }
    if (GBufferDiagnosticCapture::Current()) {
        control.status = "Another diagnostic capture owns this frame";
        return {};
    }
    control.refresh = false;
    const bool freeze_requested = std::exchange(control.frozen, false) && control.inspector_open;
    // Admit once; storage, shader and coverage failures consume this request too.
    const bool inspect_requested = std::exchange(control.inspect_requested, false);
    control.result.clear();
    control.probe_json.clear();
    control.zoom_texture = 0;
    control.image_texture = 0;
    if (!m_shader || !m_shader->IsValid()) {
        control.status = "Debug shader unavailable";
        return {};
    }
    if (!m_snapshot) {
        m_snapshot = std::make_unique<Snapshot>();
    }
    m_snapshot->ready = false;
    m_snapshot->images_ready = false;
    m_snapshot->inspect_requested = inspect_requested;
    m_snapshot->freeze_requested = freeze_requested;
    if (!m_snapshot->shader) {
        m_snapshot->shader =
            std::make_unique<Shader>(m_shader->VertexPath().c_str(),
                                     m_shader->FragmentPath().c_str(),
                                     std::vector<std::string>{},
                                     std::vector<std::string>{"LUMIN_DIAG_VIEWS 1"});
        if (!m_snapshot->shader->IsValid()) {
            control.status = "Identity shader unavailable";
            m_snapshot->shader.reset();
            return {};
        }
        if (const ExpectedLayout* layout = FindPassExpectedLayout("debug_view")) {
            auto expected = *layout;
            expected.samplers.push_back({"gIdentity", GL_UNSIGNED_INT_SAMPLER_2D, 4});
            expected.samplers.push_back({"gDrawMetadata", GL_UNSIGNED_INT_SAMPLER_BUFFER, 5});
            expected.storage_blocks.push_back({"InspectorResult", 7});
            if (!m_snapshot->shader->ValidateLayout(expected)) {
                control.status = "Identity shader layout unavailable";
                m_snapshot->shader.reset();
                return {};
            }
        }
    }
    if (!prepare_inspector_storage()) {
        return {};
    }
    m_snapshot->frame = ctx.frame_index;
    const auto view = ctx.camera ? ctx.camera->GetViewMatrix() : ctx.view;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto column = static_cast<glm::length_t>(index / 4);
        const auto row = static_cast<glm::length_t>(index % 4);
        m_snapshot->view[index] = view[column][row];
        m_snapshot->projection[index] = ctx.gbuffer_projection[column][row];
    }
    if (!m_snapshot->targets.reset()) {
        control.status = control.result = "Identity attachment still leased";
        return {};
    }
    m_snapshot->draws.clear();
    return std::make_shared<Frame>(*this);
}

void DebugViewPass::end_frame(std::shared_ptr<Frame>& frame, const RenderContext& ctx) {
    if (frame) {
        frame->finish(ctx);
        frame.reset();
    }
}

void DebugViewPass::read_inspector_pixel(GLuint buffer, InspectorPixel& pixel) {
    GLint previous = 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &previous);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(InspectorPixel), &pixel);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(previous));
}

void DebugViewPass::finish_frame(const RenderContext& ctx) {
    auto& control = controls();
    auto& shader = *m_snapshot->shader;
    const auto extent = m_snapshot->targets.extent();
    control.extent = extent;
    if (!control.enabled || !shader.IsValid() || m_vao == 0 || extent.width != ctx.internal_w() ||
        extent.height != ctx.internal_h() || m_snapshot->targets.texture() == 0) {
        control.status = "Identity attachment unavailable";
        return;
    }
    // Only on explicit refresh: <= 65,536 records / 16 MiB registry JSON.
    m_snapshot->table = nlohmann::json::parse(m_snapshot->draws.json());
    const auto& table = m_snapshot->table;
    const auto& draws = table.at("draws");
    GLint max_metadata_texels = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_metadata_texels);
    if (draws.size() > 65536 || max_metadata_texels <= 0 ||
        (draws.size() + 1) * 2 > static_cast<std::size_t>(max_metadata_texels)) {
        control.status = "Draw metadata exceeds texture-buffer capacity";
        return;
    }
    // Two uvec4 entries per draw, plus zero. At most 2,097,184 bytes.
    std::vector<DiagnosticId> metadata((draws.size() + 1) * 2);
    for (std::size_t index = 0; index < draws.size(); ++index) {
        const auto* id = m_snapshot->draws.resolve(static_cast<std::uint32_t>(index + 1));
        if (!id) {
            continue;
        }
        const auto record = JoinRecord(table, *id);
        if (record.is_null()) {
            continue;
        }
        const auto& draw = record.at("draw");
        const auto& mesh = record.at("mesh");
        std::uint32_t chunk = 0;
        for (const auto& axis : draw.at("chunk")) {
            chunk = hash_id(chunk ^ static_cast<std::uint32_t>(axis.get<std::int32_t>()));
        }
        const auto kind = producer(draw.at("shader_variant").get<std::string>(),
                                   draw.at("supported").get<bool>());
        metadata[(index + 1) * 2] = *id;
        metadata[(index + 1) * 2 + 1] = {
            chunk, static_cast<std::uint32_t>(kind), mesh.at("provenance").get<std::uint32_t>(), 0};
    }
    InspectorBindings bindings;
    if (m_snapshot->metadata == 0) {
        glGenBuffers(1, &m_snapshot->metadata);
    }
    if (m_snapshot->metadata_texture == 0) {
        glGenTextures(1, &m_snapshot->metadata_texture);
    }
    if (m_snapshot->metadata == 0 || m_snapshot->metadata_texture == 0) {
        control.status = "Draw metadata storage unavailable";
        return;
    }
    glBindBuffer(GL_TEXTURE_BUFFER, m_snapshot->metadata);
    // A failed replacement must never pass the size check using an older store.
    glBufferData(GL_TEXTURE_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    GLint64 metadata_bytes = -1;
    glGetBufferParameteri64v(GL_TEXTURE_BUFFER, GL_BUFFER_SIZE, &metadata_bytes);
    if (metadata_bytes != 0) {
        control.status = "Draw metadata storage unavailable";
        return;
    }
    glBufferData(GL_TEXTURE_BUFFER,
                 static_cast<GLsizeiptr>(metadata.size() * sizeof(DiagnosticId)),
                 metadata.data(),
                 GL_STATIC_DRAW);
    metadata_bytes = -1;
    glGetBufferParameteri64v(GL_TEXTURE_BUFFER, GL_BUFFER_SIZE, &metadata_bytes);
    if (metadata_bytes != static_cast<GLint64>(metadata.size() * sizeof(DiagnosticId))) {
        control.status = "Draw metadata storage unavailable";
        return;
    }
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_BUFFER, m_snapshot->metadata_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, m_snapshot->metadata);
    m_snapshot->ready = true;
    control.status =
        "Identity snapshot; refresh to capture a new frame. Magenta hatch = zero / unsupported.";
    if (control.inspector_open && !retain_images(ctx)) {
        control.frozen = false;
        control.result = "Snapshot image storage unavailable";
        return;
    }
    control.frozen = m_snapshot->freeze_requested && m_snapshot->images_ready;
    if (std::exchange(m_snapshot->inspect_requested, false)) {
        inspect_snapshot(ctx);
    }
}

bool DebugViewPass::retain_images(const RenderContext& ctx) {
    const auto extent = m_snapshot->targets.extent();
    // Three 4-byte AOVs plus 16-byte IDs: <= 448 MiB GPU at the existing
    // 16,777,216-pixel target bound, plus bounded draw metadata. No CPU plane copy.
    if (extent.width == 0 || extent.height == 0 || extent.width > 16384 || extent.height > 16384 ||
        extent.width > 16777216U / extent.height) {
        return false;
    }
    const std::array<GLuint, 3> sources = {
        ctx.gbuffer_normal.id, ctx.gbuffer_albedo.id, ctx.gbuffer_depth.id};
    const std::array<GLenum, 3> formats = {GL_RGBA8, GL_RGBA8, GL_DEPTH_COMPONENT32F};
    InspectorBindings bindings;
    glActiveTexture(GL_TEXTURE0);
    // Validate all three inputs before replacing storage; only shipping G-buffer formats.
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (sources[index] == 0 || glIsTexture(sources[index]) != GL_TRUE) {
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, sources[index]);
        GLint width = 0;
        GLint height = 0;
        GLint format = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
        if (width != static_cast<GLint>(extent.width) ||
            height != static_cast<GLint>(extent.height) ||
            static_cast<GLenum>(format) != formats[index]) {
            return false;
        }
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (m_snapshot->images[index] == 0) {
            glGenTextures(1, &m_snapshot->images[index]);
            if (m_snapshot->images[index] == 0) {
                return false;
            }
        }
        glBindTexture(GL_TEXTURE_2D, m_snapshot->images[index]);
        GLint immutable = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
        if (immutable == GL_FALSE) {
            glTexStorage2D(GL_TEXTURE_2D,
                           1,
                           formats[index],
                           static_cast<GLsizei>(extent.width),
                           static_cast<GLsizei>(extent.height));
        }
        GLint width = 0;
        GLint height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        if (width != static_cast<GLint>(extent.width) ||
            height != static_cast<GLint>(extent.height)) {
            return false;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glCopyImageSubData(sources[index],
                           GL_TEXTURE_2D,
                           0,
                           0,
                           0,
                           0,
                           m_snapshot->images[index],
                           GL_TEXTURE_2D,
                           0,
                           0,
                           0,
                           0,
                           static_cast<GLsizei>(extent.width),
                           static_cast<GLsizei>(extent.height),
                           1);
    }
    m_snapshot->images_ready = true;
    controls().image_texture = m_snapshot->images[1];
    return true;
}

void DebugViewPass::inspect_snapshot(const RenderContext& ctx) {
    auto& control = controls();
    if (!control.inspector_open || control.x < 0 || control.y < 0 || !m_snapshot ||
        !m_snapshot->ready || !m_snapshot->images_ready) {
        return;
    }
    const auto extent = m_snapshot->targets.extent();
    if (static_cast<std::uint32_t>(control.x) >= extent.width ||
        static_cast<std::uint32_t>(control.y) >= extent.height) {
        control.result = "No selected pixel (coordinates are top-left render texels)";
        return;
    }
    auto& shader = *m_snapshot->shader;
    const auto& table = m_snapshot->table;
    InspectorBindings bindings;
    if (m_snapshot->sample_fbo == 0) {
        glGenFramebuffers(1, &m_snapshot->sample_fbo);
        glGenTextures(1, &m_snapshot->sample_texture);
        if (m_snapshot->sample_buffer == 0 || m_snapshot->sample_fbo == 0 ||
            m_snapshot->sample_texture == 0) {
            m_snapshot->reset_sample();
            control.result = "Inspector storage unavailable";
            return;
        }
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_snapshot->sample_texture);
        // Zoom is fixed at 9 x 9 nearest texels (324 bytes GPU); only its
        // centre writes the existing 48-byte inspection record. No zoom readback.
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 9, 9);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_snapshot->sample_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               m_snapshot->sample_texture,
                               0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_snapshot->sample_fbo);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        m_snapshot->reset_sample();
        control.result = "Inspector target unavailable";
        return;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_snapshot->sample_buffer);
    GLint64 sample_bytes = 0;
    glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &sample_bytes);
    if (sample_bytes != sizeof(InspectorPixel)) {
        m_snapshot->reset_sample();
        control.result = "Inspector storage unavailable";
        return;
    }
    glViewportIndexedf(0, 0, 0, 9, 9);
    for (const auto capability : InspectorBindings::capabilities) {
        glDisable(capability);
    }
    // Only draw-buffer/viewport zero participates; preserve every other indexed enable.
    for (const auto capability : InspectorBindings::indexed_capabilities) {
        glDisablei(capability, 0);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    shader.use();
    bind_sources(ctx, shader);
    shader.setInt("u_inspect", 1);
    shader.setInt("u_zoom", 1);
    shader.setInt("u_pixelX", control.x);
    shader.setInt("u_pixelY", static_cast<int>(extent.height) - 1 - control.y);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_snapshot->sample_buffer);
    glBindVertexArray(m_vao);
    AttributedGl::Submit(glDrawArrays, static_cast<GLenum>(GL_TRIANGLES), 0, 3);
    InspectorPixel pixel;
    read_inspector_pixel(m_snapshot->sample_buffer, pixel);
    shader.setInt("u_inspect", 0);
    shader.setInt("u_zoom", 0);
    control.zoom_texture = m_snapshot->sample_texture;
    const auto record = JoinRecord(table, pixel.id);
    const auto probe = pixel_json(extent, control.x, control.y, pixel);
    if (probe.empty()) {
        control.result = "Invalid identity / unsupported pixel";
        return;
    }
    control.probe_json = probe;
    control.result =
        nlohmann::json{{"frame", m_snapshot->frame},
                       {"probe", nlohmann::json::parse(probe)},
                       {"record", record},
                       {"view", m_snapshot->view},
                       {"projection", m_snapshot->projection},
                       {"identity_supported",
                        !record.is_null() &&
                            producer(record.at("draw").at("shader_variant").get<std::string>(),
                                     record.at("draw").at("supported").get<bool>()) !=
                                ProducerKind::Unsupported}}
            .dump(2);
}

void DebugViewPass::invalidate() {
    invalidate_selection(controls());
    m_snapshot.reset();
    controls().frame_invalidated = false;
}

DebugViewPass::DebugViewPass() = default;
DebugViewPass::~DebugViewPass() {
    if (controls().surface_owner == this) {
        controls().surface_owner = nullptr;
    }
}

void DebugViewPass::init_shader(const std::filesystem::path& root_path) {
    m_contribution_shader.reset();
    m_surface.reset();
    const std::string vert = (root_path / "res/shaders/fullscreen_tri.vert").string();
    const std::string frag = (root_path / "res/shaders/debug_view.frag").string();
    m_shader = std::make_unique<Shader>(vert.c_str(), frag.c_str());
    // validate the four G-buffer samplers against the registry.
    if (m_shader && m_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("debug_view"))
            m_shader->ValidateLayout(*layout);
    }
}

void DebugViewPass::init_buffers() {
    glGenVertexArrays(1, &m_vao); // empty; the VS builds the triangle from gl_VertexID
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "debug_view.vao");
}

void DebugViewPass::destroy_buffers() {
    m_surface.reset();
    if (m_snapshot) {
        invalidate_selection(controls());
        controls().status = "Diagnostic frame released";
        m_snapshot.reset();
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
}

void DebugViewPass::reset_shader() {
    m_surface.reset();
    m_contribution_shader.reset();
    m_shader.reset();
}

void DebugViewPass::execute(const RenderContext& ctx) {
    LUMIN_PROFILE_ZONE_N("GL DebugViewPass.execute");
    // Default-OFF guard: Mode::None (0) is a true no-op so the normal render is untouched.
    if (mode() == Mode::None)
        return;
    const bool contribution_view =
        (controls().enabled || controls().wireframe) &&
        (contribution_mode(mode()) || DiagnosticContributions::surface_mode(mode()) ||
         replay_mode(mode()));
    if (contribution_view && !m_contribution_shader && m_shader) {
        m_contribution_shader =
            std::make_unique<Shader>(m_shader->VertexPath().c_str(),
                                     m_shader->FragmentPath().c_str(),
                                     std::vector<std::string>{},
                                     std::vector<std::string>{"LUMIN_DIAG_VIEWS 1"});
    }
    // Contribution views read live lighting terms and never create an identity snapshot.
    const bool identity_view =
        (controls().enabled || controls().wireframe) && mode() >= DrawId && !contribution_view;
    Shader* shader = identity_view && m_snapshot && m_snapshot->shader ? m_snapshot->shader.get()
                                                                       : m_shader.get();
    if (contribution_view) {
        // If the opt-in program is unavailable, the original shader's unknown-mode
        // sentinel paints magenta instead of leaving a plausible final scene visible.
        const bool overlay_available = m_contribution_shader && m_contribution_shader->IsValid();
        shader = overlay_available ? m_contribution_shader.get() : m_shader.get();
        // Keep the last requested count visible until another inspection or invalidation.
        // Other contribution views have no single-pixel result.
        if (mode() != Overdraw || !controls().result.starts_with("Overdraw count ")) {
            controls().result.clear();
        }
        controls().status = std::string(contribution_legend(mode()));
        controls().status +=
            overlay_available
                ? (replay_mode(mode()) ? "; magenta hatch=unavailable producer or replay target."
                   : DiagnosticContributions::surface_mode(mode())
                       ? "; magenta=raw NaN/Inf; yellow=invalid weights; hatch=unavailable/sky. "
                         "G-buffer producers only; detail blur excluded from mip scale."
                       : "; magenta hatch=unsupported/unavailable/sky; magenta=NaN, cyan=Inf. "
                         "Deferred surface only; forward/sky/shadow contributors unsupported.")
                : "; overlay shader unavailable: solid magenta error sentinel";
    }
    if (!shader || !shader->IsValid() || m_vao == 0)
        return;

    // The live program also declares InspectorResult: keep a valid bounded store
    // bound even though its contribution branches return before inspector access.
    if (contribution_view && !prepare_inspector_storage()) {
        return;
    }
    if (identity_view && (!m_snapshot || m_snapshot->sample_buffer == 0)) {
        return;
    }

    // The overlay also restores everything it touches, independently of replay.
    std::unique_ptr<ReplayStateAccess> overlay_access;
    std::unique_ptr<ReplayGlGuard> overlay_guard;
    if (replay_mode(mode())) {
        overlay_access = MakeFarWorldReplayGlAccess();
        overlay_guard = std::make_unique<ReplayGlGuard>(*overlay_access);
    }
    std::array<GLint, 2> polygon_mode{};
    std::optional<InspectorStorageBinding> storage_binding;
    if (identity_view || contribution_view) {
        // Both programs declare InspectorResult; binding 7 must hold the validated store.
        storage_binding.emplace();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_snapshot->sample_buffer);
    }
    if (identity_view || replay_mode(mode())) {
        glGetIntegerv(GL_POLYGON_MODE, polygon_mode.data());
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    // Fullscreen overlays read scene visibility from their source; wireframe blends a mask.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    if (mode() == Wireframe) {
        // Transparent mask texels preserve the existing shaded image.
        glEnablei(GL_BLEND, 0);
        glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_FUNC_ADD);
    }

    // Near/far come from the frame camera (ctx), used only for Depth-mode linearization.
    // Fall back to the historic defaults if a context arrives without a camera.
    const float near_plane = ctx.camera ? ctx.camera->GetNearPlane() : 0.1f;
    const float far_plane = ctx.camera ? ctx.camera->GetFarPlane() : 4000.0f;

    shader->use();
    shader->setInt("u_farWorldLadder", ctx.far_world_ladder ? 1 : 0);
    shader->setMat4("u_gbufferInverseProjection", ctx.gbuffer_inverse_projection);
    shader->setInt("u_mode", mode());
    if (identity_view) {
        shader->setInt("u_inspect", 0);
    }
    shader->setFloat("u_near", near_plane);
    shader->setFloat("u_far", far_plane);

    bind_sources(ctx, *shader);
    const bool inspect_count = mode() == Overdraw && controls().inspector_open &&
                               std::exchange(controls().inspect_requested, false);
    const bool count_pixel_valid = inspect_count &&
                                   contribution_extent(ctx.internal_w(), ctx.internal_h()) &&
                                   controls().x >= 0 && controls().y >= 0 &&
                                   static_cast<unsigned int>(controls().x) < ctx.internal_w() &&
                                   static_cast<unsigned int>(controls().y) < ctx.internal_h();
    if (mode() == Overdraw) {
        shader->setInt("u_inspect", count_pixel_valid ? 1 : 0);
        shader->setInt("u_pixelX", count_pixel_valid ? controls().x : 0);
        shader->setInt("u_pixelY",
                       count_pixel_valid ? static_cast<int>(ctx.internal_h()) - 1 - controls().y
                                         : 0);
    }
    if (identity_view &&
        (!m_snapshot->ready || m_snapshot->targets.extent().width != ctx.internal_w() ||
         m_snapshot->targets.extent().height != ctx.internal_h())) {
        shader->setInt("u_identityReady", 0);
    }

    if (inspect_count) {
        const InspectorPixel empty{};
        glNamedBufferSubData(m_snapshot->sample_buffer, 0, sizeof(empty), &empty);
    }
    glBindVertexArray(m_vao);
    Luminumbra::Rendering::AttributedGl::Submit(
        glDrawArrays, static_cast<GLenum>(GL_TRIANGLES), 0, 3);
    if (inspect_count) {
        controls().result = "Overdraw count unavailable (producer, target or selected pixel)";
        if (count_pixel_valid) {
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            InspectorPixel pixel{};
            read_inspector_pixel(m_snapshot->sample_buffer, pixel);
            if (pixel.normal_depth[3] == 1.0f) {
                controls().result =
                    pixel.normal_depth[0] >= 16.0f
                        ? "Overdraw count >=16 (saturated); depth test OFF"
                        : "Overdraw count " +
                              std::to_string(static_cast<int>(pixel.normal_depth[0])) +
                              "; depth test OFF";
            }
        }
    }
    glBindVertexArray(0);
    if (identity_view) {
        glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(polygon_mode[0]));
    }
}

void DebugViewPass::bind_sources(const RenderContext& ctx, Shader& shader) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id);
    shader.setInt("gPosition", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_normal.id);
    shader.setInt("gNormalMaterial", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_albedo.id);
    shader.setInt("gAlbedo", 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    shader.setInt("gDepth", 3);

    if (DiagnosticContributions::surface_mode(mode()) || replay_mode(mode())) {
        const bool ready = (controls().enabled || controls().wireframe) && !ctx.frozen_replay &&
                           m_surface && m_surface->ready && m_surface->view == mode() &&
                           m_surface->extent.width == ctx.internal_w() &&
                           m_surface->extent.height == ctx.internal_h();
        shader.setInt("u_contributionReady", ready ? 1 : 0);
        if (mode() == RawFinite) {
            shader.setInt("u_rawMaterialReady", ctx.gbuffer_material.id != 0 ? 1 : 0);
            shader.setInt("u_rawMotionReady", ctx.motion_vectors.id != 0 ? 1 : 0);
            shader.setInt("u_rawAuthoredReady", ctx.authored_surface.id != 0 ? 1 : 0);
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_material.id);
            glActiveTexture(GL_TEXTURE8);
            glBindTexture(GL_TEXTURE_2D, ctx.motion_vectors.id);
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_2D, ctx.authored_surface.id);
        }
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, ready ? m_surface->texture : 0);
        return;
    }
    if (contribution_mode(mode())) {
        const auto& control = controls();
        const bool ready = control.enabled && supported_contribution(mode()) &&
                           !ctx.frozen_replay && control.contribution_texture != 0 &&
                           control.contribution_view == mode() &&
                           control.contribution_size.width == ctx.internal_w() &&
                           control.contribution_size.height == ctx.internal_h();
        shader.setInt("u_contributionReady", ready ? 1 : 0);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, ready ? control.contribution_texture : 0);
        return;
    }
    if (!m_snapshot || &shader != m_snapshot->shader.get()) {
        return;
    }
    if (m_snapshot->images_ready) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_snapshot->images[0]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_snapshot->images[1]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_snapshot->images[2]);
    }
    const bool ready = controls().enabled && m_snapshot && m_snapshot->ready;
    shader.setInt("u_identityReady", ready ? 1 : 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ready ? m_snapshot->targets.texture() : 0);
    shader.setInt("gIdentity", 4);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_BUFFER, ready ? m_snapshot->metadata_texture : 0);
    shader.setInt("gDrawMetadata", 5);
}

} // namespace Luminumbra::Rendering
