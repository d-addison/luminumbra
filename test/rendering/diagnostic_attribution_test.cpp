#include "core/IsolationConfig.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "rendering/Camera.h"
#include "rendering/DiagnosticDrawTable.h"
#include "rendering/GBufferTargets.h"
#include "rendering/Mesh.h"
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
    const auto first_position = source.find(first);
    const auto second_position = source.find(second);
    ASSERT_NE(first_position, std::string::npos) << first;
    ASSERT_NE(second_position, std::string::npos) << second;
    EXPECT_LT(first_position, second_position);
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
} // namespace

TEST(DiagnosticAttribution, DefaultShaderTextIsUnchangedWithoutTheVariant) {
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/g_buffer.frag"))),
              0x4587e216df428316ull);
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/foliage.frag"))),
              0xd5876c01db56f7ecull);
    const auto create = Body((Read(kTargets) + Read(kPass)), "void CreateGBufferTargets(");
    Contains(create, "GL_COLOR_ATTACHMENT4");
    Contains(create, "if (authored)");
    EXPECT_EQ(create.find("GL_COLOR_ATTACHMENT6"), std::string::npos);
    EXPECT_EQ(create.find("Diagnostic"), std::string::npos);
}

TEST(DiagnosticAttribution, DisabledScopeDoesNotAllocateTargetsOrCompileVariants) {
    const auto targets = (Read(kTargets) + Read(kPass));
    EXPECT_EQ(Count(Read(kPass), "if (capture) {\n        shader = capture->variant("), 3u);
    Contains(targets, "static thread_local GBufferDiagnosticCapture* capture = nullptr;");
    const auto scope = Body(targets, "GBufferDiagnosticAttachment::GBufferDiagnosticAttachment(");
    Contains(scope, "if (capture_ && !capture_->attach(gbuffer, extent))");
    const auto constructor = Body(targets, "GBufferDiagnosticCapture::GBufferDiagnosticCapture(");
    Contains(constructor, "SetCurrent(this);");
    Contains(Body(targets, "GBufferDiagnosticCapture::~GBufferDiagnosticCapture()"),
             "SetCurrent(nullptr);");
    EXPECT_EQ(constructor.find("gl"), std::string::npos);
    EXPECT_EQ(constructor.find("ensure("), std::string::npos);
    EXPECT_EQ(constructor.find("make_unique<Shader>"), std::string::npos);
    const auto variant = Body(targets, "Shader* GBufferDiagnosticCapture::variant(");
    Contains(variant, "if (!state_->framebuffer || !shader || !shader->IsValid())");
    Contains(variant, "LUMIN_DIAG_IDS 1");
    Contains(variant, "LUMINUMBRA_FAR_WORLD_LADDER 1");
    EXPECT_LT(variant.find("if (!state_->framebuffer"), variant.find("make_unique<Shader>"));
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
    Contains(pass, "GBufferDiagnosticAttachment diagnostic_attachment(");
    Contains(pass, "? \"instanced_static_mesh_ladder\"");
    Contains(pass, "? \"skinned_mesh_ladder\"");
    Contains(pass, "std::numeric_limits<GLsizei>::max()");
    Contains(pass, "diagnostic_static_mesh_index_count");
    ASSERT_NE(pass.find("glDrawElementsInstancedBaseInstance"), std::string::npos);
    EXPECT_LT(pass.find("diagnostic_static_mesh_index_count"),
              pass.find("Submit(glDrawElementsInstancedBaseInstance"));
    const auto write =
        Body((Read(kTargets) + Read(kPass)), "void GBufferDiagnosticCapture::write(");
    Contains(write, "state_->table.submit(submission, id)");
    Contains(write, "glProgramUniform4uiv(shader.Id(), entry.identity, 1, id.data())");
    EXPECT_LT(write.find("state_->table.submit"), write.find("glProgramUniform4uiv"));
    Contains(write, "state_->refused = true;");
    Contains(write, "!state_->refused && !state_->table.submit");
    EXPECT_EQ(write.find("throw"), std::string::npos);
    Contains(write, "diagnostic_writer_without_variant");
    EXPECT_EQ(Read(kTargets).find("state_->table.submit"), std::string::npos);
    EXPECT_EQ(Read(kTargets).find("DiagnosticDrawTable.h"), std::string::npos);
}

TEST(DiagnosticAttribution, RefusalNeverThrowsAndEveryPassExitRestoresCallerBindings) {
    const auto source = Read(kPass);
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
    Contains(attach, "state_->framebuffer != 0 || state_->captured");
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
    Contains(complete, "state_->captured && !state_->framebuffer && !state_->failed");
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
    Contains(attach, "state_->targets.ensure(true, extent, 7)");
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
    Contains(targets, "std::uncaught_exceptions() > exceptions_");
    Contains(Body(targets, "void GBufferDiagnosticCapture::abort()"), "state_->failed = true;");
}

TEST(DiagnosticAttribution, AttachmentStorageIsEnsuredBeforeLeaseAndLeaseBeforeAttachment) {
    const auto header = Read("src/luminumbra_client/rendering/GBufferTargets.h");
    Contains(header, "DiagnosticTargets::AttachmentLease attachment_lease;");
    const auto attach = Body(Read(kTargets), "bool GBufferDiagnosticCapture::attach_target(");
    Before(attach,
           "state_->targets.ensure(true, extent, 7)",
           "state_->attachment_lease = state_->targets.acquire_attachment_lease()");
    Before(attach,
           "state_->attachment_lease = state_->targets.acquire_attachment_lease()",
           "if (!state_->attachment_lease)");
    Before(attach, "if (!state_->attachment_lease)", "glFramebufferTexture2D(");
    Contains(attach, "state_->attachment_lease.texture()");
    Contains(attach, "diagnostic_attachment_lease_unavailable");
    EXPECT_EQ(attach.find("attachment_lease.reset()"), std::string::npos);
}

TEST(DiagnosticAttribution, ResizeAndRegistryTeardownDetachBeforeReleasingTheAttachmentLease) {
    const auto targets = Read(kTargets);
    const auto detach = Body(targets, "void GBufferDiagnosticCapture::detach_target()");
    Before(detach, "GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0", "attachment_lease.reset()");
    Before(detach,
           "glDrawBuffers(state_->buffer_count, state_->buffers.data())",
           "attachment_lease.reset()");
    Before(detach, "static_cast<GLuint>(saved_framebuffer)", "attachment_lease.reset()");
    EXPECT_EQ(Count(targets + Read(kPass), "attachment_lease.reset()"), 1u);
    const auto retire = Body(targets, "void GBufferDiagnosticCapture::before_destroy(");
    Contains(retire, "state_->framebuffer && state_->framebuffer == gbuffer.fbo_id");
    Before(retire, "detach_target();", "state_->failed = true;");
    const auto destroy = Body(targets, "void DestroyGBufferTargets(");
    Before(destroy, "capture->before_destroy(gbuffer)", "registry.destroy_owned(");
    const auto pass = Read(kPass);
    Contains(Body(pass, "void GBufferPass::destroy_gbuffer("),
             "DestroyGBufferTargets(m_gbuffer, registry)");
    const auto execute = Body(pass, "GBufferDrawStats GBufferPass::execute(");
    Before(execute, "DestroyGBufferTargets(m_gbuffer, *ctx.registry)", "CreateGBufferTargets(");
    Before(execute,
           "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)",
           "GBufferDiagnosticAttachment diagnostic_attachment(");
    Before(execute, "GBufferDiagnosticAttachment diagnostic_attachment(", "geometry_pass_chunks(");
    Before(execute,
           "GBufferDiagnosticAttachment diagnostic_attachment(",
           "geometry_pass_static_meshes(");
    Before(execute,
           "GBufferDiagnosticAttachment diagnostic_attachment(",
           "geometry_pass_skinned_meshes(");
    // Authored preview is the other caller of the same shared lifecycle.
    const auto preview = Read("src/luminumbra_client/rendering/static_preview/StaticRenderer.cpp");
    Before(preview,
           "DestroyGBufferTargets(gbuffer, resources)",
           "CreateGBufferTargets(gbuffer, resources, w, h, true)");
}

TEST(DiagnosticAttribution, UnboundGBufferLeaseRefusesTargetResizeAndPreservesStorageAndBindings) {
    const auto targets = Read(kTargets);
    const auto attach = Body(targets, "bool GBufferDiagnosticCapture::attach_target(");
    Before(attach, "state_->targets.acquire_attachment_lease()", "glFramebufferTexture2D(");
    // Restoring the caller binding must not drop ownership of the now-unbound G-buffer.
    Before(attach, "state_->framebuffer = gbuffer.fbo_id;", "state_->captured = true;");
    EXPECT_EQ(attach.find("attachment_lease.reset()"), std::string::npos);
    const auto lifecycle = Read("src/luminumbra_client/rendering/DiagnosticTargets.h");
    const auto ensure = Body(lifecycle, "bool ensure(");
    Before(ensure, "if (leases_ != 0)", "BindingGuard guard;");
    Before(ensure, "if (leases_ != 0)", "glGenTextures(");
    Before(ensure, "if (leases_ != 0)", "destroy(texture_, framebuffer_)");
    Contains(Body(ensure, "if (leases_ != 0)"), "return false;");
    const auto reset = Body(lifecycle, "[[nodiscard]] bool reset()");
    Before(reset, "if (leases_ != 0)", "destroy(texture_, framebuffer_)");
    // The existing modeled-driver regression also checks handles, extents, all
    // caller bindings and the attachment after refusal on an unbound framebuffer.
    const auto tests = Read("test/rendering/diagnostic_capture_test.cpp");
    Contains(tests, "UnboundCallerAttachmentLeaseRefusesResizeAndReleaseEnablesResize");
}

TEST(DiagnosticAttribution, IncompleteRollbackAndExceptionTeardownDetachBeforeLeaseRelease) {
    const auto targets = Read(kTargets);
    const auto attach = Body(targets, "bool GBufferDiagnosticCapture::attach_target(");
    const auto rollback = Body(attach, "if (!complete)");
    Before(rollback, "detach_target();", "diagnostic_mrt_incomplete");
    const auto pass = Read(kPass);
    const auto scope = Body(pass, "GBufferDiagnosticAttachment::~GBufferDiagnosticAttachment()");
    Before(scope, "capture_->detach();", "std::uncaught_exceptions() > exceptions_");
    Contains(scope, "capture_->abort();");
    const auto destructor = Body(pass, "GBufferDiagnosticCapture::~GBufferDiagnosticCapture()");
    Before(destructor, "detach();", "SetCurrent(nullptr)");
    Contains(destructor, "catch (...)");
    const auto detach = Body(pass, "void GBufferDiagnosticCapture::detach()");
    Before(detach, "detach_target();", "unsupported(reason)");
    const auto release = Body(targets, "void GBufferDiagnosticCapture::detach_target()");
    Before(release, "GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0", "attachment_lease.reset()");
    const auto complete = Body(pass, "bool GBufferDiagnosticCapture::complete()");
    Contains(complete, "!state_->attachment_lease");
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
        pass_.init_gbuffer(resources_, 32, 32);
        ASSERT_TRUE(targets_.ensure(true, {32, 32}, 7));
        ASSERT_TRUE(pass_.geometry_shader()->IsValid());
        ASSERT_TRUE(pass_.instanced_static_mesh_shader()->IsValid());
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
} // namespace

TEST_F(DiagnosticAttributionGl, WritersRequireAttachmentAndCompletionRequiresLeaseRelease) {
    GBufferDiagnosticCapture capture(targets_, table_);
    auto* base = pass_.instanced_static_mesh_shader().get();
    EXPECT_EQ(capture.variant(base, false), base);
    const auto empty = table_.json();
    capture.write(*base, Submission());
    EXPECT_EQ(table_.json(), empty);
    EXPECT_FALSE(capture.complete());

    SetEntryState(false);
    ASSERT_TRUE(capture.attach(pass_.gbuffer(), {32, 32}));
    ExpectEntryState(false);
    auto* shader = capture.variant(base, false);
    ASSERT_NE(shader, base);
    capture.write(*shader, Submission());
    EXPECT_TRUE(table_.complete_coverage());
    EXPECT_FALSE(capture.complete());
    const auto texture = targets_.texture();
    EXPECT_FALSE(targets_.ensure(true, {16, 16}, 7));
    EXPECT_FALSE(targets_.reset());
    EXPECT_FALSE(targets_.ensure(false, {}));
    EXPECT_EQ(targets_.texture(), texture);
    ExpectEntryState(false);

    capture.detach();
    ExpectEntryState(false);
    EXPECT_TRUE(capture.complete());
    const auto recorded = table_.json();
    EXPECT_EQ(capture.variant(base, false), base);
    capture.write(*shader, Submission());
    EXPECT_EQ(table_.json(), recorded);
    EXPECT_TRUE(targets_.reset());
    EXPECT_FALSE(capture.attach(pass_.gbuffer(), {32, 32}));
    EXPECT_FALSE(capture.complete());
    ExpectEntryState(false);
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
        capture.detach();
        EXPECT_FALSE(capture.complete());
    }
    table_.clear();
    GBufferDiagnosticCapture fresh(targets_, table_);
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
    // In addition to the pass's three snapshots: two MRT capabilities, the
    // texture binding, two draw-FBO bindings, all draw buffers and three target
    // capabilities. SetUp allocated storage, so this measures same-extent reuse.
    const auto attachment_integer_queries = 8u + static_cast<unsigned>(max_buffers);
    for (const bool replay : {false, true}) {
        ctx_.frozen_replay = replay;
        for (const bool cull : {false, true}) {
            GBufferDiagnosticCapture capture(targets_, table_);
            SetEntryState(cull);
            {
                PassStateProbe probe;
                EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
                EXPECT_EQ(probe.integer_queries, 3u + attachment_integer_queries);
                EXPECT_EQ(probe.enabled_queries, 1u);
                EXPECT_EQ(probe.vao_binds, 1u);
                EXPECT_EQ(probe.combined_fbo_binds, 1u);
                // Attach snapshots/restores, attach/restore, detach/restore.
                EXPECT_EQ(probe.draw_fbo_binds, 1u + 6u);
                EXPECT_EQ(probe.read_fbo_binds, 1u);
            }
            ExpectEntryState(cull);
            EXPECT_TRUE(capture.complete());
        }
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
            ExpectEntryState(cull);
            EXPECT_FALSE(capture.complete());
            EXPECT_TRUE(targets_.reset());
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
    EXPECT_TRUE(capture.complete());
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
    capture.detach();
    EXPECT_FALSE(capture.complete());
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
        capture.detach();
        EXPECT_FALSE(capture.complete());
    }
}
