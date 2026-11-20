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
#include <memory>
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

class DiagnosticInspectorFrameGl : public DiagnosticInspectorGl {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(DiagnosticInspectorGl::SetUp());
        graphics_ready_ = true;
        auto& control = Pass::controls();
        control = {};
        control.enabled = true;
        control.inspector_open = true;
        control.refresh = true;
        pass_.init_shader(LUMINUMBRA_SOURCE_ROOT);
        pass_.init_buffers();
        CreateGBufferTargets(gbuffer_, registry_, 2, 2);
        ASSERT_NE(gbuffer_.fbo_id, 0U);
        ctx_.internal_width = 2;
        ctx_.internal_height = 2;
        ctx_.gbuffer_position = adopt_texture(gbuffer_.position_texture);
        ctx_.gbuffer_normal = adopt_texture(gbuffer_.normal_texture);
        ctx_.gbuffer_albedo = adopt_texture(gbuffer_.albedo_texture);
        ctx_.gbuffer_depth = adopt_texture(gbuffer_.depth_texture);
        const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
        const auto vertex = (root / "g_buffer.vert").string();
        const auto fragment = (root / "g_buffer.frag").string();
        base_ = std::make_unique<Shader>(vertex.c_str(), fragment.c_str());
        ASSERT_TRUE(base_->IsValid());
    }
    void TearDown() override {
        if (graphics_ready_) {
            frame_.reset();
            glUseProgram(0);
            pass_.destroy_buffers();
            pass_.reset_shader();
            base_.reset();
            DestroyGBufferTargets(gbuffer_, registry_);
            Pass::controls() = {};
        }
        DiagnosticInspectorGl::TearDown();
    }
    void BeginCapture() {
        frame_ = pass_.begin_frame(ctx_);
        ASSERT_TRUE(frame_);
        auto* capture = GBufferDiagnosticCapture::Current();
        ASSERT_NE(capture, nullptr);
        ASSERT_TRUE(capture->attach(gbuffer_, {2, 2}));
        auto* variant = capture->variant(base_.get(), false);
        ASSERT_NE(variant, base_.get());
        ASSERT_NE(variant, nullptr);
        ASSERT_TRUE(variant->IsValid());
        variant_program_ = variant->Id();
        variant->use();
        capture->write(*variant,
                       {.mesh_path = "res/models/rock.obj",
                        .shader_variant = "instanced_static_mesh",
                        .chunk = {-7, 0, 19},
                        .lod = 3,
                        .ownership = DiagnosticOwnership::Live,
                        .pass = DiagnosticPass::GBuffer,
                        .provenance = DiagnosticProvenance::Mesh});
        // Seed one bounded 2x2 capture with exact identities and known AOV values.
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gbuffer_.fbo_id);
        GLint identity_texture = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                              &identity_texture);
        ASSERT_NE(identity_texture, 0);
        const DiagnosticId id{1, 1, 3, 257};
        glClearTexImage(
            static_cast<GLuint>(identity_texture), 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, id.data());
        const std::array<float, 4> normal{0.5f, 0.5f, 0, 17.0f / 255};
        const std::array<float, 4> albedo{0.2f, 0.4f, 0.6f, 1};
        const float depth = 0.25f;
        glClearTexImage(gbuffer_.normal_texture, 0, GL_RGBA, GL_FLOAT, normal.data());
        glClearTexImage(gbuffer_.albedo_texture, 0, GL_RGBA, GL_FLOAT, albedo.data());
        glClearTexImage(gbuffer_.depth_texture, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        capture->detach();
        ASSERT_TRUE(capture->complete());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        GLint current = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &current);
        ASSERT_EQ(static_cast<GLuint>(current), variant_program_);
        ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
    void ExpectReleasedProgram() const {
        EXPECT_EQ(GBufferDiagnosticCapture::Current(), nullptr);
        EXPECT_EQ(glIsProgram(variant_program_), GL_FALSE);
        GLint current = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &current);
        EXPECT_TRUE(current == 0 || glIsProgram(static_cast<GLuint>(current)) == GL_TRUE);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
    Pass pass_;
    RenderContext ctx_;
    std::shared_ptr<Pass::Frame> frame_;

private:
    bool graphics_ready_ = false;
    RenderResourceRegistry registry_;
    GBuffer gbuffer_;
    std::unique_ptr<Shader> base_;
    GLuint variant_program_ = 0;
};
} // namespace

TEST_F(DiagnosticInspectorFrameGl, CaptureTeardownGatherRestoreAndLaterDisplayKeepValidPrograms) {
    ASSERT_NO_FATAL_FAILURE(BeginCapture());
    Pass::end_frame(frame_, ctx_);
    EXPECT_FALSE(frame_);
    ExpectReleasedProgram();
    const auto& control = Pass::controls();
    ASSERT_FALSE(control.result.empty());
    const auto result = nlohmann::json::parse(control.result);
    EXPECT_EQ(result.at("record").at("mesh").at("path"), "res/models/rock.obj");
    EXPECT_EQ(result.at("probe").at("material"), 17);
    EXPECT_EQ(result.at("probe").at("depth_hardware"), 0.25f);
    const auto captured_result = control.result;
    EXPECT_FALSE(pass_.begin_frame(ctx_));
    Pass::end_frame(frame_, ctx_);
    pass_.set_mode(Pass::DrawId);
    glViewport(0, 0, 2, 2);
    pass_.execute(ctx_);
    std::array<GLubyte, 4> color{};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
    const auto hash = Pass::hash_id(1);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(color[channel], (hash >> (channel * 8)) & 255U, 1);
    }
    EXPECT_EQ(control.result, captured_result);
    ExpectReleasedProgram();
}

TEST_F(DiagnosticInspectorFrameGl, RefusedCaptureReleasesTheCurrentVariant) {
    ASSERT_NO_FATAL_FAILURE(BeginCapture());
    GBufferDiagnosticCapture::Current()->abort();
    Pass::end_frame(frame_, ctx_);
    EXPECT_FALSE(frame_);
    EXPECT_EQ(Pass::controls().result, "Snapshot refused: incomplete or unsupported coverage");
    ExpectReleasedProgram();
}

TEST_F(DiagnosticInspectorFrameGl, AbandonedCaptureReleasesTheCurrentVariant) {
    ASSERT_NO_FATAL_FAILURE(BeginCapture());
    frame_.reset();
    EXPECT_EQ(Pass::controls().result, "Snapshot aborted");
    ExpectReleasedProgram();
}

TEST(DiagnosticInspector, CaptureTeardownUnbindsBeforeDeletingVariants) {
    const auto finish = SourceSection(
        "src/luminumbra_client/rendering/passes/DebugViewPass.cpp", "void finish(", "\nprivate:");
    const auto unbind = finish.find("AttributedGl::UseProgram(glUseProgram, 0U);");
    ASSERT_NE(unbind, std::string::npos);
    EXPECT_LT(unbind, finish.find("capture_.reset();"));
    const auto abandon = SourceSection(
        "src/luminumbra_client/rendering/passes/DebugViewPass.cpp", "~Frame()", "void finish(");
    EXPECT_NE(abandon.find("AttributedGl::UseProgram(glUseProgram, 0U);"), std::string::npos);
}

TEST(DiagnosticInspector, ProducerMappingIncludesUnknownAndExplicitUnsupported) {
    using Kind = Pass::ProducerKind;
    const std::array<std::pair<std::string_view, Kind>, 13> cases = {{
        {"terrain_mdi", Kind::Terrain},
        {"terrain_mdi_ladder", Kind::Terrain},
        {"live_terrain_mdi", Kind::Unsupported},
        {"legacy_far_terrain", Kind::Terrain},
        {"tree_impostor", Kind::Impostor},
        {"foliage_ladder", Kind::Foliage},
        {"instanced_static_mesh", Kind::Static},
        {"skinned_mesh_ladder", Kind::Static},
        {"static_asset", Kind::Static},
        {"far_volume", Kind::FarVolume},
        {"g_buffer_ladder_far", Kind::FarVolume},
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
    EXPECT_NE(implementation.find("glViewport(0, 0, 1, 1)"), std::string::npos);
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
    EXPECT_EQ(begin.find("inspector_open"), std::string::npos);
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
                                                       {Pass::Lod, 2 + 1},
                                                       {Pass::Chunk, 7},
                                                       {Pass::MeshPath, 4}}}) {
        shader.setInt("u_mode", mode);
        check_color(hashed_color(value));
    }
    {
        // LOD 0 is the common level; it must not render as black.
        auto lod0_id = ids[1];
        lod0_id[2] = 0;
        metadata[18] = lod0_id;
        glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(metadata), metadata.data());
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, lod0_id.data());
        shader.setInt("u_mode", Pass::Lod);
        const auto lod0 = hashed_color(1);
        EXPECT_GT(lod0[0] + lod0[1] + lod0[2], 0);
        check_color(lod0);
        metadata[18] = ids[1];
        glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(metadata), metadata.data());
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, ids[1].data());
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
    // The classifier must recognise the spellings live terrain actually submits.
    EXPECT_NE(Source("src/luminumbra_client/rendering/RenderPipeline.cpp")
                  .find("ladder ? \"terrain_mdi_ladder\" : \"terrain_mdi\""),
              std::string::npos);
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
    for (int expected = Pass::DrawId; expected <= Pass::ShadowPlane; ++expected) {
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
