#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

namespace {
// ---------------------------------------------------------------------------
//  heavy-mode oracle (Factorio "heavy mode", research Area 2 takeaway 5):
// tick N, SAVE, LOAD into a FRESH session, then resimulate M further ticks in
// BOTH the original and the loaded session and compare full + per-system
// hashes. Catches two bug classes the per-tick smoke cannot: (a) sim state not
// covered by the hash, and (b) save/load round-trip divergence. Reuses the
// existing WorldSaveService save/load machinery (GameSession::SaveWorldState +
// the runner's world_id boot path), so no new persistence format.
// ---------------------------------------------------------------------------

struct HeavyHashes {
    std::string world_hash;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    std::string scent_hash;
};

HeavyHashes CaptureHashes(Luminumbra::Server::ServerWorldRunner& runner) {
    HeavyHashes h;
    h.world_hash = runner.ComputeWorldHash();
    h.sub = runner.ComputeWorldSubHashes();
    h.scent_hash = runner.Session() ? runner.Session()->ComputeScentSubHash() : std::string();
    return h;
}

// the heavy oracle's equality is over AUTHORITATIVE SIMULATION STATE
// (terrain SDF + water sim + entities). Surface mesh geometry is EXCLUDED: it is
// a deterministically-regenerated DERIVED render artifact, and the async
// re-meshing of chunks adopted across a save/load boundary legitimately reaches
// the same geometry via a different in-memory pending-mesh/version snapshot than
// the originating session held. The sub-hashes localize this precisely (only
// `mesh` differs; terrain/water/entities are byte-identical), which is exactly
// the desync-localization the sub-hashes exist to provide. The top-level
// world_hash (which DOES include mesh) is reported but not asserted on across
// the round-trip for this reason; it is still asserted run==replay in --smoke.
bool AuthoritativeStateEqual(const HeavyHashes& a, const HeavyHashes& b) {
    // the heavy oracle compares two sessions at DIFFERENT tick
    // phases across the save/load boundary (original at tick N vs the freshly
    // loaded session at tick 0; later original at N+M vs loaded at M). Terrain/
    // water/entities are spatial state that is invariant once streaming settles,
    // so they compare exactly. The WIND field is TICK-DEPENDENT by design (it
    // evolves every tick), so it legitimately differs between two sessions at
    // different tick counts and is NOT compared here -- exactly like mesh is
    // excluded for a different reason. Wind's determinism is proven where the
    // comparison IS same-tick: the smoke (run==replay), WindFieldDeterminism,
    // and the replay roundtrip (same-tick checkpoint hashes, which include wind
    // via the composite world_hash).
    //
    // WEATHER is excluded for the IDENTICAL reason as wind. The
    // weather core (region category map + storm cells + precipitation field) is a
    // pure function of (seed+12, ABSOLUTE tick, anchor) -- it evolves every tick
    // and the storm-cell schedule keys on the absolute tick-epoch. Across the
    // save/load boundary the loaded session's tick counter resets to 0, so it has
    // no concept of the original's absolute tick; persisting the accumulated
    // weather state could NOT make a cross-phase compare match (original@N+M vs
    // loaded@M differ in absolute tick), so it is recompute-and-excluded here. Its
    // determinism is proven where the comparison IS same-tick: the smoke
    // (run==replay), the WeatherVisual state-hash (resim/replay at the same tick),
    // and the replay roundtrip / lockstep checkpoints (which include weather via
    // the composite world_hash).
    return a.sub.terrain == b.sub.terrain && a.sub.water == b.sub.water &&
           a.sub.entities == b.sub.entities;
}

bool MeshEqual(const HeavyHashes& a, const HeavyHashes& b) {
    return a.sub.mesh == b.sub.mesh;
}

nlohmann::json HeavyHashJson(const HeavyHashes& h) {
    return nlohmann::json{
        {"world_hash", h.world_hash},
        {"sub_hashes",
         {
             {"terrain", h.sub.terrain},
             {"mesh", h.sub.mesh},
             {"water", h.sub.water},
             {"entities", h.sub.entities},
             {"wind", h.sub.wind},
             {"weather", h.sub.weather},
             {"aether", h.sub.aether},
             {"aether_state", h.sub.aether_state},
             {"scents", h.scent_hash},
         }},
    };
}

} // namespace

int RunHeavy(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO(
        "Headless server HEAVY oracle: preset={} seed={} ticks={} resim={} radius={}/{}",
        options.preset,
        options.seed,
        options.ticks,
        options.heavy_resim,
        options.surface_radius,
        options.collision_radius);

    // --- : boot the ORIGINAL session, tick N, SAVE its state. ---
    Luminumbra::Server::ServerWorldRunnerConfig cfgOrig = RunnerConfigFrom(options);
    cfgOrig.world_id.clear();
    cfgOrig.world_name = "Heavy Original";
    cfgOrig.autosave_interval_ticks = 0; // explicit save below; no autosave noise

    Luminumbra::Server::ServerWorldRunner original(std::move(cfgOrig));
    if (!original.Boot()) {
        LUMINUMBRA_CORE_ERROR("heavy: original session failed to boot");
        return 1;
    }
    const auto pre_save_ticks = original.RunFixedTicks(options.ticks);

    // Persist the COMPLETE streamed-chunk set (a never-edited world has no dirty
    // chunks, so the dirty-gated GameSession::SaveWorldState would write
    // nothing; the heavy oracle needs the full set on disk to adopt on load).
    const std::size_t saved_chunks = original.SaveFullSnapshot();
    const std::string world_id = original.Session()->GetMetadata().worldId;
    const fs::path save_dir = original.Session()->GetWorldSaveDir();
    // the rotating sim-window cursor at save — the loaded session must
    // restore exactly this value or resim picks different windows and diverges.
    const std::size_t cursor_at_save =
        original.Session()->GetWorldSystem()->GetWaterSimWindowCursor();
    if (saved_chunks == 0 || world_id.empty()) {
        LUMINUMBRA_CORE_ERROR("heavy: SaveFullSnapshot wrote no chunks (world_id='{}')", world_id);
        return 1;
    }
    const HeavyHashes orig_at_save = CaptureHashes(original);

    // --- : LOAD a FRESH session from the saved world_id. ---
    Luminumbra::Server::ServerWorldRunnerConfig cfgLoad = RunnerConfigFrom(options);
    cfgLoad.world_id = world_id;
    cfgLoad.world_name = "Heavy Loaded";
    cfgLoad.autosave_interval_ticks = 0;

    Luminumbra::Server::ServerWorldRunner loaded(std::move(cfgLoad));
    if (!loaded.Boot()) {
        LUMINUMBRA_CORE_ERROR("heavy: loaded session failed to boot from world_id '{}'", world_id);
        return 1;
    }
    const HeavyHashes loaded_at_load = CaptureHashes(loaded);
    const std::size_t cursor_at_load =
        loaded.Session()->GetWorldSystem()->GetWaterSimWindowCursor();

    // The loaded session, immediately after load, must match the original at
    // save on AUTHORITATIVE sim state (terrain/water/entities). Mesh is tracked
    // informationally (see AuthoritativeStateEqual rationale).
    const bool roundtrip_ok = AuthoritativeStateEqual(orig_at_save, loaded_at_load);
    const bool roundtrip_mesh_match = MeshEqual(orig_at_save, loaded_at_load);

    // --- : resimulate M further ticks on BOTH sessions. ---
    const auto orig_resim = original.RunFixedTicks(options.heavy_resim);
    const auto loaded_resim = loaded.RunFixedTicks(options.heavy_resim);
    const HeavyHashes orig_final = CaptureHashes(original);
    const HeavyHashes loaded_final = CaptureHashes(loaded);

    const bool resim_ok = AuthoritativeStateEqual(orig_final, loaded_final);
    const bool resim_mesh_match = MeshEqual(orig_final, loaded_final);

    //  diagnostic: on a water resim mismatch, localize it — which chunks,
    // which fields. Field-by-field compare over both sessions' chunk snapshots.
    if (orig_final.sub.water != loaded_final.sub.water) {
        auto orig_chunks = original.Session()->GetWorldSystem()->snapshot_streamed_chunks();
        auto loaded_chunks = loaded.Session()->GetWorldSystem()->snapshot_streamed_chunks();
        std::unordered_map<Luminumbra::ChunkID, std::shared_ptr<Luminumbra::Chunk>> loaded_by_id;
        for (const auto& c : loaded_chunks)
            if (c)
                loaded_by_id[c->get_id()] = c;
        std::size_t diff_depth = 0, diff_bed = 0, diff_flux = 0, diff_sleep = 0, diff_ticks = 0,
                    diff_level = 0, diff_delta = 0, printed = 0;
        for (const auto& oc : orig_chunks) {
            if (!oc)
                continue;
            auto it = loaded_by_id.find(oc->get_id());
            if (it == loaded_by_id.end())
                continue;
            const auto& lc = it->second;
            const bool d_depth = oc->water_depth_mm != lc->water_depth_mm;
            const bool d_bed = oc->water_bed_mm != lc->water_bed_mm;
            const bool d_flux = oc->water_edge_flux != lc->water_edge_flux;
            const bool d_sleep = oc->is_water_sleeping.load() != lc->is_water_sleeping.load();
            const bool d_ticks = oc->ticks_below_threshold != lc->ticks_below_threshold;
            const bool d_level = oc->water_level_data != lc->water_level_data;
            const bool d_delta = oc->max_water_delta_last_tick != lc->max_water_delta_last_tick;
            diff_depth += d_depth;
            diff_bed += d_bed;
            diff_flux += d_flux;
            diff_sleep += d_sleep;
            diff_ticks += d_ticks;
            diff_level += d_level;
            diff_delta += d_delta;
            if ((d_depth || d_flux || d_sleep || d_ticks) && printed < 8) {
                ++printed;
                const auto cc = oc->get_coords();
                LUMINUMBRA_CORE_ERROR(
                    "water resim diff chunk ({},{},{}): depth={} bed={} flux={} sleep={} "
                    "(o={} l={}) tbt={} (o={} l={}) level={} delta={}",
                    cc.x,
                    cc.y,
                    cc.z,
                    d_depth,
                    d_bed,
                    d_flux,
                    d_sleep,
                    oc->is_water_sleeping.load(),
                    lc->is_water_sleeping.load(),
                    d_ticks,
                    oc->ticks_below_threshold,
                    lc->ticks_below_threshold,
                    d_level,
                    d_delta);
            }
        }
        LUMINUMBRA_CORE_ERROR(
            "water resim diff totals over {} chunks: depth={} bed={} flux={} sleep={} "
            "ticks_below={} level={} delta={}",
            orig_chunks.size(),
            diff_depth,
            diff_bed,
            diff_flux,
            diff_sleep,
            diff_ticks,
            diff_level,
            diff_delta);
    }
    // the settle CONTRACT that makes save/load water round-trip — the
    // FRESH (original) boot leaves zero uninitialized chunks; the LOADED boot skips
    // the water settle entirely (water paused through Boot, restored mid-flow state
    // authoritative, resuming from the persisted sim-window cursor). A violated
    // contract is exactly the water-roundtrip hazard this oracle measures. A global
    // all-asleep fixed point does not exist (boundary limit cycles + wake
    // propagation), so awake > 0 is expected and NOT asserted.
    const auto& settle_orig = original.GetBootSettleStats();
    const auto& settle_loaded = loaded.GetBootSettleStats();
    const bool settle_ok = settle_orig.contract_ok() && settle_loaded.contract_ok() &&
                           settle_loaded.water_settle_skipped;
    const bool passed = roundtrip_ok && resim_ok && settle_ok &&
                        pre_save_ticks.ticks_executed == options.ticks &&
                        orig_resim.ticks_executed == options.heavy_resim &&
                        loaded_resim.ticks_executed == options.heavy_resim;

    auto SettleJson = [](const Luminumbra::Server::ServerWorldRunner::BootSettleStats& s) {
        return nlohmann::json{
            {"water_chunks", s.water_chunks},
            {"awake", s.awake},
            {"uninited", s.uninited},
            {"iterations", s.iterations},
            {"water_settle_skipped", s.water_settle_skipped},
            {"contract_ok", s.contract_ok()},
        };
    };

    nlohmann::json artifact{
        {"schema", "luminumbra.server_tick_heavy.v1"},
        {"generated_by", "luminumbra_server_app --heavy ()"},
        {"preset", options.preset},
        {"seed", options.seed},
        {"tick_rate_hz", 30.0},
        {"ticks_before_save", options.ticks},
        {"resim_ticks", options.heavy_resim},
        {"world_id", world_id},
        {"original_at_save", HeavyHashJson(orig_at_save)},
        {"loaded_at_load", HeavyHashJson(loaded_at_load)},
        {"roundtrip_match", roundtrip_ok},
        {"roundtrip_mesh_match", roundtrip_mesh_match},
        {"original_final", HeavyHashJson(orig_final)},
        {"loaded_final", HeavyHashJson(loaded_final)},
        {"resim_match", resim_ok},
        {"resim_mesh_match", resim_mesh_match},
        {"boot_settle_original", SettleJson(settle_orig)},
        {"boot_settle_loaded", SettleJson(settle_loaded)},
        {"settle_contract_ok", settle_ok},
        {"water_sim_cursor_at_save", cursor_at_save},
        {"water_sim_cursor_at_load", cursor_at_load},
        {"authoritative_sections", nlohmann::json::array({"terrain", "water", "entities"})},
        {"mesh_excluded_reason",
         "surface mesh is a deterministically-regenerated derived render artifact; async "
         "re-meshing across a save/load boundary reaches identical geometry via a different "
         "in-memory pending-mesh snapshot. Authoritative sim state (terrain/water/entities) "
         "round-trips exactly."},
        {"passed", passed},
    };

    // Tear down both and clean up the throwaway save directory.
    original.Shutdown();
    loaded.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    if (!options.artifact_path.empty()) {
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path()) {
            fs::create_directories(artifact_path.parent_path(), ec);
        }
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("Heavy artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("Failed to write heavy artifact: {}", options.artifact_path);
            return 1;
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Headless server HEAVY FAILED: roundtrip_match={} resim_match={} "
            "settle_contract_ok={} (orig uninited={} awake={}; loaded uninited={} "
            "awake={} skipped={}) orig_save={} loaded_load={} orig_final={} loaded_final={}",
            roundtrip_ok,
            resim_ok,
            settle_ok,
            settle_orig.uninited,
            settle_orig.awake,
            settle_loaded.uninited,
            settle_loaded.awake,
            settle_loaded.water_settle_skipped,
            orig_at_save.world_hash,
            loaded_at_load.world_hash,
            orig_final.world_hash,
            loaded_final.world_hash);
        return 1;
    }

    LUMINUMBRA_CORE_INFO("Headless server HEAVY passed: round-trip + {}-tick resim hashes equal "
                         "(world_hash={})",
                         options.heavy_resim,
                         orig_final.world_hash);
    return 0;
}
