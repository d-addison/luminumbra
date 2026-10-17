"""Source checks for admission work that must remain outside steady ticks."""

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
            return re.sub(r"\s+", " ", source[start + 1:index]).strip()
    raise AssertionError(f"Unclosed function {name}")


class RegionAuthorityWiring(unittest.TestCase):
    def test_meshing_captures_voxel_revision_with_owner_thread_snapshot(self):
        source = SOURCE.read_text(encoding="utf-8")
        dispatch = function(source, "SHIELD_WorldSystem::dispatch_meshing_jobs(")
        header = SOURCE.with_suffix(".h").read_text(encoding="utf-8")
        record = header.split("struct MeshingJobChunk {", 1)[1].split("};", 1)[0]
        self.assertIn("u32 voxel_revision = 0;", record)
        self.assertIn("chunk->authority_hash_revision(), chunk->voxel_revision()", dispatch)
        self.assertLess(dispatch.index("chunk->voxel_revision()"),
                        dispatch.index("input->sdf_data = chunk->sdf_data"))
        self.assertLess(dispatch.index("input->sdf_data = chunk->sdf_data"),
                        dispatch.index("lane_jobs.emplace_back("))
        worker = dispatch.split("lane_jobs.emplace_back(", 1)[1]
        # Strong owners hold both the destination and immutable input until the
        # job exits. No mutable live arrays or revisions may be read by workers.
        self.assertIn("[this, chunk, input, step, transition_faces, terrain_mesh_required]", worker)
        for live_read in ("chunk->sdf_data", "chunk->heightmap_data", "chunk->material_data",
                          "chunk->water_level_data", "chunk->water_depth_mm",
                          "chunk->voxel_revision()", "chunk->authority_hash_revision()"):
            self.assertNotIn(live_read, worker)

    def test_stale_terrain_guard_precedes_every_terrain_publication_write(self):
        source = SOURCE.read_text(encoding="utf-8")
        publish = function(source, "bool SHIELD_WorldSystem::publish_front_meshing_batch(")
        self.assertIn("const bool terrain_current = "
                      "chunk->voxel_revision() == job_chunk.voxel_revision && "
                      "(job_chunk.voxel_revision != std::numeric_limits<u32>::max() || "
                      "chunk->authority_hash_revision() == job_chunk.authority_revision);", publish)
        guard = "if (job_chunk.terrain_mesh_required && terrain_current)"
        terrain = function(source, guard)
        success = function(source, "if (mesh_ready && !mesh_failed)")
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
        self.assertLess(publish.index("resident->second != chunk"), publish.index(guard))
        self.assertLess(publish.index("if (mesh_ready && !mesh_failed)"), publish.index(guard))

    def test_water_publication_and_cleanup_are_independent_of_terrain_guard(self):
        source = SOURCE.read_text(encoding="utf-8")
        publish = function(source, "bool SHIELD_WorldSystem::publish_front_meshing_batch(")
        guard = "if (job_chunk.terrain_mesh_required && terrain_current)"
        terrain = function(source, guard)
        self.assertNotIn("water_mesh", terrain)
        self.assertNotIn("continue;", terrain)
        self.assertNotIn("return", terrain)
        self.assertIn("chunk->water_mesh_vertices = std::move(chunk->pending_water_mesh_vertices);",
                      publish)
        self.assertIn("chunk->water_mesh_version++;", publish)
        self.assertIn("if (chunk->authority_hash_revision() == job_chunk.authority_revision)",
                      publish)
        for cleanup in ("pending_mesh_vertices.clear()", "pending_mesh_indices.clear()",
                        "pending_water_mesh_vertices.clear()", "pending_water_mesh_indices.clear()",
                        "pending_mesh_ready.store(false", "pending_mesh_failed.store(false",
                        "pending_lod.store(-1"):
            self.assertIn(cleanup, publish)
            self.assertNotIn(cleanup, terrain)
        # All completion entry points converge on the guarded publisher.
        self.assertIn("publish_front_meshing_batch(force)", function(
            source, "void SHIELD_WorldSystem::process_completed_meshing_jobs("))
        self.assertIn("publish_front_meshing_batch(/*force=*/true)", function(
            source, "void SHIELD_WorldSystem::activate_due("))

    def test_edit_reopens_candidates_before_synchronous_preparation_can_refuse(self):
        source = SOURCE.read_text(encoding="utf-8")
        edit = function(source, "int SHIELD_WorldSystem::EditTerrainVoxel(")
        changed = function(source, "if (edited > 0)")
        self.assertIn("++m_dirty_generation;", changed)
        self.assertLess(changed.index("++m_dirty_generation;"),
                        changed.index("EnsureSurfaceReadyNear("))
        self.assertLess(edit.index("mark_voxel_data_dirty()"),
                        edit.index("current_lod.store(-1"))
        native = (ROOT / "test/shield/meshing_hardening_test.cpp").read_text(encoding="utf-8")
        for case in ("DelayedTerrainCompletionRetainsEditInvalidationAcrossBothLanes",
                     "SynchronousEditRebuildCannotBeSkippedByStaleCompletion",
                     "RefusedSynchronousEditReopensSettledCandidateScan",
                     "RefusedWorkerDispatchLeavesEditedMeshAndCollisionUntouched",
                     "StaleTerrainDoesNotDiscardIndependentWaterMeshOutput",
                     "WaterRevisionChangeDoesNotRejectCurrentTerrainCompletion",
                     "MeshSnapshotOwnersSurviveQueuedWorkAndReleaseAfterWorldClear"):
            self.assertIn("TEST(StreamingHardening, " + case + ")", native)

    def test_persistence_pins_and_admits_owner_before_any_dereference(self):
        source = SOURCE.read_text(encoding="utf-8")
        persist = function(source, "bool SHIELD_WorldSystem::persist_authority(")
        self.assertIn("const auto chunk = incoming;", persist)
        self.assertIn("if (!chunk || !chunk->has_simulation_authority() || m_authorityFault) "
                      "return false;", persist)
        self.assertLess(persist.index("if (!chunk"), persist.index("chunk->get_id()"))
        self.assertIn("const auto owner = durable->second.owner.lock();", persist)
        self.assertIn("owner && owner == chunk && "
                      "durable->second.stamp == owner->authority_stamp()", persist)
        self.assertNotIn("owner.lock() == chunk", source)
        cache_source = (ROOT / "src/luminumbra_common/persistence/"
                        "WorldPersistenceRoundtrip.cpp").read_text(encoding="utf-8")
        cache = function(cache_source, "void AuthorityHashCache::update(")
        self.assertIn("const auto chunk = incoming;", cache)
        self.assertIn("if (!chunk || !chunk->has_simulation_authority()) return;", cache)
        self.assertIn("const bool same_owner = owner && owner == chunk;", cache)
        self.assertEqual(cache.count("owner.lock()"), 1)

    def test_refused_payloads_stage_before_creating_destination_directories(self):
        source = (ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp").read_text(
            encoding="utf-8")
        for name, interruption in (
                ("bool WorldSaveService::write_file_durably(",
                 "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool RewriteRegionFile(", "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool WriteWorldManifest(", "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool WorldSaveService::save_regions_validated(",
                 "BeforeActiveRegionsReplaceForTesting()")):
            with self.subTest(writer=name):
                writer = function(source, name)
                self.assertLess(writer.index("WriteDurableRegionTemp"),
                                writer.index(interruption))
                self.assertIn("CreatePayloadDirectory", writer)
                self.assertLess(writer.index(interruption), writer.index("CreatePayloadDirectory"))
                self.assertLess(writer.index("CreatePayloadDirectory"),
                                writer.index("ReplaceRegionFileAtomically"))
                self.assertNotIn("create_directories", writer)
        producer = function(source, "bool WriteDurableRegionTempWithProducer(")
        self.assertLess(producer.index("RegionStagingDestination"),
                        producer.index("OpenDurableRegionTemp(staging"))
        snapshot = function(source, "bool WorldSaveService::write_snapshot(")
        self.assertNotIn("create_directories", snapshot)

    def test_eviction_scan_is_reachable_only_after_capacity_check(self):
        source = SOURCE.read_text(encoding="utf-8")
        room = function(source, "bool SHIELD_WorldSystem::make_authority_room(")
        self.assertTrue(room.startswith(
            "if (resident_chunk_count() < STREAMING_MAX_ACTIVE_CHUNKS_BUDGET) return true; "
            "if (!eviction_prepared) { prepare_authority_eviction(required); "
            "eviction_prepared = true; }"))
        # Includes the definition: there must be no eager caller in fixed ticks,
        # readiness, edits, or migration. This checks shipping source, not a model.
        self.assertEqual(source.count("prepare_authority_eviction("), 2)

    def test_pressure_eviction_keeps_the_stable_order_and_shared_cap(self):
        source = SOURCE.read_text(encoding="utf-8")
        prepare = function(source, "void SHIELD_WorldSystem::prepare_authority_eviction(")
        self.assertIn("std::sort(m_authorityEvictionCandidates.rbegin(), "
                      "m_authorityEvictionCandidates.rend());", prepare)
        room = function(source, "bool SHIELD_WorldSystem::make_authority_room(")
        self.assertIn("m_authorityEvictionCandidates.back()", room)
        self.assertIn("m_authorityEvictionCandidates.pop_back();", room)
        header = SOURCE.with_suffix(".h").read_text(encoding="utf-8")
        self.assertRegex(header, r"kActiveChunkBudget\s*=\s*8192")
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(encoding="utf-8")
        self.assertIn("EvictionPreparationRequiresAdmissionPressure", native)
        self.assertIn("authority_eviction_preparations()", native)


if __name__ == "__main__":
    unittest.main()
