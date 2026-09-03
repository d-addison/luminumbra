#include "world/WorldgenPreview.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "core/Log.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h" // Luminumbra::Chunk (renderable chunk coords)
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "rendering/Camera.h"
#include "rendering/FoliageSurface.h" // Rendering::FoliageScatterContext / FoliageSurfaceQuery
#include "rendering/RenderPipeline.h"
#include "rendering/passes/ParticlePass.h" // preview precipitation particles

namespace Luminumbra::Client {

namespace {
// The diorama center: a fixed world column the preview world is streamed around
// and the orbit camera looks at. Y picks the lit valley band (same vantage band
// the menu backdrop frames), so the slice reads as a model on a table.
constexpr float kCenterX = 8.0f;
constexpr float kCenterY = 40.0f;
constexpr float kCenterZ = 8.0f;
// Bounded streaming radius (chunks). Small enough to hold the create-screen
// frame budget at the preview FBO size, while remaining large enough that the
// framed slice fills the diorama.
constexpr int kSurfaceRadius = 4;
constexpr int kCollisionRadius = 0; // no gameplay collision needed for a preview
// Render the ENTIRE bounded slice (all kSurfaceRadius rings) at full-SDF LOD0 so
// the preview shows real near-field detail — caves, overhangs, runtime SDF —
// across the whole diorama, not just the centre chunk. Decoupled from
// kCollisionRadius: with collision==0 the legacy rule put only
// ring 0 at LOD0 and rendered rings 1..4 as coarse heightmap LODs. This is a
//  radius only; it builds no extra gameplay collision (the preview has
// none) and leaves the GAME path / world_hash untouched (game callers omit it).
constexpr int kRenderLod0Radius = kSurfaceRadius;

// Worldgen-preview far-field: the centre-relative world-space radius (meters)
// inside which the streamed far-LOD mesh is discarded — the bounded live slice
// owns that space, so the coarse far mesh never pokes through the fine slice.
// One chunk inside the slice's reach (kSurfaceRadius rings of CHUNK_SIZE_X), so
// the live + far surfaces overlap by a chunk at the handoff (no void band), the
// same one-chunk overlap the first-person far-LOD uses. CHUNK_SIZE_X = 16 m.
constexpr float kPreviewFarInnerRadiusM =
    static_cast<float>((kSurfaceRadius - 1) * Luminumbra::CHUNK_SIZE_X);

// foliage fade band. The preview is a tight diorama (radius_4 ring
// around the center), so keep the fade end well inside the streamed footprint —
// near a believable carpet, no foliage past the bounded slice.
constexpr float kFoliageFadeStartM = 60.0f;
constexpr float kFoliageFadeEndM = 96.0f;
constexpr float kFoliagePreviewDensityScale = 1.6f; // showcase density (render-only)
} // namespace

Luminumbra::Vec3 WorldgenPreview::look_at_center() {
    return Luminumbra::Vec3(kCenterX, kCenterY, kCenterZ);
}

WorldgenPreview::WorldgenPreview() {
    // Seed the pending params with engine defaults so an immediate render (before
    // any set_candidate) still shows a sane world rather than nothing.
    m_pending_params = Systems::TerrainGenParams();
    m_have_pending = true;
    m_pending_dirty = true;
}

WorldgenPreview::~WorldgenPreview() {
    // stop the worker FIRST (it touches m_pending_*); join before any
    // member tears down so a build in flight can't write into freed state.
    m_shutdown.store(true);
    {
        std::lock_guard<std::mutex> lk(m_build_mutex);
        m_build_pending.store(true); // wake the worker so it observes shutdown
    }
    m_build_cv.notify_all();
    if (m_build_thread.joinable()) {
        m_build_thread.join();
    }

    // Drain far-LOD jobs that may still be sampling the live preview world BEFORE we
    // free it below (Codex #6: drain before EVERY live-world free, incl. teardown).
    // The pipeline is constructed before this controller and so outlives it — the
    // pointer is valid here. No-op if the preview never rendered (pointer null).
    if (m_drain_pipeline) {
        m_drain_pipeline->prepare_world_swap();
    }

    if (m_color_texture)
        glDeleteTextures(1, &m_color_texture);
    if (m_depth_rbo)
        glDeleteRenderbuffers(1, &m_depth_rbo);
    if (m_fbo)
        glDeleteFramebuffers(1, &m_fbo);
    // Tear down in dependency order: water holds a SHIELD_WorldSystem*, the world holds
    // collision refs into physics. Drop water -> world for BOTH the pending and live
    // sets (physics destructs last by member order).
    m_pending_water.reset();
    m_pending_world.reset();
    m_water.reset();
    m_world.reset();
}

void WorldgenPreview::ensure_target(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (m_fbo != 0 && width == m_fbo_w && height == m_fbo_h) {
        return;
    }
    m_fbo_w = width;
    m_fbo_h = height;

    if (m_color_texture == 0)
        glGenTextures(1, &m_color_texture);
    glBindTexture(GL_TEXTURE_2D, m_color_texture);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, m_fbo_w, m_fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (m_depth_rbo == 0)
        glGenRenderbuffers(1, &m_depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_fbo_w, m_fbo_h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    if (m_fbo == 0)
        glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color_texture, 0);
    glFramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depth_rbo);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LUMINUMBRA_CORE_ERROR("WorldgenPreview FBO incomplete (status 0x{:x})", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WorldgenPreview::set_candidate(const nlohmann::json& resolved_preset_json,
                                    const std::filesystem::path& data_root,
                                    int seed) {
    // Build candidate TerrainGenParams IN MEMORY via the additive loader seam,
    // with the correct data root (so biome/structure tables resolve right). On a
    // parse failure keep the previous pending params + record the error.
    world::TerrainPresetLoadResult result =
        world::LoadTerrainPresetFromJson(resolved_preset_json, data_root, "<preview-candidate>");
    if (!result.ok) {
        m_last_build_failed = true;
        m_last_error =
            result.errors.empty() ? "candidate preset parse failed" : result.errors.front();
        LUMINUMBRA_CORE_WARN("WorldgenPreview candidate parse failed: {}", m_last_error);
        return;
    }
    // Remember the data root so the foliage scatter set resolves.
    m_data_root = data_root;
    set_params(result.params, seed);
}

void WorldgenPreview::set_params(const Systems::TerrainGenParams& params, int seed) {
    std::lock_guard<std::mutex> lk(m_candidate_mutex);
    m_pending_params = params;
    m_pending_seed = seed;
    m_have_pending = true;
    m_pending_dirty = true;
    // Debounced, latest-wins: every rapid change just re-arms the timer, so a
    // burst of slider changes collapses into a single rebuild when it settles.
    m_debounce_remaining = kDebounceSeconds;
}

void WorldgenPreview::set_weather(Weather w) {
    m_weather = w;
}

void WorldgenPreview::set_time_of_day(float tod01) {
    m_tod = std::clamp(tod01, 0.0f, 1.0f);
}

void WorldgenPreview::orbit(float dyaw_deg, float dpitch_deg) {
    m_yaw += dyaw_deg;
    // Wrap yaw to keep it bounded.
    if (m_yaw >= 360.0f)
        m_yaw -= 360.0f;
    if (m_yaw < 0.0f)
        m_yaw += 360.0f;
    // Pitch: keep above the table (look down) and below straight-down so the
    // diorama always reads as a model, never an underside or a flythrough.
    m_pitch = std::clamp(m_pitch + dpitch_deg, 5.0f, 80.0f);
}

void WorldgenPreview::zoom(float dscroll) {
    // Scroll up (positive) zooms in (smaller distance).
    m_dist = std::clamp(m_dist - dscroll * 8.0f, 40.0f, 320.0f);
}

void WorldgenPreview::reset_view() {
    m_yaw = 45.0f;
    m_pitch = 28.0f;
    m_dist = 120.0f;
}

void WorldgenPreview::start_worker() {
    if (m_build_thread.joinable()) {
        return; // already running
    }
    // The synchronous EnsureSurfaceReadyNear streaming path requires a non-null
    // physics system; create it on the MAIN thread BEFORE the worker exists so the
    // worker never races its construction (it only ever reads m_physics).
    if (!m_physics) {
        m_physics = std::make_unique<Systems::PhysicsSystem>();
        m_physics->startup();
    }
    m_build_thread = std::thread([this] { worker_loop(); });
}

void WorldgenPreview::worker_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_build_mutex);
            // Wait for a build request. CRITICAL: also wait until any PREVIOUS
            // result has been consumed by the main thread (m_build_done cleared in
            // the swap) before starting the next build — otherwise this worker
            // would overwrite m_pending_* while the main thread is moving it out
            // (a data race on the pending world/water/registry). This serialises
            // pending ownership: worker owns it while building; main owns it during
            // the swap; never both at once.
            m_build_cv.wait(lk, [this] {
                return m_shutdown.load() || (m_build_pending.load() && !m_build_done.load());
            });
            if (m_shutdown.load()) {
                return;
            }
            m_build_pending.store(false);
        }
        if (m_shutdown.load()) {
            return;
        }
        // Build the queued candidate into the PENDING members only (CPU/no-GL).
        build_world_pending();
        // Signal the main/render thread to swap pending->live on the GL thread.
        // The worker now blocks (next loop iteration) until that swap clears
        // m_build_done, so pending stays stable for the main thread to move.
        m_build_done.store(true);
    }
}

void WorldgenPreview::build_world_pending() {
    // WORKER THREAD. Writes ONLY m_pending_* members. No GL here: worldgen,
    // EnsureSurfaceReadyNear (meshing) and WaterSystem are all CPU — in the real
    // game they run on worker threads too.

    // Snapshot the latest requested params/seed under the candidate lock.
    Systems::TerrainGenParams params;
    int seed = 0;
    {
        std::lock_guard<std::mutex> lk(m_candidate_mutex);
        params = m_pending_params;
        seed = m_pending_seed;
    }

    m_pending_registry.clear();
    // Drop a stale pending water FIRST (it points at the pending world we replace).
    m_pending_water.reset();
    m_pending_world = std::make_unique<Systems::SHIELD_WorldSystem>(
        /*job_system*/ nullptr, /*water_system*/ nullptr, params, seed);
    // Link a water system (no JobSystem needed — WaterSystem never dereferences it), exactly as
    // the game world does (GameSession). This is what makes a lake/ocean preset build its water
    // meshes and render water as water; without it the water render path crashes on a null system.
    m_pending_water =
        std::make_unique<Systems::WaterSystem>(/*job_system*/ nullptr, m_pending_world.get());
    m_pending_world->SetWaterSystem(m_pending_water.get());
    m_pending_world->EnsureSurfaceReadyNear(
        look_at_center(), m_physics.get(), kSurfaceRadius, kCollisionRadius, kRenderLod0Radius);
    // Pull the streamed chunks into the renderable set (into the PENDING registry).
    m_pending_world->update(m_pending_registry, look_at_center(), m_physics.get());
}

void WorldgenPreview::swap_pending_into_live(Rendering::RenderPipeline& pipeline) {
    // GL/MAIN THREAD. Called only after observing m_build_done == true, at which
    // point the worker is BLOCKED (its loop waits for m_build_done to clear before
    // touching pending again), so the main thread solely owns the pending members
    // here. Move pending->live. Reset the live water before the live world (water
    // holds a SHIELD_WorldSystem*), then adopt the pending set in the same order.
    //
    // FIRST drain any in-flight far-LOD tile-build jobs that are still sampling the
    // OUTGOING live world on worker threads — they hold a pointer to m_world, which
    // we are about to FREE. Without this drain, re-enabling far-LOD for the preview
    // reintroduces the create-screen pan use-after-free. prepare_world_swap waits
    // the FarLodSystem build handles + the  far pass, then nulls their
    // world pointer; the next render_frame re-adopts the freshly-swapped world.
    pipeline.prepare_world_swap();

    m_water.reset();
    m_world = std::move(m_pending_world);
    m_water = std::move(m_pending_water);
    m_registry = std::move(m_pending_registry);
    m_pending_registry.clear();
    m_pending_world.reset();
    m_pending_water.reset();

    m_built_once = true;
    m_last_build_failed = false;
    m_last_error.clear();
    ++m_rebuild_generation; // render-only: the foliage rebuild + tests key off this

    // Release the worker: clear done + wake it so it may start any queued build
    // (it only resumes touching pending now that we've moved it out). A build is
    // still "in flight" iff another candidate was queued (m_build_pending) while
    // this one was building — keep the flag set in that case so the lazy kick
    // stays suppressed; otherwise clear it (this build is fully adopted).
    {
        std::lock_guard<std::mutex> lk(m_build_mutex);
        m_build_done.store(false);
        m_build_inflight.store(m_build_pending.load());
    }
    m_build_cv.notify_one();
}

bool WorldgenPreview::tick(float dt) {
    if (!m_active)
        return false;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lk(m_candidate_mutex);
        if (!m_have_pending || !m_pending_dirty) {
            // No pending change; nothing to rebuild. (A first-use build is forced
            // by render when no world exists yet.)
            return false;
        }
        if (m_debounce_remaining > 0.0f) {
            m_debounce_remaining -= dt;
            if (m_debounce_remaining > 0.0f) {
                return false; // still settling
            }
        }
        // Hand this candidate off to the worker. Clear the dirty flag NOW (under
        // the lock) so subsequent ticks don't re-signal the same build; a new
        // set_params landing mid-build re-arms m_pending_dirty + the debounce,
        // and the next elapsed tick re-signals -> latest-wins.
        m_pending_dirty = false;
        dirty = true;
    }
    if (!dirty)
        return false;

    // the debounce elapsed — SIGNAL the worker rather than building here.
    // The worker snapshots the latest params under m_candidate_mutex, so a change
    // landing between now and the snapshot is still latest-wins. The pending->live
    // swap + generation bump happen later on the GL thread (render path).
    start_worker();
    {
        std::lock_guard<std::mutex> lk(m_build_mutex);
        m_build_inflight.store(true);
        m_build_pending.store(true);
    }
    m_build_cv.notify_one();
    return true;
}

void WorldgenPreview::rebuild_foliage_if_needed(Rendering::RenderPipeline& pipeline) {
    // build the deterministic per-chunk foliage scatter ONCE per actual
    // world rebuild (cached by m_rebuild_generation), mirroring the live-game
    // path (main_client normal-play rebake).: the FoliagePass hashes
    // nothing into world_hash.
    if (m_world == nullptr) {
        return;
    }
    if (m_last_foliage_generation == m_rebuild_generation) {
        return; // already built for this world rebuild
    }
    Rendering::FoliagePass* foliage = pipeline.foliage();
    if (foliage == nullptr) {
        m_last_foliage_generation = m_rebuild_generation; // no pass -> nothing to do, don't retry
        return;
    }

    // Load the scatter archetype set once (the SAME set the live game uses). The
    // data root is the absolute path to data/; the scatter set lives under
    // common/foliage/. If we never received a data root (pure set_params caller),
    // skip the load — the pass stays disabled and renders no foliage.
    if (!m_foliage_loaded && !m_data_root.empty()) {
        if (foliage->load_scatter_set(m_data_root / "common/foliage/scatter_set.json")) {
            foliage->set_fade_distances(kFoliageFadeStartM, kFoliageFadeEndM);
            foliage->set_density_scale(kFoliagePreviewDensityScale);
        }
        m_foliage_loaded = true; // attempt once; on failure the pass stays empty
    }

    // Build the per-chunk scatter inputs from the live world's renderable chunks —
    // derived EXACTLY like the real path (chunk origin/extent, biome id, biome
    // vegetation density), so the preview scatter matches the game.
    m_foliage_scatter.clear();
    const auto& renderable = m_world->get_renderable_chunks();
    m_foliage_scatter.reserve(renderable.size());
    for (const Luminumbra::Chunk* chunk : renderable) {
        if (chunk == nullptr) {
            continue;
        }
        const Luminumbra::IVec3 c = chunk->get_coords();
        const float origin_x = static_cast<float>(c.x * Luminumbra::CHUNK_SIZE_X);
        const float origin_z = static_cast<float>(c.z * Luminumbra::CHUNK_SIZE_Z);
        const float center_x = origin_x + Luminumbra::CHUNK_SIZE_X * 0.5f;
        const float center_z = origin_z + Luminumbra::CHUNK_SIZE_Z * 0.5f;
        const float surf_h = m_world->GetTerrainHeightAt(center_x, center_z);
        // Only the chunk straddling the surface column contributes scatter (skip
        // clearly sub-surface / sky chunks).
        const float chunk_y0 = static_cast<float>(c.y * Luminumbra::CHUNK_SIZE_Y);
        if (surf_h < chunk_y0 || surf_h >= chunk_y0 + Luminumbra::CHUNK_SIZE_Y) {
            continue;
        }
        const Luminumbra::u8 biome_id = m_world->BiomeIdAt(center_x, center_z);
        const float density = m_world->biomes_enabled()
                                  ? m_world->biome_table().vegetation_for(biome_id).density
                                  : 0.3f; // default temperate density when biomes are off
        Rendering::FoliagePass::ChunkScatter cs;
        cs.chunk_xz = glm::ivec2(c.x, c.z);
        cs.origin = glm::vec3(origin_x, 0.0f, origin_z);
        cs.extent_m = static_cast<float>(Luminumbra::CHUNK_SIZE_X);
        cs.biome_id = biome_id;
        cs.density = density;
        m_foliage_scatter.push_back(cs);
    }

    // No wind in the static diorama (the shader still animates sway via u_time);
    // pass the live world as the surface-query context (height/slope/moisture).
    foliage->set_wind(glm::vec2(0.0f, 0.0f));
    Rendering::FoliageScatterContext ctx{m_world.get()};
    foliage->rebuild_instances(m_foliage_scatter,
                               &Rendering::FoliageSurfaceQuery,
                               &ctx,
                               glm::vec3(kCenterX, kCenterY, kCenterZ));

    m_last_foliage_generation = m_rebuild_generation;
}

void WorldgenPreview::configure_camera(Rendering::Camera& cam) const {
    // Orbit/turntable: place the camera on a sphere around the fixed look-at,
    // then point it back at the center. yaw/pitch are the orbit angles; the
    // Camera's own Yaw/Pitch are set so its Front looks at the center.
    const float yaw_r = glm::radians(m_yaw);
    const float pitch_r = glm::radians(m_pitch);
    const glm::vec3 center(kCenterX, kCenterY, kCenterZ);
    glm::vec3 offset;
    offset.x = std::cos(pitch_r) * std::cos(yaw_r);
    offset.y = std::sin(pitch_r);
    offset.z = std::cos(pitch_r) * std::sin(yaw_r);
    cam.Position = center + offset * m_dist;
    // Camera Front must point from Position toward center: invert the offset.
    cam.Yaw = m_yaw + 180.0f;
    cam.Pitch = -m_pitch;
    cam.Zoom = 50.0f; // diorama FOV
    cam.updateCameraVectors();
}

void WorldgenPreview::apply_look(Rendering::RenderPipeline& pipeline) const {
    // Live weather / time-of-day through the SAME pipeline knobs the game uses.
    Rendering::WeatherType wt = Rendering::WeatherType::None;
    float intensity = 0.0f;
    switch (m_weather) {
        case Weather::Clear:
            wt = Rendering::WeatherType::None;
            intensity = 0.0f;
            break;
        case Weather::Rain:
            wt = Rendering::WeatherType::Rain;
            intensity = 0.7f;
            break;
        case Weather::Snow:
            wt = Rendering::WeatherType::Snow;
            intensity = 0.7f;
            break;
        case Weather::Fog:
            wt = Rendering::WeatherType::Fog;
            intensity = 0.6f;
            break;
        case Weather::Storm:
            wt = Rendering::WeatherType::Storm;
            intensity = 0.9f;
            break;
    }
    pipeline.set_weather(wt, intensity);
    pipeline.set_time_of_day(m_tod);
    Rendering::CloudRenderState clouds;
    clouds.enabled = true;
    clouds.coverage_amount = (m_weather == Weather::Clear) ? 0.35f : 0.7f;
    clouds.plane_height = 900.0f;
    pipeline.set_cloud_state(clouds);
}

void WorldgenPreview::apply_precipitation(Rendering::RenderPipeline& pipeline,
                                          const Rendering::Camera& cam) {
    // The preview's set_weather already drives the sky/cloud/fog tint via
    // apply_look; this adds the REAL falling particles so rain/snow/storm read
    // as precipitation, camera-followed (the column always spawns around/above the
    // orbit camera and falls straight past it). Render-only -> never hashed.
    auto* particles = pipeline.particles();
    if (particles == nullptr)
        return;
    using PP = Rendering::ParticlePass;
    // data root may be unset (tests / before the first candidate); without it we
    // can't resolve the emitter JSON, so leave precipitation off rather than guess.
    if (m_data_root.empty())
        return;

    // Which precip file does the current weather want? Storm reuses the rain
    // column (heavier look comes from the wind + sky); fog/clear carry none.
    const char* precip_file = nullptr;
    if (m_weather == Weather::Rain || m_weather == Weather::Storm)
        precip_file = "common/particles/precip_rain.json";
    else if (m_weather == Weather::Snow)
        precip_file = "common/particles/precip_snow.json";

    if (m_weather != m_precip_spawned_for) {
        // Weather changed: drop the previous emitters and (re)spawn for the new
        // weather. clear_emitters flushes the whole pass — fine on the create
        // screen, where the preview owns the only particles (the menu backdrop is
        // suppressed and the in-game ambient/foliage emitters are not running).
        particles->clear_emitters();
        m_precip_emitter_id = PP::kInvalidEmitter;
        m_precip_spawned_for = m_weather;
        if (precip_file != nullptr) {
            m_precip_emitter_id = particles->add_emitter(m_data_root / precip_file, cam.Position);
            // Rain/storm splash on the ground plane (snow just settles).
            if (m_weather == Weather::Rain || m_weather == Weather::Storm)
                particles->add_splash_emitter(m_data_root / "common/particles/precip_splash.json");
        }
    }

    // Camera-follow: keep the spawn column centred on the orbit camera so it always
    // fills the framed view as the turntable spins.
    if (m_precip_emitter_id != PP::kInvalidEmitter)
        particles->set_emitter_origin(m_precip_emitter_id, cam.Position);

    // Storms slant the rain with a steady cross-wind; calm weather falls straight.
    particles->set_wind(m_weather == Weather::Storm ? glm::vec3(9.0f, 0.0f, 4.0f)
                                                    : glm::vec3(0.0f));
}

void WorldgenPreview::clear_precipitation(Rendering::RenderPipeline& pipeline) {
    using PP = Rendering::ParticlePass;
    if (m_precip_spawned_for == Weather::Clear && m_precip_emitter_id == PP::kInvalidEmitter)
        return; // nothing spawned -> cheap no-op
    if (auto* particles = pipeline.particles()) {
        particles->clear_emitters();
        particles->set_wind(glm::vec3(0.0f));
    }
    m_precip_emitter_id = PP::kInvalidEmitter;
    m_precip_spawned_for = Weather::Clear;
}

bool WorldgenPreview::render(Rendering::RenderPipeline& pipeline, float dt) {
    if (!m_active)
        return false;
    m_drain_pipeline =
        &pipeline; // remember it so the dtor can drain far-LOD before freeing the world
    if (m_fbo == 0) {
        return false; // no target allocated yet
    }
    // adopt any completed background build (GL thread). swap clears
    // m_build_done + wakes the worker. Lazy first build signals the worker and
    // waits via the same path (no world until it lands).
    if (m_build_done.load()) {
        swap_pending_into_live(pipeline);
    }
    if (!m_built_once && m_world == nullptr && !m_first_build_requested &&
        !m_build_inflight.load()) {
        // Lazy first build: kick the worker ONCE so a screen that becomes active
        // renders as soon as the build lands. Guarded by m_build_inflight so it
        // does NOT re-signal when a build was already queued (e.g. tick signalled
        // it) or is mid-build — which would queue a spurious extra rebuild.
        m_first_build_requested = true;
        start_worker();
        {
            std::lock_guard<std::mutex> lk(m_build_mutex);
            m_build_inflight.store(true);
            m_build_pending.store(true);
        }
        m_build_cv.notify_one();
    }
    if (m_world == nullptr) {
        return false; // build still pending; caller keeps the last good frame
    }

    rebuild_foliage_if_needed(pipeline);

    Rendering::Camera cam(glm::vec3(kCenterX, kCenterY, kCenterZ + m_dist));
    configure_camera(cam);
    apply_look(pipeline);

    // Redirect the engine pipeline's final blit into the preview FBO and size the
    // internal passes to the preview dims for this frame, then restore. (Costly on
    // a shared pipeline — used only for one-shot headless capture/tests.)
    const u32 prev_w = pipeline.screen_width();
    const u32 prev_h = pipeline.screen_height();
    pipeline.on_resize(static_cast<u32>(m_fbo_w), static_cast<u32>(m_fbo_h));
    pipeline.set_offscreen_target(m_fbo, static_cast<u32>(m_fbo_w), static_cast<u32>(m_fbo_h));

    // Far-LOD OFF for the transient (swapped + freed) preview world — see render_to_backbuffer.
    pipeline.set_far_lod_enabled(false);
    pipeline.render_frame(m_registry, *m_world, cam, dt, /*wireframe*/ false);
    pipeline.set_far_lod_enabled(true);

    pipeline.clear_offscreen_target();
    if (prev_w != 0 && prev_h != 0) {
        pipeline.on_resize(prev_w, prev_h);
    }
    return true;
}

bool WorldgenPreview::render_to_backbuffer(Rendering::RenderPipeline& pipeline, float dt) {
    if (!m_active)
        return false;
    m_drain_pipeline =
        &pipeline; // remember it so the dtor can drain far-LOD before freeing the world
    // adopt any completed background build (GL thread) before rendering.
    // swap clears m_build_done + wakes the worker (and drains far-LOD off the
    // outgoing world first, so re-enabling far-LOD below stays use-after-free safe).
    if (m_build_done.load()) {
        swap_pending_into_live(pipeline);
    }
    if (!m_built_once && m_world == nullptr && !m_first_build_requested &&
        !m_build_inflight.load()) {
        // Lazy first build: kick the worker ONCE (see render for the rationale).
        m_first_build_requested = true;
        start_worker();
        {
            std::lock_guard<std::mutex> lk(m_build_mutex);
            m_build_inflight.store(true);
            m_build_pending.store(true);
        }
        m_build_cv.notify_one();
    }
    if (m_world == nullptr) {
        return false; // build still pending; caller keeps the last good frame
    }

    rebuild_foliage_if_needed(pipeline);

    Rendering::Camera cam(glm::vec3(kCenterX, kCenterY, kCenterZ + m_dist));
    configure_camera(cam);
    apply_look(pipeline);
    // spawn/maintain the camera-followed precipitation particles BEFORE
    // render_frame (which advances + draws the ParticlePass internally), so rain/
    // snow/storm show real falling particles in the live diorama.
    apply_precipitation(pipeline, cam);

    // Draw straight to the backbuffer at the pipeline's CURRENT (full-screen) size
    // no offscreen target, no on_resize. The create panel frames this as the
    // diorama "window"; the menu backdrop is suppressed by the host while active,
    // so this is the single world render on the create screen.
    //
    // Far-LOD ON, but anchored to the diorama CENTRE (look_at_center) rather than
    // the orbiting camera. The diorama is a BOUNDED radius-4 slice; left camera-
    // anchored, the streaming far field re-streams/evicts tiles as the turntable
    // spins (orbit churn) and carves a moving void disc around the camera. Pinning
    // the anchor to the fixed centre gives a FIXED tile set (no churn) + a real
    // distant vista beyond the slice, and the centre-relative inner discard hides
    // the coarse far mesh under the fine live slice (no poke-through, no void band
    // at the slice edge). Crash-safety is UNCHANGED: swap_pending_into_live + the
    // dtor still drain any far-LOD job before the candidate world is freed (the real
    // UAF fix). The anchor is cleared right after the frame so a subsequent normal
    // game render never inherits the preview's fixed anchor.
    pipeline.set_far_lod_enabled(true);
    pipeline.set_far_lod_preview_anchor(look_at_center(), kPreviewFarInnerRadiusM);
    pipeline.render_frame(m_registry, *m_world, cam, dt, /*wireframe*/ false);
    pipeline.clear_far_lod_preview_anchor();
    return true;
}

} // namespace Luminumbra::Client
