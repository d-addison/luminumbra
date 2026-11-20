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
        self.assertIn("resolution <= 1 || resolution > 32 || !HasCompleteWaterGrid(water_source, resolution)", water)
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
        retry = function(step, "if (retry)")
        self.assertTrue(retry.endswith("m_finished = true; return false;"))
        self.assertLess(step.index("if (ctx->session->GetRegionSchedulerFault())"),
                        step.index("if (retry)"))
        self.assertLess(step.index("if (!prepared)"), step.index("m_impl->host->PumpTick("))

    def assert_client_preparation_retry_bound(self, source):
        impl = function(source, "struct NetworkedSessionDriver::Impl")
        self.assertIn("std::uint64_t preparation_retries = 0;", impl)
        step = function(source, "bool NetworkedSessionDriver::StepAgreedTick(")
        self.assertIn("const std::uint64_t budget = m_impl->config.budget_ticks;", step)
        retry = function(step, "if (retry)")
        guard = "if (m_impl->preparation_retries < max_preparation_retries)"
        self.assertIn(guard, retry)
        self.assertEqual(function(retry, guard), "++m_impl->preparation_retries; return true;")
        self.assertEqual(retry, "const std::uint64_t max_preparation_retries = budget; "
                         + guard + " { ++m_impl->preparation_retries; return true; } "
                         'm_failure_reason = "region_preparation_stalled_at_tick_" + '
                         "std::to_string(m_agreed_ticks); m_finished = true; return false;")
        # One initialization and one guarded increment: progress cannot reset the
        # run-wide allowance, and even UINT64_MAX cannot wrap the counter.
        self.assertEqual(source.count("preparation_retries ="), 2)
        self.assertEqual(source.count("++m_impl->preparation_retries;"), 1)
        self.assertEqual(source.count("m_impl->preparation_retries"), 2)
        self.assertTrue(step.startswith(
            "if (m_finished || m_desynced || !m_impl->host || !m_impl->client) { return false; }"))
        preflight = function(step, "&m_impl->client_ctx})")
        self.assertIn("&m_impl->host_ctx, &m_impl->client_ctx", step)
        self.assertTrue(step[step.index(preflight) + len(preflight):].startswith(" } if (retry)"))
        before_pump = step[:step.index("m_impl->host->PumpTick(")]
        for advance in ("PumpTick(", "m_agreed_ticks =", "++m_agreed_ticks", "TickSimulation(",
                        "NetSessionCollectInput(", "NetSessionApplyStep("):
            self.assertNotIn(advance, before_pump)
        for peer in ("host", "client"):
            self.assertLess(step.index("if (retry)"), step.index(f"m_impl->{peer}->PumpTick("))
        self.assert_client_preflight_outcomes(source)

    def test_client_preparation_refusals_have_a_run_wide_tick_budget(self):
        source = (ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp").read_text(
            encoding="utf-8")
        self.assert_client_preparation_retry_bound(source)

    def test_client_preparation_retry_bound_mutations_are_detected(self):
        source = (ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp").read_text(
            encoding="utf-8")
        mutations = (
            ("missing bound", "if (retry) {", "if (retry) { return true; } if (false) {"),
            ("unbounded retry", "m_impl->preparation_retries < max_preparation_retries", "true"),
            ("off by one", "m_impl->preparation_retries < max_preparation_retries",
             "m_impl->preparation_retries <= max_preparation_retries"),
            ("missing increment", "++m_impl->preparation_retries;", ""),
            ("counter reset", "++m_impl->preparation_retries;", "m_impl->preparation_retries = 0;"),
            ("changed allowance", "max_preparation_retries = budget;",
             "max_preparation_retries = budget + 1;"),
            ("missing reason", '"region_preparation_stalled_at_tick_"', '""'),
            ("not terminal", "m_finished = true;", "m_finished = false;"),
            ("keeps spinning", "m_finished = true;\n        return false;",
             "m_finished = true;\n        return true;"),
        )
        for name, old, new in mutations:
            self.assertIn(old, source, name)
            with self.subTest(mutation=name), self.assertRaises(AssertionError):
                self.assert_client_preparation_retry_bound(source.replace(old, new))

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
        fault = function(refused, "if (const auto& fault =")
        self.assertIn("report.region_fault = true;", fault)
        self.assertIn("break;", fault)
        self.assertNotIn("continue;", fault)
        retry = function(refused, "if (preparation_retries < max_preparation_retries)")
        self.assertIn("++preparation_retries;", retry)
        self.assertIn("std::this_thread::sleep_for(std::chrono::milliseconds(33));", retry)
        self.assertIn("continue;", retry)
        self.assertIn("const std::uint64_t max_preparation_retries = tick_count;", body)
        self.assertIn("std::uint64_t preparation_retries = 0;", body)
        self.assertTrue(refused.endswith("break;"))
        self.assertLess(refused.index("GetRegionSchedulerFault()"),
                        refused.index("if (preparation_retries"))
        # Refused attempts record no ServerPhysics sample; only completed ticks do.
        self.assertNotIn("Record(", refused)
        self.assertNotIn("TickScope physics_budget", body)
        self.assertLess(body.index("physics_system->update_avatars("),
                        body.index("budget.Record(BudgetStage::ServerPhysics,"))

    def assert_runtime_retry_contract(self, source, entry):
        body = function(source, "int " + entry + "(")
        loop_guard = "while (executed < options.ticks)"
        loop = function(body, loop_guard)
        for declaration in ("const std::uint64_t max_preparation_retries = options.ticks;",
                            "std::uint64_t preparation_retries = 0;"):
            self.assertEqual(body.count(declaration), 1)
            self.assertLess(body.index(declaration), body.index(loop_guard))
        retry_guard = ("if (step.ticks_executed == 0 && !step.region_fault && "
                       "preparation_retries < max_preparation_retries)")
        self.assertIn(retry_guard, loop)
        retry = function(loop, retry_guard)
        self.assertEqual(loop.count("++preparation_retries;"), 1)
        self.assertIn("++preparation_retries;", retry)
        self.assertIn("std::this_thread::sleep_", retry)
        self.assertTrue(retry.endswith("continue;"))
        for forbidden in ("Close", "Shutdown", "return", "executed", "SetAvatarMove",
                          "SendUsercmd", "BroadcastSnapshot", "PumpInbound"):
            self.assertNotIn(forbidden, retry)
        terminal_guard = "if (step.region_fault || step.ticks_executed == 0)"
        self.assertIn(terminal_guard, loop)
        terminal = function(loop, terminal_guard)
        self.assertIn("return 1;", terminal)
        self.assertNotIn("continue;", terminal)
        self.assertEqual(loop.count("executed += step.ticks_executed;"), 1)
        ordered = ("runner.RunFixedTicks(1)", "executed += step.ticks_executed;",
                   retry_guard, terminal_guard, "server.BroadcastSnapshot(executed,")
        for before, after in zip(ordered, ordered[1:]):
            self.assertLess(loop.index(before), loop.index(after))
        if entry == "RunNetHostServerMode":
            self.assertIn("CloseRuntimeTcpSlot(slot);", terminal)
            self.assertIn("next_tick_time = std::chrono::steady_clock::now() + "
                          "std::chrono::milliseconds(33);", retry)
            self.assertIn("std::this_thread::sleep_until(next_tick_time);", retry)
        else:
            self.assertIn("cmd.tick = executed + 1;", loop)
            self.assertIn("std::this_thread::sleep_for(std::chrono::milliseconds(33));", retry)
            for publication in ("get_avatar_position(", "create_dynamic_sphere(",
                                "destroy_body(", "client.PumpInbound()"):
                self.assertLess(loop.index(terminal_guard), loop.index(publication))

    def test_runtime_and_replicate_refusals_have_run_wide_tick_budgets(self):
        for path, entry in (("NetRuntime.cpp", "RunNetHostServerMode"),
                            ("Replicate.cpp", "RunReplicate")):
            with self.subTest(path=path):
                source = (ROOT / "src/luminumbra_server/modes" / path).read_text(encoding="utf-8")
                self.assert_runtime_retry_contract(source, entry)

    def test_runtime_retry_contract_detects_missing_bounds_and_fault_guards(self):
        for path, entry in (("NetRuntime.cpp", "RunNetHostServerMode"),
                            ("Replicate.cpp", "RunReplicate")):
            source = (ROOT / "src/luminumbra_server/modes" / path).read_text(encoding="utf-8")
            for token in ("preparation_retries < max_preparation_retries",
                          "++preparation_retries;", "!step.region_fault &&",
                          "step.region_fault || "):
                self.assertIn(token, source)
                with self.subTest(path=path, token=token), self.assertRaises(AssertionError):
                    self.assert_runtime_retry_contract(source.replace(token, "", 1), entry)

    def test_remaining_transports_preserve_connections_and_inputs_on_bounded_retry(self):
        for path, entry in (("Gns.cpp", "RunGnsHost"), ("Steam.cpp", "RunSteamHost"),
                            ("NetTcp.cpp", "RunNetHost"), ("NetSoak.cpp", "RunNetSoak")):
            with self.subTest(path=path):
                source = (ROOT / "src/luminumbra_server/modes" / path).read_text(encoding="utf-8")
                body = function(source, "int " + entry + "(")
                self.assertIn("const std::uint64_t max_preparation_retries = options.ticks;", body)
                self.assertIn("std::uint64_t preparation_retries = 0;", body)
                retry_guard = ("if (step.ticks_executed == 0 && !step.region_fault && "
                               "preparation_retries < max_preparation_retries)")
                retry = function(body, retry_guard)
                self.assertIn("++preparation_retries;", retry)
                self.assertIn("std::this_thread::sleep_for(std::chrono::milliseconds(33));", retry)
                self.assertTrue(retry.endswith("continue;"))
                for forbidden in ("Close", "Shutdown", "return", "RecordInput", "SetAvatarMove",
                                  "BroadcastSnapshot"):
                    self.assertNotIn(forbidden, retry)
                terminal_guard = "if (step.region_fault || step.ticks_executed == 0)"
                self.assertIn("return 1;", function(body, terminal_guard))
                self.assertLess(body.index(retry_guard), body.index(terminal_guard))
                self.assertLess(body.index(terminal_guard), body.index("server.BroadcastSnapshot("))
                self.assertLess(body.index("server.PumpInbound()"), body.index("runner.RunFixedTicks(1)"))
                self.assertLess(body.index("runner.SetAvatarMove("), body.index("runner.RunFixedTicks(1)"))
                if path == "NetSoak.cpp":
                    self.assertIn("next_tick_time = std::chrono::steady_clock::now();", retry)

    def test_replay_retries_bounded_inputs_and_records_only_consumed_ticks(self):
        source = (ROOT / "src/luminumbra_server/modes/Replay.cpp").read_text(encoding="utf-8")
        record = function(source, "int RunRecord(")
        playback = function(source, "int RunReplay(")
        for body, budget in ((record, "options.ticks"), (playback, "admitted_inputs")):
            self.assertIn("const std::uint64_t max_preparation_retries = " + budget + ";", body)
            self.assertIn("std::uint64_t preparation_retries = 0;", body)
            retry_guard = ("if (step.ticks_executed == 0 && !step.region_fault && "
                           "preparation_retries < max_preparation_retries)")
            retry = function(body, retry_guard)
            self.assertIn("++preparation_retries;", retry)
            self.assertIn("std::this_thread::sleep_for(std::chrono::milliseconds(33));", retry)
            self.assertEqual(retry.count("continue;"), 1)
            for forbidden in ("RecordInput", "RecordCheckpoint", "Finalize", "Shutdown", "return"):
                self.assertNotIn(forbidden, retry)
            self.assertIn("return 1;", function(body, "if (step.region_fault)"))
            self.assertIn("return 1;", function(body, "if (step.ticks_executed == 0)"))
            self.assertLess(body.index("if (step.region_fault)"), body.index(retry_guard))
            self.assertLess(body.index(retry_guard), body.index("if (step.ticks_executed == 0)"))
        self.assertEqual(record.count("writer.RecordInput("), 1)
        self.assertIn("writer.RecordInput(executed, empty_inputs);", record)
        self.assertLess(record.index("if (step.ticks_executed == 0)"), record.index("writer.RecordInput("))
        self.assertLess(record.index("writer.RecordInput("), record.index("schedule_trace.emplace_back("))
        self.assertLess(playback.index("if (step.ticks_executed == 0)"),
                        playback.index("schedule_trace.at("))

    def test_batch_modes_capture_only_complete_fault_free_runs(self):
        modes = ROOT / "src/luminumbra_server/modes"
        smoke = function((modes / "Smoke.cpp").read_text(encoding="utf-8"), "SmokeRunResult RunSmokeOnce(")
        guard = "if (result.ticks.region_fault || result.ticks.ticks_executed != options.ticks)"
        refused = function(smoke, guard)
        self.assertIn("runner.Shutdown(&result.shutdown_save);", refused)
        self.assertIn("return result;", refused)
        self.assertLess(smoke.index(guard), smoke.index("runner.ComputeWorldHash()"))
        smoke_entry = function((modes / "Smoke.cpp").read_text(encoding="utf-8"), "int RunSmoke(")
        for guard in ("if (!first.ok)", "if (!replay.ok)"):
            self.assertIn("return 1;", function(smoke_entry, guard))
            self.assertLess(smoke_entry.index(guard), smoke_entry.index("WriteSmokeArtifact("))
        heavy = function((modes / "Heavy.cpp").read_text(encoding="utf-8"), "int RunHeavy(")
        for report, target, publication, refusal in (
            ("pre_save_ticks", "options.ticks", "original.SaveFullSnapshot()", "return 1;"),
            ("orig_resim", "options.heavy_resim", "loaded.RunFixedTicks(", "return 1;"),
            ("loaded_resim", "options.heavy_resim", "const HeavyHashes orig_final", "return 1;"),
        ):
            guard = f"if ({report}.region_fault || {report}.ticks_executed != {target})"
            self.assertIn(refusal, function(heavy, guard))
            self.assertLess(heavy.index(guard), heavy.index(publication))
        # Every refusal after boot unwinds the runners before the guard removes the world.
        self.assertLess(heavy.index("ThrowawaySaveDirGuard save_cleanup;"),
                        heavy.index("ServerWorldRunner original("))
        self.assertLess(heavy.index("save_cleanup.Arm(original.Session()->GetWorldSaveDir());"),
                        heavy.index("original.RunFixedTicks(options.ticks)"))
        server = function((modes / "Server.cpp").read_text(encoding="utf-8"), "int RunServer(")
        self.assertIn("runner.RunFixedTicks(options.ticks)", server)
        self.assertIn("return 3;", function(server, "if (report.region_fault)"))
        self.assertIn("return report.ticks_executed == options.ticks ? 0 : 1;", server)

    def test_mode_retry_and_completion_regressions_remain_registered(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("persistence/host_scheduler_fault_test.cpp", cmake)
        routes = (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8")
        self.assertIn('"tools/ci/test_replay_source_contract.py"', routes)
        aggregate = (ROOT / "tools/ci/test_replay_source_contract.py").read_text(encoding="utf-8")
        self.assertIn('pattern="test_region_authority_wiring.py"', aggregate)
        native = (ROOT / "test/persistence/host_scheduler_fault_test.cpp").read_text(encoding="utf-8")
        for name in ("BatchPreparationRetryPreservesCompleteRunAndTraceIndices",
                     "PermanentPreparationRefusalIsBoundedAndLeavesTheTickPending",
                     "PreparationReadFaultIsTerminalBeforeAnyTickIsConsumed",
                     "ServerRetriesPreparationAfterPartialProgressBeforeCompleteExit",
                     "RecordRetriesWithoutDuplicatingUnconsumedInputs",
                     "PermanentRecordRefusalPreservesThePublishedPair",
                     "ReplayRetriesTheSameInputAndStopsOnExhaustionOrFault",
                     "SmokeNeverCapturesAnIncompleteOrFaultedRun",
                     "SmokeComparesOnlyTwoCompleteRuns",
                     "HeavyNeverSnapshotsOrComparesAnIncompleteOrFaultedRun"):
            self.assertIn("TEST_F(HostSchedulerFault, " + name + ")", native)
        for mode in ("Replay", "Server", "Smoke", "Heavy"):
            self.assertIn('modes/' + mode + '.cpp"', native)

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
                "SuspendedSphereContinuationRetainsMotionAndActivity")),
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

    def assert_character_admission_precedes_both_allocations(self, source):
        admit = function(source, "bool PhysicsSystem::can_admit_simulation_owner(")
        for token in ("m_live_avatar_count >= kSimulationOwnerBudget -",
                      "horizon.add(position)", "horizon.add(get_player_position())",
                      "horizon.add(get_avatar_position(index))",
                      "horizon.add(get_body_position(state.dynamic_owners[i].id))"):
            self.assertIn(token, admit)
        for signature, check, refusal in (
            ("bool PhysicsSystem::create_player_controller(",
             "!can_admit_simulation_owner(start_pos, true)", "return false;"),
            ("std::size_t PhysicsSystem::create_avatar_character(",
             "!can_admit_simulation_owner(start_pos, false)", "return kInvalidAvatar;"),
        ):
            body = function(source, signature)
            # Admission precedes both the capsule shape and the controller allocation.
            for allocation in ("FeetCapsule(", "PrepareController("):
                self.assertIn(allocation, body)
                self.assertLess(body.index(check), body.index(allocation))
                self.assertIn(refusal, body[:body.index(allocation)])

    def test_character_admission_precedes_both_allocations(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        self.assert_character_admission_precedes_both_allocations(source)
        for allocation in ("FeetCapsule(", "PrepareController("):
            with self.subTest(missing=allocation), self.assertRaises(AssertionError):
                self.assert_character_admission_precedes_both_allocations(
                    source.replace(allocation, "MissingAllocation("))
            for signature in ("bool PhysicsSystem::create_player_controller(",
                              "std::size_t PhysicsSystem::create_avatar_character("):
                start = source.index("{", source.index(signature)) + 1
                early = source[:start] + allocation + ");" + source[start:]
                with self.subTest(early=allocation, signature=signature), self.assertRaises(AssertionError):
                    self.assert_character_admission_precedes_both_allocations(early)

    def test_avatar_restore_accounts_for_remaining_frozen_captures(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        restore = function(source, "bool PhysicsSystem::restore_avatar_owner(")
        for token in ("remaining_frozen.size() >= kMaxRestoredAvatars - m_avatars.size()",
                      "ParseController(pending, reservation)", "reservation.flags != 1u",
                      "reservation.body == JPH::BodyID::cInvalidBodyID",
                      "reservation.slot_count != recipe.slot_count",
                      "reservation.free_slots != recipe.free_slots",
                      "reserved.push_back(reservation.body);"):
            self.assertIn(token, restore)
            self.assertLess(restore.index(token), restore.index("PrepareOwnerAllocatorScratch("))
        self.assertRegex(restore, r"ValidateOwnerAllocator\(allocator,.*m_body_slots->slot_count, reserved\)")
        allocator = function(source, "bool ValidateOwnerAllocator(")
        reserved = function(allocator, "for (const auto id : reserved)")
        self.assertIn("if (!mark(id)) { return false; }", reserved)
        self.assertIn("std::all_of(", allocator)
        for signature in ("bool PhysicsSystem::OwnerContinuation::restore_avatar_owner(",
                          "bool GameSession::RestoreAvatarOwner("):
            self.assertIn("remaining_frozen)", function(source, signature))
        runner = (ROOT / "src/luminumbra_server/ServerWorldRunner.h").read_text(encoding="utf-8")
        self.assertIn("RestoreAvatarOwner(state, avatar, index, remaining_frozen)", runner)
        native = (ROOT / "test/common/PlayerAvatar_test.cpp").read_text(encoding="utf-8")
        for name in ("FrozenRosterRestoresSequentiallyAndContinuesBitExactly",
                     "FrozenReservationsRefuseMalformedAndConflictingCaptures"):
            self.assertIn(name, native)
        self.assertIn("std::span(captured).subspan(slot + 1)", native)
        server_test = (ROOT / "test/persistence/world_open_refusal_test.cpp").read_text(encoding="utf-8")
        self.assertIn("ServerFrozenRosterRestoresSequentiallyAndContinues", server_test)

    def test_avatar_contact_references_accept_only_validated_exact_reservations(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        restore = function(source, "bool PhysicsSystem::restore_avatar_owner(")
        check = "if (!ReferencesExist(recipe, *m_jolt_system, reserved)) { return false; }"
        self.assertIn(check, restore)
        for admission in ("ParseController(pending, reservation)",
                          "reserved.push_back(reservation.body)", "ValidateOwnerAllocator("):
            self.assertLess(restore.index(admission), restore.index(check))
        self.assertLess(restore.index(check), restore.index("PrepareOwnerAllocatorScratch("))
        references = function(source, "bool ReferencesExist(")
        self.assertIn("if (std::find(reserved.begin(), reserved.end(), recipe.references[i]) "
                      "!= reserved.end()) { continue; }", references)
        self.assertIn("JPH::BodyID(recipe.references[i])", references)
        self.assertIn("if (!lock.Succeeded()) { return false; }", references)
        self.assertNotIn("GetIndex()", references)
        native = (ROOT / "test/common/PlayerAvatar_test.cpp").read_text(encoding="utf-8")
        regression = function(native, "TEST(ControllerContinuation, "
                              "FrozenContactsResolveLaterCapsulesAndRefuseUnreservedGenerations)")
        for token in ("source.set_avatar_frozen(1, true,", "source.update_avatars(dt)",
                      "ASSERT_TRUE(source.is_avatar_grounded(0))",
                      "source.set_avatar_frozen(0, true,", "ASSERT_TRUE(ValidatePhysicsOwner(stale))",
                      "EXPECT_FALSE(target_world.restore_avatar_owner(captured[0], index, "
                      "std::span(&stale, 1)))", "EXPECT_EQ(index, 999u)",
                      "EXPECT_EQ(target.avatar_character_slot_count(), 0u)",
                      "ExpectBodySet(before, after)", "std::span(captured).subspan(slot + 1)",
                      "ASSERT_TRUE(target.capture_body_owner(JPH::BodyID(word(restored, 28)), "
                      "restored_capsule))", "ExpectControllerBits(expected, restored)",
                      "EXPECT_EQ(word(restored, 28), capsule)"):
            self.assertIn(token, regression)
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("common/PlayerAvatar_test.cpp", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_live_avatar_restore_derives_pending_allocator_before_reference_admission(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        restore = function(source, "bool PhysicsSystem::restore_avatar_owner(")
        self.assertNotIn("if (!remaining_frozen.empty() && "
                         "recipe.body == JPH::BodyID::cInvalidBodyID)", restore)
        topology = function(restore, "if (recipe.body == JPH::BodyID::cInvalidBodyID && reserved.empty())")
        self.assertIn("recipe.slot_count = reservation.slot_count;", topology)
        self.assertIn("recipe.free_slots = reservation.free_slots;", topology)
        self.assertLess(restore.index("ParseController(pending, reservation)"),
                        restore.index("recipe.slot_count = reservation.slot_count;"))
        for comparison in ("reservation.slot_count != recipe.slot_count",
                           "reservation.free_slots != recipe.free_slots"):
            self.assertLess(restore.index("recipe.free_slots = reservation.free_slots;"),
                            restore.index(comparison))
            self.assertLess(restore.index(comparison), restore.index("reserved.push_back("))
        existing = function(restore, "if (recipe.body != JPH::BodyID::cInvalidBodyID || !reserved.empty())")
        self.assertIn("m_jolt_system->GetBodies(existing);", existing)
        self.assertIn("needs_capsule ? std::span(&recipe, 1) : std::span<const OwnerRecipe>{}", restore)
        self.assertLess(restore.index("ValidateOwnerAllocator("), restore.index("ReferencesExist("))
        self.assertLess(restore.index("ReferencesExist("), restore.index("PrepareOwnerAllocatorScratch("))
        native = (ROOT / "test/common/PlayerAvatar_test.cpp").read_text(encoding="utf-8")
        regression = function(native, "TEST(ControllerContinuation, "
                              "MixedRosterContactsRestoreInPlayerOrderWithExactReservations)")
        for token in ("for (const auto& [free_slot, frozen_without_capsule] :",
                      "std::pair{false, false}", "std::pair{false, true}",
                      "std::pair{true, false}", "std::pair{true, true}",
                      "source.set_avatar_frozen(1, true,", "source.update_avatars(dt)",
                      "ASSERT_TRUE(source.is_avatar_grounded(0))",
                      "ASSERT_EQ(word(captured[0], 20), JPH::BodyID::cInvalidBodyID)",
                      "ASSERT_TRUE(ValidatePhysicsOwner(current))",
                      "EXPECT_FALSE(target_world.restore_avatar_owner(",
                      "std::span(pending).first(pending_count)", "EXPECT_EQ(index, 999u)",
                      "EXPECT_EQ(target.avatar_character_slot_count(), 0u)",
                      "ExpectBodySet(before, after)", "std::span(captured).subspan(slot + 1)",
                      "ExpectControllerBits(captured[slot], restored)", "ExpectBodySet(expected, actual)"):
            self.assertIn(token, regression)
        self.assertNotIn("source.set_avatar_frozen(0, true,", regression)

    def test_session_avatar_restore_enforces_player_slot_before_publication(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        restore = function(source, "bool GameSession::RestoreAvatarOwner(")
        self.assertIn("if (!m_physicsSystem ||", restore)
        admission = function(restore, "if (!m_physicsSystem ||")
        self.assertIn("state.avatar.player_id != m_physicsSystem->avatar_character_slot_count()",
                      restore)
        self.assertEqual(admission, "return false;")
        self.assertLess(restore.index("state.avatar.player_id !="),
                        restore.index(".restore_avatar_owner("))
        self.assertLess(restore.index("state.avatar.player_id !="), restore.index("avatar = state.avatar"))
        native = (ROOT / "test/common/PlayerAvatar_test.cpp").read_text(encoding="utf-8")
        regression = function(native, "TEST(GameSessionHeadlessWorldTest, "
                              "AvatarRestoreRefusesOutOfOrderAndMisalignedRosterSlots)")
        for token in ("jobs.startup(1)", "session.SetJobSystem(&jobs)",
                      "for (const int failure : {0, 1, 2, 3})",
                      "target->create_avatar_character(Vec3(16, 40, 8))",
                      "ValidateAvatarOwner(invalid)",
                      "ASSERT_TRUE(session.RestoreAvatarOwner(captured, restored, restored_index))",
                      "EXPECT_FALSE(session.RestoreAvatarOwner(invalid, output, index))",
                      "EXPECT_EQ(index, 999u)", "EXPECT_EQ(output.player_id, 77u)",
                      "EXPECT_EQ(output.position, Vec3(1))", "EXPECT_EQ(output.facing, 2)",
                      "EXPECT_EQ(output.velocity, Vec3(3))",
                      "EXPECT_EQ(target->avatar_character_slot_count(), slots)",
                      "ExpectBodySet(before, after)", "ExpectControllerBits(previous, retained.physics)"):
            self.assertIn(token, regression)

    def test_avatar_restore_refuses_after_first_tick_before_mutation(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        restore = function(source, "bool GameSession::RestoreAvatarOwner(")
        guard = "GetSimulationTickCount() != 0"
        self.assertIn(guard, restore)
        self.assertEqual(function(restore, "if (!m_physicsSystem ||"), "return false;")
        self.assertLess(restore.index(guard), restore.index(".restore_avatar_owner("))
        self.assertLess(restore.index(guard), restore.index("avatar = state.avatar"))
        runner = (ROOT / "src/luminumbra_server/ServerWorldRunner.h").read_text(
            encoding="utf-8")
        forward = function(runner.replace("remaining_frozen = {}", "remaining_frozen"),
                           "bool RestoreAvatarOwner(")
        self.assertIn(guard, forward)
        self.assertEqual(function(forward, "if (!m_session ||"), "return false;")
        self.assertLess(forward.index(guard), forward.index("m_avatars.reserve("))
        self.assertLess(forward.index(guard), forward.index("m_session->RestoreAvatarOwner("))
        native = (ROOT / "test/persistence/world_open_refusal_test.cpp").read_text(
            encoding="utf-8")
        regression = function(native, "TEST_F(WorldOpenRefusal, "
                              "AvatarRestoreRefusesAfterFirstTickWithoutMutation)")
        self.assertIn("ExpectAvatarRestoreRefusedWithoutMutation(target, captured, through_runner)",
                      regression)
        regression += " " + function(native, "void ExpectAvatarRestoreRefusedWithoutMutation(")
        for token in ("for (const bool regional : {false, true})",
                      "ASSERT_EQ(target.Session()->ActiveRegionsEnabled(), regional)",
                      "ASSERT_EQ(target.TickCount(), 0u)",
                      "ASSERT_TRUE(target.RestoreAvatarOwner(captured))",
                      "ASSERT_EQ(target.RunFixedTicks(1).ticks_executed, 1u)",
                      "captured.avatar.player_id = 1", "ValidateAvatarOwner(captured)",
                      "for (const bool through_runner : {false, true})",
                      "EXPECT_FALSE(target.RestoreAvatarOwner(captured))",
                      "EXPECT_FALSE(target.Session()->RestoreAvatarOwner(captured, output, index))",
                      "EXPECT_EQ(index, 999u)", "EXPECT_EQ(output.player_id, 77u)",
                      "EXPECT_EQ(output.position, Vec3(1))", "EXPECT_EQ(output.facing, 2)",
                      "EXPECT_EQ(output.velocity, Vec3(3))",
                      "EXPECT_EQ(target.Avatars().data(), roster_data)",
                      "EXPECT_EQ(target.Avatars().capacity(), roster_capacity)",
                      "ASSERT_EQ(target.Avatars().size(), 1u)",
                      "EXPECT_EQ(physics->avatar_character_slot_count(), 1u)",
                      "EXPECT_EQ(before_size, after_size)", "EXPECT_EQ(before_bytes, after_bytes)",
                      "EXPECT_EQ(bodies_before.slot_count, bodies_after.slot_count)",
                      "EXPECT_EQ(bodies_before.free_slots, bodies_after.free_slots)",
                      "ASSERT_EQ(bodies_before.owners.size(), bodies_after.owners.size())",
                      "EXPECT_EQ(before.bytes, after.bytes)",
                      "EXPECT_EQ(before.registration_slot, after.registration_slot)",
                      "EXPECT_EQ(target.ComputeWorldHash(), hash_before)",
                      "EXPECT_EQ(target.TickCount(), 1u)"):
            self.assertIn(token, regression)
        self.assertLess(regression.index("ASSERT_TRUE(target.RestoreAvatarOwner(captured))"),
                        regression.index("target.RunFixedTicks(1)"))
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("persistence/world_open_refusal_test.cpp", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_avatar_restore_checks_simulation_bounds_before_staging(self):
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(
            encoding="utf-8")
        restore = function(source, "bool PhysicsSystem::restore_avatar_owner(")
        admission = function(restore, "if (!m_jolt_system")
        self.assertIn("return false;", admission)
        self.assertIn("recipe.feet.GetX()", restore)
        check = restore.index("!SimulationHorizon::valid(")
        self.assertLess(check, restore.index("PrepareOwnerAllocatorScratch("))
        self.assertLess(restore.index("recipe.feet.GetX()"), restore.index("return false;"))
        native = (ROOT / "test/common/PlayerAvatar_test.cpp").read_text(encoding="utf-8")
        regression = function(native, "TEST(GameSessionHeadlessWorldTest, "
                              "OutOfBoundsAvatarRestorePublishesNothingAndTicksContinue)")
        for token in ("jobs.startup(1)", "session.SetJobSystem(&jobs)",
                      "set_durable_residency(true)", "session.SetActiveRegionsEnabled(true)",
                      "ValidateAvatarOwner(invalid)",
                      "EXPECT_FALSE(session.RestoreAvatarOwner(invalid, output, index))",
                      "EXPECT_EQ(target->avatar_character_slot_count(), 0u)",
                      "session.TickSimulation("):
            self.assertIn(token, regression)

    def assert_player_continuation_session_bridge(self, header, source):
        self.assertNotIn("OwnerContinuationTestAccess", header + source)
        adapter = function(header, "class OwnerContinuation")
        self.assertIn("friend class ::Luminumbra::world::GameSession;", adapter)
        self.assertIn("explicit OwnerContinuation(", adapter[adapter.index("private:"):])
        private = header[header.index("private:\n    bool capture_player_owner"):]
        for operation in ("capture", "restore"):
            method = operation + "_player_owner"
            self.assertIn(method, adapter)
            self.assertIn(method, private)
            self.assertNotIn(method, header[:header.index("class OwnerContinuation")])
            bridge = function(source, "bool GameSession::" + operation.title() + "PlayerOwner(")
            self.assertIn("m_physicsSystem &&", bridge)
            self.assertIn("Systems::PhysicsSystem::OwnerContinuation(*m_physicsSystem)." + method,
                          bridge)
            self.assertIn("m_physics." + method,
                          function(source, "bool PhysicsSystem::OwnerContinuation::" + method + "("))
        restore = function(source, "bool PhysicsSystem::restore_player_owner(")
        for token in ("m_player_character ||", "!ParseController(state, recipe)",
                      "recipe.kind != 2u", "!SimulationHorizon::valid(",
                      "recipe.feet.GetX()", "recipe.feet.GetY()", "recipe.feet.GetZ()",
                      "!ReferencesExist(recipe, *m_jolt_system, std::span<const std::uint32_t>{})", "return false;"):
            self.assertIn(token, restore)
            for allocation in ("FeetCapsule(", "PrepareController("):
                self.assertLess(restore.index(token), restore.index(allocation))
        for token in ("const auto standing = FeetCapsule(",
                      "const auto crouching = FeetCapsule(",
                      "auto character = PrepareController(",
                      "recipe.flags == 2u ? crouching : standing",
                      "character->RestoreState(recipe.owner);"):
            self.assertIn(token, restore)
            for publication in ("m_player_stand_shape =", "m_player_crouch_shape =",
                                "m_player_character ="):
                self.assertLess(restore.index(token), restore.index(publication))
        self.assertIn("m_player_character = std::move(character);", restore)

    def test_player_continuation_exists_only_through_session_bridge(self):
        header = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.h").read_text(encoding="utf-8")
        source = (ROOT / "src/luminumbra_common/systems/PhysicsSystem.cpp").read_text(encoding="utf-8")
        self.assert_player_continuation_session_bridge(header, source)
        for token in ("!SimulationHorizon::valid(", "!ReferencesExist(recipe, *m_jolt_system, std::span<const std::uint32_t>{})",
                      "character->RestoreState(recipe.owner);",
                      ".capture_player_owner(output)", ".restore_player_owner(state)"):
            with self.subTest(missing=token), self.assertRaises(AssertionError):
                self.assert_player_continuation_session_bridge(header, source.replace(token, ""))
        with self.assertRaises(AssertionError):
            self.assert_player_continuation_session_bridge(header + "OwnerContinuationTestAccess", source)
        session = (ROOT / "src/luminumbra_common/world/GameSession.h").read_text(encoding="utf-8")
        for token in ("bool CapturePlayerOwner(", "bool RestorePlayerOwner("):
            self.assertIn(token, session)
        runner = (ROOT / "src/luminumbra_server/ServerWorldRunner.h").read_text(encoding="utf-8")
        self.assertNotIn("PlayerOwner(", runner)
        native = (ROOT / "test/common/PlayerAvatar_test.cpp").read_text(encoding="utf-8")
        for name in ("PlayerFallingGroundedAndCrouchedResumeBitExactly",
                     "PlayerRefusalsPreserveOwnerAndCaptureOutput",
                     "OutOfBoundsPlayerRestorePublishesNothingAndTicksContinue"):
            self.assertIn(name, native)
        regression = function(native, "TEST(GameSessionHeadlessWorldTest, "
                              "OutOfBoundsPlayerRestorePublishesNothingAndTicksContinue)")
        for token in ("jobs.startup(1)", "session.SetJobSystem(&jobs)",
                      "set_durable_residency(true)", "session.SetActiveRegionsEnabled(true)",
                      "ValidatePhysicsOwner(invalid)",
                      "EXPECT_FALSE(session.RestorePlayerOwner(invalid))",
                      "EXPECT_FALSE(session.CapturePlayerOwner(absent))",
                      "ExpectBodySet(before, after)", "session.TickSimulation("):
            self.assertIn(token, regression)
        failures = (ROOT / "test/physics/jolt_startup_test.cpp").read_text(encoding="utf-8")
        sweep = function(failures, "void ControllerRestoreAllocationSweep(")
        for token in ("const bool player = mode >= 3", "source.set_player_crouched(mode == 4)",
                      "target_world.restore_player_owner(captured)",
                      "!target_world.capture_player_owner(absent)", "Require(restore())",
                      "RequireOwnerEqual(captured, actual)"):
            self.assertIn(token, sweep)
        self.assertIn("mode < 5", function(failures, "TEST(JoltStartup, "
                                         "ControllerAllocationFailuresPublishNothingAndRetryExactlyOnce)"))

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
        self.assertEqual(client.count("create_avatar_character("), 2)
        self.assertEqual(client.count("PhysicsSystem::kInvalidAvatar"), 2)
        self.assertEqual(attachment.count("create_avatar_character("), 1)
        self.assertEqual(attachment.count("PhysicsSystem::kInvalidAvatar"), 1)
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

    def attachment_source_and_body(self):
        source = (ROOT / "src/luminumbra_client/app/CreatureBodyAttachment.h").read_text(
            encoding="utf-8")
        return source, function(source, "inline void AttachMissingCreatureBodies(")

    def attachment_regression(self, name):
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(
            encoding="utf-8")
        native = re.sub(r"(TEST_F\(CreatureAttachmentTest,)\s+", r"\1 ", native)
        return function(native, "TEST_F(CreatureAttachmentTest, " + name + ")")

    def test_creature_attachment_continues_after_refusal_with_bounded_attempts(self):
        source, attach = self.attachment_source_and_body()
        refusal = function(attach, "if (idx == Systems::PhysicsSystem::kInvalidAvatar)")
        self.assertIn("continue;", refusal)
        self.assertNotIn("break;", refusal)
        self.assertIn("int maxPerFrame = 4", source)
        self.assertIn("int maxAttemptsPerFrame = 16", source)
        self.assertIn("made >= maxPerFrame", attach)
        self.assertIn("attempted >= maxAttemptsPerFrame", attach)
        self.assertLess(attach.index("++attempted;"), attach.index("create_avatar_character("))
        self.assertLess(attach.index("create_avatar_character("),
                        attach.index("reg.emplace<Components::CreaturePhysicsComponent>"))
        case = self.attachment_regression(
            "CreatureAttachmentSkipsRefusalsWithinAnIndependentAttemptBudget")
        for token in ("AttachMissingCreatureBodies(&physics, registry, cursor, 1, 1);",
                      "AttachMissingCreatureBodies(&physics, registry, cursor, 1, 2);",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(first));",
                      "EXPECT_EQ(physics.avatar_character_slot_count(), 14u);",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(refused));"):
            self.assertIn(token, case)
        cap = self.attachment_regression("CreatureAttachmentDefaultCreationBudgetIsFour")
        self.assertIn("std::array<entt::entity, 6>", cap)
        self.assertIn("EXPECT_EQ(physics.avatar_character_slot_count(), 4u);", cap)
        self.assertIn("EXPECT_EQ(physics.avatar_character_slot_count(), 6u);", cap)

    def test_creature_attachment_cursor_advances_and_wraps_with_bounded_scan(self):
        _, attach = self.attachment_source_and_body()
        for token in ("const auto& creatures = reg.storage<Components::CreatureComponent>();",
                      "const auto count = creatures.size();", "visited < count",
                      "visited < static_cast<std::size_t>(maxAttemptsPerFrame)",
                      "const auto entity = creatures.data()[cursor.next];", "++cursor.next;",
                      "reg.all_of<Components::CreaturePhysicsComponent>(entity)",
                      "!reg.all_of<Components::TransformComponent>(entity)"):
            self.assertIn(token, attach)
        self.assertEqual(function(attach, "if (cursor.next == count)"), "cursor.next = 0;")
        self.assertLess(attach.index("++cursor.next;"), attach.index("reg.all_of<"))
        self.assertLess(attach.index("++cursor.next;"), attach.index("create_avatar_character("))
        case = self.attachment_regression(
            "CreatureAttachmentEventuallyPassesMoreThanSixteenPersistentRefusals")
        for token in ("std::array<entt::entity, 33>", "call <= 2",
                      "EXPECT_EQ(cursor.next, call * 16);",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(admissible));",
                      "EXPECT_FALSE(registry.all_of<CreaturePhysicsComponent>(entity));"):
            self.assertIn(token, case)
        skipped = self.attachment_regression("CreatureAttachmentBoundsScanningAndWrapsPastSkippedSlots")
        for token in ("EXPECT_EQ(cursor.next, 16u);", "index < 15",
                      "EXPECT_FALSE(registry.all_of<CreaturePhysicsComponent>(admissible));",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(admissible));",
                      "EXPECT_EQ(registry.get<CreaturePhysicsComponent>(attached).avatar_index, body);"):
            self.assertIn(token, skipped)
        # No whole-registry view construction or search to rediscover the cursor.
        for token in ("reg.view<", "std::find", "static "):
            self.assertNotIn(token, attach)

    def test_creature_attachment_invalid_bounds_and_null_physics_are_no_ops(self):
        _, attach = self.attachment_source_and_body()
        guard = "if (!phys || maxPerFrame <= 0 || maxAttemptsPerFrame <= 0)"
        self.assertEqual(function(attach, guard), "return;")
        self.assertLess(attach.index(guard), attach.index("reg.storage<"))
        self.assertLess(attach.index(guard), attach.index("cursor.next"))
        case = self.attachment_regression("CreatureAttachmentDisabledBoundsAndNullPhysicsDoNoWork")
        for call in ("nullptr, registry, cursor", "&physics, registry, cursor, 0, 2",
                     "&physics, registry, cursor, -1, 2", "&physics, registry, cursor, 1, 0",
                     "&physics, registry, cursor, 1, -1"):
            self.assertIn("AttachMissingCreatureBodies(" + call + ");", case)
        self.assertIn("EXPECT_EQ(cursor.next, 5u);", case)
        self.assertIn("EXPECT_EQ(physics.avatar_character_slot_count(), 0u);", case)

    def test_creature_attachment_cursor_handles_destroyed_and_reused_entities(self):
        source, attach = self.attachment_source_and_body()
        self.assertEqual(function(source, "struct CreatureBodyAttachmentCursor"),
                         "std::size_t next = 0;")
        self.assertIn("CreatureBodyAttachmentCursor& cursor", source)
        self.assertEqual(function(attach, "if (cursor.next >= count)"), "cursor.next = 0;")
        self.assertLess(attach.index("if (cursor.next >= count)"),
                        attach.index("creatures.data()[cursor.next]"))
        case = self.attachment_regression("CreatureAttachmentCursorSurvivesDestructionAndEntityReuse")
        for token in ("registry.destroy(destroyed);", "ASSERT_NE(reused, destroyed);",
                      "ASSERT_EQ(entt::to_entity(reused), entt::to_entity(destroyed));",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(swapped));",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(reused));"):
            self.assertIn(token, case)
        shrink = self.attachment_regression("CreatureAttachmentCursorResetsAfterShrinkAndClear")
        for token in ("ASSERT_EQ(cursor.next, 2u);", "registry.destroy(first);",
                      "registry.destroy(second);", "registry.clear();",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(tail));",
                      "EXPECT_TRUE(registry.all_of<CreaturePhysicsComponent>(newborn));"):
            self.assertIn(token, shrink)
        client = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        runtime = function(client, "struct ClientRuntimeState")
        self.assertIn("Luminumbra::Client::CreatureBodyAttachmentCursor creatureBodyAttachmentCursor;", runtime)
        self.assertIn("Luminumbra::Client::AttachMissingCreatureBodies( gameSession->GetPhysicsSystem(), "
                      "gameSession->GetRegistry(), runtime.creatureBodyAttachmentCursor);",
                      re.sub(r"\s+", " ", client))
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("common/GameSessionHeadlessWorld_test.cpp", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

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
