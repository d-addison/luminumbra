#include "SceneSurvey.h"

#include "../core/RuntimeScenarioHarness.h" // Luminumbra::Client::ScenarioHarness::WritePixelBufferPpm
#include "Camera.h"
#include "FrameScan.h"
#include "RenderPipeline.h"
#include "WaterfallDetect.h"
#include "core/Log.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {
namespace {

using Systems::SHIELD_WorldSystem;

struct SurveyTarget {
    std::string label;
    glm::vec3 poi{0.0f}; // the feature to frame
    float back = 40.0f;  // horizontal stand-off (m); the camera is placed this far on the SUN side
    float up = 20.0f;    // height above the poi (m)
    float tod = 0.04f;
    bool eye_level = false; // grass: stand AT the poi at eye height and look across (sun behind)
};

// Central-difference terrain-gradient magnitude at (x,z): the "slope" (rise/run).
// Pure worldgen (valid before the region streams). >0.45 ~ a cliff/rock face.
float terrain_slope(SHIELD_WorldSystem& w, float x, float z, float step) {
    const float hx0 = w.GetTerrainHeightAt(x - step, z);
    const float hx1 = w.GetTerrainHeightAt(x + step, z);
    const float hz0 = w.GetTerrainHeightAt(x, z - step);
    const float hz1 = w.GetTerrainHeightAt(x, z + step);
    const float dx = (hx1 - hx0) / (2.0f * step);
    const float dz = (hz1 - hz0) / (2.0f * step);
    return std::sqrt(dx * dx + dz * dz);
}

// Record the framing INTENT for a POI (camera placed `back` m on the sun side, `up` m above,
// looking at it). The actual sun-aware camera is computed in the capture loop once the sun
// direction for this tod is known, so every face the camera sees is sun-lit.
SurveyTarget
frame_target(const std::string& label, glm::vec3 poi, float back, float up, float tod) {
    SurveyTarget t;
    t.label = label;
    t.poi = poi;
    t.back = back;
    t.up = up;
    t.tod = tod;
    return t;
}

} // namespace

void RunSceneSurvey(GLFWwindow* window,
                    world::GameSession& session,
                    RenderPipeline& pipeline,
                    Camera& camera,
                    const std::filesystem::path& out_dir,
                    const std::filesystem::path& materials_json,
                    bool wireframe) {
    SHIELD_WorldSystem* w = session.GetWorldSystem();
    Systems::PhysicsSystem* phys = session.GetPhysicsSystem();
    if (w == nullptr || phys == nullptr) {
        LUMINUMBRA_CORE_ERROR("SceneSurvey: no world/physics");
        return;
    }
    const glm::vec3 spawn(session.GetMetadata().spawnPoint);
    // Mid-angle sun (NOT near-zenith): a high sun grazes vertical rock/waterfall faces (they read
    // dark) AND has an ill-defined azimuth (so the sun-behind-camera framing degenerates). ~40deg
    // up gives a clear sun azimuth + actually lights the faces the camera is framed to see.
    constexpr float kDayTod = 0.14f;

    std::vector<SurveyTarget> targets;

    // --- WIDE DIAGNOSTIC SCAN: what does this world actually HAVE near spawn? -
    // Scan a ~5km box (step 64m), and SIMULTANEOUSLY pick the best of each scene
    // type: highest+steepest cell (rock/mountain), the most-vegetated biome cell
    // (grass), and the deepest standing water (lake). Log a biome + elevation +
    // water summary so a barren (e.g. all-sand) spawn is OBVIOUS in the report.
    const bool has_biomes = w->biomes_enabled();
    std::array<int, 256> biome_hist{};
    float elev_min = 1e9f, elev_max = -1e9f;
    int water_cells = 0;
    float best_rock = -1.0f;
    glm::vec3 best_rock_pos = spawn; // score = slope * elevation
    float best_grass_h = 0.0f;
    glm::vec3 best_grass_pos = spawn;
    bool grass_found = false;
    int best_grass_biome = -1;
    float best_lake_depth = 0.0f;
    glm::vec3 best_lake_pos(0.0f);
    bool lake_found = false;

    constexpr int kHalf = 40;      // 40 cells each way
    constexpr float kStep = 64.0f; // -> ~5.1 km box
    for (int dz = -kHalf; dz <= kHalf; ++dz) {
        for (int dx = -kHalf; dx <= kHalf; ++dx) {
            const float x = spawn.x + static_cast<float>(dx) * kStep;
            const float z = spawn.z + static_cast<float>(dz) * kStep;
            const float h = w->GetTerrainHeightAt(x, z);
            elev_min = std::min(elev_min, h);
            elev_max = std::max(elev_max, h);
            const float wl = w->WaterLevelAt(x, z);
            if (wl > h + 0.3f) {
                ++water_cells;
                const float depth = wl - h;
                if (depth > best_lake_depth) {
                    best_lake_depth = depth;
                    best_lake_pos = glm::vec3(x, wl, z);
                    lake_found = true;
                }
                continue; // underwater cell is not rock/grass
            }
            const float slope = terrain_slope(*w, x, z, 8.0f);
            // ROCK: steep AND high (mountain faces carry Stone; dunes are steep but low).
            const float rock_score = slope * std::max(0.0f, h);
            if (rock_score > best_rock) {
                best_rock = rock_score;
                best_rock_pos = glm::vec3(x, h, z);
            }
            // GRASS: flattish, above water, in a VEGETATED biome (highest veg density wins).
            if (h > 1.0f && slope < 0.18f) {
                float veg = 0.0f;
                int bid = -1;
                if (has_biomes) {
                    bid = static_cast<int>(w->BiomeIdAt(x, z));
                    if (bid >= 0 && bid < 256) {
                        ++biome_hist[static_cast<std::size_t>(bid)];
                    }
                    veg = w->biome_table().vegetation_for(static_cast<Luminumbra::u8>(bid)).density;
                }
                // Prefer the most-vegetated flat cell; fall back to any flat cell.
                const float score = veg * 10.0f + 1.0f;
                if (score > best_grass_h) {
                    best_grass_h = score;
                    best_grass_pos = glm::vec3(x, h, z);
                    grass_found = true;
                    best_grass_biome = bid;
                }
            }
        }
    }
    {
        std::string top_biomes;
        for (int i = 0; i < 256; ++i)
            if (biome_hist[i] > 0)
                top_biomes += " " + std::to_string(i) + ":" + std::to_string(biome_hist[i]);
        LUMINUMBRA_CORE_INFO("SceneSurvey world scan: elev [{:.0f}..{:.0f}], water_cells={}, "
                             "biomes_enabled={}, biome_hist={}",
                             elev_min,
                             elev_max,
                             water_cells,
                             has_biomes,
                             top_biomes);
    }

    // --- WATERFALLS (framed from downstream, looking UP at the crest, daytime) -
    {
        std::vector<WaterfallSite> sites = DetectWaterfalls(*w);
        std::sort(sites.begin(), sites.end(), [](const WaterfallSite& a, const WaterfallSite& b) {
            return a.drop_height > b.drop_height;
        });
        const int n = std::min<int>(2, static_cast<int>(sites.size()));
        for (int i = 0; i < n; ++i) {
            const glm::vec3 look = (sites[i].crest + sites[i].foot) * 0.5f;
            targets.push_back(frame_target("waterfall_" + std::to_string(i),
                                           look,
                                           16.0f + sites[i].drop_height * 1.1f,
                                           sites[i].drop_height * 0.45f + 4.0f,
                                           kDayTod));
            LUMINUMBRA_CORE_INFO(
                "SceneSurvey waterfall {}: drop {:.1f}m at ({:.0f},{:.0f}) lake_outlet={}",
                i,
                sites[i].drop_height,
                sites[i].crest.x,
                sites[i].crest.z,
                sites[i].lake_outlet);
        }
        if (n == 0)
            LUMINUMBRA_CORE_INFO("SceneSurvey: NO waterfall sites detected in this world");
    }

    // --- ROCK / CLIFF (steepest high cell) -----------------------------------
    targets.push_back(frame_target("cliff_rock", best_rock_pos, 55.0f, 30.0f, kDayTod));
    LUMINUMBRA_CORE_INFO("SceneSurvey rock: score {:.1f} at ({:.0f},{:.0f}) h={:.0f}",
                         best_rock,
                         best_rock_pos.x,
                         best_rock_pos.z,
                         best_rock_pos.y);

    // --- GRASS FIELD (most-vegetated flat cell; eye-level, look across) -------
    {
        SurveyTarget t;
        t.label = "grass_field";
        t.poi = best_grass_pos;
        t.up = 2.2f;
        t.tod = kDayTod;
        t.eye_level = true;
        targets.push_back(t);
        LUMINUMBRA_CORE_INFO("SceneSurvey grass: biome {} at ({:.0f},{:.0f}) found={}",
                             best_grass_biome,
                             best_grass_pos.x,
                             best_grass_pos.z,
                             grass_found);
    }

    // --- LAKE / WATER (deepest standing water) -------------------------------
    if (lake_found) {
        targets.push_back(frame_target("water_body", best_lake_pos, 42.0f, 14.0f, kDayTod));
        LUMINUMBRA_CORE_INFO("SceneSurvey lake: depth {:.1f}m at ({:.0f},{:.0f})",
                             best_lake_depth,
                             best_lake_pos.x,
                             best_lake_pos.z);
    } else {
        LUMINUMBRA_CORE_INFO("SceneSurvey: NO standing water found near spawn");
    }

    // --- Capture each target -------------------------------------------------
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    nlohmann::json index = nlohmann::json::array();

    for (const SurveyTarget& t : targets) {
        // Set tod first so the sun direction for THIS capture is current, then frame with the
        // sun BEHIND the camera (lit faces toward us). u_sunDirection points the way light travels;
        // toward-sun = -that. Stand on the toward-sun side and look back at the POI.
        pipeline.set_time_of_day(t.tod);
        glm::vec3 toward_sun = -pipeline.sun_direction();
        glm::vec3 ts_xz(toward_sun.x, 0.0f, toward_sun.z);
        const float ts_len = glm::length(ts_xz);
        ts_xz = (ts_len > 1e-3f) ? ts_xz / ts_len : glm::vec3(1.0f, 0.0f, 0.0f);

        glm::vec3 cam;
        glm::vec3 look;
        if (t.eye_level) {
            // Stand AT the field at eye height; look ACROSS it in the sun-travel direction
            // (sun behind), gently down so the carpet fills the lower frame.
            cam = t.poi + glm::vec3(0.0f, t.up, 0.0f);
            look = cam - ts_xz * 30.0f - glm::vec3(0.0f, 5.0f, 0.0f);
        } else {
            cam = t.poi + ts_xz * t.back + glm::vec3(0.0f, t.up, 0.0f);
            const float ground = w->GetTerrainHeightAt(cam.x, cam.z);
            cam.y = std::max(cam.y, ground + 2.5f);
            look = t.poi;
        }
        glm::vec3 dir = look - cam;
        const float dlen = glm::length(dir);
        dir = (dlen > 1e-3f) ? dir / dlen : glm::vec3(1.0f, 0.0f, 0.0f);
        camera.Position = cam;
        camera.Yaw = glm::degrees(std::atan2(dir.z, dir.x));
        camera.Pitch = glm::degrees(std::asin(glm::clamp(dir.y, -1.0f, 1.0f)));
        camera.updateCameraVectors();

        const Luminumbra::Vec3 anchor(cam);
        // Stream the new region in (the chunks here are not loaded yet).
        w->update(session.GetRegistry(), anchor, phys);
        w->EnsureSurfaceReadyNear(anchor, phys, 12, 4);
        w->wait_for_streaming_jobs();
        // Settle: render frames so foliage/water/atmosphere + mesh uploads catch up. Longer for
        // water (a river needs ticks to source-fill + flow + the surface mesh to regenerate).
        for (int i = 0; i < 130; ++i) {
            w->update(session.GetRegistry(), anchor, phys);
            pipeline.render_frame(session.GetRegistry(), *w, camera, 1.0f / 60.0f, wireframe);
        }
        //  probe: report the live sim's max water depth (mm) so river-fill is verifiable by DATA
        // even when the dark survey scene / pixel heuristic can't show it.
        LUMINUMBRA_CORE_INFO("SceneSurvey '{}': live max water depth = {} mm",
                             t.label,
                             w->debug_max_water_depth_mm());

        const FrameScanReport rep = ScanFrame(pipeline, fbw, fbh, materials_json);
        WriteFrameScanReport(rep, out_dir / (t.label + ".json"));

        std::vector<unsigned char> px(static_cast<std::size_t>(fbw) *
                                      static_cast<std::size_t>(fbh) * 3u);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        Luminumbra::Client::ScenarioHarness::WritePixelBufferPpm(
            out_dir / (t.label + ".ppm"), fbw, fbh, px);

        LUMINUMBRA_CORE_INFO(
            "SceneSurvey captured '{}' cam=({:.0f},{:.0f},{:.0f}) yaw={:.0f} pitch={:.0f}",
            t.label,
            cam.x,
            cam.y,
            cam.z,
            camera.Yaw,
            camera.Pitch);
        index.push_back({{"label", t.label},
                         {"cam", {cam.x, cam.y, cam.z}},
                         {"poi", {t.poi.x, t.poi.y, t.poi.z}},
                         {"yaw", camera.Yaw},
                         {"pitch", camera.Pitch},
                         {"tod", t.tod},
                         {"ppm", t.label + ".ppm"},
                         {"scan", t.label + ".json"}});
    }

    std::ofstream(out_dir / "survey.json") << index.dump(2) << "\n";
    LUMINUMBRA_CORE_INFO(
        "SceneSurvey: wrote {} POI captures -> {}", targets.size(), out_dir.string());
}

} // namespace Luminumbra::Rendering
