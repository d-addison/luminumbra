#include "rendering/DiagnosticDrawTable.h"
#include "rendering/DiagnosticTargets.h"
#include "rendering/GBufferTargets.h"
#include "rendering/RenderContext.h"
#include "rendering/Shader.h"
#include "rendering/passes/DebugViewPass.h"
#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace {
using namespace Luminumbra::Rendering;
using Pass = DebugViewPass;

std::string Source(std::string_view path) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / path);
    EXPECT_TRUE(input.good());
    std::ostringstream result;
    result << input.rdbuf();
    return result.str();
}

std::string SourceSection(std::string_view path, std::string_view first, std::string_view last) {
    const auto source = Source(path);
    const auto begin = source.find(first);
    const auto end = source.find(last, begin);
    EXPECT_NE(begin, std::string::npos);
    EXPECT_NE(end, std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) {
        return {};
    }
    return source.substr(begin, end - begin);
}

class DiagnosticInspectorGl : public ::testing::Test {
protected:
    void SetUp() override {
        initialized_ = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized_);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window_ = glfwCreateWindow(8, 8, "diagnostic_inspector", nullptr, nullptr);
        ASSERT_NE(window_, nullptr);
        glfwMakeContextCurrent(window_);
        ASSERT_NE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)), 0);
    }
    void TearDown() override {
        if (window_) {
            glfwDestroyWindow(window_);
        }
        if (initialized_) {
            glfwTerminate();
        }
    }

private:
    bool initialized_ = false;
    GLFWwindow* window_ = nullptr;
};
} // namespace

TEST(DiagnosticInspector, ProducerMappingIncludesUnknownAndExplicitUnsupported) {
    using Kind = Pass::ProducerKind;
    const std::array<std::pair<std::string_view, Kind>, 9> cases = {{
        {"live_terrain_mdi", Kind::Terrain},
        {"legacy_far_terrain", Kind::Terrain},
        {"tree_impostor", Kind::Impostor},
        {"foliage_ladder", Kind::Foliage},
        {"instanced_static_mesh", Kind::Static},
        {"skinned_mesh_ladder", Kind::Static},
        {"far_volume", Kind::FarVolume},
        {"", Kind::Unsupported},
        {"future_producer", Kind::Unsupported},
    }};
    for (const auto& [variant, expected] : cases) {
        EXPECT_EQ(Pass::producer(variant, true), expected) << variant;
        EXPECT_EQ(Pass::producer(variant, false), Kind::Unsupported) << variant;
    }
}

TEST(DiagnosticInspector, StableHashHasFixedUnsignedArithmeticVectors) {
    EXPECT_EQ(Pass::hash_id(0), 0U);
    EXPECT_EQ(Pass::hash_id(1), 0x9e3779b1U);
    EXPECT_EQ(Pass::hash_id(2), 0x3c6ef362U);
    EXPECT_EQ(Pass::hash_id(std::numeric_limits<std::uint32_t>::max()), 0x61c8864fU);
}

TEST(DiagnosticInspector, JoinRetainsExactDrawMeshChunkLodOwnerPassAndPath) {
    DiagnosticDrawTable table;
    DiagnosticId id{};
    ASSERT_TRUE(table.submit({.mesh_path = "res/models/rock.obj",
                              .shader_variant = "instanced_static_mesh_ladder",
                              .chunk = {-7, 0, 19},
                              .lod = 3,
                              .ownership = DiagnosticOwnership::LadderFar,
                              .pass = DiagnosticPass::GBuffer,
                              .provenance = DiagnosticProvenance::Mesh},
                             id));
    const auto document = nlohmann::json::parse(table.json());
    const auto record = nlohmann::json::parse(Pass::join(table.json(), id));
    ASSERT_FALSE(record.is_null());
    EXPECT_EQ(record.at("mesh").at("path"), "res/models/rock.obj");
    EXPECT_EQ(record.at("draw").at("chunk"), (std::array<int, 3>{-7, 0, 19}));
    EXPECT_EQ(record.at("draw").at("lod"), 3);
    EXPECT_EQ(record.at("draw").at("ownership"), 3);
    EXPECT_EQ(record.at("draw").at("pass"), 1);
    for (std::size_t channel = 0; channel < id.size(); ++channel) {
        auto mismatch = id;
        ++mismatch[channel];
        EXPECT_TRUE(Pass::join(document.dump(), mismatch).empty());
    }
    EXPECT_TRUE(Pass::join(document.dump(), {}).empty());
    EXPECT_TRUE(Pass::join("{}", id).empty());
}

TEST(DiagnosticInspector, UnsupportedRecordRemainsLabelledInTheJoin) {
    DiagnosticDrawTable table;
    DiagnosticId id{};
    ASSERT_TRUE(table.submit({.mesh_path = {},
                              .shader_variant = "tree_impostor_fragment_output_unavailable",
                              .chunk = {},
                              .lod = 3,
                              .ownership = DiagnosticOwnership::Live,
                              .pass = DiagnosticPass::GBuffer,
                              .provenance = DiagnosticProvenance::Unsupported},
                             id));
    const auto record = nlohmann::json::parse(Pass::join(table.json(), id));
    EXPECT_EQ(record.at("draw").at("supported"), false);
    EXPECT_EQ(record.at("mesh").at("path"), "");
    EXPECT_EQ(record.at("mesh").at("provenance"), 4);
}

TEST(DiagnosticInspector, ModesAndZeroUnsupportedHatchAreWiredToTheShaderAndLegend) {
    EXPECT_EQ(Pass::None, 0);
    EXPECT_EQ(Pass::Position, 5);
    EXPECT_EQ(Pass::DrawId, 6);
    EXPECT_EQ(Pass::MeshId, 7);
    EXPECT_EQ(Pass::Lod, 8);
    EXPECT_EQ(Pass::Chunk, 9);
    EXPECT_EQ(Pass::Producer, 10);
    EXPECT_EQ(Pass::MeshPath, 11);
    const auto shader = Source("res/shaders/debug_view.frag");
    for (const auto* text : {"id.x == 0u",
                             "metadata.y == 0u",
                             "unsupportedColor()",
                             "u_mode == 6",
                             "u_mode == 7",
                             "u_mode == 8",
                             "u_mode == 9",
                             "u_mode == 10",
                             "metadata.z == 2u",
                             "metadata.z == 3u"}) {
        EXPECT_NE(shader.find(text), std::string::npos) << text;
    }
    const auto widget = Source("src/luminumbra_client/debug/widgets/RenderControlWidgets.cpp");
    EXPECT_NE(widget.find("Magenta hatch: zero / unresolved / unsupported"), std::string::npos);
    const auto registry = Source("src/luminumbra_client/app/RegisterDeveloperWidgets.cpp");
    EXPECT_NE(registry.find("inspector_open = false"), std::string::npos);
}

TEST(DiagnosticInspector, DisabledAndIdleFramesAreNoOpsWithoutAGraphicsContext) {
    auto& control = Pass::controls();
    control = {};
    Pass pass;
    const RenderContext ctx{};
    control.refresh = true;
    EXPECT_FALSE(pass.begin_frame(ctx));
    EXPECT_TRUE(control.refresh);
    pass.set_mode(Pass::DrawId);
    EXPECT_EQ(pass.mode(), Pass::None);
    pass.execute(ctx);
    pass.set_mode(Pass::None);
    control.enabled = true;
    control.refresh = false;
    EXPECT_FALSE(pass.begin_frame(ctx));
    control.refresh = true;
    RenderContext replay;
    replay.frozen_replay = true;
    EXPECT_FALSE(pass.begin_frame(replay));
    EXPECT_FALSE(pass.begin_frame(ctx));
    EXPECT_EQ(control.status, "Debug shader unavailable");
    control = {};
}

TEST(DiagnosticInspector, SinglePixelGatherHasNoPlaneReadbackAndChecksBoundsBeforeSampling) {
    const auto implementation = Source("src/luminumbra_client/rendering/passes/DebugViewPass.cpp");
    EXPECT_EQ(implementation.find("glReadPixels"), std::string::npos);
    EXPECT_EQ(implementation.find("glGetTexImage"), std::string::npos);
    EXPECT_NE(implementation.find("glViewportIndexedf(0, 0, 0, 9, 9)"), std::string::npos);
    EXPECT_NE(implementation.find(
                  "glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(InspectorPixel)"),
              std::string::npos);
    EXPECT_LT(implementation.find("!control.inspector_open || control.x < 0"),
              implementation.find("read_inspector_pixel(m_snapshot"));
    const auto shader = Source("res/shaders/debug_view.frag");
    for (const auto* text : {"texelFetch(gIdentity, pixel, 0)",
                             "texelFetch(gAlbedo, pixel, 0)",
                             "texelFetch(gNormalMaterial, pixel, 0)",
                             "texelFetch(gDepth, pixel, 0)"}) {
        EXPECT_NE(shader.find(text), std::string::npos) << text;
    }
}

TEST(DiagnosticInspector, IdentityDrawRequiresOwnedInspectorStorage) {
    const auto draw = SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.cpp",
                                    "void DebugViewPass::execute(",
                                    "void DebugViewPass::bind_sources(");
    EXPECT_NE(draw.find("identity_view && (!m_snapshot || m_snapshot->sample_buffer == 0)"),
              std::string::npos);
    EXPECT_EQ(draw.find("inspector_open"), std::string::npos);
    EXPECT_LT(
        draw.find("storage_binding.emplace();"),
        draw.find("glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_snapshot->sample_buffer)"));
    EXPECT_LT(draw.find("glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_snapshot->sample_buffer)"),
              draw.find("AttributedGl::Submit("));
    const auto storage = SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.cpp",
                                       "struct InspectorStorageBinding",
                                       "struct InspectorBindings");
    EXPECT_NE(storage.find("glBindBufferRange(GL_SHADER_STORAGE_BUFFER,"), std::string::npos);
    EXPECT_NE(
        storage.find(
            "glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, static_cast<GLuint>(indexed_storage))"),
        std::string::npos);
    EXPECT_NE(storage.find("glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(storage))"),
              std::string::npos);
}

TEST(DiagnosticInspector, InspectorStorageRefusesMissingPartialAndOversizedStores) {
    const auto begin =
        SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.cpp",
                      "std::shared_ptr<DebugViewPass::Frame> DebugViewPass::begin_frame(",
                      "void DebugViewPass::end_frame(");
    EXPECT_NE(begin.find("glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(InspectorPixel), &empty, "
                         "GL_STREAM_READ)"),
              std::string::npos);
    EXPECT_NE(begin.find("if (m_snapshot->sample_buffer == 0) {\n                control.status"),
              std::string::npos);
    EXPECT_LT(begin.find("if (sample_bytes != sizeof(InspectorPixel))"),
              begin.find("return std::make_shared<Frame>"));
    EXPECT_LT(begin.find("m_snapshot->ready = false;"), begin.find("glGenBuffers("));
    EXPECT_LT(begin.find("if (!m_snapshot->targets.reset())"),
              begin.find("m_snapshot->draws.clear();"));
    EXPECT_NE(begin.find("Identity attachment still leased"), std::string::npos);
    EXPECT_LT(begin.find("if (!control.inspector_open)"), begin.find("inspect_snapshot(ctx)"));
    EXPECT_NE(begin.find("control.inspect_requested = false;"), std::string::npos);
}

TEST(DiagnosticInspector, IncompleteAndUnsupportedCapturesRefusePublication) {
    const auto frame = SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.cpp",
                                     "class DebugViewPass::Frame",
                                     "std::string DebugViewPass::join(");
    EXPECT_LT(frame.find("const bool complete = capture_->complete();"),
              frame.find("capture_.reset();"));
    EXPECT_LT(frame.find("if (!complete)"), frame.find("pass_.finish_frame(ctx);"));
    EXPECT_NE(frame.find("controls().status = controls().result ="), std::string::npos);
    EXPECT_NE(frame.find("Snapshot refused: incomplete or unsupported coverage"),
              std::string::npos);
    EXPECT_LT(frame.find("return;"), frame.find("pass_.finish_frame(ctx);"));
    const auto coverage = SourceSection("src/luminumbra_client/rendering/GBufferTargets.cpp",
                                        "bool GBufferDiagnosticCapture::complete() const",
                                        "Shader* GBufferDiagnosticCapture::variant(");
    EXPECT_NE(
        coverage.find("state_->captured && !state_->framebuffer && !state_->attachment_lease"),
        std::string::npos);
    EXPECT_NE(
        coverage.find("!state_->failed && !state_->refused && state_->table.complete_coverage()"),
        std::string::npos);
}

TEST(DiagnosticInspector, DeferredSnapshotFinishesBeforeUninstrumentedWriters) {
    const auto dispatch = SourceSection("src/luminumbra_client/rendering/RenderPipeline.cpp",
                                        "void RenderPipeline::dispatch_stages(",
                                        "RenderPipeline::stage_executor_table()");
    EXPECT_NE(dispatch.find("if (diagnostic_frame && node == \"gbuffer\")"), std::string::npos);
    EXPECT_LT(dispatch.find("(this->*fn)(camera);"), dispatch.find("DebugViewPass::end_frame("));
    const auto stages = SourceSection("src/luminumbra_client/rendering/RenderPipeline.cpp",
                                      "RenderPipeline::stage_executor_table()",
                                      "void RenderPipeline::execute_stage_shadow(");
    EXPECT_LT(stages.find("{\"gbuffer\","), stages.find("{\"plant_procgen\","));
    EXPECT_LT(stages.find("{\"gbuffer\","), stages.find("{\"ground_decals\","));
    const auto frame = SourceSection(
        "src/luminumbra_client/rendering/passes/DebugViewPass.cpp", "~Frame()", "void finish(");
    EXPECT_EQ(frame.find("finish_frame("), std::string::npos);
    EXPECT_NE(frame.find("Snapshot aborted"), std::string::npos);
}

TEST(DiagnosticInspector, FinalizationResolvesCurrentGBufferAttachments) {
    const auto dispatch = SourceSection("src/luminumbra_client/rendering/RenderPipeline.cpp",
                                        "void RenderPipeline::dispatch_stages(",
                                        "RenderPipeline::stage_executor_table()");
    EXPECT_LT(dispatch.find("(this->*fn)(camera);"),
              dispatch.find(
                  "DebugViewPass::end_frame(diagnostic_frame, make_debug_view_context(camera))"));
    const auto frame = SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.cpp",
                                     "class DebugViewPass::Frame",
                                     "std::string DebugViewPass::join(");
    EXPECT_EQ(frame.find("ctx_"), std::string::npos);
    EXPECT_NE(frame.find("pass_.finish_frame(ctx);"), std::string::npos);
    const auto context = SourceSection("src/luminumbra_client/rendering/RenderPipeline.cpp",
                                       "RenderContext RenderPipeline::make_debug_view_context(",
                                       "RenderContext RenderPipeline::make_ground_decal_context(");
    EXPECT_NE(context.find("const GBuffer& gb = m_gbuffer_pass->gbuffer();"), std::string::npos);
    EXPECT_NE(context.find("gb.position_texture"), std::string::npos);
    EXPECT_NE(context.find("gb.normal_texture"), std::string::npos);
    EXPECT_NE(context.find("gb.albedo_texture"), std::string::npos);
    EXPECT_NE(context.find("gb.depth_texture"), std::string::npos);
}

TEST(DiagnosticInspector, EmptyRepeatedAndAbandonedFramesCannotPublish) {
    const auto end = SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.cpp",
                                   "void DebugViewPass::end_frame(",
                                   "void DebugViewPass::read_inspector_pixel(");
    EXPECT_LT(end.find("if (frame)"), end.find("frame->finish(ctx);"));
    EXPECT_LT(end.find("frame->finish(ctx);"), end.find("frame.reset();"));
    const auto finish = SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.cpp",
                                      "void finish(",
                                      "std::string DebugViewPass::join(");
    EXPECT_LT(finish.find("if (!capture_)"), finish.find("capture_->complete()"));
    EXPECT_LT(finish.find("return;"), finish.find("capture_->complete()"));
    const auto abandoned = SourceSection(
        "src/luminumbra_client/rendering/passes/DebugViewPass.cpp", "~Frame()", "void finish(");
    EXPECT_NE(abandoned.find("if (capture_)"), std::string::npos);
    EXPECT_NE(abandoned.find("controls().status = controls().result = \"Snapshot aborted\""),
              std::string::npos);
    EXPECT_EQ(abandoned.find("finish_frame("), std::string::npos);
}

TEST_F(DiagnosticInspectorGl, FixedReadbackCopiesOneRecordAndRestoresCallerBinding) {
    const Pass::InspectorPixel expected{.id = {1, 2, 3, 259},
                                        .albedo = {0.1f, 0.2f, 0.3f, 17.0f},
                                        .normal_depth = {-1.0f, 0.0f, 0.0f, 0.25f}};
    const std::array<Pass::InspectorPixel, 2> records = {expected, Pass::InspectorPixel{}};
    std::array<GLuint, 2> buffers{};
    glGenBuffers(2, buffers.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(records), records.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
    Pass::InspectorPixel actual;
    Pass::read_inspector_pixel(buffers[0], actual);
    EXPECT_EQ(actual.id, expected.id);
    EXPECT_EQ(actual.albedo, expected.albedo);
    EXPECT_EQ(actual.normal_depth, expected.normal_depth);
    GLint binding = 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &binding);
    EXPECT_EQ(static_cast<GLuint>(binding), buffers[1]);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glDeleteBuffers(2, buffers.data());
}

TEST_F(DiagnosticInspectorGl, ShaderGatherUsesTheRequestedTexelForEveryAov) {
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
    const auto vertex = (root / "fullscreen_tri.vert").string();
    const auto fragment = (root / "debug_view.frag").string();
    Shader normal_shader(vertex.c_str(), fragment.c_str());
    ASSERT_TRUE(normal_shader.IsValid());
    EXPECT_EQ(glGetUniformLocation(normal_shader.Id(), "gIdentity"), -1);
    Shader shader(vertex.c_str(), fragment.c_str(), {}, {"LUMIN_DIAG_VIEWS 1"});
    ASSERT_TRUE(shader.IsValid());
    EXPECT_GE(glGetUniformLocation(shader.Id(), "gIdentity"), 0);

    const std::array<DiagnosticId, 4> ids = {{{1, 1, 0, 257}, {9, 4, 2, 257}, {}, {}}};
    const std::array<std::array<float, 4>, 4> albedo = {
        {{0, 0, 0, 0}, {0.2f, 0.4f, 0.6f, 1}, {}, {}}};
    const std::array<std::array<float, 4>, 4> normal = {
        {{0, 0, 0, 0}, {0.5f, 0.5f, 0, 17.0f / 255}, {}, {}}};
    const std::array<float, 4> depth = {0, 0.25f, 0, 0};
    std::array<GLuint, 5> textures{};
    glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
    const std::array<GLenum, 4> internal_formats = {GL_RGBA32UI, GL_RGBA32F, GL_RGBA32F, GL_R32F};
    const std::array<GLenum, 4> formats = {GL_RGBA_INTEGER, GL_RGBA, GL_RGBA, GL_RED};
    const std::array<const void*, 4> inputs = {
        ids.data(), normal.data(), albedo.data(), depth.data()};
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        glBindTexture(GL_TEXTURE_2D, textures[index]);
        glTexStorage2D(GL_TEXTURE_2D, 1, internal_formats[index], 2, 2);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        2,
                        2,
                        formats[index],
                        index == 0 ? GL_UNSIGNED_INT : GL_FLOAT,
                        inputs[index]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    GLuint framebuffer = 0;
    GLuint buffer = 0;
    GLuint vao = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    glBindTexture(GL_TEXTURE_2D, textures[4]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    glFramebufferTexture2D(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[4], 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(Pass::InspectorPixel), nullptr, GL_STREAM_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, buffer);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const std::array<int, 5> indices = {2, 1, 2, 3, 0};
    for (std::size_t unit = 0; unit < indices.size(); ++unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, textures[static_cast<std::size_t>(indices[unit])]);
    }
    shader.use();
    shader.setInt("gPosition", 0);
    shader.setInt("gNormalMaterial", 1);
    shader.setInt("gAlbedo", 2);
    shader.setInt("gDepth", 3);
    shader.setInt("u_inspect", 1);
    shader.setInt("u_pixelX", 1);
    shader.setInt("u_pixelY", 0);
    glViewport(0, 0, 1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    Pass::InspectorPixel pixel;
    Pass::read_inspector_pixel(buffer, pixel);
    EXPECT_EQ(pixel.id, ids[1]);
    EXPECT_EQ(pixel.albedo, (std::array<float, 4>{0.2f, 0.4f, 0.6f, 17}));
    EXPECT_EQ(pixel.normal_depth, (std::array<float, 4>{0, 0, 1, 0.25f}));
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    GLuint metadata_buffer = 0;
    GLuint metadata_texture = 0;
    glGenBuffers(1, &metadata_buffer);
    glGenTextures(1, &metadata_texture);
    std::array<DiagnosticId, 20> metadata{};
    metadata[18] = ids[1];
    metadata[19] = {7, 4, 1, 0};
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_BUFFER, metadata_texture);
    glBindBuffer(GL_TEXTURE_BUFFER, metadata_buffer);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(metadata), metadata.data(), GL_STATIC_DRAW);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, metadata_buffer);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, ids[1].data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    shader.setInt("u_inspect", 0);
    shader.setInt("u_identityReady", 1);
    const auto check_color = [](const std::array<int, 3>& expected) {
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<unsigned char, 4> color{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            EXPECT_NEAR(color[channel], expected[channel], 1);
        }
    };
    const auto hashed_color = [](std::uint32_t value) {
        const auto hash = Pass::hash_id(value);
        return std::array<int, 3>{static_cast<int>(hash & 255U),
                                  static_cast<int>((hash >> 8U) & 255U),
                                  static_cast<int>((hash >> 16U) & 255U)};
    };
    for (const auto& [mode, value] :
         std::array<std::pair<int, std::uint32_t>, 5>{{{Pass::DrawId, 9},
                                                       {Pass::MeshId, 4},
                                                       {Pass::Lod, 2},
                                                       {Pass::Chunk, 7},
                                                       {Pass::MeshPath, 4}}}) {
        shader.setInt("u_mode", mode);
        check_color(hashed_color(value));
    }
    shader.setInt("u_mode", Pass::Producer);
    const std::array<std::array<int, 3>, 6> producer_colors = {{{255, 0, 255},
                                                                {179, 102, 26},
                                                                {26, 204, 230},
                                                                {51, 204, 51},
                                                                {230, 204, 51},
                                                                {77, 77, 255}}};
    for (std::size_t kind = 0; kind < producer_colors.size(); ++kind) {
        metadata[19][1] = static_cast<std::uint32_t>(kind);
        glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(metadata), metadata.data());
        check_color(producer_colors[kind]);
    }
    metadata[19][1] = 4;
    for (const auto& [owner, color] : std::array<std::pair<std::uint32_t, std::array<int, 3>>, 3>{
             {{1, {230, 204, 51}}, {2, {161, 143, 36}}, {3, {103, 92, 23}}}}) {
        auto owned_id = ids[1];
        owned_id[3] = (owned_id[3] & ~255U) | owner;
        metadata[18] = owned_id;
        glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(metadata), metadata.data());
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, owned_id.data());
        check_color(color);
    }
    metadata[18] = ids[1];
    glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(metadata), metadata.data());
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, ids[1].data());
    shader.setInt("u_mode", Pass::MeshPath);
    for (const auto& [provenance, color] :
         std::array<std::pair<std::uint32_t, std::array<int, 3>>, 4>{
             {{0, {255, 0, 255}}, {2, {255, 102, 13}}, {3, {26, 102, 255}}, {4, {255, 0, 255}}}}) {
        metadata[19][2] = provenance;
        glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(metadata), metadata.data());
        check_color(color);
    }
    const DiagnosticId zero{};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, zero.data());
    shader.setInt("u_mode", Pass::DrawId);
    check_color({255, 0, 255});
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glDeleteTextures(1, &metadata_texture);
    glDeleteBuffers(1, &metadata_buffer);
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
}

TEST(DiagnosticInspector, AllBoundedDrawIdsHaveDistinctStableRgbHashes) {
    std::array<std::uint32_t, 65536> colors{};
    for (std::size_t index = 0; index < colors.size(); ++index) {
        colors[index] = Pass::hash_id(static_cast<std::uint32_t>(index + 1)) & 0xffffffU;
    }
    std::sort(colors.begin(), colors.end());
    EXPECT_EQ(std::adjacent_find(colors.begin(), colors.end()), colors.end());
}

TEST(DiagnosticInspector, JoinRefusesPartialUnknownAndOversizedMetadata) {
    const DiagnosticId id = {1, 1, 0, 257};
    for (const auto* text : {"",
                             "{",
                             "[]",
                             "null",
                             "{\"draws\":{},\"meshes\":[]}",
                             "{\"schema\":1,\"draws\":[{}],\"meshes\":[{}]}",
                             "{\"schema\":2,\"draws\":[{}],\"meshes\":[{}]}"}) {
        EXPECT_TRUE(Pass::join(text, id).empty()) << text;
    }
    const std::string oversized(16 * 1024 * 1024 + 1, ' ');
    EXPECT_TRUE(Pass::join(oversized, id).empty());
    EXPECT_EQ(Pass::producer("foliage_unknown", true), Pass::ProducerKind::Unsupported);
    EXPECT_EQ(Pass::producer("live_terrain_mdi_unsupported", true),
              Pass::ProducerKind::Unsupported);
}

TEST(DiagnosticInspector, EveryNamedIdentityModeIsSharedByPanelAndCommandLine) {
    const std::array<std::pair<std::string_view, int>, 6> cases = {
        {{"draw-id", Pass::DrawId},
         {"mesh-id", Pass::MeshId},
         {"lod", Pass::Lod},
         {"chunk", Pass::Chunk},
         {"producer-owner", Pass::Producer},
         {"mesh-path", Pass::MeshPath}}};
    for (const auto& [name, mode] : cases) {
        EXPECT_EQ(Pass::identity_mode(name), mode);
    }
    EXPECT_EQ(Pass::identity_mode("off"), Pass::None);
    EXPECT_EQ(Pass::identity_mode("unknown"), Pass::None);
    EXPECT_EQ(Pass::identity_mode(""), Pass::None);
    const auto client = Source("src/luminumbra_client/main_client.cpp");
    EXPECT_NE(client.find("Rendering::DebugViewPass::identity_mode(dv)"), std::string::npos);
    const auto widget = Source("src/luminumbra_client/debug/widgets/RenderControlWidgets.cpp");
    EXPECT_NE(widget.find("Pass::identity_mode_names.data()"), std::string::npos);
}

TEST(DiagnosticInspector, DefaultShaderRetainsTheExistingInterfaceAndInstructions) {
    std::istringstream lines(Source("res/shaders/debug_view.frag"));
    std::string line;
    bool diagnostic = false;
    int blocks = 0;
    std::uint64_t hash = 14695981039346656037ULL;
    while (std::getline(lines, line)) {
        if (line == "#ifdef LUMIN_DIAG_VIEWS") {
            diagnostic = true;
            ++blocks;
        } else if (diagnostic && line == "#endif") {
            diagnostic = false;
        } else if (!diagnostic && !line.empty()) {
            line += '\n';
            for (const char character : line) {
                hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ULL;
            }
        }
    }
    EXPECT_EQ(blocks, 3);
    EXPECT_FALSE(diagnostic);
    EXPECT_EQ(hash, 0xc185b52e84860ecaULL);
}

TEST(DiagnosticInspector, CompetingCaptureRetainsOwnershipAndThePendingRequest) {
    DiagnosticTargets targets;
    DiagnosticDrawTable table;
    GBufferDiagnosticCapture capture(targets, table);
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    control.refresh = true;
    Pass pass;
    EXPECT_FALSE(pass.begin_frame(RenderContext{}));
    EXPECT_TRUE(control.refresh);
    EXPECT_EQ(control.status, "Another diagnostic capture owns this frame");
    EXPECT_EQ(GBufferDiagnosticCapture::Current(), &capture);
    EXPECT_EQ(targets.texture(), 0U);
    control = {};
}

TEST(DiagnosticInspector, CyclePreservesLegacyOffPathAndVisitsEveryEnabledView) {
    auto& control = Pass::controls();
    control = {};
    int legacy = Pass::None;
    for (int expected = Pass::Albedo; expected <= Pass::Position; ++expected) {
        Pass::cycle_mode(legacy);
        EXPECT_EQ(legacy, expected);
        EXPECT_EQ(control.view, Pass::None);
    }
    Pass::cycle_mode(legacy);
    EXPECT_EQ(legacy, Pass::None);
    EXPECT_FALSE(control.refresh);
    legacy = std::numeric_limits<int>::max();
    Pass::cycle_mode(legacy);
    EXPECT_EQ(legacy, Pass::None);
    legacy = -1;
    Pass::cycle_mode(legacy);
    EXPECT_EQ(legacy, Pass::None);
    control.enabled = true;
    legacy = Pass::Position;
    for (int expected = Pass::DrawId; expected <= Pass::MeshPath; ++expected) {
        Pass::cycle_mode(legacy);
        EXPECT_EQ(control.view, expected);
        EXPECT_EQ(legacy, Pass::None);
    }
    EXPECT_TRUE(control.refresh);
    Pass::cycle_mode(legacy);
    EXPECT_EQ(control.view, Pass::None);
    control = {};
}

TEST(DiagnosticInspector, MaterialConversionRefusesNonfiniteFractionalAndOutOfRangeValues) {
    std::uint32_t output = 42;
    for (const float value : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -1.0f,
                              0.5f,
                              256.0f,
                              1e38f}) {
        EXPECT_FALSE(Pass::decode_material(value, output));
        EXPECT_EQ(output, 42U);
    }
    EXPECT_TRUE(Pass::decode_material(0, output));
    EXPECT_EQ(output, 0U);
    EXPECT_TRUE(Pass::decode_material(255, output));
    EXPECT_EQ(output, 255U);
    const auto source = Source("src/luminumbra_client/rendering/passes/DebugViewPass.cpp");
    EXPECT_NE(source.find("m_snapshot->sample_buffer == 0 || m_snapshot->sample_fbo == 0 ||"),
              std::string::npos);
    EXPECT_LT(source.find("m_snapshot->sample_buffer == 0 || m_snapshot->sample_fbo == 0 ||"),
              source.find("glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_snapshot->sample_fbo)"));
    EXPECT_NE(source.find("glBufferData(GL_TEXTURE_BUFFER, 0, nullptr, GL_STATIC_DRAW)"),
              std::string::npos);
    EXPECT_LT(source.find("if (metadata_bytes != 0)"), source.find("metadata.data(),"));
    for (const auto* refusal : {"if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)",
                                "if (sample_bytes != sizeof(InspectorPixel))"}) {
        const auto start = source.find(refusal);
        ASSERT_NE(start, std::string::npos);
        const auto end = source.find("return;", start);
        ASSERT_NE(end, std::string::npos);
        EXPECT_LT(source.find("m_snapshot->reset_sample();", start), end);
    }
}

TEST(DiagnosticInspector, ViewportConversionAppliesDpiAndReducedRenderScaleOnce) {
    for (const double dpi : {1.0, 1.5, 2.0}) {
        Pass::Controls control;
        control.enabled = true;
        control.inspector_open = true;
        control.armed = true;
        control.extent = {600, 300}; // 0.5 render scale, including letterboxed output.
        const Pass::Viewport viewport{.window_width = 1400 / dpi,
                                      .window_height = 800 / dpi,
                                      .framebuffer = {1400, 800},
                                      .left = 100,
                                      .top = 100,
                                      .width = 1200,
                                      .height = 600};
        ASSERT_TRUE(Pass::select_pixel(control, 701 / dpi, 303 / dpi, viewport));
        EXPECT_EQ(control.x, 300);
        EXPECT_EQ(control.y, 101);
        EXPECT_TRUE(control.inspect_requested);
        EXPECT_TRUE(control.refresh);
        ASSERT_TRUE(Pass::select_pixel(control, 100 / dpi, 100 / dpi, viewport));
        EXPECT_EQ(control.x, 0);
        EXPECT_EQ(control.y, 0);
        ASSERT_TRUE(Pass::select_pixel(control, 1299.5 / dpi, 699.5 / dpi, viewport));
        EXPECT_EQ(control.x, 599);
        EXPECT_EQ(control.y, 299);
    }
}

TEST(DiagnosticInspector, ViewportRefusalsPreserveSelectionAndPendingState) {
    Pass::Controls control;
    control.enabled = true;
    control.inspector_open = true;
    control.armed = true;
    control.extent = {600, 300};
    control.x = 42;
    control.y = 17;
    control.result = "previous result";
    const Pass::Viewport viewport{.window_width = 1400,
                                  .window_height = 800,
                                  .framebuffer = {1400, 800},
                                  .left = 100,
                                  .top = 100,
                                  .width = 1200,
                                  .height = 600};
    const auto expect_unchanged = [&control] {
        EXPECT_EQ(control.x, 42);
        EXPECT_EQ(control.y, 17);
        EXPECT_FALSE(control.refresh);
        EXPECT_FALSE(control.inspect_requested);
        EXPECT_EQ(control.result, "previous result");
    };
    for (const auto& point :
         std::array<std::array<double, 2>, 8>{{{99, 100},
                                               {100, 99},
                                               {1300, 100},
                                               {100, 700},
                                               {-1, 200},
                                               {200, -1},
                                               {std::numeric_limits<double>::infinity(), 200},
                                               {200, std::numeric_limits<double>::quiet_NaN()}}}) {
        EXPECT_FALSE(Pass::select_pixel(control, point[0], point[1], viewport));
        expect_unchanged();
    }
    for (const int gate : {0, 1, 2}) {
        auto disarmed = control;
        if (gate == 0) {
            disarmed.armed = false;
        } else if (gate == 1) {
            disarmed.inspector_open = false;
        } else {
            disarmed.enabled = false;
        }
        EXPECT_FALSE(Pass::select_pixel(disarmed, 700, 300, viewport));
        EXPECT_EQ(disarmed.x, 42);
        EXPECT_EQ(disarmed.y, 17);
        EXPECT_FALSE(disarmed.inspect_requested);
        EXPECT_FALSE(disarmed.refresh);
    }
    for (const int invalid : {0, 1, 2, 3, 4, 5, 6, 7}) {
        auto broken = viewport;
        if (invalid == 0) {
            broken.window_width = 0;
        } else if (invalid == 1) {
            broken.window_height = -1;
        } else if (invalid == 2) {
            broken.framebuffer = {};
        } else if (invalid == 3) {
            broken.width = 0;
        } else if (invalid == 4) {
            broken.height = std::numeric_limits<double>::quiet_NaN();
        } else if (invalid == 5) {
            broken.left = -1;
        } else if (invalid == 6) {
            broken.width = 1400;
        } else {
            broken.top = 900;
        }
        EXPECT_FALSE(Pass::select_pixel(control, 700, 300, broken));
        expect_unchanged();
    }
    control.extent = {};
    EXPECT_FALSE(Pass::select_pixel(control, 700, 300, viewport));
    expect_unchanged();
    control.extent = {std::numeric_limits<std::uint32_t>::max(), 1};
    EXPECT_FALSE(Pass::select_pixel(control, 700, 300, viewport));
    expect_unchanged();
}

TEST(DiagnosticInspector, ClipboardPayloadIsExactlySchemaOneAndRefusesInvalidSamples) {
    const Pass::InspectorPixel pixel{
        .id = {42, 7, 2, 259}, .albedo = {0.25f, 0.5f, 1, 17}, .normal_depth = {0, 1, 0, 0.75f}};
    const auto json = Pass::pixel_json({3840, 1600}, 100, 900, pixel);
    ASSERT_FALSE(json.empty());
    const auto probe = nlohmann::json::parse(json);
    EXPECT_EQ(probe,
              (nlohmann::json{{"schema", 1},
                              {"origin", "top_left"},
                              {"width", 3840},
                              {"height", 1600},
                              {"x", 100},
                              {"y", 900},
                              {"background", false},
                              {"id", pixel.id},
                              {"draw", 42},
                              {"mesh", 7},
                              {"lod", 2},
                              {"ownership", 3},
                              {"pass", 1},
                              {"albedo_linear", {0.25, 0.5, 1}},
                              {"normal_view", {0, 1, 0}},
                              {"depth_hardware", 0.75},
                              {"material", 17},
                              {"non_finite", false}}));
    EXPECT_FALSE(probe.contains("probe"));
    EXPECT_TRUE(Pass::pixel_json({3840, 1600}, -1, 900, pixel).empty());
    EXPECT_TRUE(Pass::pixel_json({3840, 1600}, 100, 1600, pixel).empty());
    EXPECT_TRUE(Pass::pixel_json({}, 0, 0, pixel).empty());
    auto invalid = pixel;
    invalid.id[3] |= 1U << 16U;
    EXPECT_TRUE(Pass::pixel_json({3840, 1600}, 100, 900, invalid).empty());
    invalid = pixel;
    invalid.albedo[3] = 1e38f;
    EXPECT_TRUE(Pass::pixel_json({3840, 1600}, 100, 900, invalid).empty());
    invalid = pixel;
    invalid.normal_depth[3] = std::numeric_limits<float>::infinity();
    const auto nonfinite = nlohmann::json::parse(Pass::pixel_json({10, 10}, 0, 0, invalid));
    EXPECT_EQ(nonfinite.at("depth_hardware"), nullptr);
    EXPECT_EQ(nonfinite.at("non_finite"), true);
    const auto widget = Source("src/luminumbra_client/debug/widgets/RenderControlWidgets.cpp");
    EXPECT_NE(widget.find("ImGui::SetClipboardText(control.probe_json.c_str())"),
              std::string::npos);
}

namespace {
// Counts the actual public GL entry point, including unexpected reads on closed/idle frames.
class InspectorReadbacks {
public:
    InspectorReadbacks() {
        current() = this;
        glGetBufferSubData = read;
    }
    ~InspectorReadbacks() {
        glGetBufferSubData = original_;
        current() = nullptr;
    }
    InspectorReadbacks(const InspectorReadbacks&) = delete;
    InspectorReadbacks& operator=(const InspectorReadbacks&) = delete;
    unsigned int count = 0;
    GLsizeiptr bytes = 0;

private:
    static InspectorReadbacks*& current() {
        static InspectorReadbacks* counter = nullptr;
        return counter;
    }
    static void APIENTRY read(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
        auto& counter = *current();
        ++counter.count;
        counter.bytes += size;
        counter.original_(target, offset, size, data);
    }
    decltype(glGetBufferSubData) original_ = glGetBufferSubData;
};

class DiagnosticViewportGl : public DiagnosticInspectorGl {
protected:
    void SetUp() override {
        DiagnosticInspectorGl::SetUp();
        ASSERT_FALSE(HasFatalFailure());
        Pass::controls() = {};
        CreateGBufferTargets(gbuffer_, registry_, 9, 9);
        ASSERT_NE(gbuffer_.fbo_id, 0U);
        pass_.init_shader(LUMINUMBRA_SOURCE_ROOT);
        pass_.init_buffers();
        ctx_.screen_width = 18;
        ctx_.screen_height = 18;
        ctx_.internal_width = 9;
        ctx_.internal_height = 9;
        ctx_.gbuffer_normal = {gbuffer_.normal_texture};
        ctx_.gbuffer_albedo = {gbuffer_.albedo_texture};
        ctx_.gbuffer_depth = {gbuffer_.depth_texture};
        ctx_.gbuffer_position = {gbuffer_.position_texture};
        ctx_.frame_index = 123;
        ctx_.view[3][0] = 7;
        ctx_.gbuffer_projection[0][0] = 2;
        paint_live(64, 17, 0.25f);
        auto& control = Pass::controls();
        control.enabled = true;
        control.inspector_open = true;
        control.armed = true;
        control.x = 4;
        control.y = 4;
    }
    void TearDown() override {
        pass_.destroy_buffers();
        pass_.reset_shader();
        if (gbuffer_.fbo_id != 0) {
            DestroyGBufferTargets(gbuffer_, registry_);
        }
        Pass::controls() = {};
        DiagnosticInspectorGl::TearDown();
    }
    void paint_live(unsigned char red, unsigned char material, float depth) const {
        const std::array<unsigned char, 4> albedo = {red, 128, 255, 255};
        const std::array<unsigned char, 4> normal = {128, 128, 0, material};
        glClearTexImage(gbuffer_.albedo_texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, albedo.data());
        glClearTexImage(gbuffer_.normal_texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, normal.data());
        glClearTexImage(gbuffer_.depth_texture, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    }
    void capture(bool inspect, bool close_before_finish = false) {
        auto& control = Pass::controls();
        control.refresh = true;
        if (inspect) {
            Pass::request_inspection(control);
        }
        auto frame = pass_.begin_frame(ctx_);
        ASSERT_TRUE(frame);
        auto* producer = GBufferDiagnosticCapture::Current();
        ASSERT_NE(producer, nullptr);
        ASSERT_TRUE(producer->attach(gbuffer_, {9, 9}));
        const auto shaders = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
        Shader base((shaders / "g_buffer.vert").string().c_str(),
                    (shaders / "g_buffer.frag").string().c_str());
        ASSERT_TRUE(base.IsValid());
        auto* variant = producer->variant(&base, false);
        ASSERT_NE(variant, nullptr);
        ASSERT_NE(variant, &base);
        producer->write(*variant,
                        {.mesh_path = "res/models/rock.obj",
                         .shader_variant = "instanced_static_mesh",
                         .chunk = {7, 8, 9},
                         .lod = 2,
                         .ownership = DiagnosticOwnership::Live,
                         .pass = DiagnosticPass::GBuffer,
                         .provenance = DiagnosticProvenance::Mesh});
        GLint identity_texture = 0;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                              &identity_texture);
        const DiagnosticId id = {1, 1, 2, 257};
        glClearTexImage(
            static_cast<GLuint>(identity_texture), 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, id.data());
        producer->detach();
        ASSERT_TRUE(producer->complete());
        if (close_before_finish) {
            control.inspector_open = false;
        }
        Pass::end_frame(frame, ctx_);
        ASSERT_FALSE(frame);
        EXPECT_EQ(GBufferDiagnosticCapture::Current(), nullptr);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
    RenderResourceRegistry registry_;
    GBuffer gbuffer_;
    Pass pass_;
    RenderContext ctx_;
};
} // namespace

TEST_F(DiagnosticViewportGl, FreezeKeepsImageCameraAndDrawTableFromOneFrame) {
    InspectorReadbacks readbacks;
    capture(true);
    ASSERT_FALSE(HasFatalFailure());
    auto& control = Pass::controls();
    ASSERT_FALSE(control.probe_json.empty()) << control.result;
    const auto original = nlohmann::json::parse(control.result);
    ASSERT_NE(control.image_texture, 0U);
    control.frozen = true;
    paint_live(192, 31, 0.75f);
    ctx_.view[3][0] = 999;
    ctx_.gbuffer_projection[0][0] = 11;
    ctx_.frame_index = 456;
    // A separate live table reuses draw 1 for another mesh; frozen lookup must stay local.
    DiagnosticDrawTable live_draws;
    DiagnosticId live_id{};
    GBufferDiagnosticCapture live(live_draws);
    ASSERT_TRUE(live_draws.submit({.mesh_path = "res/models/tree.obj",
                                   .shader_variant = "instanced_static_mesh",
                                   .chunk = {-1, -2, -3},
                                   .lod = 2,
                                   .ownership = DiagnosticOwnership::Live,
                                   .pass = DiagnosticPass::GBuffer,
                                   .provenance = DiagnosticProvenance::Mesh},
                                  live_id));
    Pass::request_inspection(control);
    EXPECT_FALSE(pass_.begin_frame(ctx_));
    EXPECT_EQ(GBufferDiagnosticCapture::Current(), &live);
    ASSERT_FALSE(control.probe_json.empty()) << control.result;
    EXPECT_EQ(nlohmann::json::parse(control.result), original);
    EXPECT_EQ(original.at("frame"), 123);
    EXPECT_EQ(original.at("view")[12], 7);
    EXPECT_EQ(original.at("projection")[0], 2);
    EXPECT_EQ(original.at("record").at("mesh").at("path"), "res/models/rock.obj");
    EXPECT_EQ(original.at("record").at("draw").at("chunk"), (std::array<int, 3>{7, 8, 9}));
    EXPECT_NEAR(original.at("probe").at("albedo_linear")[0].get<float>(), 64.0f / 255, 1e-6f);
    EXPECT_EQ(original.at("probe").at("material"), 17);
    EXPECT_EQ(original.at("probe").at("depth_hardware"), 0.25);
    EXPECT_EQ(readbacks.count, 2U);
    EXPECT_EQ(readbacks.bytes, 96);
    EXPECT_FALSE(control.inspect_requested);
    EXPECT_FALSE(pass_.begin_frame(ctx_));
    EXPECT_EQ(readbacks.count, 2U);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(DiagnosticViewportGl, ZoomIsNineNearestTexelsWithOnlyOnePixelReadback) {
    // An off-centre top-row texel detects orientation, neighbourhood and filtering mistakes.
    const std::array<unsigned char, 4> distinctive = {255, 0, 64, 255};
    glBindTexture(GL_TEXTURE_2D, gbuffer_.albedo_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, distinctive.data());
    InspectorReadbacks readbacks;
    capture(true);
    ASSERT_FALSE(HasFatalFailure());
    const auto& control = Pass::controls();
    ASSERT_NE(control.zoom_texture, 0U);
    glBindTexture(GL_TEXTURE_2D, control.zoom_texture);
    GLint filter = 0;
    GLint width = 0;
    GLint height = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    EXPECT_EQ(filter, GL_NEAREST);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &filter);
    EXPECT_EQ(filter, GL_NEAREST);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    EXPECT_EQ(width, 9);
    EXPECT_EQ(height, 9);
    std::array<std::array<unsigned char, 4>, 81> texels{};
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    EXPECT_EQ(texels[73], distinctive);
    EXPECT_EQ(texels[40], (std::array<unsigned char, 4>{64, 128, 255, 255}));
    EXPECT_EQ(readbacks.count, 1U);
    EXPECT_EQ(readbacks.bytes, 48);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(DiagnosticViewportGl, ClosedIdleInvalidAndReplayRequestsNeverReadBack) {
    InspectorReadbacks readbacks;
    auto& control = Pass::controls();
    control.inspector_open = false;
    control.frozen = true;
    capture(false);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(readbacks.count, 0U);
    EXPECT_EQ(control.image_texture, 0U);
    EXPECT_FALSE(control.frozen);
    EXPECT_TRUE(control.probe_json.empty());
    control.inspector_open = true;
    control.frozen = true;
    capture(true, true);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(readbacks.count, 0U);
    EXPECT_EQ(control.image_texture, 0U);
    EXPECT_EQ(control.zoom_texture, 0U);
    EXPECT_FALSE(control.frozen);
    EXPECT_FALSE(control.inspect_requested);
    EXPECT_TRUE(control.probe_json.empty());
    control.inspector_open = true;
    capture(true);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(readbacks.count, 1U);
    control.frozen = true;
    Pass::request_inspection(control);
    control.inspector_open = false;
    EXPECT_FALSE(pass_.begin_frame(ctx_));
    EXPECT_FALSE(control.inspect_requested);
    EXPECT_EQ(readbacks.count, 1U);
    control.inspector_open = true;
    EXPECT_FALSE(pass_.begin_frame(ctx_));
    EXPECT_EQ(readbacks.count, 1U);
    for (const int invalid : {-1, 9, std::numeric_limits<int>::max()}) {
        control.x = invalid;
        Pass::request_inspection(control);
        EXPECT_FALSE(pass_.begin_frame(ctx_));
        EXPECT_FALSE(control.inspect_requested);
        EXPECT_EQ(readbacks.count, 1U);
        EXPECT_TRUE(control.probe_json.empty());
    }
    control.x = 4;
    Pass::request_inspection(control);
    ctx_.frozen_replay = true;
    EXPECT_FALSE(pass_.begin_frame(ctx_));
    EXPECT_EQ(readbacks.count, 1U);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(DiagnosticViewportGl, OutputResizeAndRenderScaleInvalidatePendingSelectionAndFrozenFrame) {
    for (const int resize_case : {0, 1, 2}) {
        InspectorReadbacks readbacks;
        capture(true);
        ASSERT_FALSE(HasFatalFailure());
        auto& control = Pass::controls();
        control.frozen = true;
        const auto image = control.image_texture;
        const auto zoom = control.zoom_texture;
        ASSERT_NE(image, 0U);
        ASSERT_NE(zoom, 0U);
        Pass::request_inspection(control);
        if (resize_case == 0) {
            ++ctx_.internal_width;
        } else if (resize_case == 1) {
            ++ctx_.screen_width;
        } else {
            // Window notification invalidates before the debounced attachments change size.
            Pass::invalidate_selection(control);
            EXPECT_TRUE(control.frame_invalidated);
        }
        EXPECT_FALSE(pass_.begin_frame(ctx_));
        EXPECT_FALSE(control.frame_invalidated);
        EXPECT_EQ(readbacks.count, 1U);
        EXPECT_FALSE(control.frozen);
        EXPECT_FALSE(control.refresh);
        EXPECT_FALSE(control.inspect_requested);
        EXPECT_EQ(control.x, -1);
        EXPECT_EQ(control.y, -1);
        EXPECT_TRUE(control.result.empty());
        EXPECT_TRUE(control.probe_json.empty());
        EXPECT_EQ(control.image_texture, 0U);
        EXPECT_EQ(control.zoom_texture, 0U);
        EXPECT_EQ(glIsTexture(image), GL_FALSE);
        EXPECT_EQ(glIsTexture(zoom), GL_FALSE);
        EXPECT_NE(control.status.find("Resize invalidated"), std::string::npos);
        ctx_.screen_width = 18;
        ctx_.internal_width = 9;
        control.extent = {9, 9};
        control.output = {18, 18};
        control.x = 4;
        control.y = 4;
    }
}

TEST_F(DiagnosticViewportGl, MissingMismatchedAndPartialImagesRefuseWithoutPublishingOrReading) {
    InspectorReadbacks readbacks;
    const auto original_context = ctx_;
    GLuint partial = 0;
    glGenTextures(1, &partial);
    glBindTexture(GL_TEXTURE_2D, partial);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    for (const int invalid : {0, 1, 2, 3, 4}) {
        ctx_ = original_context;
        if (invalid == 0) {
            ctx_.gbuffer_normal = {};
        } else if (invalid == 1) {
            ctx_.gbuffer_albedo = {};
        } else if (invalid == 2) {
            ctx_.gbuffer_depth = {};
        } else if (invalid == 3) {
            ctx_.gbuffer_normal = ctx_.gbuffer_position; // Unsupported RGB format.
        } else {
            ctx_.gbuffer_albedo = {partial}; // Correct format, incomplete extent.
        }
        auto& control = Pass::controls();
        control.frozen = true;
        capture(true);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_FALSE(control.frozen);
        EXPECT_FALSE(control.inspect_requested);
        EXPECT_EQ(control.image_texture, 0U);
        EXPECT_EQ(control.zoom_texture, 0U);
        EXPECT_TRUE(control.probe_json.empty());
        EXPECT_EQ(control.result, "Snapshot image storage unavailable");
        EXPECT_EQ(readbacks.count, 0U);
    }
    ctx_ = original_context;
    capture(true);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_FALSE(Pass::controls().probe_json.empty());
    EXPECT_EQ(readbacks.count, 1U);
    glDeleteTextures(1, &partial);
}

TEST_F(DiagnosticViewportGl, CornerZoomClampsEdgesAndKeepsTheRequestedPixelAtItsCentre) {
    const std::array<std::array<int, 2>, 4> corners = {{{0, 0}, {8, 0}, {0, 8}, {8, 8}}};
    const std::array<std::array<unsigned char, 4>, 4> colors = {
        {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
    const std::array<std::size_t, 4> outward_texel = {72, 80, 0, 8};
    glBindTexture(GL_TEXTURE_2D, gbuffer_.albedo_texture);
    for (std::size_t index = 0; index < corners.size(); ++index) {
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        corners[index][0],
                        8 - corners[index][1],
                        1,
                        1,
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        colors[index].data());
    }
    InspectorReadbacks readbacks;
    auto& control = Pass::controls();
    for (std::size_t index = 0; index < corners.size(); ++index) {
        control.x = corners[index][0];
        control.y = corners[index][1];
        if (index == 0) {
            capture(true);
            ASSERT_FALSE(HasFatalFailure());
            control.frozen = true;
        } else {
            Pass::request_inspection(control);
            EXPECT_FALSE(pass_.begin_frame(ctx_));
        }
        ASSERT_FALSE(control.probe_json.empty()) << control.result;
        const auto probe = nlohmann::json::parse(control.probe_json);
        EXPECT_EQ(probe.at("x"), control.x);
        EXPECT_EQ(probe.at("y"), control.y);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            EXPECT_EQ(probe.at("albedo_linear")[channel], colors[index][channel] / 255.0f);
        }
        glBindTexture(GL_TEXTURE_2D, control.zoom_texture);
        std::array<std::array<unsigned char, 4>, 81> texels{};
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
        EXPECT_EQ(texels[40], colors[index]);
        EXPECT_EQ(texels[outward_texel[index]], colors[index]);
        EXPECT_EQ(readbacks.count, index + 1);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
}

namespace {
class RefuseInspectorImageStorage {
public:
    RefuseInspectorImageStorage() {
        current() = this;
        glTexStorage2D = allocate;
    }
    ~RefuseInspectorImageStorage() {
        glTexStorage2D = original_;
        current() = nullptr;
    }
    RefuseInspectorImageStorage(const RefuseInspectorImageStorage&) = delete;
    RefuseInspectorImageStorage& operator=(const RefuseInspectorImageStorage&) = delete;
    bool refused = false;

private:
    static RefuseInspectorImageStorage*& current() {
        static RefuseInspectorImageStorage* storage = nullptr;
        return storage;
    }
    static void APIENTRY
    allocate(GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height) {
        auto& storage = *current();
        if (!storage.refused && format == GL_RGBA8 && width == 9 && height == 9) {
            storage.refused = true;
            return;
        }
        storage.original_(target, levels, format, width, height);
    }
    decltype(glTexStorage2D) original_ = glTexStorage2D;
};
} // namespace

TEST_F(DiagnosticViewportGl, FailedImageAllocationConsumesTheRequestAndCanBeRetried) {
    InspectorReadbacks readbacks;
    RefuseInspectorImageStorage storage;
    capture(true);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(storage.refused);
    auto& control = Pass::controls();
    EXPECT_FALSE(control.inspect_requested);
    EXPECT_EQ(control.image_texture, 0U);
    EXPECT_EQ(control.zoom_texture, 0U);
    EXPECT_EQ(control.result, "Snapshot image storage unavailable");
    EXPECT_TRUE(control.probe_json.empty());
    EXPECT_EQ(readbacks.count, 0U);
    EXPECT_FALSE(pass_.begin_frame(ctx_));
    EXPECT_EQ(readbacks.count, 0U);
    capture(true);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_FALSE(control.probe_json.empty());
    EXPECT_NE(control.image_texture, 0U);
    EXPECT_NE(control.zoom_texture, 0U);
    EXPECT_EQ(readbacks.count, 1U);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(DiagnosticViewportGl, DisableAndDestructionReleaseEveryBorrowedImage) {
    for (const bool disable : {true, false}) {
        auto& control = Pass::controls();
        control.enabled = true;
        control.inspector_open = true;
        control.x = 4;
        control.y = 4;
        InspectorReadbacks readbacks;
        capture(true);
        ASSERT_FALSE(HasFatalFailure());
        const auto image = control.image_texture;
        const auto zoom = control.zoom_texture;
        ASSERT_NE(image, 0U);
        ASSERT_NE(zoom, 0U);
        control.frozen = true;
        Pass::request_inspection(control);
        if (disable) {
            control.enabled = false;
            pass_.set_mode(Pass::None);
        } else {
            pass_.destroy_buffers();
        }
        EXPECT_FALSE(control.frozen);
        EXPECT_FALSE(control.inspect_requested);
        EXPECT_EQ(control.zoom_texture, 0U);
        EXPECT_EQ(control.image_texture, 0U);
        EXPECT_EQ(glIsTexture(image), GL_FALSE);
        EXPECT_EQ(glIsTexture(zoom), GL_FALSE);
        EXPECT_TRUE(control.probe_json.empty());
        EXPECT_EQ(readbacks.count, 1U);
    }
}

TEST(DiagnosticInspector, ViewportInputSharesOwnershipGatesAndLeavesMenusAndPanelsAlone) {
    const auto request = SourceSection("src/luminumbra_client/rendering/passes/DebugViewPass.h",
                                       "static void request_inspection(",
                                       "// Resize callers");
    EXPECT_NE(request.find("control.inspect_requested = true;"), std::string::npos);
    EXPECT_NE(request.find("!control.enabled || !control.inspector_open"), std::string::npos);
    const auto input = Source("src/luminumbra_client/app/InputCallbacks.cpp");
    const auto gates = SourceSection("src/luminumbra_client/app/InputCallbacks.cpp",
                                     "bool ViewportPickingAllowed(",
                                     "} // namespace");
    for (const auto* gate : {"DeveloperUiAllowed(app)",
                             "bindings.in_world",
                             "bindings.window_focused",
                             "!app.hud.paused",
                             "!app.hud.codexOpen",
                             "diagnostics.enabled",
                             "diagnostics.inspector_open",
                             "diagnostics.armed",
                             "!ui->HasInputInteraction()",
                             "!ui->DeveloperDebuggerVisible()"}) {
        EXPECT_NE(gates.find(gate), std::string::npos) << gate;
    }
    const auto ownership = SourceSection("src/luminumbra_client/app/InputCallbacks.cpp",
                                         "void RefreshInputOwnership(",
                                         "void char_callback(");
    EXPECT_NE(ownership.find("ownership.interactive_panel || ViewportPickingAllowed(b)"),
              std::string::npos);
    const auto callback = SourceSection("src/luminumbra_client/app/InputCallbacks.cpp",
                                        "void mouse_button_callback(",
                                        "void window_focus_callback(");
    EXPECT_LT(callback.find("ViewportPickingAllowed(b)"), callback.find("select_pixel("));
    EXPECT_LT(callback.find("button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS"),
              callback.find("select_pixel("));
    EXPECT_LT(callback.find("!ImGui::GetIO().WantCaptureMouse"), callback.find("select_pixel("));
    EXPECT_NE(callback.find("glfwGetWindowSize"), std::string::npos);
    EXPECT_NE(callback.find("glfwGetFramebufferSize"), std::string::npos);
    EXPECT_NE(callback.find("glfwGetCursorPos"), std::string::npos);
    const auto pipeline = Source("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto resize = pipeline.substr(pipeline.find("void RenderPipeline::on_resize("));
    EXPECT_LT(resize.find("m_debug_view_pass->invalidate();"),
              resize.find("m_gbuffer_pass->destroy_gbuffer("));
    const auto scale = SourceSection("src/luminumbra_client/rendering/RenderPipeline.cpp",
                                     "bool RenderPipeline::resize_graphics_targets(",
                                     "bool RenderPipeline::apply_graphics(");
    EXPECT_NE(scale.find("m_debug_view_pass->invalidate();"), std::string::npos);
    const auto framebuffer = input.substr(input.find("void framebuffer_size_callback("));
    EXPECT_LT(framebuffer.find("DebugViewPass::invalidate_selection"),
              framebuffer.find("windowState.resize_pending = true"));
}

TEST_F(DiagnosticViewportGl, ZoomIgnoresCallerWriteMasksAndRestoresRasterState) {
    for (const bool index_zero_enabled : {false, true}) {
        const std::array<GLboolean, 4> mask = {GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE};
        glColorMaski(0, mask[0], mask[1], mask[2], mask[3]);
        const std::array<GLenum, 8> capabilities = {GL_DEPTH_TEST,
                                                    GL_BLEND,
                                                    GL_SCISSOR_TEST,
                                                    GL_CULL_FACE,
                                                    GL_RASTERIZER_DISCARD,
                                                    GL_DITHER,
                                                    GL_FRAMEBUFFER_SRGB,
                                                    GL_COLOR_LOGIC_OP};
        const std::array<GLenum, 2> indexed_capabilities = {GL_BLEND, GL_SCISSOR_TEST};
        for (const auto capability : capabilities) {
            glEnable(capability);
        }
        for (const auto capability : indexed_capabilities) {
            glDisablei(capability, index_zero_enabled ? 1U : 0U);
        }
        std::array<GLfloat, 4> viewport = {0.25f, 1.5f, 8.5f, 7.25f};
        std::array<GLfloat, 4> other_viewport = {2.5f, 3.25f, 6.5f, 5.75f};
        glViewportIndexedf(0, viewport[0], viewport[1], viewport[2], viewport[3]);
        glViewportIndexedf(
            1, other_viewport[0], other_viewport[1], other_viewport[2], other_viewport[3]);
        glGetFloati_v(GL_VIEWPORT, 0, viewport.data());
        glGetFloati_v(GL_VIEWPORT, 1, other_viewport.data());
        glScissor(0, 0, 0, 0);
        glLogicOp(GL_XOR);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        InspectorReadbacks readbacks;
        capture(true);
        ASSERT_FALSE(HasFatalFailure());
        const auto& control = Pass::controls();
        ASSERT_FALSE(control.probe_json.empty()) << control.result;
        ASSERT_NE(control.zoom_texture, 0U);
        glBindTexture(GL_TEXTURE_2D, control.zoom_texture);
        std::array<std::array<unsigned char, 4>, 81> texels{};
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
        EXPECT_EQ(texels[40], (std::array<unsigned char, 4>{64, 128, 255, 255}));
        std::array<GLboolean, 4> restored_mask{};
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, restored_mask.data());
        EXPECT_EQ(restored_mask, mask);
        for (const auto capability : capabilities) {
            const bool indexed = capability == GL_BLEND || capability == GL_SCISSOR_TEST;
            EXPECT_EQ(glIsEnabled(capability), indexed && !index_zero_enabled ? GL_FALSE : GL_TRUE);
        }
        for (const auto capability : indexed_capabilities) {
            EXPECT_EQ(glIsEnabledi(capability, 1), index_zero_enabled ? GL_FALSE : GL_TRUE);
        }
        std::array<GLfloat, 4> restored_viewport{};
        glGetFloati_v(GL_VIEWPORT, 0, restored_viewport.data());
        EXPECT_EQ(restored_viewport, viewport);
        glGetFloati_v(GL_VIEWPORT, 1, restored_viewport.data());
        EXPECT_EQ(restored_viewport, other_viewport);
        std::array<GLint, 2> polygon{};
        glGetIntegerv(GL_POLYGON_MODE, polygon.data());
        EXPECT_EQ(polygon[0], GL_LINE);
        EXPECT_EQ(polygon[1], GL_LINE);
        EXPECT_EQ(readbacks.count, 1U);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
}

TEST_F(DiagnosticViewportGl, RefusedFreezeReleasesTheRequestAndAllowsANewLiveInspection) {
    InspectorReadbacks readbacks;
    auto& control = Pass::controls();
    control.frozen = true;
    Pass::request_inspection(control);
    control.refresh = true;
    auto frame = pass_.begin_frame(ctx_);
    ASSERT_TRUE(frame);
    ASSERT_NE(GBufferDiagnosticCapture::Current(), nullptr);
    GBufferDiagnosticCapture::Current()->abort();
    Pass::end_frame(frame, ctx_);
    EXPECT_FALSE(control.frozen);
    EXPECT_FALSE(control.inspect_requested);
    EXPECT_FALSE(control.refresh);
    EXPECT_EQ(control.image_texture, 0U);
    EXPECT_EQ(control.zoom_texture, 0U);
    EXPECT_TRUE(control.probe_json.empty());
    EXPECT_EQ(readbacks.count, 0U);
    EXPECT_NE(control.status.find("Snapshot refused"), std::string::npos);
    Pass::request_inspection(control);
    EXPECT_TRUE(control.refresh);
    capture(true);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_FALSE(control.probe_json.empty());
    EXPECT_EQ(readbacks.count, 1U);
}
