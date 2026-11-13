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

std::string WithoutDiagnosticBlocks(std::string source, std::size_t expected_blocks = 2) {
    constexpr std::string_view directive = "#ifdef LUMIN_DIAG_IDS\n";
    EXPECT_EQ(Count(source, directive), expected_blocks);
    for (std::size_t block = 0; block < expected_blocks; ++block) {
        auto start = source.find(directive);
        if (start == std::string::npos) {
            return {};
        }
        const auto end = source.find("#endif\n", start);
        if (end == std::string::npos) {
            return {};
        }
        const auto length = end + std::string_view("#endif\n").size() - start;
        // Identity blocks introduce a separator; the guarded color location is
        // adjacent to the original color declaration and adds no separator.
        const auto color_location = source.find("layout (location = 0)\n", start);
        if (color_location >= end && start >= 2 && source.substr(start - 2, 2) == "\n\n") {
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
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/g_buffer.frag"))),
              0x73515e1c73e4a456ull);
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/foliage.frag"), 3)),
              0x4d6a41fb26226717ull);
    EXPECT_EQ(SourceHash(WithoutDiagnosticBlocks(Read("res/shaders/tree_impostor.frag"))),
              0x4b9944312066e645ull);
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
    for (const auto* path : {"res/shaders/g_buffer.frag",
                             "res/shaders/foliage.frag",
                             "res/shaders/tree_impostor.frag"}) {
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
    EXPECT_EQ(Count(pass, "shader = capture->variant(shader,"), 3u);
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
    Contains(attach, "state_->framebuffer != 0 ||");
    Contains(attach, "forward ? !state_->captured || state_->forward_captured");
    Contains(attach, ": state_->captured");
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
    Contains(attach, "forward ? *state_->forward_targets : state_->targets");
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
    const auto pipeline = Read("src/luminumbra_client/rendering/RenderPipeline.cpp");
    Contains(pipeline, "terrain_resident_lod_unavailable");
    Contains(pipeline, "terrain_identity_draw_limit");
    Contains(pass, "ladder_backend_identity_unavailable");
    Contains(pass, "authored_static_surface");
    Contains(pass, "DiagnosticProvenance::Unsupported");
    const auto targets = (Read(kTargets) + Read(kPass));
    Contains(targets, "Diagnostic capture scopes cannot nest");
    Contains(targets, "Diagnostic capture requires a fresh draw table");
    Contains(targets, "diagnostic_shader_variant_limit");
    Contains(targets, "diagnostic_shader_variant_unavailable");
    Contains(targets, "std::uncaught_exceptions() > exceptions_");
    Contains(Body(targets, "void GBufferDiagnosticCapture::abort()"), "state_->failed = true;");
}

TEST(DiagnosticAttribution, TerrainSubmitsOriginalCommandsInBucketOrderWithResidentMetadata) {
    const auto pipeline = Read("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto draw = Body(pipeline, "void RenderPipeline::draw_chunks_mdi(");
    Contains(draw, "diagnostic_capture && !diagnostic_capture->deferred_attached()");
    Contains(draw, "if (running > 65536)");
    EXPECT_LT(draw.find("if (running > 65536)"), draw.find("diagnostic_chunks.resize(running)"));
    Contains(draw, "diagnostic_chunks[dst] = chunk");
    Contains(draw, "for (std::size_t draw = first; draw < first + count; ++draw)");
    Contains(draw, ".chunk = {chunk.coords.x, chunk.coords.y, chunk.coords.z}");
    Contains(draw, ".lod = static_cast<std::uint32_t>(resident.diagnostic_lod)");
    Contains(draw, ".ownership = DiagnosticOwnership::Live");
    Contains(draw, "? \"terrain_mdi_ladder\"");
    Contains(draw, "std::to_string(resident.mesh_version)");
    Contains(draw, "resident.diagnostic_lod < 0 || resident.diagnostic_lod > 255");
    Contains(draw, "glDrawElementsIndirect,");
    Contains(draw, "draw * sizeof(DrawElementsIndirectCommand)");
    EXPECT_LT(draw.find("diagnostic_capture->write_current("),
              draw.find("glDrawElementsIndirect,"));
    Contains(draw, "} else {\n            Luminumbra::Rendering::AttributedGl::Submit(");
    EXPECT_EQ(Count(draw, "glMultiDrawElementsIndirect,"), 1u);
    const auto copy = Body(pipeline, "bool RenderPipeline::copy_terrain_mesh_payload(");
    Contains(copy, "payload.diagnostic_lod = chunk.source_chunk->current_lod.load(");
    EXPECT_LT(copy.find("payload.diagnostic_lod"), copy.find("const u32 version_after"));
    const auto upload = Body(pipeline, "void RenderPipeline::upload_chunk_mesh(");
    Contains(upload, "render_data.diagnostic_lod = payload.diagnostic_lod");
    EXPECT_LT(upload.find("if (new_handle == ChunkRenderData::kInvalidPoolHandle)"),
              upload.find("render_data.diagnostic_lod ="));
    const auto chunks = Body(Read(kPass), "void GBufferPass::geometry_pass_chunks(");
    EXPECT_EQ(chunks.find("live_terrain_mdi_unsupported"), std::string::npos);
}

TEST(DiagnosticAttribution, ImpostorsWriteLodThreeOncePerOriginalInstanceAfterAlphaCutout) {
    const auto pass = Body(Read(kPass), "void GBufferPass::geometry_pass_static_meshes(");
    Contains(pass, "impostor_shader = capture->variant(impostor_shader, false)");
    Contains(pass, "for (GLsizei instance = 0; instance < count; ++instance)");
    Contains(pass, "capture->write(*impostor_shader,");
    Contains(pass, ".shader_variant = \"tree_impostor\"");
    Contains(pass, ".generated_mesh = \"tree-small-02/octahedral-atlas\"");
    Contains(pass, ".lod = 3");
    Contains(pass, "glDrawArraysInstancedBaseInstance,");
    Contains(pass, "GLenum{GL_TRIANGLE_STRIP}");
    Contains(pass, "static_cast<GLuint>(instance)");
    EXPECT_LT(pass.find("capture->write(*impostor_shader,"),
              pass.find("glDrawArraysInstancedBaseInstance,"));
    EXPECT_EQ(pass.find("tree_impostor_fragment_output_unavailable"), std::string::npos);
    const auto fragment = Body(Read("res/shaders/tree_impostor.frag"), "void main()");
    EXPECT_LT(fragment.find("if (alb.a < 0.5) discard;"),
              fragment.find("gDiagnosticIds = u_diagnosticIds;"));
}

TEST(DiagnosticAttribution, FoliageGpuAndCpuUseTheForwardVariantAndOriginalSubmissions) {
    const auto foliage = Read("src/luminumbra_client/rendering/passes/FoliagePass.cpp");
    const auto execute = Body(foliage, "std::size_t FoliagePass::execute(");
    Contains(execute, "ctx.lit_scene.id, ctx.lit_scene_color.id");
    Contains(execute, "if (auto* capture = GBufferDiagnosticCapture::Current())");
    Contains(execute, "draw_shader = capture->variant(");
    Contains(execute, "{\"VS_OUT.worldPos\", \"VS_OUT.heightT\", \"gl_Position\"}");
    Contains(execute, "CacheFoliageDrawUniforms<DrawUniforms>(draw_shader->Id())");
    Contains(execute, "ReplayRollback restore_uniforms(");
    Contains(execute, "m_draw_uniforms = default_uniforms");
    Contains(execute, "if (draw_shader != m_shader.get())");
    Contains(execute, "WriteFoliageIdentity(\"foliage_gpu\")");
    Contains(execute, "WriteFoliageIdentity(\"foliage_cpu\")");
    EXPECT_LT(execute.find("WriteFoliageIdentity(\"foliage_gpu\")"),
              execute.find("glDrawArraysIndirect,"));
    EXPECT_LT(execute.find("WriteFoliageIdentity(\"foliage_cpu\")"),
              execute.find("glDrawArraysInstanced,"));
    EXPECT_EQ(Count(execute, "glDrawArraysIndirect,"), 1u);
    EXPECT_EQ(Count(execute, "glDrawArraysInstanced,"), 1u);
    const auto writer = Body(foliage, "void WriteFoliageIdentity(");
    Contains(writer, ".pass = DiagnosticPass::Forward");
    Contains(writer, ".lod = 0");
    Contains(writer, ".ownership = DiagnosticOwnership::Live");
    Contains(writer, ".generated_mesh = \"foliage/crossed-cards\"");
    Contains(Read("res/shaders/foliage.frag"),
             "#ifdef LUMIN_DIAG_IDS\nlayout (location = 0)\n#endif\nout vec4 FragColor;");
}

TEST(DiagnosticAttribution, EachFoliageQualificationPartitionGetsItsOwnOrderedIdentity) {
    const auto partitions = Body(Read("src/luminumbra_client/rendering/passes/FoliagePass.cpp"),
                                 "bool FoliagePass::capture_vertices(");
    Contains(partitions,
             "if (first) {\n        WriteFoliageIdentity(\"foliage_qualification_prefix\")");
    Contains(partitions, "WriteFoliageIdentity(\"foliage_qualification_sample\")");
    Contains(partitions, "if (after < m_instances.size()) {\n        WriteFoliageIdentity(");
    Contains(partitions, "\"foliage_qualification_suffix\"");
    EXPECT_EQ(Count(partitions, "WriteFoliageIdentity("), 3u);
    EXPECT_EQ(Count(partitions, "glDrawArraysInstancedBaseInstance,"), 3u);
    EXPECT_LT(partitions.find("foliage_qualification_prefix"),
              partitions.find("foliage_qualification_sample"));
    EXPECT_LT(partitions.find("foliage_qualification_sample"),
              partitions.find("glBeginTransformFeedback(GL_TRIANGLES)"));
    EXPECT_LT(partitions.find("glEndTransformFeedback()"),
              partitions.find("foliage_qualification_suffix"));
    Contains(partitions, "static_cast<GLuint>(after)");
}

TEST(DiagnosticAttribution, ForwardAttachmentIsDistinctTransactionalAndRefusesMissingStorage) {
    const auto targets = Read(kTargets) + Read(kPass);
    Contains(targets, "forward_targets == &targets");
    Contains(targets, "Forward identities require distinct storage");
    const auto forward = Body(targets, "bool GBufferDiagnosticCapture::attach_forward(");
    Contains(forward, "if (!state_->forward_targets)");
    Contains(forward, "foliage_forward_contributor_target_unavailable");
    Contains(forward, "attach_target(target, extent, true)");
    Contains(forward, "unsupported(reason, 0, DiagnosticOwnership::Live, DiagnosticPass::Forward)");
    Contains(Body(targets, "void GBufferDiagnosticCapture::unsupported("), ".pass = pass");
    const auto attach = Body(targets, "bool GBufferDiagnosticCapture::attach_target(");
    Contains(attach, "forward ? !state_->captured || state_->forward_captured");
    Contains(attach, "forward ? *state_->forward_targets : state_->targets");
    Contains(attach, "auto forward_buffers = state_->buffers");
    Contains(attach, "forward_buffers[6] = GL_COLOR_ATTACHMENT6");
    Contains(attach, "glDrawBuffers(state_->buffer_count, forward_buffers.data())");
    Contains(attach, "state_->forward_captured = true");
    const auto write = Body(targets, "void GBufferDiagnosticCapture::write(");
    Contains(write, "state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer");
    Contains(write, "diagnostic_contributor_pass_mismatch");
    const auto current = Body(targets, "void GBufferDiagnosticCapture::write_current(");
    Contains(current, "if (!state_->framebuffer)");
    EXPECT_LT(current.find("if (!state_->framebuffer)"), current.find("glGetIntegerv("));
    Contains(current, "diagnostic_writer_without_variant");
    const auto variant = Body(targets, "Shader* GBufferDiagnosticCapture::variant(");
    Contains(variant, "state_->variants.size() == 8");
    Contains(variant, "feedback_varyings.size() > 3");
    Contains(variant, "name.size() > 64");
    Contains(variant, "entry.feedback_varyings == feedback_varyings");
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
