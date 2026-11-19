#include "core/IsolationConfig.h"
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "rendering/Camera.h"
#include "rendering/DiagnosticDrawTable.h"
#include "rendering/FarLodSystem.h"
#include "rendering/GBufferTargets.h"
#include "rendering/Mesh.h"
#include "rendering/RenderResourceRegistry.h"
#include "rendering/Shader.h"
#include "rendering/passes/GBufferPass.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <gtest/gtest.h>
#include <luminumbra/rendering/RenderView.h>
#include <luminumbra/rendering/StaticScene.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::string Read(std::string_view path) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / path);
    EXPECT_TRUE(input.good()) << path;
    std::ostringstream result;
    result << input.rdbuf();
    return result.str();
}

void Contains(const std::string& source, std::string_view text) {
    EXPECT_NE(source.find(text), std::string::npos) << text;
}

void Before(const std::string& source, std::string_view first, std::string_view second) {
    ASSERT_NE(source.find(first), std::string::npos) << first;
    ASSERT_NE(source.find(second), std::string::npos) << second;
    EXPECT_LT(source.find(first), source.find(second));
}

std::size_t Count(const std::string& source, std::string_view text) {
    std::size_t result = 0;
    for (auto position = source.find(text); position != std::string::npos;
         position = source.find(text, position + text.size())) {
        ++result;
    }
    return result;
}

std::string Body(const std::string& source, std::string_view signature) {
    const auto signature_position = source.find(signature);
    if (signature_position == std::string::npos) {
        return {};
    }
    const auto start = source.find('{', signature_position);
    if (start == std::string::npos) {
        return {};
    }
    int depth = 0;
    for (auto position = start; position < source.size(); ++position) {
        if (source[position] == '{') {
            ++depth;
        } else if (source[position] == '}' && --depth == 0) {
            return source.substr(start, position - start + 1);
        }
    }
    return {};
}

std::string WithoutDiagnosticBlocks(std::string source) {
    constexpr std::string_view directive = "#ifdef LUMIN_DIAG_IDS\n";
    EXPECT_EQ(Count(source, directive), 2u);
    for (int block = 0; block < 2; ++block) {
        auto start = source.find(directive);
        if (start == std::string::npos) {
            return {};
        }
        const auto end = source.find("#endif\n", start);
        if (end == std::string::npos) {
            return {};
        }
        const auto length = end + std::string_view("#endif\n").size() - start;
        // The declaration block also introduces one blank separator line.
        if (start >= 2 && source.substr(start - 2, 2) == "\n\n") {
            source.erase(start - 1, length + 1);
        } else {
            source.erase(start, length);
        }
    }
    return source;
}

std::uint64_t SourceHash(const std::string& source) {
    std::uint64_t value = 14695981039346656037ull;
    for (const char character : source) {
        value = (value ^ static_cast<unsigned char>(character)) * 1099511628211ull;
    }
    return value;
}

const std::string kTargets = "src/luminumbra_client/rendering/GBufferTargets.cpp";
const std::string kPass = "src/luminumbra_client/rendering/passes/GBufferPass.cpp";
template<typename Submission>
bool SubmitGenerated(Luminumbra::Rendering::DiagnosticDrawTable& table,
                     Submission submission,
                     std::string_view key,
                     Luminumbra::Rendering::DiagnosticId& id) {
    if constexpr (requires { submission.generated_mesh; }) {
        submission.generated_mesh = key;
        return table.submit(submission, id);
    }
    return false;
}
} // namespace

TEST(DiagnosticAttribution, DefaultShaderTextIsUnchangedWithoutTheVariant) {
    // Re-pinned for #872's interpolated-normal slope-rock coverage (#1045).
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/g_buffer.frag"))),
              0xb68526b1edfc1436ull);
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/foliage.frag"))),
              0xd5876c01db56f7ecull);
    const auto create = Body((Read(kTargets) + Read(kPass)), "void CreateGBufferTargets(");
    Contains(create, "GL_COLOR_ATTACHMENT4");
    Contains(create, "if (authored)");
    EXPECT_EQ(create.find("GL_COLOR_ATTACHMENT6"), std::string::npos);
    EXPECT_EQ(create.find("Diagnostic"), std::string::npos);
}

TEST(DiagnosticAttribution, DisabledScopeDoesNotAllocateTargetsOrCompileVariants) {
    const auto producers = Read(kTargets) + Read(kPass);
    Contains(producers, "static thread_local GBufferDiagnosticCapture* capture = nullptr;");
    const auto constructor = Body(producers, "GBufferDiagnosticCapture::GBufferDiagnosticCapture(");
    Contains(constructor, "SetCurrent(this);");
    EXPECT_EQ(constructor.find("gl"), std::string::npos);
    EXPECT_EQ(constructor.find("ensure("), std::string::npos);
    EXPECT_EQ(constructor.find("make_unique<Shader>"), std::string::npos);
    const auto variant = Body(producers, "Shader* GBufferDiagnosticCapture::variant(");
    Contains(variant, "if (!shader || !shader->IsValid())");
    Contains(variant, "LUMIN_DIAG_IDS 1");
    Contains(variant, "LUMINUMBRA_FAR_WORLD_LADDER 1");
    EXPECT_LT(variant.find("if (!shader"), variant.find("make_unique<Shader>"));
    EXPECT_EQ(Count(producers, "if (capture) {\n        shader = capture->variant("), 3u);
    Contains(Body(producers, "GBufferDiagnosticCapture::~GBufferDiagnosticCapture()"),
             "SetCurrent(nullptr);");
}

TEST(DiagnosticAttribution, BothFragmentWritersUseTheExistingDiscardAndDepth) {
    for (const auto* path : {"res/shaders/g_buffer.frag", "res/shaders/foliage.frag"}) {
        const auto source = Read(path);
        Contains(source,
                 "#ifdef LUMIN_DIAG_IDS\nlayout (location = 6) out uvec4 gDiagnosticIds;\n"
                 "uniform uvec4 u_diagnosticIds;\n#endif");
        Contains(source, "#ifdef LUMIN_DIAG_IDS\n    gDiagnosticIds = u_diagnosticIds;\n#endif");
        const auto main = Body(source, "void main()");
        const auto write = main.find("gDiagnosticIds = u_diagnosticIds;");
        ASSERT_NE(write, std::string::npos);
        ASSERT_NE(main.rfind("discard;"), std::string::npos);
        EXPECT_LT(main.rfind("discard;"), write);
        EXPECT_EQ(main.find("return;"), std::string::npos);
        EXPECT_EQ(Count(main, "gDiagnosticIds ="), 1u);
    }
    const auto source = Read("res/shaders/g_buffer.frag");
    EXPECT_LT(source.rfind("gl_FragDepth ="), source.find("gDiagnosticIds ="));
    Contains(source, "gl_FragDepth = biasedDepth;");
    Contains(source, "u_alphaTest == 3 && baseColor.a < 0.5");
}

TEST(DiagnosticAttribution, DeferredVertexSiblingsSelectVariantsAndUploadIntegerIdentities) {
    const auto pass = Read(kPass);
    EXPECT_EQ(Count(pass, "shader = capture->variant("), 3u);
    EXPECT_EQ(Count(pass, "diagnostic_uniforms.Cache(*shader)"), 3u);
    Contains(pass, "? \"instanced_static_mesh_ladder\"");
    Contains(pass, "? \"skinned_mesh_ladder\"");
    Contains(pass, "std::numeric_limits<GLsizei>::max()");
    Contains(pass, "diagnostic_static_mesh_index_count");
    ASSERT_NE(pass.find("glDrawElementsInstancedBaseInstance"), std::string::npos);
    EXPECT_LT(pass.find("diagnostic_static_mesh_index_count"),
              pass.find("glDrawElementsInstancedBaseInstance"));
    const auto write =
        Body((Read(kTargets) + Read(kPass)), "void GBufferDiagnosticCapture::write(");
    Contains(write, "state_->table.submit(submission, id)");
    Contains(write, "glProgramUniform4uiv(shader.Id(), entry.identity, 1, id.data())");
    EXPECT_LT(write.find("state_->table.submit"), write.find("glProgramUniform4uiv"));
    Contains(write, "state_->refused = true;");
    Contains(write, "!state_->refused && !state_->table.submit");
    EXPECT_EQ(write.find("throw"), std::string::npos);
    Contains(write, "diagnostic_writer_without_variant");
    EXPECT_EQ(Read(kPass).find("state_->table.submit"), std::string::npos);
    EXPECT_EQ(Read(kPass).find("GBufferDiagnosticCapture::write("), std::string::npos);
}

TEST(DiagnosticAttribution, RefusalNeverThrowsAndEveryPassExitRestoresCallerBindings) {
    const auto source = Read(kTargets) + Read(kPass);
    const auto unsupported = Body(source, "void GBufferDiagnosticCapture::unsupported(");
    Contains(unsupported, "state_->refused = true;");
    Contains(unsupported, "if (state_->refused)");
    EXPECT_EQ(unsupported.find("throw"), std::string::npos);
    const auto pass = Body(source, "GBufferDrawStats GBufferPass::execute(");
    Contains(pass, "glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vao)");
    Contains(pass, "glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo)");
    Contains(pass, "glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo)");
    Contains(pass, "ReplayRollback restore_gl([=, this]() noexcept");
    const auto restore = Body(pass, "ReplayRollback restore_gl(");
    Contains(restore, "glBindVertexArray(static_cast<GLuint>(previous_vao))");
    Contains(restore, "static_cast<GLuint>(previous_draw_fbo) == previous_gbuffer_fbo");
    Contains(restore, "static_cast<GLuint>(previous_read_fbo) == previous_gbuffer_fbo");
    EXPECT_EQ(Count(restore, "? m_gbuffer.fbo_id"), 2u);
    Contains(restore, "glEnable(GL_CULL_FACE)");
    Contains(restore, "glDisable(GL_CULL_FACE)");
    EXPECT_LT(pass.find("ReplayRollback restore_gl("),
              pass.find("glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id)"));
}

TEST(DiagnosticAttribution, InactivePassGuardsAllSnapshotAndRestoreWork) {
    const auto pass = Body(Read(kPass), "GBufferDrawStats GBufferPass::execute(");
    Contains(pass,
             "const bool diagnostic_capture = GBufferDiagnosticCapture::Current() != nullptr;");
    const auto snapshot = Body(pass, "if (diagnostic_capture)");
    EXPECT_EQ(Count(snapshot, "glGetIntegerv("), 3u);
    EXPECT_EQ(Count(snapshot, "glIsEnabled("), 1u);
    EXPECT_EQ(Count(pass, "glGetIntegerv("), 3u);
    EXPECT_EQ(Count(pass, "glIsEnabled("), 1u);
    const auto restore = Body(pass, "ReplayRollback restore_gl(");
    Contains(restore, "if (!diagnostic_capture) {\n            return;\n        }");
    EXPECT_LT(restore.find("return;"), restore.find("glBindVertexArray("));
    EXPECT_LT(restore.find("return;"), restore.find("glBindFramebuffer("));
    Contains(pass,
             "if (!diagnostic_capture) {\n        glBindFramebuffer(GL_FRAMEBUFFER, 0);\n    }");
}

TEST(DiagnosticAttribution, StaticInstancesSubmitOnceEachWithTheResolvedLod) {
    const auto pass = Body(Read(kPass), "void GBufferPass::geometry_pass_static_meshes(");
    Contains(pass, "batch.drawPath == LodMeshPath(batch.basePath, level)");
    Contains(pass, "for (GLsizei instance = 0; instance < instance_count; ++instance)");
    Contains(pass, ".lod = resolved_lod");
    Contains(pass, "glDrawElementsInstancedBaseInstance");
    Contains(pass, "static_cast<GLuint>(instance)");
    Contains(pass, "batch.basePath.size() > 1024 || batch.drawPath.size() > 1024");
    Contains(pass, "static_mesh_identity_path_limit");
    // An unrecordable identity path still draws; only the index-count refusal skips.
    Contains(pass, "!identity_path_unrecordable && level < kTreeLodCount");
    EXPECT_EQ(pass.find("capture->unsupported(\"static_mesh_identity_path_limit\")"),
              std::string::npos);
    Contains(pass,
             "} else {\n                "
             "Luminumbra::Rendering::AttributedGl::Submit(glDrawElementsInstanced,");
    EXPECT_LT(pass.find("capture->write("), pass.find("glDrawElementsInstancedBaseInstance"));
    const auto skinned = Body(Read(kPass), "void GBufferPass::geometry_pass_skinned_meshes(");
    Contains(skinned, ".mesh_path = mesh_info.meshPath");
    EXPECT_LT(skinned.find("capture->write("), skinned.find("glDrawElements,"));
}

TEST(DiagnosticAttribution, BothLegacyFarWritersCarryTheActualTierAndOwner) {
    const auto far = Body(Read("src/luminumbra_client/rendering/FarLodSystem.cpp"),
                          "void FarLodSystem::draw_gbuffer(");
    EXPECT_EQ(Count(far, ".lod = static_cast<std::uint32_t>(region.tier)"), 2u);
    EXPECT_EQ(Count(far, ".ownership = DiagnosticOwnership::LegacyFar"), 2u);
    Contains(far, "legacy_far_terrain");
    Contains(far, "legacy_far_water");
    using namespace Luminumbra::Rendering;
    DiagnosticDrawTable table;
    for (const std::uint32_t lod : {1u, 2u}) {
        DiagnosticId id{};
        ASSERT_TRUE(table.submit({.mesh_path = {},
                                  .shader_variant = "legacy_far_terrain",
                                  .chunk = {},
                                  .lod = lod,
                                  .ownership = DiagnosticOwnership::LegacyFar,
                                  .pass = DiagnosticPass::GBuffer,
                                  .provenance = DiagnosticProvenance::Unsupported},
                                 id));
        EXPECT_EQ(id[2], lod);
        EXPECT_EQ(id[3], 258u);
    }
    EXPECT_FALSE(table.complete_coverage());
}

TEST(DiagnosticAttribution, CapabilityRefusalIsAnExplicitUnsupportedRecord) {
    const auto targets = (Read(kTargets) + Read(kPass));
    const auto attach = Body(targets, "bool GBufferDiagnosticCapture::attach_target(");
    Contains(attach, "state_->framebuffer != 0 ||");
    Contains(attach, "forward ? !state_->captured || state_->forward_captured : state_->captured");
    Contains(attach, "nested_gbuffer_attachment");
    Contains(attach, "!gbuffer.fbo_id || !gbuffer.position_texture");
    Contains(attach, "max_buffers < 7");
    Contains(attach, "max_buffers > static_cast<GLint>(state_->buffers.size())");
    Contains(attach, "max_attachments < 7");
    Contains(attach, "diagnostic_mrt_capability");
    Contains(attach, "diagnostic_attachment_lease_unavailable");
    Contains(attach, "diagnostic_integer_target_unsupported");
    Contains(attach, "diagnostic_attachment_occupied");
    Contains(attach, "diagnostic_mrt_incomplete");
    Contains(Body(targets, "bool GBufferDiagnosticCapture::attach("), "unsupported(reason)");
    EXPECT_LT(attach.find("attachment_type != GL_NONE"), attach.find("targets.ensure("));
    const auto refusal = Body(targets, "void GBufferDiagnosticCapture::unsupported(");
    Contains(refusal, "state_->failed = true");
    Contains(refusal, ".provenance = DiagnosticProvenance::Unsupported");
    Contains(refusal, "state_->table.submit(");
    Contains(refusal, "state_->refused = true;");
    EXPECT_EQ(refusal.find("throw"), std::string::npos);
    const auto complete = Body(targets, "bool GBufferDiagnosticCapture::complete()");
    Contains(complete, "state_->captured && !state_->framebuffer");
    Contains(complete, "!state_->attachment_lease");
    Contains(complete, "!state_->failed");
    Contains(complete, "!state_->refused");
    Contains(complete, "state_->table.complete_coverage()");
}

TEST(DiagnosticAttribution, ResizeAndIncompleteAttachmentRestoreTheEntireDrawBufferState) {
    const auto targets = (Read(kTargets) + Read(kPass));
    const auto attach = Body(targets, "bool GBufferDiagnosticCapture::attach_target(");
    Contains(attach, "width <= 0 || height <= 0");
    Contains(attach, "static_cast<std::uint32_t>(width) != extent.width");
    Contains(attach, "static_cast<std::uint32_t>(height) != extent.height");
    Contains(attach, "diagnostic_gbuffer_extent_mismatch");
    Contains(attach, "targets.ensure(true, extent, 7)");
    Contains(attach, "GL_DRAW_BUFFER0 + static_cast<GLenum>(index)");
    Contains(attach, "glGetBooleani_v(GL_COLOR_WRITEMASK, 6, state_->color_mask.data())");
    Contains(attach, "glColorMaski(6, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE)");
    Contains(attach, "if (!complete) {\n        detach_target();");
    EXPECT_LT(attach.find("GL_DRAW_BUFFER0"), attach.find("glFramebufferTexture2D("));
    const auto detach = Body(targets, "void GBufferDiagnosticCapture::detach_target()");
    Contains(detach, "GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0");
    Contains(detach, "glDrawBuffers(state_->buffer_count, state_->buffers.data())");
    Contains(detach, "glColorMaski(6,");
    Contains(detach, "static_cast<GLuint>(saved_framebuffer)");
    const auto retire = Body(targets, "void GBufferDiagnosticCapture::before_destroy(");
    Contains(retire, "state_->framebuffer == gbuffer.fbo_id");
    EXPECT_LT(retire.find("detach_target();"), retire.find("gbuffer_recreated_during_capture"));
    const auto destroy = Body(targets, "void DestroyGBufferTargets(");
    EXPECT_LT(destroy.find("capture->before_destroy(gbuffer)"),
              destroy.find("registry.destroy_owned"));
}

TEST(DiagnosticAttribution, MissingProducerMetadataCannotClaimCompleteCoverage) {
    const auto pass = Read(kPass);
    Contains(pass, "live_terrain_mdi_unsupported");
    Contains(pass, "if (capture && ts.draws != 0)");
    Contains(pass, "ladder_backend_identity_unavailable");
    Contains(pass, "authored_static_surface");
    Contains(pass, "tree_impostor_fragment_output_unavailable\", 3");
    Contains(pass, "DiagnosticProvenance::Unsupported");
    const auto targets = (Read(kTargets) + Read(kPass));
    Contains(targets, "Diagnostic capture scopes cannot nest");
    Contains(targets, "Diagnostic capture requires a fresh draw table");
    Contains(targets, "diagnostic_shader_variant_limit");
    Contains(targets, "diagnostic_shader_variant_unavailable");
    Contains(Body(targets, "void GBufferDiagnosticCapture::unsupported("),
             ".provenance = DiagnosticProvenance::Unsupported");
    Contains(Body(targets, "bool GBufferDiagnosticCapture::complete()"),
             "state_->table.complete_coverage()");
}

TEST(DiagnosticAttribution, DeferredAttachmentScopeEndsBeforeCallerStateRestoration) {
    const auto pass = Read(kPass);
    const auto execute = Body(pass, "GBufferDrawStats GBufferPass::execute(");
    Before(execute,
           "ReplayRollback restore_gl(",
           "GBufferDiagnosticAttachment diagnostic_attachment(");
    Before(execute,
           "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)",
           "GBufferDiagnosticAttachment diagnostic_attachment(");
    Before(execute, "GBufferDiagnosticAttachment diagnostic_attachment(", "geometry_pass_chunks(");
    Before(execute,
           "GBufferDiagnosticAttachment diagnostic_attachment(",
           "geometry_pass_skinned_meshes(");
    EXPECT_EQ(pass.find("glFramebufferTexture"), std::string::npos);
    EXPECT_EQ(Read("src/luminumbra_client/rendering/static_preview/StaticRenderer.cpp")
                  .find("GBufferDiagnosticAttachment"),
              std::string::npos);
    EXPECT_EQ(SourceHash(Body(Read(kTargets), "void CreateGBufferTargets(")),
              0x87b070d8d24c6b19ull);
}

TEST(DiagnosticAttribution, AttachmentStorageIsEnsuredBeforeLeaseAndLeaseBeforeAttachment) {
    const auto header = Read("src/luminumbra_client/rendering/GBufferTargets.h");
    Contains(header, "DiagnosticTargets::AttachmentLease attachment_lease;");
    const auto attach = Body(Read(kTargets), "bool GBufferDiagnosticCapture::attach_target(");
    Before(attach,
           "targets.ensure(true, extent, 7)",
           "state_->attachment_lease = targets.acquire_attachment_lease()");
    Before(attach,
           "state_->attachment_lease = targets.acquire_attachment_lease()",
           "if (!state_->attachment_lease)");
    Before(attach, "if (!state_->attachment_lease)", "glFramebufferTexture2D(");
    Contains(attach, "state_->attachment_lease.texture()");
    Contains(attach, "diagnostic_attachment_lease_unavailable");
    EXPECT_EQ(attach.find("attachment_lease.reset()"), std::string::npos);
}

TEST(DiagnosticAttribution, LeaseProtectsAttachmentThroughRollbackAndCallerRestoration) {
    const auto targets = Read(kTargets);
    const auto attach = Body(targets, "bool GBufferDiagnosticCapture::attach_target(");
    Before(attach, "targets.ensure(true, extent, 7)", "targets.acquire_attachment_lease()");
    Before(attach, "if (!state_->attachment_lease)", "glFramebufferTexture2D(");
    Before(Body(attach, "if (!complete)"), "detach_target();", "diagnostic_mrt_incomplete");
    const auto detach = Body(targets, "void GBufferDiagnosticCapture::detach_target()");
    Before(detach, "GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0", "attachment_lease.reset()");
    Before(detach,
           "glDrawBuffers(state_->buffer_count, state_->buffers.data())",
           "attachment_lease.reset()");
    Before(detach, "static_cast<GLuint>(saved_framebuffer)", "attachment_lease.reset()");
    EXPECT_EQ(Count(targets, "attachment_lease.reset()"), 1u);
}

TEST(DiagnosticAttribution, RetirementChecksLiveObjectAndAttachmentBeforeRestoringDrawBuffers) {
    const auto detach = Body(Read(kTargets), "void GBufferDiagnosticCapture::detach_target()");
    Before(detach,
           "glIsFramebuffer(state_->framebuffer)",
           "glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state_->framebuffer)");
    Before(detach,
           "GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME",
           "GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0");
    Contains(detach, "static_cast<GLuint>(object_name) == state_->attachment_lease.texture()");
    const auto live = Body(detach, "if (attached)");
    const auto owned = Body(live, "if (attached)");
    Contains(owned, "glDrawBuffers(state_->buffer_count, state_->buffers.data())");
    EXPECT_EQ(Count(detach, "glDrawBuffers("), 1u);
    const auto retired = Body(detach, "if (!attached)");
    Contains(retired, "state_->failed = true;");
    Contains(retired, "gbuffer_retired_during_capture");
    Before(detach, "glColorMaski(6,", "attachment_lease.reset()");
}

TEST(DiagnosticAttribution, RecreationAndBothDestructorsDetachBeforeRetiringStorage) {
    const auto source = Read(kTargets);
    Before(Body(source, "void DestroyGBufferTargets("),
           "capture->before_destroy(gbuffer)",
           "registry.destroy_owned(");
    Before(Body(source, "void GBufferDiagnosticCapture::before_destroy("),
           "detach_target();",
           "gbuffer_recreated_during_capture");
    Contains(Body(source, "void GBufferDiagnosticCapture::before_destroy("),
             "if (state_->failure.empty())");
    Before(Body(source, "GBufferDiagnosticCapture::~GBufferDiagnosticCapture()"),
           "detach();",
           "SetCurrent(nullptr)");
    const auto scope = Body(source, "GBufferDiagnosticAttachment::~GBufferDiagnosticAttachment()");
    Before(scope, "capture_->detach();", "std::uncaught_exceptions() > exceptions_");
    Contains(scope, "capture_->abort();");
    Before(Body(source, "void GBufferDiagnosticCapture::detach()"),
           "detach_target();",
           "unsupported(reason,");
}

namespace {
using namespace Luminumbra::Rendering;

// Use the existing GLAD entry-point hook pattern to observe the real pass.
// Forward every call to the context and restore each entry before leaving scope.
class PassStateProbe {
public:
    explicit PassStateProbe(GLuint retired_fbo = 0)
        : retired_fbo_(retired_fbo) {
        Active() = this;
        glGetIntegerv = GetInteger;
        glIsEnabled = IsEnabled;
        glBindVertexArray = BindVertexArray;
        glBindFramebuffer = BindFramebuffer;
        glGenFramebuffers = GenFramebuffers;
    }
    ~PassStateProbe() {
        glGetIntegerv = previous_get_integer_;
        glIsEnabled = previous_is_enabled_;
        glBindVertexArray = previous_bind_vao_;
        glBindFramebuffer = previous_bind_fbo_;
        glGenFramebuffers = previous_gen_fbos_;
        Active() = nullptr;
    }
    PassStateProbe(const PassStateProbe&) = delete;
    PassStateProbe& operator=(const PassStateProbe&) = delete;

    unsigned integer_queries = 0, enabled_queries = 0, vao_binds = 0;
    unsigned combined_fbo_binds = 0, draw_fbo_binds = 0, read_fbo_binds = 0;

private:
    static void APIENTRY GenFramebuffers(GLsizei count, GLuint* framebuffers) {
        auto& probe = *Active();
        probe.previous_gen_fbos_(count, framebuffers);
        // Force recreation to use a different name even on drivers that reuse
        // the just-deleted name. All allocations and deletions reach real GL.
        for (GLsizei index = 0; index < count; ++index) {
            if (framebuffers[index] == probe.retired_fbo_ && probe.retired_fbo_ != 0) {
                GLuint replacement = 0;
                probe.previous_gen_fbos_(1, &replacement);
                glDeleteFramebuffers(1, &framebuffers[index]);
                framebuffers[index] = replacement;
            }
        }
    }
    static PassStateProbe*& Active() {
        static PassStateProbe* probe = nullptr;
        return probe;
    }
    static void APIENTRY GetInteger(GLenum name, GLint* value) {
        auto& probe = *Active();
        ++probe.integer_queries;
        probe.previous_get_integer_(name, value);
    }
    static GLboolean APIENTRY IsEnabled(GLenum name) {
        auto& probe = *Active();
        ++probe.enabled_queries;
        return probe.previous_is_enabled_(name);
    }
    static void APIENTRY BindVertexArray(GLuint vao) {
        auto& probe = *Active();
        ++probe.vao_binds;
        probe.previous_bind_vao_(vao);
    }
    static void APIENTRY BindFramebuffer(GLenum target, GLuint fbo) {
        auto& probe = *Active();
        if (target == GL_FRAMEBUFFER) {
            ++probe.combined_fbo_binds;
        } else if (target == GL_DRAW_FRAMEBUFFER) {
            ++probe.draw_fbo_binds;
        } else if (target == GL_READ_FRAMEBUFFER) {
            ++probe.read_fbo_binds;
        }
        probe.previous_bind_fbo_(target, fbo);
    }
    decltype(glGetIntegerv) previous_get_integer_ = glGetIntegerv;
    decltype(glIsEnabled) previous_is_enabled_ = glIsEnabled;
    decltype(glBindVertexArray) previous_bind_vao_ = glBindVertexArray;
    decltype(glBindFramebuffer) previous_bind_fbo_ = glBindFramebuffer;
    decltype(glGenFramebuffers) previous_gen_fbos_ = glGenFramebuffers;
    GLuint retired_fbo_ = 0;
};

class AttributionContext {
public:
    AttributionContext() {
        if (glfwInit()) {
            initialized_ = true;
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            window_ = glfwCreateWindow(32, 32, "diagnostic attribution", nullptr, nullptr);
            if (window_) {
                glfwMakeContextCurrent(window_);
                ready_ = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
            }
        }
    }
    ~AttributionContext() {
        if (window_) {
            glfwDestroyWindow(window_);
        }
        if (initialized_) {
            glfwTerminate();
        }
    }
    bool ready() const {
        return ready_;
    }

private:
    GLFWwindow* window_ = nullptr;
    bool initialized_ = false;
    bool ready_ = false;
};

class DiagnosticAttributionGl : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(gl_.ready());
        const std::filesystem::path root(LUMINUMBRA_SOURCE_ROOT);
        pass_.init_geometry_shader(root);
        pass_.init_instanced_static_mesh(root);
        pass_.init_skinned_mesh(root);
        pass_.init_gbuffer(resources_, 32, 32);
        ASSERT_TRUE(pass_.geometry_shader()->IsValid());
        ASSERT_TRUE(pass_.instanced_static_mesh_shader()->IsValid());
        ASSERT_TRUE(pass_.skinned_mesh_shader()->IsValid());
        ASSERT_TRUE(targets_.ensure(true, {32, 32}, 7));
        ctx_.camera = &camera_;
        ctx_.screen_width = 32;
        ctx_.screen_height = 32;
        ctx_.isolation = &isolation_;
        ctx_.frustum_planes = planes_;
        ctx_.registry = &resources_;
        input_.root_path = root;
        input_.submit_terrain_chunks = [](const glm::vec4(&)[6]) {
            return TerrainSubmitStats{};
        };
        input_.static_model_tex = [](const std::string&) -> const StaticModelTex* {
            return nullptr;
        };
        glGenVertexArrays(1, &entry_vao_);
        glGenFramebuffers(2, entry_fbos_.data());
    }
    void TearDown() override {
        if (gl_.ready()) {
            pass_.destroy_instanced_static_mesh();
            pass_.destroy_skinned_mesh();
            pass_.destroy_gbuffer(resources_);
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteVertexArrays(1, &entry_vao_);
            glDeleteFramebuffers(2, entry_fbos_.data());
        }
    }
    static DiagnosticSubmission Submission() {
        return {.mesh_path = "test/diagnostic_leaf",
                .shader_variant = "instanced_static_mesh",
                .chunk = {},
                .lod = 0,
                .ownership = DiagnosticOwnership::Live,
                .pass = DiagnosticPass::GBuffer,
                .provenance = DiagnosticProvenance::Mesh};
    }
    void FillRegistry(std::uint32_t first_draw = 1) {
        DiagnosticId id{};
        // Exercise the production registry's exact 65,536-entry bound.
        for (std::uint32_t draw = first_draw; draw <= 65536; ++draw) {
            ASSERT_TRUE(table_.submit(Submission(), id));
            ASSERT_EQ(id[0], draw);
        }
        ASSERT_TRUE(table_.complete_coverage());
    }
    Mesh* AddMesh(const std::string& path = "test/diagnostic_leaf",
                  const glm::vec3& position = glm::vec3(0.0f)) {
        auto mesh = MeshLoader::CreateFromArrays({{{-1, -1, -5}, {0, 0, 1}, {0, 0}},
                                                  {{1, -1, -5}, {0, 0, 1}, {1, 0}},
                                                  {{0, 1, -5}, {0, 0, 1}, {.5f, 1}}},
                                                 {0, 1, 2});
        auto* result = mesh.get();
        pass_.register_cached_mesh(path, std::move(mesh));
        const auto entity = entities_.create();
        entities_.emplace<Luminumbra::Components::TransformComponent>(entity).position = position;
        entities_.emplace<Luminumbra::Components::StaticMeshComponent>(entity).meshPath = path;
        return result;
    }
    void SetEntryState(bool cull) const {
        glBindVertexArray(entry_vao_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, entry_fbos_[0]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, entry_fbos_[1]);
        if (cull) {
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }
    void ExpectEntryState(bool cull) const {
        EXPECT_EQ(glIsEnabled(GL_CULL_FACE), cull ? GL_TRUE : GL_FALSE);
        GLint current = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &current);
        EXPECT_EQ(static_cast<GLuint>(current), entry_vao_);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current);
        EXPECT_EQ(static_cast<GLuint>(current), entry_fbos_[0]);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &current);
        EXPECT_EQ(static_cast<GLuint>(current), entry_fbos_[1]);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }
    void ExpectInvalidMetadata(bool excessive_path);
    // Context is destroyed last, after cached meshes and shaders release their GL objects.
    AttributionContext gl_;
    RenderResourceRegistry resources_;
    GBufferPass pass_;
    DiagnosticTargets targets_;
    DiagnosticDrawTable table_;
    Camera camera_;
    Luminumbra::Client::ScenarioHarness::IsolationConfig isolation_;
    glm::vec4 planes_[6]{
        {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    RenderContext ctx_;
    GBufferPassInput input_;
    entt::registry entities_;
    GLuint entry_vao_ = 0;
    std::array<GLuint, 2> entry_fbos_{};
};

// Model a driver link refusal through GLAD, forwarding all other real GL work.
class ImpostorLinkRefusal {
public:
    ImpostorLinkRefusal() {
        Active() = this;
        glGetProgramiv = GetProgram;
    }
    ~ImpostorLinkRefusal() {
        glGetProgramiv = previous_;
        Active() = nullptr;
    }
    ImpostorLinkRefusal(const ImpostorLinkRefusal&) = delete;
    ImpostorLinkRefusal& operator=(const ImpostorLinkRefusal&) = delete;
    bool refused() const {
        return refused_;
    }

private:
    static ImpostorLinkRefusal*& Active() {
        static ImpostorLinkRefusal* probe = nullptr;
        return probe;
    }
    static void APIENTRY GetProgram(GLuint program, GLenum name, GLint* value) {
        auto& probe = *Active();
        probe.previous_(program, name, value);
        // Initialization links two geometry and two static programs before the
        // impostor. The fixture asserts that all other required programs link.
        if (name == GL_LINK_STATUS && ++probe.link_checks_ == 5) {
            *value = GL_FALSE;
            probe.refused_ = true;
        }
    }
    decltype(glGetProgramiv) previous_ = glGetProgramiv;
    unsigned link_checks_ = 0;
    bool refused_ = false;
};

class UnavailableImpostorGl : public DiagnosticAttributionGl {
protected:
    void SetUp() override {
        ASSERT_TRUE(gl_.ready());
        ImpostorLinkRefusal refusal;
        DiagnosticAttributionGl::SetUp();
        ASSERT_TRUE(refusal.refused());
    }
};
} // namespace

TEST_F(UnavailableImpostorGl, UnavailableProducerProgramFailsAnAttachedCapture) {
    // Confirm both the empty queue and a real queued leaf; no unsupported
    // record from another producer may hide the missing impostor record.
    const glm::vec3 position(200, 12, 0);
    SubmittedLiveTerrain support;
    support.add_origin({192, 0, 0});
    ASSERT_TRUE(support.covers(position));
    input_.resident_live_terrain = &support;
    input_.tree_impostor_enabled = true;
    input_.tree_impostor_grid = 1;
    input_.tree_impostor_radius = 1;
    for (const bool queued : {false, true}) {
        if (queued) {
            ASSERT_NE(AddMesh("game-assets/tree-small-02/diagnostic-leaf.lmesh", position),
                      nullptr);
        }
        for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
            table_.clear();
            input_.far_selection.effective = mode;
            ctx_.far_world_ladder = mode == FarWorldMode::Ladder;
            GBufferDiagnosticCapture capture(targets_, table_);
            SetEntryState(false);
            const auto stats = pass_.execute(ctx_, entities_, input_);
            EXPECT_EQ(stats.invalid_program_refusals, queued ? 1u : 0u);
            EXPECT_EQ(capture.complete(), !queued);
            EXPECT_EQ(table_.complete_coverage(), !queued);
            if (queued) {
                Contains(table_.json(), "tree_impostor_fragment_output_unavailable");
                const auto document = nlohmann::json::parse(table_.json());
                ASSERT_EQ(document.at("draws").size(), 1u);
                EXPECT_EQ(document.at("draws").front().at("lod"), 3u);
            } else {
                EXPECT_EQ(table_.resolve(1), nullptr);
            }
            ExpectEntryState(false);
            EXPECT_TRUE(targets_.reset());
        }
    }
}

TEST_F(DiagnosticAttributionGl, WritersRequireAttachmentAndCompletionRequiresLeaseRelease) {
    GBufferDiagnosticCapture capture(targets_, table_);
    auto* base = pass_.instanced_static_mesh_shader().get();
    const auto empty = table_.json();
    EXPECT_EQ(capture.variant(base, false), base);
    capture.write(*base, Submission());
    EXPECT_EQ(table_.json(), empty);
    EXPECT_FALSE(capture.complete());

    SetEntryState(false);
    ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
    auto* shader = capture.variant(base, false);
    ASSERT_NE(shader, base);
    ASSERT_TRUE(shader->IsValid());
    capture.write(*shader, Submission());
    EXPECT_TRUE(table_.complete_coverage());
    EXPECT_FALSE(capture.complete());
    EXPECT_FALSE(targets_.reset());
    EXPECT_FALSE(targets_.ensure(false, {}));
    EXPECT_FALSE(targets_.ensure(true, {16, 16}, 7));
    ExpectEntryState(false);

    capture.detach();
    EXPECT_TRUE(capture.complete());
    const auto recorded = table_.json();
    EXPECT_EQ(capture.variant(nullptr, true), nullptr);
    EXPECT_EQ(capture.variant(base, false), base);
    capture.write(*shader, Submission());
    EXPECT_EQ(table_.json(), recorded);
    EXPECT_TRUE(capture.complete());
    ExpectEntryState(false);
    EXPECT_TRUE(targets_.reset());
    EXPECT_FALSE(capture.attach(pass_.gbuffer(), {32, 32}));
    EXPECT_FALSE(capture.complete());
}

TEST_F(DiagnosticAttributionGl, UnavailableProducerProgramFailsAnAttachedCapture) {
    // Exercise both pointer absence and failed-link Shader objects, in both modes.
    Shader invalid("test/missing.vert", "test/missing.frag");
    ASSERT_FALSE(invalid.IsValid());
    for (const bool ladder : {false, true}) {
        for (auto* unavailable : {static_cast<Shader*>(nullptr), &invalid}) {
            table_.clear();
            GBufferDiagnosticCapture capture(targets_, table_);
            EXPECT_EQ(capture.variant(unavailable, ladder), unavailable);
            ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
            EXPECT_EQ(capture.variant(unavailable, ladder), unavailable);
            capture.detach();
            EXPECT_FALSE(capture.complete());
            EXPECT_FALSE(table_.complete_coverage());
            Contains(table_.json(), "diagnostic_base_shader_unavailable");
            EXPECT_TRUE(targets_.reset());
        }
    }

    // An empty skinned view must not bypass unavailable-program invalidation.
    pass_.reset_shaders();
    isolation_.layer_mask = Luminumbra::Client::ScenarioHarness::IsolationLayer::Skinned;
    for (const bool empty : {true, false}) {
        if (!empty) {
            const auto actor = entities_.create();
            entities_.emplace<Luminumbra::Components::TransformComponent>(actor);
            entities_.emplace<Luminumbra::Components::SkinnedMeshComponent>(actor);
            entities_.emplace<luminumbra::animation::AnimationPlayerComponent>(actor);
        }
        for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
            table_.clear();
            input_.far_selection.effective = mode;
            GBufferDiagnosticCapture capture(targets_, table_);
            SetEntryState(true);
            const auto stats = pass_.execute(ctx_, entities_, input_);
            EXPECT_EQ(stats.invalid_program_refusals, empty ? 0u : 1u);
            EXPECT_FALSE(capture.complete());
            EXPECT_FALSE(table_.complete_coverage());
            Contains(table_.json(), "diagnostic_base_shader_unavailable");
            EXPECT_TRUE(targets_.reset());
            ExpectEntryState(true);
        }
    }
}

TEST_F(DiagnosticAttributionGl, UnsupportedSubmissionRemainsIncompleteAfterTableReset) {
    GBufferDiagnosticCapture capture(targets_, table_);
    ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
    auto* shader = capture.variant(pass_.instanced_static_mesh_shader().get(), false);
    ASSERT_NE(shader, pass_.instanced_static_mesh_shader().get());
    auto submission = Submission();
    submission.provenance = DiagnosticProvenance::Unsupported;
    capture.write(*shader, submission);
    capture.detach();
    EXPECT_FALSE(capture.complete());
    table_.clear();
    EXPECT_TRUE(table_.complete_coverage());
    EXPECT_FALSE(capture.complete());
}

TEST_F(DiagnosticAttributionGl, ExhaustedWriterAndUnsupportedRefuseWithoutGrowingOrThrowing) {
    for (const bool unsupported : {false, true}) {
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
        Shader* shader = capture.variant(pass_.instanced_static_mesh_shader().get(), false);
        ASSERT_NE(shader, pass_.instanced_static_mesh_shader().get());
        ASSERT_TRUE(shader->IsValid());
        const auto location = glGetUniformLocation(shader->Id(), "u_diagnosticIds");
        ASSERT_GE(location, 0);
        // Start with a real nonzero identity to detect stale IDs after refusal.
        capture.write(*shader, Submission());
        std::array<GLuint, 4> identity{};
        glGetUniformuiv(shader->Id(), location, identity.data());
        ASSERT_NE(identity, DiagnosticId{});
        FillRegistry(2);
        const auto full = table_.json();
        EXPECT_FALSE(capture.refused());
        if (unsupported) {
            EXPECT_NO_THROW(capture.unsupported("unsupported"));
        } else {
            EXPECT_NO_THROW(capture.write(*shader, Submission()));
        }
        EXPECT_TRUE(capture.refused());
        EXPECT_FALSE(capture.complete());
        EXPECT_EQ(table_.json(), full);
        EXPECT_EQ(table_.resolve(65537), nullptr);
        EXPECT_NO_THROW(capture.write(*shader, Submission()));
        EXPECT_NO_THROW(capture.unsupported("unsupported"));
        glGetUniformuiv(shader->Id(), location, identity.data());
        EXPECT_EQ(identity, DiagnosticId{});
        EXPECT_EQ(table_.json(), full);
    }
    table_.clear();
    GBufferDiagnosticCapture fresh(table_);
    EXPECT_FALSE(fresh.refused());
    EXPECT_NO_THROW(fresh.unsupported("unsupported"));
    EXPECT_FALSE(fresh.refused());
    EXPECT_FALSE(table_.complete_coverage());
    EXPECT_FALSE(fresh.complete());
}

TEST_F(DiagnosticAttributionGl, ExhaustedDoubleSidedSubmissionRestoresCallerGlState) {
    ASSERT_NE(AddMesh(), nullptr);
    for (const bool cull : {false, true}) {
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        FillRegistry();
        const auto full = table_.json();
        SetEntryState(cull);
        GLuint query = 0;
        glGenQueries(1, &query);
        glBeginQuery(GL_PRIMITIVES_GENERATED, query);
        EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
        glEndQuery(GL_PRIMITIVES_GENERATED);
        GLuint primitives = 0;
        glGetQueryObjectuiv(query, GL_QUERY_RESULT, &primitives);
        glDeleteQueries(1, &query);
        EXPECT_EQ(primitives, 1u);
        EXPECT_TRUE(capture.refused());
        EXPECT_FALSE(capture.complete());
        EXPECT_EQ(table_.json(), full);
        EXPECT_EQ(table_.resolve(65537), nullptr);
        ExpectEntryState(cull);
    }
}

TEST_F(DiagnosticAttributionGl, InactivePassDoesNoSnapshotOrRestoreOnNormalAndExceptionalExits) {
    // Suppress producers so their pre-existing state work cannot hide pass work.
    isolation_.layer_mask = 0;
    for (const bool replay : {false, true}) {
        ctx_.frozen_replay = replay;
        SetEntryState(true);
        PassStateProbe probe;
        EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
        EXPECT_EQ(probe.integer_queries, 0u);
        EXPECT_EQ(probe.enabled_queries, 0u);
        EXPECT_EQ(probe.vao_binds, 0u);
        EXPECT_EQ(probe.combined_fbo_binds, 2u); // Pass target, then normal unbind.
        EXPECT_EQ(probe.draw_fbo_binds, 0u);
        EXPECT_EQ(probe.read_fbo_binds, 0u);
    }
    isolation_.layer_mask = Luminumbra::Client::ScenarioHarness::IsolationLayer::Terrain;
    input_.submit_terrain_chunks = [](const glm::vec4(&)[6]) -> TerrainSubmitStats {
        throw std::runtime_error("producer failure");
    };
    for (const bool replay : {false, true}) {
        ctx_.frozen_replay = replay;
        PassStateProbe probe;
        EXPECT_THROW(pass_.execute(ctx_, entities_, input_), std::runtime_error);
        EXPECT_EQ(probe.integer_queries, 0u);
        EXPECT_EQ(probe.enabled_queries, 0u);
        EXPECT_EQ(probe.vao_binds, 0u);
        EXPECT_EQ(probe.combined_fbo_binds, 1u); // Devel also leaves the target on throw.
        EXPECT_EQ(probe.draw_fbo_binds, 0u);
        EXPECT_EQ(probe.read_fbo_binds, 0u);
    }
}

TEST_F(DiagnosticAttributionGl, ActiveEmptyPassSnapshotsOnceAndRestoresOnEveryDispatch) {
    isolation_.layer_mask = 0;
    GLint max_buffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_buffers);
    ASSERT_GE(max_buffers, 7);
    ASSERT_LE(max_buffers, 32);
    // Reused storage: capabilities, texture/FBO bindings and the bounded MRT snapshot.
    const auto attachment_queries = 8u + static_cast<unsigned>(max_buffers);
    for (const bool replay : {false, true}) {
        ctx_.frozen_replay = replay;
        for (const bool cull : {false, true}) {
            table_.clear();
            GBufferDiagnosticCapture capture(targets_, table_);
            SetEntryState(cull);
            {
                PassStateProbe probe;
                EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
                EXPECT_EQ(probe.integer_queries, 3u + attachment_queries);
                EXPECT_EQ(probe.enabled_queries, 1u);
                EXPECT_EQ(probe.vao_binds, 1u);
                EXPECT_EQ(probe.combined_fbo_binds, 1u);
                EXPECT_EQ(probe.draw_fbo_binds, 7u);
                EXPECT_EQ(probe.read_fbo_binds, 1u);
            }
            ExpectEntryState(cull);
            // The fully filtered pass never certifies coverage.
            EXPECT_FALSE(capture.complete());
            Contains(table_.json(), "isolation_filtered_deferred_producer");
            EXPECT_TRUE(targets_.ensure(true, {16, 16}, 7));
            ASSERT_TRUE(targets_.ensure(true, {32, 32}, 7));
        }
    }
}

TEST_F(DiagnosticAttributionGl, FilteredDeferredLayersInvalidateCompletionEvenWhenPopulated) {
    namespace SH = Luminumbra::Client::ScenarioHarness;
    const auto actor = entities_.create();
    entities_.emplace<Luminumbra::Components::TransformComponent>(actor);
    entities_.emplace<Luminumbra::Components::SkinnedMeshComponent>(actor);
    entities_.emplace<luminumbra::animation::AnimationPlayerComponent>(actor);
    for (const std::uint32_t mask : {std::uint32_t{SH::IsolationLayer::Terrain},
                                     std::uint32_t{SH::IsolationLayer::Skinned},
                                     std::uint32_t{0}}) {
        isolation_.layer_mask = mask;
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
        EXPECT_FALSE(capture.complete());
        EXPECT_FALSE(table_.complete_coverage());
        Contains(table_.json(), "isolation_filtered_deferred_producer");
        EXPECT_TRUE(targets_.reset());
    }
}

TEST_F(DiagnosticAttributionGl, ExceptionalProducerRestoresCallerGlState) {
    input_.submit_terrain_chunks = [](const glm::vec4(&)[6]) -> TerrainSubmitStats {
        glDisable(GL_CULL_FACE);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        throw std::runtime_error("producer failure");
    };
    for (const bool replay : {false, true}) {
        ctx_.frozen_replay = replay;
        for (const bool cull : {false, true}) {
            table_.clear();
            GBufferDiagnosticCapture capture(targets_, table_);
            SetEntryState(cull);
            EXPECT_THROW(pass_.execute(ctx_, entities_, input_), std::runtime_error);
            EXPECT_FALSE(capture.complete());
            EXPECT_TRUE(targets_.reset());
            ExpectEntryState(cull);
        }
    }
}

TEST_F(DiagnosticAttributionGl, AuthoredUpgradeRestoresLiveCallerFramebuffers) {
    auto mesh = std::make_shared<StaticMesh>();
    mesh->identity = "diagnostic-authored-triangle";
    mesh->vertices = {{{-1, -1, -5}, {0, 0, 1}, {0, 0}},
                      {{1, -1, -5}, {0, 0, 1}, {1, 0}},
                      {{0, 1, -5}, {0, 0, 1}, {.5f, 1}}};
    mesh->indices = {0, 1, 2};
    auto material = std::make_shared<StaticMaterial>();
    material->identity = "diagnostic-authored-material";
    auto snapshot = std::make_shared<StaticDrawSnapshot>();
    StaticDraw draw;
    draw.key = {"instance", "node", 0};
    draw.mesh = mesh;
    draw.material = material;
    draw.normal = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    snapshot->draws.push_back(draw);
    input_.authored_draws = snapshot;
    RenderViewDescription description;
    description.width = 32;
    description.height = 32;
    description.revision = 1;
    const auto projection = glm::perspectiveRH_ZO(
        glm::radians(45.0), 1.0, description.far_plane, description.near_plane);
    std::copy_n(glm::value_ptr(projection), 16, description.projection.begin());
    const auto view = RenderView::Validate(description);
    input_.authored_view = &view;

    // External, draw-only, read-only, both, default, and initially absent targets.
    // Each of these six states exercises success and failures before/after upgrade.
    for (unsigned bindings = 0; bindings < 6; ++bindings) {
        for (unsigned failure = 0; failure < 3; ++failure) {
            SCOPED_TRACE(testing::Message() << bindings << ":" << failure);
            pass_.destroy_gbuffer(resources_);
            if (bindings != 5) {
                pass_.init_gbuffer(resources_, 32, 32);
            }
            const GLuint old_fbo = pass_.gbuffer().fbo_id;
            SetEntryState(true);
            const GLuint draw_fbo = bindings >= 4 ? 0 : (bindings & 1u) ? old_fbo : entry_fbos_[0];
            const GLuint read_fbo = bindings >= 4 ? 0 : (bindings & 2u) ? old_fbo : entry_fbos_[1];
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);
            input_.authored_temporal_enabled = failure == 2;
            input_.submit_terrain_chunks = [failure](const glm::vec4(&)[6]) {
                if (failure == 1) {
                    throw std::runtime_error("producer failure after upgrade");
                }
                return TerrainSubmitStats{};
            };
            table_.clear();
            GBufferDiagnosticCapture capture(targets_, table_);
            PassStateProbe probe(old_fbo);
            if (failure == 2) {
                EXPECT_THROW(pass_.execute(ctx_, entities_, input_), std::invalid_argument);
                EXPECT_EQ(pass_.gbuffer().fbo_id, old_fbo);
            } else {
                if (failure == 1) {
                    EXPECT_THROW(pass_.execute(ctx_, entities_, input_), std::runtime_error);
                } else {
                    EXPECT_EQ(pass_.execute(ctx_, entities_, input_).authored_draws, 1u);
                }
                ASSERT_NE(pass_.gbuffer().fbo_id, old_fbo);
                ASSERT_NE(pass_.gbuffer().authored_texture, 0u);
            }
            const auto expected = [&](GLuint binding) {
                return old_fbo != 0 && binding == old_fbo ? pass_.gbuffer().fbo_id : binding;
            };
            for (const GLenum target :
                 {GLenum{GL_DRAW_FRAMEBUFFER_BINDING}, GLenum{GL_READ_FRAMEBUFFER_BINDING}}) {
                GLint current = 0;
                glGetIntegerv(target, &current);
                EXPECT_EQ(static_cast<GLuint>(current),
                          expected(target == GL_DRAW_FRAMEBUFFER_BINDING ? draw_fbo : read_fbo));
                if (current != 0) {
                    EXPECT_EQ(glIsFramebuffer(static_cast<GLuint>(current)), GL_TRUE);
                }
            }
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }
    }
}

TEST_F(DiagnosticAttributionGl, SuccessfulInstancesRecordDistinctIdentitiesAndResolvedLods) {
    const std::string base = "test/diagnostic_leaf.lmesh";
    ASSERT_NE(AddMesh(base), nullptr);
    ASSERT_NE(AddMesh(base, {1, 0, 0}), nullptr);
    ASSERT_NE(AddMesh(base, {0, 0, -200}), nullptr);
    pass_.register_cached_mesh("test/diagnostic_leaf.lod1.lmesh",
                               MeshLoader::CreateFromArrays({{{-1, -1, -5}, {0, 0, 1}, {0, 0}},
                                                             {{1, -1, -5}, {0, 0, 1}, {1, 0}},
                                                             {{0, 1, -5}, {0, 0, 1}, {.5f, 1}}},
                                                            {0, 1, 2}));
    GBufferDiagnosticCapture capture(targets_, table_);
    SetEntryState(false);
    GLuint query = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_PRIMITIVES_GENERATED, query);
    EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
    glEndQuery(GL_PRIMITIVES_GENERATED);
    GLuint primitives = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &primitives);
    glDeleteQueries(1, &query);
    EXPECT_EQ(primitives, 3u);
    EXPECT_FALSE(capture.refused());
    EXPECT_TRUE(table_.complete_coverage());
    const auto document = nlohmann::json::parse(table_.json());
    ASSERT_EQ(document.at("draws").size(), 3u);
    ASSERT_EQ(document.at("meshes").size(), 2u);
    std::array<unsigned, 2> lod_counts{};
    for (std::uint32_t id = 1; id <= 3; ++id) {
        const auto* identity = table_.resolve(id);
        ASSERT_NE(identity, nullptr);
        EXPECT_TRUE(ValidDiagnosticId(*identity));
        EXPECT_EQ((*identity)[0], id);
        ASSERT_LE((*identity)[2], 1u);
        ++lod_counts[(*identity)[2]];
        const auto& recorded = document.at("draws").at(id - 1);
        EXPECT_EQ(recorded.at("lod"), (*identity)[2]);
        EXPECT_EQ(recorded.at("shader_variant"), "instanced_static_mesh");
        const auto& resolved_mesh = document.at("meshes").at((*identity)[1] - 1);
        EXPECT_EQ(resolved_mesh.at("path"),
                  (*identity)[2] == 0 ? base : "test/diagnostic_leaf.lod1.lmesh");
    }
    EXPECT_EQ(lod_counts, (std::array<unsigned, 2>{2, 1}));
    EXPECT_EQ(table_.resolve(4), nullptr);
    ExpectEntryState(false);
}

void DiagnosticAttributionGl::ExpectInvalidMetadata(bool excessive_path) {
    const std::string path = excessive_path ? std::string(1025, 'a') : "test/diagnostic_leaf";
    auto* mesh = AddMesh(path);
    ASSERT_NE(mesh, nullptr);
    if (!excessive_path) {
        mesh->indexCount = static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()) + 1u;
    }
    table_.clear();
    GBufferDiagnosticCapture capture(targets_, table_);
    SetEntryState(true);
    GLuint query = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_PRIMITIVES_GENERATED, query);
    EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
    glEndQuery(GL_PRIMITIVES_GENERATED);
    GLuint primitives = 1;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &primitives);
    glDeleteQueries(1, &query);
    // An excessive identity path is still drawn (the scene must not change); an
    // unrepresentable index count cannot be submitted at all.
    EXPECT_EQ(primitives, excessive_path ? 1u : 0u);
    EXPECT_FALSE(capture.complete());
    EXPECT_FALSE(table_.complete_coverage());
    const auto document = nlohmann::json::parse(table_.json());
    ASSERT_FALSE(document.at("draws").empty());
    if (excessive_path) {
        EXPECT_EQ(document.at("draws").back().at("supported"), false);
        for (const auto& recorded : document.at("meshes")) {
            EXPECT_NE(recorded.at("path"), path);
        }
    }
    EXPECT_EQ(document.at("draws").back().at("shader_variant"),
              excessive_path ? "static_mesh_identity_path_limit"
                             : "diagnostic_static_mesh_index_count");
    ExpectEntryState(true);
}

TEST_F(DiagnosticAttributionGl, InvalidIndexCountRecordsIncompleteCoverageWithoutDrawing) {
    ExpectInvalidMetadata(false);
}

TEST_F(DiagnosticAttributionGl, ExcessivePathRecordsIncompleteCoverageAndStillDraws) {
    ExpectInvalidMetadata(true);
}

TEST_F(DiagnosticAttributionGl, JsonBudgetRefusalIsStickyWithoutGrowingOrThrowing) {
    GBufferDiagnosticCapture capture(targets_, table_);
    ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
    Shader* shader = capture.variant(pass_.instanced_static_mesh_shader().get(), false);
    ASSERT_NE(shader, pass_.instanced_static_mesh_shader().get());
    auto submission = Submission();
    const std::string escaped(1024, '\n');
    submission.shader_variant = escaped;
    DiagnosticId id{};
    std::uint32_t accepted = 0;
    for (; accepted < 65536; ++accepted) {
        if (!table_.submit(submission, id)) {
            break;
        }
    }
    ASSERT_GT(accepted, 0u);
    ASSERT_LT(accepted, 65536u);
    const auto full = table_.json();
    EXPECT_LE(full.size(), 16u * 1024u * 1024u);
    EXPECT_NO_THROW(capture.write(*shader, submission));
    EXPECT_TRUE(capture.refused());
    EXPECT_FALSE(capture.complete());
    EXPECT_NO_THROW(capture.write(*shader, Submission()));
    EXPECT_NO_THROW(capture.unsupported("unsupported"));
    EXPECT_EQ(table_.json(), full);
}

TEST_F(DiagnosticAttributionGl, InvalidIdentityRefusesWithoutReusingThePreviousId) {
    const std::string malformed_utf8(1, static_cast<char>(0xff));
    auto invalid = Submission();
    invalid.lod = 256;
    auto unknown = Submission();
    unknown.ownership = static_cast<DiagnosticOwnership>(0);
    auto missing = Submission();
    missing.mesh_path = {};
    auto malformed = Submission();
    malformed.shader_variant = malformed_utf8;
    for (const auto& submission : {invalid, unknown, missing, malformed}) {
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
        Shader* shader = capture.variant(pass_.instanced_static_mesh_shader().get(), false);
        ASSERT_NE(shader, pass_.instanced_static_mesh_shader().get());
        capture.write(*shader, Submission());
        const auto before = table_.json();
        EXPECT_NO_THROW(capture.write(*shader, submission));
        EXPECT_TRUE(capture.refused());
        EXPECT_FALSE(capture.complete());
        EXPECT_NO_THROW(capture.write(*shader, Submission()));
        EXPECT_EQ(table_.json(), before);
        const auto location = glGetUniformLocation(shader->Id(), "u_diagnosticIds");
        ASSERT_GE(location, 0);
        DiagnosticId identity{};
        glGetUniformuiv(shader->Id(), location, identity.data());
        EXPECT_EQ(identity, DiagnosticId{});
    }
}

namespace {
// Forward to real GL while observing lease ownership at the detach call itself.
class AttachmentProbe {
public:
    AttachmentProbe(DiagnosticTargets& targets, GLuint framebuffer)
        : targets_(targets)
        , framebuffer_(framebuffer) {
        Active() = this;
        glFramebufferTexture2D = Attach;
        glDrawBuffers = DrawBuffers;
        glCheckFramebufferStatus = CheckStatus;
        glGetIntegerv = GetInteger;
    }
    ~AttachmentProbe() {
        glFramebufferTexture2D = attach_;
        glDrawBuffers = draw_buffers_;
        glCheckFramebufferStatus = check_status_;
        glGetIntegerv = get_integer_;
        Active() = nullptr;
    }
    AttachmentProbe(const AttachmentProbe&) = delete;
    AttachmentProbe& operator=(const AttachmentProbe&) = delete;

    bool incomplete = false;
    bool incomplete_storage = false;
    GLint max_buffers = 0;
    GLint max_attachments = 0;
    unsigned detaches = 0;
    unsigned buffer_writes = 0;
    bool released_before_detach = false;

private:
    static AttachmentProbe*& Active() {
        static AttachmentProbe* probe = nullptr;
        return probe;
    }
    static void APIENTRY
    Attach(GLenum target, GLenum point, GLenum kind, GLuint texture, GLint level) {
        auto& probe = *Active();
        if (point == GL_COLOR_ATTACHMENT6 && texture == 0) {
            ++probe.detaches;
            probe.released_before_detach = probe.targets_.reset();
        }
        probe.attach_(target, point, kind, texture, level);
    }
    static void APIENTRY DrawBuffers(GLsizei count, const GLenum* buffers) {
        auto& probe = *Active();
        ++probe.buffer_writes;
        probe.draw_buffers_(count, buffers);
    }
    static GLenum APIENTRY CheckStatus(GLenum target) {
        auto& probe = *Active();
        GLint bound = 0;
        probe.get_integer_(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        if (probe.incomplete && static_cast<GLuint>(bound) == probe.framebuffer_) {
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        if (probe.incomplete_storage && static_cast<GLuint>(bound) != probe.framebuffer_) {
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        return probe.check_status_(target);
    }
    static void APIENTRY GetInteger(GLenum name, GLint* value) {
        auto& probe = *Active();
        if (name == GL_MAX_DRAW_BUFFERS && probe.max_buffers != 0) {
            *value = probe.max_buffers;
        } else if (name == GL_MAX_COLOR_ATTACHMENTS && probe.max_attachments != 0) {
            *value = probe.max_attachments;
        } else {
            probe.get_integer_(name, value);
        }
    }
    DiagnosticTargets& targets_;
    GLuint framebuffer_;
    decltype(glFramebufferTexture2D) attach_ = glFramebufferTexture2D;
    decltype(glDrawBuffers) draw_buffers_ = glDrawBuffers;
    decltype(glCheckFramebufferStatus) check_status_ = glCheckFramebufferStatus;
    decltype(glGetIntegerv) get_integer_ = glGetIntegerv;
};

class GBufferLeaseGl : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(context_.ready());
        CreateGBufferTargets(gbuffer_, registry_, 32, 32);
        ASSERT_NE(gbuffer_.fbo_id, 0u);
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &buffer_count_);
        ASSERT_GE(buffer_count_, 7);
        ASSERT_LE(buffer_count_, static_cast<GLint>(buffers_.size()));
        buffers_[0] = GL_COLOR_ATTACHMENT4;
        buffers_[1] = GL_COLOR_ATTACHMENT2;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
        glDrawBuffers(buffer_count_, buffers_.data());
        glGenFramebuffers(2, caller_fbos_.data());
        SetCallerBindings();
        glColorMaski(6, GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    }
    void TearDown() override {
        if (context_.ready()) {
            DestroyGBufferTargets(gbuffer_, registry_);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteFramebuffers(2, caller_fbos_.data());
        }
    }
    void SetCallerBindings() const {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, caller_fbos_[0]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, caller_fbos_[1]);
        glBindTexture(GL_TEXTURE_2D, gbuffer_.albedo_texture);
    }
    void ExpectCallerBindingsAndMask() const {
        GLint binding = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &binding);
        EXPECT_EQ(static_cast<GLuint>(binding), caller_fbos_[0]);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &binding);
        EXPECT_EQ(static_cast<GLuint>(binding), caller_fbos_[1]);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        EXPECT_EQ(static_cast<GLuint>(binding), gbuffer_.albedo_texture);
        std::array<GLboolean, 4> mask{};
        glGetBooleani_v(GL_COLOR_WRITEMASK, 6, mask.data());
        EXPECT_EQ(mask, (std::array<GLboolean, 4>{GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE}));
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }
    void ExpectDetached() const {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
        GLint type = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                              &type);
        EXPECT_EQ(type, GL_NONE);
        for (GLint index = 0; index < buffer_count_; ++index) {
            GLint buffer = 0;
            glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(index), &buffer);
            EXPECT_EQ(static_cast<GLenum>(buffer), buffers_[static_cast<std::size_t>(index)]);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, caller_fbos_[0]);
    }
    AttributionContext context_;
    RenderResourceRegistry registry_;
    GBuffer gbuffer_;
    DiagnosticTargets targets_;
    DiagnosticDrawTable table_;
    std::array<GLuint, 2> caller_fbos_{};
    // Match the production snapshot's explicit bound, including trailing GL_NONE.
    std::array<GLenum, 32> buffers_{};
    GLint buffer_count_ = 0;
};
} // namespace

TEST_F(GBufferLeaseGl, UnboundAttachmentBlocksResizeUntilExplicitDetach) {
    GBufferDiagnosticCapture capture(targets_, table_);
    AttachmentProbe probe(targets_, gbuffer_.fbo_id);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    const auto texture = targets_.texture();
    EXPECT_FALSE(targets_.reset());
    EXPECT_FALSE(targets_.ensure(true, {16, 16}, 7));
    EXPECT_EQ(targets_.texture(), texture);
    capture.detach();
    EXPECT_EQ(probe.detaches, 1u);
    EXPECT_FALSE(probe.released_before_detach);
    EXPECT_FALSE(capture.attachment_failed());
    ExpectCallerBindingsAndMask();
    ExpectDetached();
    capture.detach();
    EXPECT_EQ(probe.detaches, 1u);
    EXPECT_TRUE(targets_.ensure(true, {16, 16}, 7));
    EXPECT_TRUE(targets_.reset());
}

TEST_F(GBufferLeaseGl, IncompleteFramebufferRollsBackBeforeLeaseRelease) {
    GBufferDiagnosticCapture capture(targets_, table_);
    AttachmentProbe probe(targets_, gbuffer_.fbo_id);
    probe.incomplete = true;
    EXPECT_FALSE(capture.attach(gbuffer_, {32, 32}));
    EXPECT_TRUE(capture.attachment_failed());
    EXPECT_EQ(probe.detaches, 1u);
    EXPECT_FALSE(probe.released_before_detach);
    Contains(table_.json(), "diagnostic_mrt_incomplete");
    ExpectCallerBindingsAndMask();
    ExpectDetached();
    EXPECT_TRUE(targets_.reset());
}

TEST_F(GBufferLeaseGl, CaptureAndAttachmentScopesDetachDuringNormalAndExceptionalTeardown) {
    for (const bool exceptional : {false, true}) {
        for (const bool attachment_scope : {false, true}) {
            table_.clear();
            AttachmentProbe probe(targets_, gbuffer_.fbo_id);
            if (attachment_scope) {
                GBufferDiagnosticCapture capture(targets_, table_);
                try {
                    GBufferDiagnosticAttachment attachment(gbuffer_, {32, 32});
                    EXPECT_FALSE(targets_.reset());
                    if (exceptional) {
                        throw std::runtime_error("attachment unwinding");
                    }
                } catch (const std::runtime_error&) {}
                EXPECT_EQ(capture.attachment_failed(), exceptional);
            } else {
                try {
                    GBufferDiagnosticCapture capture(targets_, table_);
                    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
                    if (exceptional) {
                        throw std::runtime_error("capture unwinding");
                    }
                } catch (const std::runtime_error&) {}
            }
            EXPECT_EQ(GBufferDiagnosticCapture::Current(), nullptr);
            EXPECT_EQ(probe.detaches, 1u);
            EXPECT_FALSE(probe.released_before_detach);
            ExpectCallerBindingsAndMask();
            ExpectDetached();
            EXPECT_TRUE(targets_.reset());
        }
    }
}

TEST_F(GBufferLeaseGl, RecreationDetachesBeforeRegistryDeletion) {
    GBufferDiagnosticCapture capture(targets_, table_);
    AttachmentProbe probe(targets_, gbuffer_.fbo_id);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    const auto retired = gbuffer_.fbo_id;
    // An unrelated G-buffer must not release this lease.
    capture.before_destroy(GBuffer{});
    EXPECT_FALSE(targets_.reset());
    DestroyGBufferTargets(gbuffer_, registry_);
    EXPECT_EQ(probe.detaches, 1u);
    EXPECT_FALSE(probe.released_before_detach);
    EXPECT_EQ(glIsFramebuffer(retired), GL_FALSE);
    EXPECT_TRUE(capture.attachment_failed());
    CreateGBufferTargets(gbuffer_, registry_, 32, 32, true);
    SetCallerBindings();
    capture.detach();
    Contains(table_.json(), "gbuffer_recreated_during_capture");
    ExpectCallerBindingsAndMask();
    EXPECT_TRUE(targets_.reset());
}

TEST_F(GBufferLeaseGl, EveryRegistryRetirementRefusesCaptureWithoutRestoringRetiredDrawBuffers) {
    for (const unsigned operation : {0u, 1u, 2u}) {
        SCOPED_TRACE(operation);
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
        if (operation == 0) {
            registry_.destroy_owned("gbuffer_fbo");
        } else if (operation == 1) {
            registry_.destroy_all_owned();
        } else {
            const std::array<RenderResourceRegistry::TargetExtent, 6> extents{
                {{"gbuffer_position", 16, 16},
                 {"gbuffer_normal", 16, 16},
                 {"gbuffer_albedo", 16, 16},
                 {"gbuffer_material", 16, 16},
                 {"gbuffer_motion", 16, 16},
                 {"gbuffer_depth", 16, 16}}};
            ASSERT_TRUE(registry_.replace_targets(extents));
        }
        // Registry operations may change bindings; detach preserves its caller's state.
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, caller_fbos_[0]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, caller_fbos_[1]);
        glBindTexture(GL_TEXTURE_2D, 0);
        {
            AttachmentProbe probe(targets_, gbuffer_.fbo_id);
            capture.detach();
            EXPECT_EQ(probe.buffer_writes, 0u);
            EXPECT_EQ(probe.detaches, 0u);
        }
        EXPECT_TRUE(capture.attachment_failed());
        Contains(table_.json(), "gbuffer_retired_during_capture");
        // These cached handles are no longer the registry's current objects.
        gbuffer_.albedo_texture = 0;
        ExpectCallerBindingsAndMask();
        EXPECT_TRUE(targets_.reset());
        DestroyGBufferTargets(gbuffer_, registry_);
        CreateGBufferTargets(gbuffer_, registry_, 32, 32);
        SetCallerBindings();
    }
}

TEST_F(GBufferLeaseGl,
       ForeignOrMissingAttachmentPreservesForeignDrawBuffersAndRestoresContextState) {
    for (const unsigned replacement : {0u, 1u, 2u}) {
        SCOPED_TRACE(replacement);
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
        GLuint renderbuffer = 0;
        if (replacement == 2) {
            glGenRenderbuffers(1, &renderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);
            glFramebufferRenderbuffer(
                GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_RENDERBUFFER, renderbuffer);
        } else {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT6,
                                   GL_TEXTURE_2D,
                                   replacement == 1 ? gbuffer_.albedo_texture : 0,
                                   0);
        }
        glDrawBuffer(GL_COLOR_ATTACHMENT1);
        SetCallerBindings();
        {
            AttachmentProbe probe(targets_, gbuffer_.fbo_id);
            capture.detach();
            EXPECT_EQ(probe.buffer_writes, 0u);
            EXPECT_EQ(probe.detaches, 0u);
        }
        EXPECT_TRUE(capture.attachment_failed());
        Contains(table_.json(), "gbuffer_retired_during_capture");
        ExpectCallerBindingsAndMask();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
        GLint buffer = 0, name = 0;
        glGetIntegerv(GL_DRAW_BUFFER0, &buffer);
        EXPECT_EQ(buffer, GL_COLOR_ATTACHMENT1);
        if (replacement != 0) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                                  GL_COLOR_ATTACHMENT6,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                                  &name);
            EXPECT_EQ(static_cast<GLuint>(name),
                      replacement == 2 ? renderbuffer : gbuffer_.albedo_texture);
        }
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
        glDrawBuffers(buffer_count_, buffers_.data());
        SetCallerBindings();
        glDeleteRenderbuffers(1, &renderbuffer);
        EXPECT_TRUE(targets_.reset());
    }
}

TEST_F(GBufferLeaseGl, InvalidAttachmentRequestsLeaveBindingsAndStorageUntouched) {
    for (const unsigned refusal : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u}) {
        SCOPED_TRACE(refusal);
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        AttachmentProbe probe(targets_, gbuffer_.fbo_id);
        auto gbuffer = gbuffer_;
        GLuint empty_texture = 0;
        DiagnosticExtent extent{32, 32};
        if (refusal == 0) {
            gbuffer.fbo_id = 0;
        } else if (refusal == 1) {
            extent.width = 0;
        } else if (refusal == 2) {
            extent.height = 16;
        } else if (refusal == 3) {
            probe.max_buffers = 6;
        } else if (refusal == 4) {
            probe.max_buffers = 33;
        } else if (refusal == 5) {
            probe.max_attachments = 6;
        } else if (refusal == 6) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT6,
                                   GL_TEXTURE_2D,
                                   gbuffer_.albedo_texture,
                                   0);
            SetCallerBindings();
        } else if (refusal == 7) {
            gbuffer.position_texture = 0;
        } else if (refusal == 8) {
            glGenFramebuffers(1, &gbuffer.fbo_id);
            glDeleteFramebuffers(1, &gbuffer.fbo_id);
        } else if (refusal == 9) {
            extent.width = 16;
        } else if (refusal == 10) {
            probe.incomplete_storage = true;
        } else {
            glGenTextures(1, &empty_texture);
            glBindTexture(GL_TEXTURE_2D, empty_texture);
            gbuffer.position_texture = empty_texture;
            SetCallerBindings();
        }
        EXPECT_FALSE(capture.attach(gbuffer, extent));
        EXPECT_TRUE(capture.attachment_failed());
        EXPECT_EQ(targets_.texture(), 0u);
        EXPECT_EQ(probe.detaches, 0u);
        ExpectCallerBindingsAndMask();
        glDeleteTextures(1, &empty_texture);
        if (refusal == 6) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
            SetCallerBindings();
        }
    }
}

TEST_F(GBufferLeaseGl, TableOnlyAndNestedAttachmentRefuseWithoutDroppingExistingLease) {
    {
        GBufferDiagnosticCapture capture(table_);
        EXPECT_FALSE(capture.attach(gbuffer_, {32, 32}));
        Contains(table_.json(), "diagnostic_target_storage_unavailable");
    }
    table_.clear();
    GBufferDiagnosticCapture capture(targets_, table_);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    EXPECT_FALSE(capture.attach(gbuffer_, {32, 32}));
    EXPECT_FALSE(targets_.reset());
    capture.detach();
    EXPECT_FALSE(capture.attach(gbuffer_, {32, 32}));
    Contains(table_.json(), "nested_gbuffer_attachment");
    ExpectCallerBindingsAndMask();
    ExpectDetached();
    EXPECT_TRUE(targets_.reset());
}

TEST_F(GBufferLeaseGl, AuthoredAttachmentUsesSixExistingOutputsAndRestoresThem) {
    DestroyGBufferTargets(gbuffer_, registry_);
    CreateGBufferTargets(gbuffer_, registry_, 32, 32, true);
    ASSERT_NE(gbuffer_.authored_texture, 0u);
    SetCallerBindings();
    GBufferDiagnosticCapture capture(targets_, table_);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
    GLint buffer = 0;
    glGetIntegerv(GL_DRAW_BUFFER5, &buffer);
    EXPECT_EQ(buffer, GL_COLOR_ATTACHMENT5);
    glGetIntegerv(GL_DRAW_BUFFER6, &buffer);
    EXPECT_EQ(buffer, GL_COLOR_ATTACHMENT6);
    SetCallerBindings();
    capture.detach();
    ExpectCallerBindingsAndMask();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
    glGetIntegerv(GL_DRAW_BUFFER5, &buffer);
    EXPECT_EQ(buffer, GL_COLOR_ATTACHMENT5);
    glGetIntegerv(GL_DRAW_BUFFER6, &buffer);
    EXPECT_EQ(buffer, GL_NONE);
    SetCallerBindings();
    EXPECT_TRUE(targets_.reset());
}

TEST_F(GBufferLeaseGl, RecreationPreservesForeignAttachmentRetirementReason) {
    GBufferDiagnosticCapture capture(targets_, table_);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
    glFramebufferTexture2D(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, gbuffer_.albedo_texture, 0);
    SetCallerBindings();
    capture.before_destroy(gbuffer_);
    capture.detach();
    EXPECT_TRUE(capture.attachment_failed());
    Contains(table_.json(), "gbuffer_retired_during_capture");
    EXPECT_EQ(table_.json().find("gbuffer_recreated_during_capture"), std::string::npos);
    ExpectCallerBindingsAndMask();
    EXPECT_TRUE(targets_.reset());
}

TEST_F(GBufferLeaseGl, RetirementReleasesLeaseEvenWhenRefusalCannotBeRecorded) {
    GBufferDiagnosticCapture capture(targets_, table_);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    capture.unsupported(std::string(1025, 'x'));
    ASSERT_TRUE(capture.refused());
    registry_.destroy_owned("gbuffer_fbo");
    capture.detach();
    EXPECT_TRUE(capture.attachment_failed());
    ExpectCallerBindingsAndMask();
    EXPECT_TRUE(targets_.reset());
}

TEST(DiagnosticAttribution, GeneratedMeshKeysAreExactBoundedAndSeparateFromAssetPaths) {
    using namespace Luminumbra::Rendering;
    DiagnosticDrawTable table;
    DiagnosticSubmission generated{.mesh_path = {},
                                   .shader_variant = "terrain_mdi",
                                   .chunk = {-2, 1, 3},
                                   .lod = 2,
                                   .ownership = DiagnosticOwnership::Live,
                                   .pass = DiagnosticPass::GBuffer,
                                   .provenance = DiagnosticProvenance::Unknown};
    DiagnosticId first{}, repeated{}, changed{}, asset{};
    ASSERT_TRUE(SubmitGenerated(table, generated, "terrain/-2/1/3/version/7", first));
    ASSERT_TRUE(SubmitGenerated(table, generated, "terrain/-2/1/3/version/7", repeated));
    ASSERT_TRUE(SubmitGenerated(table, generated, "terrain/-2/1/3/version/8", changed));
    generated.mesh_path = "terrain/-2/1/3/version/7";
    ASSERT_TRUE(table.submit(generated, asset));
    EXPECT_NE(first[0], repeated[0]);
    EXPECT_EQ(first[1], repeated[1]);
    EXPECT_NE(first[1], changed[1]);
    EXPECT_NE(first[1], asset[1]);
    EXPECT_EQ(first[2], 2u);
    EXPECT_EQ(first[3], 257u);
    const auto meshes = nlohmann::json::parse(table.json());
    EXPECT_EQ(meshes["meshes"][0]["path"], "");
    EXPECT_EQ(meshes["meshes"][0]["generated"], "terrain/-2/1/3/version/7");
    EXPECT_EQ(meshes["meshes"][0]["provenance"], DiagnosticProvenance::Unknown);
    EXPECT_TRUE(table.complete_coverage());
    const auto before = table.json();
    DiagnosticId refused{11, 12, 13, 14};
    const auto sentinel = refused;
    // Ambiguous asset/generated input, invalid keys, UTF-8 and LOD all refuse atomically.
    EXPECT_FALSE(SubmitGenerated(table, generated, "terrain/valid", refused));
    generated.mesh_path = {};
    for (const auto& key : {std::string{},
                            std::string("../escape"),
                            std::string("/absolute"),
                            std::string("a\\b"),
                            std::string(1025, 'x'),
                            std::string("\xff")}) {
        EXPECT_FALSE(SubmitGenerated(table, generated, key, refused));
    }
    generated.lod = 256;
    EXPECT_FALSE(SubmitGenerated(table, generated, "terrain/valid", refused));
    EXPECT_EQ(table.json(), before);
    EXPECT_EQ(refused, sentinel);
    generated.lod = 0;
    generated.pass = DiagnosticPass::Forward;
    ASSERT_TRUE(SubmitGenerated(table, generated, std::string(1024, 'x'), refused));
    EXPECT_EQ(refused[2], 0u);
    EXPECT_EQ(refused[3], 513u);
}

TEST(DiagnosticAttribution, ForwardAttachmentUsesTheSelectedLeaseAndPreservesDeferredStorage) {
    const auto source = Read(kTargets) + Read(kPass);
    Contains(source, "forward_targets == &targets");
    const auto attach = Body(source, "bool GBufferDiagnosticCapture::attach_target(");
    Contains(attach, "forward ? !state_->captured || state_->forward_captured");
    Contains(attach, "auto& targets = forward ? *state_->forward_targets : *state_->targets");
    Before(attach, "targets.ensure(true, extent, 7)", "targets.acquire_attachment_lease()");
    Before(attach, "targets.acquire_attachment_lease()", "glFramebufferTexture2D(");
    Contains(attach, "state_->attachment_lease = targets.acquire_attachment_lease()");
    EXPECT_EQ(Count(attach, "state_->attachment_lease.texture()"), 2u);
    Contains(attach, "auto forward_buffers = state_->buffers");
    Contains(attach, "forward_buffers[6] = GL_COLOR_ATTACHMENT6");
    Contains(attach, "glDrawBuffers(state_->buffer_count, forward_buffers.data())");
    Contains(attach, "state_->forward_captured = true");
    const auto forward = Body(source, "bool GBufferDiagnosticCapture::attach_forward(");
    Contains(forward, "if (!state_->forward_targets)");
    Contains(forward, "foliage_forward_contributor_target_unavailable");
    Contains(forward, "attach_target(target, extent, true)");
    Contains(forward, "unsupported(reason, 0, DiagnosticOwnership::Live, DiagnosticPass::Forward)");
    Contains(Body(source, "bool GBufferDiagnosticCapture::deferred_attached()"),
             "state_->framebuffer != 0 && !state_->forward");
    const auto scope =
        Body(source, "GBufferDiagnosticAttachment::GBufferDiagnosticAttachment(std::uint32_t");
    Contains(scope, "if (capture_)");
    Contains(scope, "exceptions_ = std::uncaught_exceptions()");
    Contains(scope, "capture_->attach_forward(framebuffer, color_texture, extent)");
    Contains(scope, "capture_ = nullptr");
}

TEST(DiagnosticAttribution, CurrentWriterAndFeedbackVariantsAreGuardedAndBounded) {
    const auto source = Read(kTargets);
    const auto current = Body(source, "void GBufferDiagnosticCapture::write_current(");
    Before(current, "if (!state_->framebuffer)", "glGetIntegerv(");
    Contains(current, "entry.shader->Id() == static_cast<GLuint>(program)");
    Contains(current, "write(*entry.shader, submission)");
    Contains(current, "diagnostic_writer_without_variant");
    const auto write = Body(source, "void GBufferDiagnosticCapture::write(");
    Contains(write, "submission.pass == (state_->forward ? DiagnosticPass::Forward");
    Contains(write, "diagnostic_contributor_pass_mismatch");
    Contains(write, "matching_pass && !state_->refused && !state_->table.submit");
    Before(write, "DiagnosticId id{}", "glProgramUniform4uiv(");
    EXPECT_EQ(write.find("throw"), std::string::npos);
    const auto variant = Body(source, "Shader* GBufferDiagnosticCapture::variant(");
    Contains(variant, "state_->variants.size() == 6");
    Contains(variant, "feedback_varyings.size() > 3");
    Contains(variant, "name.size() > 64");
    Contains(variant, "entry.feedback_varyings == feedback_varyings");
    Contains(variant, "shader->FragmentPath().c_str(), feedback_varyings, defines");
}

TEST_F(DiagnosticAttributionGl, ForwardLeaseProtectsOnlyItsOwnStorageAndRestoresState) {
    DiagnosticTargets forward;
    GBufferDiagnosticCapture capture(targets_, table_, &forward);
    ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
    EXPECT_TRUE(capture.deferred_attached());
    capture.detach();
    const auto deferred_texture = targets_.texture();
    const DiagnosticId sentinel{9, 8, 7, 6};
    glClearTexImage(deferred_texture, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, sentinel.data());
    SetEntryState(true);
    ASSERT_TRUE(
        capture.attach_forward(pass_.gbuffer().fbo_id, pass_.gbuffer().position_texture, {32, 32}));
    ExpectEntryState(true);
    EXPECT_FALSE(capture.deferred_attached());
    const auto forward_texture = forward.texture();
    EXPECT_NE(forward_texture, deferred_texture);
    EXPECT_FALSE(forward.reset());
    EXPECT_FALSE(forward.ensure(false, {}));
    EXPECT_FALSE(forward.ensure(true, {16, 16}, 7));
    EXPECT_EQ(forward.texture(), forward_texture);
    DiagnosticId retained{};
    glGetTextureSubImage(deferred_texture,
                         0,
                         0,
                         0,
                         0,
                         1,
                         1,
                         1,
                         GL_RGBA_INTEGER,
                         GL_UNSIGNED_INT,
                         static_cast<GLsizei>(sizeof(retained)),
                         retained.data());
    EXPECT_EQ(retained, sentinel);
    EXPECT_TRUE(targets_.ensure(true, {16, 16}, 7));
    EXPECT_FALSE(capture.complete());
    capture.detach();
    ExpectEntryState(true);
    EXPECT_TRUE(capture.complete());
    EXPECT_TRUE(forward.reset());
}

TEST_F(DiagnosticAttributionGl, CurrentWriterClearsMismatchedIdsAndRefusesUnknownPrograms) {
    DiagnosticTargets forward;
    GBufferDiagnosticCapture capture(targets_, table_, &forward);
    auto* base = pass_.instanced_static_mesh_shader().get();
    const auto empty = table_.json();
    capture.write_current(Submission());
    EXPECT_EQ(table_.json(), empty);
    ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
    auto* shader = capture.variant(base, false);
    ASSERT_NE(shader, base);
    shader->use();
    capture.write_current(Submission());
    DiagnosticId identity{};
    const auto location = glGetUniformLocation(shader->Id(), "u_diagnosticIds");
    glGetUniformuiv(shader->Id(), location, identity.data());
    EXPECT_NE(identity, DiagnosticId{});
    capture.detach();
    ASSERT_TRUE(
        capture.attach_forward(pass_.gbuffer().fbo_id, pass_.gbuffer().position_texture, {32, 32}));
    // The same cached program retains a previous ID until the refused write clears it.
    EXPECT_NO_THROW(capture.write_current(Submission()));
    glGetUniformuiv(shader->Id(), location, identity.data());
    EXPECT_EQ(identity, DiagnosticId{});
    EXPECT_FALSE(capture.complete());
    auto submission = Submission();
    submission.pass = DiagnosticPass::Forward;
    capture.write_current(submission);
    glGetUniformuiv(shader->Id(), location, identity.data());
    EXPECT_EQ(identity[3], 513u);
    base->use();
    capture.write_current(submission);
    capture.detach();
    const auto document = nlohmann::json::parse(table_.json());
    EXPECT_EQ(document.at("draws").back().at("shader_variant"),
              "diagnostic_writer_without_variant");
    EXPECT_EQ(document.at("draws").back().at("pass"), DiagnosticPass::Forward);
    EXPECT_FALSE(capture.complete());
    EXPECT_TRUE(forward.reset());
    glUseProgram(0);
}

TEST_F(DiagnosticAttributionGl, ForwardAttachmentRefusalsLeaveNoLeaseOrCallerStateChange) {
    DiagnosticTargets forward;
    EXPECT_THROW((GBufferDiagnosticCapture(targets_, table_, &targets_)), std::invalid_argument);
    // Each refused case starts with a fresh scope and draw table.
    for (unsigned failure = 0; failure < 7; ++failure) {
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_, failure == 0 ? nullptr : &forward);
        if (failure != 1) {
            ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
            if (failure != 2) {
                capture.detach();
            }
        }
        const auto framebuffer = pass_.gbuffer().fbo_id;
        const auto texture = pass_.gbuffer().position_texture;
        if (failure == 3) {
            ASSERT_TRUE(capture.attach_forward(framebuffer, texture, {32, 32}));
            capture.detach();
        }
        if (failure == 6) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(
                GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, texture, 0);
        }
        SetEntryState(false);
        EXPECT_FALSE(capture.attach_forward(failure == 4 ? 0 : framebuffer,
                                            texture,
                                            failure == 5 ? DiagnosticExtent{16, 32}
                                                         : DiagnosticExtent{32, 32}));
        ExpectEntryState(false);
        EXPECT_FALSE(capture.complete());
        capture.detach();
        EXPECT_TRUE(forward.reset());
        if (failure == 6) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
        }
    }
}

TEST_F(DiagnosticAttributionGl, FeedbackSignatureBoundsRefuseBeforeCompilation) {
    for (const auto& names : {std::vector<std::string>(4, "gl_Position"),
                              std::vector<std::string>{std::string(65, 'x')}}) {
        table_.clear();
        GBufferDiagnosticCapture capture(targets_, table_);
        ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
        auto* base = pass_.instanced_static_mesh_shader().get();
        EXPECT_EQ(capture.variant(base, false, names), base);
        const auto document = nlohmann::json::parse(table_.json());
        EXPECT_EQ(document.at("draws").back().at("shader_variant"),
                  "diagnostic_shader_variant_limit");
        capture.detach();
        EXPECT_FALSE(capture.complete());
    }
}

TEST_F(DiagnosticAttributionGl,
       ForwardAttachmentScopeReleasesDuringExceptionAndCaptureDestruction) {
    DiagnosticTargets forward;
    for (const bool exceptional : {false, true}) {
        table_.clear();
        {
            GBufferDiagnosticCapture capture(targets_, table_, &forward);
            ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
            capture.detach();
            SetEntryState(true);
            if (exceptional) {
                EXPECT_THROW(
                    {
                        GBufferDiagnosticAttachment attachment(
                            pass_.gbuffer().fbo_id, pass_.gbuffer().position_texture, {32, 32});
                        EXPECT_FALSE(forward.reset());
                        throw std::runtime_error("contributor failure");
                    },
                    std::runtime_error);
                EXPECT_FALSE(capture.complete());
                EXPECT_TRUE(forward.reset());
            } else {
                ASSERT_TRUE(capture.attach_forward(
                    pass_.gbuffer().fbo_id, pass_.gbuffer().position_texture, {32, 32}));
                EXPECT_FALSE(forward.reset());
            }
        }
        ExpectEntryState(true);
        EXPECT_TRUE(forward.reset());
    }
}

TEST(DiagnosticAttribution, GeneratedMeshRegistrationValidatesBeforeMutatingTheRegistry) {
    const auto table = Read("src/luminumbra_client/rendering/DiagnosticDrawTable.h");
    Contains(table, "std::string_view generated_mesh{}");
    Contains(table, "submission.generated_mesh.size() > 1024");
    Contains(table, "!submission.mesh_path.empty() || !valid_path(submission.generated_mesh)");
    Contains(table, "submission.mesh_path.empty() && submission.generated_mesh.empty()");
    Contains(table, "submission.generated_mesh.empty() ? \":asset:\" : \":generated:\"");
    Contains(table, "mesh[\"generated\"] = submission.generated_mesh");
    Before(table, "mesh_json = mesh.dump()", "mesh_ids_.emplace(");
    Before(table, "added > max_json_bytes - json_bytes_", "mesh_ids_.emplace(");
}

TEST_F(DiagnosticAttributionGl, FeedbackSignaturesSelectDistinctCachedPrograms) {
    DiagnosticTargets forward;
    GBufferDiagnosticCapture capture(targets_, table_, &forward);
    ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
    auto* base = pass_.instanced_static_mesh_shader().get();
    auto* plain = capture.variant(base, false);
    auto* feedback = capture.variant(base, false, {"gl_Position"});
    ASSERT_NE(plain, base);
    ASSERT_NE(feedback, base);
    EXPECT_NE(plain, feedback);
    EXPECT_EQ(capture.variant(base, false, {"gl_Position"}), feedback);
    EXPECT_EQ(capture.variant(base, false), plain);
    capture.detach();
    ASSERT_TRUE(
        capture.attach_forward(pass_.gbuffer().fbo_id, pass_.gbuffer().position_texture, {32, 32}));
    EXPECT_EQ(capture.variant(base, false, {"missing_output"}), base);
    const auto document = nlohmann::json::parse(table_.json());
    EXPECT_EQ(document.at("draws").back().at("shader_variant"),
              "diagnostic_shader_variant_unavailable");
    EXPECT_EQ(document.at("draws").back().at("pass"), DiagnosticPass::Forward);
    EXPECT_EQ(capture.variant(nullptr, false), nullptr);
    const auto unavailable = nlohmann::json::parse(table_.json());
    EXPECT_EQ(unavailable.at("draws").back().at("shader_variant"),
              "diagnostic_base_shader_unavailable");
    EXPECT_EQ(unavailable.at("draws").back().at("pass"), DiagnosticPass::Forward);
    capture.detach();
    EXPECT_FALSE(capture.complete());
}

TEST_F(GBufferLeaseGl, ForwardRollbackRetainsSelectedLeaseUntilCallerRestoration) {
    DiagnosticTargets forward;
    GBufferDiagnosticCapture capture(targets_, table_, &forward);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    capture.detach();
    AttachmentProbe probe(forward, gbuffer_.fbo_id);
    probe.incomplete = true;
    EXPECT_FALSE(capture.attach_forward(gbuffer_.fbo_id, gbuffer_.position_texture, {32, 32}));
    EXPECT_TRUE(capture.attachment_failed());
    EXPECT_EQ(probe.detaches, 1u);
    EXPECT_FALSE(probe.released_before_detach);
    Contains(table_.json(), "diagnostic_mrt_incomplete");
    ExpectCallerBindingsAndMask();
    ExpectDetached();
    EXPECT_TRUE(forward.reset());
}

TEST_F(GBufferLeaseGl, ForwardRegistryRetirementReleasesSelectedLeaseAndKeepsFailureSticky) {
    DiagnosticTargets forward;
    GBufferDiagnosticCapture capture(targets_, table_, &forward);
    ASSERT_TRUE(capture.attach(gbuffer_, {32, 32}));
    capture.detach();
    ASSERT_TRUE(capture.attach_forward(gbuffer_.fbo_id, gbuffer_.position_texture, {32, 32}));
    AttachmentProbe probe(forward, gbuffer_.fbo_id);
    registry_.destroy_owned("gbuffer_fbo");
    capture.detach();
    EXPECT_EQ(probe.detaches, 0u);
    EXPECT_EQ(probe.buffer_writes, 0u);
    EXPECT_TRUE(capture.attachment_failed());
    Contains(table_.json(), "gbuffer_retired_during_capture");
    ExpectCallerBindingsAndMask();
    EXPECT_TRUE(forward.reset());
    EXPECT_EQ(nlohmann::json::parse(table_.json()).at("draws").back().at("pass"),
              DiagnosticPass::Forward);
    table_.clear();
    EXPECT_FALSE(capture.complete());
}
