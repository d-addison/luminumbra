#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarVolumeGlBackend.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_client/rendering/FrameScan.h"
#include "luminumbra_client/rendering/PixelIo.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/Shader.h"
#include "luminumbra_client/rendering/UploadBudget.h"
#include "luminumbra_client/rendering/passes/ChunkGeometryPool.h"
#include "luminumbra_client/rendering/passes/FarWorldPassUniforms.h"

#include "../support/UniqueTempPath.h"

#include <gtest/gtest.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <glad/glad.h>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iterator>
#include <limits>
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
        for (std::size_t index = 0; index < count; ++index) {
            payload.vertices[index].position = {
                static_cast<float>(version), static_cast<float>(index), 0};
            payload.indices[index] = static_cast<u32>(index);
        }
        pipeline.upload_water_mesh(chunk, payload);
    }
    static void AdoptEmpty(RenderPipeline& pipeline, FarWorldMode mode) {
        Chunk source(IVec3{0, 0, 0});
        source.water_mesh_version.store(10);
        RenderPipeline::ChunkMeshSnapshot snapshot;
        snapshot.id = 42;
        snapshot.source_chunk = &source;
        snapshot.water_mesh_version = 10;
        pipeline.m_frame_far_selection.effective = mode;
        Camera camera;
        pipeline.manage_water_gpu_resources({snapshot}, camera);
    }
    static void DeferEmpty(RenderPipeline& pipeline, FarWorldMode mode) {
        Chunk dry(IVec3{0, 0, 0});
        dry.water_mesh_version.store(10);
        Chunk wet(IVec3{0, 0, 0});
        wet.water_mesh_version.store(10);
        wet.water_mesh_vertices.resize(3);
        wet.water_mesh_indices.resize(3);
        std::vector<RenderPipeline::ChunkMeshSnapshot> snapshots;
        // Eight new chunks precede the empty replacement in the production
        // queue. The unchanged count ceiling must defer its removal.
        for (ChunkID id = 1; id <= 8; ++id) {
            RenderPipeline::ChunkMeshSnapshot snapshot;
            snapshot.id = id;
            snapshot.source_chunk = &wet;
            snapshot.water_mesh_version = 10;
            snapshot.water_vertex_count = snapshot.water_index_count = 3;
            snapshots.push_back(snapshot);
        }
        RenderPipeline::ChunkMeshSnapshot empty;
        empty.id = 42;
        empty.source_chunk = &dry;
        empty.water_mesh_version = 10;
        snapshots.push_back(empty);
        pipeline.m_frame_far_selection.effective = mode;
        Camera camera;
        pipeline.manage_water_gpu_resources(snapshots, camera);
    }
    static bool HasWater(const RenderPipeline& pipeline) {
        return pipeline.m_water_render_data.contains(42);
    }
    static FarWorldLiveUpload PresentedUpload(const RenderPipeline& pipeline) {
        FarWorldLiveUpload upload;
        upload.cell = {-16, 0, -16, 0};
        Data(pipeline).SetPresentedLadderWaterUpload(upload, true);
        return upload;
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

// Exercise production pool growth with real GL allocation/mapping errors.
// Only loader entry points are intercepted; no production injection hooks.
class FailedPoolBlock {
public:
    FailedPoolBlock(GLenum target, bool storage)
        : original_storage_(glBufferStorage)
        , original_map_(glMapBufferRange)
        , target_(target)
        , storage_(storage) {
        active_ = this;
        glad_glBufferStorage = BufferStorage;
        glad_glMapBufferRange = MapBufferRange;
    }
    ~FailedPoolBlock() {
        glad_glBufferStorage = original_storage_;
        glad_glMapBufferRange = original_map_;
        active_ = nullptr;
    }
    std::array<GLuint, 2> buffers{};
    GLuint vao = 0;
    unsigned failures = 0;

private:
    static void GLAPIENTRY BufferStorage(GLenum target,
                                         GLsizeiptr bytes,
                                         const void* data,
                                         GLbitfield flags) {
        GLint name = 0;
        glGetIntegerv(target == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING
                                                : GL_ELEMENT_ARRAY_BUFFER_BINDING,
                      &name);
        active_->buffers[target == GL_ARRAY_BUFFER ? 0u : 1u] = static_cast<GLuint>(name);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &name);
        active_->vao = static_cast<GLuint>(name);
        if (active_->storage_ && target == active_->target_) {
            ++active_->failures;
            bytes = -1;
        }
        active_->original_storage_(target, bytes, data, flags);
    }
    static void* GLAPIENTRY MapBufferRange(GLenum target,
                                           GLintptr offset,
                                           GLsizeiptr bytes,
                                           GLbitfield flags) {
        if (!active_->storage_ && target == active_->target_) {
            ++active_->failures;
            bytes = -1;
        }
        return active_->original_map_(target, offset, bytes, flags);
    }
    PFNGLBUFFERSTORAGEPROC original_storage_;
    PFNGLMAPBUFFERRANGEPROC original_map_;
    GLenum target_;
    bool storage_;
    static inline FailedPoolBlock* active_ = nullptr;
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
        for (std::size_t id = 0; id < 256; ++id)
            lut[id] = {0, 1, 1, 0};
        int unit = 0;
        for (const auto& sampler : shader.Reflected().samplers) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            const bool material = sampler.name == "u_materialLUT";
            const GLenum texture_target =
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
        if (!growth)
            WaterUploadTestPeer::Upload(pipeline, 8, old_count);
        WaterUploadTestPeer::Upload(pipeline, 9, old_count);
        ASSERT_TRUE(WaterUploadTestPeer::Data(pipeline).LadderUploadValid());
        EXPECT_TRUE(WaterUploadTestPeer::LiveUpload(pipeline, 9).water_pass_eligible);
        const auto old_data = WaterUploadTestPeer::Data(pipeline);
        // Replacement writes target a staging slot, preserving resident bytes.
        {
            FailedWaterUpload failure(growth, GetParam());
            WaterUploadTestPeer::Upload(pipeline, 10, new_count);
            EXPECT_EQ(failure.failures(), 1);
        }
        const auto& failed_data = WaterUploadTestPeer::Data(pipeline);
        EXPECT_NE(failed_data.vao_id, 0u);
        EXPECT_NE(failed_data.vbo_id, 0u);
        EXPECT_NE(failed_data.ebo_id, 0u);
        EXPECT_EQ(failed_data.vao_id, old_data.vao_id);
        EXPECT_EQ(failed_data.vbo_id, old_data.vbo_id);
        EXPECT_EQ(failed_data.ebo_id, old_data.ebo_id);
        EXPECT_EQ(failed_data.ladder_upload_version, old_data.ladder_upload_version);
        EXPECT_TRUE(failed_data.LadderUploadValid());
        glBindVertexArray(failed_data.vao_id);
        glBindBuffer(GL_ARRAY_BUFFER, failed_data.vbo_id);
        GLint64 resident_bytes = 0;
        glGetBufferParameteri64v(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &resident_bytes);
        EXPECT_EQ(resident_bytes, static_cast<GLint64>(old_count * sizeof(VoxelVertex)));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, failed_data.ebo_id);
        glGetBufferParameteri64v(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &resident_bytes);
        EXPECT_EQ(resident_bytes, static_cast<GLint64>(old_count * sizeof(u32)));
        glBindVertexArray(0);
        EXPECT_EQ(failed_data.mesh_version, 9u);
        EXPECT_EQ(failed_data.element_count, old_count);
        const auto failed_upload = WaterUploadTestPeer::LiveUpload(pipeline, 10);
        EXPECT_FALSE(failed_upload.water_pass_eligible);
        EXPECT_FALSE(IsFarWorldLiveWaterUploadCurrent(failed_upload));
        // The intact old version remains drawable, but cannot claim version 10.
        EXPECT_TRUE(WaterUploadTestPeer::LiveUpload(pipeline, 9).water_pass_eligible);
        const auto presented = WaterUploadTestPeer::PresentedUpload(pipeline);
        EXPECT_TRUE(presented.water_pass_eligible);
        EXPECT_TRUE(IsFarWorldLiveWaterUploadCurrent(presented));
        EXPECT_EQ(presented.water_uploaded_version, 9u);
        EXPECT_TRUE(failed_data.PresentedLadderWaterEligible(true));
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

        // The same retained geometry used by the water draw list owns its cell
        // beyond the inner sphere, so the far sheet must be suppressed too.
        draw(WaterUploadTestPeer::PresentedUpload(pipeline));
        EXPECT_TRUE(renderer.LiveOwnership().OwnsWater({-248, SEA_LEVEL, -248}));
        EXPECT_EQ(Pixel(3).g, 0);

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

void CheckPoolGrowthFailure(GLenum target, bool storage_failure) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> original{};
    original[0].position = {1, 2, 3};
    const std::array<u32, 3> indices{0, 1, 2};
    EXPECT_EQ(
        pool.allocate(original.data(), std::numeric_limits<u32>::max(), indices.data(), 3, nullptr),
        ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.block_count(), 0u);
    const auto resident = pool.allocate(original.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(resident, ChunkGeometryPool::kInvalid);
    const auto allocation = pool.allocation(resident);
    const auto old_block = pool.blocks().front();
    // Bounded scratch: one block plus 64 vertices forces pool growth.
    const u32 replacement_count = old_block.vertex_capacity + 64;
    std::vector<VoxelVertex> replacement(replacement_count);
    replacement[0].position = {4, 5, 6};
    std::array<GLuint, 2> failed_buffers{};
    GLuint failed_vao = 0;
    {
        FailedPoolBlock failure(static_cast<GLenum>(target), storage_failure);
        EXPECT_EQ(pool.update(
                      resident, replacement.data(), replacement_count, indices.data(), 3, nullptr),
                  ChunkGeometryPool::kInvalid);
        EXPECT_EQ(failure.failures, 1u);
        failed_buffers = failure.buffers;
        failed_vao = failure.vao;
    }
    EXPECT_EQ(pool.block_count(), 1u);
    EXPECT_EQ(pool.live_allocation_count(), 1u);
    EXPECT_TRUE(pool.allocation(resident).live);
    EXPECT_EQ(pool.allocation(resident).vertex_offset, allocation.vertex_offset);
    EXPECT_EQ(pool.allocation(resident).index_count, 3u);
    EXPECT_EQ(glIsBuffer(old_block.vbo), GL_TRUE);
    EXPECT_EQ(glIsBuffer(old_block.ebo), GL_TRUE);
    EXPECT_EQ(glIsVertexArray(old_block.vao), GL_TRUE);
    for (const auto name : failed_buffers)
        EXPECT_EQ(glIsBuffer(name), GL_FALSE);
    EXPECT_EQ(glIsVertexArray(failed_vao), GL_FALSE);
    // Read resident bytes through GL, not just the host-side record.
    std::array<VoxelVertex, 3> received{};
    glBindBuffer(GL_ARRAY_BUFFER, old_block.vbo);
    glGetBufferSubData(GL_ARRAY_BUFFER,
                       static_cast<GLintptr>(allocation.vertex_offset * sizeof(VoxelVertex)),
                       static_cast<GLsizeiptr>(sizeof(received)),
                       received.data());
    EXPECT_EQ(received[0].position, original[0].position);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    const auto accepted =
        pool.update(resident, replacement.data(), replacement_count, indices.data(), 3, nullptr);
    ASSERT_NE(accepted, ChunkGeometryPool::kInvalid);
    EXPECT_FALSE(pool.allocation(resident).live);
    EXPECT_EQ(pool.live_allocation_count(), 1u);
    const auto reused = pool.allocate(original.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(reused, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.allocation(reused).block_index, allocation.block_index);
    EXPECT_EQ(pool.allocation(reused).vertex_offset, allocation.vertex_offset);
    EXPECT_EQ(pool.allocation(reused).index_offset, allocation.index_offset);
    const auto new_vbo = pool.blocks().back().vbo;
    pool.destroy();
    EXPECT_EQ(pool.block_count(), 0u);
    EXPECT_EQ(pool.live_allocation_count(), 0u);
    EXPECT_EQ(glIsBuffer(old_block.vbo), GL_FALSE);
    EXPECT_EQ(glIsBuffer(old_block.ebo), GL_FALSE);
    EXPECT_EQ(glIsVertexArray(old_block.vao), GL_FALSE);
    EXPECT_EQ(glIsBuffer(new_vbo), GL_FALSE);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(FarWorldWaterGl, PoolGrowthFailurePreservesResidentReusesSlicesAndReleasesObjects) {
    for (const auto target : {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER}) {
        for (const bool storage_failure : {false, true})
            CheckPoolGrowthFailure(static_cast<GLenum>(target), storage_failure);
    }
}

TEST_F(FarWorldWaterGl, FailedIndexReservationCannotOverlapLaterVertexSlices) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 65> vertices{};
    const std::array<u32, 3> indices{0, 1, 2};
    const auto resident = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(resident, ChunkGeometryPool::kInvalid);
    // Bounded scratch: one index block plus 64 indices exhausts the first block.
    const u32 index_count = pool.blocks().front().index_capacity + 64;
    const std::vector<u32> large_indices(index_count);
    {
        FailedPoolBlock failure(GL_ELEMENT_ARRAY_BUFFER, false);
        EXPECT_EQ(pool.allocate(vertices.data(), 3, large_indices.data(), index_count, nullptr),
                  ChunkGeometryPool::kInvalid);
        EXPECT_EQ(failure.failures, 1u);
    }
    const auto larger = pool.allocate(vertices.data(), 65, indices.data(), 3, nullptr);
    const auto smaller = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(larger, ChunkGeometryPool::kInvalid);
    ASSERT_NE(smaller, ChunkGeometryPool::kInvalid);
    const auto& large_slice = pool.allocation(larger);
    const auto& small_slice = pool.allocation(smaller);
    EXPECT_EQ(large_slice.block_index, small_slice.block_index);
    EXPECT_TRUE(small_slice.vertex_offset + small_slice.vertex_slot <= large_slice.vertex_offset ||
                large_slice.vertex_offset + large_slice.vertex_slot <= small_slice.vertex_offset);
    EXPECT_EQ(pool.update(resident, nullptr, 3, indices.data(), 3, nullptr),
              ChunkGeometryPool::kInvalid);
    EXPECT_TRUE(pool.allocation(resident).live);
    pool.destroy();
}

// Script completion observation without sleeping or assuming driver timing.
// Real fences are still created/deleted; the consumer's bytes remain GL-backed.
class DelayedPoolConsumer {
public:
    DelayedPoolConsumer() {
        active = this;
        glClientWaitSync = Wait;
        glFenceSync = Fence;
    }
    ~DelayedPoolConsumer() {
        glClientWaitSync = original_wait;
        glFenceSync = original_fence;
        active = nullptr;
    }
    GLenum result = GL_TIMEOUT_EXPIRED;
    bool fail_fence = false;
    std::size_t polls = 0;

private:
    static inline DelayedPoolConsumer* active = nullptr;
    PFNGLCLIENTWAITSYNCPROC original_wait = glClientWaitSync;
    PFNGLFENCESYNCPROC original_fence = glFenceSync;
    static GLenum APIENTRY Wait(GLsync, GLbitfield flags, GLuint64 timeout) {
        EXPECT_EQ(flags, static_cast<GLbitfield>(GL_SYNC_FLUSH_COMMANDS_BIT));
        EXPECT_EQ(timeout, 0u);
        ++active->polls;
        return active->result;
    }
    static GLsync APIENTRY Fence(GLenum condition, GLbitfield flags) {
        return active->fail_fence ? nullptr : active->original_fence(condition, flags);
    }
};

TEST_F(FarWorldWaterGl, DelayedConsumerPreventsOverwriteAndSliceReuseUntilCompletion) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> vertices{};
    vertices[0].position = {1, 2, 3};
    const std::array<u32, 3> indices{0, 1, 2};
    const auto resident = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(resident, ChunkGeometryPool::kInvalid);
    const auto old = pool.allocation(resident);
    const auto block = pool.blocks()[old.block_index];
    // Queue a GPU consumer of exactly these resident bytes before retirement.
    GLuint consumed = 0;
    glGenBuffers(1, &consumed);
    glBindBuffer(GL_COPY_WRITE_BUFFER, consumed);
    glBufferData(GL_COPY_WRITE_BUFFER, sizeof(vertices), nullptr, GL_STREAM_READ);
    glBindBuffer(GL_COPY_READ_BUFFER, block.vbo);
    glCopyBufferSubData(GL_COPY_READ_BUFFER,
                        GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(old.vertex_offset * sizeof(VoxelVertex)),
                        0,
                        sizeof(vertices));
    pool.mark_submitted(old.block_index);
    DelayedPoolConsumer delayed;
    vertices[0].position = {4, 5, 6};
    const auto replacement = pool.update(resident, vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(replacement, ChunkGeometryPool::kInvalid);
    EXPECT_NE(replacement, resident);
    EXPECT_FALSE(pool.allocation(resident).live);
    EXPECT_EQ(pool.retired_count(), 1u);
    EXPECT_EQ(block.vertex_ptr[old.vertex_offset].position, glm::vec3(1, 2, 3));
    const auto next = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(next, ChunkGeometryPool::kInvalid);
    EXPECT_NE(pool.allocation(next).vertex_offset, old.vertex_offset);
    EXPECT_NE(pool.allocation(next).index_offset, old.index_offset);
    EXPECT_EQ(pool.retired_count(), 1u);
    delayed.result = GL_WAIT_FAILED;
    pool.reclaim();
    EXPECT_EQ(pool.retired_count(), 1u);
    // GL's readback validates the queued consumer, independent of host records.
    std::array<VoxelVertex, 3> observed{};
    glBindBuffer(GL_COPY_READ_BUFFER, consumed);
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(observed), observed.data());
    EXPECT_EQ(observed[0].position, glm::vec3(1, 2, 3));
    // The real consumer has now completed; only its observed completion below
    // permits retirement. Never fake completion before the GL-backed readback.
    delayed.result = GL_CONDITION_SATISFIED;
    pool.reclaim();
    EXPECT_EQ(pool.retired_count(), 0u);
    const auto reused = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(reused, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.allocation(reused).vertex_offset, old.vertex_offset);
    EXPECT_EQ(pool.allocation(reused).index_offset, old.index_offset);
    pool.free(replacement);
    delayed.result = GL_ALREADY_SIGNALED;
    pool.reclaim();
    EXPECT_EQ(pool.retired_count(), 0u);
    glDeleteBuffers(1, &consumed);
    pool.destroy();
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(FarWorldWaterGl, MissingFenceQuarantinesStorageAndRetirementPollingIsBounded) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> vertices{};
    const std::array<u32, 3> indices{0, 1, 2};
    std::array<u32, 9> handles{};
    for (auto& handle : handles) {
        handle = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
        ASSERT_NE(handle, ChunkGeometryPool::kInvalid);
    }
    pool.mark_submitted(0);
    DelayedPoolConsumer delayed;
    for (const auto handle : handles)
        pool.free(handle);
    delayed.result = GL_ALREADY_SIGNALED;
    pool.reclaim();
    EXPECT_EQ(delayed.polls, kTerrainUploadLimits.count);
    EXPECT_EQ(pool.retired_count(), 1u);
    pool.reclaim();
    EXPECT_EQ(pool.retired_count(), 0u);
    const auto reused = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(reused, ChunkGeometryPool::kInvalid);
    const auto old = pool.allocation(reused);
    delayed.fail_fence = true;
    pool.free(reused);
    EXPECT_EQ(pool.retired_count(), 1u);
    pool.reclaim();
    EXPECT_EQ(pool.retired_count(), 1u);
    const auto next = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(next, ChunkGeometryPool::kInvalid);
    EXPECT_NE(pool.allocation(next).vertex_offset, old.vertex_offset);
    // Oversized requests are rejected before dereferencing either tiny input.
    const auto too_large =
        static_cast<u32>(ChunkGeometryPool::kMaxStorageBytes / sizeof(VoxelVertex));
    EXPECT_EQ(pool.update(next, vertices.data(), too_large, indices.data(), 3, nullptr),
              ChunkGeometryPool::kInvalid);
    EXPECT_TRUE(pool.valid_allocation(next, 3));
    pool.free(ChunkGeometryPool::kInvalid);
    pool.free(next);
    pool.free(next);
    const auto retired_before_swap = pool.retired_count();
    pool.clear();
    EXPECT_EQ(pool.retired_count(), retired_before_swap);
    EXPECT_EQ(pool.live_allocation_count(), 0u);
    const auto after_swap = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(after_swap, ChunkGeometryPool::kInvalid);
    EXPECT_NE(pool.allocation(after_swap).vertex_offset, old.vertex_offset);
    pool.clear();
    EXPECT_GT(pool.retired_count(), retired_before_swap);
    pool.destroy();
    EXPECT_EQ(pool.retired_count(), 0u);
    EXPECT_EQ(pool.block_count(), 0u);
    // Final destruction must reset storage and its caps for a new pool lifetime.
    const auto fresh = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    EXPECT_NE(fresh, ChunkGeometryPool::kInvalid);
    pool.destroy();
}

// Keep a real queued GL consumer in these completion-controlled fixtures. Read
// it back before reporting any scripted healthy completion to the allocator.
class PoolVertexReadback {
public:
    PoolVertexReadback(const ChunkGeometryPool::Block& block, u32 offset) {
        glGenBuffers(1, &buffer_);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer_);
        glBufferData(GL_COPY_WRITE_BUFFER, sizeof(VoxelVertex) * 3, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_COPY_READ_BUFFER, block.vbo);
        glCopyBufferSubData(
            GL_COPY_READ_BUFFER,
            GL_COPY_WRITE_BUFFER,
            static_cast<GLintptr>(static_cast<std::size_t>(offset) * sizeof(VoxelVertex)),
            0,
            sizeof(VoxelVertex) * 3);
    }
    ~PoolVertexReadback() {
        glDeleteBuffers(1, &buffer_);
    }
    glm::vec3 Read() const {
        std::array<VoxelVertex, 3> observed{};
        glBindBuffer(GL_COPY_READ_BUFFER, buffer_);
        glGetBufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(observed), observed.data());
        return observed[0].position;
    }

private:
    GLuint buffer_ = 0;
};

class SelectivePoolCompletion {
public:
    SelectivePoolCompletion(bool missing, GLenum status)
        : missing_(missing)
        , status_(status) {
        active_ = this;
        glClientWaitSync = Wait;
        glFenceSync = Fence;
    }
    ~SelectivePoolCompletion() {
        glClientWaitSync = original_wait_;
        glFenceSync = original_fence_;
        active_ = nullptr;
    }
    std::size_t polls = 0;

private:
    PFNGLCLIENTWAITSYNCPROC original_wait_ = glClientWaitSync;
    PFNGLFENCESYNCPROC original_fence_ = glFenceSync;
    bool missing_;
    GLenum status_;
    bool first_ = true;
    GLsync blocked_ = nullptr;
    static inline SelectivePoolCompletion* active_ = nullptr;
    static GLenum APIENTRY Wait(GLsync sync, GLbitfield flags, GLuint64 timeout) {
        EXPECT_EQ(flags, static_cast<GLbitfield>(GL_SYNC_FLUSH_COMMANDS_BIT));
        EXPECT_EQ(timeout, 0u);
        ++active_->polls;
        return sync == active_->blocked_ ? active_->status_ : GL_ALREADY_SIGNALED;
    }
    static GLsync APIENTRY Fence(GLenum condition, GLbitfield flags) {
        if (!active_->first_)
            return active_->original_fence_(condition, flags);
        active_->first_ = false;
        active_->blocked_ =
            active_->missing_ ? nullptr : active_->original_fence_(condition, flags);
        return active_->blocked_;
    }
};

void CheckHealthyRetirementBehindBlockedFront(bool missing, GLenum status) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> vertices{};
    vertices[0].position = {1, 2, 3};
    const std::array<u32, 3> indices{0, 1, 2};
    // Eleven records force two bounded passes, including a blocked front.
    std::array<u32, 11> handles{};
    for (auto& handle : handles) {
        handle = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
        ASSERT_NE(handle, ChunkGeometryPool::kInvalid);
    }
    const auto blocked = pool.allocation(handles.front());
    const auto healthy = pool.allocation(handles.back());
    PoolVertexReadback consumer(pool.blocks()[healthy.block_index], healthy.vertex_offset);
    pool.mark_submitted(blocked.block_index);
    SelectivePoolCompletion completion(missing, status);
    for (const auto handle : handles)
        pool.free(handle);
    EXPECT_EQ(consumer.Read(), glm::vec3(1, 2, 3));
    pool.reclaim();
    EXPECT_EQ(completion.polls, kTerrainUploadLimits.count - (missing ? 1u : 0u));
    EXPECT_EQ(pool.retired_count(), 4u);
    EXPECT_EQ(pool.blocks()[blocked.block_index].allocation_count, 4u);
    const auto polls_before_upload = completion.polls;
    vertices[0].position = {4, 5, 6};
    const auto replacement = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(replacement, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(completion.polls - polls_before_upload, missing ? 3u : 4u);
    EXPECT_LE(completion.polls - polls_before_upload, kTerrainUploadLimits.count);
    EXPECT_EQ(pool.retired_count(), 1u);
    EXPECT_EQ(pool.blocks()[blocked.block_index].allocation_count, 2u);
    EXPECT_NE(replacement, handles.front());
    EXPECT_NE(pool.allocation(replacement).vertex_offset, blocked.vertex_offset);
    EXPECT_NE(pool.allocation(replacement).index_offset, blocked.index_offset);
    EXPECT_EQ(pool.blocks()[blocked.block_index].vertex_ptr[blocked.vertex_offset].position,
              glm::vec3(1, 2, 3));
    const auto polls_before_singleton = completion.polls;
    pool.reclaim();
    EXPECT_EQ(completion.polls - polls_before_singleton, missing ? 0u : 1u);
    EXPECT_EQ(pool.retired_count(), 1u);
    pool.free(handles.front());
    EXPECT_EQ(pool.retired_count(), 1u);
    pool.destroy();
    EXPECT_EQ(pool.storage_bytes(), 0u);
    EXPECT_EQ(pool.retired_count(), 0u);
}

TEST_F(FarWorldWaterGl, FailedOrIncompleteFrontDoesNotBlockHealthyRetirement) {
    for (const auto status : {GL_TIMEOUT_EXPIRED, GL_WAIT_FAILED})
        CheckHealthyRetirementBehindBlockedFront(false, static_cast<GLenum>(status));
}

TEST_F(FarWorldWaterGl, NullFrontDoesNotBlockHealthyRetirement) {
    CheckHealthyRetirementBehindBlockedFront(true, GL_TIMEOUT_EXPIRED);
}

TEST_F(FarWorldWaterGl, FullyReclaimedFragmentedWorldAdmitsLargerMeshInSameBlock) {
    ChunkGeometryPool pool;
    const std::array<VoxelVertex, 3> seed{};
    const std::array<u32, 3> triangle{0, 1, 2};
    const auto first = pool.allocate(seed.data(), 3, triangle.data(), 3, nullptr);
    ASSERT_NE(first, ChunkGeometryPool::kInvalid);
    auto original = pool.blocks().front();
    pool.destroy();
    // Scratch is bounded by one block; four slots fragment both entire buffers.
    std::vector<VoxelVertex> vertices(original.vertex_capacity);
    const std::vector<u32> indices(original.index_capacity);
    for (unsigned slot = 0; slot < 4; ++slot) {
        ASSERT_NE(pool.allocate(vertices.data(),
                                original.vertex_capacity / 4,
                                indices.data(),
                                original.index_capacity / 4,
                                nullptr),
                  ChunkGeometryPool::kInvalid);
    }
    ASSERT_EQ(pool.block_count(), 1u);
    original = pool.blocks().front();
    EXPECT_EQ(pool.blocks().front().vertex_high_water, original.vertex_capacity);
    EXPECT_EQ(pool.blocks().front().index_high_water, original.index_capacity);
    const auto bytes = pool.storage_bytes();
    pool.clear();
    pool.clear();
    EXPECT_EQ(pool.live_allocation_count(), 0u);
    EXPECT_EQ(pool.retired_count(), 0u);
    EXPECT_EQ(pool.blocks().front().allocation_count, 0u);
    EXPECT_EQ(pool.blocks().front().vertex_high_water, 0u);
    EXPECT_EQ(pool.blocks().front().index_high_water, 0u);
    EXPECT_TRUE(pool.blocks().front().free_vertex_slices.empty());
    EXPECT_TRUE(pool.blocks().front().free_index_slices.empty());
    const auto larger = pool.allocate(vertices.data(),
                                      original.vertex_capacity,
                                      indices.data(),
                                      original.index_capacity,
                                      nullptr);
    ASSERT_NE(larger, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.block_count(), 1u);
    EXPECT_EQ(pool.storage_bytes(), bytes);
    EXPECT_EQ(pool.allocation(larger).block_index, 0u);
    EXPECT_EQ(pool.allocation(larger).vertex_offset, 0u);
    EXPECT_EQ(pool.allocation(larger).index_offset, 0u);
    EXPECT_EQ(pool.blocks().front().vao, original.vao);
    EXPECT_EQ(pool.blocks().front().vbo, original.vbo);
    EXPECT_EQ(pool.blocks().front().ebo, original.ebo);
    pool.destroy();
}

TEST_F(FarWorldWaterGl, FailedReservationLeavesReclaimedFrontiersUnchanged) {
    ChunkGeometryPool pool;
    const std::array<VoxelVertex, 3> seed{};
    const std::array<u32, 3> triangle{0, 1, 2};
    const auto first = pool.allocate(seed.data(), 3, triangle.data(), 3, nullptr);
    ASSERT_NE(first, ChunkGeometryPool::kInvalid);
    const auto original = pool.blocks().front();
    pool.free(first);
    // Valid source buffers, bounded by one block plus one aligned index slot.
    const std::vector<VoxelVertex> vertices(original.vertex_capacity);
    const std::vector<u32> indices(original.index_capacity + 64);
    {
        FailedPoolBlock failure(GL_ELEMENT_ARRAY_BUFFER, false);
        EXPECT_EQ(pool.allocate(
                      vertices.data(), 3, indices.data(), original.index_capacity + 64, nullptr),
                  ChunkGeometryPool::kInvalid);
        EXPECT_EQ(failure.failures, 1u);
    }
    EXPECT_EQ(pool.block_count(), 1u);
    EXPECT_EQ(pool.blocks().front().vertex_high_water, 0u);
    EXPECT_EQ(pool.blocks().front().index_high_water, 0u);
    EXPECT_TRUE(pool.blocks().front().free_vertex_slices.empty());
    EXPECT_TRUE(pool.blocks().front().free_index_slices.empty());
    const auto full = pool.allocate(vertices.data(),
                                    original.vertex_capacity,
                                    indices.data(),
                                    original.index_capacity,
                                    nullptr);
    ASSERT_NE(full, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.block_count(), 1u);
    EXPECT_EQ(pool.allocation(full).vertex_offset, 0u);
    EXPECT_EQ(pool.allocation(full).index_offset, 0u);
    pool.destroy();
}

TEST_F(FarWorldWaterGl, PendingWorldSwapRetainsStorageAndBytesUntilCompletion) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> seed{};
    const std::array<u32, 3> triangle{0, 1, 2};
    const auto first = pool.allocate(seed.data(), 3, triangle.data(), 3, nullptr);
    ASSERT_NE(first, ChunkGeometryPool::kInvalid);
    auto original = pool.blocks().front();
    pool.destroy();
    std::vector<VoxelVertex> vertices(original.vertex_capacity);
    vertices[0].position = {1, 2, 3};
    const std::vector<u32> indices(original.index_capacity);
    for (unsigned slot = 0; slot < 4; ++slot) {
        ASSERT_NE(pool.allocate(vertices.data(),
                                original.vertex_capacity / 4,
                                indices.data(),
                                original.index_capacity / 4,
                                nullptr),
                  ChunkGeometryPool::kInvalid);
    }
    ASSERT_EQ(pool.block_count(), 1u);
    original = pool.blocks().front();
    PoolVertexReadback consumer(pool.blocks().front(), 0);
    pool.mark_submitted(0);
    DelayedPoolConsumer delayed;
    const auto bytes = pool.storage_bytes();
    pool.clear();
    pool.clear();
    EXPECT_EQ(pool.retired_count(), 4u);
    EXPECT_EQ(pool.storage_bytes(), bytes);
    EXPECT_EQ(pool.blocks().front().allocation_count, 4u);
    EXPECT_EQ(pool.blocks().front().vertex_high_water, original.vertex_capacity);
    EXPECT_EQ(pool.blocks().front().index_high_water, original.index_capacity);
    seed[0].position = {4, 5, 6};
    const auto next = pool.allocate(seed.data(), 3, triangle.data(), 3, nullptr);
    ASSERT_NE(next, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(delayed.polls, 4u);
    EXPECT_NE(pool.allocation(next).block_index, 0u);
    EXPECT_EQ(pool.block_count(), 2u);
    EXPECT_EQ(pool.storage_bytes(), bytes * 2);
    EXPECT_EQ(pool.blocks().front().vbo, original.vbo);
    EXPECT_EQ(original.vertex_ptr[0].position, glm::vec3(1, 2, 3));
    EXPECT_EQ(glIsBuffer(original.vbo), GL_TRUE);
    EXPECT_EQ(glIsBuffer(original.ebo), GL_TRUE);
    EXPECT_EQ(glIsVertexArray(original.vao), GL_TRUE);
    EXPECT_EQ(consumer.Read(), glm::vec3(1, 2, 3));
    delayed.result = GL_CONDITION_SATISFIED;
    pool.reclaim();
    EXPECT_EQ(delayed.polls, 8u);
    EXPECT_EQ(pool.retired_count(), 0u);
    EXPECT_EQ(pool.blocks().front().allocation_count, 0u);
    EXPECT_EQ(pool.blocks().front().vertex_high_water, 0u);
    EXPECT_EQ(pool.blocks().front().index_high_water, 0u);
    const auto larger = pool.allocate(vertices.data(),
                                      original.vertex_capacity,
                                      indices.data(),
                                      original.index_capacity,
                                      nullptr);
    ASSERT_NE(larger, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.allocation(larger).block_index, 0u);
    EXPECT_EQ(pool.block_count(), 2u);
    EXPECT_EQ(pool.storage_bytes(), bytes * 2);
    EXPECT_TRUE(pool.valid_allocation(next, 3));
    pool.destroy();
    EXPECT_EQ(pool.storage_bytes(), 0u);
}

TEST_F(FarWorldWaterGl, EmptyBlockReplacementPreservesQuarantinedAndLiveBlockIndices) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> seed{};
    seed[0].position = {1, 2, 3};
    const std::array<u32, 3> indices{0, 1, 2};
    const auto first = pool.allocate(seed.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(first, ChunkGeometryPool::kInvalid);
    const auto capacity = pool.blocks().front().vertex_capacity;
    pool.destroy();
    // At most one block plus one aligned slot of host scratch.
    const std::vector<VoxelVertex> vertices(capacity + 64);
    const auto quarantined = pool.allocate(vertices.data(), capacity, indices.data(), 3, nullptr);
    const auto empty = pool.allocate(vertices.data(), capacity, indices.data(), 3, nullptr);
    const auto live = pool.allocate(seed.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(quarantined, ChunkGeometryPool::kInvalid);
    ASSERT_NE(empty, ChunkGeometryPool::kInvalid);
    ASSERT_NE(live, ChunkGeometryPool::kInvalid);
    const auto held = pool.allocation(quarantined);
    const auto empty_index = pool.allocation(empty).block_index;
    const auto stable = pool.allocation(live);
    ASSERT_EQ(held.block_index, 0u);
    ASSERT_EQ(empty_index, 1u);
    ASSERT_EQ(stable.block_index, 2u);
    const auto live_block = pool.blocks()[stable.block_index];
    const auto bytes = pool.storage_bytes();
    pool.mark_submitted(held.block_index);
    DelayedPoolConsumer delayed;
    delayed.fail_fence = true;
    pool.free(quarantined);
    pool.free(empty);
    EXPECT_EQ(pool.retired_count(), 1u);
    EXPECT_EQ(pool.blocks()[held.block_index].allocation_count, 1u);
    const auto old_block = pool.blocks()[empty_index];
    {
        FailedPoolBlock failure(GL_ELEMENT_ARRAY_BUFFER, false);
        EXPECT_EQ(pool.allocate(vertices.data(), capacity + 64, indices.data(), 3, nullptr),
                  ChunkGeometryPool::kInvalid);
        EXPECT_EQ(failure.failures, 1u);
    }
    EXPECT_EQ(pool.block_count(), 3u);
    EXPECT_EQ(pool.storage_bytes(),
              bytes - (static_cast<std::size_t>(old_block.vertex_capacity) * sizeof(VoxelVertex) +
                       static_cast<std::size_t>(old_block.index_capacity) * sizeof(u32)));
    EXPECT_EQ(pool.blocks()[empty_index].vao, 0u);
    EXPECT_EQ(glIsBuffer(old_block.vbo), GL_FALSE);
    EXPECT_EQ(glIsBuffer(old_block.ebo), GL_FALSE);
    EXPECT_EQ(glIsVertexArray(old_block.vao), GL_FALSE);
    const auto larger = pool.allocate(vertices.data(), capacity + 64, indices.data(), 3, nullptr);
    ASSERT_NE(larger, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.allocation(larger).block_index, empty_index);
    EXPECT_EQ(pool.block_count(), 3u);
    EXPECT_EQ(pool.storage_bytes(), bytes + 64 * sizeof(VoxelVertex));
    EXPECT_LE(pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes);
    EXPECT_LE(pool.block_count(), ChunkGeometryPool::kMaxBlocks);
    EXPECT_EQ(pool.retired_count(), 1u);
    EXPECT_EQ(delayed.polls, 0u);
    EXPECT_TRUE(pool.valid_allocation(live, 3));
    EXPECT_EQ(pool.allocation(live).block_index, stable.block_index);
    EXPECT_EQ(pool.allocation(live).vertex_offset, stable.vertex_offset);
    EXPECT_EQ(pool.allocation(live).index_offset, stable.index_offset);
    EXPECT_EQ(pool.blocks()[stable.block_index].vao, live_block.vao);
    EXPECT_EQ(pool.blocks()[stable.block_index].vbo, live_block.vbo);
    EXPECT_EQ(pool.blocks()[stable.block_index].ebo, live_block.ebo);
    PoolVertexReadback consumer(pool.blocks()[stable.block_index], stable.vertex_offset);
    EXPECT_EQ(consumer.Read(), glm::vec3(1, 2, 3));
    pool.destroy();
    EXPECT_EQ(pool.storage_bytes(), 0u);
}

TEST_F(FarWorldWaterGl, StorageCapIncludesPendingWorldsAndReleasesOnlyCompletedEmptyBlocks) {
    ChunkGeometryPool pool;
    const std::array<VoxelVertex, 3> seed{};
    const std::array<u32, 3> indices{0, 1, 2};
    const auto first = pool.allocate(seed.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(first, ChunkGeometryPool::kInvalid);
    const auto capacity = pool.blocks().front().vertex_capacity;
    const auto block_bytes = pool.storage_bytes();
    const auto count = ChunkGeometryPool::kMaxStorageBytes / block_bytes;
    ASSERT_LE(count, ChunkGeometryPool::kMaxBlocks);
    ASSERT_GE(count, 2u);
    pool.destroy();
    // Fixed pool cap bounds GPU storage; host scratch is two vertex blocks.
    std::vector<VoxelVertex> vertices(capacity * 2);
    vertices[0].position = {1, 2, 3};
    for (std::size_t index = 0; index < count; ++index) {
        ASSERT_NE(pool.allocate(vertices.data(), capacity, indices.data(), 3, nullptr),
                  ChunkGeometryPool::kInvalid);
    }
    ASSERT_EQ(pool.block_count(), count);
    const auto bytes = pool.storage_bytes();
    EXPECT_EQ(bytes, count * block_bytes);
    EXPECT_LE(bytes, ChunkGeometryPool::kMaxStorageBytes);
    PoolVertexReadback consumer(pool.blocks().front(), 0);
    for (u32 index = 0; index < pool.block_count(); ++index)
        pool.mark_submitted(index);
    DelayedPoolConsumer delayed;
    pool.clear();
    pool.clear();
    EXPECT_EQ(pool.retired_count(), count);
    EXPECT_EQ(pool.live_allocation_count(), 0u);
    EXPECT_EQ(pool.allocate(vertices.data(), capacity, indices.data(), 3, nullptr),
              ChunkGeometryPool::kInvalid);
    EXPECT_EQ(delayed.polls, kTerrainUploadLimits.count);
    EXPECT_EQ(pool.retired_count(), count);
    EXPECT_EQ(pool.storage_bytes(), bytes);
    EXPECT_EQ(pool.block_count(), count);
    EXPECT_EQ(consumer.Read(), glm::vec3(1, 2, 3));
    delayed.result = GL_ALREADY_SIGNALED;
    // Each pass is bounded independently, including the last partial pass.
    for (std::size_t index = 0; index < count; index += kTerrainUploadLimits.count) {
        const auto before = delayed.polls;
        pool.reclaim();
        EXPECT_EQ(delayed.polls - before, std::min(kTerrainUploadLimits.count, count - index));
    }
    EXPECT_EQ(pool.retired_count(), 0u);
    const auto larger = pool.allocate(vertices.data(), capacity * 2, indices.data(), 3, nullptr);
    ASSERT_NE(larger, ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.allocation(larger).block_index, 0u);
    EXPECT_EQ(pool.block_count(), count);
    const auto replacement_bytes =
        static_cast<std::size_t>(pool.blocks().front().vertex_capacity) * sizeof(VoxelVertex) +
        static_cast<std::size_t>(pool.blocks().front().index_capacity) * sizeof(u32);
    const auto released_blocks =
        (replacement_bytes - (ChunkGeometryPool::kMaxStorageBytes - bytes) + block_bytes - 1) /
        block_bytes;
    for (std::size_t index = 1; index < released_blocks; ++index) {
        EXPECT_EQ(pool.blocks()[index].vao, 0u);
        EXPECT_EQ(pool.blocks()[index].allocation_count, 0u);
    }
    EXPECT_EQ(pool.storage_bytes(), bytes - released_blocks * block_bytes + replacement_bytes);
    EXPECT_LE(pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes);
    pool.destroy();
    EXPECT_EQ(pool.storage_bytes(), 0u);
}

// Real 512 MiB pressure: one 44 MiB block and thirteen 36 MiB blocks,
// with both stores occupied. Scratch is at most one 44 MiB mesh, reused for
// every allocation. No pool internals or artificial capacity seams are used.
class FullTerrainPool {
public:
    ~FullTerrainPool() {
        pool.destroy();
    }
    void Fill(u32 tail = 0) {
        const std::array<VoxelVertex, 3> seed{};
        const std::array<u32, 3> triangle{0, 1, 2};
        ASSERT_NE(pool.allocate(seed.data(), 3, triangle.data(), 3, nullptr),
                  ChunkGeometryPool::kInvalid);
        vertex_capacity = pool.blocks().front().vertex_capacity;
        index_capacity = pool.blocks().front().index_capacity;
        const auto default_bytes = pool.storage_bytes();
        pool.destroy();
        vertices.resize(vertex_capacity);
        indices.resize(index_capacity * 2);
        vertices[0].position = {1, 2, 3};
        indices[0] = 2;
        // Create the full backing store before reserving the shorter resident.
        // Otherwise add_block sizes the index store to 2 * capacity - tail,
        // leaving tail * sizeof(u32) bytes outside the intended full pool.
        const auto backing = pool.allocate(
            vertices.data(), vertex_capacity, indices.data(), index_capacity * 2, nullptr);
        ASSERT_NE(backing, ChunkGeometryPool::kInvalid);
        const auto backing_bytes = pool.storage_bytes();
        pool.free(backing); // Unsubmitted storage is immediately reusable.
        ASSERT_EQ(pool.live_allocation_count(), 0u);
        resident.pool_handle = pool.allocate(vertices.data(),
                                             vertex_capacity - tail,
                                             indices.data(),
                                             index_capacity * 2 - tail,
                                             nullptr);
        ASSERT_NE(resident.pool_handle, ChunkGeometryPool::kInvalid);
        ASSERT_EQ(pool.storage_bytes(), backing_bytes);
        const auto& block = pool.blocks()[pool.allocation(resident.pool_handle).block_index];
        ASSERT_EQ(block.vertex_capacity, vertex_capacity);
        ASSERT_EQ(block.index_capacity, index_capacity * 2);
        ASSERT_EQ(block.vertex_high_water, vertex_capacity - tail);
        ASSERT_EQ(block.index_high_water, index_capacity * 2 - tail);
        resident.element_count = index_capacity * 2 - tail;
        resident.mesh_version = 7;
        const auto remaining = ChunkGeometryPool::kMaxStorageBytes - pool.storage_bytes();
        ASSERT_EQ(remaining % default_bytes, 0u);
        const auto count = remaining / default_bytes;
        ASSERT_LT(count, ChunkGeometryPool::kMaxBlocks);
        for (std::size_t index = 0; index < count; ++index)
            ASSERT_NE(
                pool.allocate(
                    vertices.data(), vertex_capacity, indices.data(), index_capacity, nullptr),
                ChunkGeometryPool::kInvalid);
        ASSERT_EQ(pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes);
        ASSERT_EQ(pool.live_allocation_count(), count + 1);
        ASSERT_LE(pool.block_count(), ChunkGeometryPool::kMaxBlocks);
    }
    u32 Update(u32 vertex_count, u32 index_count) {
        return pool.update(resident.pool_handle,
                           vertices.data(),
                           vertex_count,
                           indices.data(),
                           index_count,
                           nullptr);
    }
    void ExpectPresented(u32 version) const {
        FarWorldLiveUpload upload;
        upload.cell = {-16, 0, -16, 0};
        upload.snapshot_version = version + 1;
        resident.SetPresentedTerrainUpload(upload, pool);
        EXPECT_EQ(upload.snapshot_version, version);
        EXPECT_EQ(upload.uploaded_version, version);
        EXPECT_EQ(upload.uploaded_indices, resident.element_count);
        EXPECT_TRUE(upload.valid_allocation);
        FarWorldOwnership ownership;
        ownership.Begin(1);
        ASSERT_TRUE(ownership.Add(upload));
        ASSERT_TRUE(ownership.Finish());
        EXPECT_TRUE(ownership.Owns({-248, 8, -248}));
    }
    ChunkGeometryPool pool;
    ChunkRenderData resident;
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
    u32 vertex_capacity = 0;
    u32 index_capacity = 0;
};

class PoolIndexReadback {
public:
    PoolIndexReadback(const ChunkGeometryPool::Block& block, u32 offset) {
        glGenBuffers(1, &buffer_);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer_);
        glBufferData(GL_COPY_WRITE_BUFFER, sizeof(u32) * 3, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_COPY_READ_BUFFER, block.ebo);
        glCopyBufferSubData(GL_COPY_READ_BUFFER,
                            GL_COPY_WRITE_BUFFER,
                            static_cast<GLintptr>(static_cast<std::size_t>(offset) * sizeof(u32)),
                            0,
                            sizeof(u32) * 3);
    }
    ~PoolIndexReadback() {
        glDeleteBuffers(1, &buffer_);
    }
    std::array<u32, 3> Read() const {
        std::array<u32, 3> observed{};
        glBindBuffer(GL_COPY_READ_BUFFER, buffer_);
        glGetBufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(observed), observed.data());
        return observed;
    }

private:
    GLuint buffer_ = 0;
};

// Hash every drawable byte in BOTH GL stores, including late offsets. Readback
// is test-only and never part of an upload attempt's completion policy.
std::array<std::uint64_t, 2> PoolDrawableDigest(const ChunkGeometryPool& pool, u32 handle) {
    const auto& alloc = pool.allocation(handle);
    const auto& block = pool.blocks()[alloc.block_index];
    std::array<std::uint64_t, 2> digest{};
    const std::array<GLuint, 2> buffers{block.vbo, block.ebo};
    const std::array<std::size_t, 2> offsets{
        static_cast<std::size_t>(alloc.vertex_offset) * sizeof(VoxelVertex),
        static_cast<std::size_t>(alloc.index_offset) * sizeof(u32)};
    const std::array<std::size_t, 2> sizes{
        static_cast<std::size_t>(alloc.vertex_count) * sizeof(VoxelVertex),
        static_cast<std::size_t>(alloc.index_count) * sizeof(u32)};
    // Fixed 64 KiB readback scratch; at most 512 MiB total per digest.
    std::array<unsigned char, 64 * 1024> bytes{};
    for (std::size_t store = 0; store < buffers.size(); ++store) {
        digest[store] = 14695981039346656037ull;
        glBindBuffer(GL_COPY_READ_BUFFER, buffers[store]);
        for (std::size_t offset = 0; offset < sizes[store]; offset += bytes.size()) {
            const auto count = std::min(bytes.size(), sizes[store] - offset);
            glGetBufferSubData(GL_COPY_READ_BUFFER,
                               static_cast<GLintptr>(offsets[store] + offset),
                               static_cast<GLsizeiptr>(count),
                               bytes.data());
            for (std::size_t byte = 0; byte < count; ++byte) {
                digest[store] ^= bytes[byte];
                digest[store] *= 1099511628211ull;
            }
        }
    }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    return digest;
}

class FinalTerrainConsumer {
public:
    FinalTerrainConsumer() {
        active_ = this;
        glFenceSync = Fence;
        glClientWaitSync = Wait;
        glDeleteSync = Delete;
    }
    ~FinalTerrainConsumer() {
        glFenceSync = original_fence_;
        glClientWaitSync = original_wait_;
        glDeleteSync = original_delete_;
        active_ = nullptr;
    }
    bool missing = false;
    bool fence_error = false;
    bool wait_error = false;
    bool delete_error = false;
    GLenum result = GL_TIMEOUT_EXPIRED;
    GLsync latest = nullptr;
    GLsync polled = nullptr;
    std::size_t fences = 0;
    std::size_t polls = 0;

private:
    PFNGLFENCESYNCPROC original_fence_ = glFenceSync;
    PFNGLCLIENTWAITSYNCPROC original_wait_ = glClientWaitSync;
    PFNGLDELETESYNCPROC original_delete_ = glDeleteSync;
    static inline FinalTerrainConsumer* active_ = nullptr;
    static GLsync APIENTRY Fence(GLenum condition, GLbitfield flags) {
        ++active_->fences;
        active_->latest = active_->missing ? nullptr : active_->original_fence_(condition, flags);
        if (active_->fence_error)
            glBindBuffer(GL_TEXTURE_2D, 0); // real GL_INVALID_ENUM, even with a nonnull fence
        return active_->latest;
    }
    static GLenum APIENTRY Wait(GLsync completion, GLbitfield flags, GLuint64 timeout) {
        EXPECT_EQ(flags, static_cast<GLbitfield>(GL_SYNC_FLUSH_COMMANDS_BIT));
        EXPECT_EQ(timeout, 0u);
        active_->polled = completion;
        ++active_->polls;
        if (active_->wait_error)
            glBindBuffer(GL_TEXTURE_2D, 0);
        return active_->result;
    }
    static void APIENTRY Delete(GLsync completion) {
        active_->original_delete_(completion);
        if (active_->delete_error)
            glBindBuffer(GL_TEXTURE_2D, 0);
    }
};

TEST_F(FarWorldWaterGl, FullPoolLiveResidentReplacesRepeatedlyAfterFinalConsumerCompletion) {
    FullTerrainPool full;
    ASSERT_NO_FATAL_FAILURE(full.Fill());
    const auto handle = full.resident.pool_handle;
    const auto allocation = full.pool.allocation(handle);
    const auto block = full.pool.blocks()[allocation.block_index];
    const auto old_digest = PoolDrawableDigest(full.pool, handle);
    PoolVertexReadback first(block, allocation.vertex_offset);
    PoolIndexReadback first_indices(block, allocation.index_offset);
    full.pool.mark_submitted(allocation.block_index);
    FinalTerrainConsumer completion;
    full.vertices[0].position = {4, 5, 6};
    full.indices[0] = 1;
    for (unsigned retry = 0; retry < 2; ++retry) {
        EXPECT_EQ(full.Update(full.vertex_capacity, full.index_capacity * 2),
                  ChunkGeometryPool::kInvalid);
        EXPECT_TRUE(full.pool.allocation(handle).live);
        EXPECT_EQ(full.pool.live_allocation_count(), 14u);
        EXPECT_EQ(PoolDrawableDigest(full.pool, handle), old_digest);
        full.ExpectPresented(7);
    }
    EXPECT_EQ(completion.fences, 1u);
    EXPECT_EQ(completion.polls, 2u);
    EXPECT_EQ(first.Read(), glm::vec3(1, 2, 3));
    EXPECT_EQ(first_indices.Read()[0], 2u);
    const auto fences = completion.fences;
    // A later replay/shadow consumer replaces the earlier fence. Success must
    // observe this latest one, even if an earlier consumer already completed.
    PoolVertexReadback final(block, allocation.vertex_offset);
    PoolIndexReadback final_indices(block, allocation.index_offset);
    full.pool.mark_submitted(allocation.block_index);
    EXPECT_EQ(completion.fences, fences + 1);
    EXPECT_EQ(final.Read(), glm::vec3(1, 2, 3));
    EXPECT_EQ(final_indices.Read()[0], 2u);
    completion.result = GL_CONDITION_SATISFIED;
    const auto latest = completion.latest;
    EXPECT_EQ(full.Update(full.vertex_capacity, full.index_capacity * 2), handle);
    EXPECT_EQ(completion.polled, latest);
    const auto replacement_digest = PoolDrawableDigest(full.pool, handle);
    EXPECT_NE(replacement_digest[0], old_digest[0]);
    EXPECT_NE(replacement_digest[1], old_digest[1]);
    EXPECT_EQ(full.pool.allocation(handle).block_index, allocation.block_index);
    EXPECT_EQ(full.pool.allocation(handle).vertex_offset, allocation.vertex_offset);
    EXPECT_EQ(full.pool.allocation(handle).index_offset, allocation.index_offset);
    full.resident.mesh_version = 8;
    full.ExpectPresented(8);
    for (u32 version = 9; version < 12; ++version) {
        PoolVertexReadback consumed(block, allocation.vertex_offset);
        PoolIndexReadback consumed_indices(block, allocation.index_offset);
        full.pool.mark_submitted(allocation.block_index);
        EXPECT_EQ(consumed.Read(), full.vertices[0].position);
        EXPECT_EQ(consumed_indices.Read()[0], full.indices[0]);
        full.vertices[0].position.x = static_cast<float>(version);
        full.indices[0] = version % 3;
        EXPECT_EQ(full.Update(full.vertex_capacity, full.index_capacity * 2), handle);
        full.resident.mesh_version = version;
        full.ExpectPresented(version);
        EXPECT_EQ(full.pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes);
        EXPECT_EQ(full.pool.live_allocation_count(), 14u);
        EXPECT_EQ(full.pool.retired_count(), 0u);
    }
    // World swaps retain the exact backing charge until observed completion.
    full.pool.clear();
    EXPECT_EQ(full.pool.live_allocation_count(), 0u);
    EXPECT_EQ(full.pool.retired_count(), 1u);
    full.pool.reclaim();
    EXPECT_EQ(full.pool.retired_count(), 0u);
    std::size_t vertex_slots = 0, index_slots = 0;
    full.pool.resident_capacity(vertex_slots, index_slots);
    EXPECT_EQ(vertex_slots, 0u);
    EXPECT_EQ(index_slots, 0u);
    full.pool.destroy();
    EXPECT_EQ(full.pool.storage_bytes(), 0u);
    EXPECT_EQ(full.pool.block_count(), 0u);
    EXPECT_EQ(glIsBuffer(block.vbo), GL_FALSE);
    EXPECT_EQ(glIsBuffer(block.ebo), GL_FALSE);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(FarWorldWaterGl, FullPoolDualTailGrowthAndOversizedRefusalsAreTransactional) {
    FullTerrainPool full;
    ASSERT_NO_FATAL_FAILURE(full.Fill(64));
    const auto handle = full.resident.pool_handle;
    const auto old = full.pool.allocation(handle);
    const auto digest = PoolDrawableDigest(full.pool, handle);
    // Each target independently exceeds the backing capacity. The tiny other
    // input is never copied, and neither frontier may advance on refusal.
    EXPECT_EQ(full.Update(full.vertex_capacity + 64, 3), ChunkGeometryPool::kInvalid);
    EXPECT_EQ(full.Update(3, full.index_capacity * 2 + 64), ChunkGeometryPool::kInvalid);
    EXPECT_EQ(PoolDrawableDigest(full.pool, handle), digest);
    EXPECT_EQ(full.pool.allocation(handle).vertex_slot, old.vertex_slot);
    EXPECT_EQ(full.pool.allocation(handle).index_slot, old.index_slot);
    EXPECT_EQ(full.pool.blocks()[old.block_index].vertex_high_water, old.vertex_slot);
    EXPECT_EQ(full.pool.blocks()[old.block_index].index_high_water, old.index_slot);
    full.ExpectPresented(7);
    // Both unused tails are smaller than a second reservation, but their sum
    // with the resident slots can hold the replacement, without new storage.
    full.vertices[0].position = {4, 5, 6};
    full.indices[0] = 1;
    EXPECT_EQ(full.Update(full.vertex_capacity, full.index_capacity * 2), handle);
    EXPECT_EQ(full.pool.allocation(handle).vertex_slot, full.vertex_capacity);
    EXPECT_EQ(full.pool.allocation(handle).index_slot, full.index_capacity * 2);
    EXPECT_EQ(full.pool.allocation(handle).vertex_count, full.vertex_capacity);
    EXPECT_EQ(full.pool.allocation(handle).index_count, full.index_capacity * 2);
    EXPECT_EQ(full.pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes);
    // Shrink the drawable counts without giving away either reserved slot.
    EXPECT_EQ(full.Update(3, 3), handle);
    EXPECT_TRUE(full.pool.valid_allocation(handle, 3));
    EXPECT_EQ(full.pool.allocation(handle).vertex_slot, full.vertex_capacity);
    EXPECT_EQ(full.pool.allocation(handle).index_slot, full.index_capacity * 2);
}

void CheckFullPoolCompletionRefusal(unsigned failure) {
    FullTerrainPool full;
    ASSERT_NO_FATAL_FAILURE(full.Fill());
    const auto handle = full.resident.pool_handle;
    const auto alloc = full.pool.allocation(handle);
    const auto digest = PoolDrawableDigest(full.pool, handle);
    PoolVertexReadback consumed(full.pool.blocks()[alloc.block_index], alloc.vertex_offset);
    full.pool.mark_submitted(alloc.block_index);
    FinalTerrainConsumer completion;
    completion.missing = failure == 0;
    completion.result = failure == 1 ? GL_WAIT_FAILED : GL_ALREADY_SIGNALED;
    completion.fence_error = failure == 2;
    completion.wait_error = failure == 3;
    completion.delete_error = failure == 4;
    full.vertices[0].position = {4, 5, 6};
    full.indices[0] = 1;
    EXPECT_EQ(full.Update(full.vertex_capacity, full.index_capacity * 2),
              ChunkGeometryPool::kInvalid);
    EXPECT_EQ(completion.fences, 1u);
    EXPECT_EQ(completion.polls, failure == 0 || failure == 2 ? 0u : 1u);
    EXPECT_EQ(consumed.Read(), glm::vec3(1, 2, 3));
    EXPECT_EQ(PoolDrawableDigest(full.pool, handle), digest);
    full.ExpectPresented(7);
    completion.missing = completion.fence_error = completion.wait_error = completion.delete_error =
        false;
    completion.result = GL_ALREADY_SIGNALED;
    const auto polls = completion.polls;
    full.pool.mark_submitted(alloc.block_index);
    // Missing/failed completions stay quarantined across later healthy draws.
    EXPECT_EQ(full.Update(full.vertex_capacity, full.index_capacity * 2),
              ChunkGeometryPool::kInvalid);
    EXPECT_EQ(completion.polls, polls);
    EXPECT_EQ(PoolDrawableDigest(full.pool, handle), digest);
    EXPECT_EQ(full.pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes);
    EXPECT_TRUE(full.pool.allocation(handle).live);
    full.ExpectPresented(7);
}

TEST_F(FarWorldWaterGl, FullPoolMissingCompletionKeepsResidentDrawable) {
    CheckFullPoolCompletionRefusal(0);
}
TEST_F(FarWorldWaterGl, FullPoolFailedCompletionKeepsResidentDrawable) {
    CheckFullPoolCompletionRefusal(1);
}
TEST_F(FarWorldWaterGl, FullPoolFenceErrorKeepsResidentDrawable) {
    CheckFullPoolCompletionRefusal(2);
}
TEST_F(FarWorldWaterGl, FullPoolWaitErrorKeepsResidentDrawable) {
    CheckFullPoolCompletionRefusal(3);
}
TEST_F(FarWorldWaterGl, FullPoolCompletionCleanupErrorKeepsResidentDrawable) {
    CheckFullPoolCompletionRefusal(4);
}

TEST_F(FarWorldWaterGl, FullPoolRetirementAndResidentReuseShareEightPolls) {
    FullTerrainPool full;
    ASSERT_NO_FATAL_FAILURE(full.Fill(9 * 64));
    const auto handle = full.resident.pool_handle;
    const auto old = full.pool.allocation(handle);
    std::array<u32, 9> handles{};
    for (auto& retired : handles) {
        retired = full.pool.allocate(full.vertices.data(), 3, full.indices.data(), 3, nullptr);
        ASSERT_NE(retired, ChunkGeometryPool::kInvalid);
    }
    full.pool.mark_submitted(old.block_index);
    DelayedPoolConsumer completion;
    for (const auto retired : handles)
        full.pool.free(retired);
    PoolVertexReadback consumed(full.pool.blocks()[old.block_index], old.vertex_offset);
    full.pool.mark_submitted(old.block_index);
    full.vertices[0].position = {4, 5, 6};
    EXPECT_EQ(full.Update(old.vertex_count, old.index_count), ChunkGeometryPool::kInvalid);
    EXPECT_EQ(completion.polls, 8u); // seven retirements plus the resident
    EXPECT_EQ(full.pool.retired_count(), 9u);
    EXPECT_EQ(consumed.Read(), glm::vec3(1, 2, 3));
    completion.result = GL_ALREADY_SIGNALED;
    const auto polls = completion.polls;
    EXPECT_EQ(full.Update(old.vertex_count, old.index_count), handle);
    EXPECT_EQ(completion.polls - polls, 8u);
    EXPECT_EQ(full.pool.retired_count(), 2u);
    full.pool.reclaim();
    EXPECT_EQ(full.pool.retired_count(), 0u);
    EXPECT_EQ(full.pool.live_allocation_count(), 14u);
    EXPECT_EQ(full.pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes);
}

TEST_F(FarWorldWaterGl, ReplacementRetirementFailureKeepsBothResidentStoresAndVersion) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> vertices{};
    vertices[0].position = {1, 2, 3};
    std::array<u32, 3> indices{0, 1, 2};
    ChunkRenderData resident;
    resident.pool_handle = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    resident.mesh_version = 7;
    resident.element_count = 3;
    ASSERT_NE(resident.pool_handle, ChunkGeometryPool::kInvalid);
    const auto old = pool.allocation(resident.pool_handle);
    const auto digest = PoolDrawableDigest(pool, resident.pool_handle);
    PoolVertexReadback consumed(pool.blocks()[old.block_index], old.vertex_offset);
    pool.mark_submitted(old.block_index);
    FinalTerrainConsumer completion;
    vertices[0].position = {4, 5, 6};
    indices[0] = 2;
    for (const bool missing : {true, false}) {
        completion.missing = missing;
        completion.fence_error = !missing;
        EXPECT_EQ(pool.update(resident.pool_handle, vertices.data(), 3, indices.data(), 3, nullptr),
                  ChunkGeometryPool::kInvalid);
        EXPECT_TRUE(pool.valid_allocation(resident.pool_handle, 3));
        EXPECT_EQ(pool.live_allocation_count(), 1u);
        EXPECT_EQ(pool.retired_count(), 0u);
        EXPECT_EQ(PoolDrawableDigest(pool, resident.pool_handle), digest);
        FarWorldLiveUpload presented;
        resident.SetPresentedTerrainUpload(presented, pool);
        EXPECT_TRUE(presented.valid_allocation);
        EXPECT_EQ(presented.uploaded_version, 7u);
    }
    EXPECT_EQ(consumed.Read(), glm::vec3(1, 2, 3));
    completion.fence_error = false;
    const auto accepted =
        pool.update(resident.pool_handle, vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(accepted, ChunkGeometryPool::kInvalid);
    EXPECT_NE(accepted, resident.pool_handle);
    EXPECT_FALSE(pool.allocation(resident.pool_handle).live);
    EXPECT_EQ(pool.live_allocation_count(), 1u);
    EXPECT_EQ(pool.retired_count(), 1u);
    pool.destroy();
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(FarWorldWaterGl, HandleCapStillAdmitsCompletedResidentReplacement) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> vertices{};
    const std::array<u32, 3> indices{0, 1, 2};
    u32 handle = ChunkGeometryPool::kInvalid;
    // Exactly 65,536 records; no extra record is needed for pressure reuse.
    for (std::size_t count = 0; count < ChunkGeometryPool::kMaxAllocations; ++count) {
        const auto allocated = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
        ASSERT_NE(allocated, ChunkGeometryPool::kInvalid);
        handle = allocated;
    }
    EXPECT_EQ(pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr),
              ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.live_allocation_count(), ChunkGeometryPool::kMaxAllocations);
    vertices[0].position = {1, 2, 3};
    EXPECT_EQ(pool.update(handle, vertices.data(), 3, indices.data(), 3, nullptr), handle);
    EXPECT_EQ(pool.live_allocation_count(), ChunkGeometryPool::kMaxAllocations);
    EXPECT_EQ(pool.retired_count(), 0u);
    pool.clear();
    EXPECT_EQ(pool.live_allocation_count(), 0u);
    EXPECT_EQ(pool.retired_count(), 0u);
    pool.destroy();
    EXPECT_EQ(pool.storage_bytes(), 0u);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(FarWorldWaterGl, WithinSlotBlockErrorsNeverFallThroughToResidentReuse) {
    ChunkGeometryPool pool;
    const std::array<VoxelVertex, 3> seed{};
    const std::array<u32, 3> triangle{0, 1, 2};
    ASSERT_NE(pool.allocate(seed.data(), 3, triangle.data(), 3, nullptr),
              ChunkGeometryPool::kInvalid);
    const u32 vertex_count = pool.blocks().front().vertex_capacity;
    const u32 index_count = pool.blocks().front().index_capacity;
    pool.destroy();
    // One complete block bounds scratch and leaves room for a second block;
    // replacement fits the resident, but staging must create fresh GL storage.
    std::vector<VoxelVertex> vertices(vertex_count);
    std::vector<u32> indices(index_count);
    vertices[0].position = {1, 2, 3};
    const auto handle =
        pool.allocate(vertices.data(), vertex_count, indices.data(), index_count, nullptr);
    ASSERT_NE(handle, ChunkGeometryPool::kInvalid);
    const auto old = pool.allocation(handle);
    const auto digest = PoolDrawableDigest(pool, handle);
    PoolVertexReadback consumed(pool.blocks()[old.block_index], old.vertex_offset);
    PoolIndexReadback consumed_indices(pool.blocks()[old.block_index], old.index_offset);
    pool.mark_submitted(old.block_index);
    EXPECT_EQ(consumed.Read(), glm::vec3(1, 2, 3));
    EXPECT_EQ(consumed_indices.Read()[0], 0u);
    FinalTerrainConsumer completion;
    completion.result = GL_ALREADY_SIGNALED;
    vertices[0].position = {4, 5, 6};
    indices[0] = 2;
    for (const GLenum target : {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER}) {
        for (const bool storage_failure : {false, true}) {
            FailedPoolBlock failure(target, storage_failure);
            EXPECT_EQ(
                pool.update(
                    handle, vertices.data(), vertex_count, indices.data(), index_count, nullptr),
                ChunkGeometryPool::kInvalid);
            EXPECT_EQ(failure.failures, 1u);
            EXPECT_EQ(PoolDrawableDigest(pool, handle), digest);
            EXPECT_EQ(pool.live_allocation_count(), 1u);
            EXPECT_EQ(pool.retired_count(), 0u);
            EXPECT_TRUE(pool.valid_allocation(handle, index_count));
        }
    }
    EXPECT_EQ(completion.polls, 0u);
    EXPECT_EQ(completion.fences, 0u);
    const auto accepted =
        pool.update(handle, vertices.data(), vertex_count, indices.data(), index_count, nullptr);
    ASSERT_NE(accepted, ChunkGeometryPool::kInvalid);
    EXPECT_NE(accepted, handle);
    const auto replacement_digest = PoolDrawableDigest(pool, accepted);
    EXPECT_NE(replacement_digest[0], digest[0]);
    EXPECT_NE(replacement_digest[1], digest[1]);
    pool.destroy();
}

TEST_F(FarWorldWaterGl, ExistingSliceInputAndGlErrorsNeverPublishReplacement) {
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> vertices{};
    std::array<u32, 3> indices{0, 1, 2};
    const auto handle = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(handle, ChunkGeometryPool::kInvalid);
    const auto digest = PoolDrawableDigest(pool, handle);
    EXPECT_EQ(pool.update(handle, nullptr, 3, indices.data(), 3, nullptr),
              ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.update(handle, vertices.data(), 3, nullptr, 3, nullptr),
              ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.update(handle, vertices.data(), 0, indices.data(), 3, nullptr),
              ChunkGeometryPool::kInvalid);
    EXPECT_EQ(pool.update(handle, vertices.data(), 3, indices.data(), 0, nullptr),
              ChunkGeometryPool::kInvalid);
    for (const bool vertex : {true, false}) {
        EXPECT_EQ(pool.update(handle,
                              vertices.data(),
                              vertex ? std::numeric_limits<u32>::max() : 3,
                              indices.data(),
                              vertex ? 3 : std::numeric_limits<u32>::max(),
                              nullptr),
                  ChunkGeometryPool::kInvalid);
        glBindBuffer(GL_TEXTURE_2D, 0);
        EXPECT_EQ(pool.update(handle, vertices.data(), 3, indices.data(), 3, nullptr),
                  ChunkGeometryPool::kInvalid);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
    EXPECT_EQ(pool.live_allocation_count(), 1u);
    EXPECT_TRUE(pool.valid_allocation(handle, 3));
    EXPECT_EQ(PoolDrawableDigest(pool, handle), digest);
    pool.destroy();
}

TEST_F(FarWorldWaterGl, DeferredTerrainSnapshotKeepsPresentedOwnershipBeyondInnerSphere) {
    ChunkGeometryPool pool;
    const std::array<VoxelVertex, 3> vertices{};
    const std::array<u32, 3> indices{0, 1, 2};
    ChunkRenderData resident;
    resident.pool_handle = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    ASSERT_NE(resident.pool_handle, ChunkGeometryPool::kInvalid);
    resident.element_count = 3;
    resident.mesh_version = 7;
    for (int refusal = 0; refusal < 5; ++refusal) {
        auto data = resident;
        if (refusal == 1)
            data.element_count = 6;
        if (refusal == 2)
            data.element_count = 0;
        if (refusal == 3)
            data.pool_handle = ChunkGeometryPool::kInvalid;
        if (refusal == 4)
            data.pool_handle = 1234;
        FarWorldLiveUpload upload;
        upload.cell = {-16, 0, -16, 0};
        upload.snapshot_version = 8; // newer snapshot deferred by upload admission
        data.SetPresentedTerrainUpload(upload, pool);
        EXPECT_EQ(upload.uploaded_version, 7u); // never claim version 8 uploaded
        FarWorldOwnership ownership;
        ownership.Begin(1);
        ASSERT_TRUE(ownership.Add(upload));
        ASSERT_TRUE(ownership.Finish());
        const std::array<double, 3> overlap{-248, 8, -248};
        EXPECT_TRUE(FarWorldFragmentInClip(overlap, 1040));
        EXPECT_EQ(ownership.Owns(overlap), refusal == 0);
        EXPECT_FALSE(ownership.OwnsWater(overlap));
    }
    pool.free(resident.pool_handle);
    FarWorldLiveUpload evicted;
    resident.SetPresentedTerrainUpload(evicted, pool);
    EXPECT_FALSE(IsFarWorldLiveUploadCurrent(evicted));
    pool.destroy();
}

TEST_F(FarWorldWaterGl, RetainedTerrainDrawSuppressesOverlappingFarPixelsDuringDeferral) {
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
    Shader shader((root / "g_buffer.vert").string().c_str(),
                  (root / "g_buffer.frag").string().c_str(),
                  {},
                  {"LUMINUMBRA_FAR_WORLD_LADDER 1"});
    ASSERT_TRUE(shader.IsValid()) << shader.Diagnostic();
    Configure(shader, {0, 256, 0}, {-248, 8, -248});
    FarWorldPassUniforms uniforms;
    uniforms.Cache(shader);
    ChunkGeometryPool pool;
    std::array<VoxelVertex, 3> vertices{};
    vertices[0].position = {-256, 8, -256};
    vertices[1].position = {-240, 8, -256};
    vertices[2].position = {-248, 8, -240};
    for (auto& vertex : vertices) {
        vertex.normal = {0, 1, 0};
        vertex.material_id = 3;
    }
    const std::array<u32, 3> indices{0, 1, 2};
    ChunkRenderData resident;
    resident.pool_handle = pool.allocate(vertices.data(), 3, indices.data(), 3, nullptr);
    resident.element_count = 3;
    resident.mesh_version = 7;
    ASSERT_NE(resident.pool_handle, ChunkGeometryPool::kInvalid);
    for (auto& vertex : vertices)
        vertex.material_id = 1;
    FarVolumeGlDriver driver;
    FarVolumeRenderMesh far_mesh;
    far_mesh.content = FarVolumeContent::Mixed;
    far_mesh.vertices.assign(vertices.begin(), vertices.end());
    far_mesh.indices.assign(indices.begin(), indices.end());
    // The production far uploader/draw binds both source SSBOs used for exact
    // ownership reconstruction; a raw pooled draw cannot supply that contract.
    auto far_allocation = UploadFarVolumeGpu(far_mesh, driver);
    ASSERT_NE(far_allocation, nullptr);
    const auto draw = [&](u32 handle) {
        const auto& allocation = pool.allocation(handle);
        glBindVertexArray(pool.blocks()[allocation.block_index].vao);
        // Uniform model path: this fixture has no instance-origin buffer.
        glDisableVertexAttribArray(3);
        glDrawElementsBaseVertex(
            GL_TRIANGLES,
            3,
            GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::size_t>(allocation.index_offset) *
                                          sizeof(u32)),
            static_cast<GLint>(allocation.vertex_offset));
        pool.mark_submitted(allocation.block_index);
    };
    glDisable(GL_DEPTH_TEST); // overlapping far pixels must be discarded by ownership
    for (const bool presented : {false, true}) {
        uniforms.Reset();
        uniforms.UseInstanceOrigin(false);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw(resident.pool_handle); // the old resident is actually presented
        ASSERT_NEAR(Pixel(1).a * 255, 3, .01f);
        FarWorldLiveUpload upload;
        upload.cell = {-16, 0, -16, 0};
        upload.snapshot_version = 8; // replacement deferred; no version-8 upload
        upload.uploaded_version = resident.mesh_version;
        upload.uploaded_indices = resident.element_count;
        upload.valid_allocation =
            pool.valid_allocation(resident.pool_handle, resident.element_count);
        if (presented)
            resident.SetPresentedTerrainUpload(upload, pool);
        EXPECT_EQ(upload.uploaded_version, 7u);
        FarWorldOwnership ownership;
        ownership.Begin(1);
        ASSERT_TRUE(ownership.Add(upload));
        ASSERT_TRUE(ownership.Finish());
        ASSERT_TRUE(driver.BindLiveOwnership(ownership.cells()));
        FarWorldDrawCommand command;
        command.relative_origin = {0, -256, 0};
        command.horizontal_outer_clip_meters = 1040;
        command.tile_edge_meters = 1024;
        command.live_cell_count = static_cast<std::int32_t>(ownership.cells().size());
        FarWorldPassUniformScope reset(uniforms);
        ASSERT_TRUE(uniforms.Prepare(command));
        ASSERT_TRUE(DrawFarVolumeGpu(*far_allocation, driver));
        // Without presented ownership the far material overwrites the old draw.
        EXPECT_NEAR(Pixel(1).a * 255, presented ? 3 : 1, .01f);
        driver.UnbindLiveOwnership();
    }
    driver.ReleaseLiveOwnership();
    glBindVertexArray(0);
    pool.destroy();
}

TEST(CaptureArtifacts, UnwritableReportImageInvalidScanAndCompleteOutput) {
    const Luminumbra::TestSupport::ScopedTempFile scratch("capture-", ".json");
    const auto directory = scratch.Path().parent_path();
    // A directory is an unwritable file destination on Windows and POSIX,
    // independent of privileges, ACLs and symlink support.
    EXPECT_FALSE(WritePixelBufferPpm(directory, 1, 1, {1, 2, 3}));
    EXPECT_FALSE(WriteCaptureText(directory, "[]\n"));
    FrameScanReport report;
    report.width = report.height = 1;
    report.total_pixels = 1;
    const auto scan_path = directory / "scan.json";
    EXPECT_FALSE(WriteFrameScanReport(report, scan_path));
    EXPECT_FALSE(std::filesystem::exists(scan_path));
    report.ok = true;
    EXPECT_FALSE(WriteFrameScanReport(report, directory));
    EXPECT_FALSE(WritePixelBufferPpm(directory / "short.ppm", 2, 2, {1, 2, 3}));
    EXPECT_FALSE(std::filesystem::exists(directory / "short.ppm"));
    EXPECT_TRUE(WritePixelBufferPpm(directory / "image.ppm", 1, 1, {1, 2, 3}));
    EXPECT_TRUE(WriteFrameScanReport(report, scan_path));
    EXPECT_TRUE(WriteCaptureText(directory / "survey.json", "[]\n"));
    std::ifstream index(directory / "survey.json", std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(index),
                               std::istreambuf_iterator<char>()};
    EXPECT_EQ(contents, "[]\n");
    index.close(); // Windows cannot unlink an open file.
}

TEST_F(FarWorldWaterGl, WetToDryAdoptsRemovalInBothWaterModes) {
    for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
        RenderPipeline pipeline;
        WaterUploadTestPeer::Upload(pipeline, 9, 3, mode);
        ASSERT_TRUE(WaterUploadTestPeer::HasWater(pipeline));
        WaterUploadTestPeer::DeferEmpty(pipeline, mode);
        ASSERT_TRUE(WaterUploadTestPeer::HasWater(pipeline));
        EXPECT_EQ(WaterUploadTestPeer::Data(pipeline).mesh_version, 9u);
        EXPECT_EQ(WaterUploadTestPeer::Data(pipeline).element_count, 3u);
        EXPECT_FALSE(pipeline.render_uploads_settled());
        WaterUploadTestPeer::AdoptEmpty(pipeline, mode);
        EXPECT_FALSE(WaterUploadTestPeer::HasWater(pipeline));
        EXPECT_TRUE(pipeline.render_uploads_settled());
        EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_budget.adopted, 1u);
        EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_budget.bytes, 0u);
        WaterUploadTestPeer::Upload(pipeline, 11, 3, mode);
        EXPECT_EQ(WaterUploadTestPeer::Data(pipeline).mesh_version, 11u);
    }
}

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

struct WaterSnapshot {
    WaterRenderData data{};
    std::vector<unsigned char> vertices;
    std::vector<unsigned char> indices;
};

void ReadWaterBuffer(GLenum target, GLuint name, std::vector<unsigned char>& bytes) {
    glBindBuffer(target, name);
    GLint64 size = 0;
    glGetBufferParameteri64v(target, GL_BUFFER_SIZE, &size);
    ASSERT_GT(size, 0);
    // These fixtures use at most nine vertices/indices per store. Check before
    // allocating so a corrupt size cannot produce an unbounded test readback.
    ASSERT_LE(size, static_cast<GLint64>(9 * sizeof(VoxelVertex)));
    bytes.resize(static_cast<std::size_t>(size));
    glGetBufferSubData(target, 0, static_cast<GLsizeiptr>(size), bytes.data());
}

void CaptureWater(const RenderPipeline& pipeline, WaterSnapshot& snapshot) {
    snapshot.data = WaterUploadTestPeer::Data(pipeline);
    glBindVertexArray(snapshot.data.vao_id);
    ReadWaterBuffer(GL_ARRAY_BUFFER, snapshot.data.vbo_id, snapshot.vertices);
    ReadWaterBuffer(GL_ELEMENT_ARRAY_BUFFER, snapshot.data.ebo_id, snapshot.indices);
    glBindVertexArray(0);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

void ExpectRetainedWater(const RenderPipeline& pipeline, const WaterSnapshot& expected) {
    WaterSnapshot actual;
    CaptureWater(pipeline, actual);
    EXPECT_EQ(actual.data.vao_id, expected.data.vao_id);
    EXPECT_EQ(actual.data.vbo_id, expected.data.vbo_id);
    EXPECT_EQ(actual.data.ebo_id, expected.data.ebo_id);
    EXPECT_EQ(actual.data.mesh_version, expected.data.mesh_version);
    EXPECT_EQ(actual.data.element_count, expected.data.element_count);
    EXPECT_EQ(actual.data.vertex_capacity, expected.data.vertex_capacity);
    EXPECT_EQ(actual.data.index_capacity, expected.data.index_capacity);
    EXPECT_EQ(actual.data.ladder_upload_version, expected.data.ladder_upload_version);
    EXPECT_EQ(actual.data.LadderUploadValid(), expected.data.LadderUploadValid());
    EXPECT_EQ(actual.vertices, expected.vertices);
    EXPECT_EQ(actual.indices, expected.indices);
}

TEST_F(FarWorldWaterGl, FailedLegacyReplacementRetainsResidentBuffersAndVersion) {
    RenderPipeline pipeline;
    WaterUploadTestPeer::Upload(pipeline, 9, 3, FarWorldMode::Legacy);
    const auto resident = WaterUploadTestPeer::Data(pipeline);
    WaterSnapshot original;
    CaptureWater(pipeline, original);
    {
        FailedWaterUpload failure(true, GL_ELEMENT_ARRAY_BUFFER);
        WaterUploadTestPeer::Upload(pipeline, 10, 6, FarWorldMode::Legacy);
        EXPECT_EQ(failure.failures(), 1);
    }
    const auto& retained = WaterUploadTestPeer::Data(pipeline);
    EXPECT_EQ(retained.vao_id, resident.vao_id);
    EXPECT_EQ(retained.vbo_id, resident.vbo_id);
    EXPECT_EQ(retained.ebo_id, resident.ebo_id);
    EXPECT_EQ(retained.mesh_version, 9u);
    EXPECT_EQ(retained.element_count, 3u);
    EXPECT_FALSE(retained.LadderUploadValid());
    ExpectRetainedWater(pipeline, original);
    EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_upload_failures, 1u);
    WaterUploadTestPeer::Upload(pipeline, 10, 6, FarWorldMode::Legacy);
    EXPECT_EQ(retained.mesh_version, 10u);
    EXPECT_EQ(retained.element_count, 6u);
    EXPECT_FALSE(retained.LadderUploadValid());
    const auto accepted = retained;
    WaterSnapshot replacement;
    CaptureWater(pipeline, replacement);
    {
        FailedWaterUpload failure(true, GL_ELEMENT_ARRAY_BUFFER);
        // Nine is a valid triangle count and exceeds both the resident's six
        // and the spare slot's three elements, so this must reach BufferData.
        WaterUploadTestPeer::Upload(pipeline, 11, 9, FarWorldMode::Legacy);
        EXPECT_EQ(failure.failures(), 1);
    }
    EXPECT_EQ(retained.vao_id, accepted.vao_id);
    EXPECT_EQ(retained.vbo_id, accepted.vbo_id);
    EXPECT_EQ(retained.ebo_id, accepted.ebo_id);
    EXPECT_EQ(retained.mesh_version, 10u);
    EXPECT_EQ(retained.element_count, 6u);
    ExpectRetainedWater(pipeline, replacement);
    EXPECT_FALSE(retained.LadderUploadValid());
    EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_upload_failures, 2u);
    WaterUploadTestPeer::Upload(pipeline, 11, 9, FarWorldMode::Legacy);
    EXPECT_EQ(retained.mesh_version, 11u);
    EXPECT_EQ(retained.element_count, 9u);
    EXPECT_FALSE(retained.LadderUploadValid());
    EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_upload_failures, 2u);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(FarWorldWaterGl, WithinCapacityReplacementRefusalRetainsBytesAndRetriesInBothModes) {
    for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
        for (const auto target : {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER}) {
            RenderPipeline pipeline;
            WaterUploadTestPeer::Upload(pipeline, 8, 6, mode);
            WaterUploadTestPeer::Upload(pipeline, 9, 6, mode);
            WaterSnapshot resident;
            CaptureWater(pipeline, resident);
            {
                // The spare has capacity six. Three must use BufferSubData;
                // a growth-only injection would miss this refusal entirely.
                FailedWaterUpload failure(false, static_cast<GLenum>(target));
                WaterUploadTestPeer::Upload(pipeline, 10, 3, mode);
                EXPECT_EQ(failure.failures(), 1);
            }
            ExpectRetainedWater(pipeline, resident);
            EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_upload_failures, 1u);
            EXPECT_FALSE(WaterUploadTestPeer::LiveUpload(pipeline, 10).water_pass_eligible);
            EXPECT_EQ(WaterUploadTestPeer::PresentedUpload(pipeline).water_pass_eligible,
                      mode == FarWorldMode::Ladder);
            WaterUploadTestPeer::Upload(pipeline, 10, 3, mode);
            const auto& accepted = WaterUploadTestPeer::Data(pipeline);
            EXPECT_EQ(accepted.mesh_version, 10u);
            EXPECT_EQ(accepted.element_count, 3u);
            EXPECT_EQ(accepted.LadderUploadValid(), mode == FarWorldMode::Ladder);
            EXPECT_EQ(WaterUploadTestPeer::LiveUpload(pipeline, 10).water_pass_eligible,
                      mode == FarWorldMode::Ladder);
            EXPECT_FALSE(WaterUploadTestPeer::LiveUpload(pipeline, 9).water_pass_eligible);
            EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_upload_failures, 1u);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }
    }
}

TEST_F(FarWorldWaterGl, IncompleteTriangleRefusesBeforeGpuAndPreservesResident) {
    for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
        RenderPipeline pipeline;
        WaterUploadTestPeer::Upload(pipeline, 9, 6, mode);
        WaterSnapshot resident;
        CaptureWater(pipeline, resident);
        {
            FailedWaterUpload failure(true, GL_ELEMENT_ARRAY_BUFFER);
            WaterUploadTestPeer::Upload(pipeline, 10, 7, mode);
            EXPECT_EQ(failure.failures(), 0); // Size validation precedes every GPU write.
        }
        ExpectRetainedWater(pipeline, resident);
        EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_upload_failures, 1u);
        EXPECT_FALSE(WaterUploadTestPeer::LiveUpload(pipeline, 10).water_pass_eligible);
        WaterUploadTestPeer::Upload(pipeline, 10, 9, mode);
        EXPECT_EQ(WaterUploadTestPeer::Data(pipeline).mesh_version, 10u);
        EXPECT_EQ(WaterUploadTestPeer::Data(pipeline).element_count, 9u);
        EXPECT_EQ(WaterUploadTestPeer::Data(pipeline).LadderUploadValid(),
                  mode == FarWorldMode::Ladder);
        EXPECT_EQ(pipeline.get_last_mesh_upload_stats().water_upload_failures, 1u);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
}
} // namespace
