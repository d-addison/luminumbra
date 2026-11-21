#include "GBufferTargets.h"
#include "DiagnosticDrawTable.h"
#include "RenderResourceRegistry.h"
#include "Shader.h"
#include "core/Log.h"
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
GBufferDiagnosticCapture*& ActiveCapture() {
    static thread_local GBufferDiagnosticCapture* capture = nullptr;
    return capture;
}
} // namespace

GBufferDiagnosticCapture* GBufferDiagnosticCapture::Current() {
    return ActiveCapture();
}

void GBufferDiagnosticCapture::SetCurrent(GBufferDiagnosticCapture* capture) {
    ActiveCapture() = capture;
}

GBufferDiagnosticCapture::State::State(DiagnosticDrawTable& draw_table)
    : table(draw_table) {}

GBufferDiagnosticCapture::State::~State() = default;

GBufferDiagnosticCapture::GBufferDiagnosticCapture(DiagnosticDrawTable& table) {
    if (Current()) {
        throw std::logic_error("Diagnostic capture scopes cannot nest");
    }
    if (table.resolve(1)) {
        throw std::invalid_argument("Diagnostic capture requires a fresh draw table");
    }
    state_ = std::make_unique<State>(table);
    SetCurrent(this);
}

GBufferDiagnosticCapture::GBufferDiagnosticCapture(DiagnosticTargets& targets,
                                                   DiagnosticDrawTable& table,
                                                   DiagnosticTargets* forward_targets) {
    if (forward_targets == &targets) {
        throw std::invalid_argument("Forward identities require distinct storage");
    }
    if (Current()) {
        throw std::logic_error("Diagnostic capture scopes cannot nest");
    }
    if (table.resolve(1)) {
        throw std::invalid_argument("Diagnostic capture requires a fresh draw table");
    }
    state_ = std::make_unique<State>(table);
    state_->targets = &targets;
    state_->forward_targets = forward_targets;
    SetCurrent(this);
}

GBufferDiagnosticCapture::~GBufferDiagnosticCapture() {
    try {
        detach();
    } catch (...) {
        abort();
    }
    SetCurrent(nullptr);
}

bool GBufferDiagnosticCapture::attachment_failed() const noexcept {
    return state_->failed;
}

void GBufferDiagnosticCapture::abort() noexcept {
    state_->failed = true;
    state_->snapshot_failed = true;
}

void GBufferDiagnosticCapture::unsupported(std::string_view producer,
                                           std::uint32_t lod,
                                           DiagnosticOwnership ownership,
                                           DiagnosticPass pass) {
    state_->failed = true;
    if (state_->refused) {
        return;
    }
    DiagnosticId unused{};
    if (!state_->table.submit({.mesh_path = {},
                               .shader_variant = producer,
                               .chunk = {},
                               .lod = lod,
                               .ownership = ownership,
                               .pass = pass,
                               .provenance = DiagnosticProvenance::Unsupported},
                              unused)) {
        state_->refused = true;
    }
}

bool GBufferDiagnosticCapture::refused() const {
    return state_->refused;
}

bool GBufferDiagnosticCapture::complete() const {
    return state_->captured && !state_->framebuffer && !state_->attachment_lease &&
           !state_->failed && !state_->refused && state_->table.complete_coverage();
}

bool GBufferDiagnosticCapture::snapshot_ready() const {
    return state_->captured && !state_->framebuffer && !state_->attachment_lease &&
           !state_->snapshot_failed && !state_->refused;
}

Shader* GBufferDiagnosticCapture::variant(Shader* shader,
                                          bool ladder,
                                          const std::vector<std::string>& feedback_varyings,
                                          bool zero_ids) {
    if (!state_->framebuffer) {
        return shader;
    }
    if (!shader || !shader->IsValid()) {
        abort();
        unsupported("diagnostic_base_shader_unavailable",
                    0,
                    DiagnosticOwnership::Live,
                    state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer);
        return shader;
    }
    // At most six programs and three 64-byte feedback names per program.
    if (feedback_varyings.size() > 3 ||
        std::any_of(feedback_varyings.begin(), feedback_varyings.end(), [](const auto& name) {
            return name.size() > 64;
        })) {
        abort();
        unsupported("diagnostic_shader_variant_limit",
                    0,
                    DiagnosticOwnership::Live,
                    state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer);
        return shader;
    }
    for (const auto& entry : state_->variants) {
        if (entry.vertex == shader->VertexPath() && entry.fragment == shader->FragmentPath() &&
            entry.ladder == ladder && entry.zero_ids == zero_ids &&
            entry.feedback_varyings == feedback_varyings) {
            return entry.shader.get();
        }
    }
    if (state_->variants.size() == 6) {
        abort();
        unsupported("diagnostic_shader_variant_limit",
                    0,
                    DiagnosticOwnership::Live,
                    state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer);
        return shader;
    }
    std::vector<std::string> defines{zero_ids ? "LUMIN_DIAG_ZERO_IDS 1" : "LUMIN_DIAG_IDS 1"};
    if (ladder) {
        defines.emplace_back("LUMINUMBRA_FAR_WORLD_LADDER 1");
    }
    auto diagnostic = std::make_unique<Shader>(
        shader->VertexPath().c_str(), shader->FragmentPath().c_str(), feedback_varyings, defines);
    const GLint identity =
        diagnostic->IsValid() ? glGetUniformLocation(diagnostic->Id(), "u_diagnosticIds") : -1;
    if (!diagnostic->IsValid() || (!zero_ids && identity < 0)) {
        abort();
        unsupported("diagnostic_shader_variant_unavailable",
                    0,
                    DiagnosticOwnership::Live,
                    state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer);
        return shader;
    }
    state_->variants.push_back({shader->VertexPath(),
                                shader->FragmentPath(),
                                ladder,
                                zero_ids,
                                feedback_varyings,
                                std::move(diagnostic),
                                identity});
    return state_->variants.back().shader.get();
}

void GBufferDiagnosticCapture::write(const Shader& shader, const DiagnosticSubmission& submission) {
    if (!state_->framebuffer) {
        return;
    }
    const bool matching_pass =
        submission.pass == (state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer);
    if (!matching_pass) {
        unsupported("diagnostic_contributor_pass_mismatch",
                    submission.lod,
                    submission.ownership,
                    submission.pass);
    }
    for (const auto& entry : state_->variants) {
        if (entry.shader.get() != &shader) {
            continue;
        }
        if (entry.zero_ids) {
            // A zero-output program cannot certify a supported identity.
            abort();
            return;
        }
        DiagnosticId id{};
        if (submission.provenance == DiagnosticProvenance::Unsupported) {
            state_->failed = true;
        }
        if (matching_pass && !state_->refused && !state_->table.submit(submission, id)) {
            state_->refused = true;
            state_->failed = true;
        }
        // Refused draws carry no identity, never the previous draw's ID. Once
        // refused, do no more registry work or allocation for this capture.
        glProgramUniform4uiv(shader.Id(), entry.identity, 1, id.data());
        return;
    }
    unsupported(
        "diagnostic_writer_without_variant", submission.lod, submission.ownership, submission.pass);
    abort();
}

GBufferDiagnosticAttachment::GBufferDiagnosticAttachment(const GBuffer& gbuffer,
                                                         const DiagnosticExtent& extent)
    : capture_(GBufferDiagnosticCapture::Current()) {
    if (capture_) {
        exceptions_ = std::uncaught_exceptions();
    }
    if (capture_ && !capture_->attach(gbuffer, extent)) {
        capture_ = nullptr;
    }
}

GBufferDiagnosticAttachment::~GBufferDiagnosticAttachment() {
    if (capture_) {
        try {
            capture_->detach();
        } catch (...) {
            capture_->abort();
        }
        if (std::uncaught_exceptions() > exceptions_) {
            capture_->abort();
        }
    }
}

void GBufferDiagnosticCapture::write_current(const DiagnosticSubmission& submission) {
    const auto record_contributor = [&] {
        abort(); // A table entry alone cannot describe the blended pixel.
        if (state_->refused) {
            return;
        }
        constexpr std::string_view suffix = ":contributor_only";
        // Bound the temporary label before copying, including its coverage suffix.
        if (submission.shader_variant.empty() ||
            submission.shader_variant.size() > 1024 - suffix.size()) {
            unsupported("diagnostic_contributor_label_limit",
                        submission.lod,
                        submission.ownership,
                        submission.pass);
            return;
        }
        const std::string label = std::string(submission.shader_variant) + std::string(suffix);
        auto contributor = submission;
        contributor.shader_variant = label;
        DiagnosticId id{};
        if (!state_->table.submit(contributor, id)) {
            unsupported("diagnostic_contributor_identity_refused",
                        submission.lod,
                        submission.ownership,
                        submission.pass);
            state_->refused = true;
        }
    };
    if (!state_->framebuffer) {
        if (submission.pass == DiagnosticPass::Forward) {
            record_contributor();
        }
        return;
    }
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    for (const auto& entry : state_->variants) {
        if (entry.shader->Id() == static_cast<GLuint>(program)) {
            write(*entry.shader, submission);
            return;
        }
    }
    if (submission.pass == DiagnosticPass::Forward) {
        record_contributor();
        return;
    }
    unsupported(
        "diagnostic_writer_without_variant", submission.lod, submission.ownership, submission.pass);
    abort();
}

void GBufferDiagnosticCapture::clear_current_identity() {
    if (!state_->framebuffer) {
        return;
    }
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    for (const auto& entry : state_->variants) {
        if (entry.shader->Id() == static_cast<GLuint>(program)) {
            const DiagnosticId id{};
            glProgramUniform4uiv(entry.shader->Id(), entry.identity, 1, id.data());
            return;
        }
    }
}

bool GBufferDiagnosticCapture::deferred_attached() const {
    return state_->framebuffer != 0 && !state_->forward;
}

bool GBufferDiagnosticCapture::attach_forward(std::uint32_t framebuffer,
                                              std::uint32_t color_texture,
                                              const DiagnosticExtent& extent) {
    if (!state_->forward_targets) {
        // Original forward submissions still register exact contributor-only records.
        // Without pixel storage the frame remains ineligible for publication.
        abort();
        return false;
    }
    GBuffer target{};
    target.fbo_id = framebuffer;
    target.position_texture = color_texture;
    if (!attach_target(target, extent, true)) {
        abort();
        const auto reason = std::exchange(state_->failure, {});
        unsupported(reason, 0, DiagnosticOwnership::Live, DiagnosticPass::Forward);
        return false;
    }
    return true;
}

GBufferDiagnosticAttachment::GBufferDiagnosticAttachment(std::uint32_t framebuffer,
                                                         std::uint32_t color_texture,
                                                         const DiagnosticExtent& extent)
    : capture_(GBufferDiagnosticCapture::Current()) {
    if (capture_) {
        exceptions_ = std::uncaught_exceptions();
        if (!capture_->attach_forward(framebuffer, color_texture, extent)) {
            capture_ = nullptr;
        }
    }
}

bool GBufferDiagnosticCapture::attach(const GBuffer& gbuffer, const DiagnosticExtent& extent) {
    if (!attach_target(gbuffer, extent)) {
        abort();
        const auto reason = std::exchange(state_->failure, {});
        unsupported(reason);
        return false;
    }
    return true;
}

void GBufferDiagnosticCapture::detach() {
    detach_target();
    if (!state_->failure.empty()) {
        abort();
        const auto reason = std::exchange(state_->failure, {});
        unsupported(reason,
                    0,
                    DiagnosticOwnership::Live,
                    state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer);
    }
}

bool GBufferDiagnosticCapture::attach_target(const GBuffer& gbuffer,
                                             const DiagnosticExtent& extent,
                                             bool forward) {
    if (state_->framebuffer != 0 || (forward ? !state_->captured : state_->forward_captured)) {
        state_->failure = "nested_gbuffer_attachment";
        return false;
    }
    if (!state_->targets) {
        state_->failure = "diagnostic_target_storage_unavailable";
        return false;
    }
    if (!gbuffer.fbo_id || !gbuffer.position_texture ||
        glIsFramebuffer(gbuffer.fbo_id) != GL_TRUE) {
        state_->failure = "missing_gbuffer";
        return false;
    }
    GLint max_buffers = 0;
    GLint max_attachments = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_buffers);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max_attachments);
    if (max_buffers < 7 || max_buffers > static_cast<GLint>(state_->buffers.size()) ||
        max_attachments < 7) {
        state_->failure = "diagnostic_mrt_capability";
        return false;
    }
    GLint saved_texture = 0;
    GLint width = 0;
    GLint height = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture);
    glBindTexture(GL_TEXTURE_2D, gbuffer.position_texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_texture));
    if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) != extent.width ||
        static_cast<std::uint32_t>(height) != extent.height) {
        state_->failure = "diagnostic_gbuffer_extent_mismatch";
        return false;
    }
    GLint saved_framebuffer = 0;
    GLint attachment_type = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer.fbo_id);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT6,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                          &attachment_type);
    if (attachment_type != GL_NONE) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_framebuffer));
        state_->failure = "diagnostic_attachment_occupied";
        return false;
    }
    state_->buffer_count = max_buffers;
    for (GLsizei index = 0; index < max_buffers; ++index) {
        GLint buffer = 0;
        glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(index), &buffer);
        state_->buffers[static_cast<std::size_t>(index)] = static_cast<GLenum>(buffer);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_framebuffer));
    auto& targets = forward ? *state_->forward_targets : *state_->targets;
    const bool already_captured = forward ? state_->forward_captured : state_->captured;
    // Continuations keep earlier IDs, so resizing either plane mid-frame must refuse.
    if (already_captured &&
        (targets.extent().width != extent.width || targets.extent().height != extent.height)) {
        state_->failure = "diagnostic_continuation_extent_mismatch";
        return false;
    }
    if (!targets.ensure(true, extent, 7)) {
        state_->failure = "diagnostic_integer_target_unsupported";
        return false;
    }
    state_->attachment_lease = targets.acquire_attachment_lease();
    if (!state_->attachment_lease) {
        state_->failure = "diagnostic_attachment_lease_unavailable";
        return false;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer.fbo_id);
    glGetBooleani_v(GL_COLOR_WRITEMASK, 6, state_->color_mask.data());
    glColorMaski(6, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT6,
                           GL_TEXTURE_2D,
                           state_->attachment_lease.texture(),
                           0);
    const std::array<GLenum, 7> buffers{GL_COLOR_ATTACHMENT0,
                                        GL_COLOR_ATTACHMENT1,
                                        GL_COLOR_ATTACHMENT2,
                                        GL_COLOR_ATTACHMENT3,
                                        GL_COLOR_ATTACHMENT4,
                                        gbuffer.authored_texture ? GLenum{GL_COLOR_ATTACHMENT5}
                                                                 : GLenum{GL_NONE},
                                        GL_COLOR_ATTACHMENT6};
    if (forward || already_captured) {
        auto forward_buffers = state_->buffers;
        forward_buffers[6] = GL_COLOR_ATTACHMENT6;
        glDrawBuffers(state_->buffer_count, forward_buffers.data());
    } else {
        glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    }
    const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    state_->framebuffer = gbuffer.fbo_id;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_framebuffer));
    if (!complete) {
        detach_target();
        state_->failure = "diagnostic_mrt_incomplete";
        return false;
    }
    if (!already_captured) {
        glClearTexImage(
            state_->attachment_lease.texture(), 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
    }
    state_->forward = forward;
    if (forward) {
        state_->forward_captured = true;
    } else {
        state_->captured = true;
    }
    return true;
}

void GBufferDiagnosticCapture::detach_target() {
    if (!state_->framebuffer) {
        return;
    }
    GLint saved_framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_framebuffer);
    // Any registry path (destroy_owned, destroy_all_owned, replace_targets) can
    // retire the attached framebuffer, and GL may reuse its name. Restore the
    // draw-buffer state only on the object that still holds the leased attachment.
    bool attached = glIsFramebuffer(state_->framebuffer) == GL_TRUE;
    if (attached) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state_->framebuffer);
        GLint object_type = GL_NONE;
        GLint object_name = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                              &object_type);
        if (object_type == GL_TEXTURE) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                                  GL_COLOR_ATTACHMENT6,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                                  &object_name);
        }
        attached = object_type == GL_TEXTURE &&
                   static_cast<GLuint>(object_name) == state_->attachment_lease.texture();
        if (attached) {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
            glDrawBuffers(state_->buffer_count, state_->buffers.data());
        }
    }
    // The color mask is context state, so it is restored even after retirement.
    glColorMaski(6,
                 state_->color_mask[0],
                 state_->color_mask[1],
                 state_->color_mask[2],
                 state_->color_mask[3]);
    state_->framebuffer = 0;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_framebuffer));
    if (!attached) {
        // The pass drew into a retired or foreign framebuffer: never publish it.
        state_->failed = true;
        state_->failure = "gbuffer_retired_during_capture";
        state_->snapshot_failed = true;
    }
    // Keep storage protected until detachment and caller-state restoration finish.
    state_->attachment_lease.reset();
}

void GBufferDiagnosticCapture::before_destroy(const GBuffer& gbuffer) {
    if (state_->framebuffer && state_->framebuffer == gbuffer.fbo_id) {
        detach_target();
        state_->failed = true;
        state_->snapshot_failed = true;
        if (state_->failure.empty()) {
            state_->failure = "gbuffer_recreated_during_capture";
        }
    }
}

void CreateGBufferTargets(
    GBuffer& gbuffer, RenderResourceRegistry& registry, u32 width, u32 height, bool authored) {
    // allocate the FBO + attachments THROUGH the registry.
    // Every desc reproduces the exact GL parameters of the retired
    // glTexImage/glTexParameter calls so the migration is byte-neutral
    // (flip-score-0 on same-pose captures); the struct caches the owned ids.
    const auto color_desc = [&](u32 internal_format, u32 format, u32 type, const char* label) {
        TextureDesc d;
        d.width = width;
        d.height = height;
        d.internal_format = internal_format;
        d.format = format;
        d.type = type;
        d.min_filter = GL_NEAREST;
        d.mag_filter = GL_NEAREST;
        d.expected_layout = "color_attachment";
        d.debug_label = label;
        return d;
    };

    // Authored views support distances beyond binary16's finite 65504 limit.
    // Retain the existing world profile while storing authored positions in full precision.
    gbuffer.position_texture =
        registry
            .create_texture(
                "gbuffer_position",
                color_desc(authored ? GL_RGB32F : GL_RGB16F, GL_RGB, GL_FLOAT, "gbuffer.position"))
            .id;
    // Normal/Material: RGBA8 (octahedral normal + material ID).
    gbuffer.normal_texture =
        registry
            .create_texture(
                "gbuffer_normal",
                color_desc(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "gbuffer.normal_material"))
            .id;
    // Albedo/Roughness: RGBA8.
    gbuffer.albedo_texture =
        registry
            .create_texture(
                "gbuffer_albedo",
                color_desc(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "gbuffer.albedo_roughness"))
            .id;
    // Metallic/AO: RG16F.
    gbuffer.material_texture =
        registry
            .create_texture("gbuffer_material",
                            color_desc(GL_RG16F, GL_RG, GL_FLOAT, "gbuffer.metallic_ao"))
            .id;
    // Motion vectors: RG16F (signed NDC delta).   TAAU foundation.
    gbuffer.motion_vector_texture =
        registry
            .create_texture("gbuffer_motion",
                            color_desc(GL_RG16F, GL_RG, GL_FLOAT, "gbuffer.motion_vectors"))
            .id;

    // Depth: DEPTH_COMPONENT32F, reversed-Z, clamp-to-border with cleared sky depth.
    TextureDesc depth_desc =
        color_desc(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, "gbuffer.depth");
    depth_desc.wrap_s = GL_CLAMP_TO_BORDER;
    depth_desc.wrap_t = GL_CLAMP_TO_BORDER;
    depth_desc.has_border_color = true;
    depth_desc.border_color[0] = 0.0f;
    depth_desc.border_color[1] = 0.0f;
    depth_desc.border_color[2] = 0.0f;
    depth_desc.border_color[3] = 0.0f;
    depth_desc.expected_layout = "depth_attachment";
    gbuffer.depth_texture = registry.create_texture("gbuffer_depth", depth_desc).id;

    FboDesc fbo_desc;
    fbo_desc.attachments = {
        {GL_COLOR_ATTACHMENT0, "gbuffer_position"},
        {GL_COLOR_ATTACHMENT1, "gbuffer_normal"},
        {GL_COLOR_ATTACHMENT2, "gbuffer_albedo"},
        {GL_COLOR_ATTACHMENT3, "gbuffer_material"},
        {GL_COLOR_ATTACHMENT4, "gbuffer_motion"},
        {GL_DEPTH_ATTACHMENT, "gbuffer_depth"},
    };
    fbo_desc.draw_buffers = {GL_COLOR_ATTACHMENT0,
                             GL_COLOR_ATTACHMENT1,
                             GL_COLOR_ATTACHMENT2,
                             GL_COLOR_ATTACHMENT3,
                             GL_COLOR_ATTACHMENT4};
    if (authored) {
        gbuffer.authored_texture =
            registry
                .create_texture(
                    "gbuffer_authored",
                    color_desc(GL_RGBA16F, GL_RGBA, GL_FLOAT, "gbuffer.authored_emission"))
                .id;
        fbo_desc.attachments.push_back({GL_COLOR_ATTACHMENT5, "gbuffer_authored"});
        fbo_desc.draw_buffers.push_back(GL_COLOR_ATTACHMENT5);
    }
    fbo_desc.debug_label = "gbuffer.fbo";
    gbuffer.fbo_id = registry.create_fbo("gbuffer_fbo", fbo_desc).id;
    if (gbuffer.fbo_id == 0) {
        LUMINUMBRA_CORE_ERROR("G-Buffer FBO not complete!");
    }
}

void DestroyGBufferTargets(GBuffer& gbuffer, RenderResourceRegistry& registry) {
    if (auto* capture = GBufferDiagnosticCapture::Current()) {
        capture->before_destroy(gbuffer);
    }
    // Ownership contract: the registry deletes the owned GL objects.
    registry.destroy_owned("gbuffer_fbo");
    registry.destroy_owned("gbuffer_position");
    registry.destroy_owned("gbuffer_normal");
    registry.destroy_owned("gbuffer_albedo");
    registry.destroy_owned("gbuffer_material");
    registry.destroy_owned("gbuffer_motion");
    registry.destroy_owned("gbuffer_depth");
    registry.destroy_owned("gbuffer_authored");
    gbuffer = GBuffer{};
}

} // namespace Luminumbra::Rendering
