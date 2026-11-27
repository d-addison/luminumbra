"""Guard replay fixture symbol ownership without claiming native link coverage."""

import ast
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TEST_SOURCES = runpy.run_path(str(ROOT / "tools/ci/test_shared_test_sources.py"))
FIXTURE = "test/persistence/host_scheduler_fault_test.cpp"
MODES = "src/luminumbra_server/modes/"
ENTRY_POINTS = {
    "RunRecord": "RunSchedulerFaultRecord",
    "RunReplay": "RunSchedulerFaultReplay",
    "RunMutateReplayFixture": "RunSchedulerFaultMutateReplayFixture",
    "RunServer": "RunSchedulerFaultServer",
}


def without_comments(source):
    # Keep strings intact so commented-out directives cannot satisfy the check.
    return re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda match: "" if match[0].startswith(("//", "/*")) else match[0],
                  source, flags=re.DOTALL)


def read_source(path):
    return without_comments((ROOT / path).read_text(encoding="utf-8"))


def tracked_sources():
    paths = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-files", "-z", "--", "src", "test", "tools"])
    generated = {"__pycache__", "CMakeFiles", "build", ".cache"}
    extensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
                  ".inc", ".inl", ".ipp", ".py", ".cmake", ".sh", ".ps1"}
    for name in sorted(paths.split(b"\0")):
        if not name:
            continue
        path = Path(os.fsdecode(name))
        if not generated.intersection(path.parts) and (
                path.suffix in extensions or path.name == "CMakeLists.txt"):
            yield path


class NamedFnvBasisContract(unittest.TestCase):
    def test_basis_scan_ignores_generated_and_untracked_files(self):
        basis = b"146959810" b"3934665603"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ("src", "test", "tools/__pycache__", "tools/build"):
                (root / folder).mkdir(parents=True, exist_ok=True)
            subprocess.run(["git", "init", "--quiet", "--template=", str(root)], check=True,
                           capture_output=True)
            source = root / "src/owned.cpp"
            source.write_bytes(b"// allowed source\n")
            for name in ("tools/__pycache__/guard.pyc", "tools/__pycache__/generated.py",
                         "tools/build/generated.cpp", "tools/capture.bin"):
                (root / name).write_bytes(basis)
            subprocess.run(["git", "-C", str(root), "add", "--force", "src", "tools"],
                           check=True, capture_output=True)
            (root / "tools/untracked.py").write_bytes(basis)
            with mock.patch.dict(globals(), ROOT=root):
                self.assertEqual(list(tracked_sources()), [Path("src/owned.cpp")])
                self.test_named_digest_sites_do_not_reintroduce_the_truncated_literal()
                source.write_bytes(basis)
                with self.assertRaises(AssertionError):
                    self.test_named_digest_sites_do_not_reintroduce_the_truncated_literal()

    def test_named_digest_sites_do_not_reintroduce_the_truncated_literal(self):
        # Split the spelling so this guard does not itself introduce a forbidden hit.
        basis = b"146959810" b"3934665603"
        literal = re.compile(rb"(?<![0-9])" + basis + rb"(?![0-9])")
        allowed = {
            ("src/luminumbra_common/core/IntegerPrimitives.h",
             b"inline constexpr std::uint64_t kFnv1a64HistoricalBasis = " + basis + b"ull;"),
            ("test/common/IntegerPrimitives_test.cpp",
             b"EXPECT_EQ(Fnv1a64(nullptr, 0, kFnv1a64HistoricalBasis), " + basis + b"ull);"),
            ("tools/perf/validate_foliage_evidence.py",
             b"FNV1A64_HISTORICAL_BASIS = " + basis),
        }
        for path in tracked_sources():
            relative = path.as_posix()
            for number, line in enumerate((ROOT / path).read_bytes().splitlines(), 1):
                if literal.search(line):
                    self.assertIn((relative, line.strip()), allowed,
                                  f"{relative}:{number}: use the named historical basis")


class ReplaySourceContract(unittest.TestCase):
    def test_automatic_region_trace_uses_shared_encoder_and_pair_publication(self):
        stream = read_source("src/luminumbra_common/replay/ReplayStream.cpp")
        bounded = stream.split("bool BoundedReplayRecorder::Finalize(", 1)[1].split(
            "void BoundedReplayRecorder::Close()", 1)[0]
        self.assertIn("EncodeRegionScheduleTrace(trace, trace_bytes)", bounded)
        self.assertIn("out.commit_with_companion(publish_trace ? &trace_bytes : nullptr)", bounded)
        trace = read_source("src/luminumbra_common/replay/RegionScheduleTrace.cpp")
        writer = trace.split("bool ReplayWriter::Finalize(", 1)[1].split(
            "bool WriteRegionScheduleTrace(", 1)[0]
        self.assertIn("EncodeRegionScheduleTrace(*trace, bytes)", writer)
        for path, producer in (
            ("src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp",
             "NetSessionRegionScheduleDigest"),
            (MODES + "Lockstep.cpp", "LockstepRegionScheduleDigest"),
        ):
            source = read_source(path)
            self.assertIn("hooks_template.region_schedule_digest = &" + producer, source)
            self.assertIn("GetRegionSchedule().digest", source)
        lockstep = read_source("src/luminumbra_common/net/LockstepSession.cpp")
        capture = lockstep.split("void LockstepSession::RecordReplayTick(", 1)[1].split(
            "void LockstepSession::EmitDesyncDump(", 1)[0]
        self.assertIn("m_hooks.region_schedule_digest(m_hooks.user)", capture)
        self.assertIn("std::string_view(*digest)", capture)

    def test_ordinary_client_regional_playback_reaches_oracles(self):
        source = read_source("test/performance/runtime_world_visual_validation_test.cpp")
        regression = source.split(
            "TEST_P(ClientReplayProducerTest, AutomaticDumpMatchesCliBootAndReconstructsStartWorld)",
            1)[1].split("INSTANTIATE_TEST_SUITE_P", 1)[0]
        self.assertNotIn("if (!GetParam())", regression)
        self.assertIn("EXPECT_EQ(RunReplay(options), 1)", regression)
        self.assertIn('artifact.at("divergence_tick"), 1u', regression)
        self.assertIn('fs::remove(dump.string() + ".regions.json")', regression)
        self.assertIn('stale.at("divergence_section"), "region_schedule"', regression)
        self.assertIn("options.ticks = 1;", regression)
        self.assertIn("playback.RunFixedTicks(1)", regression)
        self.assertIn("trace.front().second, playback.Session()->GetRegionSchedule().digest",
                      regression)
        self.assertIn("EXPECT_EQ(trace, cli_trace)", regression)

    def assert_client_replicated_roster(self, source):
        begin = source.split("bool NetworkedSessionDriver::Begin(", 1)[1].split(
            "bool NetworkedSessionDriver::StepAgreedTick()", 1)[0]
        for session in ("m_impl->host_session", "client_session"):
            roster = session + "->SetReplicatedSimulationAnchors({});"
            self.assertIn(roster, begin)
            self.assertGreater(begin.index(roster), begin.rindex("return false;"))
            self.assertLess(begin.index(roster), begin.rindex("return true;"))

    def test_client_peers_match_runner_zero_avatar_replicated_roster(self):
        source = read_source("src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp")
        self.assert_client_replicated_roster(source)
        for session in ("m_impl->host_session", "client_session"):
            roster = session + "->SetReplicatedSimulationAnchors({});"
            with self.subTest(session=session), self.assertRaises(AssertionError):
                self.assert_client_replicated_roster(source.replace(roster, "", 1))

    # Pin mode-local admission and its ordering without tightening the
    # generic reader/writer, whose opaque and sparse records serve lockstep.
    def assert_mode_admission(self, source):
        recording, playback = source.split("int RunReplay(", 1)
        admission, startup = playback.split("auto config = ReplayRunnerConfigFrom", 1)
        self.assertRegex(admission, r'if \(header.engine_version != engine_now\)\s*\{'
                         r'\s*LUMINUMBRA_CORE_ERROR\("replay: engine mismatch:')
        self.assertIn("contents->inputs.size() != contents->tick_count", admission)
        self.assertIn("for (const auto& input : contents->inputs)", admission)
        self.assertIn("input.tick != admitted_inputs + 1 || !input.inputs.empty()", admission)
        self.assertIn("++admitted_inputs;", admission)
        self.assertIn("contents->tick_count != 0 && terminal == nullptr", admission)
        self.assertIn("missing authenticated terminal checkpoint", admission)
        self.assertIn("older positive recordings without one cannot be replayed", admission)
        self.assertNotIn("FindInput(", playback)
        self.assertRegex(recording, r'if \(executed != 0 && '
                         r'\(executed % kCheckpointIntervalTicks\) != 0\)\s*\{\s*'
                         r'const auto checkpoint = CaptureCheckpoint\(executed, runner\);\s*'
                         r'if \(!checkpoint\)\s*\{\s*return 1;\s*\}\s*'
                         r'writer.RecordCheckpoint\(\*checkpoint\);\s*\}\s*'
                         r'const auto final_hash = CaptureWorldHash\(executed, runner\);\s*'
                         r'if \(!final_hash\)\s*\{\s*return 1;\s*\}\s*'
                         r'const auto\* trace = [^;]*;\s*'
                         r'Luminumbra::Replay::ReplayFinalization publication\(\s*'
                         r'options.record_path, \"[^\"]*\"\);\s*'
                         r'if \(!publication.ready\(\) \|\| !writer.Finalize\(executed, trace\)'
                         r' \|\| !publication.Complete\(\)\)')
        self.assertIn("(executed % kCheckpointIntervalTicks) == 0 || "
                      "executed == contents->tick_count", startup)
        for field in ("world_hash", "terrain", "water", "entities"):
            self.assertIn(f"live->{field} != recorded->{field}", startup)
        self.assertIn("terminal ? terminal->world_hash : header.start_world_hash", startup)
        tick_loop = startup.split("while (!diverged", 1)[1].split("const std::string live_end", 1)[0]
        self.assertNotIn("FindCheckpoint(", tick_loop)
        self.assertIn("++checkpoint;", tick_loop)

    def test_replay_mode_admits_identity_inputs_and_terminal_before_startup(self):
        self.assert_mode_admission(read_source(MODES + "Replay.cpp"))

    def assert_publication_admission(self, source):
        recording = source.split("int RunRecord(", 1)[1].split("int RunReplay(", 1)[0]
        preflight = recording.split("RunnerConfigFrom(options)", 1)[0]
        self.assertIn("ReplayReadLimits{}.records", preflight)
        self.assertIn("options.ticks / kCheckpointIntervalTicks +", preflight)
        self.assertIn("(options.ticks % kCheckpointIntervalTicks != 0 ? 1u : 0u)", preflight)
        self.assertIn("options.ticks > record_limit || checkpoints > record_limit - options.ticks",
                      preflight)
        self.assertRegex(preflight, r'requested workload exceeds replay admission budget"\);'
                         r'\s*return 1;')
        playback = source.split("int RunReplay(", 1)[1]
        before_read = playback.split("Luminumbra::Replay::ReadReplay(", 1)[0]
        self.assertRegex(before_read,
                         r'if \(ReplayFinalizationIncomplete\(options.replay_path\)\)\s*\{'
                         r'\s*LUMINUMBRA_CORE_ERROR\("replay: recording finalization is incomplete"\);'
                         r'\s*return 1;\s*\}')

    def test_recording_budget_and_incomplete_marker_precede_startup_and_read(self):
        self.assert_publication_admission(read_source(MODES + "Replay.cpp"))

    def test_publication_admission_contract_detects_isolated_regressions(self):
        source = read_source(MODES + "Replay.cpp")
        for token in ("ReplayReadLimits{}.records",
                      "(options.ticks % kCheckpointIntervalTicks != 0 ? 1u : 0u)",
                      "options.ticks > record_limit || checkpoints > record_limit - options.ticks",
                      "ReplayFinalizationIncomplete(options.replay_path)"):
            self.assertIn(token, source)
            with self.subTest(token=token), self.assertRaises(AssertionError):
                self.assert_publication_admission(source.replace(token, "false", 1))

    def assert_client_capture(self, driver):
        for peer, session in (("host", "*m_impl->host_session"), ("client", "*client_session")):
            self.assertRegex(driver, peer + r"_cfg.replay_header = std::make_shared<const\s*"
                             r"Luminumbra::Replay::ReplayHeader>\(\s*"
                             r"Luminumbra::Replay::CaptureReplayHeader\(\s*" + re.escape(session) +
                             r",\s*static_cast<std::uint32_t>\(config.surface_radius\),\s*"
                             r"static_cast<std::uint32_t>\(config.collision_radius\)\)\);")
        self.assertIn("hooks_template.capture_hashes = &NetSessionCaptureAuthorityHashes;", driver)
        capture = driver.split("void NetSessionCaptureAuthorityHashes(", 1)[1].split("} // namespace", 1)[0]
        self.assertIn("AuthorityCaptureParts::All", capture)
        self.assertLess(capture.index("if (!result.capture)"), capture.index("out.tick"))
        self.assertIn("out.world_hash = result.capture->composite;", capture)
        self.assertNotIn("snapshot_streamed_chunks", capture)

    def test_all_client_boot_routes_share_bounded_preparation_before_capture(self):
        main = read_source("src/luminumbra_client/main_client.cpp")
        startup = main.split("auto finish_world_entry =", 1)[1].split(
            "auto start_world_creation =", 1)[0]
        staged = main.split("auto prepare_entry_horizon =", 1)[1].split(
            "auto drain_entry =", 1)[0]
        for entry in (startup, staged):
            self.assertIn("gameSession->PrepareInitialSimulation(", entry)
            self.assertNotIn("EnsureSurfaceReadyNear(", entry)
        self.assertIn("entry_stop.get_token()", staged)
        driver = read_source("src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp")
        self.assertLess(driver.index("host_session->PrepareInitialSimulation("),
                        driver.index("Luminumbra::Replay::CaptureReplayHeader("))
        runner = read_source("src/luminumbra_server/ServerWorldRunner.cpp")
        self.assertRegex(runner, r"m_session->SettleInitialSimulation\(\s*boot_anchors,\s*"
                         r"settle_stats\)")
        session = read_source("src/luminumbra_common/world/GameSession.cpp")
        entry = session.split("bool GameSession::PrepareInitialSimulation(", 1)[1].split(
            "bool GameSession::SettleInitialSimulation(", 1)[0]
        self.assertLess(entry.index("SetWaterBootPaused(true)"),
                        entry.index("EnsureSurfaceReadyNear("))
        self.assertLess(entry.index("SettleInitialSimulation("),
                        entry.index("sync_water_authority()"))
        self.assertIn("SettleInitialSimulation({}, stats, stop)", entry)
        settle = session.split("bool GameSession::SettleInitialSimulation(", 1)[1].split(
            "struct GameSession::LoadedWorldState", 1)[0]
        self.assertIn("i < 400 && stable < 8", settle)
        self.assertIn("kInitSettleCap = 50", settle)
        self.assertIn("kPostInitSettleTicks = 240", settle)
        self.assertIn("TryCompleteBootWaterSettle(m_registry, settle_anchors, phys, skipped)", settle)
        self.assertIn('"chunks total, {} settle passes{}"', settle)
        # Regional boots report canonical participants, including parked columns;
        # legacy and loaded boots retain the streamed snapshot.
        self.assertIn("const auto chunks = water_snapshot();", settle)
        self.assertRegex(settle, r"chunks\.size\(\),\s*settle_iters,")
        self.assertIn("if (!regional_boot || water_loaded_from_save)", settle)
        self.assertIn("return ws->snapshot_streamed_chunks();", settle)
        self.assertIn("ws->inspect_water_column(Chunk::decode_id(id))", settle)
        self.assertIn("ActiveRegionLedger boot_ledger;", settle)
        self.assertIn("WorldClock(++boot_tick)", settle)
        self.assertIn("m_waterSystem->UpdateRegionalFromSimulation(", settle)
        self.assertIn("regional_water->ResetRegionalTimeline();", settle)
        self.assertIn("!continuation && !water_loaded_from_save", settle)
        self.assertIn("for (int i = 0; !continuation && i < 400", settle)
        self.assertNotIn("settle_streaming", session)
        self.assertNotIn("TickSimulation(", settle)
        self.assertGreaterEqual(settle.count("stop.stop_requested()"), 5)
        native = read_source("test/performance/runtime_world_visual_validation_test.cpp")
        regression = native.split("TEST_P(ClientReplayProducerTest,", 1)[1].split(
            "INSTANTIATE_TEST_SUITE_P(AuthorityBackends", 1)[0]
        self.assertIn("client.PrepareInitialSimulation(", regression)
        self.assertNotIn("ServerWorldRunner client", regression)
        self.assertIn("EXPECT_EQ(start_hash, cli->header.start_world_hash)", regression)
        self.assertIn("EXPECT_EQ(automatic->header.start_world_hash, start_hash)", regression)
        self.assertIn("playback.ComputeWorldHash(), automatic->header.start_world_hash", regression)

    def test_boot_presentation_observes_publication_and_terrain_save_dependencies(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        drain = source.split("void SHIELD_WorldSystem::wait_for_streaming_jobs()", 1)[1].split(
            "namespace {", 1)[0]
        self.assertLess(drain.index("while (deferred_meshing_pending())"),
                        drain.index("m_streaming_telemetry_stats.last_queue_depth"))
        self.assertIn("m_deferred_generation_candidates + m_unadmitted_meshing_candidates", drain)
        self.assertIn("m_last_streaming_budget_stats.submitted_meshing -", source)
        self.assertIn("retained_candidates;", source)
        self.assertIn("m_streaming_telemetry_stats.pending_lod_chunks = 0", drain)
        self.assertNotIn("m_streaming_state.chunks", drain)
        capture = source.split("SHIELD_WorldSystem::capture_far_lod_sdf_snapshot(", 1)[1].split(
            "bool SHIELD_WorldSystem::is_far_lod_sdf_snapshot_current(", 1)[0]
        self.assertIn("entry.authority_durable = !chunk->is_voxel_data_dirty();", capture)
        self.assertIn("if (!chunk->has_terrain_authority()) {\n"
                      "            entry.authority_durable = true;\n        }", capture)
        native = read_source("test/performance/runtime_world_visual_validation_test.cpp")
        for name in ("RadiusZeroSettlesStreamingAndWaterAtTickZero",
                     "DrainedStreamingAndWaterOnlyTerrainArePresentationReady",
                     "StreamingDrainRetainsUnadmittedCandidates"):
            self.assertIn("TEST(ClientBootPreparation, " + name + ")", native)

    def test_client_replay_headers_share_authority_capture_for_both_peers(self):
        driver = read_source("src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp")
        self.assert_client_capture(driver)
        runner = read_source("src/luminumbra_server/ServerWorldRunner.cpp")
        capture = runner.split("ServerWorldRunner::CaptureReplayHeader() const", 1)[1].split(
            "ServerWorldRunner::ServerWorldRunner(", 1)[0]
        self.assertIn("auto header = Replay::CaptureReplayHeader(*m_session,", capture)
        # Server headers keep the requested seed text; empty means a random boot.
        self.assertIn("header.seed_string = m_config.seed;", capture)
        self.assertNotIn("header.authority_boot =", capture)

    def test_client_capture_contract_detects_isolated_regressions(self):
        driver = read_source("src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp")
        for token in ("host_cfg.replay_header", "client_cfg.replay_header",
                      "hooks_template.capture_hashes = &NetSessionCaptureAuthorityHashes;",
                      "out.world_hash = result.capture->composite;"):
            self.assertIn(token, driver)
            with self.subTest(token=token), self.assertRaises(AssertionError):
                self.assert_client_capture(driver.replace(token, "removed", 1))

    def test_mode_admission_contract_detects_isolated_regressions(self):
        source = read_source(MODES + "Replay.cpp")
        for token in ("header.engine_version != engine_now",
                      "contents->inputs.size() != contents->tick_count",
                      "input.tick != admitted_inputs + 1 || !input.inputs.empty()",
                      "contents->tick_count != 0 && terminal == nullptr",
                      "executed != 0 && (executed % kCheckpointIntervalTicks) != 0",
                      " || executed == contents->tick_count",
                      "terminal ? terminal->world_hash : header.start_world_hash",
                      "!publication.ready()", "!publication.Complete()"):
            self.assertIn(token, source)
            with self.subTest(token=token), self.assertRaises(AssertionError):
                self.assert_mode_admission(source.replace(token, "false", 1))

    def test_mode_admission_regressions_use_registered_shipping_entry_points(self):
        path = "common/ReplayModeAdmission_test.cpp"
        cmake = read_source("test/CMakeLists.txt")
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)', cmake, re.DOTALL)
        self.assertIsNotNone(target)
        self.assertEqual(target[1].split().count(path), 1)
        native = read_source("test/" + path)
        shipping_cases = native.split("#undef ServerWorldRunner\n", 1)[-1]
        self.assertNotIn("#define Run", shipping_cases)
        self.assertRegex(native, r'ASSERT_EQ\(RunRecord\(options\), 0\)')
        self.assertRegex(native, r'ASSERT_EQ\(RunReplay\(options\), 0\)')
        self.assertIn("testing::Values(std::uint64_t{0}, 1, 2, 29, 30, 31, 60, 90)", native)
        for case in ("AuthenticatesEveryExecutedTickAndPreservesCadenceBytes",
                     "ForeignEngineIsReadableButRefusedBeforeConfiguration",
                     "IsolatedInputMutationsAreReadableButRefusedBeforeConfiguration",
                     "ZeroTicksUseStartHashAndRefuseExtraInputs",
                     "MissingTerminalIsExplicitlyRefusedBeforeConfiguration",
                     "EveryTerminalHashIsVerifiedAndLocalized",
                     "MissingCadenceCheckpointStillFails",
                     "UnreadableAndPartialStreamsRefuseBeforeConfiguration"):
            self.assertRegex(native, rf'TEST_[FP]\(\w+,\s*{case}\)')

    def assert_fixture_ownership(self, fixture):
        includes = list(re.finditer(
            r'^#include "../../src/luminumbra_server/modes/(Replay|Server)\.cpp"$',
            fixture, re.MULTILINE))
        self.assertEqual([match[1] for match in includes], ["Replay", "Server"])
        first, last = includes[0].start(), includes[-1].end()
        prefix, suffix = fixture[:first], fixture[last:]
        self.assertIn('#include "luminumbra_server/modes/Modes.h"', prefix)
        # Discover every external mode definition, including the fixture mutator
        # even though the fault cases do not call it. All need distinct symbols.
        exported = set()
        for mode in ("Replay", "Server"):
            exported.update(re.findall(
                r'^int (Run\w+)\(const ServerCliOptions& options\)\s*\{',
                read_source(MODES + mode + ".cpp"), re.MULTILINE))
        self.assertEqual(exported, set(ENTRY_POINTS))
        self.assertEqual(len(set(ENTRY_POINTS.values())), len(exported))
        substitutions = {"ServerWorldRunner": "SchedulerFaultTestRunner", **ENTRY_POINTS}
        for original, adapted in substitutions.items():
            define = f"#define {original} {adapted}"
            undef = f"#undef {original}"
            self.assertEqual(fixture.count(define + "\n"), 1)
            self.assertEqual(fixture.count(undef + "\n"), 1)
            self.assertIn(define + "\n", prefix)
            self.assertIn(undef + "\n", suffix)
            # No intervening condition, undef or replacement may leave one
            # include with shipping names, or leak a macro into the test cases.
            scope = fixture[prefix.index(define):suffix.index(undef) + last]
            directives = re.findall(r'^#(\w+).*$', scope, re.MULTILINE)
            self.assertTrue(all(word in ("define", "include", "undef")
                                for word in directives))
            self.assertNotIn(undef, fixture[:last])
            self.assertLess(suffix.index(undef), suffix.index("TEST_F("))
        for original in ENTRY_POINTS:
            self.assertNotRegex(suffix, rf'\b{original}\s*\(')
        self.assertEqual(len(re.findall(r'\bRunSchedulerFaultRecord\(options\)', suffix)), 3)
        self.assertEqual(len(re.findall(r'\bRunSchedulerFaultServer\(options\)', suffix)), 1)

    def test_fixture_compiles_real_modes_with_distinct_entry_points(self):
        self.assert_fixture_ownership(read_source(FIXTURE))

    def test_every_missing_or_ineffective_substitution_is_detected(self):
        fixture = read_source(FIXTURE)
        for original, adapted in ENTRY_POINTS.items():
            define = f"#define {original} {adapted}\n"
            undef = f"#undef {original}\n"
            for mutation, changed in enumerate((
                fixture.replace(define, ""),
                fixture.replace(define, f"#define {original} {original}\n"),
                fixture.replace(undef, ""),
                fixture.replace(define, "").replace(undef, define + undef),
            )):
                with self.subTest(symbol=original, mutation=mutation), self.assertRaises(AssertionError):
                    self.assert_fixture_ownership(changed)

    def test_fault_cases_cannot_silently_call_unadapted_modes(self):
        fixture = read_source(FIXTURE)
        for original in ("RunRecord", "RunServer"):
            with self.subTest(symbol=original), self.assertRaises(AssertionError):
                self.assert_fixture_ownership(fixture.replace(
                    ENTRY_POINTS[original] + "(options)", original + "(options)", 1))

    def assert_shipping_owner(self, cmake, identity):
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)',
                           cmake, re.DOTALL)
        self.assertIsNotNone(target)
        for source in ("persistence/host_scheduler_fault_test.cpp",
                       "common/EffectivePresetIdentity_test.cpp",
                       "../" + MODES + "Replay.cpp"):
            self.assertEqual(target[1].split().count(source), 1)
        # The identity roundtrip must keep using the unadapted production bodies.
        self.assertRegex(identity, r'ASSERT_EQ\(RunRecord\(options\), 0\)')
        self.assertRegex(identity, r'ASSERT_EQ\(RunReplay\(options\), 0\)')
        self.assertNotIn("#define Run", identity)
        shipping = read_source("src/luminumbra_server/sources.cmake")
        for mode in ("Replay", "Server"):
            self.assertEqual(shipping.count(f'/modes/{mode}.cpp"'), 1)

    def test_shipping_replay_and_identity_roundtrip_keep_their_owner(self):
        self.assert_shipping_owner(read_source("test/CMakeLists.txt"),
                                   read_source("test/common/EffectivePresetIdentity_test.cpp"))

    def test_headless_session_test_has_source_specific_windows_section_capacity(self):
        cmake = read_source("test/CMakeLists.txt")
        self.assertRegex(cmake, r'if\(MSVC\)\s+set_source_files_properties\('
                         r'common/GameSessionHeadlessWorld_test\.cpp PROPERTIES\s+'
                         r'COMPILE_OPTIONS "/bigobj"\)\s+elseif\(MINGW\)\s+'
                         r'set_source_files_properties\('
                         r'common/GameSessionHeadlessWorld_test\.cpp PROPERTIES\s+'
                         r'COMPILE_OPTIONS "-Wa,-mbig-obj"\)\s+endif\(\)')

    def test_removing_shipping_replay_coverage_is_detected(self):
        cmake = read_source("test/CMakeLists.txt")
        identity = read_source("test/common/EffectivePresetIdentity_test.cpp")
        with self.assertRaises(AssertionError):
            self.assert_shipping_owner(cmake.replace("../" + MODES + "Replay.cpp", ""), identity)
        with self.assertRaises(AssertionError):
            self.assert_shipping_owner(cmake, identity.replace("RunReplay(options)", "FakeReplay(options)"))

    def test_replay_admission_fixture_distinguishes_supported_and_unknown_versions(self):
        identity = read_source("test/common/EffectivePresetIdentity_test.cpp")
        refusal = identity.split(
            "ReplayRefusesInvalidOrMismatchedIdentityBeforeCreatingDirectories) {", 1)[1].split(
                "\nTEST_F(", 1)[0]
        self.assertIn("for (const bool custom : {false, true})", refusal)
        self.assertIn("constexpr auto unsupported_version = "
                      "(std::numeric_limits<std::uint16_t>::max)();", refusal)
        self.assertRegex(refusal, r'static_assert\(unsupported_version > '
                         r'Luminumbra::Replay::kLrec1Version &&\s*unsupported_version > '
                         r'Luminumbra::Replay::kLrec1ElidedVersion\);')
        self.assertIn("header.version = unsupported_version;\n"
                      "        EXPECT_FALSE(ReplayRunnerConfigFrom(options, header).has_value());",
                      refusal)
        self.assertNotIn("++header.version", refusal)
        self.assertRegex(refusal, r'for \(const auto version :\s*'
                         r'\{Luminumbra::Replay::kLrec1Version, '
                         r'Luminumbra::Replay::kLrec1ElidedVersion\}\) \{\s*'
                         r'SCOPED_TRACE\(version\);\s*header = valid;\s*'
                         r'header.version = version;\s*'
                         r'const auto admitted = ReplayRunnerConfigFrom\(options, header\);\s*'
                         r'ASSERT_TRUE\(admitted.has_value\(\)\);')
        self.assertIn("EXPECT_EQ(admitted->required_preset_content_hash, "
                      "resolved.preset_content_hash);", refusal)
        self.assertRegex(refusal, r'EXPECT_TRUE\(Luminumbra::Replay::ConfigurationMatches\(\s*'
                         r'header, retained.SystemConfigIdentity\(\), '
                         r'admitted->required_preset_content_hash\)\);')

    def test_contract_is_registered_in_ctest_and_both_python_platform_plans(self):
        path = "tools/ci/test_replay_source_contract.py"
        self.assertIn('add_test(NAME ReplaySourceContract COMMAND "${Python3_EXECUTABLE}" -I -B\n'
                      f'    ${{CMAKE_SOURCE_DIR}}/{path})', read_source("test/CMakeLists.txt"))
        plan = ast.parse(read_source("tools/ci/ci_paths.py"))
        tests = next(ast.literal_eval(node.value) for node in plan.body
                     if isinstance(node, ast.Assign)
                     and any(isinstance(name, ast.Name) and name.id == "PYTHON_TESTS"
                             for name in node.targets))
        self.assertEqual(tests.count(path), 1)
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("tools/ci/ci_paths.py python-tests", workflow)

    def test_replay_calendar_admission_and_transient_snapshots_share_creation_values(self):
        helper = read_source(MODES + "ModeHelpers.cpp")
        for body in (helper.split("RuntimeHelloFrom(", 1)[1].split("\nLuminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(", 1)[0],
                     helper.split("ReplayRunnerConfigFrom(", 1)[1]):
            # A session that has not created a world still has legacy presentation
            # defaults when active regions are off; admission needs creation values.
            self.assertRegex(body, r'SystemConfigIdentity\(Luminumbra::world::WorldCalendar\{\}\)')
        creation = read_source("src/luminumbra_common/world/GameSession.cpp").split(
            "GameSession::PrepareWorldCreation(", 1)[1].split(
                "bool GameSession::LoadWorld(", 1)[0]
        self.assertIn("std::string saved_preset_json;", creation)
        self.assertRegex(creation, r'BoundedJsonDump\(\s*resolved,\s*'
                         r'Persistence::SavedInputLimits::kPresetBytes - 1,\s*saved_preset_json,\s*-1,\s*false\)')
        self.assertLess(creation.index("BoundedJsonDump("),
                        creation.index("fs::create_directories(worldPath.parent_path())"))
        self.assertLess(creation.index("requiredPresetContentHash"),
                        creation.index("fs::create_directories(worldPath.parent_path())"))
        self.assertLess(creation.index("LoadTerrainPresetFromJson(resolved"),
                        creation.index("ResetWorldSystems();"))
        self.assertIn("m_savedPresetJson = std::move(saved_preset_json);", creation)
        native = read_source("test/common/EffectivePresetIdentity_test.cpp")
        for name in ("ReplayAdmissionUsesTheCreatedCalendarWithActiveRegionsOnAndOff",
                     "TransientSnapshotsRetainTheEffectivePresetAndRenderCalendar"):
            self.assertIn(f"TEST_F(EffectivePresetIdentity, {name})", native)

    def test_voxel_edit_preparation_indexes_and_publishes_synchronous_generation(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        prepare = source.split("bool SHIELD_WorldSystem::PrepareTerrainVoxelEdit(", 1)[1].split(
            "int SHIELD_WorldSystem::EditTerrainVoxel(", 1)[0]
        insertion = prepare.split("if (iterator == m_streaming_state.chunks.end()) {", 1)[1].split(
            "auto& chunk = iterator->second;", 1)[0]
        self.assertIn("m_activation_index.insert(iterator->second);", insertion)
        self.assertIn("++m_dirty_generation;", insertion)
        generation = prepare.split("if (chunk->sdf_data.empty()) {", 1)[1].split(
            "if (chunk->sdf_data.size() != kFullSdfLattice)", 1)[0]
        self.assertRegex(generation, r'bump_far_lod_authority_revision\(chunk->get_coords\(\)\);\s*'
                         r'GenerateChunkData\(\*chunk, 1\);\s*'
                         r'publish_generated_column_span\(\*chunk\);\s*'
                         r'\+\+m_dirty_generation;')
        self.assertLess(prepare.index("return false;"), prepare.index("wait_for_streaming_jobs();"))
        native = read_source("test/common/ActivationResidencyIndex_test.cpp")
        self.assertIn("VoxelEditPreparationMaintainsResidencyAndPublication", native)
        for token in ("EXPECT_FALSE(world.PrepareTerrainVoxelEdit(anchor, 0.0f));",
                      "EXPECT_FALSE(world.PrepareTerrainVoxelEdit(anchor, 17.0f));",
                      "EXPECT_EQ(loaded->sdf_data, partial_sdf);",
                      "EXPECT_EQ(prepared.index_updates, initial.index_updates + 2u);",
                      "const auto missing = FullScan(world, {anchor}, 1);",
                      "ASSERT_EQ(missing.size(), 14u);",
                      "EXPECT_TRUE(FullScan(world, {anchor}, 1).empty());",
                      "world.get_last_streaming_budget_stats().generation_candidates, missing.size()"):
            self.assertIn(token, native)
        self.assertIn("common/ActivationResidencyIndex_test.cpp", read_source("test/CMakeLists.txt"))
        self.assertNotRegex(native, r'(?<![\w:])Test::SyntheticTerrainParams\(')

    def test_surface_span_cache_preserves_revision_and_owner_publication(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        sampler = source.split("SHIELD_WorldSystem::compute_column_surface_span(", 1)[1].split(
            "SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::column_surface_span_from_heights(", 1)[0]
        self.assertRegex(sampler, r'ComputeShapedHeightsAtPositionsImpl<true>\('
                         r'[^;]*m_params.surface_sampling_revision >=\s*kSurfaceSamplingRevision\);')
        self.assertIn("return column_surface_span_from_heights(chunk_x, chunk_z, heights);", sampler)
        publication = source.split("void SHIELD_WorldSystem::publish_generated_column_span(", 1)[1].split(
            "SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::column_surface_span(", 1)[0]
        self.assertRegex(publication, r'if \(m_params.surface_sampling_revision < kSurfaceSamplingRevision &&\s*'
                         r'\(m_params.biome_relief_enabled \|\| m_params.cliffs_enabled \|\| m_params.lakes_enabled \|\|\s*'
                         r'm_params.surface_breaks_enabled\)\)\s*return;')
        adoption = source.split("bool SHIELD_WorldSystem::adopt_streamed_chunk(", 1)[1]
        for token in ("chunk->stage_water_seed_inputs({});", "m_activation_index.insert(chunk);",
                      "m_activation_index.content_changed(chunk->get_coords());"):
            self.assertIn(token, adoption)
        native = read_source("test/common/ActivationResidencyIndex_test.cpp")
        for token in ("params.surface_sampling_revision < Systems::kSurfaceSamplingRevision",
                      "legacy_params.cliffs_enabled = false;",
                      "GeneratedCliffHeightmapsPreserveFullScanActivationPolicy",
                      "CurrentRevisionPublishesCompleteGeneratedCliffSpans"):
            self.assertIn(token, native)

    def test_surface_admission_precedes_boot_output_and_allocation(self):
        helper = read_source(MODES + "ModeHelpers.cpp").split("ReplayRunnerConfigFrom(", 1)[1]
        guard = "SurfaceRadiiSupported(header.surface_radius, header.collision_radius)"
        self.assertIn(guard, helper)
        self.assertRegex(helper, r'if \(!Luminumbra::World::SurfaceRadiiSupported\('
                         r'header.surface_radius, header.collision_radius\)\)\s*return std::nullopt;')
        self.assertLess(helper.index(guard), helper.index("ServerWorldRunnerConfig config;"))
        runner = read_source("src/luminumbra_server/ServerWorldRunner.cpp").split(
            "bool ServerWorldRunner::Boot()", 1)[1]
        self.assertRegex(runner, r'if \(!World::SurfaceRadiiSupported\('
                         r'm_config.surface_radius, m_config.collision_radius\)\)\s*\{'
                         r'[^}]*return false;')
        self.assertLess(runner.index("SurfaceRadiiSupported"), runner.index("m_jobSystem.startup"))
        stream = read_source("src/luminumbra_common/replay/ReplayStream.cpp")
        decode = stream.split("if (!c.GetU32(h.collision_radius))", 1)[1]
        self.assertRegex(decode, r'SurfaceRadiiSupported\(h.surface_radius, h.collision_radius\)\)'
                         r'\s*return false;')
        self.assertLess(decode.index("SurfaceRadiiSupported"), decode.index("c.GetString"))
        writer = stream.split("bool ReplayWriter::Open(", 1)[1]
        self.assertLess(writer.index("SurfaceRadiiSupported"), writer.index("m_path = path"))
        self.assertRegex(writer, r'SurfaceRadiiSupported\(header.surface_radius, '
                         r'header.collision_radius\)\)\s*return false;')
        primitive = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp").split(
            "bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(", 1)[1].split(
                "\nfloat SHIELD_WorldSystem::GetTerrainHeightAt(", 1)[0]
        self.assertIn("World::SurfaceColumnCount(static_cast<std::uint64_t>(radius))", primitive)
        self.assertRegex(primitive, r'if \(!physics_system \|\| !column_count \|\|\s*'
                         r'!World::SurfaceRadiiSupported\(radius, collision_range\)\)\s*\{'
                         r'\s*return false;')
        self.assertLess(primitive.index("SurfaceRadiiSupported"),
                        primitive.index("wait_for_generation_jobs"))
        self.assertRegex(primitive, r'if \(!World::AddSurfaceColumnChunks\('
                         r'span.min_y, span.max_y, surface_capacity\)\)\s*return false;')
        self.assertLess(primitive.index("AddSurfaceColumnChunks"),
                        primitive.index("chunks_to_build.reserve(surface_capacity)"))
        self.assertLess(primitive.index("AddSurfaceColumnChunks"),
                        primitive.index("m_streaming_state.chunks.emplace"))
        self.assertIn("const ColumnSurfaceSpan& span = surface_spans[surface_index++];", primitive)
        self.assertNotIn("(radius * 2 + 1) * (radius * 2 + 1)", primitive)
        for vector in ("chunks_to_build", "chunks_to_consider_for_collision"):
            self.assertIn(f"surface_capacity > {vector}.max_size()", primitive)
        edit = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp").split(
            "int SHIELD_WorldSystem::EditTerrainVoxel(", 1)[1]
        self.assertIn("(World::kMaxSurfaceRadius - 2) * CHUNK_SIZE_X", edit)
        self.assertRegex(edit, r'if \(!std::isfinite\(radius_m\) \|\| radius_m <= 0.0f \|\| '
                         r'radius_m > max_edit_radius\)\s*return 0;')
        self.assertLess(edit.index("std::isfinite(radius_m)"), edit.index("world_to_chunk_coords"))
        self.assertRegex(edit, r'if \(!World::AddSurfaceColumnChunks\('
                         r'span.min_y, span.max_y, remesh_chunks\)\)\s*return 0;')
        self.assertLess(edit.index("AddSurfaceColumnChunks"), edit.index("chunk->sdf_data[idx] ="))
        budget = read_source("src/luminumbra_common/world/SurfaceWorkBudget.h")
        for check in ("kMaxSurfaceRadius = 12", "kMaxSurfaceColumns = 625",
                      "kMaxSurfaceChunks = 8192", "radius > (max_size - 1) / 2",
                      "diameter > max_size / diameter",
                      "count > kMaxSurfaceChunks - total"):
            self.assertIn(check, budget)

    def test_surface_exact_admission_precedes_all_publication_drains(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        primitive = source.split("bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(", 1)[1].split(
            "\nfloat SHIELD_WorldSystem::GetTerrainHeightAt(", 1)[0]
        # The real function must finish every admission check before any drain;
        # searching the rest of the translation unit could accept a later call.
        admissions = [re.search(
            r'if \(!World::AddSurfaceColumnChunks\('
            r'span.min_y, span.max_y, surface_capacity\)\)\s*return false;', primitive)]
        for vector in ("chunks_to_build", "chunks_to_consider_for_collision"):
            admissions.append(re.search(
                rf'if \(surface_capacity > {vector}\.max_size\(\)\)\s*return false;',
                primitive))
        for admission in admissions:
            self.assertIsNotNone(admission)
        drains = list(re.finditer(r'wait_for_(?:generation|meshing|promotion|streaming)_jobs\(\);',
                                  primitive))
        self.assertEqual([drain[0] for drain in drains], [
            "wait_for_generation_jobs();", "wait_for_meshing_jobs();",
            "wait_for_streaming_jobs();"])
        # The full streaming barrier also settles deferred work. Pin both the
        # direct lane drains and the finite deferred loop, not just its name.
        admitted = source.split(
            "void SHIELD_WorldSystem::wait_for_admitted_streaming_jobs() {", 1)[1].split(
                "\n}", 1)[0]
        self.assertEqual(re.findall(r'wait_for_\w+_jobs\(\);', admitted), [
            "wait_for_generation_jobs();", "wait_for_hydro_prefetch_jobs();",
            "wait_for_meshing_jobs();", "wait_for_promotion_jobs();",
            "wait_for_meshing_jobs();"])
        streaming = source.split(
            "void SHIELD_WorldSystem::wait_for_streaming_jobs() {", 1)[1].split(
                "\n}", 1)[0]
        self.assertRegex(streaming, r'wait_for_admitted_streaming_jobs\(\);\s*'
                         r'(?:/[^\n]*\n\s*)*while \(deferred_meshing_pending\(\)\) \{\s*'
                         r'dispatch_deferred_meshing\(\);\s*wait_for_admitted_streaming_jobs\(\);\s*\}')
        for admission in admissions:
            self.assertLess(admission.end(), drains[0].start())
        self.assertLess(primitive.index("JobWatchdogEnabled()"), drains[0].start())
        for vector in ("chunks_to_build", "chunks_to_consider_for_collision"):
            self.assertLess(drains[-1].end(), primitive.index(f"{vector}.reserve(surface_capacity)"))
        self.assertLess(drains[-1].end(), primitive.index("m_streaming_state.chunks.emplace"))

        native = read_source("test/common/SurfaceWorkBudget_test.cpp")
        self.assertIn('#include "luminumbra_common/core/JobSystem.h"', native)
        for lane in ("Mesh", "Generation", "Promotion"):
            name = f"TallSurfaceRefusalRetainsUnpublished{lane}Batch"
            marker = f"TEST(SurfaceWorkBudget, {name})"
            self.assertEqual(native.count(marker), 1)
            case = native.split(marker, 1)[1].split("\nTEST(", 1)[0]
            for token in ("jobs.startup(1);", "world.begin_tick(0);",
                          "world.quiesce_streaming_jobs_for_save();",
                          "ASSERT_EQ(world.pending_publication_chunks(), 1u);",
                          "EXPECT_FALSE(world.EnsureSurfaceReadyNear(anchor, &physics, 0, 0));",
                          "EXPECT_FALSE(world.EnsureCollisionReadyNear(anchor, &physics, 0));",
                          "EXPECT_EQ(world.EditTerrainVoxel(anchor, 1.0f, false, &physics), 0);",
                          "EXPECT_EQ(world.pending_publication_chunks(), 1u);",
                          "EXPECT_EQ(chunk->current_lod.load(), original_lod);",
                          "EXPECT_EQ(chunk->mesh_version.load(), original_version);",
                          "EXPECT_EQ(chunk->sdf_data, original_sdf);",
                          "EXPECT_EQ(world.find_streamed_chunk(Luminumbra::IVec3(0)), chunk);",
                          "EXPECT_EQ(world.snapshot_streamed_chunks().size(), 1u);",
                          "EXPECT_EQ(world.far_lod_authority_revision(), original_revision);",
                          "world.begin_tick(8);", "world.activate_due(8);",
                          "EXPECT_EQ(world.pending_publication_chunks(), 0u);"):
                self.assertIn(token, case)
            self.assertLess(case.index("world.quiesce_streaming_jobs_for_save();"),
                            case.index("EXPECT_FALSE(world.EnsureSurfaceReadyNear("))
            if lane == "Generation":
                self.assertIn("world.dispatch_generation_jobs(", case)
                self.assertIn("ASSERT_TRUE(chunk->pending_generation_ready.load());", case)
            else:
                self.assertIn("world.update(registry, anchor, nullptr);", case)
                self.assertIn("ASSERT_TRUE(world.adopt_streamed_chunk(chunk));", case)
                ready = "mesh" if lane == "Mesh" else "promotion"
                self.assertIn(f"ASSERT_TRUE(chunk->pending_{ready}_ready.load());", case)
                self.assertIn("EXPECT_EQ(chunk->mesh_version.load(), original_version + 1u);", case)
        cmake = read_source("test/CMakeLists.txt")
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)',
                           cmake, re.DOTALL)
        self.assertIsNotNone(target)
        self.assertEqual(target[1].split().count("common/SurfaceWorkBudget_test.cpp"), 1)
        targets = cmake.split("set(LUMINUMBRA_GTEST_TARGETS", 1)[1].split("\n)", 1)[0]
        self.assertEqual(targets.split().count("world_open_refusal_test"), 1)
        self.assertIn("foreach(_lum_gtest_target ${LUMINUMBRA_GTEST_TARGETS})", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_surface_cli_and_direct_siblings_share_admission_and_native_owners(self):
        server = read_source("src/luminumbra_server/main_server.cpp")
        for flag, member in (("--radius", "surface_radius"),
                             ("--collision-radius", "collision_radius")):
            branch = server.split(f'std::strcmp(arg, "{flag}") == 0', 1)[1].split(
                "} else if", 1)[0]
            self.assertIn(f"TryParseSurfaceRadius(v, options.{member})", branch)
            self.assertNotIn("atoi", branch)
        client = read_source("src/luminumbra_client/core/RuntimeScenarioConfig.cpp")
        self.assertIn('std::string_view("--horizon-radius")', client)
        self.assertIn('std::string_view("--collision-radius")', client)
        self.assertIn("World::TryParseSurfaceRadius(value, radius)", client)
        self.assertIn("throw std::invalid_argument", client)
        paths = client.split("RuntimePathOptions ParseRuntimePathOptions(", 1)[1].split(
            "RuntimeScenarioConfig\nParseRuntimeScenarioConfig", 1)[0]
        self.assertIn("ParseSurfaceRadiusOptions(", paths)
        self.assertLess(paths.index("ParseSurfaceRadiusOptions("),
                        paths.index("std::filesystem::create_directories"))
        main = read_source("src/luminumbra_client/main_client.cpp")
        self.assertIn("RuntimeScenarioConfig scenario_config;", main)
        main = main.split(
            "RuntimeScenarioConfig scenario_config;", 1)[1].split("std::string override_error", 1)[0]
        self.assertRegex(main, r'try\s*\{\s*scenario_config = ParseRuntimeScenarioConfig\('
                         r'argc, argv, root_dir\);\s*\} catch '
                         r'\(const std::invalid_argument& error\)\s*\{[\s\S]*return 2;')
        for filename, function, first_write in (
            ("NetworkedSessionDriver", "bool NetworkedSessionDriver::Begin(",
             "m_impl->config = config"),
            ("PersistenceRoundtrip", "RunPersistenceRoundtripSavePhase(",
             "CarveSphereIntoChunk(*chunk"),
        ):
            body = read_source("src/luminumbra_client/core/scenarios/" + filename + ".cpp").split(
                function, 1)[1]
            self.assertIn("World::SurfaceRadiiSupported(", body)
            self.assertLess(body.index("SurfaceRadiiSupported"), body.index(first_write))
        cmake = read_source("test/CMakeLists.txt")
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)',
                           cmake, re.DOTALL)
        self.assertIsNotNone(target)
        self.assertEqual(target[1].split().count("common/SurfaceWorkBudget_test.cpp"), 1)
        native = read_source("test/common/SurfaceWorkBudget_test.cpp")
        for name in ("CheckedColumnsAndExactVerticalBudget",
                     "DecimalAdmissionHasNoOverflowOrPartialFallback",
                     "HeaderDecodeAndWriterRefuseBothRadiiBeforeOutput",
                     "RefusedWriterReopenRetainsQueuedRecordingAndPath",
                     "RunnerAndRecordingRefuseBeforeSessionOrFiles",
                     "DirectSurfaceAndCollisionRefuseBeforeChunkMutation",
                     "TallSurfaceRefusesExactWorkBeforeCreatingChunks"):
            self.assertRegex(native, rf'TEST(?:_F)?\(\w+, {name}\)')
        self.assertIn("stream.seekp(offset)", native)
        self.assertIn("RunReplay(options)", native)
        self.assertIn("RunRecord(options)", native)

    def test_preview_refusal_preserves_live_ownership_and_releases_worker(self):
        source = read_source("src/luminumbra_client/world/WorldgenPreview.cpp")
        build = source.split("void WorldgenPreview::build_world_pending()", 1)[1].split(
            "void WorldgenPreview::swap_pending_into_live(", 1)[0]
        self.assertRegex(build, r'if \(!m_pending_world->EnsureSurfaceReadyNear\('
                         r'[\s\S]*?\)\)\s*\{\s*m_pending_water.reset\(\);\s*'
                         r'm_pending_world.reset\(\);\s*return;\s*\}')
        self.assertLess(build.index("return;"), build.index("m_pending_world->update("))
        swap = source.split("void WorldgenPreview::swap_pending_into_live(", 1)[1].split(
            "bool WorldgenPreview::tick(", 1)[0]
        self.assertRegex(swap, r'if \(m_pending_world\)\s*\{\s*'
                         r'pipeline.prepare_world_swap\(\);\s*m_water.reset\(\);')
        success, failure = swap.split("} else {", 1)
        self.assertIn("++m_rebuild_generation", success)
        self.assertRegex(failure, r'm_last_build_failed = true;[\s\S]*'
                         r'm_first_build_requested = true;\s*\}')
        for forbidden in ("m_world =", "m_water.reset", "m_registry =",
                          "++m_rebuild_generation", "prepare_world_swap"):
            self.assertNotIn(forbidden, failure)
        self.assertIn("m_build_done.store(false);", failure)
        self.assertIn("m_build_inflight.store(m_build_pending.load());", failure)
        self.assertIn("m_build_cv.notify_one();", failure)
        native = read_source("test/ui/worldgen_preview_test.cpp")
        for name in ("RefusedFirstSurfaceKeepsWorldEmptyAndAcceptsNextCandidate",
                     "RefusedSurfaceRetainsLiveWorldAndAllowsRecovery"):
            self.assertIn(f"TEST(WorldgenPreviewTest, {name})", native)
        cmake = read_source("test/CMakeLists.txt")
        self.assertRegex(cmake, r'add_executable\(worldgen_preview_test\s+'
                         r'ui/worldgen_preview_test.cpp\s*\)')
        for targets in ("LUMINUMBRA_GTEST_TARGETS", "LUMINUMBRA_GL_GTEST_TARGETS"):
            registered = re.search(rf'^set\({targets}\s+(.*?)^\)', cmake,
                                   re.DOTALL | re.MULTILINE)
            self.assertIsNotNone(registered)
            self.assertEqual(registered[1].split().count("worldgen_preview_test"), 1)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_surface_consumers_stop_before_capture_save_or_world_entry(self):
        persistence = read_source(
            "src/luminumbra_client/core/scenarios/PersistenceRoundtrip.cpp").split(
                "RunPersistenceRoundtripSavePhase(", 1)[1].split(
                    "RunPersistenceRoundtripLoadPhase(", 1)[0]
        self.assertEqual(persistence.count("if (!world_system->EnsureSurfaceReadyNear("), 2)
        self.assertLess(persistence.index("if (!world_system->EnsureSurfaceReadyNear("),
                        persistence.index("CarveSphereIntoChunk(*chunk"))
        self.assertEqual(len(re.findall(
            r'if \(!world_system->EnsureSurfaceReadyNear\([\s\S]*?\)\)\s*\{\s*'
            r'result.failure_reason = "surface_not_ready";\s*return result;', persistence)), 2)
        drive = read_source("src/luminumbra_client/core/ScenarioRunnerDrive.cpp")
        self.assertEqual(drive.count("EnsureSurfaceReadyNear("), 3)
        self.assertEqual(len(re.findall(
            r'if \(![^\n]+->EnsureSurfaceReadyNear\([\s\S]*?\)\)\s*\{\s*'
            r'scenario_failed = true;\s*'
            r'scenario_failure_reason = "surface_preparation_refused";\s*'
            r'glfwSetWindowShouldClose\(window, true\);\s*return true;', drive)), 3)
        self.assertIn("return scenario_failed ? InGameDrive::kBreakCase : InGameDrive::kHandled;",
                      drive)
        survey = read_source("src/luminumbra_client/rendering/SceneSurvey.cpp")
        self.assertRegex(survey, r'if \(!w->EnsureSurfaceReadyNear\(anchor, phys, 12, 4\)\)'
                         r'\s*\{\s*LUMINUMBRA_CORE_ERROR\([^;]*;\s*return false;')
        self.assertIn("bool RunSceneSurvey(", survey)
        self.assertIn("bool RunSceneSurvey(", read_source(
            "src/luminumbra_client/rendering/SceneSurvey.h"))
        self.assertLess(survey.index("if (!w->EnsureSurfaceReadyNear("),
                        survey.index("const FrameScanReport rep = ScanFrame"))
        main = read_source("src/luminumbra_client/main_client.cpp")
        self.assertRegex(main, r'if \(!Luminumbra::Rendering::RunSceneSurvey\('
                         r'[^;]*\)\)\s*\{\s*exit_code = 2;')
        entry = main.split("auto finish_world_entry =", 1)[1].split(
            "auto start_world_creation =", 1)[0]
        self.assertRegex(entry, r'if \(!horizon_ready\)\s*\{\s*refuse_world_entry\('
                         r'[^;]*;\s*return;')
        self.assertLess(entry.index("if (!horizon_ready)"),
                        entry.index("entry_horizon_prepared = true;"))
        self.assertNotIn("GameState::IN_GAME", entry)
        preload = main.split("auto wait_for_world_presentation =", 1)[1].split(
            "auto activate_loaded_world =", 1)[0]
        self.assertLess(preload.index("g_camera ="),
                        preload.index("PrepareInitialWaterPresentationNear("))
        self.assertNotIn("world->update(", preload)
        self.assertIn("return entry_presentation_gate.waiting();", preload)
        self.assertRegex(main, r'if \(!gameSession->GetWorldSystem\(\)->EnsureSurfaceReadyNear\('
                         r'[^;]*, 4, 2\)\)\s*\{[^}]*return 2;')
        horizon = main.split("auto prepare_entry_horizon =", 1)[1].split(
            "auto drain_entry =", 1)[0]
        self.assertRegex(horizon, r'entry_horizon_prepared = gameSession->'
                         r'PrepareInitialSimulation\(\s*'
                         r'capture_horizon \? scenario_config.horizon_radius : 12,\s*'
                         r'capture_horizon \? scenario_config.collision_radius : 4,\s*'
                         r'entry_stop.get_token\(\)\);\s*'
                         r'return entry_horizon_prepared;')
        staged = main.split("case WorldEntryStage::PrepareHorizon:", 1)[1].split(
            "case WorldEntryStage::Activate:", 1)[0]
        self.assertRegex(staged, r'if \(!prepare_entry_horizon\(\)\)\s*\{\s*'
                         r'refuse_world_entry\([^;]*;\s*return WorldEntryResult::Failed;')
        loading = main.split("case GameState::WORLD_LOADING: {", 1)[1].split(
            "case GameState::IN_GAME:", 1)[0]
        self.assertRegex(loading, r'if \(!prepare_entry_horizon\(\)\)\s*\{[\s\S]*'
                         r'refuse_world_entry\([^;]*;\s*break;')
        self.assertIn('g_uiManager->RequestLoadDocument("main_menu.rml");', loading)
        self.assertLess(loading.index("if (!prepare_entry_horizon())"),
                        loading.index("activate_loaded_world();"))
        backdrop = main.split('CreateTransientWorld("Menu Vista"', 1)[1].split(
            "int exit_code = 0;", 1)[0]
        self.assertRegex(backdrop, r'if \(auto\* ws = gameSession->GetWorldSystem\(\);\s*'
                         r'ws && gameSession->GetPhysicsSystem\(\) &&\s*'
                         r'ws->EnsureSurfaceReadyNear\(')
        self.assertIn('refuse_world_entry("Menu surface preparation failed.", false);', backdrop)

    def assert_diagnostic_bundle_build_graph(self, cmake):
        targets = dict(re.findall(r'add_executable\((\w+)\s+(.*?)\)', cmake, re.DOTALL))
        runtime = "../src/luminumbra_client/core/RuntimeScenarioConfig.cpp"
        capture = "../src/luminumbra_client/rendering/DiagnosticCapture.cpp"
        temporal = "../src/luminumbra_client/rendering/TemporalAudit.cpp"
        graph = TEST_SOURCES["test_targets"](ROOT, cmake)
        closures = {name: TEST_SOURCES["source_closure"](graph, name) for name in targets}
        consumers = {name for name, sources in closures.items() if Path(runtime[3:]) in sources}
        self.assertEqual(consumers, {"capture_manifest_test", "capture_feature_profile_test",
                                     "diagnostic_bundle_test", "runtime_surface_options_test",
                                     "audio_disabled_test", "temporal_sweep_test",
                                     "render_capture_test"})
        for target, retained in (
            ("capture_manifest_test", ("rendering/capture_manifest_test.cpp",
                                       "rendering/diagnostic_capture_test.cpp",
                                       "rendering/temporal_audit_test.cpp",
                                       "../src/luminumbra_client/rendering/CaptureManifest.cpp")),
            ("capture_feature_profile_test", ("rendering/capture_feature_profile_test.cpp",)),
            ("temporal_sweep_test", ("rendering/temporal_sweep_test.cpp",)),
        ):
            for source in retained:
                self.assertEqual(targets[target].split().count(source), 1, (target, source))
        profile = Path("src/luminumbra_client/rendering/CaptureFeatureProfile.cpp")
        for target in ("capture_feature_profile_test", "temporal_sweep_test"):
            self.assertEqual(closures[target][profile], 1, target)
        for target in sorted(consumers):
            for source in (runtime, capture, temporal):
                self.assertEqual(closures[target][Path(source[3:])], 1, (target, source))
            for source in graph[target]["sources"]:
                self.assertTrue((ROOT / source).is_file(), source)
            libraries = re.search(rf'target_link_libraries\({target}\s+PRIVATE\s+(.*?)\)',
                                  cmake, re.DOTALL)
            self.assertIsNotNone(libraries)
            self.assertIn("luminumbra_common", libraries[1].split())
        bundle = "rendering/diagnostic_bundle_test.cpp"
        attribution = "rendering/diagnostic_attribution_test.cpp"
        self.assertEqual([name for name, sources in targets.items() if bundle in sources.split()],
                         ["diagnostic_bundle_test"])
        self.assertEqual([name for name, sources in targets.items()
                          if attribution in sources.split()], ["render_smoke_test"])
        registered = re.search(r'^set\(LUMINUMBRA_GTEST_TARGETS\s+(.*?)^\)', cmake,
                               re.DOTALL | re.MULTILINE)
        self.assertIsNotNone(registered)
        for target in consumers:
            self.assertEqual(registered[1].split().count(target), 1, target)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_diagnostic_bundle_and_runtime_consumers_keep_current_dependencies(self):
        self.assert_diagnostic_bundle_build_graph(read_source("test/CMakeLists.txt"))

    def test_diagnostic_bundle_build_graph_rejects_missing_dependencies_and_duplicate_owner(self):
        cmake = read_source("test/CMakeLists.txt")
        for target, token in (
            ("diagnostic_bundle_test", "../src/luminumbra_client/rendering/TemporalAudit.cpp"),
            ("capture_profile_test_support", "../src/luminumbra_client/rendering/DiagnosticCapture.cpp"),
            ("capture_profile_test_support", "../src/luminumbra_client/rendering/TemporalAudit.cpp"),
            ("capture_profile_test_support", "../src/luminumbra_client/rendering/CaptureFeatureProfile.cpp"),
            ("capture_launch_test_support", "../src/luminumbra_client/core/RuntimeScenarioConfig.cpp"),
            ("capture_manifest_test", "rendering/temporal_audit_test.cpp"),
            ("diagnostic_bundle_test", "rendering/diagnostic_bundle_test.cpp"),
            ("temporal_sweep_test", "rendering/temporal_sweep_test.cpp"),
            ("temporal_sweep_test", "capture_profile_test_support"),
        ):
            command = "add_library" if target.endswith("_support") else "add_executable"
            start = cmake.index(f"{command}({target}")
            changed = cmake[:start] + cmake[start:].replace(token, "", 1)
            with self.subTest(target=target, token=token), self.assertRaises(AssertionError):
                self.assert_diagnostic_bundle_build_graph(changed)
        changed = cmake.replace("add_executable(temporal_sweep_test",
                                "add_executable(temporal_sweep_test\n"
                                "    ../src/luminumbra_client/rendering/DiagnosticCapture.cpp", 1)
        with self.assertRaises(AssertionError):
            self.assert_diagnostic_bundle_build_graph(changed)
        changed = cmake.replace("add_executable(capture_manifest_test",
                                "add_executable(capture_manifest_test\n"
                                "    rendering/diagnostic_attribution_test.cpp", 1)
        with self.assertRaises(AssertionError):
            self.assert_diagnostic_bundle_build_graph(changed)
        changed = cmake.replace("    diagnostic_bundle_test\n", "", 1)
        with self.assertRaises(AssertionError):
            self.assert_diagnostic_bundle_build_graph(changed)

    def assert_runtime_surface_owner(self, cmake, native):
        target = "runtime_surface_options_test"
        sources = re.search(rf'add_executable\({target}\s+(.*?)\)', cmake, re.DOTALL)
        self.assertIsNotNone(sources)
        self.assertEqual(sources[1].split(), ["client/runtime_surface_options_test.cpp"])
        graph = TEST_SOURCES["test_targets"](ROOT, cmake)
        closure = TEST_SOURCES["source_closure"](graph, target)
        for source in ("src/luminumbra_client/core/RuntimeScenarioConfig.cpp",
                       "src/luminumbra_client/rendering/DiagnosticCapture.cpp",
                       "src/luminumbra_client/rendering/TemporalAudit.cpp"):
            self.assertEqual(closure[Path(source)], 1, source)
        for command, dependencies in (
            ("target_include_directories", ("../src", "../src/luminumbra_client")),
            ("target_link_libraries", ("capture_launch_test_support", "gtest_main",
                                       "luminumbra_common", "nlohmann_json", "glm")),
        ):
            block = re.search(rf'{command}\({target}\s+PRIVATE\s+(.*?)\)', cmake, re.DOTALL)
            self.assertIsNotNone(block)
            for dependency in dependencies:
                self.assertEqual(block[1].split().count(dependency), 1)
        registered = re.search(r'^set\(LUMINUMBRA_GTEST_TARGETS\s+(.*?)^\)', cmake,
                               re.DOTALL | re.MULTILINE)
        self.assertIsNotNone(registered)
        self.assertEqual(registered[1].split().count(target), 1)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)
        self.assertIn('#include "core/RuntimeScenarioConfig.h"', native)
        self.assertIn('#include "../support/UniqueTempPath.h"', native)
        self.assertIn("using Luminumbra::Client::ScenarioHarness::ParseRuntimeScenarioConfig;",
                      native)
        self.assertRegex(native, r'return ParseRuntimeScenarioConfig\('
                         r'static_cast<int>\(argv.size\(\)\), argv.data\(\), root\);')
        self.assertRegex(native, r'return Luminumbra::Client::ScenarioHarness::'
                         r'ParseRuntimePathOptions\(\s*static_cast<int>\(argv.size\(\)\), '
                         r'argv.data\(\)\);')
        self.assertIn("CreateUniqueTempDirectory(", native)
        self.assertIn("fs::remove_all(path, error);", native)
        for name in ("RefusesBeforeCreatingSaveRootInEitherArgumentOrder",
                     "RefusesInvalidRadiiAndAcceptsBoundariesInBothSpellings"):
            self.assertEqual(native.count(f"TEST(RuntimeSurfaceOptionsTest, {name})"), 1)
        for check in ("for (const bool radius_first : {false, true})",
                      "for (const bool equals : {false, true})",
                      "EXPECT_THROW(ParsePaths(arguments), std::invalid_argument);",
                      "EXPECT_FALSE(fs::exists(save_root));",
                      "EXPECT_TRUE(fs::is_directory(save_root));",
                      'EXPECT_EQ(Parse({"client"}).horizon_radius, 12);',
                      'EXPECT_EQ(Parse({"client"}).collision_radius, 4);'):
            self.assertIn(check, native)

    def test_runtime_surface_cases_use_registered_shipping_parser_target(self):
        self.assert_runtime_surface_owner(read_source("test/CMakeLists.txt"),
                                          read_source("test/client/runtime_surface_options_test.cpp"))

    def test_runtime_surface_owner_rejects_missing_registration_and_fake_helpers(self):
        cmake = read_source("test/CMakeLists.txt")
        native = read_source("test/client/runtime_surface_options_test.cpp")
        for token in ("client/runtime_surface_options_test.cpp",
                      "capture_launch_test_support",
                      "    runtime_surface_options_test\n"):
            # Mutate the new target itself, even when another target owns the
            # same production source. The target must retain every dependency.
            start = cmake.index("add_executable(runtime_surface_options_test")
            changed = cmake[:start] + cmake[start:].replace(token, "", 1)
            with self.subTest(token=token), self.assertRaises(AssertionError):
                self.assert_runtime_surface_owner(changed, native)
        for original in ("return ParseRuntimeScenarioConfig(",
                         "ParseRuntimePathOptions(",
                         "EXPECT_FALSE(fs::exists(save_root));"):
            with self.subTest(helper=original), self.assertRaises(AssertionError):
                self.assert_runtime_surface_owner(cmake, native.replace(original, "", 1))


def compact_capture_source(source):
    return re.sub(r"\s+", "", without_comments(source))


def capture_body(source, anchor):
    start = source.index("{", source.index(anchor))
    depth = 0
    for match in re.finditer(r'"(?:\\.|[^"\\])*"|[{}]', source[start:]):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if depth == 0:
                return source[start:start + match.end()]
    raise AssertionError("unclosed capture block: " + anchor)


class ThrowawaySaveSourceContract(unittest.TestCase):
    # A guard must outlive all writers of its directory, including Shutdown()
    # from a runner destructor on an early return or function-try-block unwind.
    sites = (
        ("Replay", "intRunRecord(", "save_cleanup", "runner", "."),
        ("Replay", "intRunReplay(", "save_cleanup", "runner", "."),
        ("Heavy", "intRunHeavy(", "save_cleanup", "original", "."),
        ("Smoke", "RunSmokeOnce(", "save_cleanup", "runner", "."),
        ("Lockstep", "intRunLockstepLoopback(", "host_cleanup", "host_runner", "->"),
        ("Lockstep", "intRunLockstepLoopback(", "peer_cleanup", "peer_runner", "->"),
        ("Replicate", "intRunReplicate(", "save_cleanup", "runner", "."),
    )

    def sources(self):
        return {mode: compact_capture_source(read_source(MODES + mode + ".cpp"))
                for mode, *_ in self.sites}

    def assert_cleanup(self, sources):
        for mode, entry, guard, runner, access in self.sites:
            body = capture_body(sources[mode], entry)
            declaration = "Luminumbra::Server::ThrowawaySaveDirGuard" + guard + ";"
            runner_declaration = ("auto" + runner + "=make_runner(" if access == "->" else
                                  "Luminumbra::Server::ServerWorldRunner" + runner + "(")
            self.assertIn(declaration, body)
            self.assertLess(body.index(declaration), body.index(runner_declaration))
            boot = "if(!" + runner + access + "Boot())"
            arm = guard + ".Arm(" + runner + access + "Session()->GetWorldSaveDir());"
            # Arming is the first statement after successful Boot, before any
            # capture, snapshot, other boot, or refusal return can intervene.
            refusal = capture_body(body, boot)
            self.assertIn("returnresult;" if mode == "Smoke" else "return1;", refusal)
            self.assertIn(boot + refusal + arm, body)
            self.assertEqual(body.count(guard + ".Arm("), 1)
            if mode == "Heavy":
                self.assertLess(body.index(declaration), body.index(
                    "Luminumbra::Server::ServerWorldRunnerloaded("))
                self.assertLess(body.index(arm), body.index("original.SaveFullSnapshot();"))
                self.assertIn("cfgLoad.world_id=world_id;", body)
                self.assertIn("conststd::stringworld_id=original.Session()->GetMetadata().worldId;", body)
            if mode == "Lockstep":
                self.assertLess(body.index(declaration), body.index("autohost_runner="))
                self.assertLess(body.index(declaration), body.index("autopeer_runner="))
                self.assertLess(body.index("host_cleanup.Arm("), body.index("if(!peer_runner->Boot())"))

    def test_every_throwaway_mode_arms_cleanup_before_capture_and_outlives_runners(self):
        self.assert_cleanup(self.sources())

    def test_each_mode_and_peer_detects_removed_late_or_unarmed_guard(self):
        sources = self.sources()
        for mode, entry, guard, runner, access in self.sites:
            body = capture_body(sources[mode], entry)
            declaration = "Luminumbra::Server::ThrowawaySaveDirGuard" + guard + ";"
            arm = guard + ".Arm(" + runner + access + "Session()->GetWorldSaveDir());"
            boot = "if(!" + runner + access + "Boot())"
            boot_block = boot + capture_body(body, boot)
            mutations = (
                body.replace(declaration, "", 1),
                body.replace(arm, "", 1),
                body.replace(arm, guard + ".Arm({});", 1),
                body.replace(declaration, "", 1).replace(arm, declaration + arm, 1),
                body.replace(arm, "", 1).replace(boot_block, arm + boot_block, 1),
                body.replace(boot_block, boot_block.replace(
                    "returnresult;" if mode == "Smoke" else "return1;", ""), 1),
            )
            for mutation, changed_body in enumerate(mutations):
                changed = dict(sources)
                changed[mode] = sources[mode].replace(body, changed_body, 1)
                with self.subTest(mode=mode, entry=entry, guard=guard, mutation=mutation):
                    with self.assertRaises((AssertionError, ValueError)):
                        self.assert_cleanup(changed)

    def assert_guard(self, header):
        guard = capture_body(header, "classThrowawaySaveDirGuard")
        self.assertIn("ThrowawaySaveDirGuard(constThrowawaySaveDirGuard&)=delete;", guard)
        self.assertIn("ThrowawaySaveDirGuard&operator=(constThrowawaySaveDirGuard&)=delete;", guard)
        self.assertIn("m_save_dir=save_dir;", capture_body(guard, "voidArm("))
        self.assertIn("~ThrowawaySaveDirGuard()noexcept{", guard)
        destructor = capture_body(guard, "~ThrowawaySaveDirGuard()")
        self.assertIn("if(!m_save_dir.empty())", destructor)
        self.assertIn("try{std::error_codeerror;std::filesystem::remove_all(m_save_dir,error);}", destructor)
        self.assertIn("catch(...){}", destructor)

    def test_guard_removal_is_nonthrowing_and_mutation_checked(self):
        header = compact_capture_source(read_source("src/luminumbra_server/ServerWorldRunner.h"))
        self.assert_guard(header)
        for token in ("std::filesystem::remove_all(m_save_dir,error);", "noexcept",
                      "if(!m_save_dir.empty())", "catch(...){}", "m_save_dir=save_dir;"):
            with self.subTest(token=token), self.assertRaises((AssertionError, ValueError)):
                self.assert_guard(header.replace(token, "", 1))

    def test_persistent_modes_do_not_delete_user_saves(self):
        for mode in ("Server", "NetTcp", "NetRuntime", "NetSoak", "Gns", "Steam"):
            source = read_source(MODES + mode + ".cpp")
            self.assertNotIn("ThrowawaySaveDirGuard", source)
            self.assertNotIn("remove_all(", source)

    def test_registered_native_refusals_check_save_cleanup_and_destructor_order(self):
        native = read_source("test/common/ReplayModeAdmission_test.cpp")
        for case in ("RefusedVerifyCapturesNeverPublishDivergenceArtifacts",
                     "RefusedRecordCapturesNeverPublishReplayOrRegionTrace",
                     "RefusedHeavyCapturesFailBeforeComparisonOrArtifact"):
            body = capture_body(native, case + ")")
            self.assertIn("ExpectRefusedSaveRemoved();", body)
        self.assertIn("ASSERT_TRUE(std::filesystem::is_directory(fault.save_dir));", native)
        self.assertIn("EXPECT_FALSE(fs::exists(CaptureFault().save_dir));", native)
        self.assertIn('EXPECT_TRUE(fs::is_empty(root / "worlds/saves"));', native)
        self.assertRegex(native, r'TEST_P\(ThrowawaySaveCleanup,\s*'
                         r'RemovesSavesAfterEveryRunnerOnReturnAndRefusal\)')
        self.assertIn("const SavingRunner first{first_save, first_saved};", native)
        self.assertIn("const SavingRunner second{second_save, second_saved};", native)
        self.assertIn("testing::Combine(testing::Values(0, 1, 2, 3), testing::Bool())", native)
        self.assertIn("ThrowawayCleanupToleratesUnarmedEmptyMissingAndRemovalError", native)
        self.assertIn("common/ReplayModeAdmission_test.cpp", read_source("test/CMakeLists.txt"))
        self.assertIn("tools/ci/test_replay_source_contract.py", read_source("tools/ci/ci_paths.py"))


class AuthorityConsumerSourceContract(unittest.TestCase):
    paths = {
        "runner": "src/luminumbra_server/ServerWorldRunner.cpp",
        "header": "src/luminumbra_common/persistence/WorldSaveService.h",
        "replay": MODES + "Replay.cpp",
        "heavy": MODES + "Heavy.cpp",
        "smoke": MODES + "Smoke.cpp",
        "lockstep": MODES + "Lockstep.cpp",
    }

    def sources(self):
        return {name: compact_capture_source(read_source(path))
                for name, path in self.paths.items()}

    def assert_ordered(self, source, *tokens):
        positions = [source.index(token) for token in tokens]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(len(positions), len(set(positions)))

    def assert_runner_contract(self, sources):
        for name, request, publication in (
                ("ComputeWorldHash", "{},Persistence::AuthorityCaptureParts::Composite",
                 "returnresult.capture->composite;"),
                ("ComputeWorldSubHashes", "entities_snapshot,Persistence::AuthorityCaptureParts::SubHashes",
                 "returnresult.capture->sub;"),
                ("ComputeWorldHashAndSubHashes", "entities_snapshot",
                 "out_world_hash=result.capture->composite;out_sub=result.capture->sub;")):
            body = capture_body(sources["runner"], "ServerWorldRunner::" + name + "(")
            self.assert_ordered(body,
                                "if(!m_booted||!m_session){throwPersistence::AuthorityCaptureRefused("
                                "Persistence::AuthorityCaptureStatus::MissingCursor);}",
                                "m_session->CaptureAuthorityHashes(" + request + ");",
                                "if(!result.capture){throwPersistence::AuthorityCaptureRefused(result.status);}",
                                publication)
            self.assertNotIn("snapshot_streamed_chunks", body)
            self.assertNotIn(".clear()", body)
        exception = capture_body(sources["header"], "classAuthorityCaptureRefused")
        self.assertIn(":m_status(capture_status){}", exception)
        self.assertIn("AuthorityCaptureStatusstatus()constnoexcept{returnm_status;}", exception)

    def assert_replay_contract(self, sources):
        replay = sources["replay"]
        for helper, call in (("CaptureWorldHash", "returnrunner.ComputeWorldHash();"),
                             ("CaptureCheckpoint", "runner.ComputeWorldHashAndSubHashes(cp.world_hash,sub);")):
            body = capture_body(replay, helper + "(")
            self.assert_ordered(body, call,
                                "catch(constLuminumbra::Persistence::AuthorityCaptureRefused&error)",
                                "error.status()", "returnstd::nullopt;")
        record = capture_body(replay, "intRunRecord(")
        verify = capture_body(replay, "intRunReplay(")
        self.assert_ordered(record, "constautostart_hash=CaptureWorldHash(0,runner);",
                            "if(!start_hash){return1;}", "header.start_world_hash=*start_hash;",
                            "writer.Open(")
        for cadence in ("if((executed%kCheckpointIntervalTicks)==0)",
                        "if(executed!=0&&(executed%kCheckpointIntervalTicks)!=0)"):
            body = capture_body(record, cadence)
            self.assert_ordered(body, "constautocheckpoint=CaptureCheckpoint(executed,runner);",
                                "if(!checkpoint){return1;}", "writer.RecordCheckpoint(*checkpoint);")
        self.assert_ordered(record, "constautofinal_hash=CaptureWorldHash(executed,runner);",
                            "if(!final_hash){return1;}", "writer.Finalize(")
        self.assert_ordered(verify, "constautostart_hash=CaptureWorldHash(0,runner);",
                            "if(!start_hash){return1;}", "conststd::stringlive_start=*start_hash;",
                            "constboolstart_match=", "if(!start_match)")
        self.assert_ordered(verify, "constautolive=CaptureCheckpoint(executed,runner);",
                            "if(!live){return1;}", "if(live->world_hash!=recorded->world_hash")
        self.assert_ordered(verify, "constautoend_hash=", "CaptureWorldHash(executed,runner);",
                            "if(!end_hash){return1;}", "conststd::stringlive_end=*end_hash;",
                            'if(diverged){', '"luminumbra.replay_divergence.v1"')

    def assert_other_modes_contract(self, sources):
        heavy = sources["heavy"]
        self.assertIn("intRunHeavy(constServerCliOptions&options,"
                      "constLuminumbra::world::OwnerContinuationOptions&profile)try{", heavy)
        cli = capture_body(heavy, "intRunHeavy(constServerCliOptions&options)")
        self.assertIn("returnRunHeavy(options,{.owner_continuation=!options.weather_schedule});", cli)
        skip = capture_body(cli, "if(options.weather_schedule)")
        self.assertIn("ownercomparisonskipped:weatherschedule", skip)
        for runner, config in (("original", "cfgOrig"), ("loaded", "cfgLoad")):
            self.assertIn(f"ServerWorldRunner{runner}(std::move({config}),profile);", heavy)
            self.assertEqual(heavy.count(f"CaptureOwners({runner},profile.owner_continuation)"), 2)
        catch = capture_body(heavy, "catch(constLuminumbra::Persistence::AuthorityCaptureRefused&error)")
        self.assert_ordered(catch, "error.status()", "return1;")
        checked = capture_body(heavy, "intRunHeavy(")
        self.assert_ordered(checked, "original.SaveFullSnapshot();",
                            "constHeavyHashesorig_at_save=CaptureHashes(original);",
                            "constHeavyHashesloaded_at_load=CaptureHashes(loaded);",
                            "constHeavyHashesorig_final=CaptureHashes(original);",
                            "constHeavyHashesloaded_final=CaptureHashes(loaded);",
                            "AuthoritativeStateEqual(orig_at_save,loaded_at_load)",
                            "AuthoritativeStateEqual(orig_final,loaded_final)", "std::ofstreamout(artifact_path)")
        for key, entry in (("smoke", "RunSmoke"), ("lockstep", "RunLockstepLoopback")):
            source = sources[key]
            self.assertIn("int" + entry + "(constServerCliOptions&options)try{", source)
            self.assertIn("catch(constLuminumbra::Persistence::AuthorityCaptureRefused&error)", source)
            catch = capture_body(source, "catch(constLuminumbra::Persistence::AuthorityCaptureRefused&error)")
            self.assertIn("error.status()", catch)
            self.assertIn("return1;", catch)

    def test_runner_slots_and_status_are_checked_before_publication(self):
        self.assert_runner_contract(self.sources())

    def test_replay_refusals_precede_comparisons_and_publication(self):
        self.assert_replay_contract(self.sources())

    def test_heavy_and_sibling_modes_stop_on_capture_refusal(self):
        self.assert_other_modes_contract(self.sources())

    def test_heavy_cli_owner_profile_and_skip_have_registered_regressions(self):
        native = read_source("test/common/ReplayModeAdmission_test.cpp")
        for case in ("HeavyCliComparesUnchangedOwnerCheckpoints",
                     "HeavyCliOwnerMismatchFailsBothCheckpointPairs",
                     "HeavyCliWeatherScheduleLogsSkippedOwnerComparison",
                     "HeavyCliInvalidWeatherScheduleStillRefusesBeforeSave"):
            self.assertIn("TEST_F(ReplayModeAdmission, " + case + ")", native)
        self.assertIn("ASSERT_EQ(RunHeavy(options), 0)", native)
        self.assertIn("EXPECT_EQ(RunRefusedCaptureHeavy(options), 1)", native)
        cmake = read_source("test/CMakeLists.txt")
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)', cmake, re.DOTALL)
        self.assertIsNotNone(target)
        for path in ("common/ReplayModeAdmission_test.cpp",
                     "../src/luminumbra_server/modes/Heavy.cpp"):
            self.assertEqual(target[1].split().count(path), 1)
        sources = self.sources()
        for token, replacement in (
                (".owner_continuation=!options.weather_schedule", ".owner_continuation=false"),
                (".owner_continuation=!options.weather_schedule", ".owner_continuation=true"),
                ("ownercomparisonskipped:weatherschedule", ""),
                ("std::move(cfgOrig),profile", "std::move(cfgOrig)"),
                ("std::move(cfgLoad),profile", "std::move(cfgLoad)")):
            changed = dict(sources)
            self.assertIn(token, changed["heavy"])
            changed["heavy"] = changed["heavy"].replace(token, replacement, 1)
            with self.subTest(token=token), self.assertRaises(AssertionError):
                self.assert_other_modes_contract(changed)

    def test_each_capture_guard_and_order_has_an_isolated_mutation(self):
        sources = self.sources()
        mutations = []
        for slot in ("Composite", "SubHashes"):
            mutations.append(("runner", "Persistence::AuthorityCaptureParts::" + slot, "All"))
        # Each runner refusal and output publication, including every status bridge.
        for token in ("result.status", "MissingCursor", "out_world_hash=result.capture->composite;"):
            for occurrence in range(sources["runner"].count(token)):
                mutations.append(("runner", token, occurrence))
        mutations.extend(("header", token, "") for token in
                         (":m_status(capture_status)", "returnm_status;"))
        for token in ("if((executed%kCheckpointIntervalTicks)==0)",
                      "if(executed!=0&&(executed%kCheckpointIntervalTicks)!=0)",
                      "constautocheckpoint=CaptureCheckpoint(executed,runner);",
                      "writer.RecordCheckpoint(*checkpoint);", "writer.Finalize(executed,trace)",
                      "if(!start_hash){return1;}", "if(!checkpoint){return1;}",
                      "if(!final_hash){return1;}", "if(!live){return1;}",
                      "if(!end_hash){return1;}", "returnstd::nullopt;"):
            for occurrence in range(sources["replay"].count(token)):
                mutations.append(("replay", token, occurrence))
        for match in re.finditer(r'if\(!(?:start_hash|checkpoint|final_hash|live|end_hash)\)\{return1;\}',
                                 sources["replay"]):
            changed = dict(sources)
            changed["replay"] = (sources["replay"][:match.start()] +
                                 match[0].replace("return1;", "") + sources["replay"][match.end():])
            with self.subTest(refusal_return=match.start()), self.assertRaises((AssertionError, ValueError)):
                self.assert_replay_contract(changed)
        for key in ("heavy", "smoke", "lockstep"):
            mutations.append((key, "catch(constLuminumbra::Persistence::AuthorityCaptureRefused&error)", ""))
        mutations.append(("heavy", "constHeavyHashesloaded_final=CaptureHashes(loaded);", ""))
        for key, token, replacement in mutations:
            with self.subTest(path=self.paths[key], token=token, replacement=replacement):
                changed = dict(sources)
                if isinstance(replacement, int):
                    parts = changed[key].split(token)
                    changed[key] = token.join(parts[:replacement + 1]) + "REMOVED" + token.join(parts[replacement + 1:])
                else:
                    changed[key] = changed[key].replace(token, replacement, 1)
                checker = (self.assert_runner_contract if key in ("runner", "header") else
                           self.assert_replay_contract if key == "replay" else
                           self.assert_other_modes_contract)
                with self.assertRaises((AssertionError, ValueError)):
                    checker(changed)
        # Moving the last refused capture below comparison or publication also fails.
        for key, capture, later in (
                ("replay", "if(!end_hash){return1;}", 'if(diverged){'),
                ("replay", "if(!final_hash){return1;}", "if(!writer.Finalize"),
                ("heavy", "constHeavyHashesloaded_final=CaptureHashes(loaded);", "constboolresim_ok=")):
            changed = dict(sources)
            moved = changed[key].replace(capture, "", 1)
            position = moved.index(later)
            changed[key] = moved[:position] + later + capture + moved[position + len(later):]
            with self.subTest(key=key, order=capture), self.assertRaises((AssertionError, ValueError)):
                (self.assert_replay_contract if key == "replay" else self.assert_other_modes_contract)(changed)

    def test_native_refusals_use_real_modes_and_registered_target(self):
        native = read_source("test/common/ReplayModeAdmission_test.cpp")
        for case in ("RefusedVerifyCapturesNeverPublishDivergenceArtifacts",
                     "RefusedRecordCapturesNeverPublishReplayOrRegionTrace",
                     "RefusedHeavyCapturesFailBeforeComparisonOrArtifact",
                     "RunnerCapturePreservesStatusAndCombinedOutputsOnRefusal",
                     "SubHashCaptureSkipsRefusedRegionalPreparation"):
            self.assertRegex(native, rf'TEST_[FP]\(\w+,\s*{case}\)')
        for mode in ("Replay", "Heavy"):
            self.assertIn(f'#include "../../src/luminumbra_server/modes/{mode}.cpp"', native)
        first_include = native.index('#include "../../src/luminumbra_server/modes/Heavy.cpp"')
        last_include = native.index('#include "../../src/luminumbra_server/modes/Replay.cpp"')
        first_case = native.index("TEST_P(")
        for original, adapted in (("ServerWorldRunner", "RefusedCaptureRunner"),
                                  ("RunRecord", "RunRefusedCaptureRecord"),
                                  ("RunReplay", "RunRefusedCaptureReplay"),
                                  ("RunMutateReplayFixture", "RunRefusedCaptureMutateReplayFixture"),
                                  ("RunHeavy", "RunRefusedCaptureHeavy")):
            define = f"#define {original} {adapted}\n"
            undef = f"#undef {original}\n"
            self.assertEqual(native.count(define), 1)
            self.assertEqual(native.count(undef), 1)
            self.assertLess(native.index(define), first_include)
            self.assertLess(last_include, native.index(undef))
            self.assertLess(native.index(undef), first_case)
        self.assertIn("ServerWorldRunner::ComputeWorldHash()", native)
        self.assertIn("ServerWorldRunner::ComputeWorldHashAndSubHashes(hash, sub)", native)
        self.assertIn("m_incomplete->set_state(ChunkState::Loading)", native)
        self.assertIn("EXPECT_EQ(fs::exists(options.artifact_path), retained)", native)
        self.assertIn('EXPECT_EQ(ReadReplayModeFile(options.artifact_path), "retained")', native)
        self.assertIn("common/ReplayModeAdmission_test.cpp", read_source("test/CMakeLists.txt"))


class SessionUsabilitySourceContract(unittest.TestCase):
    def assert_namespace_receipt_completion(self, source):
        completion = self.function(source, "CompleteNamespaceRetirement", "")
        ordered = (
            "operations.inspect_namespace(receipt.identity)",
            "operations.record_prepared(receipt.identity)",
            "receipt.phase = NamespaceReceiptPhase::Prepared;",
            "operations.remove_owned(receipt.identity)",
            "operations.record_namespace_done(receipt.identity)",
            "receipt.phase = NamespaceReceiptPhase::NamespaceDone;",
            "operations.flush_later_commit()",
            "if (!observation.succeeded)",
            "!QualifiesNamespaceFlush(receipt.stable_volume, observation)",
            "operations.record_flush_observed(receipt.identity, observation)",
            "receipt.phase = NamespaceReceiptPhase::FlushObserved;",
            "operations.retire_receipt(receipt.identity)",
        )
        positions = [completion.index(operation) for operation in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("return NamespaceRetirementOutcome::Retained;", completion)
        self.assertIn("receipt.identity[1] == 0 && receipt.identity[2] == 0", completion)
        self.assertIn("receipt.phase != NamespaceReceiptPhase::FlushObserved", completion)
        self.assertIn("if (receipt.phase == NamespaceReceiptPhase::Prepared)", completion)
        self.assertIn("if (receipt.phase == NamespaceReceiptPhase::NamespaceDone)", completion)

    def test_namespace_receipt_requires_ordered_same_volume_ntfs_completion(self):
        source = read_source("src/luminumbra_common/persistence/RecoveryDurability.h")
        self.assert_namespace_receipt_completion(source)
        for original, replacement in (
            ("!QualifiesNamespaceFlush(receipt.stable_volume, observation)", "false"),
            ("operations.record_flush_observed(receipt.identity, observation)", "true"),
            ("operations.inspect_namespace(receipt.identity)", "NamespaceEntryState::Absent"),
            ("operations.record_prepared(receipt.identity)", "true"),
        ):
            with self.subTest(original=original), self.assertRaises((AssertionError, ValueError)):
                self.assert_namespace_receipt_completion(source.replace(original, replacement))

    def assert_stable_volume_contract(self, source):
        qualification = self.function(source, "QualifiesNamespaceFlush", "")
        for guard in ("journal.succeeded", "journal.volume_known", "journal.ntfs",
                      "artifact.known()", "journal.stable_volume.known()",
                      "artifact == journal.stable_volume"):
            self.assertIn(guard, qualification)
        self.assertNotIn("journal.volume ==", qualification)
        native = self.function(source, "VolumeIdentityFromHandle", "")
        self.assertIn("FILE_NAME_NORMALIZED | VOLUME_NAME_GUID", native)
        self.assertIn("length == 0 || length >= name.size()", native)
        self.assertIn("VolumeIdentityFromGuidPath", native)

    def test_stable_volume_contract_refuses_serial_only_or_unknown_identity(self):
        source = read_source("src/luminumbra_common/persistence/RecoveryDurability.h")
        self.assert_stable_volume_contract(source)
        for guard in ("artifact.known()", "journal.stable_volume.known()",
                      "artifact == journal.stable_volume"):
            with self.subTest(guard=guard), self.assertRaises(AssertionError):
                self.assert_stable_volume_contract(source.replace(guard, "true"))

    def test_receipt_volume_preparation_precedes_durable_resets(self):
        source = read_source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        prepare = self.function(source, "PrepareReceipts", "")
        self.assertLess(prepare.index("ObserveReceiptVolume(save_dir, candidate)"),
                        prepare.index("RecordReceipts("))
        parse = self.function(source, "ParseJournalRecord", "")
        self.assertIn('"luminumbra.journal.v5"', parse)
        self.assertIn('"stable_volume", receipt.completion.stable_volume.words', parse)
        self.assertIn('"luminumbra.journal.v6"',
                      read_source("src/luminumbra_common/persistence/SaveTransaction.h"))

    def test_journal_flush_observation_uses_same_handle_and_clears_failures(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        append = self.function(source, "append_journal_record", "WorldSaveService::")
        self.assertLess(append.index("*observation = {};"), append.index("if (record.empty()"))
        native = append.index("::GetVolumeInformationByHandleW(file,")
        self.assertLess(append.index('write_bytes("\\n", 1) && flush()'), native)
        self.assertIn("detail::VolumeIdentityFromHandle(file)", append)
        self.assertRegex(append, r"if \(completed_flush.stable_volume.known\(\) &&\s*::GetVolumeInformationByHandleW\(file,")
        self.assertIn("serial == actual.front()", append)
        self.assertIn('std::wstring_view(filesystem_name.data()) == L"NTFS"', append)
        self.assertLess(native, append.index("const bool closed = close();"))
        self.assertLess(append.index("if (!ok || !closed)"),
                        append.index("*observation = completed_flush;"))

    def assert_transaction_receipt_integration(self, source):
        parse = self.function(source, "ParseJournalRecord", "")
        self.assertIn('schema->get<std::string>() != "luminumbra.journal.v4"', parse)
        self.assertIn('parsed.modern = schema->get<std::string>() != "luminumbra.journal.v4";', parse)
        self.assertIn('root.size() != expected_fields', parse)
        self.assertIn('phase->get<std::uint64_t>() > 3', parse)
        stream = self.function(source, "ApplyJournalRecords", "")
        self.assertIn('ApplyJournalRecords(bytes, out, error, true)',
                      self.function(source, "ParseJournal", ""))
        self.assertIn('record.modern != out.modern', stream)
        self.assertIn('journal receipt transition invalid', stream)
        completion = self.function(source, "CompleteReceipts", "")
        self.assertIn('!detail::QualifiesNamespaceFlush(receipt.completion.stable_volume, latest_flush)', completion)
        self.assertIn('if (!receipt.completion.stable_volume.known())', completion)
        self.assertIn('FlushReceiptCarrier(RecoveryControl(save_dir, document), control)', completion)
        self.assertIn('NearestExistingDirectory((save_dir / receipt.path).parent_path())', completion)
        self.assertLess(completion.index('WorldSaveService::remove_owned_entry('),
                        completion.index('namespace_done, 1, control, errors, &latest_flush'))
        self.assertLess(completion.index('namespace_done, 1, control, errors, &latest_flush'),
                        completion.index('!detail::QualifiesNamespaceFlush(receipt.completion.stable_volume, latest_flush)'))
        self.assertLess(completion.index('!detail::QualifiesNamespaceFlush(receipt.completion.stable_volume, latest_flush)'),
                        completion.index('flush_observed, 2, control, errors'))
        self.assertLess(completion.index('flush_observed, 2, control, errors'),
                        completion.index('retired, 3, control, errors'))
        encoder = self.function(source, "RecordReceipts", "")
        self.assertIn('kMaxSnapshotIndexBytes - 1 - batch_bytes - comma', encoder)
        self.assertLess(encoder.index('AppendMetadata('), encoder.index('receipts[i]->completion.phase'))
        self.assertIn('journal duplicate receipt path', parse)
        self.assertIn('next_phase == previous_phase + 1', stream)
        flush = self.function(source, "FlushReceiptCarrier", "")
        self.assertIn('actual == expected', flush)
        self.assertLess(flush.index('::FlushFileBuffers(file)'),
                        flush.index('::GetVolumeInformationByHandleW(file,'))
        self.assertIn('serial == actual.front()', flush)
        self.assertIn('detail::VolumeIdentityFromHandle(file)', flush)
        self.assertIn('if (!::CloseHandle(file))', flush)
        finish = self.function(source, "FinishJournal", "")
        self.assertLess(finish.index('PrepareReceipts('), finish.index('WorldSaveService::publish_restoration('))
        self.assertLess(finish.index('document.external_control = true;'), finish.index('bool all_retired'))
        self.assertLess(finish.index('if (!all_retired)'), finish.index('"crash-before-receipt-retirement"'))
        self.assertIn('if (!document.modern)', finish)
        self.assertIn('return FinishLegacyJournal(', finish)
        aliases = self.function(source, "ControlAliasLinks", "")
        self.assertIn('"undo/cleanup.txn.json"', aliases)
        self.assertIn('false, present)', aliases)
        inventory = self.function(source, "JournalInventory", "")
        self.assertIn('ControlAliasLinks(save_dir, document)', inventory)
        notifications = self.function(source, "report_snapshot_step", "void ")
        self.assertIn('IsReceiptCarrierName(', notifications)
        for event in ('before-publication', 'after-publication',
                      'publication-quarantined', 'retirement-quarantined'):
            self.assertIn('"crash-receipt-' + event + '"', notifications)
        self.assertNotIn('"receipt-recorded"', source)
        self.assertIn('if (kNeedsFileReceipts)', finish)
        recovery = self.function(source, "recover_pending_journal", "")
        self.assertLess(recovery.index('RecoverReceiptCarriers(save_dir, errors)'),
                        recovery.index('fs::symlink_status(journal, ec)'))
        rollback = self.function(source, "RollbackEntries", "")
        self.assertIn('const auto identity = missing ? entry->rollback_retired_identity : current;', rollback)
        self.assertIn('CompleteRemoval(save_dir,', rollback)
        self.assertIn('"crash-before-restoration-completion"', rollback)
        self.assertNotIn('if (!missing && !WorldSaveService::remove_owned_entry(', rollback)

    def assert_receipt_generation_contract(self, source):
        parse = self.function(source, "ParseJournalRecord", "")
        self.assertIn('generation ? generation->get<std::uint64_t>() : 0', parse)
        self.assertIn('generation && !generation->is_number_unsigned()', parse)
        self.assertIn('std::pair{receipt.path, receipt.generation}', parse)
        stream = self.function(source, "ApplyJournalRecords", "")
        for token in ('receipt.generation != 0',
                      'receipt.generation != latest->second.generation + 1',
                      '!latest->second.retired', 'latest != found',
                      'out.recorded_slots[slot].insert(identity)'):
            self.assertIn(token, stream)
        self.assertGreaterEqual(stream.count('out.recorded_slots[slot].insert(identity)'), 2)
        self.assertIn('!latest->second.reset', stream)
        capacity = self.function(source, 'AppendMetadata', '')
        self.assertLess(capacity.index('ReservePendingReceipts(document, reserve)'),
                        capacity.index('WorldSaveService::append_journal_record('))
        self.assertIn('!document.receipts.contains({name, generation})', capacity)
        reserve = self.function(source, 'ReservePendingReceipts', '')
        self.assertIn('document.pending_receipt_bytes > kMaxSnapshotIndexBytes - reserve', reserve)
        self.assertNotIn('for (', reserve)
        writer = self.function(source, 'write_journal', 'SaveTransaction::')
        self.assertLess(writer.index('ReservePendingReceipts(document, append_reserve)'),
                        writer.index('record,\n', writer.index('ReservePendingReceipts(')))
        encoder = self.function(source, "RecordReceipts", "")
        self.assertIn('{"generation", receipt.generation}', encoder)
        inventory = self.function(source, "JournalInventory", "")
        self.assertIn('history->second.contains(receipt.completion.identity)', inventory)

    def test_receipt_generations_preserve_identity_and_refuse_unordered_reuse(self):
        source = read_source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        self.assert_receipt_generation_contract(source)
        for token in ('generation ? generation->get<std::uint64_t>() : 0',
                      'receipt.generation != latest->second.generation + 1',
                      '!latest->second.retired', 'latest != found',
                      'out.recorded_slots[slot].insert(identity)',
                      'ReservePendingReceipts(document, reserve)',
                      'ReservePendingReceipts(document, append_reserve)',
                      '{"generation", receipt.generation}',
                      'history->second.contains(receipt.completion.identity)'):
            with self.subTest(token=token), self.assertRaises((AssertionError, ValueError)):
                self.assert_receipt_generation_contract(source.replace(token, 'removed', 1))
        self.assertIn('TEST(SaveTransaction, ReceiptGenerationsRetainEachRecordedPublicationIdentity)',
                      read_source("test/persistence/SaveTransaction_test.cpp"))

    def assert_live_journal_work_is_incremental(self, source):
        writer = self.function(source, 'write_journal', 'SaveTransaction::')
        self.assertNotIn('ParseJournal(', writer)
        self.assertNotIn('input.read(', writer)
        self.assertIn('read_live_journal(errors)', writer)
        self.assertIn('ApplyJournalRecords(record, document, error)', writer)
        self.assertIn('&document.version', writer)
        reader = self.function(source, 'read_live_journal', 'SaveTransaction::')
        self.assertIn('state.valid && version == state.document.version', reader)
        fast_path = reader.index('if (state.valid && version == state.document.version')
        self.assertIn('state.document.cached_bytes)', reader[fast_path:reader.index('input.read(')])
        self.assertLess(reader.index('input.version(version)'), fast_path)
        self.assertLess(reader.index('return true;', fast_path), reader.index('input.read('))
        self.assertLess(reader.index('after != version'), reader.index('ParseJournal('))
        self.assertIn('state.document.cached_bytes = std::move(bytes)', reader)
        self.assertIn('after != version', reader)
        completion = self.function(source, 'complete_owned_removal', 'SaveTransaction::')
        self.assertNotIn('ParseJournal(', completion)
        self.assertLess(completion.index('if (!publication)'),
                        completion.index('JournalInventory('))
        self.assertLess(completion.index('JournalInventory('),
                        completion.index('const auto& entry ='))
        self.assertEqual(completion.count('JournalInventory('), 2)
        self.assertIn('member_document.entries.push_back(document.entries[recorded->second])',
                      completion)
        self.assertRegex(completion, r'&state\.inventory_entries,\s*false\)\)')
        inventory = self.function(source, 'JournalInventory', '')
        self.assertIn('if (!scan_directories)', inventory)
        reserve = self.function(source, 'ReservePendingReceipts', '')
        self.assertNotIn('for (', reserve)
        self.assertIn('document.pending_receipt_bytes', reserve)
        stream = self.function(source, 'ApplyJournalRecords', '')
        self.assertIn('out.pending_receipt_bytes -= PendingReceiptBytes(previous->second)', stream)
        self.assertIn('out.pending_receipt_bytes += PendingReceiptBytes(receipt)', stream)
        phases = self.function(source, 'RecordReceipts', '')
        self.assertIn('document.pending_receipt_bytes -= PendingReceiptBytes(*receipts[i])', phases)
        self.assertIn('document.pending_receipt_bytes += PendingReceiptBytes(*receipts[i])', phases)
        prepare = self.function(source, 'PrepareReceipts', '')
        self.assertIn('document.pending_receipt_bytes += PendingReceiptBytes(*receipt)', prepare)
        metadata = self.function(source, 'AppendMetadata', '')
        self.assertIn('document.version == SavedInputFile::Version{} ? nullptr : &document.version',
                      metadata)
        self.assertIn('document.cached_bytes->append(bytes)', metadata)
        self.assertIn('document.cached_bytes->append(record)', writer)

    def test_live_journal_work_is_incremental_and_revalidates_native_versions(self):
        source = read_source('src/luminumbra_common/persistence/SaveTransaction.cpp')
        self.assert_live_journal_work_is_incremental(source)
        for token in ('state.valid && version == state.document.version',
                      'state.document.cached_bytes)',
                      'ApplyJournalRecords(record, document, error)',
                      '&document.version', 'after != version',
                      'member_document.entries.push_back(document.entries[recorded->second])'):
            with self.subTest(token=token), self.assertRaises((AssertionError, ValueError)):
                self.assert_live_journal_work_is_incremental(source.replace(token, 'removed'))
        service = read_source('src/luminumbra_common/persistence/WorldSaveService.cpp')
        append = self.function(service, 'append_journal_record', 'WorldSaveService::')
        self.assertIn('const auto expected_version = version ? *version : SavedInputFile::Version{}',
                      append)
        self.assertLess(append.index('current == expected_version'),
                        append.index('truncate_tail(published)'))
        self.assertLess(append.index('current == expected_version'),
                        append.index('write_bytes(record.data()'))
        for token in ('expected_bytes->size() > kMaxSnapshotIndexBytes',
                      'expected_version != SavedInputFile::Version{} && !expected_bytes',
                      'size == expected_bytes->size()',
                      'std::memcmp(tail.data(), expected_bytes->data() + offset, count) == 0'):
            self.assertIn(token, append)
        self.assertLess(append.index('std::memcmp('), append.index('truncate_tail(published)'))
        self.assertLess(append.index('std::memcmp('), append.index('write_bytes(record.data()'))
        stream = self.function(source, 'ApplyJournalRecords', '')
        self.assertIn('out.recorded_slots[slot].insert(entry.incoming_identity)', stream)
        self.assertLess(append.index('write_bytes("\\n", 1) && flush()'),
                        append.index('SavedInputFile::native_version(file, appended_version)'))
        self.assertLess(append.index('SavedInputFile::native_version(file, appended_version)'),
                        append.index('const bool closed = close()'))
        self.assertLess(append.index('::ReOpenFile(file,'),
                        append.index('const bool closed = close()'))
        self.assertLess(append.index('const bool closed = close()'),
                        append.index('SavedInputFile::native_version(version_file, appended_version)'))
        self.assertLess(append.index('::CloseHandle(version_file)'),
                        append.index('report_snapshot_step("crash-record-published", path)'))
        tests = read_source('test/persistence/SaveTransaction_test.cpp')
        for name in ('ForwardJournalWorkScalesWithAppendedRecords',
                     'LiveJournalReloadRefusesChangedControlWithoutPublishing',
                     'LiveJournalReloadAcceptsCompletePrefixAndDiscardsUnpublishedTail',
                     'LiveJournalCacheRechecksControlAndDirectoryIdentities',
                     'LiveJournalCacheDoesNotAdoptChangesAfterAppendFlush',
                     'JournalAppendRequiresUnchangedVersionBeforeMutatingPrefix',
                     'JournalAppendChecksEveryCachedByteBeforeTruncationOrWrite',
                     'JournalAppendCachedBytesHandleBoundariesAndUnavailableControl',
                     'ForwardRemovalRefusesForeignMemberSlotsBeforeMutation',
                     'LiveJournalCacheRefusesMissingProtectedEntry'):
            self.assertIn('TEST(SaveTransaction, ' + name + ')', tests)

    def assert_world_cleanup_consumers(self, source):
        removal = self.function(source, "durable_remove", "WorldSaveService::")
        self.assertLess(removal.index('SaveTransaction::publication_identity('),
                        removal.index('StagingFile retained('))
        self.assertIn('SaveTransaction::complete_owned_removal(path, errors)', removal)
        self.assertIn('if (!completed.has_value() || !*completed)', removal)
        publication = self.function(source, "PublishProtectedMember", "")
        self.assertLess(publication.index('const bool recorded_restoration = source_stage || retired_stage;'),
                        publication.index('SaveTransaction::publication_stages('))
        self.assertRegex(publication, r'const auto completed = recorded_restoration\s*'
                         r'\? std::optional<bool>\{\}\s*'
                         r': SaveTransaction::complete_owned_removal\(destination, errors\);')
        self.assertLess(publication.index('SaveTransaction::complete_owned_removal(destination, errors)'),
                        publication.index('source.publish_bound('))
        staging = self.function(source, "ReclaimAbandonedStaging", "")
        self.assertIn('if (world_owned)', staging)
        self.assertIn('SaveTransaction::complete_owned_removal(entry.path(), nullptr, false)', staging)
        self.assertLess(staging.index('ReclaimClaimedLedgerStaging('),
                        staging.index('if (world_owned)'))
        shared = self.function(source, "remove_file_and_staging", "WorldSaveService::")
        self.assertLess(shared.index('SaveTransaction::publication_identity('),
                        shared.index('std::filesystem::remove(destination, error)'))
        self.assertIn('return durable_remove(destination, errors);', shared)
        reclaim = self.function(source, "reclaim_file_staging", "WorldSaveService::")
        self.assertIn('SaveTransaction::publication_identity(destination, expected, incoming)', reclaim)

    def test_world_cleanup_consumers_require_recorded_namespace_completion(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        self.assert_world_cleanup_consumers(source)
        for token in ('SaveTransaction::complete_owned_removal(path, errors)',
                      'const bool recorded_restoration = source_stage || retired_stage;',
                      'const auto completed = recorded_restoration',
                      'SaveTransaction::complete_owned_removal(destination, errors)',
                      'SaveTransaction::complete_owned_removal(entry.path(), nullptr, false)',
                      'return durable_remove(destination, errors);'):
            with self.subTest(token=token), self.assertRaises((AssertionError, ValueError)):
                self.assert_world_cleanup_consumers(source.replace(token, 'removed', 1))
        self.assertIn('TEST(WorldSaveService, RemovalCompletionFailureRemainsRetryable)',
                      read_source("test/persistence/world_save_service_test.cpp"))

    def test_recovery_preflights_live_versions_and_always_diagnoses_failure(self):
        source = read_source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        recovery = self.function(source, "recover_pending_journal", "")
        self.assertEqual(recovery.count('RecoveryOutcome::Failed'), 1)
        self.assertIn('if (errors && errors->size() == previous_errors)', recovery)
        self.assertRegex(recovery, r'AddError\(errors, "snapshot recovery refused[^\n]*\);\s*'
                         r'return RecoveryOutcome::Failed;')
        rollback = self.function(source, "RollbackEntries", "")
        self.assertLess(rollback.index('SaveTransaction::recovery_versions('),
                        rollback.index('for (auto* entry : order)',
                                       rollback.index('SaveTransaction::recovery_versions(')))
        self.assertIn('save_dir / entry->path, control, entry->live_versions, errors)', rollback)
        preflight = self.function(source, "recovery_versions", "bool SaveTransaction::")
        self.assertIn('transaction->m_control_identity != control', preflight)
        self.assertIn('versions = {entry.previous_version, entry.installed_version, entry.incoming_version};', preflight)
        native = read_source("test/persistence/SaveTransaction_test.cpp")
        for name in ('RecoveryFailureAlwaysAppendsDiagnostic',
                     'RecoveryPreflightsLiveVersionsBeforeChangingAnyMember',
                     'RecoveryVersionsRequireMatchingWriterAndProtectedPath'):
            self.assertIn('TEST(SaveTransaction, ' + name + ')', native)

    def assert_creation_completion_order(self, source, registry):
        abort = self.function(source, "abort", "SaveTransaction::")
        self.assertIn('ParseJournal(bytes, document, error)', abort)
        self.assertLess(abort.index('m_artifacts->complete_files('),
                        abort.index('if (FinishJournal(m_save_dir, document,'))
        complete = self.function(source, "CompleteCreatedFiles", "")
        self.assertLess(complete.index('JournalInventory('), complete.index('AppendMetadata('))
        self.assertLess(complete.index('AppendMetadata('),
                        complete.index('for (const bool index_batch : {true, false})'))
        self.assertLess(complete.index('PrepareReceipts('),
                        complete.index('WorldSaveService::remove_owned_entry('))
        self.assertIn('CompleteReceipts(save_dir, document, completions, control, errors)', complete)
        registered = self.function(registry, "complete_files", "")
        self.assertLess(registered.index('if (!complete(files))'),
                        registered.index('artifact.identity.reset();'))
        self.assertIn('!tracked(child.path()) && child.path() != control_directory &&', registered)
        self.assertIn('!admitted(child.path())', registered)
        self.assertIn('creation_inventory.contains(path)', abort)
        self.assertNotIn('directory = true', complete)

    def test_creation_registry_completes_files_before_releasing_evidence(self):
        source = read_source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        registry = read_source("src/luminumbra_common/persistence/CreatedWorldArtifacts.h")
        self.assert_creation_completion_order(source, registry)
        for token in ('m_artifacts->complete_files(', 'PrepareReceipts(',
                      'CompleteReceipts(save_dir, document, completions, control, errors)'):
            complete = self.function(source, "CompleteCreatedFiles", "")
            mutated = (source.replace(complete, complete.replace(token, 'removed', 1), 1)
                       if token in complete else source.replace(token, 'removed', 1))
            with self.subTest(token=token), self.assertRaises((AssertionError, ValueError)):
                self.assert_creation_completion_order(mutated, registry)
        with self.assertRaises((AssertionError, ValueError)):
            self.assert_creation_completion_order(source, registry.replace('if (!complete(files))', 'if (false)', 1))
        self.assertIn('CreationRollbackCompletesFilesBeforeJournalAndRetriesEveryInterruption',
                      read_source("test/persistence/SaveTransaction_test.cpp"))

    def test_transaction_v5_receipts_survive_control_cleanup_and_complete_absent_retries(self):
        source = read_source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        self.assert_transaction_receipt_integration(source)
        for token in ('record.modern != out.modern',
                      '"crash-receipt-before-publication"',
                      'root.size() != expected_fields',
                      'phase->get<std::uint64_t>() > 3',
                      '!detail::QualifiesNamespaceFlush(receipt.completion.stable_volume, latest_flush)',
                      'RecoverReceiptCarriers(save_dir, errors)',
                      'const auto identity = missing ? entry->rollback_retired_identity : current;'):
            with self.subTest(token=token), self.assertRaises((AssertionError, ValueError)):
                self.assert_transaction_receipt_integration(source.replace(token, 'removed', 1))
        native = read_source("test/persistence/SaveTransaction_test.cpp")
        for name in ('AbsentCreateRetryCompletesBeforePayloadRollback',
                     'AbortFlushFailureRetainsRecoveryEvidence',
                     'MatchingRestoreFailureBlocksCleanup',
                     'ReceiptDiscoverySurvivesControlAndDirectoryRetirement',
                     'ReceiptDiscoveryCompletesOnlyRecordedControlAliases',
                     'ReceiptDiscoveryRefusesMalformedOrForeignEvidence',
                     'HistoricalV4JournalStillDecodesAndRecovers'):
            self.assertIn('TEST(SaveTransaction, ' + name + ')', native)
        self.assertIn('persistence/SaveTransaction_test.cpp', read_source('test/CMakeLists.txt'))

    def assert_matching_restoration_completion(self, source):
        rollback = self.function(source, "RollbackEntries", "")
        matching = re.search(
            r"if \(previous_bytes &&\s*!WorldSaveService::complete_restoration\(\s*"
            r"destination,\s*current,\s*\{destination.parent_path\(\),\s*save_dir,\s*"
            r"restored_stage.directory,\s*retired_stage.directory\},\s*errors\)\)\s*return false;",
            rollback)
        self.assertIsNotNone(matching)
        self.assertLess(matching.start(), rollback.index('report_snapshot_step("rollback-entry"'))
        self.assertRegex(rollback, r'if \(entry.path == kSnapshotIndexRelativePath\)\s*'
                         r'order.push_back\(&entry\);')
        self.assertRegex(rollback, r'if \(it->path != kSnapshotIndexRelativePath\)\s*'
                         r'order.push_back\(&\*it\);')
        self.assertLess(rollback.index("order.push_back(&entry)"),
                        rollback.index("order.push_back(&*it)"))
        self.assertIn("NearestExistingDirectory(destination.parent_path()), errors)", rollback)
        recovery = self.function(source, "recover_pending_journal", "")
        self.assertRegex(recovery, r'if \(!RollbackEntries\(save_dir, document, control, errors\)\)'
                         r'\s*\{\s*return fail\(\);\s*\}')

    def test_matching_restoration_completes_before_rollback_progresses(self):
        source = read_source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        self.assert_matching_restoration_completion(source)
        for original, replacement in (
            ("previous_bytes &&", "!previous_bytes &&"),
            ("!WorldSaveService::complete_restoration", "!WorldSaveService::artifact_identity"),
            ("restored_stage.directory,", "destination.parent_path(),"),
            ("if (entry.path == kSnapshotIndexRelativePath)", "if (false)"),
        ):
            with self.subTest(original=original), self.assertRaises(AssertionError):
                self.assert_matching_restoration_completion(source.replace(original, replacement))

    def assert_published_restoration_completion(self, source):
        publication = self.function(source, "publish_restoration", "WorldSaveService::")
        self.assertRegex(
            publication, r'if \(!complete_restoration\(\s*destination,\s*incoming,\s*'
            r'\{destination.parent_path\(\),\s*temp.parent_path\(\),\s*'
            r'source_stage \? source_stage->directory : std::filesystem::path\{\},\s*'
            r'retired_stage \? retired_stage->directory : std::filesystem::path\{\}\},\s*'
            r'errors\)\)\s*return false;')
        self.assertLess(publication.index("PublishProtectedMember("),
                        publication.index("complete_restoration("))
        self.assertLess(publication.index("complete_restoration("),
                        publication.index('report_snapshot_step("after-replace"'))

    def test_newly_published_restoration_completes_before_success(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        self.assert_published_restoration_completion(source)
        for original, replacement in (
            ("if (!complete_restoration(destination,", "if (false && !complete_restoration(destination,"),
            ("temp.parent_path(),", "destination.parent_path(),"),
            ("retired_stage ? retired_stage->directory", "source_stage ? source_stage->directory"),
        ):
            with self.subTest(original=original), self.assertRaises(AssertionError):
                self.assert_published_restoration_completion(source.replace(original, replacement))

    def test_restoration_completion_binds_writable_identity_and_checks_close(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        completion = self.function(source, "complete_restoration", "WorldSaveService::")
        for operation in ("LockPublication(destination)", "file.open(StagingFile::Mode::Complete)",
                          "file.identity(actual, false, allowed_links)", "identity != ArtifactIdentity{}",
                          "std::equal(identity.begin(), identity.end(), actual.begin())",
                          "return file.flush();", "return file.close();",
                          "WorldSaveService::durable_flush_directory(parent, diagnostics)"):
            self.assertIn(operation, completion)
        self.assertRegex(completion, r'#if defined\(_WIN32\)\s*constexpr bool flush_file = true;'
                         r'\s*#else\s*constexpr bool flush_file = false;')
        self.assertIn("!detail::CompleteRestoration<flush_file>(operations, expected, parents)",
                      completion)
        staging = source.split("class StagingFile {", 1)[1].split("~StagingFile()", 1)[0]
        self.assertRegex(staging, r'mode == Mode::Create \|\| mode == Mode::Complete\s*'
                         r'\? GENERIC_READ \| GENERIC_WRITE')
        self.assertIn("mode == Mode::Complete ? FILE_SHARE_READ", staging)
        barrier = read_source("src/luminumbra_common/persistence/RecoveryDurability.h")
        self.assertIn("flushed = owned && operations.flush_file();", barrier)
        self.assertIn("const bool closed = operations.close_file();", barrier)
        self.assertIn("if (!owned || !flushed || !closed)", barrier)
        self.assertIn("!parent.empty() && !operations.flush_parent(parent)", barrier)

    def assert_matching_pair_completion(self, source):
        recovery = self.function(source, "RecoverReplayPair", "")
        self.assertRegex(
            recovery, r'if \(old_trace != PairIdentity\{\} && '
            r'\(trace_identity == old_trace \|\| restored\) &&\s*'
            r'!WorldSaveService::complete_restoration\(\s*companion,\s*trace_identity,\s*'
            r'\{directory, directory, \{\}, \{\}\},\s*nullptr,\s*'
            r'std::numeric_limits<unsigned>::max\(\)\)\)\s*return false;')
        self.assertLess(recovery.index("complete_restoration("),
                        recovery.index("journal.remove()"))

    def test_matching_replay_trace_completes_before_pair_cleanup(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        self.assert_matching_pair_completion(source)
        for original, replacement in (
            ("(trace_identity == old_trace || restored)", "(trace_identity == old_trace)"),
            ("std::numeric_limits<unsigned>::max()))", "1))"),
        ):
            with self.subTest(original=original), self.assertRaises(AssertionError):
                self.assert_matching_pair_completion(source.replace(original, replacement))

    def assert_concrete_ecology_admission(self, source):
        self.assertRegex(source, r"const auto plant_env\s*=\s*\[")
        self.assertNotRegex(source, r"\bEnvSampler\b")
        self.assertRegex(
            source,
            r"RunPlantGrowthSystemOnTick\(\s*foliage_registry, current_tick, plant_env, foliage_gate\)")
        initialize = self.function(source, "InitializeScentField")
        self.assertIn("m_scentField->BindSessionAdmission(*this);", initialize)
        self.assertNotIn("BindDepositGate", initialize)
        admission = self.function(source, "SessionCellAllowed", "luminumbra::ai::ScentField::")
        self.assertIn("return m_admissionSession->ScentCellDue(x, z);", admission)

    def test_shipping_plant_and_scent_admission_use_concrete_dispatch(self):
        self.assert_concrete_ecology_admission(
            read_source("src/luminumbra_common/world/GameSession.cpp"))
        header = read_source("src/luminumbra_common/ai/ScentField.h")
        self.assertIn("const ::Luminumbra::world::GameSession* m_admissionSession", header)
        admission = self.function(header, "SimulationCellAllowed", "")
        self.assertRegex(admission, r"if \(m_admissionSession\)\s*\{\s*"
                         r"return SessionCellAllowed\(x, z\);\s*\}")
        deposit = self.function(header, "Deposit", "")
        self.assertIn("!SimulationCellAllowed(x, z)", deposit)
        self.assertNotIn("m_deposit_gate", deposit)

    def test_concrete_ecology_guard_rejects_erased_shipping_regressions(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        for before, after in (
            ("const auto plant_env", "const luminumbra::foliage::EnvSampler plant_env"),
            ("m_scentField->BindSessionAdmission(*this);",
             "m_scentField->BindDepositGate([this](int x, int z) { return ScentCellDue(x, z); });"),
            ("return m_admissionSession->ScentCellDue(x, z);", "return m_deposit_gate(x, z);"),
        ):
            with self.subTest(regression=after):
                self.assertIn(before, source)
                with self.assertRaises(AssertionError):
                    self.assert_concrete_ecology_admission(source.replace(before, after, 1))

    def assert_concrete_regional_samplers(self, source):
        binding = self.function(source, "BindRegionalFieldQueries")
        self.assertNotIn("[this]", binding)
        for system in ("windFieldSystem", "weatherSystem", "aetherFieldSystem"):
            self.assertEqual(binding.count(f"m_{system}->BindRegionalSession(*this);"), 1)
            # Only the empty compatibility call is allowed, to clear legacy sessions.
            self.assertEqual(binding.count(f"m_{system}->BindRegionalBackend("), 1)
            self.assertIn(f"m_{system}->BindRegionalBackend({{}}, {{}});", binding)

    def test_shipping_regional_samplers_use_concrete_dispatch(self):
        self.assert_concrete_regional_samplers(
            read_source("src/luminumbra_common/world/GameSession.cpp"))
        for system, sample in (("WindFieldSystem", "Wind"),
                               ("WeatherSystem", "Weather"),
                               ("AetherFieldSystem", "Aether")):
            source = read_source(f"src/luminumbra_common/systems/{system}.cpp")
            binding = self.function(source, "BindRegionalSession", system + "::")
            self.assertIn("m_sessionBinding.session = &session;", binding)
            self.assertIn("m_regional_sample = {};", binding)
            sampler = self.function(source, "SampleRegional", system + "::")
            self.assertRegex(sampler, r"if \(m_sessionBinding.session\)\s*\{\s*return "
                             + rf"m_sessionBinding.session->SampleRegional{sample}\(")

    def assert_regional_self_assignment(self, header, source, system):
        binding = header.split("struct SessionBinding {", 1)[1].split("SessionBinding m_sessionBinding", 1)[0]
        self.assertRegex(binding, r"operator=\(const SessionBinding& other\) noexcept\s*\{\s*"
                         r"if \(this != &other\)\s*\{\s*session = nullptr;\s*\}\s*return \*this;")
        move = self.function(source, "operator=", system + "::")
        self.assertRegex(move, r"\{\s*if \(this == &other\)\s*\{\s*return \*this;\s*\}")
        # The guarded move must retain the full default memberwise assignment contract.
        members = set(re.findall(r"\bm_\w+\b", header))
        assignments = set(re.findall(r"\b(m_\w+) = (?:std::move\()?other\.\1\)?;", move))
        self.assertEqual(assignments, members)

    def test_regional_self_assignment_guards_and_registered_regressions(self):
        native = read_source("test/common/GameSessionHeadlessWorld_test.cpp")
        cmake = read_source("test/CMakeLists.txt")
        self.assertIn("common/GameSessionHeadlessWorld_test.cpp", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)
        helper = self.function(native, "CheckRegionalSamplerSelfAssignment", "")
        for operation in ("system = alias;", "system = std::move(alias);"):
            self.assertIn(operation, helper)
        self.assertIn("ASSERT_NE(legacy_sample, expected_sample)", helper)
        self.assertIn("ASSERT_NE(legacy_hash, expected_hash)", helper)
        for system, name in (("WindFieldSystem", "Wind"), ("WeatherSystem", "Weather"),
                             ("AetherFieldSystem", "Aether")):
            with self.subTest(system=system):
                header = read_source(f"src/luminumbra_common/systems/{system}.h")
                source = read_source(f"src/luminumbra_common/systems/{system}.cpp")
                self.assert_regional_self_assignment(header, source, system)
                self.assertRegex(native, r"TEST\(GameSessionHeadlessWorldTest,\s*"
                                 + f"Regional{name}SelfAssignmentPreservesSessionBinding" + r"\)")

    def assert_weather_detachment(self, header, source, native):
        members = set(re.findall(r"\bm_\w+\b", header))
        normalize = (r"if \(other\.m_sessionBinding\.session\)\s*\{\s*"
                     r"SelectRegionalBackend\(false\);")
        for parameter in ("const WeatherSystem&", "WeatherSystem&&"):
            signature = f"WeatherSystem::WeatherSystem({parameter} other)"
            self.assertIn(signature, source)
            constructor = source.split(signature, 1)[1]
            constructor = constructor.split("\n}", 1)[0]
            initialized = set(re.findall(
                r"\b(m_\w+)\((?:std::move\()?other\.\1\)?\)", constructor))
            self.assertEqual(initialized, members)
            self.assertRegex(constructor, normalize)
        move = self.function(source, "operator=", "WeatherSystem::")
        self.assertRegex(move, r"if \(other\.m_sessionBinding\.session\)\s*\{\s*"
                         r"WeatherSystem detached\(std::move\(other\)\);\s*"
                         r"return \*this = std::move\(detached\);")
        copy_source = source.split(
            "WeatherSystem& WeatherSystem::operator=(const WeatherSystem& other)", 1)[1]
        copy = copy_source.split("\n}", 1)[0]
        self.assertRegex(copy, r"if \(this == &other\)\s*\{\s*return \*this;")
        self.assertIn("return *this = WeatherSystem(other);", copy)
        clear = self.function(source, "SelectRegionalBackend", "WeatherSystem::")
        for projection in ("m_storm_cells = StormCells();", "m_strikes = StrikeSchedule();"):
            self.assertLess(clear.index(projection), clear.index("m_regionalWeatherKeyed = false;"))
        for member in ("m_regionalStorms", "m_regionalStrikes", "m_regionalStormIndex",
                       "m_regionalStormCache", "m_regionalStrikeCache"):
            self.assertIn(member + ".clear();", clear)
        self.assertIn("m_regionalStrikesDeliveredThrough.reset();", clear)
        self.assertIn("m_hiddenRegionalStrikes = 0;", clear)
        factory = self.function(native, "DetachRegionalWeather", "")
        for operation in ("return WeatherSystem(source);", "return WeatherSystem(std::move(source));",
                          "destination = source;", "destination = std::move(source);"):
            self.assertIn(operation, factory)
        helper = self.function(native, "CheckDetachedRegionalWeatherCase", "")
        for operation in ("detached.Update(302, Vec3(0), nullptr);",
                          "source.RevealRegionalStrikeSchedule();"):
            self.assertIn(operation, helper)
        for name, arguments in (("CopyConstruction", "false, false"),
                                ("MoveConstruction", "true, false"),
                                ("CopyAssignment", "false, true"),
                                ("MoveAssignment", "true, true")):
            regression = re.search(
                r"TEST\(GameSessionHeadlessWorldTest,\s*RegionalWeather" + name
                + r"NormalizesDetachedStorage\)\s*\{([^}]+)\}", native)
            self.assertIsNotNone(regression)
            self.assertIn("CheckDetachedRegionalWeather<" + arguments + ">()", regression[1])

    def test_weather_detachment_preserves_members_and_registers_all_operations(self):
        self.assert_weather_detachment(
            read_source("src/luminumbra_common/systems/WeatherSystem.h"),
            read_source("src/luminumbra_common/systems/WeatherSystem.cpp"),
            read_source("test/common/GameSessionHeadlessWorld_test.cpp"))

    def test_weather_detachment_guard_rejects_each_lost_normalization(self):
        header = read_source("src/luminumbra_common/systems/WeatherSystem.h")
        source = read_source("src/luminumbra_common/systems/WeatherSystem.cpp")
        native = read_source("test/common/GameSessionHeadlessWorld_test.cpp")
        for signature in ("WeatherSystem::WeatherSystem(const WeatherSystem& other)",
                          "WeatherSystem::WeatherSystem(WeatherSystem&& other)",
                          "WeatherSystem& WeatherSystem::operator=(WeatherSystem&& other)",
                          "WeatherSystem& WeatherSystem::operator=(const WeatherSystem& other)"):
            with self.subTest(operation=signature):
                start = source.index(signature)
                end = source.index("\n}", start)
                body = source[start:end]
                if "const WeatherSystem&" in signature and "operator=" in signature:
                    mutated = body.replace("return *this = WeatherSystem(other);", "return *this;")
                elif "operator=" in signature:
                    mutated = re.sub(r"WeatherSystem detached\(std::move\(other\)\);\s*"
                                     r"return \*this = std::move\(detached\);", "", body)
                else:
                    mutated = body.replace("SelectRegionalBackend(false);", "")
                self.assertNotEqual(mutated, body)
                with self.assertRaises(AssertionError):
                    self.assert_weather_detachment(header, source[:start] + mutated + source[end:], native)

    def test_regional_hash_interface_has_fixed_inputs(self):
        header = read_source("src/luminumbra_common/world/GameSession.h")
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        declaration = "std::string RegionalAmbientHash(std::uint8_t channels, bool weather) const;"
        self.assertEqual(header.count(declaration), 1)
        self.assertGreater(header.index(declaration), header.rindex("\nprivate:"))
        for system, name, inputs in (("WindFieldSystem", "Wind", "3, false"),
                                     ("WeatherSystem", "Weather", "12, true"),
                                     ("AetherFieldSystem", "Aether", "16, false")):
            with self.subTest(system=system):
                projection = self.function(source, f"ComputeRegional{name}SubHash")
                self.assertRegex(projection, r"\{\s*return RegionalAmbientHash\("
                                 + re.escape(inputs) + r"\);\s*\}")
                sampler = read_source(f"src/luminumbra_common/systems/{system}.cpp")
                self.assertNotIn("->RegionalAmbientHash(", sampler)
                self.assertIn(f"->ComputeRegional{name}SubHash()", sampler)

    def test_concrete_regional_guard_rejects_each_erased_binding(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        for system in ("windFieldSystem", "weatherSystem", "aetherFieldSystem"):
            before = f"m_{system}->BindRegionalSession(*this);"
            after = f"m_{system}->BindRegionalBackend([this](const auto& p) {{ return p; }}, {{}});"
            with self.subTest(system=system):
                self.assertIn(before, source)
                with self.assertRaises(AssertionError):
                    self.assert_concrete_regional_samplers(source.replace(before, after, 1))

    def test_serialization_reuse_covers_every_canonical_chunk_input(self):
        source = read_source("src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp")
        serialized = (self.function(source, "ChunkToJson", "") +
                      self.function(source, "WaterStateToJson", ""))
        captured = self.function(source, "VisitChunkSnapshotInputs", "")
        for consumer in ("ChunkSnapshotInputBytes", "ChunkSnapshotInputByteCount"):
            self.assertIn("VisitChunkSnapshotInputs(chunk,", self.function(source, consumer, ""))
        fields = set(re.findall(r"\bchunk\.([A-Za-z_][A-Za-z_0-9]*)", serialized))
        self.assertTrue(fields)
        self.assertLessEqual(fields,
                             set(re.findall(r"\bchunk\.([A-Za-z_][A-Za-z_0-9]*)", captured)))
        service = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        reuse = self.function(service, "matches_serialized", "")
        self.assertIn("ChunkRecordKeyOf(found->second.record) == key", reuse)
        self.assertIn("found->second.serialization_inputs == inputs", reuse)

    def test_activation_handoff_requires_exact_record_bytes_and_consumes_the_chunk(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        handoff = self.function(source, "take_decoded", "")
        self.assertIn("ChunkRecordKeyOf(found->second.record) != key", handoff)
        self.assertIn("return std::move(found->second.decoded)", handoff)
        activation = self.function(source, "read_chunk", "WorldSaveService::")
        self.assertIn("CurrentRecordMatches(path, *region, id)", activation)
        self.assertRegex(
            activation,
            r"AdmitChunkRecord\(\s*record,\s*path,\s*&errors,\s*facts,\s*&result.chunk,"
            r"\s*false,\s*false,\s*admission->format.indexed\)")
        session = read_source("src/luminumbra_common/world/GameSession.cpp")
        for name in ("PrepareWorldLoad", "ReadPreparedWorldState"):
            self.assertIn("const Persistence::WorldSaveService::WorldReadScope chunk_reads;",
                          self.function(session, name))

    @staticmethod
    def function(source, name, prefix="GameSession::"):
        signature = re.escape(prefix).replace(r"\ ", r"\s+") + re.escape(name) + r"\("
        match = re.search(signature, source)
        if match is None:
            raise AssertionError("missing function " + prefix + name)
        start = match.start()
        cursor = match.end()
        parameter_depth = 1
        while parameter_depth:
            if source[cursor] == "(":
                parameter_depth += 1
            elif source[cursor] == ")":
                parameter_depth -= 1
            cursor += 1
        opening = source.index("{", cursor)
        # Ignore braces inside diagnostics and comments when locating the body.
        tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\{|\}', source[opening:])
        depth = 0
        for token in tokens:
            if token[0] == "{":
                depth += 1
            elif token[0] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:opening + token.end()]
        raise AssertionError("unterminated session function")

    def test_every_plant_reader_admits_and_reads_the_same_regular_file_handle(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        reader = self.function(source, "load_plant_entities", "WorldSaveService::")
        self.assertIn("const SavedInputFile input(path);", reader)
        self.assertNotIn("std::filesystem::exists(", reader)
        self.assertNotIn("ifstream", reader)
        self.assertLess(reader.index("input.read(bytes, SavedInputLimits::kPlantFileBytes, stop)"),
                        reader.index("LoadEntityRegistrySnapshotJson(bytes,"))
        admission = (self.function(source, "SavedInputFile", "SavedInputFile::") +
                     read_source("src/luminumbra_common/persistence/SavedInputFile.h"))
        self.assertIn("O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC", admission)
        self.assertIn("FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS", admission)
        for check in ("::fstat(m_descriptor, &information)", "S_ISREG(information.st_mode)",
                      "::GetFileType(m_handle) != FILE_TYPE_DISK",
                      "::GetFileInformationByHandle(m_handle, &information)",
                      "FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY",
                      "::GetFileSizeEx(m_handle, &length)", "size > bytes.max_size()"):
            self.assertLess(admission.index(check), admission.index("std::string candidate("))
        self.assertLess(admission.index("std::string candidate("), admission.index("::read(m_descriptor,"))
        self.assertLess(admission.index("std::string candidate("), admission.index("::ReadFile(m_handle,"))
        self.assertIn("size - offset", admission)
        self.assertLess(admission.index("size > maximum"),
                        admission.index("std::string candidate("))
        self.assertLess(reader.index("AdmitPlantComponentBytes(bytes)"),
                        reader.index("LoadEntityRegistrySnapshotJson(bytes,"))
        for name in ("read_exact", "at_end"):
            body = self.function(admission, name, "bool ")
            self.assertIn("detail::ReadSavedInputRetryingInterrupt(", body)
        retry = self.function(admission, "ReadSavedInputRetryingInterrupt", "auto ")
        self.assertIn("for (;;) {", retry)
        self.assertIn("received >= 0 || errno != EINTR", retry)
        self.assertIn("::read(m_descriptor, &trailing, 1)", admission)
        self.assertIn("::ReadFile(m_handle, &trailing, 1, &received, nullptr) && received == 0",
                      admission)
        world = self.function(source, "load_world_impl", "WorldSaveService::")
        self.assertIn("load_plant_entities(plants, save_dir, &errors, stop)", world)
        session = self.function(read_source("src/luminumbra_common/world/GameSession.cpp"),
                                "ReadPreparedWorldState")
        self.assertRegex(session, r"WorldSaveService::load_plant_entities\(\s*state.plants, "
                         r"save_dir, &errors, stop\)")

    def test_catalog_inspection_keeps_complete_validation_for_load_and_writers(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        load = self.function(source, "load_world", "WorldSaveService::")
        inspect = self.function(source, "inspect_save", "WorldSaveService::")
        validate = self.function(source, "validate_save", "WorldSaveService::")
        self.assertRegex(load, r"return load_world_impl\([\s\S]*,\s*false\);")
        self.assertRegex(inspect, r"load_world_impl\([\s\S]*nullptr,\s*nullptr,\s*true,\s*payloads_verified\);")
        self.assertIn(".load_world(", validate)
        self.assertNotIn("load_world_impl", validate)
        world = self.function(source, "load_world_impl", "WorldSaveService::")
        skip = world.index("if (catalog_only && indexed && !retain)")
        self.assertLess(world.index("ReadRegionFile(path, records, &errors, stop)"), skip)
        self.assertLess(world.index("retain_chunks->contains(record.id)"), skip)
        self.assertLess(skip, world.index("AdmitChunkRecord(record,"))

    def assert_publication_reader_borrows_exclusive_ownership(self, source):
        guard = self.function(source, "FilePublicationReadGuard", "FilePublicationReadGuard::")
        release = self.function(source, "~FilePublicationReadGuard", "FilePublicationReadGuard::")
        self.assertIn('const auto& held = HeldPublicationMutexes();', guard)
        self.assertIn('std::find(held.begin(), held.end(), &m_sync->publication)', guard)
        self.assertRegex(guard, r'm_sync = nullptr;\s*return;')
        self.assertLess(guard.index('m_sync = nullptr;'),
                        guard.index('m_sync->publication.lock_shared();'))
        self.assertRegex(release, r'if \(m_sync\)\s*\{\s*m_sync->publication\.unlock_shared\(\);\s*\}')

    def test_publication_reader_borrows_its_threads_exclusive_stripe(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        self.assert_publication_reader_borrows_exclusive_ownership(source)
        for token in ('const auto& held = HeldPublicationMutexes();',
                      'std::find(held.begin(), held.end(), &m_sync->publication)',
                      'm_sync = nullptr;', 'if (m_sync)'):
            with self.subTest(token=token), self.assertRaises((AssertionError, ValueError)):
                self.assert_publication_reader_borrows_exclusive_ownership(
                    source.replace(token, 'removed', 1))
        tests = read_source("test/persistence/SaveTransaction_test.cpp")
        self.assertIn('CollidingJournalStripeAllowsMemberUpdateAndRemoval', tests)
        self.assertIn('detail::file_publication_stripe_for_testing(journal)', tests)
        self.assertIn('ManyMembersKeepDescriptorsBoundedThroughPublicationAndRollback', tests)

    def test_absent_world_member_completion_flushes_existing_ancestor(self):
        source = read_source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        completion = self.function(source, "complete_owned_removal", "SaveTransaction::")
        absent = completion.split('if (identity == ArtifactIdentity{}) {', 1)[1]
        absent = absent.split('#endif', 1)[0]
        self.assertRegex(absent, r'#else\s*(?://[^\n]*\n\s*)*'
                         r'return WorldSaveService::durable_flush_directory\(\s*'
                         r'NearestExistingDirectory\(owned_path.parent_path\(\)\), errors\);')
        tests = read_source("test/persistence/world_save_service_test.cpp")
        self.assertIn('FreshPlantSaveCompletesAbsentRegionMembers', tests)
        self.assertIn('AbsentRegionRemovalRefusesNonDirectoryAncestors', tests)

    def test_native_saved_input_holds_publication_until_after_close(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        header = read_source("src/luminumbra_common/persistence/SavedInputFile.h")
        constructor = self.function(source, "SavedInputFile", "SavedInputFile::")
        destructor = self.function(source, "~SavedInputFile", "SavedInputFile::")
        self.assertIn("detail::FilePublicationReadGuard m_publication;", header)
        for opening in ("::CreateFileW(", "::open("):
            self.assertLess(constructor.index("m_publication(path)"), constructor.index(opening))
        for closing in ("::CloseHandle(m_handle)", "::close(m_descriptor)"):
            self.assertLess(destructor.index(closing), destructor.index("observer(m_path, false)"))
        reader = self.function(header, "read_exact", "bool ")
        self.assertLess(reader.index("stop.stop_requested()"), reader.index("::ReadFile("))
        self.assertLess(reader.index("stop.stop_requested()"), reader.index("::read("))

    def test_far_tile_authority_check_and_publication_share_one_ownership(self):
        source = read_source("src/luminumbra_common/world/FarLodStore.cpp")
        worker = source.split("void AsyncFarLodStore::run()", 1)[1]
        self.assertNotIn("SaveOwnership", worker)
        tile = self.function(source, "save_tile", "FarLodStore::")
        self.assertEqual(tile.count("SaveOwnership ownership;"), 1)
        self.assertLess(tile.index("SaveOwnership ownership;"),
                        tile.index("authority->far_lod_authority_revision()"))
        self.assertLess(tile.index("authority->far_lod_authority_revision()"),
                        tile.index("upsert_container_records("))
        self.assertRegex(tile, r"upsert_container_records\(\s*ownership, region_file,")
        persistence = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        signature = "bool WorldSaveService::upsert_container_records(const SaveOwnership&"
        owned = self.function(persistence[persistence.index(signature):],
                              "upsert_container_records", "WorldSaveService::")
        for recursive_guard in ("SaveGuard ", "SaveOwnership ", "RegionFileWriteGuard "):
            self.assertNotIn(recursive_guard, owned)
        self.assertIn("WriteRegionFile(region_file, raw_records, errors)", owned)
        native = read_source("test/rendering/far_lod_persistence_test.cpp")
        bounded = native.split("FirstWorkerSaveAndFlushCompleteWithinBoundedWait)", 1)[1].split(
            "TEST(FarLodPersistence, QueuedTilesRecheckAuthority", 1)[0]
        for contract in ("ASSERT_EXIT(", "std::chrono::steady_clock::now() < deadline",
                         "std::_Exit(2)", "writer.flush()", "writer.close()"):
            self.assertIn(contract, bounded)

    def test_authority_changes_serialize_with_tile_publication(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        bump = self.function(source, "bump_far_lod_authority_revision", "SHIELD_WorldSystem::")
        self.assertEqual(source.count("m_far_lod_authority_revision.fetch_add("), 1)
        self.assertEqual(bump.count("SaveOwnership ownership;"), 1)
        self.assertLess(bump.index("SaveOwnership ownership;"),
                        bump.index("m_far_lod_authority_revision.fetch_add("))
        self.assertLess(bump.index("SaveOwnership ownership;"),
                        bump.index("m_far_lod_region_revision_mutex"))
        native = read_source("test/rendering/far_lod_persistence_test.cpp")
        self.assertIn("TEST(FarLodPersistence, AuthorityBumpCannotCrossTilePublication)", native)
        self.assertRegex(native, r"set_file_publication_hook_for_testing\(\s*"
                                 r"&AuthorityPublicationOverlap::Publish\)")
        for contract in ("waiting_save_writers()", "overlap.bump_blocked", "overlap.publication_revision",
                         "std::chrono::steady_clock::now() < deadline", "std::_Exit(2)"):
            self.assertIn(contract, native)

    def test_every_visible_authority_mutation_owns_persistence_before_changing_bytes(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        edit = self.function(source, "EditTerrainVoxel", "SHIELD_WorldSystem::")
        self.assertLess(edit.index("SaveOwnership ownership;"),
                        edit.index("chunk->sdf_data[idx] ="))
        self.assertLess(edit.index("chunk->sdf_data[idx] ="),
                        edit.index("bump_far_lod_authority_revision(chunk->get_coords())"))
        adopt = self.function(source, "adopt_streamed_chunk", "SHIELD_WorldSystem::")
        self.assertLess(adopt.index("SaveOwnership ownership;"),
                        adopt.index("m_streaming_state.chunks.emplace("))
        self.assertLess(adopt.index("m_streaming_state.chunks.emplace("),
                        adopt.index("bump_far_lod_authority_revision("))
        promotion = source.split("if (ready && !failed && !chunk->pending_sdf_data.empty() &&", 1)[1]
        self.assertLess(promotion.index("SaveOwnership ownership;"),
                        promotion.index("chunk->sdf_data ="))
        self.assertLess(promotion.index("chunk->sdf_data ="),
                        promotion.index("bump_far_lod_authority_revision("))
        brush = self.function(source, "PrepareTerrainVoxelEdit", "SHIELD_WorldSystem::")
        self.assertLess(brush.index("SaveOwnership ownership;"),
                        brush.index("GenerateChunkData(*chunk, 1)"))
        self.assertLess(brush.index("bump_far_lod_authority_revision("),
                        brush.index("GenerateChunkData(*chunk, 1)"))
        surface = self.function(source, "EnsureSurfaceReadyNear", "SHIELD_WorldSystem::")
        self.assertIn("ChunkSdfProvenance::LoadedOrEdited", surface)
        self.assertLess(surface.index("ownership.emplace()"), surface.index("chunk->sdf_data.clear()"))
        self.assertLess(surface.index("ownership.emplace()"), surface.index("GenerateChunkData(*chunk)"))
        self.assertLess(surface.index("ownership.emplace()"),
                        surface.index("bump_far_lod_authority_revision("))
        self.assertLess(surface.index("bump_far_lod_authority_revision("),
                        surface.index("chunk->sdf_data.clear()"))
        self.assertLess(adopt.index("bump_far_lod_authority_revision("),
                        adopt.index("m_activation_index.insert(chunk)"))
        self.assertIn("catch (...) {\n                        bump_far_lod_authority_revision", edit)
        for name in ("set_params", "set_seed", "regenerate_all_chunks"):
            replacement = self.function(source, name, "SHIELD_WorldSystem::")
            self.assertLess(replacement.index("SaveOwnership ownership;"),
                            replacement.index("bump_far_lod_authority_revision("))
            self.assertLess(replacement.index("bump_far_lod_authority_revision("),
                            replacement.index("clear_world(" if name == "regenerate_all_chunks"
                                              else "reinitialize_noise("))
        scenario = read_source("src/luminumbra_client/core/scenarios/PersistenceRoundtrip.cpp")
        fence = scenario.index("SaveOwnership ownership;")
        self.assertLess(fence, scenario.index("if (!CarveSphereIntoChunk(*chunk,"))
        self.assertLess(scenario.index("chunk->mark_voxel_data_dirty();"),
                        scenario.index("notify_far_lod_authority_changed(chunk->get_coords())"))
        native = read_source("test/rendering/far_lod_persistence_test.cpp")
        self.assertIn("MutatedVoxelEditBlocksOldTileUntilAuthorityBump", native)
        regression = native.split("void CheckMutatedVoxelEdit(", 1)[1].split(
            "TEST(FarLodPersistence, CurrentAuthorityStillValidatesPayload", 1)[0]
        for proof in ("world.SetVoxelEditObserver(", "WorldSaveService::waiting_save_writers()",
                      "EXPECT_FALSE(saved)", "EXPECT_EQ(Bytes(path), before)"):
            self.assertIn(proof, regression)

    def test_saved_input_budgets_precede_allocations_and_publication(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        region = source.split("bool ReadRegionFile(", 1)[1].split("bool AdmitRegionWrite(", 1)[0]
        self.assertLess(region.index("RegionFileLengthAllowed(file_size)"), region.index("std::string manifest("))
        self.assertLess(region.index("ReadU16(control + 4)"), region.index("std::string manifest("))
        for budget in ("kRegionRecordBytes", "kRegionCompressedBytes", "kRegionReadWorkBytes",
                       "kRegionCompressedAggregateBytes"):
            self.assertLess(region.index(budget), region.index("payload.resize("))
        energy_file = read_source("src/luminumbra_common/persistence/SavedEnergyInput.h")
        for budget in ("length > kEnergyReadRecordBytes", "++pages > kEnergyReadPages",
                       "EnergyFieldState::AdmitRecord(header)"):
            self.assertLess(energy_file.index(budget), energy_file.index("input.read(candidate,"))
        energy = read_source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        decode = self.function(energy, "DeserializeRecord", "EnergyFieldState::")
        self.assertLess(decode.index("AdmitRecord(record)"), decode.index("std::istringstream"))
        session = read_source("src/luminumbra_common/world/GameSession.cpp")
        save = self.function(session, "SaveWorldStateToImpl")
        for admission in ("PrepareWorldMetadataTo(", "prepared_plants.prepare(",
                          "prepared_chunks.prepare(", "energy_snapshot.SerializeRecord(" ):
            self.assertLess(save.index(admission), save.index("prepared_chunks.publish("))
        self.assertRegex(save, r"write_chunks\s*&&\s*!prepared_chunks\.prepare\(")
        self.assertIn("!full_snapshot && has_snapshot", save)
        self.assertNotIn("admit_world_write(", save)
        self.assertNotIn("service.save_world(", save)
        self.assertNotIn("save_dirty_chunks(", save)
        metadata = self.function(session, "SaveWorldMetadataTo")
        self.assertLess(metadata.index("PrepareWorldMetadataTo("), metadata.index("PersistSavedPreset("))
        reader = self.function(session, "ReadPreparedWorldState")
        self.assertIn('error.find("Saved-input admission refused:") == 0', reader)
        self.assertNotIn('error.find("oversized")', reader)
        metadata_admission = self.function(source, "admit_metadata_bytes", "WorldSaveService::")
        self.assertLess(metadata_admission.index("Game::WorldClockAuthorityAdapter::WriteMetadata(clock, metadata)"),
                        metadata_admission.index("BoundedJsonDump("))
        pair = self.function(source, "save_metadata_and_active_regions", "WorldSaveService::")
        self.assertLess(pair.index("admit_metadata_bytes(bytes"), pair.index("save_active_regions("))

    def test_clean_generated_chunk_omission_is_incremental_only(self):
        session = read_source("src/luminumbra_common/world/GameSession.cpp")
        save = self.function(session, "SaveWorldStateToImpl")
        selection = save.split("WorldStreamingState state;", 1)[1].split(
            "result.chunks_total = state.size();", 1)[0]
        self.assertRegex(
            selection,
            r"if\s*\(m_activeRegionsEnabled\s*&&\s*!full_snapshot\s*&&\s*chunk\s*&&\s*"
            r"chunk->sdf_provenance\(\)\s*==\s*ChunkSdfProvenance::GeneratedCurrentParams\s*&&\s*"
            r"!chunk->needs_authority\(\)\s*&&\s*!chunk->is_voxel_data_dirty\(\)\)\s*"
            r"continue;\s*state.insert_chunk\(chunk\);")
        self.assertLess(save.index("m_worldSystem->sync_water_authority();"),
                        save.index("WorldStreamingState state;"))
        regression = read_source("test/common/GameSessionHeadlessWorld_test.cpp")
        for name in ("FirstEnabledChunkSaveIncludesCleanResidentRegions",
                     "IndexedFullSnapshotsIncludeCleanGeneratedWater",
                     "IndexedFullSnapshotsAdmitEmptyAndRejectMalformedCleanWater"):
            self.assertIn("TEST(GameSessionHeadlessWorldTest, " + name + ")", regression)

    def test_water_mutation_roundtrip_matches_saved_resolution_before_adoption(self):
        regression = read_source("test/common/GameSessionHeadlessWorld_test.cpp")
        body = regression.split(
            "TEST(GameSessionHeadlessWorldTest, GenuineWaterChangesAfterCleanInitializationPersistAndRoundTrip)",
            1)[1].split("\nTEST(", 1)[0]
        self.assertIn("mutation < 5", body)
        self.assertIn("water->ResizeSimulationGrid(*chunk, Luminumbra::Systems::WaterDetailLevel::High)",
                      body)
        self.assertIn("if (mutation != 4)", body)
        self.assertRegex(
            body,
            r"EnableSnapshotSession\(reopened, root, jobs\);\s*"
            r"reopened.SetWaterHighResEnabled\(mutation == 4\);\s*"
            r'ASSERT_TRUE\(reopened.CreateWorld\("Water mutation reload", "1337", "default"\)\);\s*'
            r"ASSERT_TRUE\(reopened.LoadWorldStateFrom\(session.GetWorldSaveDir\(\)\)\);")
        self.assertIn("EXPECT_TRUE(adopted->same_water_state(*chunk));", body)
        self.assertIn("EXPECT_EQ(reopened.ComputeChunkStateHash(), before_save);", body)

    def test_saved_json_depth_is_admitted_before_parsing_and_component_copying(self):
        admission = read_source("src/luminumbra_common/persistence/SavedJsonAdmission.h")
        traversal = admission.split("inline bool AdmitJsonValueDepth(", 1)[1].split("inline bool BoundedJsonDump(", 1)[0]
        for guard in ("value.is_binary()", "child.is_binary()", "value.is_discarded()", "child.is_discarded()"):
            self.assertIn(guard, traversal)
        reader = admission.split("inline bool ReadSavedJson(", 1)[1].split("// Lexical preflight", 1)[0]
        self.assertLess(reader.index("JsonDepthAllowed(bytes)"),
                        reader.index("nlohmann::json::parse(bytes,"))
        plant = admission.split("inline bool AdmitPlantComponentBytes(", 1)[1]
        self.assertIn("JsonDepthAllowed(bytes)", plant)
        entity = read_source("src/luminumbra_common/ecs/EntitySnapshot.h")
        loader = entity.split("inline bool LoadEntityRegistrySnapshotJson(", 1)[1]
        self.assertLess(loader.index("JsonDepthAllowed(json_text)"),
                        loader.index("nlohmann::json::parse(json_text)"))
        self.assertLess(loader.index("JsonDepthAllowed(json_text)"),
                        loader.index("entity.components.push_back("))
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        writer = self.function(source, "prepare_plant_entities", "WorldSaveService::")
        self.assertLess(writer.index("AdmitJsonValueDepth(component.data,"),
                        writer.index('values.push_back({{"type", component.type}'))

    def test_metadata_writer_and_catalog_share_one_limit_and_creation_is_session_owned(self):
        limits = read_source("src/luminumbra_common/persistence/SavedInputLimits.h")
        self.assertNotIn("kCatalogMetadataBytes", limits)
        catalog = read_source("src/luminumbra_common/persistence/SavedWorldCatalog.cpp")
        self.assertRegex(catalog, r"ReadSavedJson\(\s*metadata_path, SavedInputLimits::kMetadataBytes,")
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        writer = self.function(source, "save_metadata", "WorldSaveService::")
        self.assertNotIn("create_directories", writer)
        session = read_source("src/luminumbra_common/world/GameSession.cpp")
        reconcile = self.function(session, "ReconcileWorldStateMetadataTo")
        self.assertLess(reconcile.index("PrepareWorldMetadataTo("),
                        reconcile.index("create_directories("))
        self.assertLess(reconcile.index("create_directories("),
                        reconcile.index("WorldSaveService::save_metadata("))

    def test_checked_rename_uses_explicit_source_link_policy_on_both_platforms(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        rename = self.function(source, "rename_file_durably", "WorldSaveService::")
        self.assertIn("SavedInputFile input(source, SavedInputFile::LeafSymlinks::Follow)", rename)
        reader = read_source("src/luminumbra_common/persistence/SavedInputFile.h")
        self.assertIn("LeafSymlinks leaf_symlinks = LeafSymlinks::Refuse", reader)
        constructor = self.function(source, "SavedInputFile", "SavedInputFile::")
        self.assertEqual(constructor.count("leaf_symlinks == LeafSymlinks::Refuse"), 4)
        self.assertIn("m_missing = MissingSavedInputAncestorsAreValid(m_path);", constructor)
        self.assertIn("m_location_admitted = is_within_save_root();", constructor)
        location = self.function(source, "is_within_save_root", "SavedInputFile::")
        for verification in ("::GetFinalPathNameByHandleW(", "OpenResolvedSavedInput(resolved)",
                             "actual.st_dev == expected.st_dev", "actual.st_ino == expected.st_ino"):
            self.assertIn(verification, location)
        self.assertIn("!m_location_admitted", reader)
        self.assertLess(rename.index("input.read(contents, expected_bytes.size())"),
                        rename.index("ReclaimAbandonedStaging("))

    def test_synthetic_save_preserves_collision_precedence_and_write_diagnostics(self):
        source = read_source("src/luminumbra_common/persistence/SyntheticSaveFixture.cpp")
        writer = self.function(source, "WriteSyntheticSave", "SyntheticSaveReport ")
        self.assertLess(writer.index("save already exists"), writer.index("SavedInputFile input("))
        self.assertLess(writer.index("SavedInputFile input("), writer.index("fs::create_directories("))
        self.assertRegex(writer, r'write_file_durably\(\s*report.save_dir / "preset.json", '
                         r'preset_bytes, &save_errors\)')
        self.assertIn('save_errors.empty() ? "cannot persist preset" : save_errors.front()', writer)

    def test_preset_hash_encoding_and_existing_exact_limit_are_consistent(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        create = self.function(source, "PrepareWorldCreation")
        self.assertRegex(create, r'kPresetBytes - 1,\s*saved_preset_json,\s*-1,\s*false\)')
        compatible = self.function(source, "SavedPresetCompatible")
        self.assertNotIn("canonical.size()", compatible.split("if (has_preset)", 1)[0])
        self.assertIn("canonical.size() < Persistence::SavedInputLimits::kPresetBytes", compatible)
        persist = self.function(source, "PersistSavedPreset", "bool ")
        self.assertLess(persist.index("if (!missing)"), persist.index("canonical.size() >="))
        self.assertRegex(persist, r'write_file_durably\(\s*path, canonical \+ "\\n", nullptr, artifacts\)')

    def test_energy_writer_counts_normalized_pages_and_plants_publish_prepared_bytes(self):
        energy = read_source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        encode = self.function(energy, "SerializeRecord", "EnergyFieldState::")
        self.assertLess(encode.index("normalized.NormalizeAllPages()"),
                        encode.index("normalized.m_pages.size() > kEnergyWritePages"))
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        publish = self.function(source, "publish", "WorldSaveService::PreparedPlantWrite::")
        self.assertIn("WriteFileDurably(path, m_bytes, staging, errors)", publish)
        self.assertNotIn("prepare_plant_entities", publish)
        session = self.function(read_source("src/luminumbra_common/world/GameSession.cpp"),
                                "SaveWorldStateToImpl")
        self.assertIn("prepared_plants.publish(save_dir, &errors)", session)
        self.assertNotIn("save_plant_entities(", session)
        self.assertEqual(session.count("SerializeRecord("), 1)
        self.assertIn("write_file_durably(energy_path, energy_bytes)", session)
        self.assertLess(session.index("prepared_plants.prepare("),
                        session.index("prepared_chunks.publish("))

    def test_operation_refusal_cannot_change_current_session_usability(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        header = read_source("src/luminumbra_common/world/GameSession.h")
        self.assertIn("bool m_worldUsable = false;", header)
        self.assertIn("return m_worldOpenError;", header)
        reset = self.function(source, "ResetWorldSystems")
        self.assertIn("m_worldUsable = false;", reset)
        for preparation_name, adoption_name in (("PrepareWorldCreation", "AdoptCreatedWorld"),
                                                 ("PrepareWorldLoad", "AdoptLoadedWorld")):
            preparation = self.function(source, preparation_name)
            adoption = self.function(source, adoption_name)
            self.assertNotIn("ResetWorldSystems();", preparation)
            self.assertNotIn("m_worldUsable", preparation)
            self.assertIn("prepared->error =", preparation)
            self.assertLess(adoption.index("m_speciesTable ="),
                            adoption.index("m_worldUsable = true;"))
            self.assertIn("ResetWorldSystems();", adoption)
        creation = self.function(source, "AdoptCreatedWorld")
        metadata_failure = creation.split("if (!transient && !SaveWorldMetadataTo(GetWorldSaveDir(), artifacts))", 1)[1]
        self.assertLess(metadata_failure.index("m_worldUsable = false;"),
                        metadata_failure.index("return false;"))
        prepare = self.function(source, "PrepareWorldLoad")
        self.assertIn("ReadPreparedWorldState(worldPath, state_options, loaded, prepared->error, true, stop)",
                      prepare)
        self.assertLess(prepare.index("ReadPreparedWorldState("),
                        prepare.index("prepared->ready = true;"))
        for name in ("LoadWorld", "LoadWorldFrom"):
            body = self.function(source, name)
            self.assertIn("AdoptPreparedWorld(PrepareWorldLoad(", body)
            self.assertNotIn("m_saveRootPath =", body)
            self.assertNotIn("clear_world(", body)
            self.assertIn("!IsSimulationTickBoundary()", body)
        self.assertIn("options.save_root = save_root;", self.function(source, "LoadWorldFrom"))
        self.assertRegex(prepare, r"Persistence::InspectSavedWorld\(\s*options.save_root,\s*"
                         r"RuntimeRoot\(options.root_path\),\s*worldId,\s*stop,\s*"
                         r"Persistence::SavedWorldValidation::DeferredToPreparation\)")
        adoption = self.function(source, "AdoptLoadedWorld")
        self.assertLess(adoption.index("AdoptWorldStateFrom(worldPath, loaded);"),
                        adoption.index("m_worldUsable = true;"))
        self.assertIn("m_renderCalendar = loaded.render_calendar;", adoption)
        self.assertIn("m_worldClock = loaded.clock;", adoption)
        gate = self.function(source, "AdoptPreparedWorld")
        self.assertLess(gate.index("!prepared->ready || !m_worldOpenError.empty()"),
                        gate.index("AdoptLoadedWorld(*prepared)"))

    def test_persistence_uses_session_state_and_fatal_snapshot_refusals_latch_it(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        for name in ("SaveWorldMetadataTo", "SaveWorldStateToImpl",
                     "LoadWorldStateFrom"):
            with self.subTest(consumer=name):
                body = self.function(source, name)
                admission = body[:body.index("LoadedWorldState state;")] \
                    if name == "LoadWorldStateFrom" else body[:body.index("return false;")]
                self.assertIn("!m_worldUsable", admission)
                self.assertNotIn("m_worldOpenError", admission)
        wrapper = self.function(source, "SaveWorldStateTo")
        self.assertIn("return SaveWorldStateToImpl(", wrapper)
        readiness = self.function(source, "LoadWorldState")
        self.assertIn("return m_worldSystem && m_worldUsable;", readiness)
        snapshot = self.function(source, "LoadWorldStateFrom")
        # Fatal snapshot refusals invalidate; admission refuses before live state changes.
        failure = snapshot.split("if (!ReadWorldStateFrom(save_dir, state))", 1)[1]
        self.assertLess(failure.index("m_worldUsable = false;"),
                        failure.index("m_worldSystem->clear_world("))
        self.assertLess(failure.index("return false;"),
                        failure.index("AdoptWorldStateFrom(save_dir, state);"))
        self.assertEqual(snapshot.count("m_worldSystem->clear_world("), 1)
        self.assertIn("if (!state.input_refused)", failure)
        for name in ("LoadWorld", "LoadWorldFrom"):
            candidate = self.function(source, name)
            preparation = candidate.split("ResetWorldSystems();", 1)[0]
            self.assertNotIn("clear_world(", preparation)
            self.assertNotIn("m_worldUsable =", preparation)
            self.assertNotIn("m_lastLoadedChunkCount =", preparation)
        reader = self.function(source, "ReadPreparedWorldState")
        for name in ("PrepareWorldLoad", "PrepareWorldCreation", "PrepareWorldState", "ReadPreparedWorldState"):
            self.assertNotRegex(self.function(source, name), r"\bm_[A-Za-z]")
        wrapper = self.function(source, "ReadWorldStateFrom")
        self.assertRegex(wrapper, r"ReadPreparedWorldState\(\s*save_dir,\s*CaptureWorldPreparationOptions\(\),")
        self.assertRegex(wrapper, r"m_worldOpenError,\s*resetting_clock,\s*\{\}")
        self.assertIn("resetting_clock ? nullptr : m_energyFieldState.get()", wrapper)
        for validation in ("service.load_world(", "WorldSaveService::read_clock_metadata(",
                           "WorldSaveService::load_active_regions(", "options.scheduler_override",
                           "chunk->sdf_data.size() != kFullSdfLattice",
                           "ReadSavedEnergy(input, record, stop)",
                           "state.energy->DeserializeRecord(record, tick_base)"):
            self.assertIn(validation, reader)
        self.assertLess(reader.index("load_plant_entities("),
                        reader.index("service.load_world("))
        adoption = self.function(source, "AdoptWorldStateFrom")
        self.assertNotIn("load_plant_entities(", adoption)
        self.assertNotIn("aether_state.efs", adoption)
        self.assertIn("*m_energyFieldState = std::move(*saved.energy)", adoption)
        self.assertIn("ApplyPlantEntitySnapshot(m_registry, saved.plants)", adoption)
        self.assertNotIn("m_worldUsable", reader)
        self.assertNotIn("m_worldSystem->", reader)
        self.assertNotIn("m_renderCalendar =", reader)
        self.assertNotIn("RestoreWorldClock(", reader)
        self.assertNotIn("m_worldUsable = true;", snapshot)
        saving = self.function(source, "SaveWorldStateToImpl")
        self.assertLess(saving.index("ValidatePlantEntitySnapshot(plant_snapshot)"),
                        saving.index("prepared_chunks.publish("))
        self.assertLess(saving.index("ValidatePlantEntitySnapshot(plant_snapshot)"),
                        saving.index("prepared_plants.publish("))
        runner = read_source("src/luminumbra_server/ServerWorldRunner.cpp")
        full_snapshot = runner.split("ServerWorldRunner::SaveFullSnapshot()", 1)[1].split(
            "ServerWorldRunner::StreamedChunkCount()", 1)[0]
        self.assertNotIn("GetWorldOpenError()", full_snapshot)
        self.assertIn("m_session->SaveWorldStateTo(", full_snapshot)

    def test_normal_custom_validation_precedes_directory_and_uses_prepared_identity(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        creation = (self.function(source, "PrepareWorldCreation") +
                    self.function(source, "AdoptCreatedWorld"))
        directory = creation.index("fs::create_directories(worldPath.parent_path())")
        for validation in ("nlohmann::json::parse(*customPresetJson)",
                           "ResolveEffectivePreset(resolved, presetUse)",
                           "RuntimeRoot(options.root_path)",
                           "if (!preset.ok)",
                           "preset.preset_content_hash != requiredPresetContentHash"):
            self.assertLess(creation.index(validation), directory)
        self.assertIn('BoundedJsonDump(', creation)
        self.assertIn('saved_preset_json + "\\n"', creation)
        adoption = creation.split("ResetWorldSystems();", 1)[1]
        self.assertIn("m_presetContentHash = preset.preset_content_hash;", adoption)
        self.assertIn("m_speciesTable = preset.params.worldgen_dependencies->species_table;", adoption)
        self.assertNotIn("LoadTerrainPreset", adoption)


class FarVolumeAdmissionSourceContract(unittest.TestCase):
    """Guard bounded publication structure; native behaviour stays a native test."""

    def method(self, path, start, end):
        source = read_source("src/luminumbra_client/rendering/" + path)
        return source.split(start, 1)[1].split(end, 1)[0]

    def test_admission_uses_bounded_sorted_scratch_and_monotonic_free_cursors(self):
        for path, start, end in (
                ("FarVolumeHandoff.cpp", "bool FarVolumeHandoff::StageTarget(",
                 "void FarVolumeHandoff::Reject("),
                ("FarVolumeResidency.cpp", "bool FarVolumeResidency::ReplaceWanted(",
                 "void FarVolumeResidency::Reject(")):
            with self.subTest(path=path):
                body = self.method(path, start, end)
                if path == "FarVolumeHandoff.cpp":
                    start_body = self.method(
                        path, "FarHandoffAdmissionStatus FarVolumeHandoff::StartTarget(",
                        "FarHandoffAdmissionStatus FarVolumeHandoff::StartSessionTarget(")
                    self.assertNotIn("PrepareTarget(", start_body)
                    self.assertNotIn("std::sort(", start_body)
                    self.assertIn("pending->scratch.resize(manifest.tiles.size())", start_body)
                    prepare = self.method(
                        path, "FarWorldRefusal FarVolumeHandoff::PrepareStep(",
                        "FarWorldRefusal FarVolumeHandoff::AdmissionStep(")
                    self.assertNotRegex(prepare, r"\b(?:for|while)\s*\(")
                    self.assertNotIn("std::sort(", prepare)
                    self.assertIn("pending.scratch[pending.output++]", prepare)
                    self.assertIn("incoming[pending.cursor] = pending.scratch[pending.cursor]",
                                  prepare)
                    self.assertIn("pending.width >= count - pending.width ? count : 2 * pending.width",
                                  prepare)
                    self.assertIn("++admission_work_.key_comparisons", prepare)
                    pump = self.method(
                        path, "FarHandoffAdmissionPump FarVolumeHandoff::PumpAdmission(",
                        "bool FarVolumeHandoff::StageTarget(")
                    self.assertIn("while (result.work < budget && pending.tile < pending.count)",
                                  pump)
                    self.assertLess(pump.index("++result.work"),
                                    pump.index("AdmissionStep(pending)"))
                    self.assertNotIn("PrepareTarget(", pump)
                    step = self.method(
                        path, "FarWorldRefusal FarVolumeHandoff::AdmissionStep(",
                        "FarHandoffAdmissionPump FarVolumeHandoff::PumpAdmission(")
                    self.assertIn("return PrepareStep(pending)", step)
                    self.assertNotIn("std::sort(", body)
                    body = prepare + body
                    write = "incoming[count++]"
                else:
                    self.assertIn("std::sort(incoming.begin(), incoming.end()", body)
                    write = "tier.key_index[tier.entries.size() + count++]"
                self.assertIn(write, body)
                self.assertLess(body.index("count == tier.entries.size()"), body.index(write))
                self.assertIn("tier.order[index] == 0", body)
                self.assertIn("auto& cursor = next_free[number]", body)
                self.assertIn("entry = &tier.entries[cursor++]", body)
                self.assertIn("tier.queue.ForgetMatching", body)
                self.assertNotIn("queue.Forget(", body)
                self.assertNotRegex(body, r"std::(?:none_of|find_if|count_if)")
                self.assertNotRegex(body, r"for\s*\([^;]*;\s*j\s*<\s*(?:i|index)\b")

    def test_live_lookup_and_rebuild_have_no_hash_collision_chains(self):
        for owner in ("FarVolumeHandoff", "FarVolumeResidency"):
            with self.subTest(owner=owner):
                lookup = self.method(owner + ".cpp", "const " + owner + "::Entry* " +
                                     owner + "::Find(", owner + "::Entry* " + owner + "::Find(")
                self.assertIn("std::lower_bound", lookup)
                self.assertNotIn("for (", lookup)
                self.assertNotIn("while (", lookup)
                rebuild = self.method(owner + ".cpp", "void " + owner +
                                      "::Tier::RebuildIndex(", owner + "::" + owner + "(")
                self.assertIn("std::sort", rebuild)
                self.assertNotIn("while (", rebuild)
                self.assertIn("++work.key_comparisons", rebuild)
                drain = read_source("src/luminumbra_client/rendering/" + owner + ".cpp").split(
                    "void " + owner + "::CancelAndDrain()", 1)[1]
                self.assertIn("index_count = 0", drain)

    def test_batch_retirement_does_not_search_descriptors_per_selected_key(self):
        header = read_source("src/luminumbra_client/rendering/FarVolumeBuildQueue.h")
        batch = header.split("std::size_t ForgetMatching(", 1)[1].split(
            "bool Retry(", 1)[0]
        self.assertIn("noexcept(predicate", batch)
        self.assertIn("index < descriptors_.size()", batch)
        self.assertIn("ForgetAt(index)", batch)
        self.assertNotIn("Find(", batch)
        self.assertNotIn("Cancel(", batch)
        forget = self.method("FarVolumeBuildQueue.cpp", "void FarVolumeBuildQueue::ForgetAt(",
                             "bool FarVolumeBuildQueue::Retry(")
        self.assertIn("CancelAt(index)", forget)
        self.assertNotIn("Find(", forget)
        self.assertNotIn("Cancel(", forget)


def load_tests(loader, tests, pattern):
    tests.addTests(loader.discover(str(ROOT / "test/tools"),
                                   pattern="test_region_authority_wiring.py",
                                   top_level_dir=str(ROOT / "test/tools")))
    return tests


if __name__ == "__main__":
    unittest.main()
