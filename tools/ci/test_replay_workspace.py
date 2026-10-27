"""Source guards and controlled filesystem examples for replay test ownership.

These checks do not execute C++ or establish the cause of a native I/O failure.
The registered ReplayStreamTest cases exercise the recorder itself.
"""

from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source, marker):
    start = source.index("{", source.index(marker))
    depth = 1
    for index in range(start + 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(marker)


class ReplayWorkspaceContract(unittest.TestCase):
    def test_all_fixture_roots_reserve_and_retain_their_namespace(self):
        for source, helper, prefix in (
            ("LockstepSession", "LockstepTempDir", "lockstep_"),
            ("ReplayStream", "ReplayTempDir", "replay_"),
            ("lockstep_replay_hardening", "HardeningTempDir", "replay_harden_"),
        ):
            with self.subTest(source=source):
                code = read(f"test/common/{source}_test.cpp")
                helper_body = body(code, f"fs::path {helper}()")
                self.assertIn('static const Luminumbra::TestSupport::ScopedTempFile '
                              f'workspace("{prefix}", ".unused");', helper_body)
                self.assertIn("return workspace.Path().parent_path();", helper_body)
                self.assertNotIn("create_directories", helper_body)
                self.assertNotIn("LUMINUMBRA_TEST_ARTIFACT_DIR", helper_body)

    def test_reservation_failure_cannot_return_an_unowned_root(self):
        support = read("test/support/UniqueTempPath.h")
        reserve = body(support, "inline std::filesystem::path ReserveTempDirectory(")
        self.assertIn("attempt < kTempPathAttempts", reserve)
        self.assertRegex(reserve, r"if \(std::filesystem::create_directory\(candidate, error\)\)"
                         r"\s*\{\s*return candidate;")
        self.assertIn("return {};", reserve)
        allocate = body(support, "inline std::filesystem::path CreateUniqueTempDirectory("
                        "std::string_view prefix)")
        self.assertIn("if (directory.empty())", allocate)
        self.assertIn("throw std::runtime_error", allocate)

    def test_recorder_closes_workers_and_files_before_cleanup(self):
        source = read("src/luminumbra_common/replay/ReplayStream.cpp")
        queued = body(source, "void QueuedReplayRecorder::State::Close()")
        self.assertLess(queued.index("state.Stop();"), queued.index("state.sink->Close();"))
        bounded = body(source, "void BoundedReplayRecorder::Close()")
        self.assertLess(bounded.index("m_output.close();"),
                        bounded.index("fs::remove_all(m_directory, ec);"))
        opened = body(source, "bool BoundedReplayRecorder::Open(")
        self.assertIn("attempt < kSegmentTicks", opened)
        self.assertIn("if (fs::create_directory(candidate, ec))", opened)
        self.assertIn("m_directory = candidate;", opened)
        for diagnostic in ("cannot create parent", "cannot reserve directory",
                           "exhausted {} directory candidates"):
            self.assertIn(diagnostic, opened)
        self.assertEqual(opened.count("ec.message()"), 2)
        # Both filesystem failures log the OS error and repeat it in the refusal.
        self.assertEqual(opened.count("ec.value()"), 4)

    def test_every_open_refusal_is_bounded_and_names_its_step(self):
        source = read("src/luminumbra_common/replay/ReplayStream.cpp")
        opened = body(source, "bool BoundedReplayRecorder::Open(")
        refusals = opened.count("return Refused(std::snprintf(m_open_refusal.data(),")
        self.assertEqual(refusals, 6)
        self.assertEqual(opened.count("kOpenRefusalBytes,"), refusals)
        self.assertEqual(opened.count("return false;"), 0)
        for stage in ("unconfigured: ", "already-open: ", "header-budget: ", "create-parent: ",
                      "reserve-directory: ", "exhausted: "):
            self.assertEqual(opened.count(f'"{stage}'), 1, stage)
        # Paths and OS messages enter the refusal only through the bounded helpers.
        for stage in ("create-parent: ", "reserve-directory: "):
            refusal = opened.split(f'"{stage}', 1)[1].split(";", 1)[0]
            for helper in ("PathTailPrefix(", "PathTailLength(", "PathTail(",
                           "message.c_str()", "ec.category().name()", "ec.value()"):
                self.assertIn(helper, refusal, (stage, helper))
        self.assertIn("m_open_refusal_length = 0;", opened)
        header = read("src/luminumbra_common/replay/ReplayStream.h")
        self.assertIn("static constexpr std::size_t kOpenRefusalBytes = 256;", header)
        self.assertIn("std::array<char, kOpenRefusalBytes> m_open_refusal{};", header)
        queued = body(source, "bool QueuedReplayRecorder::State::Open(")
        self.assertIn('state.open_refusal = "already-open: recorder is open";', queued)
        self.assertIn('state.open_refusal = "unconfigured: Configure must succeed before Open";',
                      queued)
        self.assertIn("state.open_refusal = nullptr;", queued)
        session = body(read("src/luminumbra_common/net/LockstepSession.cpp"),
                       "bool LockstepSession::OpenReplayRecorder()")
        self.assertIn("const auto refusal = m_replay->OpenRefusal();", session)
        self.assertIn('m_status.replay_error = "recorder setup failed";', session)
        self.assertIn('m_status.replay_error.append(": ").append(refusal);', session)
        native = read("test/common/ReplayStream_test.cpp")
        self.assertIn("TEST_F(ReplayStreamTest, OpenRefusalsNameTheFailingStepAndStayBounded)",
                      native)
        self.assertIn("TEST(LockstepSessionTest, "
                      "RecorderSetupRefusalNamesTheFailingStepInReplayError)",
                      read("test/common/LockstepSession_test.cpp"))
        # Every fixture Open of a recorder reports the refusal in its assertion.
        # Declarations precede uses, so the latest declared type of a name wins.
        declared = {}
        recorders = {"BoundedReplayRecorder", "QueuedReplayRecorder"}
        checked = 0
        for line in native.splitlines():
            declaration = re.search(r"replay::(\w+) (\w+)[;({]", line)
            if declaration:
                declared[declaration.group(2)] = declaration.group(1)
            opened = re.search(r"ASSERT_TRUE\((\w+)\.Open\(", line)
            if opened and declared.get(opened.group(1)) in recorders:
                self.assertIn(f" << {opened.group(1)}.OpenRefusal();", line, line)
                checked += 1
        self.assertGreaterEqual(checked, 15)

    def test_late_desync_keeps_every_budget_and_artifact_assertion(self):
        case = body(read("test/common/LockstepSession_test.cpp"),
                    "TEST(LockstepSessionTest, "
                    "LateDesyncAfterSendRefusalSurvivesAnExhaustedReceiveBudget)")
        for token in (
            "{net::MessageType::Input, net::MessageType::Hash}",
            "for (const std::uint64_t divergence : {1u, 2u})",
            "EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);",
            "EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);",
            "EXPECT_FALSE(transport.State().closed);",
            "EXPECT_FALSE(session.Status().peer_disconnected);",
            "EXPECT_EQ(world.tick, 2u);",
            "EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);",
            "EXPECT_LE(transport.State().polls, net::kReceiveQueueFrameLimit);",
            "EXPECT_EQ(session.Status().desync_tick, divergence);",
            "EXPECT_EQ(session.Status().dump_path, dump.string());",
            "const auto contents = replay::ReadReplay(dump.string());",
            "ASSERT_TRUE(contents.has_value());",
            "ASSERT_EQ(contents->checkpoints.size(), divergence);",
            "EXPECT_EQ(contents->checkpoints.back().tick, divergence);",
            'EXPECT_EQ(contents->checkpoints.back().terrain, HashAt(world, divergence, "terrain"));',
        ):
            self.assertIn(token, case)

    def test_native_counterexamples_are_on_the_discovered_common_target(self):
        cmake = read("test/CMakeLists.txt")
        target = re.search(r"add_executable\(common_tests\s+(.*?)^\)", cmake, re.S | re.M)
        self.assertIsNotNone(target)
        self.assertIn("common/LockstepSession_test.cpp", target.group(1))
        self.assertIn("common/ReplayStream_test.cpp", target.group(1))
        self.assertIn("common/lockstep_replay_hardening_test.cpp", target.group(1))
        roster = re.search(r"set\(LUMINUMBRA_GTEST_TARGETS\s+(.*?)^\)", cmake, re.S | re.M)
        self.assertIsNotNone(roster)
        self.assertIn("common_tests", roster.group(1).split())
        self.assertIn("gtest_discover_tests", cmake)
        native = read("test/common/ReplayStream_test.cpp")
        for name in ("OccupiedRecorderCandidateRefusesWithoutDeletingTheOtherOwner",
                     "ClosingOneRecorderPreservesTheOtherRecordersWorkingFiles"):
            self.assertIn(f"TEST_F(ReplayStreamTest, {name})", native)

    def test_shared_candidate_file_is_a_deterministic_setup_failure(self):
        source = read("src/luminumbra_common/replay/ReplayStream.cpp")
        prefix = re.search(r'"(lockstep-recording-)"', source).group(1)
        with tempfile.TemporaryDirectory(prefix="replay-counterexample-") as root:
            candidate = Path(root) / f"{prefix}0"
            candidate.write_bytes(b"other owner")
            with self.assertRaises(FileExistsError):
                candidate.mkdir()
            self.assertEqual(candidate.read_bytes(), b"other owner")
            # Parent creation failure is independent of the header or dump name.
            with self.assertRaises((FileExistsError, NotADirectoryError)):
                (candidate / "child").mkdir(parents=True)

    def test_exclusive_reservations_separate_processes_and_preserve_other_owners(self):
        # Force equal candidate stems in two real Python processes. This checks
        # filesystem arbitration, not execution of the C++ reservation helper.
        program = """
from pathlib import Path
import json, sys
for attempt in range(64):
    candidate = Path(sys.argv[1]) / f"workspace_{attempt}"
    try:
        candidate.mkdir()
    except FileExistsError:
        continue
    (candidate / "owner").write_text(sys.argv[2], encoding="utf-8")
    print(json.dumps(str(candidate)))
    break
else:
    raise RuntimeError("reservation exhausted")
"""
        with tempfile.TemporaryDirectory(prefix="replay-ownership-") as root:
            paths = []
            for owner in ("first", "second"):
                result = subprocess.run([sys.executable, "-I", "-B", "-c", program, root, owner],
                                        check=True, capture_output=True, text=True)
                paths.append(Path(json.loads(result.stdout)))
            self.assertNotEqual(*paths)
            (paths[0] / "owner").unlink()
            paths[0].rmdir()
            self.assertEqual((paths[1] / "owner").read_text(encoding="utf-8"), "second")
            # Exhaustion leaves all existing owners untouched.
            for attempt in range(64):
                (Path(root) / f"workspace_{attempt}").mkdir(exist_ok=True)
            result = subprocess.run([sys.executable, "-I", "-B", "-c", program, root, "third"],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((paths[1] / "owner").read_text(encoding="utf-8"), "second")


if __name__ == "__main__":
    unittest.main()
