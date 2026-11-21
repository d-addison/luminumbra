#include "GBufferPass.h"
#include "../DiagnosticDrawTable.h"
#include "../FarWorldReplay.h"
#include "../GBufferTargets.h"
#include "DebugViewPass.h"
#include "StaticDrawPass.h"
#include "core/Profiler.h"
#include "rendering/AttributedGl.h"
#include <luminumbra/rendering/StaticScene.h>
#include <stdexcept>

#include "../FarLodSystem.h"
#include "../PassShaderLayouts.h"      // enumerable ExpectedLayout registry
#include "../RenderResourceRegistry.h" // /: registry-owned G-buffer
#include "../TerrainAnchor.h"
#include "../TreeLod.h" // tree rendering: per-instance distance LOD mesh selection (render-only)
#include "PassGlHelpers.h"
#include "core/IsolationConfig.h" // ctx.isolation->renders(...) needs the full type
#include "core/Log.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/world/Chunk.h"
#include "rendering/Camera.h"
#include "rendering/Mesh.h"
#include "rendering/Shader.h"

#include <cmath> // std::sin/std::floor for the render-only per-instance tint hash
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono> // CPU submit cost of the static-prop pass
#include <cstddef>
#include <filesystem> // existence-probe LOD variants before loading (avoid load-error spam)
#include <limits>
#include <string>
#include <tuple>
#include <utility> // std::pair for the LOD-resolve helper return

#include "luminumbra_common/animation/AnimationRuntime.h"

namespace Luminumbra::Rendering {

// capacity of the shared per-(mesh,material) instance-matrix VBO. The draw
// path clamps uploads + draw counts to this so a group larger than capacity can
// never run glBufferSubData past the buffer end (GL_INVALID_VALUE -> garbage /
// dropped instances). Must exceed the largest single instance group.
//
// the old 16384 cap SILENTLY CLAMPED below the dense forest's
// per-group visible count (~28k tree parts / ~40k bushes -> dropped props on the
// budget-dense pose). Sized to cover the full scattered set in a single group
// (worst case ~84k); the GPU-driven path replaces this with a
// per-instance SSBO sized for the total set + growth.
static constexpr GLsizei kStaticInstanceCapacity = 131072;

GBufferPass::GBufferPass() = default;
GBufferPass::~GBufferPass() = default;

FarWorldSelection GBufferPass::SelectFarWorldRenderer(FarWorldSelection selection) const {
    if (selection.effective == FarWorldMode::Ladder) {
        for (const auto* shader : {m_geometry_ladder_shader.get(),
                                   m_instanced_static_mesh_ladder_shader.get(),
                                   m_skinned_mesh_ladder_shader.get()}) {
            // IsValid means nonzero and successfully linked/adopted by Shader.
            if (!shader || !shader->IsValid()) {
                selection.effective = FarWorldMode::Legacy;
                selection.refusal = FarWorldRefusal::ShaderProgramUnavailable;
                break;
            }
        }
    }
    return selection;
}

namespace {
bool UseDrawProgram(const Shader* shader, GBufferDrawStats& stats) {
    if (!shader || !shader->IsValid()) {
        stats.refusal = FarWorldRefusal::ShaderProgramUnavailable;
        ++stats.invalid_program_refusals;
        return false;
    }
    shader->use();
    return true;
}
} // namespace

void GBufferPass::register_cached_mesh(const std::string& key, std::unique_ptr<Mesh> mesh) {
    if (mesh)
        m_meshCache[key] = std::move(mesh);
}
bool GBufferPass::has_cached_mesh(const std::string& key) const {
    return m_meshCache.find(key) != m_meshCache.end();
}

void GBufferPass::init_geometry_shader(const std::filesystem::path& root_path) {
    m_geometry_shader =
        std::make_unique<Shader>((root_path / "res/shaders/g_buffer.vert").string().c_str(),
                                 (root_path / "res/shaders/g_buffer.frag").string().c_str());
    m_geometry_ladder_shader =
        std::make_unique<Shader>((root_path / "res/shaders/g_buffer.vert").string().c_str(),
                                 (root_path / "res/shaders/g_buffer.frag").string().c_str(),
                                 std::vector<std::string>{},
                                 std::vector<std::string>{"LUMINUMBRA_FAR_WORLD_LADDER 1"});
    m_geometry_far_uniforms.Cache(*m_geometry_shader);
    m_geometry_ladder_uniforms.Cache(*m_geometry_ladder_shader);
    if (m_geometry_ladder_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("gbuffer_geometry"))
            m_geometry_ladder_shader->ValidateLayout(*layout);
    }
    PassGl::label_gl_object(
        GL_PROGRAM, m_geometry_shader ? m_geometry_shader->Id() : 0u, "shader.geometry");
    // validate g_buffer.frag's sampler bindings against the registry.
    if (m_geometry_shader && m_geometry_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("gbuffer_geometry"))
            m_geometry_shader->ValidateLayout(*layout);
    }
}

void GBufferPass::init_instanced_static_mesh(const std::filesystem::path& root_path) {
    std::string instanced_vert_path = (root_path / "res/shaders/instanced_mesh.vert").string();
    std::string gbuffer_frag_path = (root_path / "res/shaders/g_buffer.frag").string();
    m_instanced_static_mesh_shader =
        std::make_unique<Shader>(instanced_vert_path.c_str(), gbuffer_frag_path.c_str());
    m_instanced_static_mesh_ladder_shader =
        std::make_unique<Shader>((root_path / "res/shaders/instanced_mesh.vert").string().c_str(),
                                 (root_path / "res/shaders/g_buffer.frag").string().c_str(),
                                 std::vector<std::string>{},
                                 std::vector<std::string>{"LUMINUMBRA_FAR_WORLD_LADDER 1"});
    m_static_far_uniforms.Cache(*m_instanced_static_mesh_shader);
    m_static_ladder_uniforms.Cache(*m_instanced_static_mesh_ladder_shader);
    if (m_instanced_static_mesh_ladder_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("gbuffer_static"))
            m_instanced_static_mesh_ladder_shader->ValidateLayout(*layout);
    }
    PassGl::label_gl_object(GL_PROGRAM,
                            m_instanced_static_mesh_shader ? m_instanced_static_mesh_shader->Id()
                                                           : 0u,
                            "shader.instanced_static_mesh");
    // Real mesh UVs retain the authored normal-map sampler.
    if (m_instanced_static_mesh_shader && m_instanced_static_mesh_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("gbuffer_static"))
            m_instanced_static_mesh_shader->ValidateLayout(*layout);
    }
    glGenBuffers(1, &m_instanceMatrixVBO);
    PassGl::label_gl_object(GL_BUFFER, m_instanceMatrixVBO, "static_mesh.instance_matrices");
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
    // capacity must cover the largest single (mesh, material) instance group
    // after frustum culling; glBufferSubData does NOT resize, so the draw path
    // also clamps to kStaticInstanceCapacity.
    glBufferData(
        GL_ARRAY_BUFFER, kStaticInstanceCapacity * sizeof(glm::mat4), nullptr, GL_DYNAMIC_DRAW);
    // Per-instance albedo tint VBO (vast-forest genetic/seasonal leaf+bark colour variation).
    glGenBuffers(1, &m_instanceTintVBO);
    PassGl::label_gl_object(GL_BUFFER, m_instanceTintVBO, "static_mesh.instance_tints");
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceTintVBO);
    glBufferData(
        GL_ARRAY_BUFFER, kStaticInstanceCapacity * sizeof(glm::vec3), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    //  far-field tree impostors: a camera-facing billboard (quad corners from gl_VertexID) with a
    // single per-instance vec4 (xyz=tree base pos, w=scale) at location 0, divisor 1. Writes the
    // same G-buffer attachments as g_buffer.frag so impostors light + depth-sort like real tree
    // geometry.
    m_tree_impostor_shader =
        std::make_unique<Shader>((root_path / "res/shaders/tree_impostor.vert").string().c_str(),
                                 (root_path / "res/shaders/tree_impostor.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM,
                            m_tree_impostor_shader ? m_tree_impostor_shader->Id() : 0u,
                            "shader.tree_impostor");
    if (m_tree_impostor_shader && m_tree_impostor_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("gbuffer_impostor"))
            m_tree_impostor_shader->ValidateLayout(*layout);
    }
    glGenBuffers(1, &m_impostorInstanceVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_impostorInstanceVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 kStaticInstanceCapacity * sizeof(ImpostorInstance),
                 nullptr,
                 GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &m_impostorVAO);
    glBindVertexArray(m_impostorVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_impostorInstanceVBO);
    for (GLuint column = 0; column < 5; ++column) {
        glEnableVertexAttribArray(column);
        glVertexAttribPointer(
            column,
            4,
            GL_FLOAT,
            GL_FALSE,
            sizeof(ImpostorInstance),
            reinterpret_cast<void*>(static_cast<size_t>(column) * sizeof(glm::vec4)));
        glVertexAttribDivisor(column, 1);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GBufferPass::init_skinned_mesh(const std::filesystem::path& root_path) {
    std::string skinned_vert_path = (root_path / "res/shaders/skinned_mesh.vert").string();
    std::string gbuffer_frag_path = (root_path / "res/shaders/g_buffer.frag").string();
    m_skinned_mesh_shader =
        std::make_unique<Shader>(skinned_vert_path.c_str(), gbuffer_frag_path.c_str());
    m_skinned_mesh_ladder_shader =
        std::make_unique<Shader>((root_path / "res/shaders/skinned_mesh.vert").string().c_str(),
                                 (root_path / "res/shaders/g_buffer.frag").string().c_str(),
                                 std::vector<std::string>{},
                                 std::vector<std::string>{"LUMINUMBRA_FAR_WORLD_LADDER 1"});
    m_skinned_far_uniforms.Cache(*m_skinned_mesh_shader);
    m_skinned_ladder_uniforms.Cache(*m_skinned_mesh_ladder_shader);
    if (m_skinned_mesh_ladder_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("gbuffer_skinned"))
            m_skinned_mesh_ladder_shader->ValidateLayout(*layout);
    }
    PassGl::label_gl_object(GL_PROGRAM,
                            m_skinned_mesh_shader ? m_skinned_mesh_shader->Id() : 0u,
                            "shader.skinned_mesh");
    // Shares g_buffer.frag with the chunk geometry program -> same sampler layout.
    if (m_skinned_mesh_shader && m_skinned_mesh_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("gbuffer_skinned"))
            m_skinned_mesh_shader->ValidateLayout(*layout);
    }
    glGenBuffers(1, &m_jointPaletteSSBO);
    PassGl::label_gl_object(GL_BUFFER, m_jointPaletteSSBO, "skinned_mesh.joint_palette");
    // 256 joints (the LMS2 skeleton cap) of column-major mat4.
    m_jointPaletteSSBOCapacityBytes = 256u * sizeof(glm::mat4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_jointPaletteSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(m_jointPaletteSSBOCapacityBytes),
                 nullptr,
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void GBufferPass::init_gbuffer(RenderResourceRegistry& registry, u32 width, u32 height) {
    CreateGBufferTargets(m_gbuffer, registry, width, height);
}

void GBufferPass::destroy_gbuffer(RenderResourceRegistry& registry) {
    DestroyGBufferTargets(m_gbuffer, registry);
}

void GBufferPass::destroy_instanced_static_mesh() {
    if (m_instanceMatrixVBO) {
        glDeleteBuffers(1, &m_instanceMatrixVBO);
        m_instanceMatrixVBO = 0;
    }
    if (m_instanceTintVBO) {
        glDeleteBuffers(1, &m_instanceTintVBO);
        m_instanceTintVBO = 0;
    }
}

void GBufferPass::destroy_skinned_mesh() {
    if (m_jointPaletteSSBO) {
        glDeleteBuffers(1, &m_jointPaletteSSBO);
        m_jointPaletteSSBO = 0;
    }
    m_jointPaletteSSBOCapacityBytes = 0;
    m_skinnedMeshCache.clear();
}

void GBufferPass::reset_shaders() {
    m_authored_pass.reset();
    // Detach the generation observers before destroying their Shader owners.
    m_geometry_far_uniforms.Cache(0);
    m_static_far_uniforms.Cache(0);
    m_skinned_far_uniforms.Cache(0);
    m_geometry_ladder_uniforms.Cache(0);
    m_static_ladder_uniforms.Cache(0);
    m_skinned_ladder_uniforms.Cache(0);
    m_geometry_shader.reset();
    m_instanced_static_mesh_shader.reset();
    m_skinned_mesh_shader.reset();
    m_geometry_ladder_shader.reset();
    m_instanced_static_mesh_ladder_shader.reset();
    m_skinned_mesh_ladder_shader.reset();
}

GBufferDrawStats GBufferPass::execute(const RenderContext& ctx,
                                      entt::registry& registry,
                                      const GBufferPassInput& input) {
    LUMIN_PROFILE_ZONE_N("GL GBufferPass.execute");
    // Only diagnostic captures save caller state. Ordinary frames retain the
    // normal pass bindings without capture-only driver queries or restore binds.
    const bool diagnostic_capture = GBufferDiagnosticCapture::Current() != nullptr;
    GLint previous_vao = 0, previous_draw_fbo = 0, previous_read_fbo = 0;
    GLuint previous_gbuffer_fbo = 0;
    GLboolean previous_cull = GL_FALSE;
    if (diagnostic_capture) {
        previous_gbuffer_fbo = m_gbuffer.fbo_id;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vao);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
        previous_cull = glIsEnabled(GL_CULL_FACE);
    }
    ReplayRollback restore_gl([=, this]() noexcept {
        if (!diagnostic_capture) {
            return;
        }
        glBindVertexArray(static_cast<GLuint>(previous_vao));
        // Authored submissions can recreate the targets. Restore the caller's
        // logical G-buffer binding rather than its deleted framebuffer name.
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          previous_gbuffer_fbo != 0 &&
                                  static_cast<GLuint>(previous_draw_fbo) == previous_gbuffer_fbo
                              ? m_gbuffer.fbo_id
                              : static_cast<GLuint>(previous_draw_fbo));
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          previous_gbuffer_fbo != 0 &&
                                  static_cast<GLuint>(previous_read_fbo) == previous_gbuffer_fbo
                              ? m_gbuffer.fbo_id
                              : static_cast<GLuint>(previous_read_fbo));
        if (previous_cull) {
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }
    });
    // ON already populated persistent mesh caches. Keep reused per-dispatch
    // scratch exactly as ON left it, including when a later stage throws.
    auto restore = [this,
                    groups = ctx.frozen_replay ? m_visibleGroups : decltype(m_visibleGroups){},
                    memo = ctx.frozen_replay ? m_resolveMemo : decltype(m_resolveMemo){},
                    impostors =
                        ctx.frozen_replay ? m_impostorInstances : decltype(m_impostorInstances){},
                    replay = ctx.frozen_replay]() mutable noexcept {
        if (replay) {
            m_visibleGroups.swap(groups);
            m_resolveMemo.swap(memo);
            m_impostorInstances.swap(impostors);
        }
    };
    ReplayRollback rollback(std::move(restore));
    GBufferDrawStats stats;
    const bool authored = input.authored_draws && !input.authored_draws->draws.empty();
    if (authored) {
        if (!input.authored_view || input.authored_temporal_enabled ||
            ctx.internal_w() != ctx.screen_width || ctx.internal_h() != ctx.screen_height)
            throw std::invalid_argument(
                "Authored static draws require exact view, scale 1 and TAAU disabled");
        if (ctx.frozen_replay && (!m_authored_pass || !m_gbuffer.authored_texture))
            throw std::logic_error("Replay cannot allocate authored resources");
        if (!m_authored_pass)
            m_authored_pass = std::make_unique<StaticDrawPass>(input.root_path);
        if (!m_gbuffer.authored_texture) {
            DestroyGBufferTargets(m_gbuffer, *ctx.registry);
            CreateGBufferTargets(
                m_gbuffer, *ctx.registry, ctx.internal_w(), ctx.internal_h(), true);
        }
    }
    const Camera& camera = *ctx.camera;

    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    PreserveReplayObject(ReplayObjectKind::Framebuffer, m_gbuffer.fbo_id);
    {
        GLenum buffers[6] = {GL_COLOR_ATTACHMENT0,
                             GL_COLOR_ATTACHMENT1,
                             GL_COLOR_ATTACHMENT2,
                             GL_COLOR_ATTACHMENT3,
                             GL_COLOR_ATTACHMENT4,
                             GL_COLOR_ATTACHMENT5};
        // Plant/decal stages narrow the draw buffers; restore the full set before the clear so
        // attachments 4 (motion) and 5 (authored surface) are cleared and written every frame.
        glDrawBuffers(m_gbuffer.authored_texture ? 6 : 5, buffers);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    GBufferDiagnosticAttachment diagnostic_attachment(m_gbuffer,
                                                      {ctx.internal_w(), ctx.internal_h()});

    DebugViewPass::SurfaceFrame surface_frame(ctx, m_gbuffer);

    //  isolation/layer mode: skip the draws whose layer bit is cleared so an
    // isolated subsystem reads ALONE against the SkyboxPass backdrop. The gbuffer
    // is ALWAYS cleared first, so suppressing a sub-pass just leaves cleared pixels
    // (depth = far plane); the late additive passes still depth-test correctly and
    // the skybox fills the empty pixels with the backdrop. NOT the deferred
    // clear-before-lighting masking the critique rejected — this only OMITS draws.
    // Default config renders == true for every bit, so this is byte-stable off.
    namespace SH = Luminumbra::Client::ScenarioHarness;
    const SH::IsolationConfig& iso = *ctx.isolation;
    // A filtered deferred producer leaves its pixels unattributed whether or not it
    // has scene content, so isolation never certifies complete coverage.
    if (diagnostic_capture &&
        (!iso.renders(SH::IsolationLayer::Terrain) || !iso.renders(SH::IsolationLayer::Skinned))) {
        GBufferDiagnosticCapture::Current()->unsupported("isolation_filtered_deferred_producer");
    }

    // copy ctx.frustum_planes (a const glm::vec4* pointer) into a local
    // array so the terrain-submit callback (which takes a const glm::vec4(&)[6]
    // array reference) and the per-instance cull loops see a real array. Byte-
    // identical to the frustum_planes[6] that render_frame previously passed by value.
    glm::vec4 fp[6];
    for (int i = 0; i < 6; ++i)
        fp[i] = ctx.frustum_planes[i];

    // Pass 1: Render all the terrain chunks (+ far-LOD region meshes). Static
    // props/structures are part of the solid world, so they follow the Terrain bit.
    if (iso.renders(SH::IsolationLayer::Terrain)) {
        geometry_pass_chunks(ctx, input, camera, fp, stats);

        // Pass 2: Render all instanced static meshes
        geometry_pass_static_meshes(ctx, input, registry, camera, fp, stats);
        if (authored) {
            DebugViewPass::unsupported_surface();
            const auto authored_stats =
                m_authored_pass->Render(*input.authored_view, input.authored_draws, m_gbuffer);
            stats.authored_draws += authored_stats.draws;
            stats.authored_indices += authored_stats.indices;
        }
    }

    // Pass 3: non-instanced skinned meshes (CPU-sampled joint
    // palettes from the fixed-tick animation runtime, GPU skinning).
    if (iso.renders(SH::IsolationLayer::Skinned)) {
        geometry_pass_skinned_meshes(ctx, input, registry, camera, fp, stats);
    }

    if (DebugViewPass::geometry_requested() &&
        DebugViewPass::replay_mode(DebugViewPass::geometry_view()) && !diagnostic_capture &&
        !ctx.frozen_replay) {
        // One opt-in replay of participating deferred draws, no clear, no production writes.
        // Keep the normal draw statistics; diagnostics never count as scene geometry.
        const double production_static_cpu_ms = m_last_static_prop_cpu_ms;
        ReplayRollback restore_timing([this, production_static_cpu_ms]() noexcept {
            m_last_static_prop_cpu_ms = production_static_cpu_ms;
        });
        DebugViewPass::SurfaceFrame replay(ctx, m_gbuffer, true);
        if (replay.ready()) {
            GBufferDrawStats replay_stats;
            if (iso.renders(SH::IsolationLayer::Terrain)) {
                geometry_pass_chunks(ctx, input, camera, fp, replay_stats);
                geometry_pass_static_meshes(ctx, input, registry, camera, fp, replay_stats);
                if (authored) {
                    DebugViewPass::unsupported_surface();
                }
            }
            if (iso.renders(SH::IsolationLayer::Skinned)) {
                geometry_pass_skinned_meshes(ctx, input, registry, camera, fp, replay_stats);
            }
        }
    }

    if (!diagnostic_capture) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    return stats;
}

void GBufferPass::geometry_pass_chunks(const RenderContext& ctx,
                                       const GBufferPassInput& input,
                                       const Camera& camera,
                                       const glm::vec4 (&frustum_planes)[6],
                                       GBufferDrawStats& stats) {
    Shader* shader = geometry_shader(input.far_selection.effective).get();
    auto* capture = GBufferDiagnosticCapture::Current();
    FarWorldDiagnosticDraw diagnostic_draw(capture);
    FarWorldPassUniforms diagnostic_uniforms;
    const bool surface = DebugViewPass::geometry_requested();
    if (surface && !capture) {
        shader = DebugViewPass::surface_variant(
            shader, input.far_selection.effective == FarWorldMode::Ladder);
    }
    if (capture) {
        shader = capture->variant(shader, input.far_selection.effective == FarWorldMode::Ladder);
    }
    if (capture || surface) {
        if (shader) {
            diagnostic_uniforms.Cache(*shader);
        }
    }
    auto& uniforms = (capture || surface) ? diagnostic_uniforms
                     : input.far_selection.effective == FarWorldMode::Ladder
                         ? m_geometry_ladder_uniforms
                         : m_geometry_far_uniforms;

    if (!UseDrawProgram(shader, stats))
        return;
    glm::mat4 projection = ctx.gbuffer_projection;
    glm::mat4 view = camera.GetViewMatrix();
    // Ladder writers share the exact projection inverted by the depth readers.
    // Preserve the original projection/jitter arithmetic for compatibility paths.
    const glm::vec2 taau_jit = ctx.taau_jitter_ndc;
    if (!ctx.far_world_ladder) {
        projection = ReversedZPerspective(glm::radians(camera.Zoom),
                                          (float)ctx.screen_width / (float)ctx.screen_height,
                                          camera.GetNearPlane(),
                                          camera.GetFarPlane());
        projection[2][0] += taau_jit.x;
        projection[2][1] += taau_jit.y;
    }

    shader->setMat4("projection", projection);
    shader->setMat4("view", view);
    //  TAAU: previous-frame view-proj + inverse screen size so the fragment shader writes
    // screen-space motion vectors (curr screen pos - reprojected prev pos). Identity prev (frame 0)
    // -> the shader's w<=0 guard yields zero motion. u_jitter_ndc lets the frag remove this frame's
    // jitter from the current position so motion vectors stay jitter-free.
    shader->setMat4("u_prev_view_proj", ctx.prev_view_proj);
    shader->setVec2("u_inv_screen_size",
                    glm::vec2(1.0f / (float)ctx.screen_width, 1.0f / (float)ctx.screen_height));
    shader->setVec2("u_jitter_ndc", taau_jit);
    // LOD mip bias = log2(render_scale) so material textures stay sharp when
    // the scene renders at a reduced internal resolution. internal_w==screen_width at
    // render_scale 1.0 -> exactly log2(1.0)==0.0 -> the shader takes its unbiased path.
    shader->setFloat("u_lodBias",
                     ctx.screen_width > 0u
                         ? std::log2((float)ctx.internal_w() / (float)ctx.screen_width)
                         : 0.0f);
    // View rotation: triplanar normal mapping perturbs the normal in
    // world space then rotates it into view space for the octahedral G-buffer.
    shader->setMat3("u_normalViewMatrix", glm::mat3(view));
    // live terrain is now submitted via glMultiDrawElementsIndirect
    // from the shared geometry pool; the per-draw chunk origin arrives through
    // the instanced aOrigin attribute, so the per-chunk model uniform path is
    // disabled (u_useInstanceOrigin == 1). Far-LOD draws below re-enable the
    // model-uniform path (u_useInstanceOrigin == 0).
    uniforms.Reset();
    uniforms.UseInstanceOrigin(true);
    // Reset above makes the clip band inert for live chunks.

    // Bind material LUT + triplanar terrain arrays for the G-Buffer pass.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.material_lut.id);
    shader->setInt("u_materialLUT", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.terrain_textures.id);
    shader->setInt("u_terrainTextures", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.terrain_normals.id);
    shader->setInt("u_terrainNormals", 2);
    // u_skinnedTextures must point at a distinct unit (3) even though terrain
    // never samples it: a sampler2DArray sharing unit 0 with the sampler2D LUT is
    // undefined (black draws on some drivers). Bind a valid 2D array there.
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  ctx.skinned_textures.id ? ctx.skinned_textures.id : ctx.terrain_textures.id);
    shader->setInt("u_skinnedTextures", 3);
    shader->setInt("u_skinnedAlbedoLayer", -1); // terrain uses triplanar, not UV
    shader->setInt("u_skinnedNormalLayer", -1);
    // terrain PBR roughness-map: per-texel terrain roughness map (unit 4).
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  ctx.terrain_roughness.id ? ctx.terrain_roughness.id : ctx.terrain_textures.id);
    shader->setInt("u_terrainRoughness", 4);
    shader->setInt("u_terrainRoughnessValid", ctx.terrain_roughness_valid);
    shader->setInt("u_macroRockOverlay", 1); // terrain keeps the macro rock overlay

    // Live-terrain submit via the shared callback (wraps
    // CullHierarchical + draw_chunks_mdi exactly, byte-identical). It returns the
    // cull/draw/index counts; we fold them into the returned stats with the EXACT
    // current policy (terrain_visible_chunks '=', draws/indices '+=').
    const TerrainSubmitStats ts = input.submit_terrain_chunks(frustum_planes);
    stats.terrain_visible_chunks = ts.visible_chunks;
    stats.terrain_draws += ts.draws;
    stats.terrain_indices += ts.indices;

    // Far-LOD region meshes AFTER the live chunks: same geometry
    // shader/material LUT (VoxelVertex layout is identical), region-AABB
    // frustum culling, depth-biased so overlapping live terrain wins. Far
    // draws stay inside the gbuffer GPU timer window; they are excluded from
    // the shadow cascades (ShadowPass never sees them).
    // Scope resets the shared program after refusal, draw errors or exceptions.
    FarWorldPassUniformScope ladder_uniform_reset(uniforms);
    const auto far_stats = SubmitSelectedFarWorld(
        input.far_selection,
        [&] {
            FarWorldSubmitStats result;
            if (input.legacy_far)
                input.legacy_far->draw_gbuffer(
                    *shader, view, frustum_planes, result.draws, result.indices);
            return result;
        },
        input.submit_ladder,
        [&uniforms, capture, &diagnostic_draw](const FarWorldDrawCommand& command) {
            if (!uniforms.Prepare(command)) {
                return false;
            }
            // The ladder installs GREATER + writable depth before visiting draws.
            // Reapply the diagnostic depth policy after that installation.
            DebugViewPass::replay_depth_policy();
            if (capture) {
                diagnostic_draw.prepare(command);
            }
            return true;
        });
    if (capture && far_stats.refused) {
        capture->abort();
    }
    stats.far_region_draws += far_stats.draws;
    stats.far_indices += far_stats.indices;
    stats.far_water_indices += far_stats.water_indices;

    glBindVertexArray(0);
}

//  (re)build the cached static-prop instance data. Runs once
// (props are scattered once and never move) and again only if the static-mesh
// population changes. Precomputes the per-instance model matrix + albedo tint +
// base-mesh hash so the per-frame draw path no longer rebuilds them for ALL
// ~84k instances every frame (the measured CPU-submit bottleneck)..
void GBufferPass::build_static_prop_cache(entt::registry& registry) {
    m_staticPropCache.clear();
    m_propMeshPaths.clear();
    m_resolveMemo.clear();
    m_visibleGroups.clear();
    m_anchorChunks.clear();
    m_resolvedAnchorSlots.clear();
    std::unordered_map<std::string, std::uint32_t> pathIntern;
    std::map<std::tuple<int, int, int>, std::uint32_t> anchorSlots;
    auto intern = [&](const std::string& p) -> std::uint32_t {
        auto it = pathIntern.find(p);
        if (it != pathIntern.end())
            return it->second;
        const std::uint32_t idx = static_cast<std::uint32_t>(m_propMeshPaths.size());
        m_propMeshPaths.push_back(p);
        pathIntern.emplace(p, idx);
        return idx;
    };
    auto fnv64 = [](const std::string& s, std::uint64_t seed) {
        std::uint64_t h = seed;
        for (const char character : s) {
            const auto c = static_cast<unsigned char>(character);
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    };
    // Render-only deterministic position hash -> [0,1] (never feeds sim/world_hash).
    auto hash01 = [](const glm::vec3& p, float salt) {
        float s = std::sin(glm::dot(p, glm::vec3(12.9898f, 78.233f, 37.719f)) + salt) * 43758.5453f;
        return s - std::floor(s);
    };
    auto view =
        registry
            .view<const Components::TransformComponent, const Components::StaticMeshComponent>();
    m_staticPropCache.reserve(view.size_hint());
    for (auto entity : view) {
        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& mesh_info = view.get<const Components::StaticMeshComponent>(entity);
        CachedStaticProp cp;
        cp.position = transform.position;
        cp.maxScale = glm::max(glm::max(transform.scale.x, transform.scale.y), transform.scale.z);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position);
        model *= glm::mat4_cast(transform.rotation);
        model = glm::scale(model, transform.scale);
        cp.model = model;
        // Per-instance tint — IDENTICAL to the legacy per-frame computation so the
        // rendered colour is unchanged. LEAF: green->autumn-gold genetic variation;
        // BARK: subtle warm-brown; everything else white (no-op).
        glm::vec3 tint(1.0f);
        const std::string& bp = mesh_info.meshPath;
        auto endsWith = [&](const char* suf, std::size_t n) {
            return bp.size() >= n && bp.compare(bp.size() - n, n, suf) == 0;
        };
        if (endsWith("_leaf", 5)) {
            const float h = hash01(transform.position, 0.0f);
            const float b = 0.70f + 0.30f * hash01(transform.position, 11.3f);
            tint =
                glm::mix(glm::vec3(0.62f, 0.90f, 0.48f), glm::vec3(1.05f, 0.82f, 0.42f), h * h) * b;
        } else if (endsWith("_bark", 5)) {
            const float h = hash01(transform.position, 5.1f);
            tint = glm::vec3(0.82f + 0.32f * h, 0.78f + 0.22f * h, 0.72f + 0.20f * h);
        }
        cp.tint = tint;
        //  impostor classification (once here; the per-frame loop must not string-search the far
        //  field).
        cp.impostorTree = bp.find("tree") != std::string::npos;
        cp.impostorLeaf = cp.impostorTree && (bp.find("leaf") != std::string::npos ||
                                              bp.find("leaves") != std::string::npos);
        cp.terrainAnchored = bp.starts_with("procgen://rock_") || bp.starts_with("procgen://bush_");
        IVec3 anchor_coords{};
        if (cp.terrainAnchored && AnchorChunkCoords(cp.position, anchor_coords)) {
            const auto [slot, inserted] =
                anchorSlots.try_emplace({anchor_coords.x, anchor_coords.y, anchor_coords.z},
                                        static_cast<std::uint32_t>(m_anchorChunks.size()));
            if (inserted) {
                m_anchorChunks.push_back({.coords = anchor_coords,
                                          .chunk = nullptr,
                                          .resolved = false,
                                          .supportOwner = {}});
            }
            cp.anchorSlot = slot->second;
        }
        cp.baseMeshHash = fnv64(mesh_info.meshPath, 1469598103934665603ull);
        cp.pathIndex = intern(mesh_info.meshPath);
        cp.materialId = mesh_info.materialId;
        m_staticPropCache.push_back(cp);
    }
    // The per-frame loop records each resolved slot once, so this never grows there.
    m_resolvedAnchorSlots.reserve(m_anchorChunks.size());
    m_staticPropCachePopulation = registry.view<const Components::StaticMeshComponent>().size();
}

void GBufferPass::geometry_pass_static_meshes(const RenderContext& ctx,
                                              const GBufferPassInput& input,
                                              entt::registry& registry,
                                              const Camera& camera,
                                              const glm::vec4 (&frustum_planes)[6],
                                              GBufferDrawStats& stats) {
    Shader* shader = instanced_static_mesh_shader(input.far_selection.effective).get();
    auto* capture = GBufferDiagnosticCapture::Current();
    FarWorldPassUniforms diagnostic_uniforms;
    const bool surface = DebugViewPass::geometry_requested();
    if (surface && !capture) {
        shader = DebugViewPass::surface_variant(
            shader, input.far_selection.effective == FarWorldMode::Ladder);
    }
    if (capture) {
        shader = capture->variant(shader, input.far_selection.effective == FarWorldMode::Ladder);
    }
    if (capture || surface) {
        if (shader) {
            diagnostic_uniforms.Cache(*shader);
        }
    }
    auto& uniforms = (capture || surface) ? diagnostic_uniforms
                     : input.far_selection.effective == FarWorldMode::Ladder
                         ? m_static_ladder_uniforms
                         : m_static_far_uniforms;

    const auto _sp_t0 = std::chrono::steady_clock::now(); // CPU submit cost
    if (!ctx.frozen_replay)
        m_last_static_prop_cpu_ms = 0.0;
    if (!UseDrawProgram(shader, stats))
        return;
    uniforms.Reset();
    const glm::mat4 static_view = camera.GetViewMatrix();
    const glm::vec2 static_taau_jit = ctx.taau_jitter_ndc; //  TAAU (0 when off)
    glm::mat4 static_proj = ctx.gbuffer_projection;
    if (!ctx.far_world_ladder) {
        static_proj = ReversedZPerspective(glm::radians(camera.Zoom),
                                           (float)ctx.screen_width / (float)ctx.screen_height,
                                           camera.GetNearPlane(),
                                           camera.GetFarPlane());
        static_proj[2][0] += static_taau_jit.x;
        static_proj[2][1] += static_taau_jit.y;
    }
    shader->setMat4("projection", static_proj);
    shader->setMat4("view", static_view);
    shader->setMat4("u_prev_view_proj",
                    ctx.prev_view_proj); //  TAAU motion vectors
    shader->setVec2("u_inv_screen_size",
                    glm::vec2(1.0f / (float)ctx.screen_width, 1.0f / (float)ctx.screen_height));
    shader->setVec2("u_jitter_ndc", static_taau_jit);
    shader->setFloat("u_lodBias", //  render-scale mip bias
                     ctx.screen_width > 0u
                         ? std::log2((float)ctx.internal_w() / (float)ctx.screen_width)
                         : 0.0f);
    shader->setMat3("u_normalViewMatrix", glm::mat3(static_view));
    // Triplanar terrain arrays + LUT: a static mesh tagged with a
    // textured material id (e.g. grass props) reuses the terrain triplanar path.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.material_lut.id);
    shader->setInt("u_materialLUT", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.terrain_textures.id);
    shader->setInt("u_terrainTextures", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.terrain_normals.id);
    shader->setInt("u_terrainNormals", 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  ctx.skinned_textures.id ? ctx.skinned_textures.id : ctx.terrain_textures.id);
    shader->setInt("u_skinnedTextures", 3);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  input.static_model_normal_array ? input.static_model_normal_array
                                                  : ctx.terrain_normals.id);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  input.static_model_surface_array ? input.static_model_surface_array
                                                   : ctx.terrain_textures.id);
    shader->setInt("u_skinnedAlbedoLayer", -1);
    shader->setInt("u_skinnedNormalLayer", -1);
    shader->setInt("u_alphaTest", 0); // Overridden per group below.
    // instanced foliage/props skip the terrain-only macro rock overlay (a per-fragment
    // vnoise + up to 3 triplanar samples) — a leaf/bark/bush card never wants rock texturing,
    // and the forest's heavy overdraw made this branch a top G-buffer cost. Terrain keeps it.
    shader->setInt("u_macroRockOverlay", 0);
    shader->setFloat("u_time",
                     ctx.time_seconds); //  wind (per-frame snapshot)
    //  TAAU: prev-frame wind clock so the vert reconstructs each swayed vertex's PREVIOUS world
    // position -> the G-buffer motion vector tracks wind sway, not just camera motion (no tree-top
    // ghost).
    shader->setFloat("u_prevTime", ctx.prev_time);
    shader->setFloat("u_windStrength", 0.0f); // per-group override below
    // terrain PBR roughness-map: per-texel terrain roughness map (unit 4) — g_buffer.frag is
    // shared, so every program using it must bind a valid array to unit 4.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  ctx.terrain_roughness.id ? ctx.terrain_roughness.id : ctx.terrain_textures.id);
    shader->setInt("u_terrainRoughness", 4);
    shader->setInt("u_terrainRoughnessValid", ctx.terrain_roughness_valid);
    //  refresh the cached prop instance data only when the
    // static-mesh population changes (props are scattered once -> usually a no-op).
    {
        const std::size_t pop = registry.view<const Components::StaticMeshComponent>().size();
        if (pop != m_staticPropCachePopulation) {
            if (ctx.frozen_replay)
                throw std::logic_error("Replay cannot rebuild static residency");
            build_static_prop_cache(registry);
        }
    }
    // groups carry the material id (per-group uniform). tree rendering: each
    // instance picks a LOD bucket from its camera distance (SelectTreeLod) and the
    // group is keyed by the RESOLVED LOD mesh path so distant trees draw a coarser
    // variant; texture/material lookup uses the BASE path so LOD never loses the
    // bark/leaf textures. Authored trees try lower mesh LODs in descending order;
    // other props retain the LOD0 fallback when their selected variant is missing.
    const glm::vec3 cameraPos = camera.Position;
    const bool impostorsOn = input.tree_impostor_enabled;
    TreeLodConfig kTreeLodCfg; // data-driven defaults; render.tree_lod.* may override later.
    // Resolves (loading + caching) the mesh for a candidate path, falling back to
    // the base LOD0 path if the LOD variant cannot be loaded. Returns the path that
    // actually resolved so the group is keyed by what is really drawn.
    auto resolve_mesh = [&](const std::string& candidatePath,
                            const std::string& basePath) -> std::pair<Mesh*, std::string> {
        auto candidate = m_meshCache.find(candidatePath);
        if (candidate == m_meshCache.end()) {
            if (ctx.frozen_replay)
                throw std::logic_error("Replay cannot change mesh residency");
            std::string full = (input.root_path / candidatePath).string();
            // Only load the LOD variant if it exists on disk; an absent.lodN.lmesh caches a
            // null and falls back to LOD0 SILENTLY (no MeshLoader "failed header" error spam).
            candidate =
                m_meshCache
                    .emplace(candidatePath,
                             std::filesystem::exists(full) ? MeshLoader::Load(full) : nullptr)
                    .first;
        }
        Mesh* m = candidate->second.get();
        if (m)
            return {m, candidatePath};
        if (candidatePath != basePath) {
            auto base_entry = m_meshCache.find(basePath);
            if (base_entry == m_meshCache.end()) {
                if (ctx.frozen_replay)
                    throw std::logic_error("Replay cannot change mesh residency");
                std::string full = (input.root_path / basePath).string();
                base_entry = m_meshCache.emplace(basePath, MeshLoader::Load(full)).first;
            }
            Mesh* base = base_entry->second.get();
            if (base)
                return {base, basePath};
        }
        return {nullptr, basePath};
    };
    auto resolve_lod = [&](const CachedStaticProp& cp, bool authoredTree, int lod) {
        const std::uint64_t rkey = cp.baseMeshHash ^ (static_cast<std::uint64_t>(lod) *
                                                      std::uint64_t{0x9E3779B97F4A7C15ull});
        auto rit = m_resolveMemo.find(rkey);
        if (rit == m_resolveMemo.end()) {
            const std::string& basePath = m_propMeshPaths[cp.pathIndex];
            const std::string candidatePath = LodMeshPath(basePath, lod);
            auto resolved = resolve_mesh(candidatePath, candidatePath);
            // At most kTreeLodCount probes for authored trees, only on a memo miss.
            // Cache failed loads too; steady frames do no new probing or allocation.
            if (authoredTree) {
                for (int mesh_lod = lod - 1; !resolved.first && mesh_lod >= 0; --mesh_lod) {
                    const std::string fallbackPath = LodMeshPath(basePath, mesh_lod);
                    resolved = resolve_mesh(fallbackPath, fallbackPath);
                }
            } else if (!resolved.first) {
                resolved = resolve_mesh(basePath, basePath);
            }
            rit = m_resolveMemo
                      .emplace(rkey, std::make_pair(resolved.first, std::move(resolved.second)))
                      .first;
        }
        return rit;
    };
    // Reuse the per-group buffers across frames: keep capacity, clear contents,
    // mark inactive. A group's mesh/drawPath/basePath/material are fixed by its key
    // (rkey encodes baseMeshHash+lod), so the metadata is set once and kept.
    for (auto& kv : m_visibleGroups) {
        kv.second.mats.clear();
        kv.second.tints.clear();
        kv.second.active = false;
    }
    // Per-frame loop over the CACHE (not the registry): distance->LOD->resolve
    // (persistent memo)->frustum cull (per-LOD mesh sphere -> identical visible
    // set)->append the PRECOMPUTED matrix + tint. The old per-frame matrix build +
    // leaf/bark tint hash over all ~84k instances is gone (now done once at cache build).
    //  far-field tree impostors: collected here, drawn after the mesh groups. One billboard per
    // tree (triggered on the leaf part; bark/trunk at LOD3 are folded into the same impostor).
    std::vector<ImpostorInstance>& impostorInstances = m_impostorInstances;
    impostorInstances.clear(); // keep capacity across frames (no per-frame realloc)
    int impostorMatId = 0;
    for (const CachedStaticProp& cp : m_staticPropCache) {
        const float dist = glm::length(cp.position - cameraPos);
        const bool authoredTree =
            m_propMeshPaths[cp.pathIndex].starts_with("game-assets/tree-small-02/");
        int lod =
            SelectTreeLod(dist, authoredTree ? AuthoredTreeLodConfig(impostorsOn) : kTreeLodCfg);
        // This atlas covers only the authored species. All other trees retain mesh LOD2;
        // never fall back to the old LOD3 cross-card when no usable atlas covers them.
        if (cp.impostorTree && !authoredTree) {
            lod = std::min(lod, 2);
        }
        bool impostor_candidate = false;
        if (impostorsOn && authoredTree && lod == 3 && cp.impostorTree) {
            impostor_candidate = true; // a tree part -> impostor replaces it at LOD3
        }
        // A paired far-OFF replay may need meshes even when this frame uses an
        // impostor. Resolve every eligible part before freezing, including failed
        // probes, so replay only consumes the same memoized fallback chain.
        if (impostor_candidate && !ctx.frozen_replay) {
            (void)resolve_lod(cp, authoredTree, 2);
        }
        // Submitted chunks are resident too; culling the ground must not remove
        // support while its canopy is still visible (including the authored LOD3 band).
        const bool live_terrain_owned =
            impostor_candidate &&
            ((input.submitted_live_terrain && input.submitted_live_terrain->covers(cp.position)) ||
             (input.resident_live_terrain && input.resident_live_terrain->covers(cp.position)));
        // Ladder exposes no per-position coverage here; unknown support refuses.
        // Never substitute the inactive Legacy owner for selected Ladder terrain.
        const bool far_terrain_resident =
            impostor_candidate && !live_terrain_owned && input.far_selection.submit &&
            input.far_selection.effective == FarWorldMode::Legacy && input.legacy_far &&
            input.legacy_far->terrain_resident_at(cp.position);
        const auto submission = SelectStaticPropSubmission(
            input.far_selection, impostor_candidate, live_terrain_owned, far_terrain_resident);
        if (submission == StaticPropSubmission::Refused) {
            // Refusing the billboard must not remove its leaf, bark or trunk parts.
            // Rejoin ordinary mesh batching with the same transform/material/attribution.
            lod = 2;
        }
        if (submission == StaticPropSubmission::Impostor) {
            if (cp.impostorLeaf) { // the per-tree representative; bark/trunk are folded into the
                                   // billboard
                const glm::vec3 c =
                    glm::vec3(cp.model * glm::vec4(input.tree_impostor_center, 1.0f));
                const float r = input.tree_impostor_radius * cp.maxScale;
                bool culled = false;
                for (int i = 0; i < 6; i++) {
                    if (glm::dot(glm::vec4(c, 1.0f), frustum_planes[i]) < -r) {
                        culled = true;
                        break;
                    }
                }
                if (!culled) {
                    impostorInstances.push_back({cp.model, glm::vec4(cp.tint, 1.0f)});
                    impostorMatId = static_cast<int>(cp.materialId);
                }
            }
            continue; // all LOD3 tree parts are folded into the billboard
        }
        const auto rit = resolve_lod(cp, authoredTree, lod);
        const std::uint64_t rkey = rit->first;
        Mesh* mesh = rit->second.first;
        if (!mesh)
            continue;
        const glm::vec3 world_sphere_center =
            glm::vec3(cp.model * glm::vec4(glm::vec3(mesh->boundingSphere), 1.0f));
        const float radius = mesh->boundingSphere.w * cp.maxScale;
        bool culled = false;
        for (int i = 0; i < 6; i++) {
            if (glm::dot(glm::vec4(world_sphere_center, 1.0f), frustum_planes[i]) < -radius) {
                culled = true;
                break;
            }
        }
        if (culled)
            continue;
        if (cp.anchorSlot < m_anchorChunks.size() && ctx.world_system) {
            AnchorChunkSlot& slot = m_anchorChunks[cp.anchorSlot];
            if (!slot.resolved) {
                slot.resolved = true;
                m_resolvedAnchorSlots.push_back(cp.anchorSlot);
                auto chunk = ctx.world_system->find_streamed_chunk(slot.coords);
                if (chunk && chunk->has_terrain_authority()) {
                    slot.chunk = std::move(chunk);
                    // Support answers stay valid while the same resident chunk keeps
                    // its voxel revision and SDF storage; any change re-evaluates.
                    const bool same_owner = !slot.supportOwner.owner_before(slot.chunk) &&
                                            !slot.chunk.owner_before(slot.supportOwner);
                    const auto revision = slot.chunk->voxel_revision();
                    const float* const sdf = slot.chunk->sdf_data.data();
                    if (!same_owner || slot.supportRevision != revision || slot.supportSdf != sdf) {
                        slot.supportOwner = slot.chunk;
                        slot.supportRevision = revision;
                        slot.supportSdf = sdf;
                        slot.supportGeneration =
                            slot.supportGeneration == std::numeric_limits<std::uint32_t>::max()
                                ? 1u
                                : slot.supportGeneration + 1u;
                    }
                }
            }
            if (slot.chunk) {
                // At most eight lattice reads per prop per support change, not per frame.
                if (cp.supportGeneration != slot.supportGeneration) {
                    cp.supportRemoved = EditedTerrainRemovesAnchor(slot.chunk.get(), cp.position);
                    cp.supportGeneration = slot.supportGeneration;
                }
                if (cp.supportRemoved) {
                    continue;
                }
            }
        }
        // Group by a cheap uint64 (no per-instance string alloc/copy/compare).
        const std::uint64_t gkey =
            rkey ^ (static_cast<std::uint64_t>(cp.materialId) * std::uint64_t{0x100000001B3ull});
        auto& batch = m_visibleGroups[gkey];
        if (batch.mesh == nullptr) { // first ever sighting fixes the group metadata
            batch.mesh = mesh;
            batch.drawPath = rit->second.second;
            batch.basePath = m_propMeshPaths[cp.pathIndex];
            batch.materialId = cp.materialId;
        }
        batch.active = true;
        batch.mats.push_back(cp.model);
        batch.tints.push_back(cp.tint);
    }
    for (const std::uint32_t index : m_resolvedAnchorSlots) {
        m_anchorChunks[index].chunk.reset();
        m_anchorChunks[index].resolved = false;
    }
    m_resolvedAnchorSlots.clear();
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    for (const auto& kv : m_visibleGroups) {
        const InstanceBatchCached& batch = kv.second;
        if (!batch.active)
            continue;
        const std::vector<glm::mat4>& matrices = batch.mats;
        Mesh* mesh = batch.mesh;
        if (!mesh || matrices.empty())
            continue;
        // Set when the mesh identity cannot be recorded: the draw is still submitted,
        // carrying an unsupported (path-less) identity so the scene is unchanged.
        bool identity_path_unrecordable = false;
        if (capture) {
            if (mesh->indexCount >
                static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
                capture->unsupported("diagnostic_static_mesh_index_count");
                continue;
            }
            // Bound temporary LOD-path copies to the registry's 1,024-byte UTF-8 limit.
            identity_path_unrecordable =
                batch.basePath.size() > 1024 || batch.drawPath.size() > 1024;
        }
        shader->setInt("u_materialId", static_cast<int>(batch.materialId));
        bool two_sided = false;
        //  static-model UV texture lane: if this mesh has registered bark/leaf
        // textures, bind the static-model array to unit 3 and set its albedo/normal
        // layers (+ alpha-test) so g_buffer.frag's UV branch samples the model's own
        // texture by mesh UV instead of the world-projected terrain triplanar.
        // tree rendering: texture lookup uses the BASE path so LOD variants keep textures.
        {
            const auto* smt = input.static_model_tex(batch.basePath);
            glActiveTexture(GL_TEXTURE3);
            if (smt && input.static_model_texture_array != 0) {
                glBindTexture(GL_TEXTURE_2D_ARRAY, input.static_model_texture_array);
                shader->setInt("u_skinnedAlbedoLayer", smt->albedoLayer);
                shader->setInt("u_skinnedNormalLayer", smt->normalLayer);
                shader->setInt("u_alphaTest", smt->alphaTest ? 3 : 0);
                shader->setInt("u_staticNormalsBound", 1);
                shader->setInt("u_staticSurfaceLayer", smt->surfaceLayer);
                two_sided = smt->doubleSided;
                shader->setInt("u_staticDoubleSided", two_sided ? 1 : 0);
                shader->setFloat("u_staticMetallicFactor", smt->metallicFactor);
                // textured tree parts sway in the wind (rigid props stay at 0).
                shader->setFloat("u_windStrength", 1.0f);
                shader->setInt("u_forceFlat", 0);
                shader->setInt("u_leafSurface", 0);
            } else {
                glBindTexture(GL_TEXTURE_2D_ARRAY,
                              ctx.skinned_textures.id ? ctx.skinned_textures.id
                                                      : ctx.terrain_textures.id);
                shader->setInt("u_skinnedAlbedoLayer", -1);
                shader->setInt("u_skinnedNormalLayer", -1);
                shader->setInt("u_staticNormalsBound", 0);
                shader->setInt("u_staticDoubleSided", 0);
                shader->setInt("u_staticSurfaceLayer", -1);
                shader->setInt("u_alphaTest", 0);
                // VAST-FOREST: the procedural LEAF submeshes (no texture lane) still sway in the
                // wind (height-scaled); bark/trunk + rigid props stay at 0. Keyed on the palette
                // key suffix so only procgen leaves flutter.
                const bool isLeaf = batch.basePath.size() >= 5 &&
                                    batch.basePath.rfind("_leaf") == batch.basePath.size() - 5;
                const bool isBush = batch.basePath.starts_with("procgen://bush_");
                const bool isFarLod = batch.drawPath.find(".lod") != std::string::npos;
                const bool leafCards = isLeaf || (isBush && !isFarLod);
                shader->setInt("u_alphaTest", leafCards ? 2 : 0);
                two_sided = leafCards;
                // Near bush leaf cards sway like procgen leaves; far hulls and bark stay rigid.
                shader->setFloat("u_windStrength", leafCards ? 0.85f : 0.0f);
                // Shrubs are foliage at every distance, never opaque grass-textured stone.
                // Distant shrubs keep the closed hull; only their near mesh uses leaf cards.
                shader->setInt("u_forceFlat", (isBush || (isLeaf && isFarLod)) ? 1 : 0);
                // Near cards and distant hulls share the leaf reflectance, not grass.
                shader->setInt("u_leafSurface", isBush ? 1 : 0);
            }
        }
        if (two_sided || !cull_was_enabled)
            glDisable(GL_CULL_FACE);
        else
            glEnable(GL_CULL_FACE);
        // Submit every instance in bounded batches; large stands can exceed one VBO.
        for (size_t offset = 0; offset < matrices.size(); offset += kStaticInstanceCapacity) {
            const GLsizei instance_count = static_cast<GLsizei>(std::min<std::size_t>(
                matrices.size() - offset, static_cast<std::size_t>(kStaticInstanceCapacity)));
            glBindVertexArray(mesh->vao);
            PreserveReplayObject(ReplayObjectKind::VertexArray, mesh->vao);
            glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
            glBufferSubData(GL_ARRAY_BUFFER,
                            0,
                            static_cast<GLsizeiptr>(static_cast<std::size_t>(instance_count) *
                                                    sizeof(glm::mat4)),
                            matrices.data() + offset);
            for (GLuint i = 0; i < 4; ++i) {
                glEnableVertexAttribArray(3 + i);
                glVertexAttribPointer(3 + i,
                                      4,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      sizeof(glm::mat4),
                                      (void*)(sizeof(glm::vec4) * i));
                glVertexAttribDivisor(3 + i, 1);
            }
            // Per-instance albedo tint at location 7 (divisor 1).
            glBindBuffer(GL_ARRAY_BUFFER, m_instanceTintVBO);
            glBufferSubData(GL_ARRAY_BUFFER,
                            0,
                            static_cast<GLsizeiptr>(static_cast<std::size_t>(instance_count) *
                                                    sizeof(glm::vec3)),
                            batch.tints.data() + offset);
            glEnableVertexAttribArray(7);
            glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)nullptr);
            glVertexAttribDivisor(7, 1);
            if (capture) {
                std::uint32_t resolved_lod = 0;
                for (int level = 1; !identity_path_unrecordable && level < kTreeLodCount; ++level) {
                    if (batch.drawPath == LodMeshPath(batch.basePath, level)) {
                        resolved_lod = static_cast<std::uint32_t>(level);
                        break;
                    }
                }
                const bool generated = batch.drawPath.starts_with("procgen://");
                const bool unattributed = identity_path_unrecordable;
                const std::string generated_key = generated && !identity_path_unrecordable
                                                      ? "procedural/" + batch.drawPath.substr(10)
                                                      : std::string{};
                const std::string_view variant =
                    identity_path_unrecordable ? "static_mesh_identity_path_limit"
                    : input.far_selection.effective == FarWorldMode::Ladder
                        ? "instanced_static_mesh_ladder"
                        : "instanced_static_mesh";
                for (GLsizei instance = 0; instance < instance_count; ++instance) {
                    capture->write(*shader,
                                   {.mesh_path = unattributed || generated ? std::string_view{}
                                                                           : batch.drawPath,
                                    .shader_variant = variant,
                                    .chunk = {},
                                    .lod = resolved_lod,
                                    .ownership = DiagnosticOwnership::Live,
                                    .pass = DiagnosticPass::GBuffer,
                                    .provenance = unattributed ? DiagnosticProvenance::Unsupported
                                                               : DiagnosticProvenance::Mesh,
                                    .generated_mesh = generated_key});
                    Luminumbra::Rendering::AttributedGl::Submit(
                        glDrawElementsInstancedBaseInstance,
                        GLenum{GL_TRIANGLES},
                        static_cast<GLsizei>(mesh->indexCount),
                        GLenum{GL_UNSIGNED_INT},
                        nullptr,
                        1,
                        static_cast<GLuint>(instance));
                }
            } else {
                Luminumbra::Rendering::AttributedGl::Submit(glDrawElementsInstanced,
                                                            GLenum{GL_TRIANGLES},
                                                            static_cast<GLsizei>(mesh->indexCount),
                                                            GLenum{GL_UNSIGNED_INT},
                                                            nullptr,
                                                            instance_count);
            }
            glBindVertexArray(0);
        }
    }
    if (cull_was_enabled)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);

    //  far-field tree impostors: one camera-facing billboard per collected far tree, sampling the
    // octahedral atlas into the same G-buffer attachments. ONE instanced draw + one shared atlas
    // for the whole far field (the draw-call/triangle collapse the forest_perf_budget gate
    // targets).
    if (impostorsOn && !impostorInstances.empty()) {
        // Queued geometry is unsupported even when its program prevents submission.
        if (capture) {
            capture->unsupported("tree_impostor_fragment_output_unavailable", 3);
        }
        DebugViewPass::unsupported_surface();
        if (!UseDrawProgram(m_tree_impostor_shader.get(), stats)) {
            return;
        }
        m_tree_impostor_shader->setMat4("u_view", static_view);
        m_tree_impostor_shader->setMat4("u_proj", static_proj);
        m_tree_impostor_shader->setVec3("u_cameraPos", cameraPos);
        m_tree_impostor_shader->setFloat("u_radius", input.tree_impostor_radius);
        m_tree_impostor_shader->setVec3("u_center", input.tree_impostor_center);
        m_tree_impostor_shader->setFloat("u_grid", static_cast<float>(input.tree_impostor_grid));
        m_tree_impostor_shader->setFloat("u_materialId",
                                         static_cast<float>(impostorMatId) / 255.0f);
        m_tree_impostor_shader->setInt("u_albedo", 0);
        m_tree_impostor_shader->setInt("u_normal", 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, input.tree_impostor_albedo);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, input.tree_impostor_normal);
        for (size_t offset = 0; offset < impostorInstances.size();
             offset += kStaticInstanceCapacity) {
            const GLsizei count = static_cast<GLsizei>(
                std::min<size_t>(impostorInstances.size() - offset, kStaticInstanceCapacity));
            glBindBuffer(GL_ARRAY_BUFFER, m_impostorInstanceVBO);
            glBufferSubData(
                GL_ARRAY_BUFFER,
                0,
                static_cast<GLsizeiptr>(static_cast<std::size_t>(count) * sizeof(ImpostorInstance)),
                impostorInstances.data() + offset);
            glBindVertexArray(m_impostorVAO);
            glDisable(GL_CULL_FACE);
            Luminumbra::Rendering::AttributedGl::Submit(
                glDrawArraysInstanced, GLenum{GL_TRIANGLE_STRIP}, 0, 4, count);
            glBindVertexArray(0);
        }
        if (cull_was_enabled)
            glEnable(GL_CULL_FACE);
    }

    if (!ctx.frozen_replay)
        m_last_static_prop_cpu_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _sp_t0)
                .count();
}

void GBufferPass::geometry_pass_skinned_meshes(const RenderContext& ctx,
                                               const GBufferPassInput& input,
                                               entt::registry& registry,
                                               const Camera& camera,
                                               const glm::vec4 (&frustum_planes)[6],
                                               GBufferDrawStats& stats) {
    Shader* shader = skinned_mesh_shader(input.far_selection.effective).get();
    auto* capture = GBufferDiagnosticCapture::Current();
    FarWorldPassUniforms diagnostic_uniforms;
    const bool surface = DebugViewPass::geometry_requested();
    if (surface && !capture) {
        shader = DebugViewPass::surface_variant(
            shader, input.far_selection.effective == FarWorldMode::Ladder);
    }
    if (capture) {
        shader = capture->variant(shader, input.far_selection.effective == FarWorldMode::Ladder);
    }
    if (capture || surface) {
        if (shader) {
            diagnostic_uniforms.Cache(*shader);
        }
    }
    auto& uniforms = (capture || surface) ? diagnostic_uniforms
                     : input.far_selection.effective == FarWorldMode::Ladder
                         ? m_skinned_ladder_uniforms
                         : m_skinned_far_uniforms;

    namespace anim = luminumbra::animation;
    auto view = registry.view<const Components::TransformComponent,
                              const Components::SkinnedMeshComponent,
                              const anim::AnimationPlayerComponent>();
    if (view.begin() == view.end()) {
        return;
    }
    if (!UseDrawProgram(shader, stats)) {
        return;
    }
    uniforms.Reset();
    const glm::mat4 skinned_view = camera.GetViewMatrix();
    const glm::vec2 skinned_taau_jit = ctx.taau_jitter_ndc; //  TAAU (0 when off)
    glm::mat4 skinned_proj = ctx.gbuffer_projection;
    if (!ctx.far_world_ladder) {
        skinned_proj = ReversedZPerspective(glm::radians(camera.Zoom),
                                            (float)ctx.screen_width / (float)ctx.screen_height,
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
        skinned_proj[2][0] += skinned_taau_jit.x;
        skinned_proj[2][1] += skinned_taau_jit.y;
    }
    shader->setMat4("projection", skinned_proj);
    shader->setMat4("view", skinned_view);
    shader->setMat4("u_prev_view_proj", ctx.prev_view_proj); //  TAAU motion vectors
    shader->setVec2("u_inv_screen_size",
                    glm::vec2(1.0f / (float)ctx.screen_width, 1.0f / (float)ctx.screen_height));
    shader->setVec2("u_jitter_ndc", skinned_taau_jit);
    shader->setFloat("u_lodBias", //  render-scale mip bias
                     ctx.screen_width > 0u
                         ? std::log2((float)ctx.internal_w() / (float)ctx.screen_width)
                         : 0.0f);
    shader->setMat3("u_normalViewMatrix", glm::mat3(skinned_view));
    // Triplanar terrain arrays + LUT.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.material_lut.id);
    shader->setInt("u_materialLUT", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.terrain_textures.id);
    shader->setInt("u_terrainTextures", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.terrain_normals.id);
    shader->setInt("u_terrainNormals", 2);
    // UV-mapped skinned-mesh texture array on unit 3. Skinned creatures
    // take the UV-sampled path (precedence over the terrain LUT triplanar path).
    glActiveTexture(GL_TEXTURE3);
    // Match the terrain/static paths: when no skinned texture array is loaded,
    // bind the terrain array as a valid fallback so unit 3's sampler2DArray is
    // never left referencing an empty unit (GL_INVALID_OPERATION "program
    // texture usage" otherwise — fires once per skinned draw). The albedo/normal
    // layers below resolve to -1 in that case, so the fallback is never sampled.
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  ctx.skinned_textures.id ? ctx.skinned_textures.id : ctx.terrain_textures.id);
    shader->setInt("u_skinnedTextures", 3);
    shader->setInt("u_skinnedAlbedoLayer", ctx.skinned_textures.id ? ctx.skinned_albedo_layer : -1);
    shader->setInt("u_skinnedNormalLayer", ctx.skinned_textures.id ? ctx.skinned_normal_layer : -1);
    // terrain PBR roughness-map: per-texel terrain roughness map (unit 4) — shared g_buffer.frag.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  ctx.terrain_roughness.id ? ctx.terrain_roughness.id : ctx.terrain_textures.id);
    shader->setInt("u_terrainRoughness", 4);
    shader->setInt("u_terrainRoughnessValid", ctx.terrain_roughness_valid);

    for (auto entity : view) {
        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& mesh_info = view.get<const Components::SkinnedMeshComponent>(entity);
        auto const& player = view.get<const anim::AnimationPlayerComponent>(entity);

        auto mesh_entry = m_skinnedMeshCache.find(mesh_info.meshPath);
        if (mesh_entry == m_skinnedMeshCache.end()) {
            if (ctx.frozen_replay)
                throw std::logic_error("Replay cannot change skinned residency");
            std::string full_mesh_path = (input.root_path / mesh_info.meshPath).string();
            mesh_entry = m_skinnedMeshCache
                             .emplace(mesh_info.meshPath, MeshLoader::LoadSkinned(full_mesh_path))
                             .first;
        }
        Mesh* mesh = mesh_entry->second.get();
        if (!mesh || mesh->jointCount == 0)
            continue;

        // The  runtime emits 16 floats per joint; a palette that does
        // not match the mesh skeleton is a wiring bug, skip the draw.
        const std::size_t expected_floats = static_cast<std::size_t>(mesh->jointCount) * 16u;
        if (player.palette.size() != expected_floats)
            continue;

        // Sphere culling: bounds are bind-pose, padded for animation sway.
        glm::vec3 world_sphere_center = transform.position + glm::vec3(mesh->boundingSphere);
        float radius = 1.5f * mesh->boundingSphere.w *
                       glm::max(glm::max(transform.scale.x, transform.scale.y), transform.scale.z);
        bool culled = false;
        for (int i = 0; i < 6; i++) {
            if (glm::dot(glm::vec4(world_sphere_center, 1.0f), frustum_planes[i]) < -radius) {
                culled = true;
                break;
            }
        }
        if (culled)
            continue;

        glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position);
        model *= glm::mat4_cast(transform.rotation);
        model = glm::scale(model, transform.scale);
        shader->setMat4("model", model);
        shader->setInt("u_materialId", static_cast<int>(mesh_info.materialId));
        //  per-creature albedo tint from the species base_color (white = no-op).
        shader->setVec3("u_albedo_tint",
                        glm::vec3(mesh_info.tintR, mesh_info.tintG, mesh_info.tintB));

        const std::size_t palette_bytes = player.palette.size() * sizeof(float);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_jointPaletteSSBO);
        if (palette_bytes > m_jointPaletteSSBOCapacityBytes) {
            if (ctx.frozen_replay)
                throw std::logic_error("Replay cannot grow joint storage");
            m_jointPaletteSSBOCapacityBytes = palette_bytes;
            glBufferData(GL_SHADER_STORAGE_BUFFER,
                         static_cast<GLsizeiptr>(palette_bytes),
                         player.palette.data(),
                         GL_DYNAMIC_DRAW);
        } else {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                            0,
                            static_cast<GLsizeiptr>(palette_bytes),
                            player.palette.data());
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_jointPaletteSSBO);

        glBindVertexArray(mesh->vao);
        if (capture) {
            capture->write(*shader,
                           {.mesh_path = mesh_info.meshPath,
                            .shader_variant = input.far_selection.effective == FarWorldMode::Ladder
                                                  ? "skinned_mesh_ladder"
                                                  : "skinned_mesh",
                            .chunk = {},
                            .lod = 0,
                            .ownership = DiagnosticOwnership::Live,
                            .pass = DiagnosticPass::GBuffer,
                            .provenance = DiagnosticProvenance::Mesh});
        }
        Luminumbra::Rendering::AttributedGl::Submit(glDrawElements,
                                                    GLenum{GL_TRIANGLES},
                                                    static_cast<GLsizei>(mesh->indexCount),
                                                    GLenum{GL_UNSIGNED_INT},
                                                    nullptr);
        glBindVertexArray(0);
        stats.skinned_draws++;
        stats.skinned_indices += mesh->indexCount;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

} // namespace Luminumbra::Rendering
