#pragma once

#include "../DiagnosticCapture.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <string_view>

namespace Luminumbra::Rendering {

// Surface display contracts. Runtime reads production shader values; these predicates
// describe their validity without requiring a GL context.
struct DiagnosticContributions {
    static constexpr int FirstSurface = 30;
    static constexpr int LastSurface = 33;
    static constexpr bool surface_mode(int mode) {
        return mode >= FirstSurface && mode <= LastSurface;
    }
    static bool normalized_weights(const std::array<float, 3>& weights) {
        float sum = 0;
        for (const float weight : weights) {
            if (!std::isfinite(weight) || weight < 0 || weight > 1) {
                return false;
            }
            sum += weight;
        }
        return std::abs(sum - 1.0f) <= 0.001f;
    }
    static constexpr std::string_view legend(int mode) {
        switch (mode) {
            case 30:
                return "Actual mip: maximum base filtered sampler LOD, anisotropy and bias, "
                       "clamped to sampler levels; black=0, white=16 (saturated)";
            case 31:
                return "Texel density: maximum biased footprint in texels/pixel, "
                       "log2 scale black=-8, grey=0 (1 texel/pixel), white=8";
            case 32:
                return "Triplanar weights: production XYZ in RGB; sum=1 +/-0.001, each [0,1]; "
                       "yellow=invalid weights";
            case 33:
                return "Raw finite check: black=finite, magenta=NaN/Inf before material "
                       "clamp or G-buffer storage";
            default:
                return "Unsupported surface mode";
        }
    }
};

class Shader;
struct RenderContext;
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
        DirectSun = 12,
        DirectMoon = 13,
        Ambient = 14,
        ShadowFactor = 15,
        Ao = 16,
        Emissive = 17,
        Fog = 18,
        Specular = 19,
        Roughness = 20,
        Metallic = 21,
        NormalView = 22,
        NormalWorld = 23,
        SkyVisibility = 24,
        MaterialAo = 25,
        Lighting = 26,
        ForwardPlane = 27,
        SkyPlane = 28,
        ShadowPlane = 29,
        ActualMip = 30,
        TexelDensity = 31,
        TriplanarWeights = 32,
        RawFinite = 33,
        ShadowCascade = 34,
        ShadowFootprint = 35,
        Wireframe = 36,
    };

    static constexpr std::array<const char*, 32> identity_mode_names = {
        "off",          "draw-id",        "mesh-id",          "lod",
        "chunk",        "producer-owner", "mesh-path",        "direct-sun",
        "direct-moon",  "sky-ambient",    "shadow-factor",    "ao",
        "emissive",     "fog-atmosphere", "specular",         "roughness",
        "metallic",     "normal-view",    "normal-world",     "sky-visibility",
        "material-ao",  "lighting",       "forward-plane",    "sky-plane",
        "shadow-plane", "actual-mip",     "texel-density",    "triplanar-weights",
        "raw-finite",   "shadow-cascade", "shadow-footprint", "wireframe-shaded"};
    static int identity_mode(std::string_view name) {
        // Shared CLI/panel catalogue; legacy modes retain their original numbers.
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
        // RenderPipeline submits live terrain as terrain_mdi[_ladder].
        if (variant == "terrain_mdi" || variant == "terrain_mdi_ladder" ||
            variant == "legacy_far_terrain" || variant == "legacy_far_water") {
            return ProducerKind::Terrain;
        }
        if (variant == "tree_impostor" || variant == "tree_impostor_ladder") {
            return ProducerKind::Impostor;
        }
        if (variant == "foliage" || variant == "foliage_ladder") {
            return ProducerKind::Foliage;
        }
        // StaticDrawPass submits authored and file statics as static_asset.
        if (variant == "instanced_static_mesh" || variant == "instanced_static_mesh_ladder" ||
            variant == "skinned_mesh" || variant == "skinned_mesh_ladder" ||
            variant == "static_asset") {
            return ProducerKind::Static;
        }
        // FarVolumeGlBackend submits Ladder far volumes as g_buffer_ladder_far.
        if (variant == "far_volume" || variant == "far_volume_ladder" ||
            variant == "g_buffer_ladder_far") {
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

    static constexpr bool contribution_mode(int mode) {
        return (mode >= DirectSun && mode <= ShadowPlane) || mode == ShadowCascade ||
               mode == ShadowFootprint;
    }
    static constexpr bool supported_contribution(int mode) {
        return contribution_mode(mode) && mode != Fog &&
               (mode < ForwardPlane || mode >= ShadowCascade);
    }
    static std::string_view contribution_legend(int mode);
    // RGBA16F, at most 16M pixels / 128 MiB. Validate before GPU allocation.
    static constexpr bool contribution_extent(unsigned int width, unsigned int height) {
        return width != 0 && height != 0 && width <= 16384 && height <= 16384 &&
               width <= 16777216U / height;
    }
    struct Controls {
        bool enabled = false;
        bool wireframe = false; // existing renderer toggle; independent of identity diagnostics
        bool inspector_open = false;
        bool refresh = false;
        bool armed = false;
        bool frozen = false;
        bool inspect_requested = false;
        bool frame_invalidated = false; // Window resize notification precedes target recreation.
        int view = None;
        int x = -1;
        int y = -1;
        DiagnosticExtent extent{};
        DiagnosticExtent output{};
        unsigned int zoom_texture = 0; // Borrowed until refresh, resize or disable.
        unsigned int image_texture = 0;
        std::string probe_json;
        unsigned int contribution_texture = 0; // borrowed, owned by LightingPass
        DiagnosticExtent contribution_size{};
        int contribution_view = None;
        DebugViewPass* surface_owner = nullptr; // borrowed for this render frame only
        std::string result;
        std::string status;
    };
    struct Viewport {
        double window_width = 0;
        double window_height = 0;
        DiagnosticExtent framebuffer{};
        // Rendered image rectangle, top-left framebuffer pixels (allows letterboxing).
        double left = 0;
        double top = 0;
        double width = 0;
        double height = 0;
    };
    // The sole window/DPI -> top-left attachment conversion. Refusals do not mutate state.
    static bool
    select_pixel(Controls& control, double window_x, double window_y, const Viewport& viewport);
    // Kept GL-free and inline: the console widget also links in the standalone UI target.
    static void request_inspection(Controls& control) {
        if (!control.enabled || !control.inspector_open) {
            return;
        }
        control.inspect_requested = true;
        control.probe_json.clear();
        control.result.clear();
        control.zoom_texture = 0;
        if (!control.frozen) {
            control.refresh = true;
        }
    }
    // Resize callers invalidate immediately, before deferred target recreation.
    static void invalidate_selection(Controls& control);
    void invalidate();
    // Exact schema-1 clipboard payload; the richer UI record is separate.
    static std::string
    pixel_json(const DiagnosticExtent& extent, int x, int y, const InspectorPixel& pixel);
    // Render-thread UI bridge; the registry owns visibility, the pass owns GL storage.
    static Controls& controls() {
        static Controls control;
        return control;
    }
    static constexpr bool replay_mode(int view) {
        return view == Wireframe;
    }
    static int geometry_view() {
        return controls().wireframe ? Wireframe : controls().view;
    }
    static bool geometry_requested() {
        return controls().wireframe ||
               (controls().enabled && (DiagnosticContributions::surface_mode(controls().view) ||
                                       replay_mode(controls().view)));
    }
    static void cycle_mode(int& legacy_mode) {
        auto& control = controls();
        if (control.enabled && control.view >= DrawId && control.view <= Wireframe) {
            control.view = control.view == Wireframe ? None : control.view + 1;
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
    // Stack scope attaches for an explicitly selected surface or deferred replay view.
    // Replay routes colour only to location six and never writes opaque depth.
    // Construction with diagnostics off performs no GL calls or allocations.
    class SurfaceFrame {
    public:
        SurfaceFrame(const RenderContext& ctx, const GBuffer& gbuffer, bool replay = false);
        bool ready() const;
        ~SurfaceFrame();
        SurfaceFrame(const SurfaceFrame&) = delete;
        SurfaceFrame& operator=(const SurfaceFrame&) = delete;

    private:
        DebugViewPass* owner_ = nullptr;
        int exceptions_ = 0;
    };
    // All six deferred vertex/backend siblings select their opt-in program here.
    static Shader* surface_variant(Shader* base, bool ladder);
    static void unsupported_surface();
    static bool replaying();
    // Reassert after a backend installs its own depth convention, before its draw.
    static void replay_depth_policy();
    class Frame;
    // Null when disabled, replaying, idle, or another capture owns the producer scope.
    std::shared_ptr<Frame> begin_frame(const RenderContext& ctx);
    // Finalize with current attachments after plants/decals and before lighting/post.
    static void end_frame(std::shared_ptr<Frame>& frame, const RenderContext& ctx);

    DebugViewPass();
    ~DebugViewPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers(); // empty VAO (verts from gl_VertexID)
    void destroy_buffers();
    void reset_shader();

    void set_mode(int mode);
    int mode() const {
        const auto& control = controls();
        if (control.wireframe) {
            return Wireframe;
        }
        if (m_mode >= DrawId && (!control.enabled || m_mode > Wireframe)) {
            return None;
        }
        return control.enabled && control.view >= DrawId && control.view <= Wireframe ? control.view
                                                                                      : m_mode;
    }

    // Draw the debug visualization into the CURRENTLY BOUND framebuffer (the caller binds
    // the screen/backbuffer + sets the viewport). A no-op when mode == None.  pass
    // contract: the G-buffer attachment reads come from the RenderContext handles,
    // and the near/far planes for hardware-depth linearization (Depth mode) from ctx.camera.
    // Fullscreen diagnostics disable depth; wireframe alpha-composites its depth-tested mask.
    void execute(const RenderContext& ctx);

private:
    struct SurfaceState;
    std::unique_ptr<SurfaceState> m_surface;
    struct Snapshot;
    std::unique_ptr<Snapshot> m_snapshot;
    bool prepare_inspector_storage();
    void finish_frame(const RenderContext& ctx);
    bool retain_images(const RenderContext& ctx);
    void inspect_snapshot(const RenderContext& ctx);
    void bind_sources(const RenderContext& ctx, Shader& shader);
    std::unique_ptr<Shader> m_shader;
    std::unique_ptr<Shader> m_contribution_shader;
    unsigned int m_vao = 0;  // empty VAO; the VS builds a fullscreen triangle
    int m_mode = Mode::None; // default-OFF (pass config, not frame state; set via set_mode)
};

} // namespace Luminumbra::Rendering
