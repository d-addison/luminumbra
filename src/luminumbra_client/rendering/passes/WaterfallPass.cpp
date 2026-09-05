#include "WaterfallPass.h"

#include "ParticlePass.h"
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <algorithm>
#include <cmath>

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
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
    m_sheet_sites.clear();
    m_geometry_built = false;
}

void WaterfallPass::execute(const RenderContext& ctx) {
    // 7-W. WATERFALL SHEETS (, ): the animated falling-sheet veil drawn
    // over each detected waterfall site. Render-only dressing on a world-
    // deterministic site set (never hashed). Drawn after water, into the lit FBO,
    // as a translucent double-sided veil (depth-test on, depth-write off, blend on
    // so it layers over the scene + each other). A no-op (zero draws) when no
    // sites were baked, so existing visual gates stay byte-stable.
    if (m_geometry_built && m_vao != 0 && !m_sheet_sites.empty() && m_shader &&
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
        glEnable(GL_DEPTH_TEST); // self-sufficient: don't rely on the water pass leaving it on
        glDepthMask(GL_FALSE);   // translucent veil: don't occlude
        glDisable(GL_CULL_FACE); // double-sided sheet
        // The sheet hugs the cliff face the river carved (same surface WaterPass/terrain
        // draws), so pull it slightly toward the camera in depth to avoid z-fighting /
        // being hidden behind the toe of the drop.
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);

        glBindVertexArray(m_vao);
        for (std::size_t i = 0; i < m_sheet_sites.size(); ++i) {
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
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(i * 12), 12);
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
void WaterfallPass::prepare(const Systems::SHIELD_WorldSystem& world,
                            ParticlePass* particle_pass,
                            const std::filesystem::path& root_path) {
    // Re-bakeable: drop any prior geometry so a re-enter rebuilds cleanly.
    destroy_geometry();

    // World-deterministic detection (cached). Copy into m_sheet_sites so the
    // per-site crest/foot Y survive even if the cache is later cleared.
    const std::vector<WaterfallSite>& sites = waterfall_sites(world);
    if (sites.empty()) {
        LUMINUMBRA_CORE_INFO("[waterfall] prepare_waterfalls: 0 sites detected (no dressing)");
        return;
    }
    m_sheet_sites = sites;

    // Interleaved pos(3) + normal(3) per vertex; 12 verts per site = the vertical SHEET (6) +
    // a horizontal plunge-POOL quad at the foot (6) so the fall visibly lands in water (the
    // "connect to water" fix — the pool reads as roiling foam because the shader's fall_t≈1 there).
    constexpr int kFloatsPerVertex = 6;
    constexpr int kVertsPerSite = 12;
    std::vector<float> verts;
    verts.reserve(m_sheet_sites.size() * kVertsPerSite * kFloatsPerVertex);

    constexpr float kMinWidth = 2.0f; // a sane minimum sheet width (m)
    constexpr float kMinRun = 0.5f;   // ensure the foot is advanced downstream
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    for (const WaterfallSite& s : m_sheet_sites) {
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

    // Upload the combined sheet geometry to a dedicated VAO/VBO.
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(),
                 GL_STATIC_DRAW);
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
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "waterfall.sheet_vao");
    PassGl::label_gl_object(GL_BUFFER, m_vbo, "waterfall.sheet_vbo");
    m_geometry_built = true;

    // Spray: one mist emitter per plunge foot, capped to bound particle cost.
    constexpr std::size_t kMaxSpray = 24;
    std::size_t spray_count = 0;
    if (particle_pass) {
        const std::filesystem::path spray_json =
            root_path / "data/common/particles/waterfall_spray.json";
        for (const WaterfallSite& s : m_sheet_sites) {
            if (spray_count >= kMaxSpray)
                break;
            particle_pass->add_waterfall_spray(spray_json, s.foot, s.drop_height, s.width);
            ++spray_count;
        }
    }

    if (m_sheet_sites.size() > kMaxSpray) {
        LUMINUMBRA_CORE_INFO(
            "[waterfall] prepare_waterfalls: {} sites; sheets drawn for all, spray capped to {}",
            m_sheet_sites.size(),
            kMaxSpray);
    } else {
        LUMINUMBRA_CORE_INFO(
            "[waterfall] prepare_waterfalls: {} sites (sheets + {} spray emitters)",
            m_sheet_sites.size(),
            spray_count);
    }
}

} // namespace Luminumbra::Rendering
