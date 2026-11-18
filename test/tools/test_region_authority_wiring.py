"""Source checks for independent mesh publication and pending work."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp"


def function(source, name):
    # Mask comments and literals before balancing production C++ braces.
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                    lambda match: " " * len(match.group()), source, flags=re.S)
    start = masked.index("{", source.index(name))
    depth = 1
    for index in range(start + 1, len(masked)):
        depth += (masked[index] == "{") - (masked[index] == "}")
        if depth == 0:
            body = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                          lambda match: match.group() if match.group().startswith('"') else " ",
                          source[start + 1:index], flags=re.S)
            return re.sub(r"\s+", " ", body).strip()
    raise AssertionError(f"Unclosed function {name}")


class RegionAuthorityWiring(unittest.TestCase):
    def test_eviction_ownership_probe_excludes_streaming_residency(self):
        session = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(
            encoding="utf-8")
        schedule = function(session, "void GameSession::RunRegionScheduler(")
        enabled = function(schedule, "if (m_activeRegionsEnabled && !m_regionFault)")
        probe = function(enabled, "[this, &owned_regions](RegionKey key)")
        opt_in = function(probe, "if (m_authorityProfile.residency_free_eviction)")
        self.assertEqual(opt_in, "return owned_regions->contains(key);")
        self.assertNotIn("has_region_members", opt_in)
        self.assertNotIn("m_worldSystem", opt_in)
        for owner in ("m_groundObjects.owns_region(key)", "m_fieldPages.owns_region(key)",
                      "m_energyFieldState->OwnsRegion(key)", "m_regionWork",
                      "PlantRegionComponent", "owned_regions->contains(key)"):
            self.assertIn(owner, probe)
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(
            encoding="utf-8")
        peer = function(native, "TEST(GameSessionHeadlessWorldTest, "
                        "IdleRetentionUsesWorkRegardlessOfStreamedResidency)")
        self.assertIn("const GameSession::AuthorityProfileOptions profile{.residency_free_eviction = true};", peer)
        self.assertIn("GameSession session(profile, GameSession::AuthorityProfileTestOnly{}),", peer)
        self.assertIn("peer(profile, GameSession::AuthorityProfileTestOnly{});", peer)
        for assertion in (
                "EXPECT_EQ(session.GetRegionSchedule(), peer.GetRegionSchedule());",
                "EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), "
                "peer.GetActiveRegionLedger().canonical_bytes());",
                "EXPECT_EQ(session.GetActiveRegionLedger().encode(), "
                "peer.GetActiveRegionLedger().encode());",
                "EXPECT_EQ(session.ComputeChunkStateHash(), peer.ComputeChunkStateHash());",
                "EXPECT_EQ(SessionWorldHash(session), SessionWorldHash(peer));"):
            self.assertGreaterEqual(peer.count(assertion), 2)
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("common/GameSessionHeadlessWorld_test.cpp", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_profile_opt_in_is_reachable_only_from_tests(self):
        # Until slice I binds the option to negotiated identity, no production
        # binary may construct a non-default profile. The opt-in constructor is
        # declared in the header but defined only in a test-only source, so a
        # production caller fails to link instead of relying on a nameable friend.
        header = (ROOT / "src/luminumbra_common/world/GameSession.h").read_text(
            encoding="utf-8")
        tag = function(header, "struct AuthorityProfileTestOnly")
        self.assertEqual(tag, "explicit AuthorityProfileTestOnly() = default;")
        self.assertIn("GameSession(const AuthorityProfileOptions& authority_profile, "
                      "AuthorityProfileTestOnly);", header)
        self.assertIn("GameSession(const AuthorityProfileOptions& authority_profile, "
                      "AuthorityProfileTag);", header)
        private = header[header.index("\nprivate:\n", header.index("class GameSession {")):]
        self.assertIn("struct AuthorityProfileTag {};", private)
        self.assertNotIn("friend struct GameSessionAuthorityProfileTestAccess", header)
        self.assertNotIn("AuthorityProfileTestKey", header)
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in (".h", ".hpp", ".cpp", ".cc", ".inl"):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            rel = path.relative_to(ROOT).as_posix()
            if rel != "src/luminumbra_common/world/GameSession.h":
                self.assertNotIn("AuthorityProfileTestOnly", text, rel)
            if rel not in ("src/luminumbra_common/world/GameSession.h",
                           "src/luminumbra_common/world/GameSession.cpp"):
                self.assertNotIn("AuthorityProfileTag", text, rel)
                self.assertNotIn("residency_free_eviction", text, rel)
        # The seam source is the only definition, and only a test target compiles it.
        seam_name = "GameSessionAuthorityProfileTestSeam.cpp"
        seam = (ROOT / "test/common" / seam_name).read_text(encoding="utf-8")
        self.assertIn("AuthorityProfileTestOnly /*test_only*/)", seam)
        self.assertIn(": GameSession(authority_profile, AuthorityProfileTag{}) {}", seam)
        for path in sorted(ROOT.rglob("CMakeLists.txt")) + sorted(ROOT.rglob("*.cmake")):
            rel = path.relative_to(ROOT).as_posix()
            if rel.startswith(("build/", ".git/", "vendor/", "_deps/")) or "/_deps/" in rel:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            expected = 1 if rel == "test/CMakeLists.txt" else 0
            self.assertEqual(text.count(seam_name), expected, rel)

    def test_default_eviction_preserves_legacy_residency_probe(self):
        session = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(
            encoding="utf-8")
        schedule = function(session, "void GameSession::RunRegionScheduler(")
        enabled = function(schedule, "if (m_activeRegionsEnabled && !m_regionFault)")
        probe = function(enabled, "[this, &owned_regions](RegionKey key)")
        self.assertTrue(probe.endswith(
            "} return owned_regions->contains(key) || "
            "(m_worldSystem && m_worldSystem->has_region_members(key.x, key.z));"))
        self.assertEqual(probe.count("m_authorityProfile.residency_free_eviction"), 1)
        header = (ROOT / "src/luminumbra_common/world/GameSession.h").read_text(
            encoding="utf-8")
        self.assertEqual(function(header, "struct AuthorityProfileOptions"),
                         "bool residency_free_eviction = false;")
        self.assertIn("const AuthorityProfileOptions m_authorityProfile;", header)
        self.assertIn(": GameSession(AuthorityProfileOptions{}, AuthorityProfileTag{}) {}", session)
        self.assertIn(": m_authorityProfile(authority_profile)", session)
        # Construction and the single probe gate are its only production uses.
        # In particular, preparation, config, hashes and save writers cannot read it.
        self.assertEqual(session.count("m_authorityProfile"), 2)
        self.assertEqual(session.count("residency_free_eviction"), 1)
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(
            encoding="utf-8")
        default = function(native, "TEST(GameSessionHeadlessWorldTest, "
                           "IdleRetentionMatchesWorkAndAllResidentOwnership)")
        self.assertIn("GameSession session;", default)
        self.assertNotIn("AuthorityProfileOptions", default)
        self.assertIn("pending, duplicate, zero_count, resident, pristine, loading, pristine_loading", default)
        self.assertIn("DormantEvictionProfileDoesNotAffectDisabledRegions", native)

    def test_far_authority_capture_is_bounded_and_regressions_are_registered(self):
        source = SOURCE.read_text(encoding="utf-8")
        capture = function(source, "SHIELD_WorldSystem::capture_far_lod_sdf_snapshot(")
        for full_scan in ("snapshot_authoritative_chunks(", "snapshot_streamed_chunks(",
                          "snapshot_parked_chunks(",
                          "m_streaming_state.chunks", "m_parked)"):
            self.assertNotIn(full_scan, capture)
        for token in ("m_activation_index.columns()", "m_parked_columns.find(key)",
                      "++m_far_lod_capture_input_items;", "z <= max_chunk_z", "x <= max_chunk_x",
                      "HasPublishedFarLodSdf(chunk.get())", "std::sort(snapshot->entries.begin()"):
            self.assertIn(token, capture)
        membership = function(source, "void SHIELD_WorldSystem::note_digest_chunk(")
        self.assertLess(membership.index("m_parked_columns[key].insert_or_assign"),
                        membership.index("if (!m_durable_residency)"))
        self.assertIn("m_parked_columns.erase(column)", membership)
        self.assertIn("m_parked_columns.clear()", function(
            source, "void SHIELD_WorldSystem::clear_world("))
        native = (ROOT / "test/common/world_streaming_hardening_test.cpp").read_text(encoding="utf-8")
        for case in ("FarAuthorityCaptureMatchesFullScanAcrossSeededResidencyChanges",
                     "FarAuthorityCaptureTickVisitsOnlyFootprintParticipants"):
            self.assertIn("TEST(StreamingHardening, " + case + ")", native)
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("common/world_streaming_hardening_test.cpp", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_suite_is_discovered_by_registered_replay_contract(self):
        runner = (ROOT / "tools/ci/test_replay_source_contract.py").read_text(encoding="utf-8")
        self.assertIn("def load_tests(loader, tests, pattern):", runner)
        self.assertIn('loader.discover(str(ROOT / "test/tools"),', runner)
        self.assertIn('pattern="test_region_authority_wiring.py"', runner)
        plan = (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8")
        self.assertIn('"tools/ci/test_replay_source_contract.py",', plan)
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('add_test(NAME ReplaySourceContract COMMAND "${Python3_EXECUTABLE}" -I -B', cmake)
        self.assertIn('${CMAKE_SOURCE_DIR}/tools/ci/test_replay_source_contract.py)', cmake)

    def test_meshing_captures_voxel_revision_with_owner_thread_snapshot(self):
        source = SOURCE.read_text(encoding="utf-8")
        dispatch = function(source, "SHIELD_WorldSystem::dispatch_meshing_jobs(")
        header = SOURCE.with_suffix(".h").read_text(encoding="utf-8")
        record = header.split("struct MeshingJobChunk {", 1)[1].split("};", 1)[0]
        self.assertIn("u32 voxel_revision = 0;", record)
        self.assertIn("u64 terrain_edit_generation = 0;", record)
        self.assertIn("chunk->water_seed_edit_generation(), voxel_revision", dispatch)
        self.assertLess(dispatch.index("chunk->voxel_revision()"),
                        dispatch.index("snapshot.sdf_data = chunk->sdf_data"))
        self.assertLess(dispatch.index("snapshot.sdf_data = chunk->sdf_data"),
                        dispatch.index("lane_jobs.emplace_back("))
        worker = dispatch.split("lane_jobs.emplace_back(", 1)[1]
        # Strong owners hold both the destination and immutable input until the
        # job exits. No mutable live arrays or revisions may be read by workers.
        self.assertIn("[this, chunk, scratch_owner = std::move(scratch_owner), step, transition_faces, terrain_mesh_required]", worker)
        for live_read in ("chunk->sdf_data", "chunk->heightmap_data", "chunk->material_data",
                          "chunk->water_level_data", "chunk->water_depth_mm",
                          "chunk->voxel_revision()", "chunk->authority_hash_revision()",
                          "chunk->water_seed_edit_generation()"):
            self.assertNotIn(live_read, worker)

    def test_stale_terrain_guard_precedes_every_terrain_publication_write(self):
        source = SOURCE.read_text(encoding="utf-8")
        publish = function(source, "bool SHIELD_WorldSystem::publish_front_meshing_batch(")
        self.assertIn("const bool terrain_current = "
                      "chunk->mesh_version.load(std::memory_order_acquire) == job_chunk.mesh_version && "
                      "chunk->voxel_revision() == job_chunk.voxel_revision && "
                      "chunk->water_seed_edit_generation() == job_chunk.terrain_edit_generation;", publish)
        self.assertNotIn("authority_hash_revision()", publish)
        success_guard = "if (mesh_ready && !mesh_failed && current_owner)"
        self.assertIn(success_guard, publish)
        success = function(source, success_guard)
        guard = "if (job_chunk.terrain_mesh_required && terrain_current)"
        terrain = function(success, guard)
        self.assertIn(terrain, success)
        self.assertNotIn("continue;", success)
        self.assertNotIn("return ", success)
        for write in ("chunk->mesh_vertices =", "chunk->mesh_indices =",
                      "chunk->current_lod.store(", "chunk->applied_transition_faces.store(",
                      "chunk->mesh_version++", "chunk->has_collision.store(",
                      "m_collision_pass_dirty = true;"):
            self.assertIn(write, terrain)
            self.assertEqual(publish.count(write), terrain.count(write))
        self.assertNotIn("clear_voxel_data_dirty", publish)
        # Failure/refusal never restores a completed LOD or clears edit truth.
        self.assertEqual(publish.count("current_lod.store("), 1)
        self.assertLess(publish.index("resident->second == chunk"), publish.index(guard))
        self.assertLess(publish.index(success_guard), publish.index(guard))

    def test_water_only_publication_is_independent_and_cleanup_is_unconditional(self):
        source = SOURCE.read_text(encoding="utf-8")
        publish = function(source, "bool SHIELD_WorldSystem::publish_front_meshing_batch(")
        success_guard = "if (mesh_ready && !mesh_failed && current_owner)"
        self.assertIn(success_guard, publish)
        success = function(source, success_guard)
        terrain = function(success, "if (job_chunk.terrain_mesh_required && terrain_current)")
        self.assertIn(terrain, success)
        self.assertNotIn("water_mesh", terrain)
        self.assertNotIn("continue;", terrain)
        self.assertNotIn("return", terrain)
        water_guard = ("if (replaces_water && water_current && "
                       "(!job_chunk.terrain_mesh_required || terrain_current))")
        water = function(success, water_guard)
        self.assertIn(water, success)
        self.assertIn("const bool water_current = "
                      "chunk->water_mesh_version.load(std::memory_order_acquire) == "
                      "job_chunk.water_mesh_version;", publish)
        self.assertNotIn("authority_hash_revision", water)
        self.assertNotIn("voxel_revision", water)
        self.assertNotIn("current_lod", water)
        self.assertIn("water_unchanged && "
                      "chunk->pending_water_mesh_generated.load(std::memory_order_acquire)", water)
        for write in ("chunk->water_mesh_vertices = std::move(chunk->pending_water_mesh_vertices);",
                      "chunk->water_mesh_indices = std::move(chunk->pending_water_mesh_indices);",
                      "chunk->water_mesh_version++;"):
            self.assertIn(write, water)
            self.assertEqual(publish.count(write), water.count(write))
        for cleanup in ("pending_mesh_vertices.clear()", "pending_mesh_indices.clear()",
                        "pending_water_mesh_vertices.clear()", "pending_water_mesh_indices.clear()",
                        "pending_mesh_ready.store(false", "pending_water_mesh_generated.store(false",
                        "pending_mesh_failed.store(false",
                        "pending_lod.store(-1"):
            self.assertIn(cleanup, publish)
            self.assertNotIn(cleanup, success)
        # All completion entry points converge on the guarded publisher.
        self.assertIn("publish_front_meshing_batch(force)", function(
            source, "void SHIELD_WorldSystem::process_completed_meshing_jobs("))
        self.assertIn("publish_front_meshing_batch( true)", function(
            source, "void SHIELD_WorldSystem::activate_due("))

    def test_edit_reopens_candidates_before_synchronous_preparation_can_refuse(self):
        source = SOURCE.read_text(encoding="utf-8")
        edit = function(source, "int SHIELD_WorldSystem::EditTerrainVoxel(")
        changed = function(edit, "if (changed)")
        self.assertIn("m_activation_index.content_changed(chunk->get_coords());", changed)
        self.assertIn("++m_dirty_generation;", changed)
        self.assertLess(edit.index("++m_dirty_generation;"),
                        edit.index("EnsureSurfaceReadyNear("))
        completed = function(edit, "if (edited > 0)")
        self.assertEqual(completed.count("EnsureSurfaceReadyNear("), 1)
        self.assertLess(completed.index("if (m_durable_read_failed)"),
                        completed.index("EditTerrainBed("))
        self.assertLess(edit.index("mark_voxel_data_dirty()"),
                        edit.index("current_lod.store(-1"))
        native = (ROOT / "test/shield/meshing_hardening_test.cpp").read_text(encoding="utf-8")
        for case in ("DelayedTerrainCompletionRetainsEditInvalidationAcrossBothLanes",
                     "SynchronousEditRebuildCannotBeSkippedByStaleCompletion",
                     "RefusedSynchronousEditReopensSettledCandidateScan",
                     "RefusedWorkerDispatchLeavesEditedMeshAndCollisionUntouched",
                     "StaleTerrainRejectsWaterMeshUntilEditedSnapshotCompletes",
                     "WaterRevisionChangeDoesNotRejectCurrentTerrainCompletion",
                     "MeshSnapshotOwnersSurviveQueuedWorkAndReleaseAfterWorldClear"):
            self.assertIn("TEST(StreamingHardening, " + case + ")", native)

    def test_pending_lod_survives_ordinary_adoption_and_counts_as_work(self):
        source = SOURCE.read_text(encoding="utf-8")
        adopt = function(source, "bool SHIELD_WorldSystem::adopt_streamed_chunk(")
        self.assertNotIn("reset_pending_jobs", adopt)
        self.assertNotIn("pending_lod.store", adopt)
        self.assertIn("if (chunk_ptr->pending_lod.load(std::memory_order_acquire) >= 0) "
                      "++m_streaming_telemetry_stats.pending_lod_chunks;",
                      re.sub(r"\s+", " ", source))
        header = SOURCE.with_suffix(".h").read_text(encoding="utf-8")
        self.assertIn("pending_lod_chunks == 0 && !promotion_pending", header)
        chunk = (ROOT / "src/luminumbra_common/world/Chunk.h").read_text(encoding="utf-8")
        reset = function(chunk, "void reset_pending_jobs()")
        self.assertIn("pending_water_mesh_generated.store(false", reset)
        parked = function(chunk, "void normalize_for_park()")
        self.assertIn("pending_water_mesh_generated.store(false", parked)
        native = (ROOT / "test/common/world_streaming_hardening_test.cpp").read_text(encoding="utf-8")
        self.assertIn("TEST(StreamingHardening, PendingLodPreventsStreamingQuiescence)", native)

    def test_promotion_cannot_replace_authority_and_water_validation_is_bounded(self):
        source = SOURCE.read_text(encoding="utf-8")
        promotion = function(source, "void SHIELD_WorldSystem::process_completed_promotion_jobs(")
        guard = "if (ready && !failed && !chunk->pending_sdf_data.empty() && !chunk->has_terrain_authority())"
        publication = function(promotion, guard)
        for write in ("chunk->sdf_data =", "chunk->heightmap_data =", "chunk->material_data =",
                      "chunk->mark_sdf_generated_current_params()", "chunk->clear_voxel_data_dirty_after_generation()"):
            self.assertIn(write, publication)
            self.assertEqual(promotion.count(write), publication.count(write))
        dispatch = function(source, "SHIELD_WorldSystem::dispatch_meshing_jobs(")
        self.assertIn("const bool terrain_authority = work_item.chunk->has_terrain_authority();", dispatch)
        self.assertIn("terrain_authority && (has_malformed_sdf || needs_sim_truth ||", dispatch)
        self.assertIn("!terrain_authority && (needs_sim_truth || has_malformed_sdf)", dispatch)
        self.assertNotIn("render_item.lod_level", dispatch)
        native_promotion = (ROOT / "test/shield/meshing_hardening_test.cpp").read_text(encoding="utf-8")
        self.assertIn("TEST(StreamingHardening, WaterAuthoritySurvivesTerrainBackfill)", native_promotion)
        mesher = (ROOT / "src/luminumbra_common/world/MarchingCubes.cpp").read_text(encoding="utf-8")
        water = function(mesher, "void GenerateWaterMesh(")
        self.assertIn("resolution <= 1 || resolution > 32 || !HasCompleteWaterGrid(chunk, resolution)", water)
        self.assertLess(water.index("resolution <= 1"), water.index("water_vertices.reserve"))
        native = (ROOT / "test/shield/async_water_meshing_test.cpp").read_text(encoding="utf-8")
        for case in ("DueSnapshotsRetainVoxelInvalidationAndIndependentWater",
                     "QueuedWaterOnlySnapshotsRemainIndependentOfVoxelEdits",
                     "SynchronousEditPreparationInvalidatesPublishedWaterOnlySnapshot",
                     "WaterOnlyCompletionDoesNotRequireTerrainGeometryOrCurrentLod",
                     "InvalidResolutionAndPartialSurfaceRejectBeforeAllocation"):
            self.assertIn("TEST(AsyncWaterMeshing, " + case + ")", native)


class SimulationCollisionWiring(unittest.TestCase):
    HOSTS = (
        ("src/luminumbra_server/ServerWorldRunner.cpp",
         "ServerTickReport ServerWorldRunner::RunFixedTicks(", "physics_system->update("),
        ("src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp",
         "bool NetSessionApplyStep(", "physics_system->update("),
        ("src/luminumbra_client/main_client.cpp", None, "physics->update("),
        ("src/luminumbra_common/world/GameSession.cpp",
         "std::uint32_t GameSession::TickSimulation(", "StepCreatureAvatars("),
    )

    def assert_barrier(self, source, name, step):
        body = function(source, name) if name else source.split(
            "case GameState::IN_GAME:", 1)[1].split("case GameState::", 1)[0]
        self.assertIn("prepare_simulation_collisions(", body)
        self.assertRegex(body, r"prepare_simulation_collisions\(\s*\{\},")
        prepare = body.index("prepare_simulation_collisions(")
        self.assertLess(prepare, body.index(step))
        if name is None:
            self.assertLess(prepare, body.index("g_playerController->Update("))
            self.assertIn("break;", body[prepare:body.index("g_playerController->Update(")])
        elif "ServerWorldRunner" in name:
            self.assertLess(prepare, body.index("physics_system->update_avatars("))
            self.assertIn("break;", body[prepare:body.index(step)])
        elif "GameSession" in name:
            self.assertIn("return i;", body[prepare:body.index(step)])
            self.assertLess(prepare, body.index("m_worldClock.set_tick("))
        else:
            self.assertIn("return false;", body[prepare:body.index(step)])

    def test_all_hosts_prepare_before_physics(self):
        for path, name, step in self.HOSTS:
            with self.subTest(path=path):
                self.assert_barrier((ROOT / path).read_text(encoding="utf-8"), name, step)

    def test_missing_preparation_is_detected_at_every_host(self):
        for path, name, step in self.HOSTS:
            source = (ROOT / path).read_text(encoding="utf-8")
            with self.subTest(path=path), self.assertRaises(AssertionError):
                self.assert_barrier(source.replace("prepare_simulation_collisions(",
                                                   "removed_preparation(", 1), name, step)

    def test_lockstep_refusal_waits_before_consuming_either_peer_input(self):
        source = (ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp").read_text(
            encoding="utf-8")
        step = function(source, "bool NetworkedSessionDriver::StepAgreedTick(")
        prepare = step.index("prepare_simulation_collisions(")
        self.assertRegex(step, r"prepare_simulation_collisions\(\s*\{\},")
        self.assertIn("&m_impl->host_ctx, &m_impl->client_ctx", step)
        self.assertLess(prepare, step.index("m_impl->host->PumpTick("))
        self.assertIn("return true;", step[prepare:step.index("m_impl->host->PumpTick(")])

    def test_client_preflight_separates_terminal_faults_from_retryable_refusal(self):
        source = (ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp").read_text(
            encoding="utf-8")
        self.assert_client_preflight_outcomes(source)
        with self.assertRaises(AssertionError):
            self.assert_client_preflight_outcomes(source.replace("m_finished = true;", "m_finished = false;"))

    def assert_client_preflight_outcomes(self, source):
        step = function(source, "bool NetworkedSessionDriver::StepAgreedTick(")
        self.assertIn("const bool prepared = !ctx->session->GetRegionSchedulerFault() &&", step)
        terminal = function(step, "if (ctx->session->GetRegionSchedulerFault())")
        for token in ('m_failure_reason = "region_preparation_fault";', "m_finished = true;", "return false;"):
            self.assertIn(token, terminal)
        self.assertLess(step.index("if (ctx->session->GetRegionSchedulerFault())"),
                        step.index("if (!prepared)"))
        self.assertEqual(function(step, "if (!prepared)"), "retry = true;")
        self.assertEqual(function(step, "if (retry)"), "return true;")
        self.assertLess(step.index("if (!prepared)"), step.index("m_impl->host->PumpTick("))

    def test_denied_avatar_checks_session_prepared_terrain_headroom(self):
        native = (ROOT / "test/common/RegionConsumerGate_test.cpp").read_text(encoding="utf-8")
        body = function(native, "TEST(RegionConsumerGate, "
                        "SessionDeniedAvatarStaysFrozenWithoutCapsuleAndRetriesOnLaterTicks)")
        self.assertIn("session.TickSimulation(", body)
        self.assertIn("world->sample_simulation_collision(0, 0, heights)", body)
        self.assertIn("physics->simulation_collision_sample_count(), 0u", body)
        self.assertIn("physics->audio_raycast({8, floor + 2, 8}, {8, floor - 2, 8}).hit", body)
        self.assertNotIn("add_chunk_collision(", body)
        self.assertIn("EXPECT_EQ(physics->work_items().bodies, body_limit);", body)

    def test_server_retry_is_distinct_from_terminal_preparation_faults(self):
        runner = (ROOT / "src/luminumbra_server/ServerWorldRunner.cpp").read_text(encoding="utf-8")
        body = function(runner, "ServerTickReport ServerWorldRunner::RunFixedTicks(")
        for guard in ("if (!m_booted || !m_session)",
                      "budget_tick))"):
            refused = function(body, guard)
            self.assertIn("report.region_fault = true;", refused)
        refused = function(body, "budget_tick))")
        self.assertIn("m_session->GetRegionSchedulerFault()", refused)
        self.assertIn("break;", refused)
        self.assertNotIn("++", refused)
        for path in ("NetRuntime.cpp", "Replicate.cpp"):
            source = (ROOT / "src/luminumbra_server/modes" / path).read_text(encoding="utf-8")
            retry = function(source, "if (step.ticks_executed == 0 && !step.region_fault)")
            self.assertIn("std::this_thread::sleep_", retry)
            self.assertIn("continue;", retry)
            self.assertNotIn("Close", retry)
            self.assertNotIn("return 1;", retry)
            self.assertIn("if (step.region_fault || step.ticks_executed == 0)", source)

    def test_headless_lockstep_prepares_both_peers_before_input_consumption(self):
        source = (ROOT / "src/luminumbra_server/modes/Lockstep.cpp").read_text(encoding="utf-8")
        body = function(source, "int RunLockstepLoopback(")
        loop = function(body, "for (; pumps < max_pumps; ++pumps)")
        preflight = function(loop, "peer_runner.get()})")
        self.assertIn("session->GetRegionSchedulerFault()", preflight)
        self.assertIn("session->GetSimulationTickCount() >= budget", preflight)
        self.assertIn("prepare_simulation_collisions( {}, session->GetSimulationTickCount() + 1)",
                      preflight)
        self.assertIn("terminal = session->GetRegionSchedulerFault().has_value();", preflight)
        refused = function(preflight, "if (!terminal)")
        self.assertIn("retry = true;", refused)
        self.assertTrue(refused.endswith("continue;"))
        retry = function(loop, "if (retry)")
        self.assertIn("std::this_thread::sleep_for(std::chrono::milliseconds(33));", retry)
        self.assertTrue(retry.endswith("continue;"))
        self.assertIn("return 1;", function(loop, "if (terminal)"))
        self.assertLess(loop.index("if (retry)"), loop.index("host.PumpTick(budget)"))
        self.assertLess(loop.index("if (retry)"), loop.index("peer.PumpTick(budget)"))
        self.assertIn("step.ticks_executed == 1 && !step.region_fault", function(
            source, "bool LockstepApplyStep("))

    def test_collision_cache_resolves_keys_with_fixed_capacity(self):
        source = SOURCE.read_text(encoding="utf-8")
        sample = function(source, "bool SHIELD_WorldSystem::sample_simulation_collision(")
        probe = function(sample, "for (std::size_t probe = 0; probe < m_collision_samples.size(); ++probe)")
        self.assertIn("entry.x == chunk_x && entry.z == chunk_z", probe)
        self.assertIn("entry.last_use < oldest.last_use", probe)
        self.assertIn("auto& cached = m_collision_samples[selected];", sample)
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(encoding="utf-8")
        stationary = function(native, "TEST(SurfaceQuery, DisjointStationaryCollisionHorizonsReuseEveryColumn)")
        self.assertIn("128 * 16 + 8", stationary)
        self.assertIn("physics.prepare_simulation_collisions({}, 2)", stationary)
        self.assertIn("EXPECT_EQ(world->simulation_collision_terrain_evaluations(), evaluations);", stationary)
        self.assertIn("CollisionSampleCacheRetainsCapacityAndReplacesOnlyColdColumns", native)

    def test_saved_collision_cache_precedes_io_and_invalidates_at_publication(self):
        source = SOURCE.read_text(encoding="utf-8")
        sample = function(source, "bool SHIELD_WorldSystem::sample_simulation_collision(")
        self.assertNotIn("find_authoritative_chunk(", sample)
        self.assertEqual(sample.count("read_committed_chunk(coords)"), 1)
        reuse = function(sample, "if (cached.valid && cached.complete && cached.saved)")
        self.assertIn("std::copy(cached.heights.begin()", reuse)
        self.assertIn("return true;", reuse)
        self.assertLess(sample.index("if (cached.valid && cached.complete && cached.saved)"),
                        sample.index("read_committed_chunk(coords)"))
        self.assertIn("cached.generation != generation", sample)
        publish = function(sample, "if (valid && expected && !resident_terrain)")
        for token in ("std::copy(heights.begin()", "cached.saved = true;", "cached.complete = true;"):
            self.assertIn(token, publish)
        for name in ("void SHIELD_WorldSystem::remember_durable_chunks(",
                     "void SHIELD_WorldSystem::set_durable_chunk_loader(",
                     "void SHIELD_WorldSystem::set_durable_residency(",
                     "void SHIELD_WorldSystem::clear_world("):
            self.assertIn("sample.valid = false;", function(source, name))
        self.assertIn("remember_durable_chunks(coords);", function(
            source, "void SHIELD_WorldSystem::replace_durable_chunks("))
        session = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(encoding="utf-8")
        self.assertIn("m_worldSystem->remember_durable_chunks(covered);", session)
        self.assertIn("transaction->defer_until_commit(std::move(committed));", session)
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(encoding="utf-8")
        for case in ("SavedCollisionSamplesReadOnceUntilSaveOrTerrainGenerationChanges",
                     "SavedCollisionSamplesDoNotCacheInvalidAuthorityAndRespectReplacement",
                     "SavedWaterOnlyCollisionSamplesRetainValidatedProceduralFallback"):
            self.assertIn("TEST(SurfaceQuery, " + case + ")", native)

    def test_horizon_stages_only_nine_columns_and_commits_after_capacity_check(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(encoding="utf-8")
        body = function(function(source, "struct SimulationHorizon"), "bool add(")
        self.assertIn("std::array<ChunkID, 9> candidate{};", body)
        self.assertNotIn("= columns;", body)
        self.assertNotIn("columns =", body)
        capacity = body.index("candidate_count > columns.size() - count")
        self.assertLess(body.index("std::binary_search(columns.begin(), end, id)"), capacity)
        self.assertLess(capacity, body.index("std::move_backward("))
        self.assertIn("i < candidate_count", body)
        self.assertIn("return false;", body[:body.index("std::move_backward(")])

    def test_traversal_catchup_stops_on_a_refused_tick(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        loop = source.split("while (!gameSession->GetRegionSchedulerFault()", 1)[1].split(
            "traversal_ticks += advanced;", 1)[0]
        self.assertIn("const auto advanced =", loop)
        self.assertIn("if (advanced == 0)", loop)
        self.assertIn("break;", loop.split("if (advanced == 0)", 1)[1])

    def test_render_preparation_refusal_reaches_fault_and_persistence_handlers(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        body = source.split("case GameState::IN_GAME:", 1)[1].split("case GameState::", 1)[0]
        refusal = function(body, "std::numeric_limits<std::uint64_t>::max()))")
        self.assertLess(refusal.index("PollSessionFault("), refusal.index("break;"))
        self.assertLess(refusal.index("SaveSessionWhenDue("), refusal.index("break;"))

    def test_render_frames_do_not_reuse_an_upcoming_tick(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        body = source.split("case GameState::IN_GAME:", 1)[1].split("case GameState::", 1)[0]
        self.assertRegex(body, r"prepare_simulation_collisions\(\s*\{\},\s*"
                              r"std::numeric_limits<std::uint64_t>::max\(\)\)")

    def test_teleport_invalidates_and_character_driver_prepares_before_stepping(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(encoding="utf-8")
        teleport = function(source, "void PhysicsSystem::set_player_position(")
        self.assertIn("m_simulation_collisions->ready = false;", teleport)
        update = function(source, "void PhysicsSystem::update_player(")
        self.assertLess(update.index("!prepare_simulation_collisions({})"),
                        update.index("SetLinearVelocity("))
        self.assertIn("return;", update[update.index("!prepare_simulation_collisions({})"):
                                       update.index("SetLinearVelocity(")])

    def test_collision_sampling_checks_authority_before_full_procedural_sampling(self):
        source = SOURCE.read_text(encoding="utf-8")
        sample = function(source, "bool SHIELD_WorldSystem::sample_simulation_collision(")
        self.assertLess(sample.index("chunk->has_terrain_authority()"),
                        sample.index("for (int z = 0;"))
        self.assertIn("if (!cached.complete)", sample)
        self.assertIn("cached.generation != generation", sample)
        self.assertIn("generation == std::numeric_limits<u64>::max()", sample)
        self.assertIn("cached.complete = true;", sample)
        header = SOURCE.with_suffix(".h").read_text(encoding="utf-8")
        self.assertIn("std::array<CollisionSamples, 128>", header)

    def test_collision_regressions_remain_registered(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        for path, cases in (
            ("common/GameSessionHeadlessWorld_test.cpp", (
                "SimulationHorizonIgnoresRenderCollidersAndIncludesEveryBodyOwner",
                "SimulationPreparationRefusesWithoutPublicationAndRetryMatchesPeer",
                "SimulationPreparationBoundsInputsAndMissingAuthority",
                "SimulationPreparationCanonicalizesOrderAndPreservesLegacyPath",
                "SimulationOwnerRegistrationAndTickReuseStayBounded",
                "SimulationTerrainReservationsLeaveTheCapsuleBudgetUnchanged",
                "SimulationPreparationVisitsOnlyLiveAvatarOwners",
                "TeleportPreparesDestinationBeforeTheLocalCharacterSteps",
                "RenderPreparationRefreshesEvenWhenTheUpcomingTickDoesNotChange",
                "CollisionSamplingCachesProceduralColumnsAndPrefersCommittedHeights")),
            ("common/GameSessionHeadlessWorld_test.cpp", (
                "SimulationCollisionsIgnoreRenderCadenceAcrossSessionTicks",
                "LegacyColliderPreparationPreservesHashesAndSaveBytes",
                "RefusedCollisionBarrierPreservesTheSessionTickAndAccumulator",
                "SessionCollisionBarrierPrecedesTheFirstCreatureStep",
                "CharacterAdmissionChecksTheCombinedHorizonBeforeAllocating",
                "ExistingOverBudgetOwnersKeepTickingAndCanShrink",
                "RefusedCatchupPreservesPendingTicksAndFraction",
                "OverflowBodiesWaitForTerrainAndResumeTheirVelocity",
                "RestoredSpheresInvalidatePreparationAndJoinTheNextHorizon",
                "SuspendedSphereContinuationRetainsMotionAndActivity",
                "CreatureAttachmentSkipsRefusalsWithinAnIndependentAttemptBudget")),
        ):
            self.assertIn(path, cmake)
            native = (ROOT / "test" / path).read_text(encoding="utf-8")
            for case in cases:
                self.assertRegex(native, r"TEST\(\w+,\s*" + case + r"\)")

    def test_render_mutation_and_refused_physics_are_guarded(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        for name in ("bool PhysicsSystem::add_chunk_collision(",
                     "void PhysicsSystem::remove_chunk_collision("):
            self.assertIn("if (simulation_collisions_enabled())", function(source, name))
        for name in ("void PhysicsSystem::update(", "void PhysicsSystem::update_avatars(",
                     "void PhysicsSystem::update_player("):
            self.assertIn("!simulation_collisions_ready()", function(source, name))
        prepare = function(source, "bool PhysicsSystem::prepare_simulation_collisions(")
        self.assertLess(prepare.index("sample_simulation_collision("),
                        prepare.index("m_body_interface->RemoveBody("))
        for whole_world_scan in ("snapshot_streamed_chunks", "m_chunk_bodies", "GetBodies(",
                                 "m_avatars.size()", "avatar : m_avatars"):
            self.assertNotIn(whole_world_scan, prepare)

    def assert_terrain_reservation_accounting(self, source):
        count = function(source, "std::uint32_t PhysicsSystem::non_simulation_body_count(")
        self.assertIn("m_simulation_collisions->body_count", count)
        self.assertIn("GetNumBodies() - static_cast<std::uint32_t>(terrain_slots)", count)
        update = function(source, "void PhysicsSystem::update(")
        self.assertIn("m_work_items.bodies = non_simulation_body_count();", update)
        freeze = function(source, "void PhysicsSystem::set_avatar_frozen(")
        self.assertIn("non_simulation_body_count() < max_bodies - kTerrainBodyHeadroom", freeze)

    def test_terrain_reservations_use_headroom_at_both_body_count_consumers(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        self.assert_terrain_reservation_accounting(source)
        for original, replacement in (
            ("GetNumBodies() - static_cast<std::uint32_t>(terrain_slots)", "GetNumBodies()"),
            ("m_work_items.bodies = non_simulation_body_count();",
             "m_work_items.bodies = m_jolt_system->GetNumBodies();"),
            ("non_simulation_body_count() < max_bodies - kTerrainBodyHeadroom",
             "m_jolt_system->GetNumBodies() < max_bodies - kTerrainBodyHeadroom"),
        ):
            self.assertIn(original, source)
            with self.subTest(original=original), self.assertRaises(AssertionError):
                self.assert_terrain_reservation_accounting(source.replace(original, replacement))

    def test_character_admission_precedes_both_allocations(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        admit = function(source, "bool PhysicsSystem::can_admit_simulation_owner(")
        for token in ("m_live_avatar_count >= kSimulationOwnerBudget -",
                      "horizon.add(position)", "horizon.add(get_player_position())",
                      "horizon.add(get_avatar_position(index))",
                      "horizon.add(get_body_position(state.dynamic_owners[i].id))"):
            self.assertIn(token, admit)
        for signature, check, allocation, refusal in (
            ("bool PhysicsSystem::create_player_controller(",
             "!can_admit_simulation_owner(start_pos, true)", "CapsuleShapeSettings(", "return false;"),
            ("std::size_t PhysicsSystem::create_avatar_character(",
             "!can_admit_simulation_owner(start_pos, false)", "m_avatars.push_back(",
             "return kInvalidAvatar;"),
        ):
            body = function(source, signature)
            self.assertLess(body.index(check), body.index(allocation))
            self.assertIn(refusal, body[:body.index(allocation)])

    def test_overflow_horizons_keep_existing_sessions_running(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        prepare = function(source, "bool PhysicsSystem::prepare_simulation_collisions(")
        existing = function(prepare, "const auto include_existing =")
        self.assertIn("!SimulationHorizon::valid(position)", existing)
        self.assertIn("(void)horizon.add(position);", existing)
        self.assertIn("++state.owner_visit_count;", existing)
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(encoding="utf-8")
        self.assertIn("EXPECT_EQ(physics.simulation_collision_owner_visit_count(), 1u);", native)
        self.assertIn("EXPECT_EQ(physics.simulation_collision_owner_visit_count(), 0u);", native)
        self.assertNotIn("m_live_avatar_count >", prepare)
        self.assertNotIn("++owner_count", prepare)
        sphere = function(source, "JPH::BodyID PhysicsSystem::create_dynamic_sphere(")
        self.assertNotIn("kSimulationOwnerBudget", sphere)
        self.assertNotIn("can_admit_simulation_owner", sphere)
        self.assertIn("std::array<DynamicOwner, 10240> dynamic_owners", source)
        horizon = function(source, "struct SimulationHorizon")
        self.assertIn("PhysicsSystem::kSimulationColliderBudget", horizon)
        self.assertLess(horizon.index("candidate_count > columns.size() - count"),
                        horizon.index("std::move_backward("))

    def test_uncovered_owners_wait_without_losing_rigid_body_velocity(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        update = function(source, "void PhysicsSystem::update(")
        self.assertLess(update.index("m_work_items.bodies = non_simulation_body_count();"),
                        update.index("!simulation_collisions_ready()"))
        for token in ("GetLinearAndAngularVelocity(", "JPH::EMotionType::Kinematic",
                      "JPH::EMotionType::Dynamic", "owner.suspended = true;",
                      "owner.suspended = false;"):
            self.assertIn(token, update)
        for signature, position in (("void PhysicsSystem::update_avatars(",
                                     "horizon.covers(get_avatar_position(i))"),
                                    ("void PhysicsSystem::update_player(",
                                     "horizon.covers(get_player_position())")):
            body = function(source, signature)
            self.assertLess(body.index(position), body.index("++m_work_items.characters"))
        active = function(source, "bool PhysicsSystem::body_is_active(")
        self.assertIn("return owner.was_active;", active)
        self.assertNotIn("for (", active)
        destroy = function(source, "void PhysicsSystem::destroy_body(")
        self.assertIn("state.dynamic_slots[body.GetIndex()] = 0;", destroy)
        self.assertNotIn("for (", destroy)
        avatars = function(source, "void PhysicsSystem::update_avatars(")
        self.assertIn("i = m_avatar_head; i != kNoAvatar; i = m_avatars[i].next", avatars)
        self.assertNotIn("m_avatars.size()", avatars)
        clear = function(source, "void PhysicsSystem::clear_simulation_collisions(")
        self.assertIn("JPH::EMotionType::Dynamic", clear)
        self.assertIn("owner.suspended = false;", clear)
        for resume in (clear, update):
            self.assertIn("SetOwnerVelocityWithoutActivation(", resume)
        velocity = function(source, "void SetOwnerVelocityWithoutActivation(")
        self.assertIn("body.SetLinearVelocityClamped(velocity);", velocity)
        self.assertIn("body.SetAngularVelocityClamped(angular_velocity);", velocity)
        self.assertNotIn("ActivateBody", velocity)

    def test_refused_ticks_return_to_the_advanced_clock(self):
        source = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(
            encoding="utf-8")
        tick = function(source, "std::uint32_t GameSession::TickSimulation(")
        self.assertIn("m_simulationClock.return_unconsumed_ticks(ticks_executed - i);", tick)
        self.assertNotIn("m_simulationClock = clock_before", tick)
        header = (ROOT / "src/luminumbra_common/core/SimulationClock.h").read_text(
            encoding="utf-8")
        restore = function(header, "void return_unconsumed_ticks(")
        self.assertIn("tick_count_ -= ticks;", restore)
        self.assertIn("accumulator_ += static_cast<double>(ticks) * fixed_dt_;", restore)
        self.assertNotIn("reset(", restore)

    def test_creation_callers_handle_admission_refusal(self):
        client = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        attachment = (ROOT / "src/luminumbra_client/app/CreatureBodyAttachment.h").read_text(
            encoding="utf-8")
        self.assertEqual(client.count("create_avatar_character(") +
                         attachment.count("create_avatar_character("), 3)
        self.assertEqual(client.count("PhysicsSystem::kInvalidAvatar") +
                         attachment.count("PhysicsSystem::kInvalidAvatar"), 3)
        player = (ROOT / "src/luminumbra_client/player/PlayerController.cpp").read_text(
            encoding="utf-8")
        self.assertEqual(player.count("create_player_controller("), 3)
        self.assertEqual(len(re.findall(r"m_hasInitializedPhysicsPlayer\s*=\s*"
                                       r"m_physicsSystem->create_player_controller", player)), 3)
        roster = (ROOT / "src/luminumbra_server/ServerWorldRunner.cpp").read_text(encoding="utf-8")
        boot = function(roster, "bool ServerWorldRunner::Boot(")
        self.assertLess(boot.index("m_config.avatar_count > static_cast<int>(Systems::PhysicsSystem::kSimulationOwnerBudget)"),
                        boot.index("m_session->CreateWorld("))
        guard = function(roster, "if (character == Systems::PhysicsSystem::kInvalidAvatar)")
        for token in ("m_bootError =", "m_avatars.clear();", "m_session.reset();",
                      "m_jobSystem.shutdown();", "return false;"):
            self.assertIn(token, guard)
        replicate = (ROOT / "src/luminumbra_server/modes/Replicate.cpp").read_text(encoding="utf-8")
        self.assertIn("return 1;", function(
            replicate, "if (ci == Luminumbra::Systems::PhysicsSystem::kInvalidAvatar)"))
        self.assertIn("return 1;", function(replicate, "if (arrow_body.IsInvalid())"))
        self.assertLess(replicate.index("if (arrow_body.IsInvalid())"),
                        replicate.index("arrow_entity = registry.create();"))
        driver = (ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp").read_text(
            encoding="utf-8")
        self.assertNotIn("create_avatar_character(", driver)
        self.assertNotIn("create_player_controller(", driver)

    def test_restored_spheres_register_only_at_commit_and_invalidate_preparation(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        restore = function(source, "bool PhysicsSystem::restore_body_owners(")
        commit = restore.index("OwnerNativeStream empty;")
        for token in ("m_simulation_collisions = std::move(registrations);",
                      "m_simulation_collisions->dynamic_count = 0;",
                      "m_simulation_collisions->dynamic_slots.fill(0);",
                      "m_simulation_collisions->ready = false;",
                      "registrations_state.dynamic_owners[registrations_state.dynamic_count++]",
                      "registrations_state.dynamic_slots[id.GetIndex()] ="):
            self.assertIn(token, restore)
            self.assertGreater(restore.index(token), commit)
        self.assertLess(restore.index("std::make_unique<SimulationCollisions>()"), commit)
        self.assertNotIn("return false;", restore[commit:])
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(
            encoding="utf-8")
        case = function(native, "TEST(SurfaceQuery, RestoredSpheresInvalidatePreparationAndJoinTheNextHorizon)")
        for token in ("EXPECT_FALSE(target.restore_body_owners(invalid, ids));",
                      "EXPECT_EQ(target.simulation_collision_owner_visit_count(), 1u);",
                      "EXPECT_EQ(target.simulation_collision_sample_count(), 9u);",
                      "target.update(1.0f / 30);", "target.destroy_body(ids[0]);"):
            self.assertIn(token, case)

    def test_suspended_sphere_continuation_preserves_the_existing_codec(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        capture = function(source, "bool PhysicsSystem::capture_body_owner(")
        for token in ("owner.IsKinematic() && captured.suspended",
                      "captured.suspended = true;",
                      "captured.was_active = registered.was_active;",
                      "payload.retained_velocity = captured.retained_velocity;",
                      "payload.retained_angular_velocity = captured.retained_angular_velocity;",
                      "payload.suspended = captured.suspended;",
                      "payload.was_active = captured.was_active;"):
            self.assertIn(token, capture)
        restore = function(source, "bool PhysicsSystem::restore_body_owners(")
        for token in ("!finite(saved.retained_velocity)", "!finite(saved.retained_angular_velocity)",
                      "if (active)", ".suspended = saved.suspended,", ".was_active = saved.was_active",
                      "saved.retained_velocity.x", "saved.retained_angular_velocity.x",
                      "if (saved.suspended)", "JPH::EMotionType::Kinematic"):
            self.assertIn(token, restore)
        schema = function(source, "void OwnerSchema(")
        self.assertNotIn("suspended", schema)
        self.assertIn("constexpr std::uint32_t kOwnerVersion = 2;", source)
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(
            encoding="utf-8")
        case = function(native, "TEST(SurfaceQuery, SuspendedSphereContinuationRetainsMotionAndActivity)")
        for token in ("std::pair{false, false}", "std::pair{false, true}",
                      "std::pair{true, false}", "std::pair{true, true}",
                      "EXPECT_TRUE(captured.suspended);",
                      "EXPECT_EQ(captured.was_active, active);",
                      "EXPECT_EQ(captured.retained_velocity, state.owners[0].retained_velocity);",
                      "EXPECT_EQ(captured.retained_angular_velocity, state.owners[0].retained_angular_velocity);"):
            self.assertIn(token, case)

    def test_creature_attachment_continues_after_refusal_with_bounded_attempts(self):
        source = (ROOT / "src/luminumbra_client/app/CreatureBodyAttachment.h").read_text(
            encoding="utf-8")
        attach = function(source, "inline void AttachMissingCreatureBodies(")
        refusal = function(attach, "if (idx == Systems::PhysicsSystem::kInvalidAvatar)")
        self.assertIn("continue;", refusal)
        self.assertNotIn("break;", refusal)
        self.assertIn("attempted >= maxAttemptsPerFrame", attach)
        self.assertIn("made >= maxPerFrame", attach)
        self.assertLess(attach.index("++attempted;"), attach.index("create_avatar_character("))
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(
            encoding="utf-8")
        case = function(native, "TEST(SurfaceQuery, CreatureAttachmentSkipsRefusalsWithinAnIndependentAttemptBudget)")
        for token in ("AttachMissingCreatureBodies(&physics, registry, 1, 1);",
                      "AttachMissingCreatureBodies(&physics, registry, 1, 2);",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(entities[1]));",
                      "EXPECT_EQ(physics.avatar_character_slot_count(), 14u);"):
            self.assertIn(token, case)

    def test_expected_collision_authority_cannot_be_masked_by_water_or_unknown_metadata(self):
        source = (ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp").read_text(
            encoding="utf-8")
        sample = function(source, "bool SHIELD_WorldSystem::sample_simulation_collision(")
        self.assertIn("const bool resident_terrain = chunk && chunk->has_terrain_authority();", sample)
        self.assertIn("expected && !resident_terrain", sample)
        self.assertIn("expected && (!chunk || !chunk->has_simulation_authority())", sample)
        self.assertLess(sample.index("read_committed_chunk("),
                        sample.index("if (chunk && chunk->has_terrain_authority())"))
        self.assertLess(sample.index("expected && (!chunk || !chunk->has_simulation_authority())"),
                        sample.index("if (chunk && chunk->has_terrain_authority())"))


if __name__ == "__main__":
    unittest.main()
