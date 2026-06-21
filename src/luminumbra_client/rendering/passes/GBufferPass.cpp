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
// dropped instances). Must exceed the largest single instance group; the tree
// scatter caps well under this (see main_client.cpp kMaxInstances).
static constexpr GLsizei kStaticInstanceCapacity = 16384;

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

    const GLenum attachments[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, attachments);

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

    m_geometry_shader->setMat4("projection", projection);
    m_geometry_shader->setMat4("view", view);
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

void GBufferPass::geometry_pass_static_meshes(RenderPipeline& pipeline,
                                              entt::registry& registry,
                                              const Camera& camera,
                                              const glm::vec4 frustum_planes[6]) {
    m_instanced_static_mesh_shader->use();
    const glm::mat4 static_view = camera.GetViewMatrix();
    m_instanced_static_mesh_shader->setMat4("projection", glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane()));
    m_instanced_static_mesh_shader->setMat4("view", static_view);
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
    m_instanced_static_mesh_shader->setFloat("u_time", static_cast<float>(glfwGetTime())); // I8 wind
    m_instanced_static_mesh_shader->setFloat("u_windStrength", 0.0f); // per-group override below
    // I7.1-PBR B1d: per-texel terrain roughness map (unit 4) — g_buffer.frag is
    // shared, so every program using it must bind a valid array to unit 4.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainRoughnessArray ? pipeline.m_terrainRoughnessArray : pipeline.m_terrainTextureArray);
    m_instanced_static_mesh_shader->setInt("u_terrainRoughness", 4);
    m_instanced_static_mesh_shader->setInt("u_terrainRoughnessValid", pipeline.m_terrainRoughnessValid);
    auto view = registry.view<const Components::TransformComponent, const Components::StaticMeshComponent>();
    // T-I3-16: groups carry the material id (per-group uniform) so each
    // static mesh renders with its component material instead of the old
    // hardcoded grass id.
    //
    // Track-B (roadmap pillar B): per-instance distance LOD. Each instance picks a
    // LOD bucket from its camera distance (SelectTreeLod) and the group is keyed by
    // the RESOLVED LOD mesh path so distant trees draw a coarser variant. The
    // material/texture lane is still looked up by the BASE asset path so the LOD
    // swap never loses the tree's bark/leaf textures. RENDER-ONLY: nothing here is
    // hashed or fed back into the sim. If a LOD variant file is missing the load
    // returns null and we transparently fall back to the base (LOD0) mesh, so a
    // world with no LOD variants is byte-identical to the pre-LOD renderer.
    const glm::vec3 cameraPos = camera.Position;
    static const TreeLodConfig kTreeLodCfg = []() {
        TreeLodConfig c; // data-driven defaults; render.tree_lod.* may override later.
        return c;
    }();
    // Cheap FNV-1a over a path string — used to key the per-frame resolve memo and
    // the instance groups by uint64 instead of allocating/copying std::strings per
    // instance (the old per-instance string alloc + std::map-of-strings was a large
    // per-frame CPU tax that made flying laggy with tens of thousands of instances).
    auto fnv64 = [](const std::string& s, std::uint64_t seed) {
        std::uint64_t h = seed;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    };
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
    // Each visible group carries parallel instance matrices + per-instance albedo tints
    // (vast-forest leaf/bark colour variation). Tint defaults to white (no-op) for non-trees.
    struct InstanceBatch {
        Mesh* mesh = nullptr;
        std::string drawPath;     // resolved LOD mesh path actually drawn
        std::string basePath;     // original asset path for texture/material lookup
        std::uint32_t materialId = 0;
        std::vector<glm::mat4> mats; std::vector<glm::vec3> tints;
    };
    // Per-frame memo: (basePath,lod) -> resolved {mesh, drawPath}, so the expensive
    // LodMeshPath() string build + resolve_mesh() lookup run ONCE per distinct mesh
    // variant per frame (~hundreds) instead of once per instance (~tens of thousands).
    std::unordered_map<std::uint64_t, std::pair<Mesh*, std::string>> resolveMemo;
    std::unordered_map<std::uint64_t, InstanceBatch> visible_instance_groups;
    // Render-only deterministic position hash -> [0,1] (never feeds the sim/world_hash).
    auto hash01 = [](const glm::vec3& p, float salt) {
        float s = std::sin(glm::dot(p, glm::vec3(12.9898f, 78.233f, 37.719f)) + salt) * 43758.5453f;
        return s - std::floor(s);
    };
    for (auto entity : view) {
        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& mesh_info = view.get<const Components::StaticMeshComponent>(entity);
        // Pick the LOD bucket from this instance's distance to the render camera,
        // then resolve the LOD variant path (with LOD0 fallback) THROUGH THE MEMO so
        // we don't allocate a path string per instance. Distance uses the instance
        // origin; the near/far thresholds make the near field unchanged.
        const float dist = glm::length(transform.position - cameraPos);
        const int lod = SelectTreeLod(dist, kTreeLodCfg);
        const std::uint64_t rkey = fnv64(mesh_info.meshPath, 1469598103934665603ull)
                                 ^ (static_cast<std::uint64_t>(lod) * 0x9E3779B97F4A7C15ull);
        auto rit = resolveMemo.find(rkey);
        if (rit == resolveMemo.end()) {
            const std::string candidatePath = LodMeshPath(mesh_info.meshPath, lod);
            auto resolved = resolve_mesh(candidatePath, mesh_info.meshPath);
            rit = resolveMemo.emplace(rkey, std::make_pair(resolved.first, std::move(resolved.second))).first;
        }
        Mesh* mesh = rit->second.first;
        if (!mesh) continue;
        const std::string& drawPath = rit->second.second;
        glm::vec3 world_sphere_center = transform.position + glm::vec3(mesh->boundingSphere);
        float radius = mesh->boundingSphere.w * glm::max(glm::max(transform.scale.x, transform.scale.y), transform.scale.z);
        bool culled = false;
        for (int i = 0; i < 6; i++) {
            if (glm::dot(glm::vec4(world_sphere_center, 1.0f), frustum_planes[i]) < -radius) {
                culled = true;
                break;
            }
        }
        if (!culled) {
            // T-I3-16 fix: the instance transform previously dropped the
            // rotation quaternion (translate * scale only).
            glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position);
            model *= glm::mat4_cast(transform.rotation);
            model = glm::scale(model, transform.scale);
            // Per-instance tint: procedural LEAF submeshes get a green->autumn-gold genetic
            // variation (+ brightness jitter); BARK a subtle warm-brown variation; everything
            // else white (no-op). Keyed on the palette "_leaf"/"_bark" suffix.
            glm::vec3 tint(1.0f);
            const std::string& bp = mesh_info.meshPath;
            auto endsWith = [&](const char* suf, std::size_t n) {
                return bp.size() >= n && bp.compare(bp.size() - n, n, suf) == 0;
            };
            if (endsWith("_leaf", 5)) {
                const float h = hash01(transform.position, 0.0f);
                const float b = 0.70f + 0.30f * hash01(transform.position, 11.3f);
                // Natural foliage green -> autumn gold. The green channel stays <= 1
                // (no over-boost): leaves now carry a real textured green albedo, so
                // the old 1.18 green-boost (x up to 1.25 brightness) pushed them to a
                // neon lime that washed the horizon sky-band (GREEN_SKY_SPECKLE) and
                // read unnaturally. This modulates the texture instead of inflating it.
                tint = glm::mix(glm::vec3(0.62f, 0.90f, 0.48f), glm::vec3(1.05f, 0.82f, 0.42f), h * h) * b;
            } else if (endsWith("_bark", 5)) {
                const float h = hash01(transform.position, 5.1f);
                tint = glm::vec3(0.82f + 0.32f * h, 0.78f + 0.22f * h, 0.72f + 0.20f * h);
            }
            // Group by a cheap uint64 (no per-instance string alloc/copy/compare).
            const std::uint64_t gkey = rkey ^ (static_cast<std::uint64_t>(mesh_info.materialId) * 0x100000001B3ull);
            auto& batch = visible_instance_groups[gkey];
            if (batch.mats.empty()) {
                batch.mesh = mesh;
                batch.drawPath = drawPath;
                batch.basePath = mesh_info.meshPath;
                batch.materialId = mesh_info.materialId;
            }
            batch.mats.push_back(model);
            batch.tints.push_back(tint);
        }
    }
    for (const auto& [gkey, batch] : visible_instance_groups) {
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
    m_skinned_mesh_shader->setMat4("projection", glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane()));
    m_skinned_mesh_shader->setMat4("view", skinned_view);
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
