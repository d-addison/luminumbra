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
                         "bool residency_free_eviction = false; bool simulation_host_integration = false;")
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
        self.assertIn("PrepareSimulationCollisions(", body)
        self.assertIn("GetSimulationTickCount() + 1" if "GameSession" not in (name or "")
                      else "PrepareSimulationCollisions(current_tick)", body)
        prepare = body.index("PrepareSimulationCollisions(")
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
                self.assert_barrier(source.replace("PrepareSimulationCollisions(",
                                                   "removed_preparation(", 1), name, step)

    def test_lockstep_refusal_waits_before_consuming_either_peer_input(self):
        source = (ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp").read_text(
            encoding="utf-8")
        step = function(source, "bool NetworkedSessionDriver::StepAgreedTick(")
        prepare = step.index("PrepareSimulationCollisions(")
        self.assertIn("SetHostSimulationAnchors({&ctx->spawn_anchor, 1})", step)
        self.assertIn("&m_impl->host_ctx, &m_impl->client_ctx", step)
        self.assertLess(prepare, step.index("m_impl->host->PumpTick("))
        self.assertIn("return true;", step[prepare:step.index("m_impl->host->PumpTick(")])

    def test_traversal_catchup_stops_on_a_refused_tick(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        loop = source.split("while (!gameSession->GetRegionSchedulerFault()", 1)[1].split(
            "traversal_ticks += advanced;", 1)[0]
        self.assertIn("const auto advanced =", loop)
        self.assertIn("if (advanced == 0)", loop)
        self.assertIn("break;", loop.split("if (advanced == 0)", 1)[1])

    def test_collision_regressions_remain_registered(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        for path, cases in (
            ("common/GameSessionHeadlessWorld_test.cpp", (
                "SimulationHorizonIgnoresRenderCollidersAndIncludesEveryBodyOwner",
                "SimulationPreparationRefusesWithoutPublicationAndRetryMatchesPeer",
                "SimulationPreparationBoundsInputsAndMissingAuthority",
                "SimulationPreparationCanonicalizesOrderAndPreservesLegacyPath",
                "SimulationOwnerRegistrationAndTickReuseStayBounded",
                "SimulationPreparationVisitsOnlyLiveAvatarOwners")),
            ("common/GameSessionHeadlessWorld_test.cpp", (
                "SimulationCollisionsIgnoreRenderCadenceAcrossSessionTicks",
                "LegacyColliderPreparationPreservesHashesAndSaveBytes",
                "RefusedCollisionBarrierPreservesTheSessionTickAndAccumulator",
                "SessionCollisionBarrierPrecedesTheFirstCreatureStep")),
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

    def test_expected_collision_authority_cannot_be_masked_by_water_or_unknown_metadata(self):
        source = (ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp").read_text(
            encoding="utf-8")
        sample = function(source, "bool SHIELD_WorldSystem::sample_simulation_collision(")
        self.assertIn("expected && (!chunk || !chunk->has_terrain_authority())", sample)
        self.assertIn("expected && (!chunk || !chunk->has_simulation_authority())", sample)
        self.assertLess(sample.index("read_committed_chunk("), sample.index("std::copy("))


class SimulationHostWiring(unittest.TestCase):
    @staticmethod
    def read(path):
        return (ROOT / path).read_text(encoding="utf-8")

    def assert_shared_inputs(self, session, header, server, network, client):
        gate = function(header, "bool SimulationHostIntegrationEnabled() const")
        self.assertEqual(gate, "return m_activeRegionsEnabled && "
                              "m_authorityProfile.simulation_host_integration;")
        options = function(header, "struct AuthorityProfileOptions")
        self.assertIn("bool simulation_host_integration = false;", options)
        anchors = function(session, "GameSession::SimulationAnchors() const")
        self.assertIn("if (m_replicatedSimulationAnchors)", anchors)
        self.assertIn("return *m_replicatedSimulationAnchors;", anchors)
        self.assertIn("m_activeRegionLedger.local_anchor()", anchors)
        publish_inputs = function(session, "bool GameSession::SetHostSimulationAnchors(")
        self.assertIn("m_activeRegionLedger.set_local_anchor(anchors.front())", publish_inputs)
        prepare = function(session, "bool GameSession::PrepareSimulationCollisions(")
        self.assertIn("SimulationHostIntegrationEnabled() ? SimulationAnchors() : "
                      "std::span<const Vec3>{}", prepare)
        self.assertIn("m_physicsSystem->prepare_simulation_collisions(", prepare)
        tick = function(session, "std::uint32_t GameSession::TickSimulation(")
        self.assertIn("UpdateRegionalFromSimulation(m_registry, SimulationAnchors(),", tick)
        self.assertLess(tick.index("PrepareSimulationCollisions(current_tick)"),
                        tick.index("m_physicsSystem->update(fixed_dt)"))
        self.assertLess(tick.index("m_physicsSystem->update(fixed_dt)"),
                        tick.index("UpdateRegionalFromSimulation("))
        publish = function(server, "ServerTickReport ServerWorldRunner::RunFixedTicks(")
        self.assertIn("anchors[count++] = avatar.position;", publish)
        self.assertIn("anchors[count++] = *smoke_anchor;", publish)
        self.assertIn("SetHostSimulationAnchors({anchors.data(), count})", publish)
        self.assertLess(publish.index("SetHostSimulationAnchors("),
                        publish.index("PrepareSimulationCollisions("))
        self.assertIn("m_avatars.size() > anchors.size()", publish)
        self.assertIn("ActiveRegionsEnabled() && !m_session->SimulationHostIntegrationEnabled()", publish)
        for entry in ("bool NetSessionApplyStep(", "bool NetworkedSessionDriver::StepAgreedTick("):
            step = function(network, entry)
            self.assertIn("SetHostSimulationAnchors({&ctx->spawn_anchor, 1})", step)
            self.assertLess(step.index("SetHostSimulationAnchors("),
                            step.index("PrepareSimulationCollisions("))
        # Every legacy external rigid step is excluded from the fixed-tick profile.
        for source, name, owner in (
                (server, "ServerTickReport ServerWorldRunner::RunFixedTicks(", "m_session"),
                (network, "bool NetSessionApplyStep(", "ctx->session")):
            body = function(source, name)
            legacy = function(body, f"if (!{owner}->SimulationHostIntegrationEnabled())")
            self.assertIn("physics_system->update(", legacy)
        play = function(client, "int main(").split("case GameState::IN_GAME:", 1)[1].split("case GameState::", 1)[0]
        local_preflight = function(play, "if (advance_frame_simulation && "
                                   "!scenario_config.networked_session_smoke())")
        self.assertIn("gameSession->PrepareSimulationCollisions(", local_preflight)
        self.assertNotIn("SimulationHostIntegrationEnabled", local_preflight)
        local_physics = function(play, "if (advance_frame_simulation)")
        local_legacy = function(local_physics, "if (!gameSession->SimulationHostIntegrationEnabled())")
        self.assertIn("physics->update(deltaTime * g_app.capture.timeScale)", local_legacy)
        self.assertRegex(play, r"if \(benchmark_v4 && g_camera && "
                              r"!gameSession->SimulationHostIntegrationEnabled\(\)\) "
                              r"gameSession->SetLocalPlayerSimulationPosition\(g_camera->Position, true\)")
        self.assertIn("g_playerController->GetPosition()", play)

    def assert_smoke(self, session, server, native):
        choose = function(session, "GameSession::DefaultSimulationSmokeAnchor() const")
        for token in ("z = -4; z <= 4", "x = -4; x <= 4", "x * 512", "z * 512 + 8",
                      "WaterLevelAt(boundary - 8.0f, centre_z) > left + 0.25f",
                      "WaterLevelAt(boundary + 8.0f, centre_z) > right + 0.25f",
                      "return std::nullopt;", "row < resolution",
                      "return Vec3(boundary - 8.0f, std::max(left, right) + 2.0f, centre_z);"):
            self.assertIn(token, choose)
        self.assertNotIn("snapshot_", choose)
        self.assertNotIn("spawnPoint", choose)
        run = function(server, "ServerTickReport ServerWorldRunner::RunFixedTicks(")
        self.assertIn("!smoke_anchor", run)
        self.assertIn('"Simulation smoke requires a wet region seam"', run)
        self.assertLess(run.index("DefaultSimulationSmokeAnchor()"), run.index("while ("))
        smoke = function(native, "TEST(GameSessionHeadlessWorldTest, "
                         "DefaultSimulationSmokeHasPositiveWetRegionSeamPairs)")
        for token in ("session.DefaultSimulationSmokeAnchor()", "ASSERT_TRUE(anchor.has_value())",
                      "session.SetHostSimulationAnchors(anchors)", "EXPECT_GT(wet_pairs, 0)",
                      "EXPECT_GT(regional_wet_pairs, 0)",
                      "for (const bool high_resolution : {false, true})",
                      "left->water_depth_mm[row * resolution + resolution - 1] > 0",
                      "right->water_depth_mm[row * resolution] > 0"):
            self.assertIn(token, smoke)

    def sources(self):
        return tuple(self.read(path) for path in (
            "src/luminumbra_common/world/GameSession.cpp",
            "src/luminumbra_common/world/GameSession.h",
            "src/luminumbra_server/ServerWorldRunner.cpp",
            "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp",
            "src/luminumbra_client/main_client.cpp"))

    def test_all_shipping_hosts_share_simulation_inputs_at_the_tick_boundary(self):
        self.assert_shared_inputs(*self.sources())

    def test_default_smoke_is_wet_on_both_sides_of_a_region_seam(self):
        session, _, server, _, _ = self.sources()
        self.assert_smoke(session, server, self.read("test/common/GameSessionHeadlessWorld_test.cpp"))

    def test_camera_preparation_and_dry_smoke_mutations_are_detected(self):
        sources = self.sources()
        for index, old, new in (
                (0, "SimulationHostIntegrationEnabled() ? SimulationAnchors()",
                 "SimulationHostIntegrationEnabled() ? camera_anchors"),
                (2, "anchors[count++] = avatar.position;", "anchors[count++] = camera.position;"),
                (3, "SetHostSimulationAnchors({&ctx->spawn_anchor, 1})",
                 "SetHostSimulationAnchors({&camera.position, 1})"),
                (4, "benchmark_v4 && g_camera &&\n                            !gameSession->SimulationHostIntegrationEnabled()",
                 "benchmark_v4 && g_camera")):
            mutated = list(sources)
            self.assertIn(old, mutated[index])
            mutated[index] = mutated[index].replace(old, new, 1)
            with self.subTest(index=index), self.assertRaises(AssertionError):
                self.assert_shared_inputs(*mutated)
        session, _, server, _, _ = sources
        native = self.read("test/common/GameSessionHeadlessWorld_test.cpp")
        for old, new in (("EXPECT_GT(wet_pairs, 0)", "EXPECT_GE(wet_pairs, 0)"),
                         ("EXPECT_GT(regional_wet_pairs, 0)", "EXPECT_GE(regional_wet_pairs, 0)"),
                         ("session.SetHostSimulationAnchors(anchors)", "session.SetHostSimulationAnchors({})")):
            self.assertIn(old, native)
            with self.subTest(old=old), self.assertRaises(AssertionError):
                self.assert_smoke(session, server, native.replace(old, new))

    def test_avatar_step_cannot_scan_removed_owner_slots(self):
        source = self.read("src/luminumbra_common/systems/PhysicsSystem.cpp")
        step = function(source, "void PhysicsSystem::update_avatars(")
        live = "for (auto i = m_avatar_head; i != kNoAvatar; i = m_avatars[i].next)"
        self.assertIn(live, step)
        self.assertNotIn("m_avatars.size()", step)
        with self.assertRaises(AssertionError):
            self.assertIn(live, step.replace(live,
                          "for (std::size_t i = 0; i < m_avatars.size(); ++i)"))

    def test_flag_off_profile_keeps_the_existing_exact_save_golden(self):
        native = self.read("test/common/GameSessionPlantPersistence_test.cpp")
        golden = function(native, "TEST(GameSessionHeadlessWorldTest, "
                          "DisabledRegionsMatchDevelWorldHashAndExactSaveBytes)")
        for token in (".simulation_host_integration = true", "s->SetActiveRegionsEnabled(false)",
                      "expected.SetHostSimulationAnchors(ignored)",
                      "expected.PrepareSimulationCollisions(1)", "ExpectSameWorldHash(session, expected)",
                      "expected.SaveWorldStateTo(profile_save)", "EXPECT_EQ(profile_files, 2u)",
                      "ReadClockTestFile(fixture / entry.path().filename())"):
            self.assertIn(token, golden)

    def test_native_host_regressions_are_registered_and_nonvacuous(self):
        native = self.read("test/common/GameSessionHeadlessWorld_test.cpp")
        for case in ("BoundaryWaterAndFreezeIgnoreCameraFramesAndRestore",
                     "HostBoundaryInputsShareAuthoritySequenceWithoutRenderCadence",
                     "DefaultSimulationSmokeHasPositiveWetRegionSeamPairs",
                     "HostAnchorAdmissionRefusesInvalidInputsWithoutReplacement",
                     "HostCollisionRefusalPreservesTheTickAndRetriesTheSameInputs",
                     "DefaultSimulationSmokeRefusesAnEntirelyDryPreset"):
            self.assertRegex(native, r"TEST\(GameSessionHeadlessWorldTest,\s*" + case + r"\)")
        boundary = function(native, "TEST(GameSessionHeadlessWorldTest, "
                            "BoundaryWaterAndFreezeIgnoreCameraFramesAndRestore)")
        for token in ("moving.SaveWorldState()", "reloaded.LoadWorld(", "RegionState::Frozen",
                      "EXPECT_NE(record.frozen_digest, 0u)", "EXPECT_NE(flowing->water_depth_mm",
                      "EXPECT_GT(session->GetWaterSystem()->dbg_seam_wet_pairs(), 0)"):
            self.assertIn(token, boundary)
        cmake = self.read("test/CMakeLists.txt")
        self.assertIn("common/GameSessionHeadlessWorld_test.cpp", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)


if __name__ == "__main__":
    unittest.main()
