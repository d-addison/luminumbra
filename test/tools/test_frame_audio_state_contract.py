#!/usr/bin/env python3
"""Check that shipping frame audio uses the lifecycle-owned bookkeeping under test."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "src/luminumbra_client/app"


def read(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.DOTALL)


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1:end - 1]


class FrameAudioOwnershipContract(unittest.TestCase):
    def test_no_process_lifetime_mutable_history(self):
        source = read(APP / "FrameAudio.cpp")
        # Immutable probe/species tables can remain shared; histories cannot.
        for declaration in re.finditer(r"\bstatic\s+([^;]+);", source):
            self.assertRegex(declaration[1], r"^const\b")
        self.assertNotRegex(source, r"\bs_(?:stride|foot\w+|\w+Timer|sunUp)\b")
        self.assertIn("FrameAudioState audio;", read(APP / "ClientAppContext.h"))
        for name in ("UpdateFrameAudio", "UpdatePlayerFootsteps", "UpdateLivingWorldAudio"):
            self.assertRegex(source, rf"void {name}\(ClientAppContext& app,\s*FrameAudioState& audio,")
        for call in ("audio.PlayerFootstep(fp, deltaTime)", "audio.AdvanceTimers(deltaTime)",
                     "audio.BeginCreatureFrame()", "audio.CreatureFootfall(key, cur, kStride)"):
            self.assertIn(call, source)
        self.assertNotIn("app.audio.", source)
        for timer in ("env", "call", "sleep", "feed", "colony", "thunder"):
            self.assertIn(f"audio.{timer}Timer", source)
        self.assertIn("audio.sunUp", source)

    def test_world_lifecycle_and_sole_frame_caller_share_owner(self):
        source = read(ROOT / "src/luminumbra_client/main_client.cpp")
        self.assertRegex(source, r"auto gameSession = std::make_unique<Luminumbra::world::GameSession>\(\);"
                                r"\s*g_app.audio.Reset\(\);")
        self.assertRegex(source, r"UpdateFrameAudio\(g_app,\s*g_app.audio,")
        release = body(source, "auto release_player_controller =")
        self.assertLess(release.index("g_app.audio.Reset(audioManager.get());"),
                        release.index("if (g_playerController)"))
        replacement = body(source, "auto finish_world_replacement =")
        self.assertIn("gameSession->WorldRevision() != entry_world_revision", replacement)
        self.assertIn("release_player_controller();", replacement)
        self.assertIn("g_app.audio.Reset(audioManager.get());",
                      body(source, "auto finish_world_entry ="))
        for boundary in ("auto return_entry_to_menu =", "auto drain_entry ="):
            self.assertIn("release_player_controller();", body(source, boundary))
        # Every clear path resets directly or releases presentation in that scope.
        for clear in re.finditer(r"\b\w+->clear_world\([^;]*;", source):
            surrounding = source[max(0, clear.start() - 250):clear.end() + 250]
            self.assertTrue("g_app.audio.Reset(" in surrounding or
                            "release_player_controller();" in surrounding)

    def test_native_and_source_regressions_are_registered(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_executable(frame_audio_state_test audio/frame_audio_state_test.cpp)", cmake)
        self.assertIn("    frame_audio_state_test\n", cmake)
        self.assertIn("add_test(NAME FrameAudioOwnershipContract COMMAND", cmake)
        self.assertIn("test/tools/test_frame_audio_state_contract.py", cmake)
        paths = (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8")
        self.assertIn('"test/tools/test_frame_audio_state_contract.py",', paths)


if __name__ == "__main__":
    unittest.main()
