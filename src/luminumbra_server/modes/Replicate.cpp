#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  live replication smoke. Boots the authoritative server with N
// avatars, wires an in-process ReplicationServer + a loopback ReplicationClient,
// and each tick broadcasts the avatar states + pumps the client/acks. Asserts the
// client mirrors the server's avatars (within mm) and that an ack flowed back --
// the end-to-end server->client replication loop with the REAL runner (physics-
// settled avatar positions), in the gate harness. RENDER/transport-side only:
// world_hash is untouched (this reads the avatar list, never writes the sim).
// ---------------------------------------------------------------------------
int RunReplicate(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Replication Smoke";
    config.autosave_interval_ticks = 0;
    if (config.avatar_count <= 0) {
        config.avatar_count = 3; // replication needs avatars to replicate
    }

    LUMINUMBRA_CORE_INFO("Headless server REPLICATE smoke: preset={} seed={} ticks={} avatars={}",
                         options.preset,
                         options.seed,
                         options.ticks,
                         config.avatar_count);

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("replicate: session failed to boot");
        return 1;
    }

    auto pair = Luminumbra::Net::MakeLoopbackPair();
    Luminumbra::Net::ReplicationServer server;
    server.AddClient(/*client_id=*/1, pair.first.get());
    Luminumbra::Net::ReplicationClient client(/*player_id=*/1, pair.second.get());
    //  polish: CHUNK-INDEX area of interest. Scope each client's snapshot to the
    // chunk neighbourhood around its own avatar (the grid the world streams on), the
    // scalable interest-management path for a 20+ player persistent server. Radius 3
    // chunks (48 m) comfortably covers the spawn-clustered avatars + NPCs here, so
    // the mirror stays complete while the bucketed-AOI path is exercised in the gate.
    server.SetAoiChunkRadius(/*chunk_radius=*/3, /*chunk_size_mm=*/Luminumbra::CHUNK_SIZE_X * 1000);

    // the loopback client CONTROLS one avatar -- it sends a constant +X
    // move usercmd each tick; the server applies it so that avatar walks. We then
    // assert the avatar actually moved (network input -> server movement loop).
    const std::uint32_t controlled = runner.Avatars().size() > 1 ? 1u : 0u;
    const float initial_x =
        runner.Avatars().empty() ? 0.0f : runner.Avatars()[controlled].position.x;

    // spawn server-side replicated NPC entities (type_id 1 = "animal") in the
    // GameSession registry, tagged ReplicatedComponent. They wander deterministically
    // each tick and replicate through BuildEntityReplStates alongside the avatars --
    // proving heterogeneous entities (NPCs/animals) flow through the same pipeline.
    auto& registry = runner.Session()->GetRegistry();
    auto* world_sys = runner.Session()->GetWorldSystem();
    auto* npc_physics = runner.Session()->GetPhysicsSystem();
    const Luminumbra::Vec3 npc_origin = runner.Session()->GetMetadata().spawnPoint;
    // each NPC is a real Jolt CharacterVirtual (the same physics body the
    // avatars use -- gravity + terrain collision).  polish: instead of a
    // meaningless circle-wander, each NPC is a GOAP agent that PLANS toward a water
    // opportunity and the InstinctLocomotionSystem steers it there (seek + arrival).
    //
    // The water "opportunity" is a positioned entity the planner ranks and the
    // locomotion executor steers toward. Placed a short walk from the NPC line so
    // the animals visibly converge on it. Engine-generic: the action string is
    // game data; the executor only follows the planned target's transform.
    entt::entity water_opportunity = entt::null;
    if (options.npcs > 0) {
        const float wx = npc_origin.x + 2.0f;
        const float wz = npc_origin.z + 14.0f; // ahead of the NPC line
        const float wy = world_sys ? world_sys->GetTerrainHeightAt(wx, wz) : npc_origin.y;
        water_opportunity = registry.create();
        auto& otf = registry.emplace<Luminumbra::Components::TransformComponent>(water_opportunity);
        otf.position = Luminumbra::Vec3(wx, wy, wz);
        auto& opp =
            registry.emplace<Luminumbra::Components::OpportunityComponent>(water_opportunity);
        opp.id = "water-hole";
        opp.action = "drink";
        opp.target = "water-hole";
        opp.need = "thirst";
        opp.satisfaction = 1.0f;
        opp.urgency = 1.0f;
        opp.radius = 0.0f; // unbounded: always a candidate
    }

    struct NpcRec {
        entt::entity e;
        std::size_t char_index;
        Luminumbra::Vec3 start;
    };
    std::vector<NpcRec> npc_recs;
    for (int i = 0; i < options.npcs; ++i) {
        const float bx = npc_origin.x + 6.0f + static_cast<float>(i) * 2.0f;
        const float bz = npc_origin.z - 4.0f;
        const float by = (world_sys ? world_sys->GetTerrainHeightAt(bx, bz) : npc_origin.y) + 1.5f;
        const std::size_t ci =
            npc_physics ? npc_physics->create_avatar_character(Luminumbra::Vec3(bx, by, bz)) : 0u;
        auto e = registry.create();
        auto& tf = registry.emplace<Luminumbra::Components::TransformComponent>(e);
        tf.position = Luminumbra::Vec3(bx, by, bz);
        auto& rep = registry.emplace<Luminumbra::Components::ReplicatedComponent>(e);
        rep.network_id = 1000u + static_cast<std::uint32_t>(i); // distinct from avatar ids
        rep.type_id = 1u; // "animal" archetype (client picks the mesh)
        // GOAP agent: needs drive planning; the planner (GameSession tick slot 2)
        // writes the winning action into ActionPlanComponent each replan.
        auto& agent = registry.emplace<Luminumbra::Components::InstinctAgentComponent>(e);
        agent.actor_id = "npc-" + std::to_string(i);
        agent.archetype = "animal";
        agent.replan_interval_ticks = 10;
        auto& needs = registry.emplace<Luminumbra::Components::NeedsComponent>(e);
        needs.needs = {Luminumbra::Components::Need{"thirst", 0.8f, 0.005f}};
        auto& loco = registry.emplace<Luminumbra::Components::LocomotionProfile>(e);
        loco.move_speed = 2.0f;
        loco.arrival_radius = 1.5f;
        loco.slow_radius = 4.0f;
        npc_recs.push_back({e, ci, Luminumbra::Vec3(bx, by, bz)});
    }

    //  one server-authoritative ballistic ARROW (type_id 2). Fired at tick 10,
    // integrated under gravity, despawned (reliable removed_id) on ground-hit/timeout.
    constexpr std::uint32_t kArrowNetId = 2000u;
    constexpr std::uint64_t kArrowFireTick = 10;
    auto* physics = runner.Session()->GetPhysicsSystem();
    entt::entity arrow_entity = entt::null;
    JPH::BodyID arrow_body; //  real Jolt dynamic body
    bool arrow_active = false;
    bool arrow_seen_by_client = false;
    bool arrow_despawn_signalled = false;

    std::uint64_t executed = 0;
    while (executed < options.ticks) {
        // Client -> server: full +X movement input for its avatar this tick.
        Luminumbra::Net::UsercmdMsg cmd;
        cmd.tick = executed + 1;
        cmd.player_id = controlled;
        cmd.move_x = 32767; // normalized +1.0
        cmd.move_z = 0;
        client.SendUsercmd(cmd);
        server.PumpInbound(); // receive the usercmd (newest-wins)
        if (const Luminumbra::Net::UsercmdMsg* got = server.LatestUsercmd(1)) {
            runner.SetAvatarMove(got->player_id,
                                 static_cast<float>(got->move_x) / 32767.0f,
                                 static_cast<float>(got->move_z) / 32767.0f);
        }
        //  polish: GOAP-driven NPC locomotion. The planner ran inside the
        // PREVIOUS RunFixedTicks (GameSession tick slot 2) and wrote each NPC's
        // ActionPlanComponent; the locomotion executor turns that plan into a wish
        // velocity toward the planned target (seek + arrival). One-tick coupling
        // (plan from  steers N) -- the same pattern the avatar usercmd uses --
        // and fully deterministic (pure function of registry state). Set the wish
        // BEFORE the step so update_avatars (in RunFixedTicks) walks the
        // CharacterVirtual with real gravity + terrain collision.
        if (npc_physics) {
            luminumbra::ai::RunInstinctLocomotionOnTick(registry);
            for (std::size_t i = 0; i < npc_recs.size(); ++i) {
                glm::vec2 wish(0.0f);
                if (const auto* intent =
                        registry.try_get<Luminumbra::Components::LocomotionIntentComponent>(
                            npc_recs[i].e)) {
                    wish = intent->wish_xz;
                }
                npc_physics->set_avatar_wish_velocity(npc_recs[i].char_index, wish);
            }
        }

        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("replicate: tick {} did not advance", executed + 1);
            return 1;
        }
        // mirror each NPC's Jolt CharacterVirtual position (stepped above with
        // gravity + terrain collision) into its replicated transform.
        if (npc_physics) {
            for (const NpcRec& n : npc_recs) {
                if (!registry.valid(n.e))
                    continue;
                registry.get<Luminumbra::Components::TransformComponent>(n.e).position =
                    npc_physics->get_avatar_position(n.char_index);
            }
        }

        // arrow lifecycle with REAL JOLT PHYSICS. Fire once -> a dynamic
        // sphere body that arcs under gravity and COLLIDES with the terrain; its
        // body position drives the replicated transform; despawn (reliable removed_id)
        // when the body comes to rest (sleeps) or times out.
        std::vector<std::uint32_t> tick_removed;
        if (options.arrow && !arrow_active && arrow_entity == entt::null &&
            executed == kArrowFireTick && physics) {
            const Luminumbra::Vec3 from =
                runner.Avatars().empty() ? npc_origin : runner.Avatars()[controlled].position;
            const Luminumbra::Vec3 spawn_pos(from.x, from.y + 1.2f, from.z);
            arrow_body = physics->create_dynamic_sphere(
                spawn_pos, Luminumbra::Vec3(10.0f, 6.0f, 0.0f), 0.12f);
            arrow_entity = registry.create();
            auto& tf = registry.emplace<Luminumbra::Components::TransformComponent>(arrow_entity);
            tf.position = spawn_pos;
            auto& rep = registry.emplace<Luminumbra::Components::ReplicatedComponent>(arrow_entity);
            rep.network_id = kArrowNetId;
            rep.type_id = 2u; // "arrow"
            arrow_active = true;
        }
        if (arrow_active && physics && registry.valid(arrow_entity)) {
            // Jolt stepped the body in RunFixedTicks above; mirror its position.
            const Luminumbra::Vec3 apos = physics->get_body_position(arrow_body);
            registry.get<Luminumbra::Components::TransformComponent>(arrow_entity).position = apos;
            // Despawn when the body comes to REST on the terrain (Jolt slept it), or it
            // fell below the world (no collision under it), or it times out -- whichever
            // first. Reliable removed_id that snapshot either way.
            const bool rested =
                executed > kArrowFireTick + 5 && !physics->body_is_active(arrow_body);
            const bool fell_through = apos.y < npc_origin.y - 30.0f;
            const bool expired = executed > kArrowFireTick + 60; // 2 s @30 Hz
            if (rested || fell_through || expired) {
                physics->destroy_body(arrow_body);
                arrow_body = JPH::BodyID();
                registry.destroy(arrow_entity);
                arrow_entity = entt::null;
                arrow_active = false;
                tick_removed.push_back(kArrowNetId); // reliable despawn this snapshot
            }
        }

        // Snapshot = player avatars + registry-driven entities (NPCs + live arrow).
        std::vector<Luminumbra::Net::ReplEntityState> states =
            Luminumbra::World::BuildAvatarReplStates(runner.Avatars());
        const auto entity_states = Luminumbra::World::BuildEntityReplStates(registry);
        states.insert(states.end(), entity_states.begin(), entity_states.end());
        server.BroadcastSnapshot(executed, states, tick_removed);
        client.PumpInbound(); // apply snapshot (most-recent-wins) + ack
        server.PumpInbound(); // drain the ack

        //  observe: did the client see the typed arrow + its reliable despawn?
        if (client.has_snapshot()) {
            for (const auto& e : client.snapshot().entities) {
                if (e.type_id == 2u)
                    arrow_seen_by_client = true;
            }
            for (std::uint32_t rid : client.snapshot().removed_ids) {
                if (rid == kArrowNetId)
                    arrow_despawn_signalled = true;
            }
        }
    }

    // Verify the client mirrors the server's authoritative avatars.
    const auto& avatars = runner.Avatars();
    const float final_x = avatars.empty() ? 0.0f : avatars[controlled].position.x;
    // The controlled avatar walked +X under network input (>= ~0.5 m over the run).
    const bool moved = (final_x - initial_x) > 0.5f;
    // Client mirrors avatars + the N replicated NPCs.
    const std::size_t expected_entities = avatars.size() + static_cast<std::size_t>(options.npcs);
    bool size_ok = client.has_snapshot() && client.snapshot().entities.size() == expected_entities;
    // the NPCs replicated as typed entities (type_id 1).
    std::size_t npc_seen = 0;
    if (client.has_snapshot()) {
        for (const auto& e : client.snapshot().entities) {
            if (e.type_id == 1u)
                ++npc_seen;
        }
    }
    const bool npcs_ok = npc_seen == static_cast<std::size_t>(options.npcs);
    //  polish: verify the GOAP locomotion actually STEERED the NPCs -- every
    // NPC must have ended meaningfully CLOSER to the water hole it planned toward
    // (not just replicated). This is the behavioural assert for action->locomotion.
    bool npcs_approached_water = true;
    double min_npc_approach_m = 1e9;
    if (options.npcs > 0 && water_opportunity != entt::null && registry.valid(water_opportunity)) {
        const Luminumbra::Vec3 w =
            registry.get<Luminumbra::Components::TransformComponent>(water_opportunity).position;
        auto dist_xz = [&](const Luminumbra::Vec3& p) {
            const float dx = p.x - w.x, dz = p.z - w.z;
            return std::sqrt(dx * dx + dz * dz);
        };
        for (const NpcRec& n : npc_recs) {
            if (!registry.valid(n.e)) {
                npcs_approached_water = false;
                continue;
            }
            const Luminumbra::Vec3 end =
                registry.get<Luminumbra::Components::TransformComponent>(n.e).position;
            const double approached = static_cast<double>(dist_xz(n.start) - dist_xz(end));
            min_npc_approach_m = std::min(min_npc_approach_m, approached);
            if (approached < 2.0)
                npcs_approached_water = false; // >= 2 m closer
        }
    } else {
        min_npc_approach_m = 0.0;
    }
    //  if an arrow was fired, the client must have SEEN it (typed) in flight AND
    // received its reliable despawn.
    const bool arrow_ok = !options.arrow || (arrow_seen_by_client && arrow_despawn_signalled);
    double max_pos_err = 0.0;
    bool ids_ok = size_ok;
    if (size_ok) {
        for (std::size_t i = 0; i < avatars.size(); ++i) {
            const auto& e = client.snapshot().entities[i];
            if (e.entity_id != avatars[i].player_id)
                ids_ok = false;
            max_pos_err =
                std::max(max_pos_err,
                         static_cast<double>(std::abs(Luminumbra::Net::ReplDequantPos(e.px_mm) -
                                                      avatars[i].position.x)));
            max_pos_err =
                std::max(max_pos_err,
                         static_cast<double>(std::abs(Luminumbra::Net::ReplDequantPos(e.py_mm) -
                                                      avatars[i].position.y)));
            max_pos_err =
                std::max(max_pos_err,
                         static_cast<double>(std::abs(Luminumbra::Net::ReplDequantPos(e.pz_mm) -
                                                      avatars[i].position.z)));
        }
    }
    const bool acked = server.AckedSnapshotSeq(1) > 0;
    // bandwidth: MEASURED per-client snapshot bytes (vs the research estimate).
    // est kbps per client = bytes * a realistic 20 Hz snapshot rate * 8 / 1000.
    const std::size_t snapshot_bytes = server.last_broadcast_max_client_bytes();
    const double est_kbps_per_client = static_cast<double>(snapshot_bytes) * 20.0 * 8.0 / 1000.0;
    const bool passed = size_ok && ids_ok && npcs_ok && npcs_approached_water && arrow_ok &&
                        max_pos_err < 0.01 && acked && moved && executed == options.ticks;

    nlohmann::json artifact{
        {"schema", "luminumbra.replication_smoke.v1"},
        {"generated_by", "luminumbra_server_app --replicate ( )"},
        {"preset", options.preset},
        {"seed", options.seed},
        {"ticks", options.ticks},
        {"avatar_count", avatars.size()},
        {"client_has_snapshot", client.has_snapshot()},
        {"client_entity_count", client.has_snapshot() ? client.snapshot().entities.size() : 0u},
        {"final_snapshot_seq", client.has_snapshot() ? client.snapshot().snapshot_seq : 0u},
        {"acked_snapshot_seq", server.AckedSnapshotSeq(1)},
        {"max_position_error_m", max_pos_err},
        {"controlled_avatar", controlled},
        {"controlled_dx_m", final_x - initial_x},
        {"snapshot_bytes_per_client", snapshot_bytes},
        {"est_kbps_per_client_at_20hz", est_kbps_per_client},
        {"npc_count", options.npcs},
        {"npcs_replicated", npc_seen},
        {"npcs_ok", npcs_ok},
        {"npcs_approached_water", npcs_approached_water},
        {"min_npc_approach_m", min_npc_approach_m},
        {"arrow_fired", options.arrow},
        {"arrow_seen_by_client", arrow_seen_by_client},
        {"arrow_despawn_signalled", arrow_despawn_signalled},
        {"arrow_ok", arrow_ok},
        {"size_ok", size_ok},
        {"ids_ok", ids_ok},
        {"ack_flowed", acked},
        {"input_moved_avatar", moved},
        {"passed", passed},
    };

    const fs::path save_dir = runner.Session() ? runner.Session()->GetWorldSaveDir() : fs::path();
    runner.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    if (!options.artifact_path.empty()) {
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path())
            fs::create_directories(artifact_path.parent_path(), ec);
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("Replicate artifact written: {}", options.artifact_path);
        }
    }

    if (!passed) {
        LUMINUMBRA_CORE_ERROR(
            "Replicate smoke FAILED: size_ok={} ids_ok={} npcs_ok={} npcs_approached_water={} (min "
            "{:.2f} m) "
            "arrow_ok={} max_pos_err={:.4f} acked={} moved={} (dx={:.2f}) ticks={}/{}",
            size_ok,
            ids_ok,
            npcs_ok,
            npcs_approached_water,
            min_npc_approach_m,
            arrow_ok,
            max_pos_err,
            acked,
            moved,
            final_x - initial_x,
            executed,
            options.ticks);
        return 1;
    }
    LUMINUMBRA_CORE_INFO("Replicate smoke passed: {} avatars mirrored to client (seq={}, "
                         "acked_seq={}, max_pos_err={:.4f} m); "
                         "network input walked avatar {} +{:.2f} m in X; {} GOAP NPCs approached "
                         "water (min {:.2f} m closer); "
                         "bandwidth {} B/snapshot/client (~{:.1f} kbps @20Hz)",
                         avatars.size(),
                         client.snapshot().snapshot_seq,
                         server.AckedSnapshotSeq(1),
                         max_pos_err,
                         controlled,
                         final_x - initial_x,
                         options.npcs,
                         min_npc_approach_m,
                         snapshot_bytes,
                         est_kbps_per_client);
    return 0;
}
