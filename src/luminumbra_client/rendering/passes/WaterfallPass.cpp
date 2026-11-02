#include "WaterfallPass.h"
#include "core/Profiler.h"
#include "rendering/AttributedGl.h"

#include "ParticlePass.h"
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Luminumbra::Rendering {

WaterfallPass::WaterfallPass() = default;
WaterfallPass::~WaterfallPass() = default;

void WaterfallPass::init_shader(const std::filesystem::path& root_path) {
    // The waterfall falling-sheet shader. Drawn over detected waterfall sites
    // (waterfall_sites) on a world-deterministic site; the dressing is
    // render-only and never hashed. Reuses the basic vertex stage
    // (world-position + normal varyings) the sheet quad is built with.
    m_shader =
        std::make_unique<Shader>((root_path / "res/shaders/basic.vert").string().c_str(),
                                 (root_path / "res/shaders/waterfall.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shader ? m_shader->Id() : 0u, "shader.waterfall");
}

void WaterfallPass::reset_shader() {
    m_shader.reset();
}

void WaterfallPass::destroy_geometry() {
    for (const auto& page : m_pages) {
        if (page.vao)
            glDeleteVertexArrays(1, &page.vao);
        if (page.vbo)
            glDeleteBuffers(1, &page.vbo);
    }
    m_pages = std::vector<GeometryPage>{};
    m_pending_vertices = std::vector<float>{};
    m_uploaded_floats = 0;
    m_adopted_spray = 0;
    m_replace_spray = false;
    m_sheet_sites.clear();
    m_sites.clear();
    m_geometry_built = false;
}

void WaterfallPass::execute(const RenderContext& ctx) {
    LUMIN_PROFILE_ZONE_N("GL WaterfallPass.execute");
    // 7-W. WATERFALL SHEETS (, ): the animated falling-sheet veil drawn
    // over each detected waterfall site. Render-only dressing on a world-
    // deterministic site set (never hashed). Drawn after water, into the lit FBO,
    // as a translucent double-sided veil (depth-test on, depth-write off, blend on
    // so it layers over the scene + each other). A no-op (zero draws) when no
    // sites were baked, so existing visual gates stay byte-stable.
    if (m_geometry_built && !m_pages.empty() && !m_sheet_sites.empty() && m_shader &&
        m_shader->IsValid()) {
        glBindFramebuffer(GL_FRAMEBUFFER, ctx.lit_scene.id);
        glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // into internal lighting FBO

        // Save GL state we toggle so it is restored exactly afterwards.
        const GLboolean blend_was = glIsEnabled(GL_BLEND);
        const GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
        const GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean poly_off_was = glIsEnabled(GL_POLYGON_OFFSET_FILL);

        m_shader->use();
        m_shader->setMat4("model", glm::mat4(1.0f));
        m_shader->setMat4("view", ctx.view);
        m_shader->setMat4("projection", ctx.projection);
        m_shader->setMat3("normalMatrix", glm::mat3(1.0f));
        // The per-frame wall-clock SNAPSHOT (prepare_frame), not a live
        // glfwGetTime read — dispatch must be bit-idempotent per prepared frame
        // (sub-µs sway phase shift; render-only).
        m_shader->setFloat("u_time", ctx.time_seconds);
        m_shader->setVec3("u_camera_pos", ctx.camera->Position);
        const glm::vec3 sun_color =
            (ctx.sun.color != glm::vec3(0.0f)) ? ctx.sun.color : glm::vec3(1.0f);
        m_shader->setVec3("u_sun_color", sun_color);
        // Scene-light the waterfall (bright-waterfall fix): drive u_scene_light from the
        // sun-up factor with a small night floor so the cascade is LIT by the scene and
        // darkens at dusk/night instead of emitting near-white.  (the standalone
        // WaterfallVisual gate never sets this uniform -> it keeps the shader default 1.0).
        const float waterfall_scene_lit = 0.10f + 0.90f * glm::clamp(ctx.sun.intensity, 0.0f, 1.0f);
        m_shader->setVec3("u_scene_light", glm::vec3(waterfall_scene_lit));

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthFunc(GL_GREATER);
        glEnable(GL_DEPTH_TEST); // self-sufficient: don't rely on the water pass leaving it on
        glDepthMask(GL_FALSE);   // translucent veil: don't occlude
        glDisable(GL_CULL_FACE); // double-sided sheet
        // The sheet hugs the cliff face the river carved (same surface WaterPass/terrain
        // draws), so pull it slightly toward the camera in depth to avoid z-fighting /
        // being hidden behind the toe of the drop.
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);

        for (std::size_t i = 0; i < m_sheet_sites.size(); ++i) {
            if (i % kSitesPerPage == 0)
                glBindVertexArray(m_pages[i / kSitesPerPage].vao);
            const WaterfallSite& s = m_sheet_sites[i];
            //  ( T.1): the LIVE upstream water scales the sheet —
            // a dammed/drained crest extinguishes its fall. Reads the live float
            // mirror (one-way from mm, legal after derived-state reclassification); stable within
            // the frame (the sim ticks outside render_frame), so dispatch stays bit-idempotent.
            // Unstreamed crests read neutral 1.0.
            float live = 1.0f;
            if (ctx.world_system != nullptr) {
                live = Rendering::LiveWaterFactorAt(*ctx.world_system, s);
            }
            if (live < 0.02f) {
                continue; // extinguished: no sheet, no plunge pool
            }
            m_shader->setFloat("u_live_factor", live);
            m_shader->setFloat("u_crest_y", s.crest.y);
            m_shader->setFloat("u_foot_y", s.foot.y);
            // 12 verts/site: the vertical sheet (0..5) + the horizontal plunge-pool quad (6..11).
            Luminumbra::Rendering::AttributedGl::Submit(
                glDrawArrays, GL_TRIANGLES, static_cast<GLint>((i % kSitesPerPage) * 12), 12);
        }
        glBindVertexArray(0);

        // Restore GL state: depth writes on, cull/blend/depth-test/poly-offset as they were.
        glDepthMask(GL_TRUE);
        glPolygonOffset(0.0f, 0.0f);
        if (poly_off_was)
            glEnable(GL_POLYGON_OFFSET_FILL);
        else
            glDisable(GL_POLYGON_OFFSET_FILL);
        if (depth_was)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (cull_was)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (blend_was)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
    }
}

// Bake the live waterfall DRESSING for `world`. Builds one vertical
// world-space quad per detected site into m_vao/m_vbo and emits one
// spray emitter per site (capped). The SITES are a pure function of
// the generated world (camera/frame independent, same seed -> same sites), but
// the dressing geometry/spray are never hashed (one-way, regression review/).
WaterfallPass::PreparedGeometry
WaterfallPass::build_geometry(const Systems::SHIELD_WorldSystem& world) {
    // Every world entry gets a fresh survey: seed and water epoch alone
    // cannot distinguish terrain presets. Both entry paths build here.
    m_sites.clear();
    PreparedGeometry geometry;
    const auto& sites = waterfall_sites(world);
    // 12 vertices * 6 floats per site. Check both CPU and GL sizes BEFORE
    // copying/allocating, including the GLint vertex offsets used by execute.
    constexpr std::size_t kFloatsPerSite = 72;
    const auto max_gl_bytes = static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max());
    if (sites.size() > geometry.vertices.max_size() / kFloatsPerSite ||
        sites.size() > max_gl_bytes / sizeof(float) / kFloatsPerSite ||
        sites.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) / 12) {
        throw std::length_error("Waterfall geometry exceeds addressable size");
    }
    geometry.sites = sites;
    auto& verts = geometry.vertices;
    // Interleaved pos(3) + normal(3) per vertex; 12 verts per site = the vertical SHEET (6) +
    // a horizontal plunge-POOL quad at the foot (6) so the fall visibly lands in water (the
    // "connect to water" fix — the pool reads as roiling foam because the shader's fall_t≈1 there).
    verts.reserve(sites.size() * kFloatsPerSite);

    constexpr float kMinWidth = 2.0f; // a sane minimum sheet width (m)
    constexpr float kMinRun = 0.5f;   // ensure the foot is advanced downstream
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    for (const WaterfallSite& s : sites) {
        // Downhill flow azimuth in XZ. Guard div-by-zero on normalize (fallback +X).
        glm::vec2 flow2 = s.flow_dir;
        float flow_len = std::sqrt(flow2.x * flow2.x + flow2.y * flow2.y);
        if (flow_len > 1e-5f) {
            flow2 /= flow_len;
        } else {
            flow2 = glm::vec2(1.0f, 0.0f);
        }
        const glm::vec3 flow_dir(flow2.x, 0.0f, flow2.y);
        // Cross-flow axis (the sheet's horizontal width direction): perpendicular
        // to flow in XZ. cross(up, flow_dir) is unit (both unit + orthogonal).
        const glm::vec3 cross_axis = glm::cross(up, flow_dir);

        const float width = std::max(s.width, kMinWidth);
        const float half_w = width * 0.5f;
        const float run = std::max(s.run_length, kMinRun);

        // Lip (top) at the crest XZ; foot (bottom) advanced downstream by the run.
        // CONNECT-TO-WATER: anchor the lip to the upstream WATER surface where the river/lake
        // actually carries water (WaterLevelAt > terrain), so the sheet starts AT the water
        // instead of floating on dry rock. Falls back to the terrain crest on perched/dry drops.
        const float crest_water = world.WaterLevelAt(s.crest.x, s.crest.z);
        const float top_y = std::max(s.crest.y, crest_water);
        const float bot_y = s.foot.y;
        const glm::vec3 top_center(s.crest.x, top_y, s.crest.z);
        const glm::vec3 bot_center =
            top_center + flow_dir * run + glm::vec3(0.0f, bot_y - top_y, 0.0f);

        // Four corners of the vertical sheet (left/right along the cross axis).
        const glm::vec3 tl = top_center - cross_axis * half_w;
        const glm::vec3 tr = top_center + cross_axis * half_w;
        const glm::vec3 bl = bot_center - cross_axis * half_w;
        const glm::vec3 br = bot_center + cross_axis * half_w;

        // Normal: horizontal, perpendicular to the sheet. cross(cross_axis, up)
        // == flow_dir (the downstream-facing horizontal normal). The sheet is
        // drawn double-sided (cull off) so the exact orientation is cosmetic.
        glm::vec3 n = glm::cross(cross_axis, up);
        float nlen = glm::length(n);
        n = (nlen > 1e-5f) ? (n / nlen) : glm::vec3(0.0f, 0.0f, 1.0f);

        auto push_vert = [&](const glm::vec3& p) {
            verts.push_back(p.x);
            verts.push_back(p.y);
            verts.push_back(p.z);
            verts.push_back(n.x);
            verts.push_back(n.y);
            verts.push_back(n.z);
        };
        // Triangle 1: tl, bl, br; Triangle 2: tl, br, tr.
        push_vert(tl);
        push_vert(bl);
        push_vert(br);
        push_vert(tl);
        push_vert(br);
        push_vert(tr);

        // PLUNGE POOL: a horizontal foamy water quad at the foot so the fall lands IN water
        // (visually connects sheet -> pool). Centred at the foot, sized ~1.6x the sheet width,
        // facing UP; at y≈foot the shader's fall_t≈1 -> roiling plunge foam, so it reads as a pool.
        n = up; // these 6 verts face up (overrides the sheet's horizontal normal in push_vert)
        const float pool_half = std::max(half_w * 1.6f, 2.0f);
        const glm::vec3 pc(bot_center.x, bot_y + 0.10f, bot_center.z);
        const glm::vec3 pa = cross_axis * pool_half; // width axis
        const glm::vec3 pb = flow_dir * pool_half;   // downstream axis
        const glm::vec3 ptl = pc - pa - pb;
        const glm::vec3 ptr = pc + pa - pb;
        const glm::vec3 pbl = pc - pa + pb;
        const glm::vec3 pbr = pc + pa + pb;
        push_vert(ptl);
        push_vert(pbl);
        push_vert(pbr);
        push_vert(ptl);
        push_vert(pbr);
        push_vert(ptr);
    }

    return geometry;
}

void WaterfallPass::begin_adoption(PreparedGeometry geometry) {
    constexpr std::size_t kFloatsPerSite = 72;
    const auto max_bytes = static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max());
    if (geometry.sites.size() > max_bytes / sizeof(float) / kFloatsPerSite ||
        geometry.sites.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) / 12 ||
        geometry.vertices.size() != geometry.sites.size() * kFloatsPerSite)
        throw std::length_error("Invalid prepared waterfall geometry");
    // The CPU preparation already surveyed this world. Replacing GPU geometry
    // must retain that cache for the first audio query after adoption.
    auto prepared_sites = std::move(m_sites);
    destroy_geometry();
    m_sites = std::move(prepared_sites);
    m_sheet_sites = std::move(geometry.sites);
    m_pending_vertices = std::move(geometry.vertices);
    const auto page_count = m_sheet_sites.size() / kSitesPerPage +
                            (m_sheet_sites.size() % kSitesPerPage != 0 ? 1u : 0u);
    if (page_count > m_pages.max_size())
        throw std::length_error("Waterfall page count exceeds addressable size");
    m_pages.reserve(page_count);
    m_replace_spray = true;
}

bool WaterfallPass::adopt_next(ParticlePass* particle_pass,
                               const std::filesystem::path& root_path) {
    if (m_geometry_built || (m_sheet_sites.empty() && !m_replace_spray))
        return true;
    constexpr int kFloatsPerVertex = 6;
    constexpr std::size_t kFloatsPerPage = kSitesPerPage * 72;
    const auto remaining = m_pending_vertices.size() - m_uploaded_floats;
    const auto count = std::min(remaining, kFloatsPerPage);
    if (count != 0) {
        // Exactly one page, bounded to 64 KiB including buffer allocation.
        m_pages.push_back(GeometryPage{.vao = 0, .vbo = 0});
        auto& page = m_pages.back();
        glGenVertexArrays(1, &page.vao);
        glGenBuffers(1, &page.vbo);
        if (page.vao == 0 || page.vbo == 0)
            throw std::runtime_error("Could not allocate waterfall geometry");
        glBindVertexArray(page.vao);
        glBindBuffer(GL_ARRAY_BUFFER, page.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(count * sizeof(float)),
                     nullptr,
                     GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER,
                        0,
                        static_cast<GLsizeiptr>(count * sizeof(float)),
                        m_pending_vertices.data() + m_uploaded_floats);
        // location 0 = aPos, location 1 = aNormal (basic.vert layout).
        glVertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE, kFloatsPerVertex * sizeof(float), reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,
                              3,
                              GL_FLOAT,
                              GL_FALSE,
                              kFloatsPerVertex * sizeof(float),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        PassGl::label_gl_object(GL_VERTEX_ARRAY, page.vao, "waterfall.sheet_vao");
        PassGl::label_gl_object(GL_BUFFER, page.vbo, "waterfall.sheet_vbo");
        m_uploaded_floats += count;
    }
    // Defer emitter replacement until validated adoption advances. CPU
    // preparation, refused payloads and cancellation before adoption leave the
    // old world's spray intact. Empty replacement worlds must clear it too.
    if (m_replace_spray && particle_pass) {
        particle_pass->clear_waterfall_spray();
        m_replace_spray = false;
    }
    constexpr std::size_t kMaxSpray = 24;
    const auto spray_count = particle_pass ? std::min(m_sheet_sites.size(), kMaxSpray) : 0;
    if (m_adopted_spray < spray_count) {
        const auto& site = m_sheet_sites[m_adopted_spray++];
        particle_pass->add_waterfall_spray(root_path / "data/common/particles/waterfall_spray.json",
                                           site.foot,
                                           site.drop_height,
                                           site.width);
    }
    if (m_uploaded_floats != m_pending_vertices.size() || m_adopted_spray != spray_count)
        return false;
    m_pending_vertices = std::vector<float>{};
    m_uploaded_floats = 0;
    m_geometry_built = true;
    LUMINUMBRA_CORE_INFO(
        "[waterfall] prepared {} sites ({} spray emitters)", m_sheet_sites.size(), spray_count);
    return true;
}

void WaterfallPass::prepare(const Systems::SHIELD_WorldSystem& world,
                            ParticlePass* particle_pass,
                            const std::filesystem::path& root_path) {
    begin_adoption(build_geometry(world));
    while (!adopt_next(particle_pass, root_path)) {}
}

} // namespace Luminumbra::Rendering
