#include "DebugViewPass.h"
#include "core/Profiler.h"
#include "rendering/AttributedGl.h"

#include "../DiagnosticDrawTable.h"
#include "../DiagnosticTargets.h"
#include "../GBufferTargets.h"

#include <array>
#include <exception>
#include <limits>
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

// Snapshot operations affect six texture units and one SSBO binding only.
struct InspectorBindings {
    GLint framebuffer = 0;
    std::array<GLint, 4> viewport{};
    GLint program = 0;
    GLint vao = 0;
    GLint active = 0;
    GLint storage = 0;
    GLint indexed_storage = 0;
    GLint64 storage_start = 0;
    GLint64 storage_size = 0;
    GLint texture_buffer = 0;
    std::array<GLint, 6> textures{};
    std::array<GLint, 2> polygon{};
    static constexpr std::array<GLenum, 5> capabilities = {
        GL_DEPTH_TEST, GL_BLEND, GL_SCISSOR_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD};
    std::array<GLboolean, capabilities.size()> enabled{};
    InspectorBindings() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &storage);
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &indexed_storage);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 7, &storage_start);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 7, &storage_size);
        glGetIntegerv(GL_TEXTURE_BUFFER_BINDING, &texture_buffer);
        glGetIntegerv(GL_POLYGON_MODE, polygon.data());
        for (std::size_t index = 0; index < textures.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
            glGetIntegerv(index == 5 ? GL_TEXTURE_BINDING_BUFFER : GL_TEXTURE_BINDING_2D,
                          &textures[index]);
        }
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            enabled[index] = glIsEnabled(capabilities[index]);
        }
    }
    ~InspectorBindings() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        AttributedGl::UseProgram(glUseProgram, static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vao));
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
        glBindBuffer(GL_TEXTURE_BUFFER, static_cast<GLuint>(texture_buffer));
        for (std::size_t index = 0; index < textures.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
            glBindTexture(index == 5 ? GL_TEXTURE_BUFFER : GL_TEXTURE_2D,
                          static_cast<GLuint>(textures[index]));
        }
        glActiveTexture(static_cast<GLenum>(active));
        glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(polygon[0]));
        for (std::size_t index = 0; index < capabilities.size(); ++index) {
            if (enabled[index] == GL_TRUE) {
                glEnable(capabilities[index]);
            } else {
                glDisable(capabilities[index]);
            }
        }
    }
};
} // namespace

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
    }
};

class DebugViewPass::Frame {
public:
    Frame(DebugViewPass& pass, const RenderContext& ctx)
        : pass_(pass)
        , ctx_(ctx)
        , exceptions_(std::uncaught_exceptions())
        , capture_(std::make_unique<GBufferDiagnosticCapture>(pass.m_snapshot->targets,
                                                              pass.m_snapshot->draws)) {}
    ~Frame() {
        capture_.reset();
        if (std::uncaught_exceptions() != exceptions_) {
            controls().status = "Snapshot aborted";
            return;
        }
        try {
            pass_.finish_frame(ctx_);
        } catch (const std::exception&) {
            controls().status = "Snapshot unavailable";
            pass_.m_snapshot->ready = false;
        }
    }

private:
    DebugViewPass& pass_;
    RenderContext ctx_;
    int exceptions_;
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

void DebugViewPass::set_mode(int mode) {
    m_mode = mode;
    if (m_snapshot && !controls().enabled) {
        m_snapshot.reset();
    }
}

std::shared_ptr<DebugViewPass::Frame> DebugViewPass::begin_frame(const RenderContext& ctx) {
    auto& control = controls();
    if (!control.enabled || !control.refresh || ctx.frozen_replay) {
        return {};
    }
    if (GBufferDiagnosticCapture::Current()) {
        control.status = "Another diagnostic capture owns this frame";
        return {};
    }
    control.refresh = false;
    control.result.clear();
    if (!m_shader || !m_shader->IsValid()) {
        control.status = "Debug shader unavailable";
        return {};
    }
    if (!m_snapshot) {
        m_snapshot = std::make_unique<Snapshot>();
    }
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
    if (control.extent.width != 0 &&
        (control.extent.width != ctx.internal_w() || control.extent.height != ctx.internal_h())) {
        control.x = -1;
        control.y = -1;
    }
    const auto view = ctx.camera ? ctx.camera->GetViewMatrix() : ctx.view;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto column = static_cast<glm::length_t>(index / 4);
        const auto row = static_cast<glm::length_t>(index % 4);
        m_snapshot->view[index] = view[column][row];
        m_snapshot->projection[index] = ctx.gbuffer_projection[column][row];
    }
    m_snapshot->ready = false;
    m_snapshot->draws.clear();
    m_snapshot->targets.reset();
    return std::make_shared<Frame>(*this, ctx);
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
    if (!control.inspector_open || control.x < 0 || control.y < 0 ||
        static_cast<std::uint32_t>(control.x) >= extent.width ||
        static_cast<std::uint32_t>(control.y) >= extent.height) {
        control.result = "No selected pixel (coordinates are top-left render texels)";
        return;
    }
    if (m_snapshot->sample_buffer == 0) {
        glGenBuffers(1, &m_snapshot->sample_buffer);
        glGenFramebuffers(1, &m_snapshot->sample_fbo);
        glGenTextures(1, &m_snapshot->sample_texture);
        if (m_snapshot->sample_buffer == 0 || m_snapshot->sample_fbo == 0 ||
            m_snapshot->sample_texture == 0) {
            m_snapshot->reset_sample();
            control.result = "Inspector storage unavailable";
            return;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_snapshot->sample_buffer);
        const InspectorPixel empty{};
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(InspectorPixel), &empty, GL_STREAM_READ);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_snapshot->sample_texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
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
    glViewport(0, 0, 1, 1);
    for (const auto capability : InspectorBindings::capabilities) {
        glDisable(capability);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    shader.use();
    bind_sources(ctx, shader);
    shader.setInt("u_inspect", 1);
    shader.setInt("u_pixelX", control.x);
    shader.setInt("u_pixelY", static_cast<int>(extent.height) - 1 - control.y);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_snapshot->sample_buffer);
    glBindVertexArray(m_vao);
    AttributedGl::Submit(glDrawArrays, static_cast<GLenum>(GL_TRIANGLES), 0, 3);
    InspectorPixel pixel;
    read_inspector_pixel(m_snapshot->sample_buffer, pixel);
    shader.setInt("u_inspect", 0);
    const auto record = JoinRecord(table, pixel.id);
    std::uint32_t material = 0;
    if (!decode_material(pixel.albedo[3], material)) {
        control.result = "Invalid material / unsupported pixel";
        return;
    }
    DiagnosticSample sample{
        .id = pixel.id,
        .albedo = {pixel.albedo[0], pixel.albedo[1], pixel.albedo[2]},
        .normal = {pixel.normal_depth[0], pixel.normal_depth[1], pixel.normal_depth[2]},
        .depth = pixel.normal_depth[3],
        .material = material};
    const auto probe = BuildDiagnosticProbe(extent,
                                            static_cast<std::uint32_t>(control.x),
                                            static_cast<std::uint32_t>(control.y),
                                            sample);
    if (probe.empty()) {
        control.result = "Invalid identity / unsupported pixel";
        return;
    }
    control.result =
        nlohmann::json{{"probe", nlohmann::json::parse(probe)},
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

DebugViewPass::DebugViewPass() = default;
DebugViewPass::~DebugViewPass() = default;

void DebugViewPass::init_shader(const std::filesystem::path& root_path) {
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
    m_snapshot.reset();
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
}

void DebugViewPass::reset_shader() {
    m_shader.reset();
}

void DebugViewPass::execute(const RenderContext& ctx) {
    LUMIN_PROFILE_ZONE_N("GL DebugViewPass.execute");
    // Default-OFF guard: Mode::None (0) is a true no-op so the normal render is untouched.
    if (mode() == Mode::None)
        return;
    const bool identity_view = controls().enabled && mode() >= DrawId;
    Shader* shader = identity_view && m_snapshot && m_snapshot->shader ? m_snapshot->shader.get()
                                                                       : m_shader.get();
    if (!shader || !shader->IsValid() || m_vao == 0)
        return;

    std::array<GLint, 2> polygon_mode{};
    if (identity_view) {
        glGetIntegerv(GL_POLYGON_MODE, polygon_mode.data());
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    // Diagnostic overlay: never depth-test or blend; we fully replace the bound target.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Near/far come from the frame camera (ctx), used only for Depth-mode linearization.
    // Fall back to the historic defaults if a context arrives without a camera.
    const float near_plane = ctx.camera ? ctx.camera->GetNearPlane() : 0.1f;
    const float far_plane = ctx.camera ? ctx.camera->GetFarPlane() : 4000.0f;

    shader->use();
    shader->setInt("u_farWorldLadder", ctx.far_world_ladder ? 1 : 0);
    shader->setMat4("u_gbufferInverseProjection", ctx.gbuffer_inverse_projection);
    shader->setInt("u_mode", mode());
    if (identity_view && m_snapshot && m_snapshot->shader) {
        shader->setInt("u_inspect", 0);
    }
    shader->setFloat("u_near", near_plane);
    shader->setFloat("u_far", far_plane);

    bind_sources(ctx, *shader);
    if (mode() >= DrawId && (!m_snapshot || !m_snapshot->ready ||
                             m_snapshot->targets.extent().width != ctx.internal_w() ||
                             m_snapshot->targets.extent().height != ctx.internal_h())) {
        shader->setInt("u_identityReady", 0);
    }

    glBindVertexArray(m_vao);
    Luminumbra::Rendering::AttributedGl::Submit(
        glDrawArrays, static_cast<GLenum>(GL_TRIANGLES), 0, 3);
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

    if (!m_snapshot || &shader != m_snapshot->shader.get()) {
        return;
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
