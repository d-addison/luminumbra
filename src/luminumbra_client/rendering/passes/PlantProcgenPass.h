#pragma once

#include "../RenderContext.h"

#include <glad/glad.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// ===========================================================================
//  render-only PROCEDURAL plant pass (behind render.plant_procgen,
// OFF by default). Renders the new PROGRAMMATICALLY/GENETICALLY/ATMOSPHERICALLY
// grown plants (foliage::GeneratePlant -> TessellatePlant) directly into the
// deferred G-buffer, instead of relying only on the baked static tree models.
//
// The caller (main_client) bakes ONE combined CPU vertex/index buffer for a
// bounded subset of the deterministic tree-scatter positions, with each plant's
// local mesh already TRANSFORMED TO WORLD SPACE at its position. It pushes that
// buffer (plus a signature) via set_plants; the pass uploads it to a dedicated
// VAO/VBO/EBO ONCE and re-uploads only when the signature changes (flag toggle /
// position rebuild). execute draws it into the SAME G-buffer FBO the static
// meshes write (view position, octahedral view normal, albedo, depth), so the
// lighting pass shades it like any other solid geometry.
//
// nothing here touches the sim or world_hash. When disabled (or
// no plants pushed) the pass issues zero GL work, so default behaviour is
// byte-identical.
//
// converted to the RenderContext seam. execute takes a
// const RenderContext& (screen size + the per-frame time snapshot) instead of a
// RenderPipeline&, so the pass reaches no pipeline privates and needs no friend.
// u_time MUST come from ctx.time_seconds (the single per-frame glfwGetTime
// snapshot), NOT a fresh glfwGetTime, so the seam is deterministic.
// ===========================================================================
class PlantProcgenPass {
public:
    // One uploaded vertex (world-space pos + world-space normal + a bake flag in
    // uv.x: 0 = woody branch, 1 = leaf card). Matches the plant_procgen.vert
    // attribute layout (locations 0/1/2).
    struct Vertex {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv;
        glm::vec3 color; // per-vertex albedo (genetic + seasonal leaf color / bark brown)
    };

    PlantProcgenPass();
    ~PlantProcgenPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers();
    void destroy_buffers();
    void reset_shader();

    // Replace the combined plant mesh. Re-uploads to the GPU only when
    // `signature` differs from the last upload (cheap no-op otherwise). An empty
    // mesh clears the pass (subsequent execute is a no-op).
    void set_plants(const std::vector<Vertex>& vertices,
                    const std::vector<std::uint32_t>& indices,
                    std::uint64_t signature);

    void set_enabled(bool on) {
        m_enabled = on;
    }
    bool enabled() const {
        return m_enabled;
    }
    std::uint64_t signature() const {
        return m_signature;
    }
    std::size_t index_count() const {
        return m_index_count;
    }

    // Draws the combined plant mesh into the currently-bound G-buffer FBO. The
    // caller binds the FBO + sets the viewport (same pattern as the
    // far-field injection). No-op when disabled, no shader, or no geometry.
    void execute(const RenderContext& ctx, const Camera& camera);

private:
    std::unique_ptr<Shader> m_shader;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_ebo = 0;
    GLsizei m_vbo_capacity_bytes = 0; // current VBO allocation
    GLsizei m_ebo_capacity_bytes = 0; // current EBO allocation
    std::size_t m_index_count = 0;
    std::uint64_t m_signature = 0;
    bool m_has_geometry = false;
    bool m_enabled = false;
};

} // namespace Luminumbra::Rendering