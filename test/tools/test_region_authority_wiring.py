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
    def test_scheduler_fold_is_unsigned_fixed_width_and_outside_chunk_cache(self):
        source = (ROOT / "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp").read_text(encoding="utf-8")
        fold = function(source, "WorldAuthorityHashes BindWaterScheduler(")
        self.assertIn('std::string schedule("water_schedule:v1:");', fold)
        self.assertIn("for (unsigned shift = 0; shift < 64; shift += 8)", fold)
        self.assertIn("(raw_cursor >> shift) & 0xffu", fold)
        self.assertIn("const auto scheduler_hash = StableChecksum(schedule);", fold)
        self.assertIn('hashes.sub.water = StableChecksum("simulation_water_v5|grids:" + hashes.sub.water +', fold)
        self.assertIn('"|schedule:" + scheduler_hash', fold)
        self.assertIn('hashes.world = StableChecksum("simulation_authority_v5|base:" + hashes.world +', fold)
        self.assertIn('"|water:" + hashes.sub.water', fold)
        self.assertIn("hashes.water_scheduler_cursor = raw_cursor;", fold)
        self.assertNotIn("%", fold)
        for name in ("void AuthorityHashCache::update(", "WorldAuthorityHashes AuthorityHashCache::hashes()",
                     "void RegionContentHasher::append("):
            self.assertNotIn("cursor", function(source, name))
        world = SOURCE.read_text(encoding="utf-8")
        capture = function(world, "Persistence::WorldAuthorityHashes SHIELD_WorldSystem::compute_chunk_hashes()")
        self.assertEqual(capture.count("GetWaterSimWindowCursor()"), 1)
        self.assertIn("const auto cursor = static_cast<std::uint64_t>(GetWaterSimWindowCursor());", capture)
        self.assertIn("return Persistence::BindWaterScheduler(compute_legacy_chunk_hashes(), cursor);", capture)
        self.assertNotIn("water_cursor", capture)
        self.assertNotIn("for (", capture)
        legacy = function(world, "Persistence::WorldAuthorityHashes SHIELD_WorldSystem::compute_legacy_chunk_hashes()")
        self.assertIn("m_authorityHashCache->hashes()", legacy)
        self.assertIn("Persistence::ComputeWorldStreamingStateHash(legacy)", legacy)
        self.assertNotIn("BindWaterScheduler", legacy)

    def test_every_live_capture_uses_the_completed_session_value(self):
        server = (ROOT / "src/luminumbra_server/ServerWorldRunner.cpp").read_text(encoding="utf-8")
        for name in ("std::string ServerWorldRunner::ComputeWorldHash()",
                     "ServerWorldRunner::ComputeWorldSubHashes()",
                     "void ServerWorldRunner::ComputeWorldHashAndSubHashes("):
            capture = function(server, name)
            self.assertIn("const auto chunk_hashes = m_session->CaptureAuthorityHashes();", capture)
            self.assertLess(capture.index("wait_for_streaming_jobs()"), capture.index("CaptureAuthorityHashes()"))
            self.assertNotIn("compute_legacy_chunk_hashes", capture)
        combined = function(server, "void ServerWorldRunner::ComputeWorldHashAndSubHashes(")
        self.assertIn("*water_scheduler_cursor = chunk_hashes.water_scheduler_cursor;", combined)
        client = (ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp").read_text(encoding="utf-8")
        capture = function(client, "void NetSessionCaptureHashes(")
        self.assertIn("auto hashes = ctx->session->CaptureAuthorityHashes();", capture)
        self.assertIn("out.world_hash = hashes.world;", capture)
        self.assertIn("out.water = sub.water;", capture)
        session = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(encoding="utf-8")
        capture = function(session, "Persistence::WorldAuthorityHashes GameSession::CaptureAuthorityHashes()")
        self.assertIn("!IsSimulationTickBoundary()", capture)
        self.assertIn("m_simulationBatchInProgress", capture)
        batch = function(session, "std::uint32_t GameSession::TickSimulation(")
        self.assertIn("const BatchGuard guard(m_simulationBatchInProgress);", batch)
        self.assertIn("m_active = true;", batch)
        self.assertNotIn("active = enabled;", batch)
        self.assertIn("return m_worldSystem->compute_chunk_hashes();", capture)
        heavy = (ROOT / "src/luminumbra_server/modes/Heavy.cpp").read_text(encoding="utf-8")
        capture = function(heavy, "HeavyHashes CaptureHashes(")
        self.assertIn("runner.ComputeWorldHashAndSubHashes(h.world_hash, h.sub, &h.water_scheduler_cursor);", capture)
        self.assertNotIn("GetWaterSimWindowCursor", capture)
        comparison = (ROOT / "src/luminumbra_server/HeavyComparison.h").read_text(encoding="utf-8")
        self.assertIn("a.water_scheduler_cursor == b.water_scheduler_cursor", comparison)

    def test_cursor_load_paths_keep_persisted_state_and_preflight_metadata(self):
        source = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(encoding="utf-8")
        self.assertIn("m_worldSystem->SetWaterSimWindowCursor(saved.water_sim_cursor);", function(source, "bool GameSession::LoadWorld("))
        reload = function(source, "bool GameSession::LoadWorldStateFrom(")
        self.assertIn("&water_cursor", reload)
        self.assertIn("m_worldSystem->SetWaterSimWindowCursor(water_cursor);", reload)
        self.assertLess(reload.index("read_clock_metadata("), reload.index("clear_world("))
        self.assertLess(reload.index("SetWaterSimWindowCursor(water_cursor)"), reload.index("adopt_streamed_chunk("))
        self.assertNotIn("SetWaterSimWindowCursor(0)", reload)
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(encoding="utf-8")
        case = function(native, "NonzeroSchedulerRoundTripsBothLoadPathsAndChangesContinuation)")
        for assertion in ("fresh.LoadWorld(", "original.LoadWorldStateFrom(save)",
                          'metadata["waterSimCursor"] = 0u;', "EXPECT_GT(different, 0u);",
                          "fresh.CaptureAuthorityHashes().world, before.world"):
            self.assertIn(assertion, case)

    def test_profile_refusal_covers_both_directions_and_shared_replication_gate(self):
        source = (ROOT / "test/common/ReplicationAdmission_test.cpp").read_text(encoding="utf-8")
        case = function(source, "AuthorityProfilesRefuseBothDirectionsForEveryRoleBeforeForwarding)")
        for token in ("for (const bool regions : {false, true})", "for (const bool legacy_local : {false, true})",
                      "ReplicationAdmissionRole::Host", "ReplicationAdmissionRole::Joiner",
                      "session.LegacyRevision3SystemConfigIdentity()", "session.SystemConfigIdentity()",
                      "EXPECT_FALSE(state.admitted());", "EXPECT_TRUE(state.failed());"):
            self.assertIn(token, case)
        source = (ROOT / "test/net/runtime_tcp_hello_test.cpp").read_text(encoding="utf-8")
        for name in ("CurrentAuthorityRefusesLegacyPeerBeforeAdmission)",
                     "LegacyAuthorityRefusesCurrentPeerBeforeAdmission)"):
            case = function(source, name)
            self.assertIn("peer.SendFrame(EncodeHello(hello))", case)
            self.assertIn("EXPECT_FALSE(PollRuntimeTcpAccept", case)
            self.assertIn("ExpectReleased();", case)
        admission = (ROOT / "src/luminumbra_common/net/ReplicationAdmission.cpp").read_text(encoding="utf-8")
        case = function(admission, "ReplicationAdmissionState::OnFrame(")
        self.assertLess(case.index("peer.system_config_identity != m_local.system_config_identity"),
                        case.index("m_phase = ReplicationAdmissionPhase::Admitted"))
        self.assertIn('Fail("SystemConfig identity mismatch");', case)
        for path in ("Gns.cpp", "Steam.cpp"):
            backend = (ROOT / "src/luminumbra_server/modes" / path).read_text(encoding="utf-8")
            self.assertIn("AdmittedReplicationTransport", backend)
            self.assertNotIn("LegacyRevision3SystemConfigIdentity", backend)
        for path in ("NetTcp.cpp", "RuntimeTcpSlot.cpp"):
            backend = (ROOT / "src/luminumbra_server/modes" / path).read_text(encoding="utf-8")
            self.assertIn("HelloMatches", backend)
            self.assertNotIn("LegacyRevision3SystemConfigIdentity", backend)

    def test_snapshot_decoder_moves_terrain_samples_without_numeric_dom_arrays(self):
        source = (ROOT / "src/luminumbra_common/persistence/"
                  "WorldPersistenceRoundtrip.cpp").read_text(encoding="utf-8")
        # Select the numeric callback's branch, not the same guard in start_object.
        number = source[source.index("bool number(Number value)"):]
        direct = function(number, "if (m_samples)")
        self.assertIn("m_samples->push_back(static_cast<float>(value));", direct)
        self.assertNotIn("m_dom", direct)
        self.assertNotIn("std::function", source)
        self.assertIn("return true;", direct)
        fields = function(source, "static int terrain_field(")
        self.assertEqual(re.findall(r'key == "([^"]+)"', fields),
                         ["sdf_data", "heightmap_data"])
        start = function(source, "bool start_array(std::size_t elements)")
        self.assertIn("frame.kind == Kind::Chunk", start)
        self.assertIn("m_samples = &captured.emplace();", start)
        self.assertIn("return m_dom.start_array(elements);", start)
        apply = function(source, "void ApplyChunkJson(")
        for field in ("sdf", "heightmap"):
            self.assertIn(f"std::move(*terrain->{field})", apply)
        decoder = function(source, "bool WorldStreamingSnapshotDecoder::load(")
        self.assertIn("nlohmann::json::sax_parse(json_text, &sax)", decoder)
        self.assertIn("direct && chunks.is_array() && terrain.size() == chunks.size()", decoder)
        self.assertIn("ApplyChunkJson(chunk_json, *chunk, authority, samples)", decoder)
        self.assertIn("m_directTerrainSamples = direct_samples;", decoder)
        wrapper = function(source, "bool LoadWorldStreamingStateSnapshotJson(")
        self.assertIn("return decoder.load(json_text, out_state, errors, authority);", wrapper)

    def test_snapshot_fallback_checks_numeric_tokens_before_conversion(self):
        source = (ROOT / "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp").read_text(encoding="utf-8")
        samples = function(source, "std::vector<float> NumericSamplesFromJson(")
        self.assertRegex(samples, r'if \(!value.is_array\(\)\) throw')
        self.assertRegex(samples, r'if \(!sample.is_number\(\)\) throw')
        self.assertLess(samples.index("sample.is_number()"), samples.index("sample.get<float>()"))
        apply = function(source, "void ApplyChunkJson(")
        for field in ("sdf_data", "heightmap_data", "water_level_data", "water_sim_terrain_height"):
            self.assertIn('NumericSamplesFromJson(value.at("' + field + '"))', apply)
        sax = function(source, "bool scalar_allowed() const")
        self.assertIn("!m_samples", sax)
        self.assertNotIn("nlohmann::json(value)", function(source, "bool number(Number value)"))
        native = (ROOT / "test/persistence/persistence_hardening_test.cpp").read_text(encoding="utf-8")
        self.assertIn("TEST(PersistenceHardening, SnapshotNumericTypesMatchAcrossFastFallbackAndLegacyContainers)", native)

    def test_handshake_refusals_are_terminal_before_tick_or_replay_admission(self):
        source = (ROOT / "src/luminumbra_common/net/LockstepSession.cpp").read_text(encoding="utf-8")
        handshake = function(source, "bool LockstepSession::Handshake()")
        self.assertIn("const auto refuse = [this] { TransportDisconnected(); return false; };", handshake)
        for marker in ("if (peer.seed != mine.seed)", "if (peer.preset != mine.preset)",
                       "if (peer.tick_rate_hz != mine.tick_rate_hz)", "if (!HelloMatches(mine, peer))"):
            self.assertIn("return refuse();", function(source[source.index(marker):], marker))
        self.assertIn("!m_hello_sent && !m_transport->SendFrame", handshake)
        self.assertIn("return m_transport->IsPeerConnected() ? false : refuse();", handshake)
        pump = function(source, "TickResult LockstepSession::PumpTick(")
        pending = function(source[source.index("TickResult LockstepSession::PumpTick("):], "if (!m_handshaken)")
        self.assertIn("return terminal_result();", pending)
        self.assertLess(pump.index("if (!m_handshaken)"), pump.index("DrainPeerMessages()"))
        drain = function(source, "bool LockstepSession::DrainPeerMessages()")
        self.assertIn("if (!m_handshaken && m_disconnect_pending) continue;", drain)
        self.assertLess(drain.index("!m_handshaken && m_disconnect_pending"), drain.index("switch (type)"))

    def test_snapshot_fallback_releases_partial_buffers_and_keeps_validation(self):
        source = (ROOT / "src/luminumbra_common/persistence/"
                  "WorldPersistenceRoundtrip.cpp").read_text(encoding="utf-8")
        decoder = function(source, "bool WorldStreamingSnapshotDecoder::load(")
        fallback = function(source[source.index("bool WorldStreamingSnapshotDecoder::load("):],
                            "if (!direct)")
        for release in ("terrain.clear();", "snapshot = nullptr;"):
            self.assertLess(fallback.index(release),
                            fallback.index("snapshot = nlohmann::json::parse(json_text);"))
        self.assertTrue(decoder.startswith("m_directTerrainSamples = 0;"))
        for validation in ('snapshot.at("schema")', 'snapshot.at("order_contract")',
                           'snapshot.at("chunk_count")', "chunk->get_id() !=",
                           "!staged.insert_chunk(chunk)", "out_state.clear();"):
            self.assertIn(validation, decoder)
        self.assertLess(decoder.index('snapshot.at("chunk_count")'),
                        decoder.index("out_state.insert_chunk(chunk)"))
        self.assertIn("staged_chunks.push_back(std::move(chunk));", decoder)
        self.assertIn("for (const auto& chunk : staged_chunks) out_state.insert_chunk(chunk);", decoder)
        keys = function(source, "bool key(std::string& value)")
        self.assertIn("if (m_seenChunks) return false;", keys)
        self.assertIn("if (frame.seen[index]) return false;", keys)
        for callback, call in (("bool null()", "m_dom.null()"),
                               ("bool boolean(bool value)", "m_dom.boolean(value)"),
                               ("bool string(std::string& value)", "m_dom.string(value)")):
            self.assertIn("scalar_allowed() && " + call, function(source, callback))
        native = (ROOT / "test/persistence/persistence_hardening_test.cpp").read_text(
            encoding="utf-8")
        for case in ("SnapshotTerrainSamplesBypassNumericDomAndKeepCanonicalBytes",
                     "SnapshotDirectTerrainParsingKeepsNumericConversionsAndUnknownFields",
                     "SnapshotTerrainFallbackPreservesDuplicateKeysAndLegacyContainers",
                     "SnapshotTerrainParsingRefusesMalformedSamplesWithoutPartialState",
                     "SnapshotTerrainParsingHandlesEmptyWorldAndEmptyArrays"):
            self.assertIn("TEST(PersistenceHardening, " + case + ")", native)
        self.assertIn("EXPECT_EQ(decoder.direct_terrain_samples(), expected_samples);", native)

    def test_save_validation_consumers_keep_full_record_and_lattice_checks(self):
        source = (ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp").read_text(
            encoding="utf-8")
        decode = function(source, "std::shared_ptr<Chunk> DecodeChunkRecord(")
        self.assertIn("LoadWorldStreamingStateSnapshotJson(", decode)
        self.assertIn(".validate_record_authority = true", decode)
        loader = function(source, "bool WorldSaveService::load_world(")
        self.assertIn("for (const auto& entry : index.records)", loader)
        self.assertIn("DecodeChunkRecord(record, path, &errors, migrate_legacy_water)", loader)
        self.assertLess(loader.index("DecodeChunkRecord("),
                        loader.index("state.size() < max_resident_chunks"))
        self.assertIn("chunk->sdf_data.size() != lattice", loader)
        for entry in ("bool WorldSaveService::validate_save(",
                      "WorldSaveService::SaveBatch::begin(",
                      "bool WorldSaveService::has_world_save(",
                      "bool WorldSaveService::has_chunk_snapshot("):
            self.assertIn("load_world(", function(source, entry))
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("persistence/persistence_hardening_test.cpp", cmake)

    def test_meshing_captures_voxel_revision_with_owner_thread_snapshot(self):
        source = SOURCE.read_text(encoding="utf-8")
        dispatch = function(source, "SHIELD_WorldSystem::dispatch_meshing_jobs(")
        header = SOURCE.with_suffix(".h").read_text(encoding="utf-8")
        record = header.split("struct MeshingJobChunk {", 1)[1].split("};", 1)[0]
        self.assertIn("u32 voxel_revision = 0;", record)
        self.assertIn("chunk->authority_hash_revision(), chunk->voxel_revision()", dispatch)
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
        success = function(source, "if (mesh_ready && !mesh_failed && current_result)")
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
        self.assertLess(publish.index("if (mesh_ready && !mesh_failed && current_result)"), publish.index(guard))

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
                ("bool WriteFileDurably(",
                 "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool RewriteRegionFile(", "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool WriteWorldManifest(", "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool WorldSaveService::save_regions_validated(",
                 "BeforeActiveRegionsReplaceForTesting()")):
            with self.subTest(writer=name):
                implementation = source[source.index("namespace {\nbool WriteFileDurably"): ] if name == "bool WriteFileDurably(" else source
                writer = function(implementation, name)
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
