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
                "SimulationPreparationVisitsOnlyLiveAvatarOwners",
                "TeleportPreparesDestinationBeforeTheLocalCharacterSteps",
                "RenderPreparationRefreshesEvenWhenTheUpcomingTickDoesNotChange",
                "CollisionSamplingCachesProceduralColumnsAndPrefersCommittedHeights")),
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


if __name__ == "__main__":
    unittest.main()
