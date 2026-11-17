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

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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
    const auto producers = Read(kPass);
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
    Contains(pass, "ReplayRollback restore_gl([=]() noexcept");
    const auto restore = Body(pass, "ReplayRollback restore_gl(");
    Contains(restore, "glBindVertexArray(static_cast<GLuint>(previous_vao))");
    Contains(restore,
             "glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw_fbo))");
    Contains(restore,
             "glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read_fbo))");
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
    Contains(Body(targets, "bool GBufferDiagnosticCapture::complete()"), "return false;");
}

TEST(DiagnosticAttribution, ProducerScopeNeverAttachesOrClaimsCompleteCoverage) {
    const auto pass = Read(kPass);
    const auto header = Read("src/luminumbra_client/rendering/GBufferTargets.h");
    for (const auto* source : {&pass, &header}) {
        EXPECT_EQ(source->find("DiagnosticTargets"), std::string::npos);
        EXPECT_EQ(source->find("glFramebufferTexture"), std::string::npos);
        EXPECT_EQ(source->find("GBufferDiagnosticAttachment"), std::string::npos);
        EXPECT_EQ(source->find("attach_target"), std::string::npos);
    }
    EXPECT_EQ(SourceHash(Read(kTargets)), 0x9915b3a54a41b2a9ull);
    Contains(header, "explicit GBufferDiagnosticCapture(DiagnosticDrawTable& table)");
    Contains(Body(pass, "bool GBufferDiagnosticCapture::complete()"), "return false;");
}

namespace {
using namespace Luminumbra::Rendering;

// Use the existing GLAD entry-point hook pattern to observe the real pass.
// Forward every call to the context and restore each entry before leaving scope.
class PassStateProbe {
public:
    PassStateProbe() {
        Active() = this;
        glGetIntegerv = GetInteger;
        glIsEnabled = IsEnabled;
        glBindVertexArray = BindVertexArray;
        glBindFramebuffer = BindFramebuffer;
    }
    ~PassStateProbe() {
        glGetIntegerv = previous_get_integer_;
        glIsEnabled = previous_is_enabled_;
        glBindVertexArray = previous_bind_vao_;
        glBindFramebuffer = previous_bind_fbo_;
        Active() = nullptr;
    }
    PassStateProbe(const PassStateProbe&) = delete;
    PassStateProbe& operator=(const PassStateProbe&) = delete;

    unsigned integer_queries = 0, enabled_queries = 0, vao_binds = 0;
    unsigned combined_fbo_binds = 0, draw_fbo_binds = 0, read_fbo_binds = 0;

private:
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
    Mesh* AddMesh(const std::string& path = "test/diagnostic_leaf") {
        auto mesh = MeshLoader::CreateFromArrays({{{-1, -1, -5}, {0, 0, 1}, {0, 0}},
                                                  {{1, -1, -5}, {0, 0, 1}, {1, 0}},
                                                  {{0, 1, -5}, {0, 0, 1}, {.5f, 1}}},
                                                 {0, 1, 2});
        auto* result = mesh.get();
        pass_.register_cached_mesh(path, std::move(mesh));
        const auto entity = entities_.create();
        entities_.emplace<Luminumbra::Components::TransformComponent>(entity);
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

TEST_F(DiagnosticAttributionGl, ExhaustedWriterAndUnsupportedRefuseWithoutGrowingOrThrowing) {
    for (const bool unsupported : {false, true}) {
        table_.clear();
        GBufferDiagnosticCapture capture(table_);
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
        GBufferDiagnosticCapture capture(table_);
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
    for (const bool replay : {false, true}) {
        ctx_.frozen_replay = replay;
        for (const bool cull : {false, true}) {
            GBufferDiagnosticCapture capture(table_);
            SetEntryState(cull);
            {
                PassStateProbe probe;
                EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
                EXPECT_EQ(probe.integer_queries, 3u);
                EXPECT_EQ(probe.enabled_queries, 1u);
                EXPECT_EQ(probe.vao_binds, 1u);
                EXPECT_EQ(probe.combined_fbo_binds, 1u);
                EXPECT_EQ(probe.draw_fbo_binds, 1u);
                EXPECT_EQ(probe.read_fbo_binds, 1u);
            }
            ExpectEntryState(cull);
        }
    }
}

TEST_F(DiagnosticAttributionGl, ExceptionalProducerRestoresCallerGlState) {
    GBufferDiagnosticCapture capture(table_);
    input_.submit_terrain_chunks = [](const glm::vec4(&)[6]) -> TerrainSubmitStats {
        glDisable(GL_CULL_FACE);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        throw std::runtime_error("producer failure");
    };
    for (const bool replay : {false, true}) {
        ctx_.frozen_replay = replay;
        for (const bool cull : {false, true}) {
            SetEntryState(cull);
            EXPECT_THROW(pass_.execute(ctx_, entities_, input_), std::runtime_error);
            ExpectEntryState(cull);
        }
    }
}

void DiagnosticAttributionGl::ExpectInvalidMetadata(bool excessive_path) {
    const std::string path = excessive_path ? std::string(1025, 'a') : "test/diagnostic_leaf";
    auto* mesh = AddMesh(path);
    ASSERT_NE(mesh, nullptr);
    if (!excessive_path) {
        mesh->indexCount = static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()) + 1u;
    }
    table_.clear();
    GBufferDiagnosticCapture capture(table_);
    SetEntryState(true);
    GLuint query = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_PRIMITIVES_GENERATED, query);
    EXPECT_NO_THROW(pass_.execute(ctx_, entities_, input_));
    glEndQuery(GL_PRIMITIVES_GENERATED);
    GLuint primitives = 1;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &primitives);
    glDeleteQueries(1, &query);
    EXPECT_EQ(primitives, 0u);
    EXPECT_FALSE(capture.complete());
    EXPECT_FALSE(table_.complete_coverage());
    const auto document = nlohmann::json::parse(table_.json());
    ASSERT_FALSE(document.at("draws").empty());
    EXPECT_EQ(document.at("draws").back().at("shader_variant"),
              excessive_path ? "static_mesh_identity_path_limit"
                             : "diagnostic_static_mesh_index_count");
    ExpectEntryState(true);
}

TEST_F(DiagnosticAttributionGl, InvalidIndexCountRecordsIncompleteCoverageWithoutDrawing) {
    ExpectInvalidMetadata(false);
}

TEST_F(DiagnosticAttributionGl, ExcessivePathRecordsIncompleteCoverageWithoutDrawing) {
    ExpectInvalidMetadata(true);
}

TEST_F(DiagnosticAttributionGl, JsonBudgetRefusalIsStickyWithoutGrowingOrThrowing) {
    GBufferDiagnosticCapture capture(table_);
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
        GBufferDiagnosticCapture capture(table_);
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
