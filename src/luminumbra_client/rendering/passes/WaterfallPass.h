#pragma once

#include "../RenderContext.h"
#include "../WaterfallDetect.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class ParticlePass;
class Shader;

// Waterfall dressing pass extracted from RenderPipeline. The world-deterministic
// site cache, baked sheet geometry, shader, bake, and draw stay together under
// one owner; per-frame state arrives through RenderContext.
class WaterfallPass {
public:
    WaterfallPass();
    ~WaterfallPass();

    void init_shader(const std::filesystem::path& root_path);
    void reset_shader();
    void destroy_geometry();
    void execute(const RenderContext& ctx);

    // CPU payload in stable site order; render-only, never simulation authority.
    struct PreparedGeometry {
        std::vector<WaterfallSite> sites;
        std::vector<float> vertices;
    };

    // CPU-only; the caller must exclude other users of this pass/site cache
    // and retain the immutable world until its job is drained.
    PreparedGeometry build_geometry(const Systems::SHIELD_WorldSystem& world);
    // Main thread: validate sizes and transfer CPU storage, without uploading.
    void begin_adoption(PreparedGeometry geometry);
    // At most 64 KiB of vertices and one spray emitter per call. Main/GL thread.
    bool adopt_next(ParticlePass* particle_pass, const std::filesystem::path& root_path);

    // Synchronous convenience for non-interactive hosts, using the same build
    // and adoption operations. Spray retains the existing 24-emitter cap.
    void prepare(const Systems::SHIELD_WorldSystem& world,
                 ParticlePass* particle_pass,
                 const std::filesystem::path& root_path);

    const std::vector<WaterfallSite>& waterfall_sites(const Systems::SHIELD_WorldSystem& world,
                                                      const WaterfallDetectParams& params = {}) {
        return m_sites.sites_for(world, params);
    }
    WaterfallSiteCache& waterfall_cache() {
        return m_sites;
    }
    const std::unique_ptr<Shader>& shader() const {
        return m_shader;
    }

private:
    WaterfallSiteCache m_sites;
    std::unique_ptr<Shader> m_shader;
    // Baked waterfall sheet geometry. One vertical world-space quad plus one
    // plunge-pool quad (12 verts, interleaved pos+normal) per detected site,
    // packed in fixed-size pages. Render-only, never hashed.
    // One page per loading frame: at most 64 KiB of allocation AND upload.
    // Each site has 12 vertices of 6 floats; keep whole sites within a page.
    static constexpr std::size_t kSitesPerPage = (64 * 1024) / (72 * sizeof(float));
    struct GeometryPage {
        u32 vao = 0;
        u32 vbo = 0;
    };
    std::vector<GeometryPage> m_pages;
    std::vector<WaterfallSite> m_sheet_sites;
    bool m_geometry_built = false;
    std::vector<float> m_pending_vertices;
    std::size_t m_uploaded_floats = 0;
    std::size_t m_adopted_spray = 0;
    bool m_replace_spray = false;
};

} // namespace Luminumbra::Rendering
