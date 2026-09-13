#pragma once

#include <luminumbra/rendering/RenderView.h>
#include <luminumbra/rendering/StaticScene.h>
#include <memory>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

struct StaticFrame {
    std::uint64_t sequence = 0, scene_revision = 0;
    RenderViewDescription camera;
    std::array<float, 16> actual_view{}, actual_projection{};
    // Row zero is the bottom image row. Color is the production inspection
    // grade, power-2.2 encoded, straight alpha (covered 1, clear background 0).
    std::vector<std::uint8_t> rgba8;
    std::vector<float> depth32f;         // Finite reversed-Z [0,1], clear 0; same submission.
    std::vector<std::uint8_t> coverage8; // Exactly depth > 0, OPAQUE/MASK only.
    std::size_t draw_count = 0, index_count = 0;
    std::size_t uploaded_meshes = 0, uploaded_textures = 0, updated_instances = 0;
    double synchronous_render_readback_ms = 0;
    std::string vendor, renderer, version;
};

// Initial installed inspection module: one hidden context on its owning thread,
// no game process/services and no temporal history. Uses the production static
// geometry and LightingPass. Render() performs a bounded synchronous readback;
// it is a capture API, not the eventual nonblocking Blender viewport transport.
// Construct, Render and destroy on the same thread. This first host profile
// requires exclusive process-wide GLFW ownership: no other GLFW user/window may
// coexist with it, because the module initializes and terminates GLFW itself.
class StaticRenderer {
public:
    explicit StaticRenderer(const std::string& resource_root,
                            bool require_software_renderer = false);
    ~StaticRenderer();
    static std::string ModulePath(); // Actual loaded renderer module, for executable-byte receipts.
    StaticRenderer(const StaticRenderer&) = delete;
    StaticRenderer& operator=(const StaticRenderer&) = delete;
    StaticFrame Render(const RenderView& view, std::shared_ptr<const StaticDrawSnapshot> scene);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Luminumbra::Rendering
