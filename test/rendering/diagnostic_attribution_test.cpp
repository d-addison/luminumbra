#include "rendering/DiagnosticDrawTable.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
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
              0x7c9a01227c8d4b33ull);
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/foliage.frag"))),
              0x4d6a41fb26226717ull);
    const auto create = Body((Read(kTargets) + Read(kPass)), "void CreateGBufferTargets(");
    Contains(create, "GL_COLOR_ATTACHMENT4");
    Contains(create, "if (authored)");
    EXPECT_EQ(create.find("GL_COLOR_ATTACHMENT6"), std::string::npos);
    EXPECT_EQ(create.find("Diagnostic"), std::string::npos);
}

TEST(DiagnosticAttribution, DisabledScopeDoesNotAllocateTargetsOrCompileVariants) {
    const auto targets = (Read(kTargets) + Read(kPass));
    Contains(targets, "static thread_local GBufferDiagnosticCapture* capture = nullptr;");
    const auto scope = Body(targets, "GBufferDiagnosticAttachment::GBufferDiagnosticAttachment(");
    Contains(scope, "if (capture_ && !capture_->attach(gbuffer, extent))");
    const auto constructor = Body(targets, "GBufferDiagnosticCapture::GBufferDiagnosticCapture(");
    Contains(constructor, "SetCurrent(this);");
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
    EXPECT_LT(pass.find("diagnostic_static_mesh_index_count"),
              pass.find("Submit(glDrawElementsInstancedBaseInstance"));
    const auto write =
        Body((Read(kTargets) + Read(kPass)), "void GBufferDiagnosticCapture::write(");
    Contains(write, "state_->table.submit(submission, id)");
    Contains(write, "glProgramUniform4uiv(shader.Id(), entry.identity, 1, id.data())");
    EXPECT_LT(write.find("state_->table.submit"), write.find("glProgramUniform4uiv"));
    Contains(write, "diagnostic_identity_refused");
    Contains(write, "throw std::runtime_error");
    Contains(write, "diagnostic_writer_without_variant");
    EXPECT_EQ(Read(kTargets).find("state_->table.submit"), std::string::npos);
    EXPECT_EQ(Read(kTargets).find("DiagnosticDrawTable.h"), std::string::npos);
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
    Contains(attach, "diagnostic_integer_target_unsupported");
    Contains(attach, "diagnostic_attachment_occupied");
    Contains(attach, "diagnostic_mrt_incomplete");
    Contains(Body(targets, "bool GBufferDiagnosticCapture::attach("), "unsupported(reason)");
    EXPECT_LT(attach.find("attachment_type != GL_NONE"), attach.find("targets.ensure("));
    const auto refusal = Body(targets, "void GBufferDiagnosticCapture::unsupported(");
    Contains(refusal, "state_->failed = true");
    Contains(refusal, ".provenance = DiagnosticProvenance::Unsupported");
    Contains(refusal, "state_->table.submit(");
    Contains(refusal, "throw std::runtime_error");
    const auto complete = Body(targets, "bool GBufferDiagnosticCapture::complete()");
    Contains(complete, "state_->captured && !state_->framebuffer && !state_->failed");
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
    Contains(targets, "std::uncaught_exceptions() > exceptions_");
    Contains(Body(targets, "void GBufferDiagnosticCapture::abort()"), "state_->failed = true;");
}
