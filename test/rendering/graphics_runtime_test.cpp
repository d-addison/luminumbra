#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra_client/app/FrameAttribution.h"
#include "luminumbra_client/app/GraphicsSessionOverrides.h"
#include "luminumbra_client/rendering/FarLodSystem.h"
#include "luminumbra_client/rendering/GraphicsRuntimeConfig.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/RenderResourceRegistry.h"
#include "luminumbra_client/rendering/SceneRenderScale.h"
#include "luminumbra_client/rendering/passes/FoliagePass.h"
#include "luminumbra_client/rendering/passes/ShadowPass.h"
#include "luminumbra_client/ui/BackdropBlurQuality.h"
#include "luminumbra_common/core/GraphicsPresets.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <glm/ext/vector_float3.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using namespace luminumbra::core;

ResolvedGraphics Resolve(GraphicsPreset preset, const GraphicsOverrides& overrides = {}) {
    GraphicsSettings settings;
    EXPECT_TRUE(SelectGraphicsPreset(settings, preset));
    settings.preset_revision = 1; // Exercise the retained visual reductions and resource changes.
    ResolvedGraphics resolved;
    std::string error;
    EXPECT_TRUE(ResolveGraphics(settings, overrides, resolved, error)) << error;
    return resolved;
}

// OpenGL entry-point fixture, never a context or GPU. It models target storage,
// FBO attachment completeness and retirement so failure paths are deterministic.
struct TargetGl {
    struct Texture {
        GLsizei width = 0, height = 0, layers = 1;
        GLint format = 0;
    };
    struct Attachment {
        GLuint texture = 0;
        bool layered = false;
    };
    GLuint next = 1, texture = 0, framebuffer = 0;
    std::map<GLuint, Texture> textures;
    std::map<GLuint, std::map<GLenum, Attachment>> framebuffers;
    int allocations = 0, clears = 0;
    int fail_check = 0, checks = 0;
    bool storage_failed = false;
    bool fail_storage = false;
    bool scissor_enabled = true;
    std::array<GLboolean, 4> color_mask{GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE};
    static inline TargetGl* active = nullptr;

    static GLenum APIENTRY Error() {
        if (active->storage_failed) {
            active->storage_failed = false;
            return GL_OUT_OF_MEMORY;
        }
        return GL_NO_ERROR;
    }
    static void APIENTRY GenTextures(GLsizei count, GLuint* ids) {
        for (GLsizei i = 0; i < count; ++i) {
            ids[i] = active->next++;
            active->textures.emplace(ids[i], Texture{});
            ++active->allocations;
        }
    }
    static void APIENTRY DeleteTextures(GLsizei count, const GLuint* ids) {
        for (GLsizei i = 0; i < count; ++i) {
            for (const auto& [fbo, attachments] : active->framebuffers) {
                (void)fbo;
                for (const auto& [point, attachment] : attachments) {
                    (void)point;
                    EXPECT_NE(attachment.texture, ids[i]) << "retired attachment before FBO";
                }
            }
            active->textures.erase(ids[i]);
        }
    }
    static void APIENTRY BindTexture(GLenum, GLuint id) {
        active->texture = id;
    }
    static void APIENTRY Image3D(GLenum,
                                 GLint,
                                 GLint format,
                                 GLsizei width,
                                 GLsizei height,
                                 GLsizei layers,
                                 GLint,
                                 GLenum,
                                 GLenum,
                                 const void*) {
        active->textures.at(active->texture) = {width, height, layers, format};
        active->storage_failed = active->fail_storage;
    }
    static void APIENTRY Image2D(GLenum target,
                                 GLint level,
                                 GLint format,
                                 GLsizei width,
                                 GLsizei height,
                                 GLint border,
                                 GLenum layout,
                                 GLenum type,
                                 const void* data) {
        Image3D(target, level, format, width, height, 1, border, layout, type, data);
    }
    static void APIENTRY Parameter(GLenum, GLenum, GLint) {}
    static void APIENTRY ParameterVector(GLenum, GLenum, const GLfloat*) {}
    static GLboolean APIENTRY IsObject(GLuint id) {
        return id != 0 ? GL_TRUE : GL_FALSE;
    }
    static void APIENTRY GenFbos(GLsizei count, GLuint* ids) {
        for (GLsizei i = 0; i < count; ++i) {
            ids[i] = active->next++;
            active->framebuffers.emplace(ids[i], std::map<GLenum, Attachment>{});
        }
    }
    static void APIENTRY DeleteFbos(GLsizei count, const GLuint* ids) {
        for (GLsizei i = 0; i < count; ++i)
            active->framebuffers.erase(ids[i]);
    }
    static void APIENTRY BindFbo(GLenum, GLuint id) {
        active->framebuffer = id;
    }
    static void APIENTRY Attach(GLenum, GLenum point, GLuint texture_id, GLint) {
        active->framebuffers.at(active->framebuffer)[point] = {texture_id, true};
    }
    static void APIENTRY
    Attach2D(GLenum target, GLenum point, GLenum, GLuint texture_id, GLint level) {
        Attach(target, point, texture_id, level);
        active->framebuffers.at(active->framebuffer)[point].layered = false;
    }
    static void APIENTRY
    AttachLayer(GLenum target, GLenum point, GLuint texture_id, GLint level, GLint) {
        Attach2D(target, point, GL_TEXTURE_2D_ARRAY, texture_id, level);
    }
    static GLenum APIENTRY Check(GLenum) {
        if (++active->checks == active->fail_check)
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        const auto& attachments = active->framebuffers.at(active->framebuffer);
        if (attachments.empty())
            return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
        const auto first = attachments.begin()->second;
        const auto& extent = active->textures.at(first.texture);
        for (const auto& [point, attachment] : attachments) {
            (void)point;
            const auto& value = active->textures.at(attachment.texture);
            if (value.width != extent.width || value.height != extent.height ||
                value.layers != extent.layers || attachment.layered != first.layered)
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        return GL_FRAMEBUFFER_COMPLETE;
    }
    static void APIENTRY Buffer(GLenum) {}
    static void APIENTRY Buffers(GLsizei, const GLenum*) {}
    static void APIENTRY ClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
    static void APIENTRY Clear(GLbitfield) {
        EXPECT_EQ(Check(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        EXPECT_FALSE(active->scissor_enabled);
        EXPECT_EQ(active->color_mask,
                  (std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}));
        ++active->clears;
    }
    static GLboolean APIENTRY IsEnabled(GLenum) {
        return active->scissor_enabled ? GL_TRUE : GL_FALSE;
    }
    static void APIENTRY Enable(GLenum) {
        active->scissor_enabled = true;
    }
    static void APIENTRY Disable(GLenum) {
        active->scissor_enabled = false;
    }
    static void APIENTRY GetBoolean(GLenum, GLboolean* mask) {
        std::copy(active->color_mask.begin(), active->color_mask.end(), mask);
    }
    static void APIENTRY ColorMask(GLboolean red,
                                   GLboolean green,
                                   GLboolean blue,
                                   GLboolean alpha) {
        active->color_mask = {red, green, blue, alpha};
    }
    static void APIENTRY BindBuffer(GLenum, GLuint) {}
    static void APIENTRY BindVertexArray(GLuint) {}
};

template<class Function>
class ScopedEntryPoint {
    Function& slot;
    Function previous;

public:
    ScopedEntryPoint(Function& entry, Function replacement)
        : slot(entry)
        , previous(entry) {
        slot = replacement;
    }
    ~ScopedEntryPoint() {
        slot = previous;
    }
};

class GraphicsRuntime : public testing::Test {
protected:
    TargetGl& state() {
        return gl;
    }
    void SetUp() override {
        TargetGl::active = &gl;
    }
    void TearDown() override {
        TargetGl::active = nullptr;
    }

private:
    TargetGl gl;
    // GLAD uses the platform's function-pointer calling convention on both
    // Windows and POSIX. Restore every entry before another test can use GL.
    ScopedEntryPoint<decltype(glGetError)> error{glGetError, TargetGl::Error};
    ScopedEntryPoint<decltype(glGenTextures)> gen{glGenTextures, TargetGl::GenTextures};
    ScopedEntryPoint<decltype(glDeleteTextures)> remove{glDeleteTextures, TargetGl::DeleteTextures};
    ScopedEntryPoint<decltype(glBindTexture)> bind{glBindTexture, TargetGl::BindTexture};
    ScopedEntryPoint<decltype(glTexImage3D)> image3{glTexImage3D, TargetGl::Image3D};
    ScopedEntryPoint<decltype(glTexImage2D)> image2{glTexImage2D, TargetGl::Image2D};
    ScopedEntryPoint<decltype(glTexParameteri)> parameter{glTexParameteri, TargetGl::Parameter};
    ScopedEntryPoint<decltype(glTexParameterfv)> vector{glTexParameterfv,
                                                        TargetGl::ParameterVector};
    ScopedEntryPoint<decltype(glIsTexture)> is_texture{glIsTexture, TargetGl::IsObject};
    ScopedEntryPoint<decltype(glIsFramebuffer)> is_fbo{glIsFramebuffer, TargetGl::IsObject};
    ScopedEntryPoint<decltype(glObjectLabel)> label{glObjectLabel, nullptr};
    ScopedEntryPoint<decltype(glGenFramebuffers)> gen_fbo{glGenFramebuffers, TargetGl::GenFbos};
    ScopedEntryPoint<decltype(glDeleteFramebuffers)> remove_fbo{glDeleteFramebuffers,
                                                                TargetGl::DeleteFbos};
    ScopedEntryPoint<decltype(glBindFramebuffer)> bind_fbo{glBindFramebuffer, TargetGl::BindFbo};
    ScopedEntryPoint<decltype(glFramebufferTexture)> attach{glFramebufferTexture, TargetGl::Attach};
    ScopedEntryPoint<decltype(glFramebufferTexture2D)> attach2{glFramebufferTexture2D,
                                                               TargetGl::Attach2D};
    ScopedEntryPoint<decltype(glFramebufferTextureLayer)> layer{glFramebufferTextureLayer,
                                                                TargetGl::AttachLayer};
    ScopedEntryPoint<decltype(glCheckFramebufferStatus)> check{glCheckFramebufferStatus,
                                                               TargetGl::Check};
    ScopedEntryPoint<decltype(glDrawBuffer)> draw{glDrawBuffer, TargetGl::Buffer};
    ScopedEntryPoint<decltype(glReadBuffer)> read{glReadBuffer, TargetGl::Buffer};
    ScopedEntryPoint<decltype(glDrawBuffers)> draws{glDrawBuffers, TargetGl::Buffers};
    ScopedEntryPoint<decltype(glClearColor)> color{glClearColor, TargetGl::ClearColor};
    ScopedEntryPoint<decltype(glClear)> clear{glClear, TargetGl::Clear};
    ScopedEntryPoint<decltype(glIsEnabled)> enabled{glIsEnabled, TargetGl::IsEnabled};
    ScopedEntryPoint<decltype(glEnable)> enable{glEnable, TargetGl::Enable};
    ScopedEntryPoint<decltype(glDisable)> disable{glDisable, TargetGl::Disable};
    ScopedEntryPoint<decltype(glGetBooleanv)> boolean{glGetBooleanv, TargetGl::GetBoolean};
    ScopedEntryPoint<decltype(glColorMask)> mask{glColorMask, TargetGl::ColorMask};
    ScopedEntryPoint<decltype(glBindBuffer)> bind_buffer{glBindBuffer, TargetGl::BindBuffer};
    ScopedEntryPoint<decltype(glBindVertexArray)> bind_vao{glBindVertexArray,
                                                           TargetGl::BindVertexArray};
};

class Targets {
    RenderResourceRegistry m_registry;

public:
    RenderResourceRegistry& registry() {
        return m_registry;
    }
    ~Targets() {
        m_registry.destroy_all_owned();
    }
};

TEST_F(GraphicsRuntime, UltraRendererSnapshotMatchesShippingConstants) {
    RenderPipeline pipeline;
    ASSERT_TRUE(pipeline.apply_graphics(Resolve(GraphicsPreset::Ultra)));
    const auto v = pipeline.graphics_runtime_config();
    EXPECT_EQ(v,
              (GraphicsRuntimeConfig{.render_scale = 1.0f,
                                     .grass_density = 1.6f,
                                     .grass_fade_start_m = 48.0f,
                                     .grass_fade_end_m = 92.0f,
                                     .far_radius_m = 3000.0f,
                                     .far_outer_clip_m = 3050.0f,
                                     .shadow_resolution = 2048,
                                     .volumetric_quality = 1,
                                     .grass_enabled = true,
                                     .ui_blur = UiBlurQuality::Full}));
    EXPECT_FLOAT_EQ(v.far_radius_m, FarLodSystem::kF2OuterRangeMeters);
    EXPECT_FLOAT_EQ(v.far_outer_clip_m, FarLodSystem::kFarClipOuterRadiusMeters);
    EXPECT_EQ(v.shadow_resolution, static_cast<int>(ShadowMap{}.resolution));
    EXPECT_EQ(FoliagePass::kMaxCandidatesPerChunk, 8192u);
    EXPECT_EQ(FoliagePass::kMaxInstances, 262144u);
    EXPECT_EQ(ShadowMap::CASCADE_COUNT, 4);
    EXPECT_EQ(state().allocations, 0); // Startup seeding does not allocate targets.
}

TEST_F(GraphicsRuntime, PreviewExitRestoresEveryPresetIncludingUltraWithUnchangedSettings) {
    RenderPipeline pipeline;
    const std::array presets{
        GraphicsPreset::Low, GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra};
    const std::array densities{.4f, .8f, 1.2f, 1.6f};
    const std::array starts{24.f, 32.f, 40.f, 48.f};
    const std::array ends{48.f, 64.f, 80.f, 92.f};
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const auto resolved = Resolve(presets[i]);
        SCOPED_TRACE(i);
        ASSERT_TRUE(pipeline.apply_graphics(resolved));
        const auto gameplay = pipeline.graphics_runtime_config();
        // The shared preview loads the same scatter set and applies its
        // showcase values, without changing any saved graphics inputs.
        auto* foliage = pipeline.foliage();
        foliage->set_enabled(true);
        foliage->set_density_scale(1.6f);
        foliage->set_fade_distances(60.f, 96.f);
        const auto allocations = state().allocations;
        ASSERT_TRUE(pipeline.apply_graphics(resolved)); // Unconditional preview exit.
        EXPECT_EQ(pipeline.graphics_runtime_config(), gameplay);
        EXPECT_FLOAT_EQ(foliage->density_scale(), densities[i]);
        EXPECT_FLOAT_EQ(foliage->fade_start_m(), starts[i]);
        EXPECT_FLOAT_EQ(foliage->fade_end_m(), ends[i]);
        EXPECT_EQ(state().allocations, allocations);
    }
}

TEST_F(GraphicsRuntime, FoliageAttributionRetainsHistoryAcrossGraphicsInvalidation) {
    namespace Attribution = Luminumbra::Client::Attribution;
    FoliagePass foliage;
    const auto reasons = [&] {
        const Attribution::FoliageFrameScope frame(true);
        // Empty inputs and an unavailable surface query need no graphics resources.
        foliage.rebuild_instances({}, nullptr, nullptr, glm::vec3(0));
        return frame.snapshot().rebuild.reasons;
    };
    foliage.set_density_scale(1.2f);
    foliage.set_fade_distances(40.f, 80.f);
    EXPECT_EQ(reasons(), Attribution::Initial);
    EXPECT_EQ(reasons(), 0u);

    foliage.set_density_scale(.8f);
    EXPECT_EQ(reasons(), Attribution::Config);
    foliage.set_density_scale(.801f); // Same quantized signature, different live setting.
    EXPECT_EQ(reasons(), Attribution::Config);
    foliage.set_density_scale(.801f);
    EXPECT_EQ(reasons(), 0u);

    foliage.set_fade_distances(32.f, 64.f);
    EXPECT_EQ(reasons(), Attribution::Config);
    foliage.set_fade_distances(32.1f, 64.1f); // Also below signature quantization.
    EXPECT_EQ(reasons(), Attribution::Config);
    foliage.set_fade_distances(32.1f, 64.1f);
    EXPECT_EQ(reasons(), 0u);

    foliage.set_density_scale(-1.f);
    EXPECT_EQ(reasons(), Attribution::Config);
    EXPECT_FLOAT_EQ(foliage.density_scale(), 1.f);
    foliage.set_density_scale(0.f); // Same normalized value does not invalidate.
    EXPECT_EQ(reasons(), 0u);

    foliage.clear_ground_meshes();
    EXPECT_EQ(reasons(), Attribution::Initial);
    EXPECT_EQ(reasons(), 0u);
}

TEST_F(GraphicsRuntime, FoliageAttributionStartsWithTheFirstCollectedSignature) {
    namespace Attribution = Luminumbra::Client::Attribution;
    FoliagePass foliage;
    {
        const Attribution::FoliageFrameScope disabled;
        foliage.rebuild_instances({}, nullptr, nullptr, glm::vec3(0));
    }
    const Attribution::FoliageFrameScope frame(true);
    foliage.rebuild_instances({}, nullptr, nullptr, glm::vec3(0));
    EXPECT_EQ(frame.snapshot().rebuild.reasons, Attribution::Initial);
    EXPECT_EQ(frame.snapshot().rebuild.rebuild_calls, 1u);
}

TEST_F(GraphicsRuntime, EveryPresetReachesPassesAndUnchangedApplicationDoesNotAllocate) {
    const std::array expected{GraphicsRuntimeConfig{.render_scale = .67f,
                                                    .grass_density = .4f,
                                                    .grass_fade_start_m = 24,
                                                    .grass_fade_end_m = 48,
                                                    .far_radius_m = 1500,
                                                    .far_outer_clip_m = 1550,
                                                    .shadow_resolution = 1024,
                                                    .volumetric_quality = 0,
                                                    .grass_enabled = true,
                                                    .ui_blur = UiBlurQuality::Off},
                              GraphicsRuntimeConfig{.render_scale = .8f,
                                                    .grass_density = .8f,
                                                    .grass_fade_start_m = 32,
                                                    .grass_fade_end_m = 64,
                                                    .far_radius_m = 2250,
                                                    .far_outer_clip_m = 2300,
                                                    .shadow_resolution = 1536,
                                                    .volumetric_quality = 0,
                                                    .grass_enabled = true,
                                                    .ui_blur = UiBlurQuality::Half},
                              GraphicsRuntimeConfig{.render_scale = 1,
                                                    .grass_density = 1.2f,
                                                    .grass_fade_start_m = 40,
                                                    .grass_fade_end_m = 80,
                                                    .far_radius_m = 3000,
                                                    .far_outer_clip_m = 3050,
                                                    .shadow_resolution = 2048,
                                                    .volumetric_quality = 1,
                                                    .grass_enabled = true,
                                                    .ui_blur = UiBlurQuality::Full},
                              GraphicsRuntimeConfig{}};
    RenderPipeline pipeline;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto resolved = Resolve(static_cast<GraphicsPreset>(i));
        ASSERT_TRUE(pipeline.apply_graphics(resolved));
        EXPECT_EQ(pipeline.graphics_runtime_config(), expected[i]);
        ASSERT_TRUE(pipeline.apply_graphics(resolved));
        EXPECT_EQ(pipeline.graphics_runtime_config(), expected[i]);
    }
    EXPECT_EQ(state().allocations, 0);
}

TEST_F(GraphicsRuntime, NewSelectionsRetainUltraPassValuesPendingVisualApproval) {
    RenderPipeline pipeline;
    for (const auto preset : {GraphicsPreset::Low,
                              GraphicsPreset::Medium,
                              GraphicsPreset::High,
                              GraphicsPreset::Ultra}) {
        GraphicsSettings settings;
        ASSERT_TRUE(SelectGraphicsPreset(settings, preset));
        ResolvedGraphics resolved;
        std::string resolution_error;
        ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, resolution_error));
        ASSERT_TRUE(pipeline.apply_graphics(resolved));
        EXPECT_EQ(pipeline.graphics_runtime_config(), GraphicsRuntimeConfig{});
    }
    EXPECT_EQ(state().allocations, 0);
}

TEST_F(GraphicsRuntime, SessionOverridesWinAcrossLivePresetChangesWithoutMutatingSavedSettings) {
    auto config = SystemConfig::Defaults();
    config.user().graphics.overrides["render_scale"] = .9;
    const auto session = Luminumbra::Client::GraphicsSessionOverrides("performance", .75f, 1);
    RenderPipeline pipeline;
    for (auto preset : {GraphicsPreset::Low, GraphicsPreset::Ultra}) {
        config.user().graphics.base_preset = preset;
        const auto before = config.user().graphics.overrides;
        ResolvedGraphics resolved;
        std::string error_text;
        ASSERT_TRUE(ResolveGraphics(config.user().graphics, session, resolved, error_text));
        ASSERT_TRUE(pipeline.apply_graphics(resolved));
        EXPECT_FLOAT_EQ(pipeline.render_scale(), .75f); // Environment > benchmark > saved.
        EXPECT_EQ(pipeline.volumetric_quality(), 1);    // CLI > Low's froxel off.
        EXPECT_EQ(config.user().graphics.overrides, before);
    }
    EXPECT_DOUBLE_EQ(std::get<double>(Luminumbra::Client::GraphicsSessionOverrides(
                                          "quality", std::nullopt, std::nullopt)
                                          .at("render_scale")),
                     1.0);
    EXPECT_TRUE(Luminumbra::Client::GraphicsSessionOverrides("", 0.0f, std::nullopt).empty());
    ASSERT_TRUE(pipeline.apply_graphics(Resolve(
        GraphicsPreset::Ultra, Luminumbra::Client::GraphicsSessionOverrides("", std::nullopt, 0))));
    EXPECT_EQ(pipeline.volumetric_quality(), 0);
}

TEST_F(GraphicsRuntime, RuntimeApplicationCannotChangeSimulationOrSessionIdentity) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "data/common/systems.json";
    auto config = SystemConfig::LoadFromFile(path.string());
    Luminumbra::world::GameSession session;
    session.ApplySystemConfig(config);
    const auto identity = session.SystemConfigIdentity();
    const auto hash = config.ComputeConfigSubHash();
    const auto gates = config.SimulationGates();
    RenderPipeline pipeline;
    for (auto preset : {GraphicsPreset::Low,
                        GraphicsPreset::Medium,
                        GraphicsPreset::High,
                        GraphicsPreset::Ultra}) {
        ASSERT_TRUE(SelectGraphicsPreset(config.user().graphics, preset));
        ASSERT_TRUE(pipeline.apply_graphics(Resolve(preset)));
        session.ApplySystemConfig(config);
        EXPECT_EQ(session.SystemConfigIdentity(), identity);
        EXPECT_EQ(config.ComputeConfigSubHash(), hash);
        EXPECT_EQ(config.SimulationGates(), gates);
        EXPECT_TRUE(config.enabled(SysKey::SimWaterHighRes));
        EXPECT_TRUE(config.enabled(SysKey::SimActiveRegions));
    }
}

TEST_F(GraphicsRuntime, ShadowResizePublishesCompleteDepthAndTintBeforeRetiringOldTargets) {
    Targets owner;
    ShadowPass shadow;
    ASSERT_TRUE(shadow.init_shadow_map(owner.registry()));
    EXPECT_EQ(state().clears, 4);
    EXPECT_TRUE(state().scissor_enabled);
    EXPECT_EQ(state().color_mask, (std::array<GLboolean, 4>{GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE}));
    const auto original = shadow.shadow_map().depth_texture_array;
    for (const std::uint32_t size : {1024u, 1536u, 2048u, 1024u}) {
        const auto depth = shadow.shadow_map().depth_texture_array;
        const auto tint = shadow.tint_texture_array();
        ASSERT_TRUE(shadow.set_resolution(owner.registry(), size));
        EXPECT_NE(shadow.shadow_map().depth_texture_array, depth);
        EXPECT_NE(shadow.tint_texture_array(), tint);
        EXPECT_FALSE(state().textures.contains(depth));
        EXPECT_FALSE(state().textures.contains(tint));
        EXPECT_EQ(state().textures.at(shadow.shadow_map().depth_texture_array).width,
                  static_cast<GLsizei>(size));
        EXPECT_EQ(state().textures.at(shadow.tint_texture_array()).height,
                  static_cast<GLsizei>(size));
        EXPECT_EQ(state().textures.size(), 2u);
        EXPECT_EQ(state().framebuffers.size(), 2u);
        const auto allocations = state().allocations;
        const auto clears = state().clears;
        ASSERT_TRUE(shadow.set_resolution(owner.registry(), size));
        EXPECT_EQ(state().allocations, allocations);
        EXPECT_EQ(state().clears, clears);
    }
    EXPECT_FALSE(state().textures.contains(original));
    EXPECT_EQ(state().clears, 20);
    EXPECT_FALSE(shadow.set_resolution(owner.registry(), 4096));
}

TEST_F(GraphicsRuntime, ShadowAllocationAndFramebufferFailuresRetainUsableOldAtlas) {
    Targets owner;
    ShadowPass shadow;
    ASSERT_TRUE(shadow.init_shadow_map(owner.registry()));
    const auto depth = shadow.shadow_map().depth_texture_array;
    const auto tint = shadow.tint_texture_array();
    const auto framebuffer = shadow.shadow_map().fbo_id;
    state().fail_storage = true;
    EXPECT_FALSE(shadow.set_resolution(owner.registry(), 1024));
    state().fail_storage = false;
    for (const int check_offset : {1, 2}) {
        state().fail_check = state().checks + check_offset;
        EXPECT_FALSE(shadow.set_resolution(owner.registry(), 1024));
        EXPECT_EQ(shadow.shadow_map().resolution, 2048u);
        EXPECT_EQ(shadow.shadow_map().depth_texture_array, depth);
        EXPECT_EQ(shadow.tint_texture_array(), tint);
        EXPECT_EQ(shadow.shadow_map().fbo_id, framebuffer);
        EXPECT_EQ(state().textures.size(), 2u);
        EXPECT_EQ(state().framebuffers.size(), 2u);
    }
    state().fail_check = 0;
    ASSERT_TRUE(shadow.set_resolution(owner.registry(), 1536));
}

TEST_F(GraphicsRuntime, SceneAndShadowReplacementRollBackTogetherOnIncompleteFramebuffer) {
    Targets owner;
    auto& registry = owner.registry();
    ShadowPass shadow;
    ASSERT_TRUE(shadow.init_shadow_map(registry));
    TextureDesc scene_color;
    scene_color.width = 1920;
    scene_color.height = 1080;
    scene_color.internal_format = GL_RGBA16F;
    scene_color.format = GL_RGBA;
    scene_color.type = GL_FLOAT;
    const auto scene = registry.create_texture("scene", scene_color).id;
    auto depth = scene_color;
    depth.internal_format = GL_DEPTH_COMPONENT32F;
    depth.format = GL_DEPTH_COMPONENT;
    const auto scene_depth = registry.create_texture("scene_depth", depth).id;
    FboDesc fbo;
    fbo.attachments = {{GL_COLOR_ATTACHMENT0, "scene"}, {GL_DEPTH_ATTACHMENT, "scene_depth"}};
    const auto scene_fbo = registry.create_fbo("scene_fbo", fbo).id;
    ASSERT_NE(scene_fbo, 0u);
    const auto shadow_depth = shadow.shadow_map().depth_texture_array;
    const auto shadow_tint = shadow.tint_texture_array();
    const std::array extents{
        RenderResourceRegistry::TargetExtent{"scene", 1536, 864},
        RenderResourceRegistry::TargetExtent{"scene_depth", 1536, 864},
        RenderResourceRegistry::TargetExtent{"shadow_depth_cascades", 1024, 1024},
        RenderResourceRegistry::TargetExtent{"shadow_tint_cascades", 1024, 1024}};
    state().fail_check = state().checks + 3; // Fail the last of the three new FBOs.
    EXPECT_FALSE(registry.replace_targets(extents));
    EXPECT_EQ(registry.texture("scene").id, scene);
    EXPECT_EQ(registry.texture("scene_depth").id, scene_depth);
    EXPECT_EQ(registry.fbo("scene_fbo").id, scene_fbo);
    EXPECT_EQ(registry.texture("shadow_depth_cascades").id, shadow_depth);
    EXPECT_EQ(registry.texture("shadow_tint_cascades").id, shadow_tint);
    EXPECT_EQ(state().textures.size(), 4u);
    EXPECT_EQ(state().framebuffers.size(), 3u);
    state().fail_check = 0;
    ASSERT_TRUE(registry.replace_targets(extents));
    const auto allocations = state().allocations;
    ASSERT_TRUE(shadow.set_resolution(registry, 1024)); // Refresh, without another allocation.
    EXPECT_EQ(state().allocations, allocations);
    EXPECT_EQ(shadow.shadow_map().depth_texture_array,
              registry.texture("shadow_depth_cascades").id);
    EXPECT_FALSE(state().textures.contains(scene));
    EXPECT_FALSE(state().textures.contains(scene_depth));
    EXPECT_FALSE(state().textures.contains(shadow_depth));
    EXPECT_FALSE(state().textures.contains(shadow_tint));
    EXPECT_EQ(state().textures.size(), 4u);
    EXPECT_EQ(state().framebuffers.size(), 3u);
}

TEST_F(GraphicsRuntime, LiveScaleResetsHistoryOnceAfterCompleteTargetsAndRetainsItOnFailure) {
    Targets owner;
    TextureDesc desc;
    desc.width = 1920;
    desc.height = 1080;
    desc.internal_format = GL_RGBA16F;
    desc.format = GL_RGBA;
    desc.type = GL_FLOAT;
    const auto original = owner.registry().create_texture("scene", desc).id;
    FboDesc fbo;
    fbo.attachments = {{GL_COLOR_ATTACHMENT0, "scene"}};
    ASSERT_NE(owner.registry().create_fbo("scene_fbo", fbo).id, 0u);
    float scale = 1.0f;
    bool history_valid = true;
    int resets = 0;
    const auto recreate = [&](float requested) {
        EXPECT_TRUE(history_valid); // Reset follows successful publication.
        const std::array extents{
            RenderResourceRegistry::TargetExtent{"scene",
                                                 static_cast<std::uint32_t>(1920 * requested),
                                                 static_cast<std::uint32_t>(1080 * requested)}};
        return owner.registry().replace_targets(extents);
    };
    const auto reset = [&] {
        history_valid = false;
        ++resets;
    };
    ASSERT_TRUE(ApplySceneRenderScale(scale, .8f, true, recreate, reset));
    EXPECT_FALSE(history_valid);
    EXPECT_EQ(resets, 1);
    EXPECT_FALSE(state().textures.contains(original));
    const auto allocated = state().allocations;
    history_valid = true;
    ASSERT_TRUE(ApplySceneRenderScale(scale, .8f, true, recreate, reset));
    EXPECT_TRUE(history_valid);
    EXPECT_EQ(state().allocations, allocated);
    EXPECT_EQ(resets, 1);
    state().fail_storage = true;
    EXPECT_FALSE(ApplySceneRenderScale(scale, .67f, true, recreate, reset));
    EXPECT_TRUE(history_valid);
    EXPECT_FLOAT_EQ(scale, .8f);
    EXPECT_EQ(resets, 1);
    state().fail_storage = false;
    ASSERT_TRUE(ApplySceneRenderScale(scale, 1.0f, true, recreate, reset));
    EXPECT_FALSE(history_valid);
    EXPECT_EQ(resets, 2);
}

TEST(GraphicsRuntimePure, FarHorizonChangesAdmissionAndKeepsInnerCoverage) {
    FarLodSystem far;
    const glm::vec3 anchor(0);
    EXPECT_TRUE(far.region_admission(5, 0, anchor).wanted);
    for (const auto preset : {GraphicsPreset::Low,
                              GraphicsPreset::Medium,
                              GraphicsPreset::High,
                              GraphicsPreset::Ultra}) {
        const auto v = GraphicsRuntimeValues(Resolve(preset));
        far.set_horizon(v.far_radius_m, v.far_outer_clip_m);
        EXPECT_FLOAT_EQ(far.wanted_radius_m(), v.far_radius_m);
        EXPECT_FLOAT_EQ(far.outer_clip_m(), v.far_outer_clip_m);
        EXPECT_TRUE(far.region_admission(0, 0, anchor).wanted);
        EXPECT_EQ(far.region_admission(0, 0, anchor).tier, Luminumbra::World::FarLodTier::F1);
        EXPECT_EQ(far.region_admission(2, 0, anchor).tier, Luminumbra::World::FarLodTier::F2);
        EXPECT_TRUE(far.region_admission(2, 0, anchor).wanted);
        EXPECT_EQ(far.region_admission(3, 0, anchor).wanted, preset != GraphicsPreset::Low);
        EXPECT_EQ(far.region_admission(5, 0, anchor).wanted,
                  preset == GraphicsPreset::High || preset == GraphicsPreset::Ultra);
        EXPECT_FALSE(far.region_admission(7, 0, anchor).wanted);
        EXPECT_EQ(far.region_admission(-6, 0, anchor).wanted,
                  far.region_admission(5, 0, anchor).wanted);
    }
    far.set_horizon(1500, 3050); // Reject unpaired horizons transactionally.
    EXPECT_FLOAT_EQ(far.wanted_radius_m(), 3000);
    far.set_horizon(std::numeric_limits<float>::quiet_NaN(), 1550);
    EXPECT_FLOAT_EQ(far.outer_clip_m(), 3050);
}

TEST_F(GraphicsRuntime, FractionalCustomHorizonSurvivesIndependentFloatRounding) {
    RenderPipeline pipeline;
    const auto resolved = Resolve(GraphicsPreset::Ultra, {{"far_terrain_radius_m", 2000.000065}});
    ASSERT_TRUE(pipeline.apply_graphics(resolved));
    const auto values = pipeline.graphics_runtime_config();
    EXPECT_FLOAT_EQ(values.far_radius_m, static_cast<float>(resolved.values.far_terrain_radius_m));
    EXPECT_FLOAT_EQ(values.far_outer_clip_m, static_cast<float>(resolved.values.far_outer_clip_m));
}

TEST(GraphicsRuntimePure, ShrinkingAndGrowingHorizonRetainsEditedAuthorityAndRejectsStaleCapture) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0;
    params.height_offset = 12;
    params.caves_enabled = false;
    Luminumbra::Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    // Region three enters Medium's horizon and leaves Low's horizon.
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(100, 0, 5));
    world.GenerateChunkData(*chunk, 1);
    ASSERT_FALSE(chunk->sdf_data.empty());
    chunk->sdf_data.front() = 1;
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    const auto before = world.capture_far_lod_sdf_snapshot(3, 0);
    ASSERT_TRUE(before);
    ASSERT_EQ(before->entries.size(), 1u);
    EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*before));
    EXPECT_GT(before->region_authority_revision, 0u); // Adoption published authority.
    EXPECT_EQ(before->entries.front().provenance, Luminumbra::ChunkSdfProvenance::LoadedOrEdited);
    ASSERT_TRUE(before->entries.front().payload);
    ASSERT_FALSE(before->entries.front().as_reduction_snapshot().sdf_data.empty());
    EXPECT_EQ(before->entries.front().as_reduction_snapshot().sdf_data.front(), 1);
    FarLodSystem far;
    far.set_horizon(1500, 1550);
    EXPECT_FALSE(far.region_admission(3, 0, glm::vec3(0)).wanted);
    EXPECT_TRUE(far.region_admission(0, 0, glm::vec3(0)).wanted);
    chunk->sdf_data.front() = -1;
    chunk->mark_voxel_data_dirty();
    EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*before));
    for (const auto preset : {GraphicsPreset::Medium,
                              GraphicsPreset::Low,
                              GraphicsPreset::High,
                              GraphicsPreset::Low,
                              GraphicsPreset::Ultra}) {
        SCOPED_TRACE(GraphicsPresetName(preset));
        const auto values = GraphicsRuntimeValues(Resolve(preset));
        far.set_horizon(values.far_radius_m, values.far_outer_clip_m);
        const auto admission = far.region_admission(3, 0, glm::vec3(0));
        EXPECT_EQ(admission.wanted, preset != GraphicsPreset::Low);
        EXPECT_EQ(admission.tier, Luminumbra::World::FarLodTier::F2);
        const auto current = world.capture_far_lod_sdf_snapshot(3, 0);
        ASSERT_TRUE(current);
        ASSERT_EQ(current->entries.size(), 1u);
        EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*current));
        EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*before));
        EXPECT_EQ(before->entries.front().as_reduction_snapshot().sdf_data.front(),
                  1); // The capture owns its bytes.
        EXPECT_EQ(current->entries.front().coords, chunk->get_coords());
        EXPECT_EQ(current->entries.front().provenance,
                  Luminumbra::ChunkSdfProvenance::LoadedOrEdited);
        EXPECT_FALSE(current->entries.front().authority_durable);
        ASSERT_TRUE(current->entries.front().payload);
        ASSERT_FALSE(current->entries.front().as_reduction_snapshot().sdf_data.empty());
        EXPECT_EQ(current->entries.front().as_reduction_snapshot().sdf_data.front(), -1);
        EXPECT_GT(current->entries.front().voxel_revision, before->entries.front().voxel_revision);
        // Direct chunk edits advance the local revision. World/region counters
        // track world-owned authority publication, independently of the horizon.
        EXPECT_EQ(current->authority_revision, before->authority_revision);
        EXPECT_EQ(current->region_authority_revision, before->region_authority_revision);
    }
}

TEST(GraphicsRuntimePure, BackdropQualityScalesEveryAuthoredRadiusWithoutCompounding) {
    Luminumbra::Client::BackdropBlurQuality blur;
    for (const float radius : {12.0f, 22.0f, 14.0f, 22.0f, 3.5f}) {
        EXPECT_FLOAT_EQ(blur.radius(radius), radius);
        blur.set(UiBlurQuality::Half);
        EXPECT_FLOAT_EQ(blur.radius(radius), radius / 2);
        blur.set(UiBlurQuality::Off);
        EXPECT_FLOAT_EQ(blur.radius(radius), 0);
        blur.set(UiBlurQuality::Full);
        EXPECT_FLOAT_EQ(blur.radius(radius), radius);
    }
}

TEST(GraphicsRuntimePure, GrassFadeRebuildsOnceIncludingSubmetreChangesAndEdits) {
    FoliagePass foliage;
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    ASSERT_TRUE(foliage.load_scatter_set(root / "data/common/foliage/scatter_set.json"));
    foliage.set_readback_enabled(false);
    foliage.set_fade_distances(24, 48);
    const std::vector<FoliagePass::ChunkScatter> chunks{
        {.chunk_xz = {0, 0}, .origin = {0, 0, 0}, .extent_m = 16, .biome_id = 1, .density = .3f}};
    const auto query = +[](void*, float, float) -> FoliagePass::SurfaceSample {
        return {.height = 8, .slope = 0, .moisture = 1, .valid = true};
    };
    const glm::vec3 camera(0);
    foliage.rebuild_instances(chunks, query, nullptr, camera);
    const auto first = foliage.build_generation();
    ASSERT_FALSE(foliage.instances().empty());
    foliage.set_fade_distances(24, 48);
    foliage.rebuild_instances(chunks, query, nullptr, camera);
    EXPECT_EQ(foliage.build_generation(), first);
    foliage.set_fade_distances(24.1f, 48.1f); // Former signature rounded both metres.
    foliage.rebuild_instances(chunks, query, nullptr, camera);
    EXPECT_EQ(foliage.build_generation(), first + 1);
    foliage.rebuild_instances(chunks, query, nullptr, camera);
    EXPECT_EQ(foliage.build_generation(), first + 1);
    foliage.set_density_scale(foliage.density_scale() + .001f);
    foliage.rebuild_instances(chunks, query, nullptr, camera);
    EXPECT_EQ(foliage.build_generation(), first + 2);
    foliage.use_rendered_ground();
    // The removed surface is not permission to reuse cached support after an
    // edit, including following a fade expansion into the formerly hidden area.
    foliage.update_ground_mesh(1, {0, 0, 0}, {}, {});
    foliage.set_fade_distances(48, 92);
    foliage.rebuild_instances(chunks, query, nullptr, camera);
    EXPECT_TRUE(foliage.instances().empty());
}

} // namespace
