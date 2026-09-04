#include "app/CaveFlourishes.h"

#include "core/Log.h"
#include "debug/DebugCamera.h" // deterministic feature locator (--debug-goto cave|doline)
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/LightingComponents.h" // lumin-crystal cave point lights
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "rendering/Camera.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace Luminumbra::Client::App {

// the  one-time world-entry flourishes (doline
// locate + the 25-anchor enclosed-cave crystal scan + the hero call) cost
// minutes of full-SDF probing in a debug build (est. 10-25 s release) and used
// to run INLINE on the frame-2 main thread — the world-entry stall. They now
// run as a batch of BACKGROUND JobSystem jobs (pure deterministic SDF reads,
// the same sampling meshing workers already do concurrently); the main thread
// polls the handle and creates the point-light entities when the batch lands.
// TEARDOWN CONTRACT: the jobs hold a raw SHIELD_WorldSystem* — every world
// transition (CreateWorld / session reset) MUST DrainBackgroundWorldScan first.
struct BackgroundWorldScan {
    const void* world = nullptr; // identity guard: consume only for the world scanned
    Luminumbra::Systems::SHIELD_WorldSystem::SurfaceBreakInfo doline;
    float doline_surface_h = 0.0f;
    std::optional<Luminumbra::Debug::DebugCamPose> hero;
    std::array<std::optional<glm::vec3>, 25> anchor_caves; // one slot per anchor (disjoint writes)
};
struct BackgroundWorldScanState {
    std::shared_ptr<BackgroundWorldScan> scan; // null = idle/consumed
    Luminumbra::JobHandle handle;
};

BackgroundWorldScanState& GetBackgroundWorldScanState() {
    static BackgroundWorldScanState state;
    return state;
}

void DrainBackgroundWorldScan(Luminumbra::JobSystem& jobs) {
    auto& state = GetBackgroundWorldScanState();
    if (state.handle.counter) {
        jobs.wait(state.handle);
    }
    state.handle = {};
    state.scan.reset();
}

// The body below is the frame loop's flourish region, moved verbatim; only
// the former globals now come in through the context and parameters.
void UpdateCaveFlourishes(ClientAppContext& app,
                          Luminumbra::JobSystem& jobSystem,
                          Luminumbra::world::GameSession& gameSession,
                          Luminumbra::Rendering::Camera* camera) {
    auto& freg = gameSession.GetRegistry();
    auto* fws = gameSession.GetWorldSystem();
    auto& background_scan = GetBackgroundWorldScanState();
    // headless automation skips the  one-time
    // flourishes entirely — captures want the world + shaders, not spelunking
    // cues (and the crystal point lights would move visual baselines).
    // --debug-goto cave still lights its framed cave (below).
    const bool headless_automation =
        app.capture.frame_scan_active || app.capture.scene_active || app.capture.play_paths ||
        !app.capture.render_benchmark_path.empty() || !app.capture.survey_dir.empty() ||
        app.capture.timelapse_frames > 0;
    // dispatch the  scans (doline locate + 25
    // cave anchors + hero call) as ONE background job batch instead of the
    // old frame-2 inline walk (6m25s main-thread in debug, est. 10-25 s
    // release). Pure deterministic SDF reads — the same sampling the meshing
    // workers already run concurrently. Process-once, matching the old
    // statics' behavior. Every world transition drains the handle first
    // (DrainBackgroundWorldScan), so the raw fws capture can never dangle.
    static bool s_backgroundWorldScanDispatched = false;
    if (!s_backgroundWorldScanDispatched && fws && !headless_automation) {
        s_backgroundWorldScanDispatched = true;
        auto scan = std::make_shared<BackgroundWorldScan>();
        scan->world = fws;
        const auto& sp = gameSession.GetMetadata().spawnPoint;
        const glm::vec3 spawn(sp.x, sp.y, sp.z);
        std::vector<Luminumbra::Job> scan_jobs;
        scan_jobs.reserve(26);
        // Job 0: doline locate + the hero enclosed-cave call.
        scan_jobs.emplace_back([scan, fws, spawn]() {
            scan->doline = fws->FindLargestSurfaceBreak(spawn.x, spawn.z, 500.0f);
            if (scan->doline.found) {
                scan->doline_surface_h = fws->GetTerrainHeightAt(scan->doline.x, scan->doline.z);
            }
            scan->hero = Luminumbra::Debug::FindEnclosedCave(*fws, spawn, 256.0f);
        });
        // Jobs 1..25: one enclosed-cave anchor each (disjoint result slots, no
        // locking; dedup happens at consume time in the ORIGINAL scan order so
        // crystal placement stays byte-identical to the old sequential walk).
        for (int gz = -2; gz <= 2; ++gz) {
            for (int gx = -2; gx <= 2; ++gx) {
                constexpr float kAnchorStep = 120.0f;   // metres between anchors
                constexpr float kSearchRadius = 140.0f; // per-anchor search
                const std::size_t slot = static_cast<std::size_t>((gz + 2) * 5 + (gx + 2));
                const glm::vec3 anchorW(spawn.x + static_cast<float>(gx) * kAnchorStep,
                                        spawn.y,
                                        spawn.z + static_cast<float>(gz) * kAnchorStep);
                scan_jobs.emplace_back([scan, fws, anchorW, slot]() {
                    if (auto cave =
                            Luminumbra::Debug::FindEnclosedCave(*fws, anchorW, kSearchRadius)) {
                        scan->anchor_caves[slot] = cave->target;
                    }
                });
            }
        }
        background_scan.handle = jobSystem.dispatch_batch(scan_jobs);
        background_scan.scan = std::move(scan);
        LUMINUMBRA_CORE_INFO(
            " world scan dispatched to background jobs (doline + 25 cave anchors + hero)");
    }

    // Debug suite: --debug-goto cave|doline|spawn — deterministically frame a feature so
    // captures (--timelapse/--frame-scan) can SEE it (the gap that blocked cave shots).
    // Sets the fixed camera; the streaming anchor follows it (far-camera bug already
    // fixed).
    static bool s_debugGotoDone = false;
    if (!s_debugGotoDone && fws && !app.capture.debug_goto.empty()) {
        s_debugGotoDone = true;
        const auto& dsp = gameSession.GetMetadata().spawnPoint;
        const glm::vec3 dnear(dsp.x, dsp.y, dsp.z);
        std::optional<Luminumbra::Debug::DebugCamPose> pose;
        if (app.capture.debug_goto == "cave")
            pose = Luminumbra::Debug::FindEnclosedCave(*fws, dnear, 256.0f);
        else if (app.capture.debug_goto == "doline")
            pose = Luminumbra::Debug::FindDoline(*fws, dnear, 500.0f);
        else if (app.capture.debug_goto == "spawn")
            pose = Luminumbra::Debug::FrameFeature(dnear, 24.0f);
        if (pose) {
            app.capture.fixed_cam_pos = pose->pos;
            app.capture.fixed_cam_yaw = pose->yaw;
            app.capture.fixed_cam_pitch = pose->pitch;
            app.capture.fixed_cam = true;
            if (camera) {
                camera->Position = pose->pos;
                camera->Yaw = pose->yaw;
                camera->Pitch = pose->pitch;
                camera->updateCameraVectors();
            }
            // The crystal scatter is skipped under headless automation, so a
            // captured cave would be pitch-black: light the framed cave from the pose we
            // already computed (no second FindEnclosedCave walk).
            if (app.capture.debug_goto == "cave" && headless_automation) {
                const auto ce = freg.create();
                auto& ctf = freg.emplace<Luminumbra::Components::TransformComponent>(ce);
                ctf.position = Luminumbra::Vec3(pose->target.x, pose->target.y, pose->target.z);
                auto& cpl = freg.emplace<Luminumbra::Components::PointLightComponent>(ce);
                cpl.color = Luminumbra::Vec3(0.55f, 0.85f, 1.0f);
                cpl.intensity = 6.0f; // hero crystal (mirrors the interactive one)
                cpl.radius = 40.0f;
                LUMINUMBRA_CORE_INFO(
                    "--debug-goto cave: hero crystal lit at the framed cave (headless)");
            }
            LUMINUMBRA_CORE_INFO(
                "--debug-goto {}: framed ({:.1f},{:.1f},{:.1f}) yaw {:.0f} pitch {:.0f}",
                app.capture.debug_goto,
                pose->pos.x,
                pose->pos.y,
                pose->pos.z,
                pose->yaw,
                pose->pitch);
        } else {
            LUMINUMBRA_CORE_WARN("--debug-goto {}: no '{}' feature found near spawn",
                                 app.capture.debug_goto,
                                 app.capture.debug_goto);
        }
    }

    // LUMIN CRYSTALS — emissive point lights that light dark caves (so you can
    // see + photograph underground without sunlight) and double as photo subjects.
    // consume the background scan when the batch lands (non-blocking
    // poll of the JobHandle counter). Dedup runs here in the ORIGINAL sequential
    // anchor order with the same 16-cap, so placement is byte-identical to the
    // old inline walk. Client-only point lights (never hashed) — no re-pin.
    if (background_scan.scan && background_scan.scan->world == fws &&
        (!background_scan.handle.counter ||
         background_scan.handle.counter->load(std::memory_order_acquire) <= 0)) {
        const BackgroundWorldScan& scan = *background_scan.scan;
        //  the doline cave-mouth aim cue.
        if (scan.doline.found) {
            LUMINUMBRA_CORE_INFO("Largest doline near spawn: world ({:.1f}, {:.1f}, {:.1f}) "
                                 "radius={:.1f}m depth={:.1f}m shaft={}",
                                 scan.doline.x,
                                 scan.doline_surface_h,
                                 scan.doline.z,
                                 scan.doline.radius,
                                 scan.doline.depth,
                                 scan.doline.shaft ? 1 : 0);
        } else {
            LUMINUMBRA_CORE_INFO(
                "No doline found within 500m of spawn (surface breaks off or sparse).");
        }
        // Crystal scatter: the roof-checked enclosed caverns found near each anchor
        // (cave bug B fix preserved — no floating crystals in open dips).
        constexpr float kMinSepSq = 20.0f * 20.0f; // de-dup nearby hits
        int placed = 0;
        float firstX = 0.0f, firstY = 0.0f, firstZ = 0.0f;
        std::vector<glm::vec3> caveCenters;
        for (const auto& maybe_cave : scan.anchor_caves) {
            if (placed >= 16)
                break;
            if (!maybe_cave)
                continue;
            const glm::vec3 c = *maybe_cave; // interior void point of the cavern
            bool dup = false;
            for (const glm::vec3& prev : caveCenters) {
                const glm::vec3 d = prev - c;
                if (glm::dot(d, d) < kMinSepSq) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            caveCenters.push_back(c);
            const auto e = freg.create();
            auto& tf = freg.emplace<Luminumbra::Components::TransformComponent>(e);
            tf.position = Luminumbra::Vec3(c.x, c.y + 0.6f, c.z);
            auto& pl = freg.emplace<Luminumbra::Components::PointLightComponent>(e);
            pl.color = Luminumbra::Vec3(0.45f, 0.78f, 1.0f); // cyan lumin glow
            pl.intensity = 4.5f;
            pl.radius = 24.0f;
            if (placed == 0) {
                firstX = c.x;
                firstY = c.y;
                firstZ = c.z;
            }
            ++placed;
        }
        LUMINUMBRA_CORE_INFO(
            "Lumin crystals: placed {} enclosed-cave glow point-lights near spawn; "
            "first at world ({:.1f}, {:.1f}, {:.1f})",
            placed,
            firstX,
            firstY,
            firstZ);
        // Hero crystal in the located enclosed cave (aligns with --debug-goto cave).
        // Take the pose by value so the guarded access below reads a local
        // optional (the registry emplaces cannot re-seat it).
        const std::optional<Luminumbra::Debug::DebugCamPose> hero_pose = scan.hero;
        if (hero_pose.has_value()) {
            const Luminumbra::Debug::DebugCamPose& hero = hero_pose.value();
            const auto ce = freg.create();
            auto& ctf = freg.emplace<Luminumbra::Components::TransformComponent>(ce);
            ctf.position = Luminumbra::Vec3(hero.target.x, hero.target.y, hero.target.z);
            auto& cpl = freg.emplace<Luminumbra::Components::PointLightComponent>(ce);
            cpl.color = Luminumbra::Vec3(0.55f, 0.85f, 1.0f);
            cpl.intensity = 6.0f; // hero crystal in the located enclosed cave
            cpl.radius = 40.0f;
            LUMINUMBRA_CORE_INFO(
                "Lumin crystal: hero light in enclosed cave at ({:.1f}, {:.1f}, {:.1f})",
                hero.target.x,
                hero.target.y,
                hero.target.z);
        }
        background_scan.scan.reset();
        background_scan.handle = {};
    }
}

} // namespace Luminumbra::Client::App
