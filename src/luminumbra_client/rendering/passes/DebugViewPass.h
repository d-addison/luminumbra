#pragma once

#include "../DiagnosticCapture.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <string_view>

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
        DrawId = 6,
        MeshId = 7,
        Lod = 8,
        Chunk = 9,
        Producer = 10,
        MeshPath = 11,
    };

    static constexpr std::array<const char*, 7> identity_mode_names = {
        "off", "draw-id", "mesh-id", "lod", "chunk", "producer-owner", "mesh-path"};
    static int identity_mode(std::string_view name) {
        // Six identity modes; legacy mode parsing remains with the existing host.
        for (std::size_t index = 1; index < identity_mode_names.size(); ++index) {
            if (name == identity_mode_names[index]) {
                return DrawId + static_cast<int>(index) - 1;
            }
        }
        return None;
    }

    enum class ProducerKind : std::uint32_t {
        Unsupported,
        Terrain,
        Impostor,
        Foliage,
        Static,
        FarVolume
    };
    // Explicit renderer spellings; unknown metadata never claims a real producer.
    static ProducerKind producer(std::string_view variant, bool supported) {
        if (!supported) {
            return ProducerKind::Unsupported;
        }
        if (variant == "live_terrain" || variant == "live_terrain_mdi" ||
            variant == "live_terrain_mdi_ladder" || variant == "legacy_far_terrain" ||
            variant == "legacy_far_water") {
            return ProducerKind::Terrain;
        }
        if (variant == "tree_impostor" || variant == "tree_impostor_ladder") {
            return ProducerKind::Impostor;
        }
        if (variant == "foliage" || variant == "foliage_ladder") {
            return ProducerKind::Foliage;
        }
        if (variant == "instanced_static_mesh" || variant == "instanced_static_mesh_ladder" ||
            variant == "skinned_mesh" || variant == "skinned_mesh_ladder") {
            return ProducerKind::Static;
        }
        if (variant == "far_volume" || variant == "far_volume_ladder") {
            return ProducerKind::FarVolume;
        }
        return ProducerKind::Unsupported;
    }
    // Specified uint32 arithmetic, shared with the shader; never std::hash.
    static constexpr std::uint32_t hash_id(std::uint32_t value) {
        return value * 2654435761U;
    }
    // Bounded schema-1 lookup. Empty result means absent, invalid or mismatched identity.
    static std::string join(std::string_view table, const DiagnosticId& id);
    static bool decode_material(float value, std::uint32_t& output) {
        if (!std::isfinite(value) || value < 0 || value > 255 || std::trunc(value) != value) {
            return false;
        }
        output = static_cast<std::uint32_t>(value);
        return true;
    }
    struct InspectorPixel {
        DiagnosticId id{};
        std::array<float, 4> albedo{};
        std::array<float, 4> normal_depth{};
    };
    static_assert(sizeof(InspectorPixel) == 48);
    // One fixed 48-byte record gathered from exactly one attachment coordinate.
    static void read_inspector_pixel(unsigned int buffer, InspectorPixel& pixel);

    struct Controls {
        bool enabled = false;
        bool inspector_open = false;
        bool refresh = false;
        int view = None;
        int x = 0;
        int y = 0;
        DiagnosticExtent extent{};
        std::string result;
        std::string status;
    };
    // Render-thread UI bridge; the registry owns visibility, the pass owns GL storage.
    static Controls& controls() {
        static Controls control;
        return control;
    }
    static void cycle_mode(int& legacy_mode) {
        auto& control = controls();
        if (control.enabled && control.view >= DrawId && control.view <= MeshPath) {
            control.view = control.view == MeshPath ? None : control.view + 1;
            legacy_mode = None;
        } else if (control.enabled && legacy_mode == Position) {
            control.view = DrawId;
            control.refresh = true;
            legacy_mode = None;
        } else {
            control.view = None;
            legacy_mode = legacy_mode >= None && legacy_mode < Position ? legacy_mode + 1 : None;
        }
    }
    class Frame;
    // Null when disabled, replaying, idle, or another capture owns the producer scope.
    std::shared_ptr<Frame> begin_frame(const RenderContext& ctx);

    DebugViewPass();
    ~DebugViewPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers(); // empty VAO (verts from gl_VertexID)
    void destroy_buffers();
    void reset_shader();

    void set_mode(int mode);
    int mode() const {
        const auto& control = controls();
        if (m_mode >= DrawId && (!control.enabled || m_mode > MeshPath)) {
            return None;
        }
        return control.enabled && control.view >= DrawId && control.view <= MeshPath ? control.view
                                                                                     : m_mode;
    }

    // Draw the debug visualization into the CURRENTLY BOUND framebuffer (the caller binds
    // the screen/backbuffer + sets the viewport). A no-op when mode == None.  pass
    // contract: the G-buffer attachment reads come from the RenderContext handles,
    // and the near/far planes for hardware-depth linearization (Depth mode) from ctx.camera.
    // depth test/blend are disabled internally.
    void execute(const RenderContext& ctx);

private:
    struct Snapshot;
    std::unique_ptr<Snapshot> m_snapshot;
    void finish_frame(const RenderContext& ctx);
    void bind_sources(const RenderContext& ctx, Shader& shader);
    std::unique_ptr<Shader> m_shader;
    unsigned int m_vao = 0;  // empty VAO; the VS builds a fullscreen triangle
    int m_mode = Mode::None; // default-OFF (pass config, not frame state; set via set_mode)
};

} // namespace Luminumbra::Rendering
