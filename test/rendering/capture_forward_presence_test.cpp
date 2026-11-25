#include "rendering/DiagnosticContributions.h"

#include <gtest/gtest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
using namespace Luminumbra::Rendering;
constexpr DiagnosticExtent kExtent{4, 1};
constexpr std::array<std::string_view, 4> kProducers{
    "water", "waterfall", "magical_particles", "foliage_cpu"};
constexpr std::array<ForwardFeature, 4> kFeatures{ForwardFeature::WaterSurface,
                                                  ForwardFeature::Waterfall,
                                                  ForwardFeature::Particles,
                                                  ForwardFeature::Foliage};

ForwardFeatureSettings EnabledSettings() {
    ForwardFeatureSettings settings;
    settings.fill(true);
    return settings;
}

DiagnosticSubmission Submission(std::string_view producer) {
    return {.mesh_path = {},
            .shader_variant = producer,
            .chunk = {},
            .lod = 0,
            .ownership = DiagnosticOwnership::Live,
            .pass = DiagnosticPass::Forward,
            .provenance = DiagnosticProvenance::Mesh,
            .generated_mesh = "fixture/quad"};
}

ForwardFeatureEvidence Row(const DiagnosticForwardPresence& presence, ForwardFeature feature) {
    return presence.evidence()[static_cast<std::size_t>(feature)];
}

TEST(ForwardPresenceJoin, PresentAbsentOccludedPartialAndDisabledForEachProducer) {
    for (std::size_t producer = 0; producer < kProducers.size(); ++producer) {
        for (int scenario = 0; scenario < 5; ++scenario) {
            SCOPED_TRACE(::testing::Message() << kProducers[producer] << " scenario " << scenario);
            auto settings = EnabledSettings();
            settings[static_cast<std::size_t>(kFeatures[producer])] = scenario != 4;
            DiagnosticForwardPresence presence(7, 11, kExtent, settings);
            DiagnosticDrawTable table;
            DiagnosticId id{};
            std::array<DiagnosticId, 4> plane{};
            const bool submitted = scenario != 1 && scenario != 4;
            if (submitted) {
                const auto submission = Submission(kProducers[producer]);
                ASSERT_TRUE(table.submit(submission, id));
                presence.submit(submission, id);
                if (scenario == 0)
                    plane.fill(id);
                if (scenario == 3)
                    plane[0] = id;
            }
            ASSERT_TRUE(presence.join(table, plane, kExtent, 7, 11));
            const auto row = Row(presence, kFeatures[producer]);
            EXPECT_EQ(row.submitted_draws, submitted ? 1u : 0u);
            EXPECT_EQ(row.visible_draws, scenario == 0 || scenario == 3 ? 1u : 0u);
            EXPECT_EQ(row.contributor_samples, scenario == 0 ? 4u : scenario == 3 ? 1u : 0u);
            constexpr std::array expected{ForwardPresenceStatus::Present,
                                          ForwardPresenceStatus::Absent,
                                          ForwardPresenceStatus::Occluded,
                                          ForwardPresenceStatus::Present,
                                          ForwardPresenceStatus::Disabled};
            EXPECT_EQ(row.status, expected[static_cast<std::size_t>(scenario)]);
        }
    }
}

TEST(ForwardPresenceJoin, RiverWaterfallAndSprayNeverBorrowEachOthersEvidence) {
    DiagnosticForwardPresence presence(7, 11, kExtent, EnabledSettings());
    DiagnosticDrawTable table;
    std::array<DiagnosticId, 4> plane{};
    for (std::size_t producer = 0; producer < 3; ++producer) {
        const auto submission = Submission(kProducers[producer]);
        ASSERT_TRUE(table.submit(submission, plane[producer]));
        presence.submit(submission, plane[producer]);
    }
    ASSERT_TRUE(presence.join(table, plane, kExtent, 7, 11));
    for (const auto feature : {ForwardFeature::River, ForwardFeature::Spray}) {
        const auto row = Row(presence, feature);
        EXPECT_EQ(row.status, ForwardPresenceStatus::Unsupported);
        EXPECT_EQ(row.submitted_draws, 0u);
        EXPECT_EQ(row.contributor_samples, 0u);
        EXPECT_FALSE(row.reason.empty());
    }
    EXPECT_EQ(Row(presence, ForwardFeature::WaterSurface).contributor_samples, 1u);
    EXPECT_EQ(Row(presence, ForwardFeature::Waterfall).contributor_samples, 1u);
    EXPECT_EQ(Row(presence, ForwardFeature::Particles).contributor_samples, 1u);
    EXPECT_EQ(Row(presence, ForwardFeature::Foliage).status, ForwardPresenceStatus::Absent);
}

TEST(ForwardPresenceJoin, EveryFoliageSiblingJoinsAndUninstrumentedLabelsDoNot) {
    for (const auto label : {"foliage_gpu",
                             "foliage_cpu",
                             "foliage_qualification_prefix",
                             "foliage_qualification_sample",
                             "foliage_qualification_suffix"}) {
        EXPECT_EQ(DiagnosticForwardPresence::classify(label), ForwardFeature::Foliage);
    }
    for (const auto label : {"water:contributor_only",
                             "waterfall:contributor_only",
                             "magical_particles:contributor_only",
                             "foliage_cpu:contributor_only",
                             "weather_overlay",
                             "glass_oit",
                             "unknown",
                             ""}) {
        EXPECT_FALSE(DiagnosticForwardPresence::classify(label).has_value());
    }
}

TEST(ForwardPresenceJoin, PoolOnlyPixelsCannotCertifyWaterfallSheets) {
    DiagnosticForwardPresence presence(7, 11, kExtent, EnabledSettings());
    DiagnosticDrawTable table;
    auto sheet = Submission("waterfall");
    sheet.generated_mesh = "waterfall/sheet-pool/0";
    auto pool = sheet;
    pool.generated_mesh = "waterfall/sheet-pool/0/pool";
    DiagnosticId sheet_id{};
    DiagnosticId pool_id{};
    ASSERT_TRUE(table.submit(sheet, sheet_id));
    presence.submit(sheet, sheet_id);
    ASSERT_TRUE(table.submit(pool, pool_id));
    presence.submit(pool, pool_id);
    ASSERT_TRUE(
        presence.join(table, std::array{pool_id, pool_id, pool_id, pool_id}, kExtent, 7, 11));
    const auto row = Row(presence, ForwardFeature::Waterfall);
    EXPECT_EQ(row.submitted_draws, 1u);
    EXPECT_EQ(row.visible_draws, 0u);
    EXPECT_EQ(row.contributor_samples, 0u);
    EXPECT_EQ(row.status, ForwardPresenceStatus::Occluded);
}

TEST(ForwardPresenceJoin, UnknownDisabledContradictoryUnsampledAndFailedCaptureRefuse) {
    DiagnosticDrawTable table;
    DiagnosticId id{};
    const auto submission = Submission("water");
    ASSERT_TRUE(table.submit(submission, id));
    const std::array plane{id, id, id, id};
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto settings = EnabledSettings();
        if (scenario == 0)
            settings[0] = std::nullopt;
        if (scenario == 1)
            settings[0] = false;
        DiagnosticForwardPresence presence(7, 11, kExtent, settings);
        presence.submit(submission, id);
        if (scenario != 2) {
            ASSERT_TRUE(presence.join(table, plane, kExtent, 7, 11));
        }
        if (scenario == 3)
            presence.unavailable();
        EXPECT_EQ(Row(presence, ForwardFeature::WaterSurface).status,
                  ForwardPresenceStatus::Unsupported);
    }
}

TEST(ForwardPresenceJoin, RefusesWrongFrameTickExtentPartialUnresolvedAndMismatchedPlanes) {
    for (int scenario = 0; scenario < 8; ++scenario) {
        SCOPED_TRACE(scenario);
        DiagnosticForwardPresence presence(7, 11, kExtent, EnabledSettings());
        DiagnosticDrawTable table;
        DiagnosticId id{};
        const auto submission = Submission("water");
        ASSERT_TRUE(table.submit(submission, id));
        presence.submit(submission, id);
        std::array plane{id, id, id, id};
        if (scenario == 4)
            plane.back()[0] = 65537;
        if (scenario == 5)
            ++plane.back()[1];
        if (scenario == 6)
            plane.back() = {0, 1, 0, 0};
        if (scenario == 7) {
            auto unsupported = submission;
            unsupported.provenance = DiagnosticProvenance::Unsupported;
            DiagnosticId ignored{};
            ASSERT_TRUE(table.submit(unsupported, ignored));
        }
        EXPECT_FALSE(presence.join(table,
                                   std::span(plane).first(scenario == 3 ? 3u : 4u),
                                   scenario == 2 ? DiagnosticExtent{0, 1} : kExtent,
                                   scenario == 0 ? 8 : 7,
                                   scenario == 1 ? 12 : 11));
        const auto row = Row(presence, ForwardFeature::WaterSurface);
        EXPECT_EQ(row.status, ForwardPresenceStatus::Unsupported);
        EXPECT_EQ(row.contributor_samples, 0u); // No prefix commits on malformed tails.
        EXPECT_EQ(row.visible_draws, 0u);
    }
}

TEST(ForwardPresenceJoin, StagesPreserveEarlierFeaturesWithoutDoubleCounting) {
    DiagnosticForwardPresence presence(7, 11, kExtent, EnabledSettings());
    DiagnosticDrawTable table;
    DiagnosticId water{};
    const auto water_submission = Submission("water");
    ASSERT_TRUE(table.submit(water_submission, water));
    presence.submit(water_submission, water);
    ASSERT_TRUE(presence.join(table, std::array{water, water, water, water}, kExtent, 7, 11));
    DiagnosticId particles{};
    const auto particle_submission = Submission("magical_particles");
    ASSERT_TRUE(table.submit(particle_submission, particles));
    presence.submit(particle_submission, particles);
    const std::array mixed{water, water, particles, particles};
    ASSERT_TRUE(presence.join(table, mixed, kExtent, 7, 11));
    ASSERT_TRUE(presence.join(table, mixed, kExtent, 7, 11));
    EXPECT_EQ(Row(presence, ForwardFeature::WaterSurface).contributor_samples, 4u);
    EXPECT_EQ(Row(presence, ForwardFeature::Particles).contributor_samples, 2u);
}

TEST(ForwardPresenceJoin, UsedAndFailedCollectorsCannotSeedAnotherCapture) {
    DiagnosticDrawTable table;
    const std::array<DiagnosticId, 4> plane{};
    DiagnosticForwardPresence sampled(7, 11, kExtent, EnabledSettings());
    EXPECT_TRUE(sampled.fresh());
    ASSERT_TRUE(sampled.join(table, plane, kExtent, 7, 11));
    EXPECT_FALSE(sampled.fresh());
    DiagnosticForwardPresence submitted(7, 11, kExtent, EnabledSettings());
    DiagnosticId id{};
    const auto submission = Submission("water");
    ASSERT_TRUE(table.submit(submission, id));
    submitted.submit(submission, id);
    EXPECT_FALSE(submitted.fresh());
    DiagnosticForwardPresence failed(7, 11, kExtent, EnabledSettings());
    failed.unavailable();
    EXPECT_FALSE(failed.fresh());
}

TEST(ForwardPresenceJoin, BoundsAndInvalidSubmissionsRefuseWithoutAliasingDraws) {
    for (int scenario = 0; scenario < 9; ++scenario) {
        DiagnosticForwardPresence presence(
            7, 11, scenario == 0 ? DiagnosticExtent{16384, 16384} : kExtent, EnabledSettings());
        auto submission = Submission("water");
        DiagnosticId id{scenario == 1 ? 65537u : scenario == 2 ? 0u : 1u, 1, 0, 513};
        if (scenario == 3)
            submission.pass = DiagnosticPass::GBuffer;
        if (scenario == 5)
            id[1] = 0;
        if (scenario == 6)
            id[2] = 1;
        if (scenario == 7)
            id[3] = 257;
        if (scenario == 8)
            submission.provenance = DiagnosticProvenance::Unsupported;
        presence.submit(submission, id);
        if (scenario == 4)
            presence.submit(submission, id);
        EXPECT_EQ(Row(presence, ForwardFeature::WaterSurface).status,
                  ForwardPresenceStatus::Unsupported);
        EXPECT_EQ(Row(presence, ForwardFeature::WaterSurface).submitted_draws,
                  scenario == 4 ? 1u : 0u);
    }
    DiagnosticForwardPresence presence(7, 11, kExtent, EnabledSettings());
    DiagnosticDrawTable table;
    const std::array<DiagnosticId, 4> background{};
    for (int stage = 0; stage < 64; ++stage) {
        ASSERT_TRUE(presence.can_sample());
        ASSERT_TRUE(presence.join(table, background, kExtent, 7, 11));
    }
    EXPECT_FALSE(presence.can_sample());
    EXPECT_FALSE(presence.join(table, background, kExtent, 7, 11));
    EXPECT_EQ(Row(presence, ForwardFeature::WaterSurface).status,
              ForwardPresenceStatus::Unsupported);
}

std::string ReadSource(std::string_view relative) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / relative, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string WhitespaceFree(std::string source) {
    std::erase_if(source, [](unsigned char value) { return std::isspace(value) != 0; });
    return source;
}

TEST(ForwardPresenceJoin, OriginalSubmissionAndStageReadbackRemainWired) {
    const auto targets = ReadSource("src/luminumbra_client/rendering/GBufferTargets.cpp");
    const auto waterfall = ReadSource("src/luminumbra_client/rendering/passes/WaterfallPass.cpp");
    EXPECT_NE(targets.find("state_->forward_presence->submit(submission, id)"), std::string::npos);
    EXPECT_NE(targets.find("state_->forward_presence->unavailable()"), std::string::npos);
    EXPECT_NE(targets.find("!forward_presence->fresh()"), std::string::npos);
    EXPECT_LT(targets.find("sample_forward_presence();"), targets.find("GL_TEXTURE_2D, 0, 0"));
    EXPECT_NE(targets.find("state_->forward_presence->join_stage(state_->table, plane, extent)"),
              std::string::npos);
    EXPECT_NE(targets.find("!state_->forward_presence->can_sample()"), std::string::npos);
    EXPECT_NE(waterfall.find("for (GLint part = 0; part < 2; ++part)"), std::string::npos);
    EXPECT_NE(waterfall.find("mesh_key += \"/pool\""), std::string::npos);
    EXPECT_NE(WhitespaceFree(waterfall).find("+part*6,6)"), std::string::npos);
    for (const auto token : {"GL_PACK_ALIGNMENT",
                             "GL_PACK_ROW_LENGTH",
                             "GL_PACK_IMAGE_HEIGHT",
                             "GL_PACK_SKIP_PIXELS",
                             "GL_PACK_SKIP_ROWS",
                             "GL_PACK_SKIP_IMAGES",
                             "GL_PACK_SWAP_BYTES",
                             "GL_PACK_LSB_FIRST",
                             "GL_PIXEL_PACK_BUFFER_BINDING"}) {
        EXPECT_NE(targets.find(token), std::string::npos);
    }
}

// Hidden GL fixtures use the shipping fragment contributors with fixed synthetic
// vertex inputs. No final-colour threshold, simulation, assets or time history.
class ForwardPresenceGl : public ::testing::Test {
protected:
    void SetUp() override {
        initialized_ = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized_);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window_ = glfwCreateWindow(32, 32, "forward presence", nullptr, nullptr);
        ASSERT_NE(window_, nullptr);
        glfwMakeContextCurrent(window_);
        ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glGenTextures(3, textures_.data());
        glBindTexture(GL_TEXTURE_2D, textures_[0]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32UI, 32, 32);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, textures_[0], 0);
        glBindTexture(GL_TEXTURE_2D, textures_[1]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 32, 32);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, textures_[1], 0);
        glBindTexture(GL_TEXTURE_2D, textures_[2]);
        const std::array<float, 4> zero{};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, zero.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const std::array<GLenum, 7> buffers{
            GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT6};
        glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        glViewport(0, 0, 32, 32);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
    }
    void TearDown() override {
        if (vao_ != 0) {
            glDeleteVertexArrays(1, &vao_);
            glDeleteTextures(3, textures_.data());
            glDeleteFramebuffers(1, &framebuffer_);
        }
        if (window_)
            glfwDestroyWindow(window_);
        if (initialized_)
            glfwTerminate();
    }
    static GLuint Compile(GLenum stage, const std::string& source) {
        const auto shader = glCreateShader(stage);
        const auto* bytes = source.c_str();
        glShaderSource(shader, 1, &bytes, nullptr);
        glCompileShader(shader);
        GLint success = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        EXPECT_EQ(success, GL_TRUE) << source;
        return shader;
    }
    static std::string Vertex(std::size_t producer) {
        const std::array<std::string_view, 4> outputs{
            "out VS_OUT { vec3 world_pos; vec3 world_normal; } v;",
            "out vec3 FragPos; out vec3 Normal;",
            "out VS_OUT { vec2 texCoord; vec4 color; flat float atlasLayer; flat uint ambientMote; "
            "float distanceToCamera; "
            "float viewDepth; vec3 worldPos; flat float streakAspect; } v;",
            "out VS_OUT { vec2 texCoord; vec4 color; float fade; float heightT; vec3 worldPos; "
            "vec3 worldNormal; } v;"};
        const std::array<std::string_view, 4> assignments{
            "v.world_pos=vec3(0,1,0); v.world_normal=vec3(0,1,0);",
            "FragPos=vec3(0,0.5,0); Normal=vec3(0,0,1);",
            "v.texCoord=vec2(0.5); v.color=vec4(1); v.atlasLayer=0; v.ambientMote=0u; "
            "v.distanceToCamera=1; "
            "v.viewDepth=1; v.worldPos=vec3(0); v.streakAspect=1;",
            "v.texCoord=vec2(0.5); v.color=vec4(1); v.fade=1; v.heightT=0.5; "
            "v.worldPos=vec3(0); v.worldNormal=vec3(0,1,0);"};
        return "#version 450 core\n" + std::string(outputs[producer]) +
               "\nvoid main(){ vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); "
               "gl_Position=vec4(p*2.0-1.0,0,1); " +
               std::string(assignments[producer]) + "}";
    }
    void Check(int scenario) {
        constexpr std::array<std::string_view, 4> fragments{
            "water.frag", "waterfall.frag", "magical_particles.frag", "foliage.frag"};
        for (std::size_t producer = 0; producer < fragments.size(); ++producer) {
            if (scenario == 5 && producer != 1) {
                continue;
            }
            SCOPED_TRACE(fragments[producer]);
            auto fragment = ReadSource("res/shaders/" + std::string(fragments[producer]));
            ASSERT_FALSE(fragment.empty());
            fragment.insert(fragment.find('\n') + 1, "#define LUMIN_DIAG_IDS 1\n");
            const auto vertex_shader = Compile(GL_VERTEX_SHADER, Vertex(producer));
            const auto fragment_shader = Compile(GL_FRAGMENT_SHADER, fragment);
            const auto program = glCreateProgram();
            glAttachShader(program, vertex_shader);
            glAttachShader(program, fragment_shader);
            glLinkProgram(program);
            glDeleteShader(vertex_shader);
            glDeleteShader(fragment_shader);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            EXPECT_EQ(linked, GL_TRUE);
            glUseProgram(program);
            glUniform2f(glGetUniformLocation(program, "u_screenSize"), 32, 32);
            glUniform2f(glGetUniformLocation(program, "u_screen_size"), 32, 32);
            glUniform1f(glGetUniformLocation(program, "u_nearPlane"), 0.1f);
            glUniform1f(glGetUniformLocation(program, "u_farPlane"), 100);
            glUniform1f(glGetUniformLocation(program, "u_crest_y"), 1);
            glUniform1f(glGetUniformLocation(program, "u_water_depth_scaler"), 0.1f);
            for (const auto name : {"u_cameraPos", "u_camera_pos"}) {
                glUniform3f(glGetUniformLocation(program, name), 0, 2, 2);
            }
            for (const auto name : {"u_sunDirection", "u_sun_direction"}) {
                glUniform3f(glGetUniformLocation(program, name), 0, -1, 0);
            }
            constexpr std::array<float, 16> identity{
                1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            for (const auto name :
                 {"u_inverse_view", "u_inverse_projection", "u_view_projection"}) {
                glUniformMatrix4fv(
                    glGetUniformLocation(program, name), 1, GL_FALSE, identity.data());
            }
            glClearTexImage(textures_[0], 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
            const float clear_depth = scenario == 2 || scenario == 5 ? 1.0f : 0.0f;
            glClearTexImage(textures_[1], 0, GL_DEPTH_COMPONENT, GL_FLOAT, &clear_depth);
            if (scenario == 3) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(0, 0, 16, 32);
                glDepthMask(GL_TRUE);
                const float occluder_depth = 1;
                glClearBufferfv(GL_DEPTH, 0, &occluder_depth);
                glDepthMask(GL_FALSE);
                glDisable(GL_SCISSOR_TEST);
            }
            auto settings = EnabledSettings();
            settings[static_cast<std::size_t>(kFeatures[producer])] = scenario != 4;
            DiagnosticForwardPresence presence(7, 11, {32, 32}, settings);
            DiagnosticDrawTable table;
            DiagnosticId id{};
            if (scenario != 1 && scenario != 4) {
                const auto submission = Submission(kProducers[producer]);
                EXPECT_TRUE(table.submit(submission, id));
                presence.submit(submission, id);
                const auto location = glGetUniformLocation(program, "u_diagnosticIds");
                EXPECT_GE(location, 0);
                glUniform4uiv(location, 1, id.data());
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            if (scenario == 5) {
                auto pool = Submission("waterfall");
                pool.generated_mesh = "waterfall/sheet-pool/0/pool";
                DiagnosticId pool_id{};
                ASSERT_TRUE(table.submit(pool, pool_id));
                presence.submit(pool, pool_id);
                const float open_depth = 0;
                glClearTexImage(textures_[1], 0, GL_DEPTH_COMPONENT, GL_FLOAT, &open_depth);
                glUniform4uiv(glGetUniformLocation(program, "u_diagnosticIds"), 1, pool_id.data());
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            std::array<DiagnosticId, 1024> plane{};
            glReadBuffer(GL_COLOR_ATTACHMENT6);
            glReadPixels(0, 0, 32, 32, GL_RGBA_INTEGER, GL_UNSIGNED_INT, plane.data());
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            EXPECT_TRUE(presence.join(table, plane, {32, 32}, 7, 11));
            const auto row = Row(presence, kFeatures[producer]);
            const auto expected = scenario == 1                    ? ForwardPresenceStatus::Absent
                                  : scenario == 2 || scenario == 5 ? ForwardPresenceStatus::Occluded
                                  : scenario == 4                  ? ForwardPresenceStatus::Disabled
                                                                   : ForwardPresenceStatus::Present;
            EXPECT_EQ(row.status, expected);
            EXPECT_EQ(row.contributor_samples, scenario == 0 ? 1024u : scenario == 3 ? 512u : 0u);
            glUseProgram(0);
            glDeleteProgram(program);
        }
    }

private:
    bool initialized_ = false;
    GLFWwindow* window_ = nullptr;
    GLuint vao_ = 0;
    GLuint framebuffer_ = 0;
    std::array<GLuint, 3> textures_{};
};

TEST_F(ForwardPresenceGl, Present) {
    Check(0);
}
TEST_F(ForwardPresenceGl, Absent) {
    Check(1);
}
TEST_F(ForwardPresenceGl, FullyOccluded) {
    Check(2);
}
TEST_F(ForwardPresenceGl, PartiallyOccluded) {
    Check(3);
}
TEST_F(ForwardPresenceGl, DisabledFeature) {
    Check(4);
}
TEST_F(ForwardPresenceGl, PoolOnlyIsNotSheetPresence) {
    Check(5);
}
} // namespace
