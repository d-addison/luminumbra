#include "core/Environment.h"
#include "rendering/GBufferTargets.h"
#include "rendering/RenderResourceRegistry.h"
#include "rendering/passes/LightingPass.h"
#include <algorithm>
#include <cctype>
#include <glad/glad.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <gtest/gtest.h>
#include <luminumbra/rendering/StaticRenderer.h>
#include <numeric>

namespace {
using namespace Luminumbra::Rendering;
std::shared_ptr<StaticMesh> Quad(float uv_x = .5f) {
    auto mesh = std::make_shared<StaticMesh>();
    mesh->identity = "test-quad";
    mesh->vertices = {{{-1, -1, -3}, {0, 0, 1}, {uv_x, 0}},
                      {{1, -1, -3}, {0, 0, 1}, {uv_x, 0}},
                      {{1, 1, -3}, {0, 0, 1}, {uv_x, 1}},
                      {{-1, 1, -3}, {0, 0, 1}, {uv_x, 1}}};
    mesh->indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}
std::shared_ptr<StaticTexture> Texture(std::string id,
                                       std::vector<std::uint8_t> rgba,
                                       StaticEncoding encoding = StaticEncoding::Linear) {
    auto texture = std::make_shared<StaticTexture>();
    texture->identity = std::move(id);
    texture->encoding = encoding;
    const auto width = static_cast<std::uint32_t>(rgba.size() / 4);
    texture->mips.push_back({width, 1, std::move(rgba)});
    if (width == 2)
        texture->mips.push_back({1, 1, {128, 128, 128, 255}});
    return texture;
}
std::shared_ptr<StaticMaterial> Material() {
    auto material = std::make_shared<StaticMaterial>();
    material->identity = "test-material";
    material->metallic = 0;
    material->roughness = .8;
    for (auto& binding : material->textures) {
        binding.sampler.min_filter = 9728;
        binding.sampler.mag_filter = 9728;
    }
    return material;
}
std::shared_ptr<StaticDrawSnapshot> Scene(std::shared_ptr<StaticMaterial> material = Material(),
                                          std::shared_ptr<StaticMesh> mesh = Quad()) {
    auto scene = std::make_shared<StaticDrawSnapshot>();
    scene->revision = 1;
    StaticDraw draw;
    draw.key = {"instance", "node", 0};
    draw.mesh = std::move(mesh);
    draw.material = std::move(material);
    draw.normal = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    scene->draws.push_back(std::move(draw));
    return scene;
}
RenderView View() {
    RenderViewDescription d;
    d.width = 160;
    d.height = 120;
    d.revision = 1;
    d.near_plane = .1;
    d.far_plane = 100;
    const auto projection = glm::perspectiveRH_ZO(glm::radians(45.0), 4.0 / 3.0, 100.0, .1);
    std::copy_n(glm::value_ptr(projection), 16, d.projection.begin());
    return RenderView::Validate(d);
}
RenderView OrthographicView(double half_height = 2, double center_x = 0) {
    auto d = View().description();
    const double half_width = half_height * d.width / d.height;
    const auto projection = glm::orthoRH_ZO(center_x - half_width,
                                            center_x + half_width,
                                            -half_height,
                                            half_height,
                                            d.far_plane,
                                            d.near_plane);
    std::copy_n(glm::value_ptr(projection), 16, d.projection.begin());
    return RenderView::Validate(d);
}
bool RendererMatches(bool native,
                     const std::string& expected_vendor,
                     const std::string& expected_renderer,
                     const std::string& vendor,
                     const std::string& renderer) {
    if (!native)
        return renderer.find("llvmpipe") != std::string::npos;
    if (expected_vendor.empty() || expected_renderer.empty() || vendor != expected_vendor ||
        renderer != expected_renderer)
        return false;
    std::string lower = vendor + " " + renderer;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto software : {"llvmpipe",
                                "softpipe",
                                "swrast",
                                "swiftshader",
                                "lavapipe",
                                "software",
                                "gdi generic",
                                "microsoft basic"})
        if (lower.find(software) != std::string::npos)
            return false;
    return true;
}
size_t Coverage(const StaticFrame& frame) {
    return std::count(frame.coverage8.begin(), frame.coverage8.end(), 1);
}
std::uint64_t Sum(const StaticFrame& frame, size_t channel) {
    std::uint64_t result = 0;
    for (size_t i = 0; i < frame.coverage8.size(); ++i)
        if (frame.coverage8[i])
            result += frame.rgba8[i * 4 + channel];
    return result;
}
class StaticPreviewRender : public ::testing::Test {
protected:
    std::unique_ptr<StaticRenderer> renderer;
    void SetUp() override {
        const auto expected_vendor =
            Luminumbra::Core::ReadEnvironment("LUMINUMBRA_PREVIEW_EXPECT_NATIVE_VENDOR");
        const auto expected_renderer =
            Luminumbra::Core::ReadEnvironment("LUMINUMBRA_PREVIEW_EXPECT_NATIVE_RENDERER");
        const bool native = expected_vendor.has_value() || expected_renderer.has_value();
        if (native) {
            ASSERT_TRUE(expected_vendor && !expected_vendor->empty());
            ASSERT_TRUE(expected_renderer && !expected_renderer->empty());
        }
        renderer = std::make_unique<StaticRenderer>(STATIC_PREVIEW_RESOURCE_ROOT, !native);
        const auto* vendor = glGetString(GL_VENDOR);
        const auto* actual = glGetString(GL_RENDERER);
        ASSERT_NE(vendor, nullptr);
        ASSERT_NE(actual, nullptr);
        const std::string vendor_text(reinterpret_cast<const char*>(vendor));
        const std::string renderer_text(reinterpret_cast<const char*>(actual));
        RecordProperty("renderer_profile", native ? "native-exact-pins" : "forced-llvmpipe");
        RecordProperty("gpu_vendor", vendor_text);
        RecordProperty("gpu_renderer", renderer_text);
        ASSERT_TRUE(RendererMatches(native,
                                    expected_vendor.value_or(""),
                                    expected_renderer.value_or(""),
                                    vendor_text,
                                    renderer_text))
            << "Unexpected GPU identity: " << vendor_text << " / " << renderer_text;
    }
};
TEST(StaticPreviewRendererProfile, NativeRequiresExactPinsAndRefusesSoftwareIdentities) {
    const std::string vendor = "NVIDIA Corporation", gpu = "NVIDIA GeForce RTX 5090/PCIe/SSE2";
    EXPECT_TRUE(RendererMatches(true, vendor, gpu, vendor, gpu));
    EXPECT_FALSE(RendererMatches(true, "", gpu, vendor, gpu));
    EXPECT_FALSE(RendererMatches(true, vendor, "", vendor, gpu));
    EXPECT_FALSE(RendererMatches(true, vendor, gpu, "unknown vendor", gpu));
    EXPECT_FALSE(RendererMatches(true, vendor, gpu, vendor, "unknown renderer"));
    for (const std::string software : {"llvmpipe (LLVM)",
                                       "softpipe",
                                       "SwiftShader Device",
                                       "GDI Generic",
                                       "Microsoft Basic Render Driver"})
        EXPECT_FALSE(RendererMatches(true, "pinned vendor", software, "pinned vendor", software));
    EXPECT_TRUE(RendererMatches(false, "", "", "Mesa", "llvmpipe (LLVM)"));
    EXPECT_FALSE(RendererMatches(false, "", "", vendor, gpu));
}
TEST_F(StaticPreviewRender, ProductionBackgroundDepthCoverageAndShaderBindingsAgree) {
    const auto frame = renderer->Render(View(), Scene());
    EXPECT_GT(Coverage(frame), 1000u);
    EXPECT_LT(Coverage(frame), frame.coverage8.size());
    EXPECT_EQ(frame.draw_count, 1u);
    EXPECT_EQ(frame.index_count, 6u);
    for (size_t i = 0; i < frame.coverage8.size(); ++i) {
        EXPECT_EQ(frame.coverage8[i] != 0, frame.depth32f[i] > 0);
        EXPECT_EQ(frame.rgba8[i * 4 + 3], frame.coverage8[i] ? 255 : 0);
    }
}
TEST_F(StaticPreviewRender, OrthographicDepthKeepsApparentSizeAndNearestSurfaceWins) {
    const auto view = OrthographicView();
    const auto scene = Scene();
    const auto near = renderer->Render(view, scene);
    ASSERT_GT(Coverage(near), 1000u);
    auto moved = std::make_shared<StaticDrawSnapshot>(*scene);
    moved->revision = 2;
    moved->draws[0].model[14] = -27;
    const auto far = renderer->Render(view, moved);
    EXPECT_EQ(near.coverage8, far.coverage8);
    EXPECT_EQ(near.actual_projection, view.projection());
    for (size_t i = 0; i < near.coverage8.size(); ++i) {
        if (near.coverage8[i]) {
            EXPECT_NEAR(near.depth32f[i], (100.0 - 3) / 99.9, 2e-7);
            EXPECT_NEAR(far.depth32f[i], (100.0 - 30) / 99.9, 2e-7);
            EXPECT_GT(near.depth32f[i], far.depth32f[i]);
        } else {
            EXPECT_EQ(near.depth32f[i], 0);
            EXPECT_EQ(far.depth32f[i], 0);
        }
        EXPECT_EQ(far.rgba8[i * 4 + 3], far.coverage8[i] ? 255 : 0);
    }
    auto combined = std::make_shared<StaticDrawSnapshot>(*scene);
    auto behind = moved->draws[0];
    behind.key.node_id = "behind";
    auto emission = Material();
    emission->emissive = {1, 0, 0};
    behind.material = emission;
    combined->draws.push_back(behind);
    combined->revision = 3;
    const auto first = renderer->Render(view, combined);
    EXPECT_EQ(first.rgba8, near.rgba8);
    EXPECT_EQ(first.depth32f, near.depth32f);
    combined = std::make_shared<StaticDrawSnapshot>(*combined);
    std::reverse(combined->draws.begin(), combined->draws.end());
    combined->revision = 4;
    EXPECT_EQ(renderer->Render(view, combined).rgba8, near.rgba8);
    moved = std::make_shared<StaticDrawSnapshot>(*moved);
    moved->draws[0].model[14] = -100;
    moved->revision = 5;
    EXPECT_EQ(Coverage(renderer->Render(view, moved)), 0u);
}
TEST_F(StaticPreviewRender, OrthographicPanZoomAndResizeChangeActualCoverage) {
    const auto scene = Scene();
    const auto baseline = renderer->Render(OrthographicView(), scene);
    const auto shifted_view = OrthographicView(2, .5);
    const auto shifted = renderer->Render(shifted_view, scene);
    EXPECT_EQ(Coverage(shifted), Coverage(baseline));
    EXPECT_NE(shifted.coverage8, baseline.coverage8);
    EXPECT_EQ(shifted.actual_projection, shifted_view.projection());
    const auto zoomed = renderer->Render(OrthographicView(1), scene);
    EXPECT_EQ(Coverage(zoomed), 4 * Coverage(baseline));
    auto resized = OrthographicView().description();
    resized.width *= 2;
    resized.height *= 2;
    const auto larger = renderer->Render(RenderView::Validate(resized), scene);
    EXPECT_EQ(Coverage(larger), 4 * Coverage(baseline));
    EXPECT_EQ(larger.coverage8.size(), 4 * baseline.coverage8.size());
}
TEST_F(StaticPreviewRender, OrthographicSpecularUsesParallelRaysAndPerspectiveRestoresPointEye) {
    auto material = Material();
    material->metallic = 1;
    material->roughness = .25;
    const auto scene = Scene(material);
    const auto before = renderer->Render(View(), scene);
    const auto orthographic = renderer->Render(OrthographicView(), scene);
    const auto variation = [](const StaticFrame& frame) {
        unsigned low = 255, high = 0;
        for (size_t i = 0; i < frame.coverage8.size(); ++i) {
            if (!frame.coverage8[i])
                continue;
            low = std::min(low, unsigned(frame.rgba8[i * 4]));
            high = std::max(high, unsigned(frame.rgba8[i * 4]));
        }
        return high - low;
    };
    ASSERT_GT(Coverage(orthographic), 1000u);
    EXPECT_LE(variation(orthographic), 1u);
    EXPECT_GT(variation(before), 5u); // Negative control for the previous point-eye lighting.
    EXPECT_EQ(renderer->Render(View(), scene).rgba8, before.rgba8);
}
TEST_F(StaticPreviewRender, DistantAuthoredPositionsRemainFiniteBeyondBinary16Range) {
    const auto near = renderer->Render(View(), Scene());
    auto mesh = Quad();
    mesh->identity = "distant-quad";
    for (auto& vertex : mesh->vertices)
        for (auto& component : vertex.position)
            component *= 100000.0f / 3.0f;
    auto description = View().description();
    description.far_plane = 1e6;
    const auto projection = glm::perspectiveRH_ZO(glm::radians(45.0), 4.0 / 3.0, 1e6, .1);
    std::copy_n(glm::value_ptr(projection), 16, description.projection.begin());
    const auto distant =
        renderer->Render(RenderView::Validate(description), Scene(Material(), mesh));
    EXPECT_EQ(near.coverage8, distant.coverage8);
    const size_t center = (description.height / 2 * description.width + description.width / 2) * 4;
    ASSERT_EQ(distant.rgba8[center + 3], 255);
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_GT(distant.rgba8[center + channel], 50);
        EXPECT_NEAR(distant.rgba8[center + channel], near.rgba8[center + channel], 2);
    }
}
TEST_F(StaticPreviewRender, TransformUpdatesReuseImmutableMeshAndChangeActualPixels) {
    const auto scene = Scene();
    const auto before = renderer->Render(View(), scene);
    auto changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 2;
    changed->draws[0].model[12] = .4;
    const auto after = renderer->Render(View(), changed);
    EXPECT_NE(before.rgba8, after.rgba8);
    EXPECT_EQ(after.uploaded_meshes, 0u);
    EXPECT_EQ(after.uploaded_textures, 0u);
    EXPECT_EQ(after.updated_instances, 1u);
    EXPECT_EQ(after.scene_revision, 2u);
    EXPECT_EQ(after.sequence, 2u);
    const auto same = renderer->Render(View(), changed);
    EXPECT_EQ(after.rgba8, same.rgba8);
    EXPECT_EQ(same.updated_instances, 0u);
}
TEST_F(StaticPreviewRender, TrueSamplerWrapChangesSamplingWithoutChangingRawGeometry) {
    auto material = Material();
    material->textures[0].texture =
        Texture("wrap", {255, 0, 0, 255, 0, 0, 255, 255}, StaticEncoding::Srgb);
    const auto scene = Scene(material, Quad(1.25f));
    const auto repeat = renderer->Render(View(), scene);
    auto clamped = std::make_shared<StaticMaterial>(*material);
    clamped->textures[0].sampler.wrap_s = 33071;
    auto changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 2;
    changed->draws[0].material = clamped;
    const auto clamp = renderer->Render(View(), changed);
    EXPECT_EQ(repeat.depth32f, clamp.depth32f);
    EXPECT_GT(Sum(repeat, 0), Sum(repeat, 2));
    EXPECT_GT(Sum(clamp, 2), Sum(clamp, 0));
    EXPECT_EQ(clamp.uploaded_meshes, 0u);
    EXPECT_EQ(clamp.uploaded_textures, 0u);
}
TEST_F(StaticPreviewRender, MaskUsesNumericCutoffAndOpaqueIgnoresTextureAlpha) {
    auto material = Material();
    material->alpha_mode = StaticAlphaMode::Mask;
    material->textures[0].texture =
        Texture("alpha", {255, 255, 255, 0, 255, 255, 255, 255}, StaticEncoding::Srgb);
    auto mesh = Quad();
    mesh->vertices[0].uv[0] = 0;
    mesh->vertices[3].uv[0] = 0;
    mesh->vertices[1].uv[0] = 1;
    mesh->vertices[2].uv[0] = 1;
    const auto scene = Scene(material, mesh);
    const auto mask = renderer->Render(View(), scene);
    auto opaque = std::make_shared<StaticMaterial>(*material);
    opaque->alpha_mode = StaticAlphaMode::Opaque;
    auto changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 2;
    changed->draws[0].material = opaque;
    const auto full = renderer->Render(View(), changed);
    EXPECT_GT(Coverage(full), Coverage(mask) * 18 / 10);
    auto zero = std::make_shared<StaticMaterial>(*material);
    zero->alpha_cutoff = 0;
    changed = std::make_shared<StaticDrawSnapshot>(*changed);
    changed->revision = 3;
    changed->draws[0].material = zero;
    EXPECT_EQ(Coverage(renderer->Render(View(), changed)), Coverage(full));
}
TEST_F(StaticPreviewRender, NormalBasisUsesRawUvAndNormalScaleChangesActualLighting) {
    auto material = Material();
    material->textures[2].texture = Texture("normal", {255, 128, 255, 255});
    auto mesh = Quad();
    mesh->vertices[0].uv[0] = 0;
    mesh->vertices[3].uv[0] = 0;
    mesh->vertices[1].uv[0] = 1;
    mesh->vertices[2].uv[0] = 1;
    auto scene = Scene(material, mesh);
    const auto original = renderer->Render(View(), scene);
    auto rotated = std::make_shared<StaticMaterial>(*material);
    rotated->textures[2].rotation = 1.5707963267948966;
    auto changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 2;
    changed->draws[0].material = rotated;
    const auto rotation = renderer->Render(View(), changed);
    EXPECT_EQ(original.rgba8, rotation.rgba8);
    auto flat = std::make_shared<StaticMaterial>(*rotated);
    flat->normal_scale = 0;
    changed = std::make_shared<StaticDrawSnapshot>(*changed);
    changed->revision = 3;
    changed->draws[0].material = flat;
    EXPECT_NE(original.rgba8, renderer->Render(View(), changed).rgba8);
}
TEST_F(StaticPreviewRender, FullAffineMirrorPreservesWindingAndShearRemainsVisible) {
    auto scene = Scene();
    const auto original = renderer->Render(View(), scene);
    auto changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 2;
    changed->draws[0].model[0] = -1;
    changed->draws[0].normal[0] = -1;
    changed->draws[0].reverse_front_face = true;
    const auto mirror = renderer->Render(View(), changed);
    EXPECT_EQ(Coverage(original), Coverage(mirror));
    changed = std::make_shared<StaticDrawSnapshot>(*changed);
    changed->revision = 3;
    changed->draws[0].model[4] = .3;
    const auto normal =
        glm::transpose(glm::inverse(glm::dmat3(glm::make_mat4(changed->draws[0].model.data()))));
    std::copy_n(glm::value_ptr(normal), 9, changed->draws[0].normal.begin());
    const auto shear = renderer->Render(View(), changed);
    EXPECT_GT(Coverage(shear), 1000u);
    EXPECT_NE(shear.coverage8, mirror.coverage8);
}
TEST_F(StaticPreviewRender, AuthoredRgbEmissionAndMaterialOcclusionReachProductionLighting) {
    auto material = Material();
    material->base_color = {0, 0, 0, 1};
    auto scene = Scene(material);
    const auto dark = renderer->Render(View(), scene);
    auto emission = std::make_shared<StaticMaterial>(*material);
    emission->emissive = {.2, 0, 0};
    auto changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 2;
    changed->draws[0].material = emission;
    const auto glow = renderer->Render(View(), changed);
    EXPECT_GT(Sum(glow, 0), Sum(dark, 0));
    EXPECT_EQ(glow.depth32f, dark.depth32f);
    auto ao = Material();
    ao->textures[3].texture = Texture("ao", {0, 0, 0, 255});
    changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 3;
    changed->draws[0].material = ao;
    const auto occluded = renderer->Render(View(), changed);
    auto none = std::make_shared<StaticMaterial>(*ao);
    none->occlusion_strength = 0;
    changed = std::make_shared<StaticDrawSnapshot>(*changed);
    changed->revision = 4;
    changed->draws[0].material = none;
    EXPECT_GT(Sum(renderer->Render(View(), changed), 0), Sum(occluded, 0));
}
TEST_F(StaticPreviewRender, UnsupportedMaterialRefusesAndDoesNotConsumeFrameSequence) {
    auto scene = Scene();
    const auto original = renderer->Render(View(), scene);
    auto bad = Material();
    bad->alpha_mode = static_cast<StaticAlphaMode>(55);
    auto changed = std::make_shared<StaticDrawSnapshot>(*scene);
    changed->revision = 2;
    changed->draws[0].material = bad;
    EXPECT_THROW(renderer->Render(View(), changed), std::invalid_argument);
    const auto recovered = renderer->Render(View(), scene);
    EXPECT_EQ(recovered.sequence, 2u);
    EXPECT_EQ(recovered.rgba8, original.rgba8);
}
TEST_F(StaticPreviewRender, RepeatedFailedUploadsReleaseResourcesAndPreserveSuccessfulCache) {
    const auto scene = Scene();
    const auto original = renderer->Render(View(), scene);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        std::weak_ptr<const StaticMesh> rejected_mesh;
        std::weak_ptr<const StaticTexture> rejected_texture;
        {
            auto mesh = Quad();
            mesh->identity = "rejected-mesh-" + std::to_string(attempt);
            auto material = Material();
            material->textures[0].texture =
                Texture("rejected-texture-" + std::to_string(attempt), {255, 0, 0, 255});
            // Refuse after uploading the new mesh and base-color texture.
            material->textures[4].sampler.mag_filter = 0;
            rejected_mesh = mesh;
            rejected_texture = material->textures[0].texture;
            EXPECT_THROW(renderer->Render(View(), Scene(material, mesh)), std::invalid_argument);
        }
        EXPECT_TRUE(rejected_mesh.expired());
        EXPECT_TRUE(rejected_texture.expired());
    }
    const auto recovered = renderer->Render(View(), scene);
    EXPECT_EQ(recovered.sequence, 2u);
    EXPECT_EQ(recovered.rgba8, original.rgba8);
    EXPECT_EQ(recovered.depth32f, original.depth32f);
    EXPECT_EQ(recovered.uploaded_meshes, 0u);
    EXPECT_EQ(recovered.uploaded_textures, 0u);
    EXPECT_EQ(recovered.updated_instances, 0u);
}
TEST_F(StaticPreviewRender, TerrainSnowPreservesAuthoredMaterialButStillAffectsLegacySurface) {
    RenderResourceRegistry resources;
    GBuffer gbuffer;
    LightingPass lighting;
    GLuint quad = 0, buffer = 0;
    struct Cleanup {
        RenderResourceRegistry& resources;
        GLuint& quad;
        GLuint& buffer;
        ~Cleanup() {
            resources.destroy_all_owned();
            glDeleteVertexArrays(1, &quad);
            glDeleteBuffers(1, &buffer);
        }
    } cleanup{resources, quad, buffer};
    constexpr unsigned width = 160, height = 120;
    CreateGBufferTargets(gbuffer, resources, width, height, true);
    glBindFramebuffer(GL_FRAMEBUFFER, gbuffer.fbo_id);
    const GLfloat position[]{0, 0, -3, 0}, normal[]{.5f, 1, 0, 0}, albedo[]{.2f, .05f, .1f, .8f},
        material[]{0, 1, 0, 0}, authored[]{0, 0, 0, 1}, zero[]{0, 0, 0, 0}, depth[]{.03f};
    for (const auto& [attachment, value] : std::array<std::pair<GLint, const GLfloat*>, 6>{
             {{0, position}, {1, normal}, {2, albedo}, {3, material}, {4, zero}, {5, authored}}})
        glClearBufferfv(GL_COLOR, attachment, value);
    glClearBufferfv(GL_DEPTH, 0, depth);
    const float vertices[]{-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    glGenVertexArrays(1, &quad);
    glBindVertexArray(quad);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    for (GLuint attribute = 0; attribute < 2; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(attribute,
                              2,
                              GL_FLOAT,
                              GL_FALSE,
                              4 * sizeof(float),
                              reinterpret_cast<void*>(attribute * 2 * sizeof(float)));
    }
    lighting.init_shader(STATIC_PREVIEW_RESOURCE_ROOT);
    lighting.init_environment_brdf(resources);
    lighting.init_lighting_fbo(resources, width, height);
    const auto view = View();
    RenderContext context;
    context.render_view = &view;
    context.static_studio = true;
    context.screen_width = width;
    context.screen_height = height;
    context.registry = &resources;
    context.screen_quad_vao = quad;
    context.gbuffer_position = adopt_texture(gbuffer.position_texture);
    context.gbuffer_normal = adopt_texture(gbuffer.normal_texture);
    context.gbuffer_albedo = adopt_texture(gbuffer.albedo_texture);
    context.gbuffer_material = adopt_texture(gbuffer.material_texture);
    context.gbuffer_depth = adopt_texture(gbuffer.depth_texture);
    context.authored_surface = adopt_texture(gbuffer.authored_texture);
    context.sun.direction = glm::normalize(glm::vec3(.35f, -.55f, -.75f));
    context.sun.color = glm::vec3(1);
    context.sky_ambient_color = glm::vec3(.22f);
    const auto render = [&] {
        lighting.execute(context);
        std::array<GLubyte, 4> pixel{};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, lighting.lighting_fbo().fbo_id);
        glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
        return pixel;
    };
    const auto baseline = render();
    context.snow_cover = 1;
    EXPECT_EQ(render(), baseline);
    glClearTexImage(gbuffer.authored_texture, 0, GL_RGBA, GL_FLOAT, zero);
    const auto legacy_snow = render();
    context.snow_cover = 0;
    EXPECT_NE(render(), legacy_snow);
}
} // namespace
