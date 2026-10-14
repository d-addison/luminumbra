#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarVolumeGlBackend.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/Shader.h"
#include "luminumbra_client/rendering/passes/FarWorldPassUniforms.h"

#include <gtest/gtest.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <glad/glad.h>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <thread>
#include <vector>

namespace Luminumbra::Rendering {
struct WaterUploadTestPeer {
    static void Upload(RenderPipeline& pipeline,
                       u32 version,
                       std::size_t count,
                       FarWorldMode effective = FarWorldMode::Ladder) {
        pipeline.m_frame_far_selection.effective = effective;
        RenderPipeline::ChunkMeshSnapshot chunk;
        chunk.id = 42;
        chunk.water_mesh_version = version;
        RenderPipeline::ChunkMeshPayload payload;
        payload.mesh_version = version;
        payload.vertices.resize(count);
        payload.indices.resize(count);
        pipeline.upload_water_mesh(chunk, payload);
    }
    static const WaterRenderData& Data(const RenderPipeline& pipeline) {
        return pipeline.m_water_render_data.at(42);
    }
    static FarWorldLiveUpload LiveUpload(const RenderPipeline& pipeline, u32 version) {
        const auto& data = Data(pipeline);
        FarWorldLiveUpload upload;
        upload.cell = {-16, 0, -16, 0};
        data.SetLadderWaterUpload(upload, version, true);
        return upload;
    }
};
} // namespace Luminumbra::Rendering

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;

// Intercept only the requested live-water write, producing a real GL error.
// Restore the loader entry points before any far-volume upload or submission.
class FailedWaterUpload {
public:
    FailedWaterUpload(bool growth, GLenum failed_target)
        : original_data_(glBufferData)
        , original_sub_data_(glBufferSubData)
        , failed_target_(failed_target) {
        active_ = this;
        if (growth)
            glad_glBufferData = BufferData;
        else
            glad_glBufferSubData = BufferSubData;
    }
    ~FailedWaterUpload() {
        glad_glBufferData = original_data_;
        glad_glBufferSubData = original_sub_data_;
        active_ = nullptr;
    }
    int failures() const {
        return failures_;
    }

private:
    static void GLAPIENTRY BufferData(GLenum buffer_target,
                                      GLsizeiptr bytes,
                                      const void* pixels,
                                      GLenum usage) {
        if (buffer_target == active_->failed_target_) {
            ++active_->failures_;
            bytes = -1;
        }
        active_->original_data_(buffer_target, bytes, pixels, usage);
    }
    static void GLAPIENTRY BufferSubData(GLenum buffer_target,
                                         GLintptr offset,
                                         GLsizeiptr bytes,
                                         const void* pixels) {
        if (buffer_target == active_->failed_target_) {
            ++active_->failures_;
            bytes = -1;
        }
        active_->original_sub_data_(buffer_target, offset, bytes, pixels);
    }
    PFNGLBUFFERDATAPROC original_data_;
    PFNGLBUFFERSUBDATAPROC original_sub_data_;
    GLenum failed_target_;
    int failures_ = 0;
    static inline FailedWaterUpload* active_ = nullptr;
};

class Sea final : public FarWorldPristineFixture {
public:
    std::uint64_t content_identity() const noexcept override {
        return 20042;
    }
    std::uint64_t owned_bytes() const noexcept override {
        return sizeof(*this);
    }
    FarWorldWaterCapability WaterCapability() const noexcept override {
        return FarWorldWaterCapability::Wet;
    }
    std::optional<FarWorldWaterBound> WaterBound(const FarTileKey&) const override {
        return FarWorldWaterBound{false, SEA_LEVEL, SEA_LEVEL};
    }
    std::optional<FarWorldWaterSample> WaterSample(std::int64_t, std::int64_t) const override {
        return FarWorldWaterSample{true, SEA_LEVEL};
    }
    std::optional<float> Height(std::int64_t, std::int64_t) const override {
        return 0.0f;
    }
    std::optional<FarVolumeSample>
    Sample(const FarVolumePosition&, float, std::uint32_t, FarCaveMode) const override {
        return FarVolumeSample{3, 1};
    }
};
class FarWorldWaterGl : public ::testing::Test {
protected:
    void SetUp() override {
        initialized = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(9, 9, "far_water", nullptr, nullptr);
        ASSERT_NE(window, nullptr);
        glfwMakeContextCurrent(window);
        loaded = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
        ASSERT_TRUE(loaded);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const std::array<GLint, 5> formats{GL_RGB16F, GL_RGBA8, GL_RGBA8, GL_RG16F, GL_RG16F};
        const std::array<GLenum, 5> channels{GL_RGB, GL_RGBA, GL_RGBA, GL_RG, GL_RG};
        std::array<GLenum, 5> buffers{};
        for (std::size_t i = 0; i < formats.size(); ++i) {
            const auto texture = Texture(GL_TEXTURE_2D, formats[i], channels[i], 9, 9, nullptr);
            buffers[i] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
            glFramebufferTexture2D(GL_FRAMEBUFFER, buffers[i], GL_TEXTURE_2D, texture, 0);
        }
        glDrawBuffers(5, buffers.data());
        const auto depth =
            Texture(GL_TEXTURE_2D, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, 9, 9, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
                  static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, 9, 9);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glClearDepth(0);
        glClearColor(0, 0, 0, 0);
    }
    void TearDown() override {
        if (loaded) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindVertexArray(0);
            glUseProgram(0);
            glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
            glDeleteFramebuffers(1, &fbo);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }
        if (window)
            glfwDestroyWindow(window);
        if (initialized)
            glfwTerminate();
    }
    GLuint Texture(
        GLenum target, GLint format, GLenum channels, int width, int height, const void* pixels) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        glBindTexture(target, texture);
        if (target == GL_TEXTURE_2D)
            glTexImage2D(target, 0, format, width, height, 0, channels, GL_FLOAT, pixels);
        else
            glTexImage3D(target, 0, format, width, height, 1, 0, channels, GL_FLOAT, pixels);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return texture;
    }
    void Configure(Shader& shader,
                   const glm::vec3& eye,
                   const glm::vec3& look_at_target = {-248, 0, -248}) {
        shader.use();
        std::array<glm::vec4, 256 * 4> lut{};
        for (int id = 0; id < 256; ++id)
            lut[id] = {0, 1, 1, 0};
        int unit = 0;
        for (const auto& sampler : shader.Reflected().samplers) {
            glActiveTexture(GL_TEXTURE0 + unit);
            const bool material = sampler.name == "u_materialLUT";
            const auto texture_target =
                sampler.type == GL_SAMPLER_2D_ARRAY ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
            Texture(texture_target,
                    GL_RGBA32F,
                    GL_RGBA,
                    material ? 256 : 1,
                    material ? 4 : 1,
                    material ? lut.data() : nullptr);
            shader.setInt(sampler.name, unit++);
        }
        const auto view = glm::lookAt(eye, look_at_target, glm::vec3(0, 1, 0));
        shader.setMat4("view", view);
        shader.setMat4("projection", ReversedZPerspective(glm::radians(45.0f), 1, .1f, 17408));
        shader.setMat4("model", glm::mat4(1));
        shader.setMat3("normalMatrix", glm::mat3(view));
        shader.setMat3("u_normalViewMatrix", glm::mat3(view));
        shader.setInt("u_forceFlat", 1);
    }
    glm::vec4 Pixel(int attachment) {
        glm::vec4 pixel{};
        glReadBuffer(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(attachment));
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_FLOAT, glm::value_ptr(pixel));
        return pixel;
    }
    GLFWwindow* window = nullptr;
    bool initialized = false, loaded = false;
    GLuint fbo = 0;
    std::vector<GLuint> textures;
};

TEST_F(FarWorldWaterGl, ProductionUploadedSeaPixelsOwnershipArrivalAndActualSubmission) {
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
    Shader shader((root / "g_buffer.vert").string().c_str(),
                  (root / "g_buffer.frag").string().c_str(),
                  {},
                  {"LUMINUMBRA_FAR_WORLD_LADDER 1"});
    ASSERT_TRUE(shader.IsValid()) << shader.Diagnostic();
    std::array<double, 3> eye{0, 256, 0};
    Configure(shader, {0, 256, 0});
    FarWorldPassUniforms uniforms;
    uniforms.Cache(shader);
    JobSystem jobs;
    jobs.startup(2);
    FarWorldRenderer renderer(FarWorldMode::Ladder);
    renderer.InstallBackend(std::make_unique<FarVolumeGlDriver>());
    ASSERT_EQ(renderer.BindFixture(
                  jobs, std::make_shared<const Sea>(), 1, 1, FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    const FarVolumeWantedRequest request{
        {1, 20042, 0, {2, -1, -1}, 1, 1}, {{2, -1, -1}, FarVolumeCoverageSpan{-32, 256}}, 0};
    std::uint64_t frame = 0;
    bool settled = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        ASSERT_TRUE(renderer.AdvanceFrame(++frame, eye, std::span(&request, 1)));
        const auto observation = renderer.Residency()->Observe(request.identity.key);
        ASSERT_TRUE(observation);
        if (observation) {
            settled = observation->eligibility == FarVolumeEligibility::Settled;
        }
        if (!settled)
            std::this_thread::yield();
    } while (!settled && std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(settled);
    const auto draw = [&](bool terrain_ready, bool water_ready, bool arrival, bool clipped) {
        renderer.BeginLiveOwnership(frame);
        FarWorldLiveUpload upload;
        upload.cell = {-16, 0, -16, 0};
        upload.snapshot_version = upload.uploaded_version = 7;
        upload.uploaded_indices = 3;
        upload.valid_allocation = terrain_ready;
        upload.water_snapshot_version = upload.water_uploaded_version = 9;
        upload.water_uploaded_indices = 6;
        upload.water_valid_allocation = water_ready;
        upload.water_pass_eligible = water_ready;
        EXPECT_TRUE(renderer.AddLiveUpload(upload));
        EXPECT_TRUE(renderer.FinishLiveOwnership());
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        FarWorldPassUniformScope reset(uniforms);
        return renderer.Submit(eye, [&](FarWorldDrawCommand command) {
            if (arrival) {
                command.arrival_eligible = true;
                command.finer_outer_radius_meters = 1040;
                command.child_local_y_spans.fill({0, 1024});
            }
            if (clipped)
                command.horizontal_outer_clip_meters = 200;
            return uniforms.Prepare(command);
        });
    };
    const auto submitted = draw(true, false, false, false);
    EXPECT_EQ(submitted.draws, 1u);
    EXPECT_EQ(submitted.water_indices, 98304u);
    const auto normal = Pixel(1), albedo = Pixel(2), material = Pixel(3);
    EXPECT_NEAR(normal.a * 255, 200, .01f);
    EXPECT_LT(material.g, 0);
    EXPECT_GT(albedo.b, albedo.r);
    // The center ray hits the unbiased SEA_LEVEL target. Tier 2's spacing/2
    // bias is 4 m away along that ray, with the actual zero-to-one projection.
    float depth = 0;
    glReadPixels(4, 4, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    const auto distance = glm::length(glm::vec3(-248, -256, -248));
    const auto clip = ReversedZPerspective(glm::radians(45.0f), 1, .1f, 17408) *
                      glm::vec4(0, 0, -distance - 4, 1);
    EXPECT_NEAR(depth, clip.z / clip.w, 1e-7f);
    renderer.CaptureDiagnostic(renderer.Selection(FarWorldPurpose::World),
                               frame,
                               eye,
                               17408,
                               true,
                               submitted.draws,
                               submitted.indices);
    const auto on = renderer.Diagnostic();
    EXPECT_EQ(on.tiers[1].resident.submitted_water_indices, submitted.water_indices);
    EXPECT_EQ(on.tiers[1].resident.retained_water_indices, submitted.water_indices);
    EXPECT_EQ(on.tiles[0].terrain_indices, 0u);
    for (int control = 0; control < 3; ++control) {
        draw(false, control == 0, control == 1, control == 2);
        EXPECT_EQ(Pixel(3).g, 0) << control;
    }
    eye = {0, 64, 0};
    Configure(shader, {0, 64, 0}, {-64, 0, -64});
    EXPECT_EQ(draw(false, false, false, false).water_indices, 98304u);
    EXPECT_EQ(Pixel(3).g, 0); // within the exact 176 m inner sphere
    EXPECT_EQ(renderer.Diagnostic().frame, on.frame);
    renderer.Shutdown();
    jobs.shutdown();
}

class FarWorldWaterUploadGl : public FarWorldWaterGl, public ::testing::WithParamInterface<GLenum> {
protected:
    void CheckFailure(bool growth) {
        RenderPipeline pipeline;
        const std::size_t old_count = growth ? 3u : 6u;
        const std::size_t new_count = growth ? 6u : 3u;
        WaterUploadTestPeer::Upload(pipeline, 9, old_count);
        ASSERT_TRUE(WaterUploadTestPeer::Data(pipeline).LadderUploadValid());
        EXPECT_TRUE(WaterUploadTestPeer::LiveUpload(pipeline, 9).water_pass_eligible);
        // Even reuse that leaves the store size intact must lose its certificate.
        {
            FailedWaterUpload failure(growth, GetParam());
            WaterUploadTestPeer::Upload(pipeline, 10, new_count);
            EXPECT_EQ(failure.failures(), 1);
        }
        const auto& failed_data = WaterUploadTestPeer::Data(pipeline);
        EXPECT_NE(failed_data.vao_id, 0u);
        EXPECT_NE(failed_data.vbo_id, 0u);
        EXPECT_NE(failed_data.ebo_id, 0u);
        EXPECT_FALSE(failed_data.ladder_upload_version.has_value());
        EXPECT_FALSE(failed_data.LadderUploadValid());
        EXPECT_EQ(failed_data.mesh_version, 9u);
        EXPECT_EQ(failed_data.element_count, old_count);
        const auto failed_upload = WaterUploadTestPeer::LiveUpload(pipeline, 10);
        EXPECT_FALSE(failed_upload.water_pass_eligible);
        EXPECT_FALSE(IsFarWorldLiveWaterUploadCurrent(failed_upload));
        // A damaged previous version cannot reclaim ownership either.
        EXPECT_FALSE(WaterUploadTestPeer::LiveUpload(pipeline, 9).water_pass_eligible);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        CheckFallbackPixels(pipeline, failed_upload, new_count);
    }

    void CheckFallbackPixels(RenderPipeline& pipeline,
                             const FarWorldLiveUpload& failed_upload,
                             std::size_t new_count) {
        const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
        Shader shader((root / "g_buffer.vert").string().c_str(),
                      (root / "g_buffer.frag").string().c_str(),
                      {},
                      {"LUMINUMBRA_FAR_WORLD_LADDER 1"});
        ASSERT_TRUE(shader.IsValid()) << shader.Diagnostic();
        Configure(shader, {0, 256, 0});
        FarWorldPassUniforms uniforms;
        uniforms.Cache(shader);
        JobSystem jobs;
        jobs.startup(2);
        FarWorldRenderer renderer(FarWorldMode::Ladder);
        renderer.InstallBackend(std::make_unique<FarVolumeGlDriver>());
        ASSERT_EQ(renderer.BindFixture(
                      jobs, std::make_shared<const Sea>(), 1, 1, FarWorldDevelopmentLimits()),
                  FarWorldRefusal::None);
        const std::array<double, 3> eye{0, 256, 0};
        const FarVolumeWantedRequest request{
            {1, 20042, 0, {2, -1, -1}, 1, 1}, {{2, -1, -1}, FarVolumeCoverageSpan{-32, 256}}, 0};
        std::uint64_t frame = 0;
        bool settled = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            ASSERT_TRUE(renderer.AdvanceFrame(++frame, eye, std::span(&request, 1)));
            const auto observation = renderer.Residency()->Observe(request.identity.key);
            ASSERT_TRUE(observation);
            if (!observation)
                return;
            settled = observation->eligibility == FarVolumeEligibility::Settled;
            if (!settled)
                std::this_thread::yield();
        } while (!settled && std::chrono::steady_clock::now() < deadline);
        ASSERT_TRUE(settled);
        const auto draw = [&](const FarWorldLiveUpload& upload) {
            renderer.BeginLiveOwnership(frame);
            EXPECT_TRUE(renderer.AddLiveUpload(upload));
            EXPECT_TRUE(renderer.FinishLiveOwnership());
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            FarWorldPassUniformScope reset(uniforms);
            return renderer.Submit(
                eye, [&](const FarWorldDrawCommand& command) { return uniforms.Prepare(command); });
        };
        // A terrain-ready cell still has no water bit after either failed write.
        auto upload = failed_upload;
        upload.snapshot_version = upload.uploaded_version = 7;
        upload.uploaded_indices = 3;
        upload.valid_allocation = true;
        const auto submitted = draw(upload);
        ASSERT_EQ(renderer.LiveOwnership().cells().size(), 1u);
        EXPECT_EQ(renderer.LiveOwnership().cells()[0].reserved, kFarWorldTerrainReady);
        EXPECT_FALSE(renderer.LiveOwnership().OwnsWater({-248, SEA_LEVEL, -248}));
        EXPECT_EQ(submitted.draws, 1u);
        EXPECT_EQ(submitted.water_indices, 98304u);
        EXPECT_NEAR(Pixel(1).a * 255, 200, .01f);
        EXPECT_LT(Pixel(3).g, 0);
        EXPECT_GT(Pixel(2).b, Pixel(2).r);

        // A successful retry certifies only version 10 and suppresses the sheet.
        WaterUploadTestPeer::Upload(pipeline, 10, new_count);
        ASSERT_TRUE(WaterUploadTestPeer::Data(pipeline).LadderUploadValid());
        upload = WaterUploadTestPeer::LiveUpload(pipeline, 10);
        EXPECT_TRUE(upload.water_pass_eligible);
        EXPECT_FALSE(WaterUploadTestPeer::LiveUpload(pipeline, 9).water_pass_eligible);
        draw(upload);
        ASSERT_EQ(renderer.LiveOwnership().cells().size(), 1u);
        EXPECT_EQ(renderer.LiveOwnership().cells()[0].reserved, kFarWorldWaterReady);
        EXPECT_TRUE(renderer.LiveOwnership().OwnsWater({-248, SEA_LEVEL, -248}));
        EXPECT_EQ(Pixel(3).g, 0);
        renderer.Shutdown();
        jobs.shutdown();
    }
};

TEST_P(FarWorldWaterUploadGl, GrowthFailureClearsWaterBitAndPreservesFarPixels) {
    CheckFailure(true);
}
TEST_P(FarWorldWaterUploadGl, ReuseFailureClearsWaterBitAndPreservesFarPixels) {
    CheckFailure(false);
}
INSTANTIATE_TEST_SUITE_P(VertexAndIndexWrites,
                         FarWorldWaterUploadGl,
                         ::testing::Values(static_cast<GLenum>(GL_ARRAY_BUFFER),
                                           static_cast<GLenum>(GL_ELEMENT_ARRAY_BUFFER)));

TEST_F(FarWorldWaterGl, LegacyUploadDoesNotPublishLadderCertificate) {
    RenderPipeline pipeline;
    pipeline.set_far_world_mode(FarWorldMode::Legacy);
    WaterUploadTestPeer::Upload(pipeline, 9, 3, FarWorldMode::Legacy);
    const auto& data = WaterUploadTestPeer::Data(pipeline);
    EXPECT_EQ(data.mesh_version, 9u);
    EXPECT_EQ(data.element_count, 3u);
    EXPECT_FALSE(data.LadderUploadValid());
    EXPECT_FALSE(data.ladder_upload_version.has_value());
    WaterUploadTestPeer::Upload(pipeline, 9, 3);
    EXPECT_TRUE(data.LadderUploadValid());
    WaterUploadTestPeer::Upload(pipeline, 9, 3, FarWorldMode::Legacy);
    EXPECT_EQ(data.mesh_version, 9u);
    EXPECT_EQ(data.element_count, 3u);
    EXPECT_FALSE(data.LadderUploadValid());
}
} // namespace
