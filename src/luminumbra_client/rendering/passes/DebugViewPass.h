#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Shader;
struct GBuffer;

// Render-only G-buffer DEBUG visualizer. A fullscreen pass that, when enabled, overrides
// the final composite with a single-channel view of the DEFERRED G-buffer (albedo / normal
// / depth / material-id / position) so a human or the frame-scan can distinguish a genuinely
// "dark night" from a lighting/geometry bug. Purely diagnostic: it reads the G-buffer only,
// never touches sim state or world_hash, and is DEFAULT-OFF (Mode::None => host skips it,
// so the normal render stays byte-identical).
class DebugViewPass {
public:
    // Keep these in lockstep with debug_view.frag's u_mode switch.
    enum Mode {
        None     = 0,   // pass is a no-op; caller skips it
        Albedo   = 1,   // unlit base color
        Normal   = 2,   // octahedral normal decoded -> 0.5 + 0.5*n
        Depth    = 3,   // linearized depth, grayscale
        Material = 4,   // material-id hashed to a distinct color
        Position = 5,   // view-space position remapped to [0,1]
    };

    DebugViewPass();
    ~DebugViewPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers();        // empty VAO (verts from gl_VertexID)
    void destroy_buffers();
    void reset_shader();

    void set_mode(int mode) { m_mode = mode; }
    int  mode() const { return m_mode; }

    // Optional: camera planes for hardware-depth linearization (sky pixels in Depth mode).
    void set_camera_planes(float near_plane, float far_plane) {
        m_near = near_plane;
        m_far  = far_plane;
    }

    // Draw the debug visualization into the CURRENTLY BOUND framebuffer (the caller binds
    // the screen/backbuffer + sets the viewport). A no-op when mode == None. Reads the
    // G-buffer textures by their formats; depth test/blend are disabled internally.
    void execute(const GBuffer& gbuffer, int mode);

private:
    std::unique_ptr<Shader> m_shader;
    GLuint m_vao  = 0;          // empty VAO; the VS builds a fullscreen triangle
    int    m_mode = Mode::None; // default-OFF
    float  m_near = 0.1f;
    float  m_far  = 4000.0f;
};

} // namespace Luminumbra::Rendering
