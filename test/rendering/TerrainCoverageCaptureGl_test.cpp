#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/Shader.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

#include "../support/UniqueTempPath.h"

#include <gtest/gtest.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;

constexpr int kExtent = 9;
constexpr std::size_t kPixels = kExtent * kExtent;
constexpr int kCoveredX = 2;
constexpr int kCoveredY = 1;
constexpr std::size_t kCovered = kCoveredY * kExtent + kCoveredX;
constexpr std::size_t kCoveredTopDown = (kExtent - 1 - kCoveredY) * kExtent + kCoveredX;

// Every state changed by ReadbackState and FogReadback, plus draw/read bindings
// and the caller's current read-buffer selection. Texture bindings are per unit.
constexpr std::array<GLenum, 14> kIntegerState{GL_READ_FRAMEBUFFER_BINDING,
                                               GL_DRAW_FRAMEBUFFER_BINDING,
                                               GL_READ_BUFFER,
                                               GL_PIXEL_PACK_BUFFER_BINDING,
                                               GL_PACK_ALIGNMENT,
                                               GL_PACK_ROW_LENGTH,
                                               GL_PACK_SKIP_ROWS,
                                               GL_PACK_SKIP_PIXELS,
                                               GL_PACK_SWAP_BYTES,
                                               GL_PACK_IMAGE_HEIGHT,
                                               GL_PACK_SKIP_IMAGES,
                                               GL_ACTIVE_TEXTURE,
                                               GL_CURRENT_PROGRAM,
                                               GL_VERTEX_ARRAY_BINDING};
constexpr std::array<GLenum, 4> kCapabilities{
    GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD};

struct CallerState {
    std::array<GLint, kIntegerState.size()> integers{};
    std::array<GLint, 4> viewport{}, texture_2d{}, texture_3d{};
    std::array<GLint, 2> polygon_mode{};
    std::array<GLboolean, 4> color_mask{};
    std::array<GLboolean, kCapabilities.size()> enabled{};
    GLboolean blend = GL_FALSE;
    GLint gbuffer_read = 0, default_read = 0;

    explicit CallerState(GLuint gbuffer) {
        for (std::size_t index = 0; index < kIntegerState.size(); ++index)
            glGetIntegerv(kIntegerState[index], &integers[index]);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetIntegerv(GL_POLYGON_MODE, polygon_mode.data());
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, color_mask.data());
        blend = glIsEnabledi(GL_BLEND, 0);
        for (std::size_t index = 0; index < kCapabilities.size(); ++index)
            enabled[index] = glIsEnabled(kCapabilities[index]);
        GLint active = 0, read_fbo = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        for (std::size_t unit = 0; unit < texture_2d.size(); ++unit) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d[unit]);
            glGetIntegerv(GL_TEXTURE_BINDING_3D, &texture_3d[unit]);
        }
        glActiveTexture(static_cast<GLenum>(active));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer);
        glGetIntegerv(GL_READ_BUFFER, &gbuffer_read);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glGetIntegerv(GL_READ_BUFFER, &default_read);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
    }

    void ExpectRestored(GLuint gbuffer) const {
        const CallerState actual(gbuffer);
        for (std::size_t index = 0; index < integers.size(); ++index) {
            SCOPED_TRACE(kIntegerState[index]);
            EXPECT_EQ(actual.integers[index], integers[index]);
        }
        EXPECT_EQ(actual.viewport, viewport);
        EXPECT_EQ(actual.polygon_mode, polygon_mode);
        EXPECT_EQ(actual.color_mask, color_mask);
        EXPECT_EQ(actual.blend, blend);
        EXPECT_EQ(actual.enabled, enabled);
        EXPECT_EQ(actual.texture_2d, texture_2d);
        EXPECT_EQ(actual.texture_3d, texture_3d);
        EXPECT_EQ(actual.gbuffer_read, gbuffer_read);
        EXPECT_EQ(actual.default_read, default_read);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
};

class TerrainCoverageCaptureGl : public ::testing::Test {
protected:
    void SetUp() override {
        initialized_ = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized_);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window_ = glfwCreateWindow(kExtent, kExtent, "terrain coverage", nullptr, nullptr);
        ASSERT_NE(window_, nullptr);
        glfwMakeContextCurrent(window_);
        loaded_ = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
        ASSERT_TRUE(loaded_);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    }

    void TearDown() override {
        if (loaded_) {
            glUseProgram(0);
            program_.reset();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindVertexArray(0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glDeleteFramebuffers(static_cast<GLsizei>(fbos_.size()), fbos_.data());
            glDeleteTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
            glDeleteVertexArrays(1, &vao_);
            glDeleteBuffers(1, &pbo_);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }
        if (window_)
            glfwDestroyWindow(window_);
        if (initialized_)
            glfwTerminate();
    }

    void Prepare(RenderPipeline& pipeline, Systems::SHIELD_WorldSystem& world) {
        luminumbra::core::ResolvedGraphics graphics;
        graphics.values.shadow_resolution = 1024;
        graphics.values.volumetric_quality = 0;
        graphics.values.grass_enabled = false;
        ASSERT_TRUE(pipeline.apply_graphics(graphics));
        ASSERT_TRUE(pipeline.startup(kExtent, kExtent, LUMINUMBRA_SOURCE_ROOT));
        pipeline.set_render_scale(1.0f);
        pipeline.set_taau_enabled(false);
        pipeline.set_far_lod_enabled(false);
        pipeline.set_volumetric_quality(0);
        pipeline.set_terrain_coverage_diagnostics_enabled(true);
        pipeline.render_frame(registry_, world, camera_, 0.0f, false, UploadPresentation::Settled);
        ASSERT_TRUE(pipeline.render_uploads_settled());
        ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        // No terrain generation or upload is needed: the public attachment IDs
        // accept a synthetic scene with one asymmetric covered texel. All work
        // and storage for these planes are bounded by 9x9 pixels and four units.
        // Auxiliary shadow reads use the minimum supported 1024x1024x4 atlases.
        const auto& gbuffer = pipeline.gbuffer();
        const float clear_depth = 0, covered_depth = 0.5f;
        const std::array<float, 3> clear_position{}, position{1, 2, -8};
        const std::array<unsigned char, 4> clear_normal{128, 128, 0, 255};
        const std::array<unsigned char, 4> normal{128, 128, 0, 42};
        const std::array<unsigned char, 4> clear_albedo{}, albedo{32, 64, 128, 255};
        glClearTexImage(gbuffer.depth_texture, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &clear_depth);
        glClearTexImage(gbuffer.position_texture, 0, GL_RGB, GL_FLOAT, clear_position.data());
        glClearTexImage(gbuffer.normal_texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, clear_normal.data());
        glClearTexImage(gbuffer.albedo_texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, clear_albedo.data());
        glTextureSubImage2D(gbuffer.depth_texture,
                            0,
                            kCoveredX,
                            kCoveredY,
                            1,
                            1,
                            GL_DEPTH_COMPONENT,
                            GL_FLOAT,
                            &covered_depth);
        glTextureSubImage2D(gbuffer.position_texture,
                            0,
                            kCoveredX,
                            kCoveredY,
                            1,
                            1,
                            GL_RGB,
                            GL_FLOAT,
                            position.data());
        glTextureSubImage2D(gbuffer.normal_texture,
                            0,
                            kCoveredX,
                            kCoveredY,
                            1,
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            normal.data());
        glTextureSubImage2D(gbuffer.albedo_texture,
                            0,
                            kCoveredX,
                            kCoveredY,
                            1,
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            albedo.data());
        ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    void SeedCaller(GLuint gbuffer, bool enabled) {
        const std::filesystem::path root(LUMINUMBRA_SOURCE_ROOT);
        program_ = std::make_unique<Shader>((root / "res/shaders/ssao.vert").string().c_str(),
                                            (root / "res/shaders/ssao_blur.frag").string().c_str());
        ASSERT_TRUE(program_->IsValid());
        program_->use();
        glCreateVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        glCreateFramebuffers(static_cast<GLsizei>(fbos_.size()), fbos_.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT3);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_FRONT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos_[0]);
        glReadBuffer(GL_COLOR_ATTACHMENT4);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos_[1]);
        glCreateBuffers(1, &pbo_);
        glNamedBufferData(pbo_, 64, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
        glPixelStorei(GL_PACK_ALIGNMENT, 8);
        glPixelStorei(GL_PACK_ROW_LENGTH, 13);
        glPixelStorei(GL_PACK_SKIP_ROWS, 2);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, 15);
        glPixelStorei(GL_PACK_SKIP_IMAGES, 1);
        for (std::size_t unit = 0; unit < 4; ++unit) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glCreateTextures(GL_TEXTURE_2D, 1, &textures_[unit * 2]);
            glCreateTextures(GL_TEXTURE_3D, 1, &textures_[unit * 2 + 1]);
            glBindTexture(GL_TEXTURE_2D, textures_[unit * 2]);
            glBindTexture(GL_TEXTURE_3D, textures_[unit * 2 + 1]);
        }
        glActiveTexture(GL_TEXTURE7);
        glViewport(1, 2, 3, 4);
        glColorMaski(0, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glScissor(0, 0, 1, 1);
        for (const auto capability : kCapabilities) {
            if (enabled)
                glEnable(capability);
            else
                glDisable(capability);
        }
        if (enabled)
            glEnablei(GL_BLEND, 0);
        else
            glDisablei(GL_BLEND, 0);
        ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    template<typename Value, std::size_t Count>
    static void ReadPlane(const std::filesystem::path& path,
                          const std::string& header,
                          std::array<Value, Count>& values) {
        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.is_open()) << path;
        // Fixed-size buffers: reject malformed/truncated headers or payloads
        // without allocating from file-supplied dimensions.
        std::array<char, 32> actual_header{};
        ASSERT_LE(header.size(), actual_header.size());
        input.read(actual_header.data(), static_cast<std::streamsize>(header.size()));
        ASSERT_EQ(input.gcount(), static_cast<std::streamsize>(header.size()));
        ASSERT_EQ(std::string(actual_header.data(), header.size()), header);
        input.read(reinterpret_cast<char*>(values.data()),
                   static_cast<std::streamsize>(sizeof(values)));
        ASSERT_EQ(input.gcount(), static_cast<std::streamsize>(sizeof(values)));
        ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
    }

    static void ExpectPlanes(const std::filesystem::path& directory) {
        const std::string scale = std::endian::native == std::endian::little ? "-1.0\n" : "1.0\n";
        std::array<float, kPixels> depth{}, opacity{};
        std::array<float, kPixels * 3> position{}, normal{}, albedo{};
        std::array<unsigned char, kPixels> material{};
        ASSERT_NO_FATAL_FAILURE(ReadPlane(directory / "depth.pfm", "Pf\n9 9\n" + scale, depth));
        ASSERT_NO_FATAL_FAILURE(
            ReadPlane(directory / "position.pfm", "PF\n9 9\n" + scale, position));
        ASSERT_NO_FATAL_FAILURE(ReadPlane(directory / "normal.pfm", "PF\n9 9\n" + scale, normal));
        ASSERT_NO_FATAL_FAILURE(ReadPlane(directory / "albedo.pfm", "PF\n9 9\n" + scale, albedo));
        ASSERT_NO_FATAL_FAILURE(ReadPlane(directory / "material.pgm", "P5\n9 9\n255\n", material));
        ASSERT_NO_FATAL_FAILURE(
            ReadPlane(directory / "aerial-opacity-0.pfm", "Pf\n9 9\n" + scale, opacity));
        for (std::size_t pixel = 0; pixel < kPixels; ++pixel) {
            SCOPED_TRACE(pixel);
            EXPECT_FLOAT_EQ(depth[pixel], pixel == kCovered ? 0.5f : 0.0f);
            EXPECT_EQ(material[pixel], pixel == kCoveredTopDown ? 42 : 255);
            if (pixel != kCovered) {
                EXPECT_FLOAT_EQ(opacity[pixel], 0.0f);
            }
        }
        // This also proves the fog replay drew despite hostile caller state.
        EXPECT_GT(opacity[kCovered], 0.0f);
        EXPECT_LT(opacity[kCovered], 1.0f);
        const std::array<float, 3> expected_position{1, 2, -8};
        const std::array<float, 3> expected_albedo{32.0f / 255, 64.0f / 255, 128.0f / 255};
        const float normal_length = std::sqrt(253.0f * 253.0f + 2.0f);
        const std::array<float, 3> expected_normal{
            1 / normal_length, 1 / normal_length, 253 / normal_length};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            SCOPED_TRACE(channel);
            EXPECT_FLOAT_EQ(position[kCovered * 3 + channel], expected_position[channel]);
            EXPECT_NEAR(albedo[kCovered * 3 + channel], expected_albedo[channel], 1e-6f);
            EXPECT_NEAR(normal[kCovered * 3 + channel], expected_normal[channel], 1e-6f);
            EXPECT_FLOAT_EQ(position[channel], 0.0f);
            EXPECT_FLOAT_EQ(albedo[channel], 0.0f);
        }
        std::ifstream manifest_file(directory / "manifest.json");
        ASSERT_TRUE(manifest_file.is_open());
        const auto manifest = nlohmann::json::parse(manifest_file);
        EXPECT_EQ(manifest.at("complete"), true);
        EXPECT_EQ(manifest.at("gbuffer_dimensions"), (nlohmann::json{kExtent, kExtent}));
        EXPECT_EQ(manifest.at("lighting_sources").at("aerial-opacity").at("status"), "captured");
        ASSERT_NO_FATAL_FAILURE(ExpectLightingEvidence(directory, manifest));
    }

    static void ExpectLightingEvidence(const std::filesystem::path& directory,
                                       const nlohmann::json& manifest) {
        EXPECT_EQ(manifest.at("schema"), "luminumbra.terrain_coverage_capture.v1");
        EXPECT_EQ(manifest.at("lighting_source_scope"),
                  "raw_pass_inputs_and_composites_not_additive_terms");
        EXPECT_EQ(manifest.at("unavailable_terms"),
                  (nlohmann::json{"canopy_visibility_no_standalone_target"}));
        struct ExpectedSource {
            const char* name;
            int channels;
        };
        constexpr std::array<ExpectedSource, 9> expected{{{"lit-scene", 3},
                                                          {"opaque-scene", 3},
                                                          {"sky-view", 3},
                                                          {"transmittance", 3},
                                                          {"ambient-ssao", 1},
                                                          {"shadow-depth", 1},
                                                          {"shadow-tint", 3},
                                                          {"aerial-inscatter", 3},
                                                          {"aerial-opacity", 1}}};
        const auto& sources = manifest.at("lighting_sources");
        // Check every advertised layer without allocating image-sized buffers.
        // Capture admits at most 16M pixels and 64 layers per source.
        constexpr std::size_t max_pixels = 16u * 1024u * 1024u;
        for (const auto& item : expected) {
            SCOPED_TRACE(item.name);
            const auto& source = sources.at(item.name);
            ASSERT_EQ(source.at("status"), "captured");
            EXPECT_NE(source.at("texture").get<GLuint>(), 0u);
            EXPECT_EQ(source.at("channels"), item.channels);
            const auto dimensions = source.at("dimensions").get<std::array<std::size_t, 3>>();
            ASSERT_GT(dimensions[0], 0u);
            ASSERT_GT(dimensions[1], 0u);
            ASSERT_LE(dimensions[0], max_pixels / dimensions[1]);
            ASSERT_GT(dimensions[2], 0u);
            ASSERT_LE(dimensions[2], 64u);
            const auto& files = source.at("files");
            ASSERT_EQ(files.size(), dimensions[2]);
            const std::string header =
                std::string(item.channels == 3 ? "PF\n" : "Pf\n") + std::to_string(dimensions[0]) +
                " " + std::to_string(dimensions[1]) + "\n" +
                (std::endian::native == std::endian::little ? "-1.0\n" : "1.0\n");
            const auto bytes = dimensions[0] * dimensions[1] *
                               static_cast<std::size_t>(item.channels) * sizeof(float);
            for (std::size_t layer = 0; layer < dimensions[2]; ++layer) {
                const std::string filename =
                    std::string(item.name) + "-" + std::to_string(layer) + ".pfm";
                EXPECT_EQ(files.at(layer), filename);
                std::ifstream input(directory / filename, std::ios::binary | std::ios::ate);
                ASSERT_TRUE(input.is_open()) << filename;
                EXPECT_EQ(input.tellg(), static_cast<std::streamoff>(header.size() + bytes));
                input.seekg(0);
                std::array<char, 64> actual_header{};
                ASSERT_LE(header.size(), actual_header.size());
                input.read(actual_header.data(), static_cast<std::streamsize>(header.size()));
                ASSERT_EQ(input.gcount(), static_cast<std::streamsize>(header.size()));
                EXPECT_EQ(std::string(actual_header.data(), header.size()), header);
            }
        }
        // Quality zero never allocates the optional integrated volume.
        EXPECT_EQ(manifest.at("volumetric_quality"), 0);
        for (const auto* name : {"fog-inscatter", "fog-transmittance"})
            EXPECT_EQ(sources.at(name), (nlohmann::json{{"status", "unavailable"}}));
        const auto& lighting = manifest.at("lighting_uniforms");
        const auto& fog = manifest.at("fog_uniforms");
        EXPECT_EQ(lighting.at("u_skyAmbientColor"), manifest.at("ambient_color"));
        EXPECT_EQ(lighting.at("u_viewPos"), (nlohmann::json{8.0f, 80.0f, 8.0f}));
        EXPECT_EQ(fog.at("u_viewPos"), lighting.at("u_viewPos"));
        EXPECT_EQ(fog.at("u_volumetricMode"), 0);
        EXPECT_GT(fog.at("u_aerialDensity").at(0).get<float>(), 0.0f);
    }

private:
    GLFWwindow* window_ = nullptr;
    bool initialized_ = false, loaded_ = false;
    std::unique_ptr<Shader> program_;
    std::array<GLuint, 2> fbos_{};
    std::array<GLuint, 8> textures_{};
    GLuint vao_ = 0, pbo_ = 0;
    entt::registry registry_;
    Camera camera_{{8, 80, 8}, {0, 1, 0}, -90, 0};
};

TEST_F(TerrainCoverageCaptureGl, SyntheticPlanesAndCallerStateSurviveReadback) {
    RenderPipeline pipeline;
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    ASSERT_NO_FATAL_FAILURE(Prepare(pipeline, world));
    ASSERT_NO_FATAL_FAILURE(SeedCaller(pipeline.gbuffer().fbo_id, true));
    const CallerState caller(pipeline.gbuffer().fbo_id);
    const TestSupport::ScopedTempFile scratch("coverage-", ".json");
    const auto directory = scratch.Path().parent_path() / "capture";
    const bool captured = pipeline.capture_terrain_coverage(directory);
    caller.ExpectRestored(pipeline.gbuffer().fbo_id);
    ASSERT_TRUE(captured);
    ASSERT_NO_FATAL_FAILURE(ExpectPlanes(directory));
}

TEST_F(TerrainCoverageCaptureGl, IncompleteFramebufferRefusesAndRestoresReadbackState) {
    RenderPipeline pipeline;
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    ASSERT_NO_FATAL_FAILURE(Prepare(pipeline, world));
    const GLuint gbuffer = pipeline.gbuffer().fbo_id;
    // An incomplete source fails after final-color readback has changed pack
    // state, so this exercises the guard's early-return destructor as well.
    for (GLuint attachment = 0; attachment < 6; ++attachment)
        glNamedFramebufferTexture(gbuffer, GL_COLOR_ATTACHMENT0 + attachment, 0, 0);
    glNamedFramebufferTexture(gbuffer, GL_DEPTH_ATTACHMENT, 0, 0);
    ASSERT_NE(glCheckNamedFramebufferStatus(gbuffer, GL_READ_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    ASSERT_NO_FATAL_FAILURE(SeedCaller(gbuffer, false));
    const CallerState caller(gbuffer);
    const TestSupport::ScopedTempFile scratch("coverage-refusal-", ".json");
    const auto directory = scratch.Path().parent_path() / "capture";
    EXPECT_FALSE(pipeline.capture_terrain_coverage(directory));
    caller.ExpectRestored(gbuffer);
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "color.ppm"));
    EXPECT_FALSE(std::filesystem::exists(directory / "manifest.json"));
}
} // namespace
