#include "GBufferPass.h"

#include "PassGlHelpers.h"
#include "../FarLodSystem.h"
#include "../TreeLod.h" // Track-B: per-instance distance LOD mesh selection (render-only)
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include "rendering/Mesh.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/world/Chunk.h"

#include <cmath> // std::sin/std::floor for the render-only per-instance tint hash
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono> // spec 004: CPU submit cost of the static-prop pass
#include <cstddef>
#include <string>
#include <utility> // std::pair for the LOD-resolve helper return
#include <filesystem> // existence-probe LOD variants before loading (avoid load-error spam)

#include <GLFW/glfw3.h> // I8: glfwGetTime() for render-only tree wind animation

#include "luminumbra_common/animation/AnimationRuntime.h"

namespace Luminumbra::Rendering {

// I8: capacity of the shared per-(mesh,material) instance-matrix VBO. The draw
// path clamps uploads + draw counts to this so a group larger than capacity can
// never run glBufferSubData past the buffer end (GL_INVALID_VALUE -> garbage /
// dropped instances). Must exceed the largest single instance group.
//
// spec 004 FR-R1: the old 16384 cap SILENTLY CLAMPED below the dense forest's
// per-group visible count (~28k tree parts / ~40k bushes -> dropped props on the
// budget-dense pose). Sized to cover the full scattered set in a single group
// (worst case ~84k); the GPU-driven path (Phase 2) replaces this with a
// per-instance SSBO sized for the total set + growth.
static constexpr GLsizei kStaticInstanceCapacity = 131072;

GBufferPass::GBufferPass() = default;
GBufferPass::~GBufferPass() = default;

void GBufferPass::register_cached_mesh(const std::string& key, std::unique_ptr<Mesh> mesh) {
    if (mesh) m_meshCache[key] = std::move(mesh);
}
bool GBufferPass::has_cached_mesh(const std::string& key) const {
    return m_meshCache.find(key) != m_meshCache.end();
}

void GBufferPass::init_geometry_shader(const std::filesystem::path& root_path) {
    m_geometry_shader = std::make_unique<Shader>((root_path / "res/shaders/g_buffer.vert").string().c_str(), (root_path / "res/shaders/g_buffer.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_geometry_shader ? m_geometry_shader->Id() : 0u, "shader.geometry");
}

void GBufferPass::init_instanced_static_mesh(const std::filesystem::path& root_path) {
    std::string instanced_vert_path = (root_path / "res/shaders/instanced_mesh.vert").string();
    std::string gbuffer_frag_path = (root_path / "res/shaders/g_buffer.frag").string();
    m_instanced_static_mesh_shader = std::make_unique<Shader>(instanced_vert_path.c_str(), gbuffer_frag_path.c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_instanced_static_mesh_shader ? m_instanced_static_mesh_shader->Id() : 0u, "shader.instanced_static_mesh");
    glGenBuffers(1, &m_instanceMatrixVBO);
    PassGl::label_gl_object(GL_BUFFER, m_instanceMatrixVBO, "static_mesh.instance_matrices");
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
    // I8: capacity must cover the largest single (mesh, material) instance group
    // after frustum culling; glBufferSubData does NOT resize, so the draw path
    // also clamps to kStaticInstanceCapacity.
    glBufferData(GL_ARRAY_BUFFER, kStaticInstanceCapacity * sizeof(glm::mat4), nullptr, GL_DYNAMIC_DRAW);
    // Per-instance albedo tint VBO (vast-forest genetic/seasonal leaf+bark colour variation).
    glGenBuffers(1, &m_instanceTintVBO);
    PassGl::label_gl_object(GL_BUFFER, m_instanceTintVBO, "static_mesh.instance_tints");
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceTintVBO);
    glBufferData(GL_ARRAY_BUFFER, kStaticInstanceCapacity * sizeof(glm::vec3), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Wave-3 far-field tree impostors: a camera-facing billboard (quad corners from gl_VertexID) with a
    // single per-instance vec4 (xyz=tree base pos, w=scale) at location 0, divisor 1. Writes the same
    // G-buffer attachments as g_buffer.frag so impostors light + depth-sort like real tree geometry.
    m_tree_impostor_shader = std::make_unique<Shader>(
        (root_path / "res/shaders/tree_impostor.vert").string().c_str(),
        (root_path / "res/shaders/tree_impostor.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_tree_impostor_shader ? m_tree_impostor_shader->Id() : 0u, "shader.tree_impostor");
    glGenBuffers(1, &m_impostorInstanceVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_impostorInstanceVBO);
    glBufferData(GL_ARRAY_BUFFER, kStaticInstanceCapacity * sizeof(glm::vec4), nullptr, GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &m_impostorVAO);
    glBindVertexArray(m_impostorVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_impostorInstanceVBO);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void*)0);
    glVertexAttribDivisor(0, 1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GBufferPass::init_skinned_mesh(const std::filesystem::path& root_path) {
    std::string skinned_vert_path = (root_path / "res/shaders/skinned_mesh.vert").string();
    std::string gbuffer_frag_path = (root_path / "res/shaders/g_buffer.frag").string();
    m_skinned_mesh_shader = std::make_unique<Shader>(skinned_vert_path.c_str(), gbuffer_frag_path.c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_skinned_mesh_shader ? m_skinned_mesh_shader->Id() : 0u, "shader.skinned_mesh");
    glGenBuffers(1, &m_jointPaletteSSBO);
    PassGl::label_gl_object(GL_BUFFER, m_jointPaletteSSBO, "skinned_mesh.joint_palette");
    // 256 joints (the LMS2 skeleton cap) of column-major mat4.
    m_jointPaletteSSBOCapacityBytes = 256u * sizeof(glm::mat4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_jointPaletteSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(m_jointPaletteSSBOCapacityBytes), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void GBufferPass::init_gbuffer(u32 width, u32 height) {
    glGenFramebuffers(1, &m_gbuffer.fbo_id);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_gbuffer.fbo_id, "gbuffer.fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);

    // Position: full view-space position for deferred lighting, SSAO, and material projection.
    glGenTextures(1, &m_gbuffer.position_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.position_texture, "gbuffer.position");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gbuffer.position_texture, 0);

    // Normal/Material: RGBA8 (4 bytes/pixel -> octahedral normal + material ID)
    glGenTextures(1, &m_gbuffer.normal_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.normal_texture, "gbuffer.normal_material");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gbuffer.normal_texture, 0);

    // Albedo/Roughness: RGBA8 (4 bytes/pixel -> RGB albedo + roughness)
    glGenTextures(1, &m_gbuffer.albedo_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.albedo_texture, "gbuffer.albedo_roughness");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.albedo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_gbuffer.albedo_texture, 0);

    // Metallic/AO: RG16F (4 bytes/pixel -> metallic + ambient occlusion)
    glGenTextures(1, &m_gbuffer.material_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.material_texture, "gbuffer.metallic_ao");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.material_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, width, height, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, m_gbuffer.material_texture, 0);

    // Motion vectors: RG16F (signed NDC delta = current - previous screen position).
    // spec 004 FR-R5 (TAAU) foundation; spec 007 particle Phase 3 writes velocity here.
    glGenTextures(1, &m_gbuffer.motion_vector_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.motion_vector_texture, "gbuffer.motion_vectors");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.motion_vector_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, width, height, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, m_gbuffer.motion_vector_texture, 0);

    const GLenum attachments[5] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4 };
    glDrawBuffers(5, attachments);

    // Depth texture (unchanged)
    glGenTextures(1, &m_gbuffer.depth_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.depth_texture, "gbuffer.depth");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_gbuffer.depth_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LUMINUMBRA_CORE_ERROR("G-Buffer FBO not complete!");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GBufferPass::destroy_gbuffer() {
    if (m_gbuffer.fbo_id) { glDeleteFramebuffers(1, &m_gbuffer.fbo_id); m_gbuffer.fbo_id = 0; }
    if (m_gbuffer.position_texture) { glDeleteTextures(1, &m_gbuffer.position_texture); m_gbuffer.position_texture = 0; }
    if (m_gbuffer.normal_texture) { glDeleteTextures(1, &m_gbuffer.normal_texture); m_gbuffer.normal_texture = 0; }
    if (m_gbuffer.albedo_texture) { glDeleteTextures(1, &m_gbuffer.albedo_texture); m_gbuffer.albedo_texture = 0; }
    if (m_gbuffer.material_texture) { glDeleteTextures(1, &m_gbuffer.material_texture); m_gbuffer.material_texture = 0; }
    if (m_gbuffer.motion_vector_texture) { glDeleteTextures(1, &m_gbuffer.motion_vector_texture); m_gbuffer.motion_vector_texture = 0; }
    if (m_gbuffer.depth_texture) { glDeleteTextures(1, &m_gbuffer.depth_texture); m_gbuffer.depth_texture = 0; }
}

void GBufferPass::destroy_instanced_static_mesh() {
    if (m_instanceMatrixVBO) { glDeleteBuffers(1, &m_instanceMatrixVBO); m_instanceMatrixVBO = 0; }
    if (m_instanceTintVBO) { glDeleteBuffers(1, &m_instanceTintVBO); m_instanceTintVBO = 0; }
}

void GBufferPass::destroy_skinned_mesh() {
    if (m_jointPaletteSSBO) { glDeleteBuffers(1, &m_jointPaletteSSBO); m_jointPaletteSSBO = 0; }
    m_jointPaletteSSBOCapacityBytes = 0;
    m_skinnedMeshCache.clear();
}

void GBufferPass::reset_shaders() {
    m_geometry_shader.reset();
    m_instanced_static_mesh_shader.reset();
    m_skinned_mesh_shader.reset();
}

void GBufferPass::execute(RenderPipeline& pipeline,
                          entt::registry& registry,
                          const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                          const Camera& camera,
                          const glm::vec4 frustum_planes[6]) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // T-I6 isolation/layer mode: skip the draws whose layer bit is cleared so an
    // isolated subsystem reads ALONE against the SkyboxPass backdrop. The gbuffer
    // is ALWAYS cleared first, so suppressing a sub-pass just leaves cleared pixels
    // (depth = far plane); the late additive passes still depth-test correctly and
    // the skybox fills the empty pixels with the backdrop. NOT the deferred
    // clear-before-lighting masking the critique rejected — this only OMITS draws.
    // Default config renders() == true for every bit, so this is byte-stable off.
    namespace SH = Luminumbra::Client::ScenarioHarness;
    const SH::IsolationConfig& iso = pipeline.isolation_config();

    // Pass 1: Render all the terrain chunks (+ far-LOD region meshes). Static
    // props/structures are part of the solid world, so they follow the Terrain bit.
    if (iso.renders(SH::IsolationLayer::Terrain)) {
        geometry_pass_chunks(pipeline, renderable_chunks, camera, frustum_planes);

        // Pass 2: Render all instanced static meshes
        geometry_pass_static_meshes(pipeline, registry, camera, frustum_planes);
    }

    // Pass 3 (T-I3-16): non-instanced skinned meshes (CPU-sampled joint
    // palettes from the fixed-tick animation runtime, GPU skinning).
    if (iso.renders(SH::IsolationLayer::Skinned)) {
        geometry_pass_skinned_meshes(pipeline, registry, camera, frustum_planes);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GBufferPass::geometry_pass_chunks(RenderPipeline& pipeline,
                                       const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                                       const Camera& camera,
                                       const glm::vec4 frustum_planes[6]) {
    m_geometry_shader->use();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();
    // FR-R5 TAAU: sub-pixel jitter the projection (0 when TAAU off -> byte-identical).
    const glm::vec2 taau_jit = pipeline.taau_jitter_ndc();
    projection[2][0] += taau_jit.x;
    projection[2][1] += taau_jit.y;

    m_geometry_shader->setMat4("projection", projection);
    m_geometry_shader->setMat4("view", view);
    // FR-R5 TAAU: previous-frame view-proj + inverse screen size so the fragment shader writes
    // screen-space motion vectors (curr screen pos - reprojected prev pos). Identity prev (frame 0)
    // -> the shader's w<=0 guard yields zero motion. u_jitter_ndc lets the frag remove this frame's
    // jitter from the current position so motion vectors stay jitter-free.
    m_geometry_shader->setMat4("u_prev_view_proj", pipeline.prev_view_proj());
    m_geometry_shader->setVec2("u_inv_screen_size",
        glm::vec2(1.0f / (float)pipeline.m_screen_width, 1.0f / (float)pipeline.m_screen_height));
    m_geometry_shader->setVec2("u_jitter_ndc", taau_jit);
    // View rotation: triplanar normal mapping (T-I4-7) perturbs the normal in
    // world space then rotates it into view space for the octahedral G-buffer.
    m_geometry_shader->setMat3("u_normalViewMatrix", glm::mat3(view));
    // T-I4-16: live terrain is now submitted via glMultiDrawElementsIndirect
    // from the shared geometry pool; the per-draw chunk origin arrives through
    // the instanced aOrigin attribute, so the per-chunk model uniform path is
    // disabled (u_useInstanceOrigin == 1). Far-LOD draws below re-enable the
    // model-uniform path (u_useInstanceOrigin == 0).
    m_geometry_shader->setInt("u_useInstanceOrigin", 1);
    // Clip band is inert for live chunks (matches the legacy per-chunk path).
    m_geometry_shader->setFloat("u_farClipNearRadius", 0.0f);
    m_geometry_shader->setFloat("u_farClipFarRadius", 0.0f);

    // Bind material LUT + triplanar terrain arrays for the G-Buffer pass.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pipeline.m_materialLUT);
    m_geometry_shader->setInt("u_materialLUT", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainTextureArray);
    m_geometry_shader->setInt("u_terrainTextures", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainNormalArray);
    m_geometry_shader->setInt("u_terrainNormals", 2);
    // u_skinnedTextures must point at a distinct unit (3) even though terrain
    // never samples it: a sampler2DArray sharing unit 0 with the sampler2D LUT is
    // undefined (black draws on some drivers). Bind a valid 2D array there.
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_skinnedTextureArray ? pipeline.m_skinnedTextureArray : pipeline.m_terrainTextureArray);
    m_geometry_shader->setInt("u_skinnedTextures", 3);
    m_geometry_shader->setInt("u_skinnedAlbedoLayer", -1); // terrain uses triplanar, not UV
    m_geometry_shader->setInt("u_skinnedNormalLayer", -1);
    // I7.1-PBR B1d: per-texel terrain roughness map (unit 4).
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainRoughnessArray ? pipeline.m_terrainRoughnessArray : pipeline.m_terrainTextureArray);
    m_geometry_shader->setInt("u_terrainRoughness", 4);
    m_geometry_shader->setInt("u_terrainRoughnessValid", pipeline.m_terrainRoughnessValid);
    m_geometry_shader->setInt("u_macroRockOverlay", 1); // FR-C2: terrain keeps the macro rock overlay

    // Perform hierarchical frustum culling
    std::vector<const RenderPipeline::ChunkCullEntry*> visible_chunks;
    visible_chunks.reserve(renderable_chunks.size());
    pipeline.m_hierarchicalCuller.CullHierarchical(frustum_planes, visible_chunks);
    pipeline.m_last_render_pass_stats.terrain_visible_chunks = visible_chunks.size();

    // T-I4-16: ONE (per-bucket) glMultiDrawElementsIndirect over the shared
    // geometry pool replaces the per-chunk glDrawElements loop. The chunk world
    // origin reaches g_buffer.vert via the instanced aOrigin attribute; the
    // pool VAOs carry the VoxelVertex layout, so no per-draw VAO/uniform binds.
    std::size_t terrain_draws = 0;
    std::size_t terrain_indices = 0;
    pipeline.draw_chunks_mdi(visible_chunks, terrain_draws, terrain_indices);
    pipeline.m_last_render_pass_stats.terrain_draws += terrain_draws;
    pipeline.m_last_render_pass_stats.terrain_indices_drawn += terrain_indices;

    // Far-LOD path uses the per-region model uniform: switch the shader back to
    // the model-uniform origin path before it runs (it never sets this flag).
    m_geometry_shader->setInt("u_useInstanceOrigin", 0);

    // Far-LOD region meshes AFTER the live chunks (T-I3-9): same geometry
    // shader/material LUT (VoxelVertex layout is identical), region-AABB
    // frustum culling, depth-biased so overlapping live terrain wins. Far
    // draws stay inside the gbuffer GPU timer window; they are excluded from
    // the shadow cascades (ShadowPass never sees them).
    if (pipeline.m_farlod) {
        std::size_t far_draws = 0;
        std::size_t far_indices = 0;
        pipeline.m_farlod->draw_gbuffer(*m_geometry_shader, view, frustum_planes, far_draws, far_indices);
        pipeline.m_last_render_pass_stats.far_region_draws += far_draws;
        pipeline.m_last_render_pass_stats.far_indices_drawn += far_indices;
    }

    glBindVertexArray(0);
}

// spec 004 Phase 1: (re)build the cached static-prop instance data. Runs once
// (props are scattered once and never move) and again only if the static-mesh
// population changes. Precomputes the per-instance model matrix + albedo tint +
// base-mesh hash so the per-frame draw path no longer rebuilds them for ALL
// ~84k instances every frame (the measured CPU-submit bottleneck). RENDER-ONLY.
void GBufferPass::build_static_prop_cache(entt::registry& registry) {
    m_staticPropCache.clear();
    m_propMeshPaths.clear();
    m_resolveMemo.clear();
    m_visibleGroups.clear();
    std::unordered_map<std::string, std::uint32_t> pathIntern;
    auto intern = [&](const std::string& p) -> std::uint32_t {
        auto it = pathIntern.find(p);
        if (it != pathIntern.end()) return it->second;
        const std::uint32_t idx = static_cast<std::uint32_t>(m_propMeshPaths.size());
        m_propMeshPaths.push_back(p);
        pathIntern.emplace(p, idx);
        return idx;
    };
    auto fnv64 = [](const std::string& s, std::uint64_t seed) {
        std::uint64_t h = seed;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    };
    // Render-only deterministic position hash -> [0,1] (never feeds sim/world_hash).
    auto hash01 = [](const glm::vec3& p, float salt) {
        float s = std::sin(glm::dot(p, glm::vec3(12.9898f, 78.233f, 37.719f)) + salt) * 43758.5453f;
        return s - std::floor(s);
    };
    auto view = registry.view<const Components::TransformComponent, const Components::StaticMeshComponent>();
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
            tint = glm::mix(glm::vec3(0.62f, 0.90f, 0.48f), glm::vec3(1.05f, 0.82f, 0.42f), h * h) * b;
        } else if (endsWith("_bark", 5)) {
            const float h = hash01(transform.position, 5.1f);
            tint = glm::vec3(0.82f + 0.32f * h, 0.78f + 0.22f * h, 0.72f + 0.20f * h);
        }
        cp.tint = tint;
        cp.baseMeshHash = fnv64(mesh_info.meshPath, 1469598103934665603ull);
        cp.pathIndex = intern(mesh_info.meshPath);
        cp.materialId = mesh_info.materialId;
        m_staticPropCache.push_back(cp);
    }
    m_staticPropCachePopulation = registry.view<const Components::StaticMeshComponent>().size();
}

void GBufferPass::geometry_pass_static_meshes(RenderPipeline& pipeline,
                                              entt::registry& registry,
                                              const Camera& camera,
                                              const glm::vec4 frustum_planes[6]) {
    const auto _sp_t0 = std::chrono::steady_clock::now(); // spec 004: CPU submit cost
    m_instanced_static_mesh_shader->use();
    const glm::mat4 static_view = camera.GetViewMatrix();
    const glm::vec2 static_taau_jit = pipeline.taau_jitter_ndc();  // FR-R5 TAAU (0 when off)
    glm::mat4 static_proj = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    static_proj[2][0] += static_taau_jit.x;
    static_proj[2][1] += static_taau_jit.y;
    m_instanced_static_mesh_shader->setMat4("projection", static_proj);
    m_instanced_static_mesh_shader->setMat4("view", static_view);
    m_instanced_static_mesh_shader->setMat4("u_prev_view_proj", pipeline.prev_view_proj());  // FR-R5 TAAU motion vectors
    m_instanced_static_mesh_shader->setVec2("u_inv_screen_size",
        glm::vec2(1.0f / (float)pipeline.m_screen_width, 1.0f / (float)pipeline.m_screen_height));
    m_instanced_static_mesh_shader->setVec2("u_jitter_ndc", static_taau_jit);
    m_instanced_static_mesh_shader->setMat3("u_normalViewMatrix", glm::mat3(static_view));
    // Triplanar terrain arrays + LUT (T-I4-7): a static mesh tagged with a
    // textured material id (e.g. grass props) reuses the terrain triplanar path.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pipeline.m_materialLUT);
    m_instanced_static_mesh_shader->setInt("u_materialLUT", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainTextureArray);
    m_instanced_static_mesh_shader->setInt("u_terrainTextures", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainNormalArray);
    m_instanced_static_mesh_shader->setInt("u_terrainNormals", 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_skinnedTextureArray ? pipeline.m_skinnedTextureArray : pipeline.m_terrainTextureArray);
    m_instanced_static_mesh_shader->setInt("u_skinnedTextures", 3);
    m_instanced_static_mesh_shader->setInt("u_skinnedAlbedoLayer", -1);
    m_instanced_static_mesh_shader->setInt("u_skinnedNormalLayer", -1);
    m_instanced_static_mesh_shader->setInt("u_alphaTest", 0); // I8: per-group override below
    // FR-C2: instanced foliage/props skip the terrain-only macro rock overlay (a per-fragment
    // vnoise + up to 3 triplanar samples) — a leaf/bark/bush card never wants rock texturing,
    // and the forest's heavy overdraw made this branch a top G-buffer cost. Terrain keeps it.
    m_instanced_static_mesh_shader->setInt("u_macroRockOverlay", 0);
    m_instanced_static_mesh_shader->setFloat("u_time", static_cast<float>(glfwGetTime())); // I8 wind
    // §13 TAAU: prev-frame wind clock so the vert reconstructs each swayed vertex's PREVIOUS world
    // position -> the G-buffer motion vector tracks wind sway, not just camera motion (no tree-top ghost).
    m_instanced_static_mesh_shader->setFloat("u_prevTime", pipeline.prev_time());
    m_instanced_static_mesh_shader->setFloat("u_windStrength", 0.0f); // per-group override below
    // I7.1-PBR B1d: per-texel terrain roughness map (unit 4) — g_buffer.frag is
    // shared, so every program using it must bind a valid array to unit 4.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainRoughnessArray ? pipeline.m_terrainRoughnessArray : pipeline.m_terrainTextureArray);
    m_instanced_static_mesh_shader->setInt("u_terrainRoughness", 4);
    m_instanced_static_mesh_shader->setInt("u_terrainRoughnessValid", pipeline.m_terrainRoughnessValid);
    // spec 004 Phase 1: refresh the cached prop instance data only when the
    // static-mesh population changes (props are scattered once -> usually a no-op).
    {
        const std::size_t pop = registry.view<const Components::StaticMeshComponent>().size();
        if (pop != m_staticPropCachePopulation) build_static_prop_cache(registry);
    }
    // T-I3-16: groups carry the material id (per-group uniform). Track-B: each
    // instance picks a LOD bucket from its camera distance (SelectTreeLod) and the
    // group is keyed by the RESOLVED LOD mesh path so distant trees draw a coarser
    // variant; texture/material lookup uses the BASE path so LOD never loses the
    // bark/leaf textures. RENDER-ONLY. Missing LOD variant -> LOD0 fallback, so a
    // world with no LOD variants is byte-identical to the pre-LOD renderer.
    const glm::vec3 cameraPos = camera.Position;
    const bool impostorsOn = pipeline.tree_impostor_enabled();
    TreeLodConfig kTreeLodCfg; // data-driven defaults; render.tree_lod.* may override later.
    if (impostorsOn) {
        // The octa impostor is a single cheap quad (unlike the wide cross-billboard, which added
        // overdraw at 620 m), so kick LOD3 in much earlier to actually replace the far-field stand.
        kTreeLodCfg.lod3Distance = 200.0f;
    }
    // Resolves (loading + caching) the mesh for a candidate path, falling back to
    // the base LOD0 path if the LOD variant cannot be loaded. Returns the path that
    // actually resolved so the group is keyed by what is really drawn.
    auto resolve_mesh = [&](const std::string& candidatePath,
                            const std::string& basePath) -> std::pair<Mesh*, std::string> {
        if (m_meshCache.find(candidatePath) == m_meshCache.end()) {
            std::string full = (pipeline.m_root_path / candidatePath).string();
            // Only load the LOD variant if it exists on disk; an absent .lodN.lmesh caches a
            // null and falls back to LOD0 SILENTLY (no MeshLoader "failed header" error spam).
            m_meshCache[candidatePath] =
                std::filesystem::exists(full) ? MeshLoader::Load(full) : nullptr;
        }
        Mesh* m = m_meshCache[candidatePath].get();
        if (m) return {m, candidatePath};
        if (candidatePath != basePath) {
            if (m_meshCache.find(basePath) == m_meshCache.end()) {
                std::string full = (pipeline.m_root_path / basePath).string();
                m_meshCache[basePath] = MeshLoader::Load(full);
            }
            Mesh* base = m_meshCache[basePath].get();
            if (base) return {base, basePath};
        }
        return {nullptr, basePath};
    };
    // Reuse the per-group buffers across frames: keep capacity, clear contents,
    // mark inactive. A group's mesh/drawPath/basePath/material are fixed by its key
    // (rkey encodes baseMeshHash+lod), so the metadata is set once and kept.
    for (auto& kv : m_visibleGroups) { kv.second.mats.clear(); kv.second.tints.clear(); kv.second.active = false; }
    // Per-frame loop over the CACHE (not the registry): distance->LOD->resolve
    // (persistent memo)->frustum cull (per-LOD mesh sphere -> identical visible
    // set)->append the PRECOMPUTED matrix + tint. The old per-frame matrix build +
    // leaf/bark tint hash over all ~84k instances is gone (now done once at cache build).
    // Wave-3 far-field tree impostors: collected here, drawn after the mesh groups. One billboard per
    // tree (triggered on the leaf part; bark/trunk at LOD3 are folded into the same impostor).
    std::vector<glm::vec4> impostorInstances;
    int impostorMatId = 0;
    for (const CachedStaticProp& cp : m_staticPropCache) {
        const float dist = glm::length(cp.position - cameraPos);
        const int lod = SelectTreeLod(dist, kTreeLodCfg);
        if (impostorsOn && lod == 3) {
            const std::string& bp = m_propMeshPaths[cp.pathIndex];
            if (bp.find("tree") != std::string::npos) { // a tree part -> impostor replaces it at LOD3
                const bool isLeaf = bp.find("leaf") != std::string::npos; // "leaf"/"leaves"
                if (isLeaf) {
                    const glm::vec3 c = cp.position + glm::vec3(0.0f, pipeline.tree_impostor_sphere_y() * cp.maxScale, 0.0f);
                    const float r = pipeline.tree_impostor_radius() * cp.maxScale;
                    bool culled = false;
                    for (int i = 0; i < 6; i++) { if (glm::dot(glm::vec4(c, 1.0f), frustum_planes[i]) < -r) { culled = true; break; } }
                    if (!culled) { impostorInstances.emplace_back(cp.position, cp.maxScale); impostorMatId = static_cast<int>(cp.materialId); }
                }
                continue; // all LOD3 tree parts are folded into the billboard
            }
        }
        const std::uint64_t rkey = cp.baseMeshHash
                                 ^ (static_cast<std::uint64_t>(lod) * 0x9E3779B97F4A7C15ull);
        auto rit = m_resolveMemo.find(rkey);
        if (rit == m_resolveMemo.end()) {
            const std::string& basePath = m_propMeshPaths[cp.pathIndex];
            const std::string candidatePath = LodMeshPath(basePath, lod);
            auto resolved = resolve_mesh(candidatePath, basePath);
            rit = m_resolveMemo.emplace(rkey, std::make_pair(resolved.first, std::move(resolved.second))).first;
        }
        Mesh* mesh = rit->second.first;
        if (!mesh) continue;
        const glm::vec3 world_sphere_center = cp.position + glm::vec3(mesh->boundingSphere);
        const float radius = mesh->boundingSphere.w * cp.maxScale;
        bool culled = false;
        for (int i = 0; i < 6; i++) {
            if (glm::dot(glm::vec4(world_sphere_center, 1.0f), frustum_planes[i]) < -radius) {
                culled = true;
                break;
            }
        }
        if (culled) continue;
        // Group by a cheap uint64 (no per-instance string alloc/copy/compare).
        const std::uint64_t gkey = rkey ^ (static_cast<std::uint64_t>(cp.materialId) * 0x100000001B3ull);
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
    for (const auto& kv : m_visibleGroups) {
        const InstanceBatchCached& batch = kv.second;
        if (!batch.active) continue;
        const std::vector<glm::mat4>& matrices = batch.mats;
        Mesh* mesh = batch.mesh;
        if (!mesh || matrices.empty()) continue;
        m_instanced_static_mesh_shader->setInt("u_materialId", static_cast<int>(batch.materialId));
        // I8 static-model UV texture lane: if this mesh has registered bark/leaf
        // textures, bind the static-model array to unit 3 and set its albedo/normal
        // layers (+ alpha-test) so g_buffer.frag's UV branch samples the model's own
        // texture by mesh UV instead of the world-projected terrain triplanar.
        // Track-B: texture lookup uses the BASE path so LOD variants keep textures.
        {
            const auto* smt = pipeline.static_model_tex(batch.basePath);
            glActiveTexture(GL_TEXTURE3);
            if (smt && pipeline.static_model_texture_array() != 0) {
                glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.static_model_texture_array());
                m_instanced_static_mesh_shader->setInt("u_skinnedAlbedoLayer", smt->albedoLayer);
                m_instanced_static_mesh_shader->setInt("u_skinnedNormalLayer", smt->normalLayer);
                m_instanced_static_mesh_shader->setInt("u_alphaTest", smt->alphaTest ? 1 : 0);
                // I8: textured tree parts sway in the wind (rigid props stay at 0).
                m_instanced_static_mesh_shader->setFloat("u_windStrength", 1.0f);
                m_instanced_static_mesh_shader->setInt("u_forceFlat", 0);
            } else {
                glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_skinnedTextureArray ? pipeline.m_skinnedTextureArray : pipeline.m_terrainTextureArray);
                m_instanced_static_mesh_shader->setInt("u_skinnedAlbedoLayer", -1);
                m_instanced_static_mesh_shader->setInt("u_skinnedNormalLayer", -1);
                m_instanced_static_mesh_shader->setInt("u_alphaTest", 0);
                // VAST-FOREST: the procedural LEAF submeshes (no texture lane) still sway in the
                // wind (height-scaled); bark/trunk + rigid props stay at 0. Keyed on the palette
                // key suffix so only procgen leaves flutter.
                const bool isLeaf = batch.basePath.size() >= 5 &&
                                    batch.basePath.rfind("_leaf") == batch.basePath.size() - 5;
                m_instanced_static_mesh_shader->setFloat("u_windStrength", isLeaf ? 0.85f : 0.0f);
                // FR-C2: procgen FOLIAGE cards (leaves + bushes, no texture lane) take the flat
                // path at DISTANCE (LOD1+) — there the world-projected grass triplanar (6-9
                // texture-array samples/fragment) is invisible on a fluttering card but was a top
                // G-buffer cost under forest overdraw, and the per-instance green tint carries the
                // colour. The NEAR band (LOD0, the group drawn from the un-suffixed base mesh)
                // KEEPS the full triplanar so foreground foliage the player reads up close is
                // unchanged. Bark/trunk always keep triplanar (read as wood bark texture).
                const bool isBush = batch.basePath.find("procgen://bush_") != std::string::npos;
                const bool isFarLod = batch.drawPath.find(".lod") != std::string::npos;
                m_instanced_static_mesh_shader->setInt("u_forceFlat", ((isLeaf || isBush) && isFarLod) ? 1 : 0);
            }
        }
        // I8: clamp to the VBO capacity so an oversized group can't overrun the
        // buffer (glBufferSubData does not resize). Trees cap well under this.
        const GLsizei instance_count = static_cast<GLsizei>(
            std::min<std::size_t>(matrices.size(), static_cast<std::size_t>(kStaticInstanceCapacity)));
        glBindVertexArray(mesh->vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(instance_count) * sizeof(glm::mat4), matrices.data());
        for (int i = 0; i < 4; i++) {
            glEnableVertexAttribArray(3 + i);
            glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * i));
            glVertexAttribDivisor(3 + i, 1);
        }
        // Per-instance albedo tint at location 7 (divisor 1).
        glBindBuffer(GL_ARRAY_BUFFER, m_instanceTintVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(instance_count) * sizeof(glm::vec3), batch.tints.data());
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glVertexAttribDivisor(7, 1);
        glDrawElementsInstanced(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, 0, instance_count);
        glBindVertexArray(0);
    }

    // Wave-3 far-field tree impostors: one camera-facing billboard per collected far tree, sampling the
    // octahedral atlas into the same G-buffer attachments. ONE instanced draw + one shared atlas for the
    // whole far field (the draw-call/triangle collapse the forest_perf_budget gate targets).
    if (impostorsOn && !impostorInstances.empty() && m_tree_impostor_shader) {
        const GLsizei count = static_cast<GLsizei>(
            std::min<std::size_t>(impostorInstances.size(), static_cast<std::size_t>(kStaticInstanceCapacity)));
        m_tree_impostor_shader->use();
        m_tree_impostor_shader->setMat4("u_view", static_view);
        m_tree_impostor_shader->setMat4("u_proj", static_proj);
        m_tree_impostor_shader->setVec3("u_cameraPos", cameraPos);
        m_tree_impostor_shader->setFloat("u_radius", pipeline.tree_impostor_radius());
        m_tree_impostor_shader->setFloat("u_sphereY", pipeline.tree_impostor_sphere_y());
        m_tree_impostor_shader->setFloat("u_grid", static_cast<float>(pipeline.tree_impostor_grid()));
        m_tree_impostor_shader->setFloat("u_materialId", static_cast<float>(impostorMatId) / 255.0f);
        m_tree_impostor_shader->setInt("u_albedo", 0);
        m_tree_impostor_shader->setInt("u_normal", 1);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, pipeline.tree_impostor_albedo());
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, pipeline.tree_impostor_normal());
        glBindBuffer(GL_ARRAY_BUFFER, m_impostorInstanceVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(count) * sizeof(glm::vec4), impostorInstances.data());
        glBindVertexArray(m_impostorVAO);
        glDisable(GL_CULL_FACE);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
        glBindVertexArray(0);
    }

    m_last_static_prop_cpu_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _sp_t0).count();
}

void GBufferPass::geometry_pass_skinned_meshes(RenderPipeline& pipeline,
                                               entt::registry& registry,
                                               const Camera& camera,
                                               const glm::vec4 frustum_planes[6]) {
    namespace anim = luminumbra::animation;
    auto view = registry.view<const Components::TransformComponent,
                              const Components::SkinnedMeshComponent,
                              const anim::AnimationPlayerComponent>();
    if (view.begin() == view.end()) {
        return;
    }
    if (!m_skinned_mesh_shader || !m_skinned_mesh_shader->IsValid()) {
        return;
    }

    m_skinned_mesh_shader->use();
    const glm::mat4 skinned_view = camera.GetViewMatrix();
    const glm::vec2 skinned_taau_jit = pipeline.taau_jitter_ndc();  // FR-R5 TAAU (0 when off)
    glm::mat4 skinned_proj = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    skinned_proj[2][0] += skinned_taau_jit.x;
    skinned_proj[2][1] += skinned_taau_jit.y;
    m_skinned_mesh_shader->setMat4("projection", skinned_proj);
    m_skinned_mesh_shader->setMat4("view", skinned_view);
    m_skinned_mesh_shader->setMat4("u_prev_view_proj", pipeline.prev_view_proj());  // FR-R5 TAAU motion vectors
    m_skinned_mesh_shader->setVec2("u_inv_screen_size",
        glm::vec2(1.0f / (float)pipeline.m_screen_width, 1.0f / (float)pipeline.m_screen_height));
    m_skinned_mesh_shader->setVec2("u_jitter_ndc", skinned_taau_jit);
    m_skinned_mesh_shader->setMat3("u_normalViewMatrix", glm::mat3(skinned_view));
    // Triplanar terrain arrays + LUT (T-I4-7).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pipeline.m_materialLUT);
    m_skinned_mesh_shader->setInt("u_materialLUT", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainTextureArray);
    m_skinned_mesh_shader->setInt("u_terrainTextures", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainNormalArray);
    m_skinned_mesh_shader->setInt("u_terrainNormals", 2);
    // T-I4-8: UV-mapped skinned-mesh texture array on unit 3. Skinned creatures
    // take the UV-sampled path (precedence over the terrain LUT triplanar path).
    glActiveTexture(GL_TEXTURE3);
    // Match the terrain/static paths: when no skinned texture array is loaded,
    // bind the terrain array as a valid fallback so unit 3's sampler2DArray is
    // never left referencing an empty unit (GL_INVALID_OPERATION "program
    // texture usage" otherwise — fires once per skinned draw). The albedo/normal
    // layers below resolve to -1 in that case, so the fallback is never sampled.
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_skinnedTextureArray ? pipeline.m_skinnedTextureArray : pipeline.m_terrainTextureArray);
    m_skinned_mesh_shader->setInt("u_skinnedTextures", 3);
    m_skinned_mesh_shader->setInt("u_skinnedAlbedoLayer", pipeline.m_skinnedTextureArray ? pipeline.m_skinnedAlbedoLayer : -1);
    m_skinned_mesh_shader->setInt("u_skinnedNormalLayer", pipeline.m_skinnedTextureArray ? pipeline.m_skinnedNormalLayer : -1);
    // I7.1-PBR B1d: per-texel terrain roughness map (unit 4) — shared g_buffer.frag.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainRoughnessArray ? pipeline.m_terrainRoughnessArray : pipeline.m_terrainTextureArray);
    m_skinned_mesh_shader->setInt("u_terrainRoughness", 4);
    m_skinned_mesh_shader->setInt("u_terrainRoughnessValid", pipeline.m_terrainRoughnessValid);

    for (auto entity : view) {
        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& mesh_info = view.get<const Components::SkinnedMeshComponent>(entity);
        auto const& player = view.get<const anim::AnimationPlayerComponent>(entity);

        if (m_skinnedMeshCache.find(mesh_info.meshPath) == m_skinnedMeshCache.end()) {
            std::string full_mesh_path = (pipeline.m_root_path / mesh_info.meshPath).string();
            m_skinnedMeshCache[mesh_info.meshPath] = MeshLoader::LoadSkinned(full_mesh_path);
        }
        Mesh* mesh = m_skinnedMeshCache[mesh_info.meshPath].get();
        if (!mesh || mesh->jointCount == 0) continue;

        // The T-I3-15 runtime emits 16 floats per joint; a palette that does
        // not match the mesh skeleton is a wiring bug, skip the draw.
        const std::size_t expected_floats = static_cast<std::size_t>(mesh->jointCount) * 16u;
        if (player.palette.size() != expected_floats) continue;

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
        if (culled) continue;

        glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position);
        model *= glm::mat4_cast(transform.rotation);
        model = glm::scale(model, transform.scale);
        m_skinned_mesh_shader->setMat4("model", model);
        m_skinned_mesh_shader->setInt("u_materialId", static_cast<int>(mesh_info.materialId));
        // Phase 2: per-creature albedo tint from the species base_color (white = no-op).
        m_skinned_mesh_shader->setVec3("u_albedo_tint",
            glm::vec3(mesh_info.tintR, mesh_info.tintG, mesh_info.tintB));

        const std::size_t palette_bytes = player.palette.size() * sizeof(float);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_jointPaletteSSBO);
        if (palette_bytes > m_jointPaletteSSBOCapacityBytes) {
            m_jointPaletteSSBOCapacityBytes = palette_bytes;
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(palette_bytes), player.palette.data(), GL_DYNAMIC_DRAW);
        } else {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(palette_bytes), player.palette.data());
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_jointPaletteSSBO);

        glBindVertexArray(mesh->vao);
        glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
        pipeline.m_last_render_pass_stats.skinned_draws++;
        pipeline.m_last_render_pass_stats.skinned_indices_drawn += mesh->indexCount;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

} // namespace Luminumbra::Rendering
