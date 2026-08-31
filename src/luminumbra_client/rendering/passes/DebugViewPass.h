#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Shader;
struct RenderContext;

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
        None = 0,     // pass is a no-op; caller skips it
        Albedo = 1,   // unlit base color
        Normal = 2,   // octahedral normal decoded -> 0.5 + 0.5*n
        Depth = 3,    // linearized depth, grayscale
        Material = 4, // material-id hashed to a distinct color
        Position = 5, // view-space position remapped to [0,1]
    };

    DebugViewPass();
    ~DebugViewPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers(); // empty VAO (verts from gl_VertexID)
    void destroy_buffers();
    void reset_shader();

    void set_mode(int mode) {
        m_mode = mode;
    }
    int mode() const {
        return m_mode;
    }

    // Draw the debug visualization into the CURRENTLY BOUND framebuffer (the caller binds
    // the screen/backbuffer + sets the viewport). A no-op when mode == None.  pass
    // contract: the G-buffer attachment reads come from the RenderContext handles,
    // and the near/far planes for hardware-depth linearization (Depth mode) from ctx.camera.
    // depth test/blend are disabled internally.
    void execute(const RenderContext& ctx);

private:
    std::unique_ptr<Shader> m_shader;
    GLuint m_vao = 0;        // empty VAO; the VS builds a fullscreen triangle
    int m_mode = Mode::None; // default-OFF (pass config, not frame state; set via set_mode)
};

} // namespace Luminumbra::Rendering
