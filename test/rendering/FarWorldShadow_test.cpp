#include "luminumbra_client/rendering/FarWorldSurfaceTag.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>

// Production stage and shader readbacks run in render_smoke_test's existing
// software-GL lane. Pure surface-tag quantization remains in common_tests.
#include "TerrainCullingTestPeer.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_client/rendering/RenderInputs.h"
#include "luminumbra_client/rendering/Shader.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;

class ShadowBoundaryDriver final : public FarVolumeGpuDriver {
public:
    explicit ShadowBoundaryDriver(GLuint shadow_fbo)
        : shadow_fbo_(shadow_fbo) {}
    bool ValidContext() const noexcept override {
        return true;
    }
    bool HasError() noexcept override {
        return false;
    }
    void GetVertexArrayBinding(std::uint32_t& name) noexcept override {
        name = vao;
    }
    void GetVertexBufferBinding(std::uint32_t& name) noexcept override {
        name = vbo;
    }
    void CreateVertexArray(std::uint32_t& name) noexcept override {
        name = next++;
    }
    void CreateBuffer(std::uint32_t& name) noexcept override {
        name = next++;
    }
    void DeleteVertexArray(std::uint32_t) noexcept override {}
    void DeleteBuffer(std::uint32_t) noexcept override {}
    void BindVertexArray(std::uint32_t name) noexcept override {
        vao = name;
    }
    void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept override {
        (kind == FarVolumeBufferKind::Vertex ? vbo : ebo) = name;
    }
    void
    UploadBuffer(FarVolumeBufferKind kind, std::intptr_t bytes, const void*) noexcept override {
        sizes[kind == FarVolumeBufferKind::Vertex ? vbo : ebo] = bytes;
    }
    void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept override {
        bytes = sizes[kind == FarVolumeBufferKind::Vertex ? vbo : ebo];
    }
    void SetAttribute(FarVolumeVertexAttribute, std::int32_t, std::size_t) noexcept override {}
    void EnableAttribute(FarVolumeVertexAttribute) noexcept override {}
    bool BindLiveOwnership(std::span<const FarWorldLiveCell> cells) noexcept override {
        EXPECT_TRUE(cells.empty());
        Record(submissions, cascade_submissions);
        return true;
    }
    bool DrawIndexed(std::int32_t count) noexcept override {
        EXPECT_NE(vao, 0u);
        EXPECT_GT(count, 0);
        Record(draws, cascade_draws);
        return true;
    }
    bool Excluded() const {
        return submissions == 0 && draws == 0 &&
               std::all_of(cascade_submissions.begin(),
                           cascade_submissions.end(),
                           [](std::size_t count) { return count == 0; }) &&
               std::all_of(cascade_draws.begin(), cascade_draws.end(), [](std::size_t count) {
                   return count == 0;
               });
    }
    void Reset() {
        submissions = draws = 0;
        cascade_submissions.fill(0);
        cascade_draws.fill(0);
    }
    std::size_t submissions = 0, draws = 0;
    std::array<std::size_t, ShadowMap::CASCADE_COUNT> cascade_submissions{}, cascade_draws{};

private:
    // Observe the real layer selected by ShadowPass, rather than assigning a
    // cascade index in the test's callback. Total counts also catch submissions
    // made by the pipeline before/after the per-cascade loop.
    void Record(std::size_t& total,
                std::array<std::size_t, ShadowMap::CASCADE_COUNT>& per_cascade) noexcept {
        ++total;
        GLint bound = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        if (static_cast<GLuint>(bound) != shadow_fbo_)
            return;
        GLint layer = -1;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_DEPTH_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER,
                                              &layer);
        EXPECT_GE(layer, 0);
        EXPECT_LT(layer, ShadowMap::CASCADE_COUNT);
        if (layer >= 0 && layer < ShadowMap::CASCADE_COUNT)
            ++per_cascade[static_cast<std::size_t>(layer)];
    }
    const GLuint shadow_fbo_;
    std::uint32_t vao = 0, vbo = 0, ebo = 0, next = 1;
    std::array<std::int64_t, 32> sizes{};
};

// Same bounded column authority and AdvanceFrame/settlement seam as
// FarWorldRenderer_test. All five tiers contain real uploaded geometry.
class ShadowBoundaryFixture final : public FarWorldPristineFixture {
public:
    std::uint64_t content_identity() const noexcept override {
        return 42;
    }
    FarWorldWaterCapability WaterCapability() const noexcept override {
        return FarWorldWaterCapability::ExplicitlyDry;
    }
    std::optional<FarWorldWaterBound>
    WaterBound(const Luminumbra::World::FarTileKey&) const override {
        return FarWorldWaterBound{true, 0, 0};
    }
    std::optional<FarWorldWaterSample> WaterSample(std::int64_t, std::int64_t) const override {
        return FarWorldWaterSample{false, 0};
    }
    std::uint64_t owned_bytes() const noexcept override {
        return sizeof(*this);
    }
    Luminumbra::World::FarVolumeNumericProfile numeric() const noexcept override {
        return Luminumbra::World::FarVolumeNumericProfile::RawFloat;
    }
    std::optional<float> Height(std::int64_t, std::int64_t) const override {
        return 0.0f;
    }
    std::optional<Luminumbra::World::FarVolumeSample>
    Sample(const Luminumbra::World::FarVolumePosition& p,
           float,
           std::uint32_t,
           Luminumbra::World::FarCaveMode) const override {
        const bool column = p.x >= 0 && p.x <= 256 && p.z >= 0 && p.z <= 256;
        return Luminumbra::World::FarVolumeSample{column ? static_cast<float>(p.y) + .25f : 3.0f,
                                                  1};
    }
};

struct ShadowBoundaryJobs {
    Luminumbra::JobSystem jobs;
    ShadowBoundaryJobs() {
        jobs.startup(2);
    }
    ~ShadowBoundaryJobs() {
        jobs.shutdown();
    }
};

class FarWorldShadowGl : public testing::Test {
protected:
    void SetUp() override {
        initialized = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(8, 8, "far shadow contract", nullptr, nullptr);
        ASSERT_NE(window, nullptr);
        glfwMakeContextCurrent(window);
        loaded = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
        ASSERT_TRUE(loaded);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DITHER);
        glViewport(0, 0, 1, 1);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenFramebuffers(1, &fbo);
    }
    void TearDown() override {
        if (loaded) {
            glUseProgram(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindVertexArray(0);
            for (const auto texture : textures)
                glDeleteTextures(1, &texture);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, 0);
            glDeleteBuffers(1, &ownership);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
            glDeleteFramebuffers(1, &fbo);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }
        if (window)
            glfwDestroyWindow(window);
        if (initialized)
            glfwTerminate();
    }
    std::string Path(const char* name) {
        return (std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders" / name).string();
    }
    GLuint Texture(GLenum target,
                   GLint internal,
                   GLenum format,
                   const float* data,
                   int width = 1,
                   int height = 1) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        glBindTexture(target, texture);
        if (target == GL_TEXTURE_2D_ARRAY)
            glTexImage3D(target, 0, internal, width, height, 4, 0, format, GL_FLOAT, data);
        else
            glTexImage2D(target, 0, internal, width, height, 0, format, GL_FLOAT, data);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return texture;
    }
    void Bind(
        Shader& shader, const char* name, int unit, GLuint texture, GLenum target = GL_TEXTURE_2D) {
        shader.setInt(name, unit);
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(target, texture);
    }
    void Attach(GLuint texture, int slot = 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + slot, GL_TEXTURE_2D, texture, 0);
    }
    glm::vec4 Pixel(int slot = 0) {
        glm::vec4 pixel{};
        glReadBuffer(GL_COLOR_ATTACHMENT0 + slot);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, glm::value_ptr(pixel));
        return pixel;
    }
    GLFWwindow* window = nullptr;
    bool initialized = false, loaded = false;
    GLuint vao = 0, fbo = 0, vbo = 0, ownership = 0;
    std::vector<GLuint> textures;
};

TEST_F(FarWorldShadowGl, UploadedLadderAllocationsNeverEnterProductionCascades) {
    // Jobs outlive the pipeline: its destructor drains residency and releases
    // fake allocations while this fixture's GL context is still current.
    ShadowBoundaryJobs workers;
    RenderPipeline pipeline;
    constexpr int resolution = 8;
    auto& pass = TerrainCullingTestPeer::InitShadow(pipeline, LUMINUMBRA_SOURCE_ROOT, resolution);
    ASSERT_TRUE(pass.shader());
    ASSERT_TRUE(pass.shader()->IsValid()) << pass.shader()->Diagnostic();
    ASSERT_NE(pass.shadow_map().fbo_id, 0u);
    ASSERT_NE(pass.shadow_map().depth_texture_array, 0u);
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    auto backend = std::make_unique<ShadowBoundaryDriver>(pass.shadow_map().fbo_id);
    auto* observer = backend.get();
    auto& far = pipeline.far_world_renderer();
    far.SetRequested(FarWorldMode::Ladder);
    far.InstallBackend(std::move(backend));
    ASSERT_EQ(far.BindFixture(workers.jobs,
                              std::make_shared<const ShadowBoundaryFixture>(),
                              1,
                              1,
                              FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    ASSERT_EQ(far.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
    ASSERT_NE(far.Residency(), nullptr);
    std::array<FarVolumeWantedRequest, 5> requests{};
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const Luminumbra::World::FarTileKey key{static_cast<std::uint8_t>(i + 1), 0, 0};
        Luminumbra::World::FarVolumeTileKey canonical;
        ASSERT_TRUE(Luminumbra::World::ToFarVolumeTileKey(key, canonical));
        requests[i] = {{1, 42, 0, key, 1, 1}, {canonical, {}}, 0};
    }
    std::uint64_t frame = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool settled = false;
    do {
        ASSERT_TRUE(far.AdvanceFrame(++frame, {0, 0, 0}, requests));
        settled = true;
        for (const auto& request : requests) {
            const auto observed = far.Residency()->Observe(request.identity.key);
            ASSERT_TRUE(observed);
            if (!observed)
                return;
            ASSERT_NE(observed->eligibility, FarVolumeEligibility::RequestRejected);
            settled = settled && observed->eligibility == FarVolumeEligibility::Settled;
        }
        if (!settled)
            std::this_thread::yield();
    } while (!settled && std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(settled);
    for (const auto& request : requests) {
        const auto* allocation = far.Residency()->Resource(request.identity.key);
        ASSERT_NE(allocation, nullptr);
        EXPECT_GT(allocation->bytes(), 0u);
    }
    far.BeginLiveOwnership(frame);
    ASSERT_TRUE(far.FinishLiveOwnership());

    // Prove real residency allocations are submit-ready before the exclusion
    // assertion; a refused/unsettled FarWorld would make that assertion vacuous.
    std::size_t prepared = 0;
    const FarWorldPrepareDraw prepare = [&](const FarWorldDrawCommand& command) {
        EXPECT_GT(command.index_count, 0);
        ++prepared;
        return true;
    };
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const auto drawable = far.Submit({0, 0, 0}, prepare);
    ASSERT_EQ(drawable.refused, 0u);
    ASSERT_EQ(drawable.draws, requests.size());
    ASSERT_EQ(prepared, requests.size());
    ASSERT_EQ(observer->draws, requests.size());
    ASSERT_EQ(observer->submissions, 1u);
    observer->Reset();
    prepared = 0;

    // Enter the production pipeline stage and its real live terrain submitter.
    // Seed every layer so readback also proves ShadowPass ran all cascades,
    // rather than returning early and trivially satisfying the zero counters.
    const float stale_depth = 0.25f;
    glClearTexImage(
        pass.shadow_map().depth_texture_array, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &stale_depth);
    Camera camera;
    TerrainCullingTestPeer::ExecuteShadow(pipeline, camera);
    ASSERT_EQ(pass.shadow_map().light_space_matrices.size(),
              static_cast<std::size_t>(ShadowMap::CASCADE_COUNT));
    EXPECT_TRUE(observer->Excluded());
    EXPECT_EQ(observer->submissions, 0u);
    EXPECT_EQ(observer->draws, 0u);
    std::vector<float> depths(resolution * resolution * ShadowMap::CASCADE_COUNT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pass.shadow_map().depth_texture_array);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths.data());
    for (int cascade = 0; cascade < ShadowMap::CASCADE_COUNT; ++cascade) {
        SCOPED_TRACE(cascade);
        EXPECT_EQ(observer->cascade_submissions[cascade], 0u);
        EXPECT_EQ(observer->cascade_draws[cascade], 0u);
        const auto begin = depths.begin() + cascade * resolution * resolution;
        EXPECT_TRUE(std::all_of(
            begin, begin + resolution * resolution, [](float value) { return value == 1.0f; }));
    }
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    // Companion negative control: inject a real FarWorld::Submit through the
    // existing submitter seam, still driven by ShadowPass's production loop.
    // The SAME exclusion predicate must reject it, in every actual GL layer.
    ShadowPassInput injected;
    injected.light_space_matrices = pass.shadow_map().light_space_matrices;
    injected.submit_terrain = [&](const glm::vec4(&)[6]) {
        const auto stats = far.Submit({0, 0, 0}, prepare);
        EXPECT_EQ(stats.refused, 0u);
        return TerrainSubmitStats{stats.draws, stats.draws, stats.indices};
    };
    const auto injected_stats = pass.execute(RenderContext{}, injected);
    EXPECT_FALSE(observer->Excluded()) << "an injected far submission must fail exclusion";
    EXPECT_EQ(prepared, requests.size() * ShadowMap::CASCADE_COUNT);
    EXPECT_EQ(observer->submissions, static_cast<std::size_t>(ShadowMap::CASCADE_COUNT));
    EXPECT_EQ(observer->draws, requests.size() * ShadowMap::CASCADE_COUNT);
    for (int cascade = 0; cascade < ShadowMap::CASCADE_COUNT; ++cascade) {
        SCOPED_TRACE(cascade);
        EXPECT_EQ(observer->cascade_submissions[cascade], 1u);
        EXPECT_EQ(observer->cascade_draws[cascade], requests.size());
        EXPECT_EQ(injected_stats[cascade].draws, requests.size());
        EXPECT_GT(injected_stats[cascade].indices, 0u);
    }
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(FarWorldShadowGl, ProductionGBufferPreservesMaterialBytesAndTagsWaterAndAOEndpoints) {
    Shader ordinary(Path("g_buffer.vert").c_str(), Path("g_buffer.frag").c_str());
    Shader ladder(Path("g_buffer.vert").c_str(),
                  Path("g_buffer.frag").c_str(),
                  {},
                  {"LUMINUMBRA_FAR_WORLD_LADDER 1"});
    ASSERT_TRUE(ordinary.IsValid()) << ordinary.Diagnostic();
    ASSERT_TRUE(ladder.IsValid()) << ladder.Diagnostic();

    std::array<GLuint, 5> outputs{};
    const std::array<GLint, 5> formats{GL_RGB16F, GL_RGBA8, GL_RGBA8, GL_RG16F, GL_RG16F};
    const std::array<GLenum, 5> channels{GL_RGB, GL_RGBA, GL_RGBA, GL_RG, GL_RG};
    const std::array<GLenum, 5> draw_buffers{GL_COLOR_ATTACHMENT0,
                                             GL_COLOR_ATTACHMENT1,
                                             GL_COLOR_ATTACHMENT2,
                                             GL_COLOR_ATTACHMENT3,
                                             GL_COLOR_ATTACHMENT4};
    for (int i = 0; i < 5; ++i) {
        outputs[i] = Texture(GL_TEXTURE_2D, formats[i], channels[i], nullptr);
        Attach(outputs[i], i);
    }
    glDrawBuffers(5, draw_buffers.data());
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

    std::array<float, 256 * 4 * 4> lut{};
    const GLuint material_lut = Texture(GL_TEXTURE_2D, GL_RGBA32F, GL_RGBA, lut.data(), 256, 4);
    const GLuint arrays = Texture(GL_TEXTURE_2D_ARRAY, GL_RGBA8, GL_RGBA, nullptr);
    const float triangle[]{-200, -200, -200, 600, -200, -200, -200, 600, -200};
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glVertexAttrib3f(1, 0, 0, 1);
    // A valid empty ownership store avoids relying on an unbound active SSBO.
    const std::array<std::int32_t, 4> empty_cell{};
    glGenBuffers(1, &ownership);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ownership);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(empty_cell), empty_cell.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, ownership);
    Camera camera;
    camera.SelectFarWorldPlane(FarWorldMode::Ladder);
    const auto projection = camera.GetProjectionMatrix(1, 1);
    const auto render = [&](Shader& shader, bool far, std::uint32_t id) {
        shader.use();
        Bind(shader, "u_materialLUT", 0, material_lut);
        const char* samplers[]{"u_terrainTextures",
                               "u_terrainNormals",
                               "u_skinnedTextures",
                               "u_terrainRoughness",
                               "u_staticNormals",
                               "u_staticSurface"};
        for (int i = 0; i < 6; ++i)
            Bind(shader, samplers[i], i + 1, arrays, GL_TEXTURE_2D_ARRAY);
        shader.setMat4("projection", projection);
        shader.setMat4("view", glm::mat4(1));
        shader.setMat4("model", glm::mat4(1));
        shader.setMat3("normalMatrix", glm::mat3(1));
        shader.setMat3("u_normalViewMatrix", glm::mat3(1));
        shader.setInt("u_farWorldLadder", far ? 1 : 0);
        if (far)
            shader.setFloat("u_farRayBiasMeters", 2);
        shader.setFloat("u_farOuterRadius", 1040);
        shader.setInt("u_forceFlat", 1);
        glVertexAttribI1ui(2, id);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<glm::vec4, 5> pixels{};
        for (int i = 0; i < 5; ++i)
            pixels[i] = Pixel(i);
        return pixels;
    };
    for (const float ao : {0.0f, 1.0f, 0.3333f}) {
        for (int id = 0; id < 256; ++id) {
            lut[id * 4] = 0.25f;
            lut[id * 4 + 1] = 0.7f;
            lut[id * 4 + 2] = ao;
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, material_lut);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 4, GL_RGBA, GL_FLOAT, lut.data());
        for (std::uint32_t id = 0; id < 256; ++id) {
            SCOPED_TRACE(id);
            const auto legacy = render(ordinary, false, id);
            const auto inert = render(ladder, false, id);
            for (int i = 0; i < 5; ++i) {
                EXPECT_EQ(legacy[i], inert[i]);
            }
            const auto far = render(ladder, true, id);
            if (id == 7u) { // The live-water path is discarded in both variants.
                EXPECT_EQ(far[3], glm::vec4(0, 0, 0, 1));
                continue;
            }
            EXPECT_EQ(far[1], legacy[1]); // Whole RGBA8 normal/material byte payload.
            EXPECT_EQ(static_cast<std::uint32_t>(std::lround(far[1].a * 255)), id);
            EXPECT_FLOAT_EQ(far[1].b, 0); // Existing marker lane is untouched.
            EXPECT_EQ(far[2], legacy[2]);
            EXPECT_FLOAT_EQ(far[3].r, legacy[3].r);
            EXPECT_TRUE(IsFarWorldSurface(far[3].g));
            EXPECT_FALSE(IsFarWorldSurface(legacy[3].g));
            EXPECT_NEAR(DecodeFarWorldSurfaceAO(far[3].g), ao, 1.0f / 1024.0f);
            if (ao == 0 || ao == 1) {
                EXPECT_FLOAT_EQ(DecodeFarWorldSurfaceAO(far[3].g), ao);
                EXPECT_FLOAT_EQ(legacy[3].g, ao);
            }
        }
    }
}

TEST_F(FarWorldShadowGl, ProductionLightingExcludesCascadesButKeepsLiveShadowsAndMarkers) {
    Shader shader(Path("fullscreen_tri.vert").c_str(), Path("lighting_pass.frag").c_str());
    ASSERT_TRUE(shader.IsValid()) << shader.Diagnostic();
    shader.use();
    const auto output = Texture(GL_TEXTURE_2D, GL_RGBA32F, GL_RGBA, nullptr);
    Attach(output);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    const float position[]{0, 0, -200};
    const float normal[]{0.5f, 0.5f, 0, 1.0f / 255.0f};
    const float albedo[]{0.18f, 0.18f, 0.18f, 0.7f};
    const float material[]{0, 1};
    const float one = 1;
    const std::array<float, 4> zero{};
    const auto pos = Texture(GL_TEXTURE_2D, GL_RGB16F, GL_RGB, position);
    const auto nrm = Texture(GL_TEXTURE_2D, GL_RGBA8, GL_RGBA, normal);
    const auto alb = Texture(GL_TEXTURE_2D, GL_RGBA8, GL_RGBA, albedo);
    const auto mat = Texture(GL_TEXTURE_2D, GL_RG16F, GL_RG, material);
    const auto ssao = Texture(GL_TEXTURE_2D, GL_R16F, GL_RED, &one);
    const auto lut = Texture(GL_TEXTURE_2D, GL_RGBA32F, GL_RGBA, zero.data());
    const auto environment = Texture(GL_TEXTURE_2D, GL_RG16F, GL_RG, zero.data());
    const std::array<float, 4> depths{1, 1, 1, 1};
    const auto shadow = Texture(GL_TEXTURE_2D_ARRAY, GL_R32F, GL_RED, depths.data());
    std::array<float, 12> tint_pixels{};
    tint_pixels.fill(1);
    const auto tint = Texture(GL_TEXTURE_2D_ARRAY, GL_RGB16F, GL_RGB, tint_pixels.data());
    Bind(shader, "gPosition", 0, pos);
    Bind(shader, "gNormalMaterial", 1, nrm);
    Bind(shader, "gAlbedoRoughness", 2, alb);
    Bind(shader, "gMetallicAO", 3, mat);
    Bind(shader, "u_ssao", 4, ssao);
    Bind(shader, "u_materialLUT", 5, lut);
    Bind(shader, "u_environmentBrdf", 6, environment);
    Bind(shader, "u_shadowCascades", 7, shadow, GL_TEXTURE_2D_ARRAY);
    Bind(shader, "u_shadowTintCascades", 8, tint, GL_TEXTURE_2D_ARRAY);
    shader.setMat4("u_inverseView", glm::mat4(1));
    shader.setVec3("u_viewPos", 0, 0, 0);
    shader.setVec3("u_sun.direction", 0, 0, -1);
    shader.setVec3("u_moonDir", 0, 0, -1);
    shader.setVec3("u_moonRadiance", 0.25f, 0.25f, 0.25f);
    shader.setVec4("u_cascadeSplits", 15, 40, 100, 250);
    shader.setInt("u_shadowTintEnabled", 1);
    // Every receiver maps to the middle of its cascade, at depth 0.5.
    glm::mat4 light(0);
    light[3][3] = 1;
    for (int i = 0; i < 4; ++i)
        shader.setMat4("u_lightSpaceMatrices[" + std::to_string(i) + "]", light);
    const auto render = [&](bool far, float shadow_depth, float tint_value) {
        const float values[]{0, EncodeFarWorldSurfaceAO(1, far)};
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, mat);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RG, GL_FLOAT, values);
        std::array<float, 4> depth_pixels{};
        depth_pixels.fill(shadow_depth);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D_ARRAY, shadow);
        glTexSubImage3D(
            GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 4, GL_RED, GL_FLOAT, depth_pixels.data());
        tint_pixels.fill(tint_value);
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tint);
        glTexSubImage3D(
            GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 4, GL_RGB, GL_FLOAT, tint_pixels.data());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        return Pixel();
    };
    for (const bool night : {false, true}) {
        SCOPED_TRACE(night);
        shader.setVec3("u_sun.color", night ? glm::vec3(0) : glm::vec3(0.4f));
        const auto far_lit = render(true, 1, 1);
        EXPECT_GT(far_lit.r, 0.01f);
        EXPECT_EQ(render(true, 0, 1), far_lit);
        EXPECT_EQ(render(true, 1, 0.15f), far_lit);
        const auto live_lit = render(false, 1, 1);
        EXPECT_EQ(live_lit, far_lit);
        EXPECT_LT(render(false, 0, 1).r, live_lit.r);
        if (!night) {
            EXPECT_LT(render(false, 1, 0.15f).r, live_lit.r);
        }
    }
    // Marker emissivity still uses normalData.b, independently of the AO sign.
    shader.setVec3("u_sun.color", glm::vec3(0));
    shader.setVec3("u_moonRadiance", glm::vec3(0));
    const auto dark = render(false, 1, 1);
    const float marker[]{0.5f, 0.5f, 0.5f, 1.0f / 255.0f};
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, nrm);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, marker);
    const auto glowing = render(false, 1, 1);
    EXPECT_GT(glowing.r, dark.r + 0.1f);
    EXPECT_EQ(render(true, 0, 0.15f), glowing);

    // SSAO still attenuates ambient on both surface classes. Isolate it from
    // direct light and marker emission so a cascade bypass cannot mask a failure.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, nrm);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, normal);
    shader.setVec3("u_skyAmbientColor", glm::vec3(0.2f));
    const auto ambient = render(false, 1, 1);
    EXPECT_GT(ambient.r, 0.01f);
    EXPECT_EQ(render(true, 0, 0.15f), ambient);
    const float occluded_ao = 0.25f;
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ssao);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_FLOAT, &occluded_ao);
    const auto occluded_ambient = render(false, 1, 1);
    EXPECT_LT(occluded_ambient.r, ambient.r);
    EXPECT_EQ(render(true, 0, 0.15f), occluded_ambient);
}
} // namespace
