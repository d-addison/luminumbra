#!/usr/bin/env python3

import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("validate_public_tree.py")
SPEC = importlib.util.spec_from_file_location("validate_public_tree", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class PublicTreeValidatorTests(unittest.TestCase):
    def test_accepts_source_and_semantic_placeholder_language(self):
        self.assertEqual(MODULE.inspect_path("src/example.cpp"), [])
        self.assertEqual(
            MODULE.inspect_text(
                "src/example.cpp",
                "// Serialization writes a placeholder then backpatches its length.\n",
            ),
            [],
        )

    def test_rejects_private_and_generated_paths(self):
        self.assertTrue(MODULE.inspect_path(".banso/config.yaml"))
        self.assertTrue(MODULE.inspect_path("references/old.png"))
        self.assertTrue(MODULE.inspect_path("assets/audio/private.wav"))
        self.assertTrue(MODULE.inspect_path("build/release/game.exe"))

    def test_rejects_unfinished_and_internal_provenance(self):
        text = (
            "// TODO Spec 42; TASK #9; Wave B; PR #6; FR-A-004; "
            "iteration-3; I9-ECO; Pillar B; A3b\n"
            "// (B3) storm rendering; defect M6; Codex C5; T18-Particle\n"
            "// 016-P1 extraction; (-005) stale run\n"
            "// P3.1d: replication\n"
        )
        rules = {finding.rule for finding in MODULE.inspect_text("src/example.cpp", text)}
        self.assertEqual(
            rules,
            {
                "unfinished-marker",
                "internal-spec-reference",
                "internal-task-reference",
                "internal-wave-reference",
                "numbered-pr-reference",
                "internal-requirement-id",
                "internal-project-id",
                "implementation-history-reference",
                "internal-leading-label",
                "internal-roadmap-reference",
                "internal-short-label",
            },
        )

    def test_accepts_technical_wave_and_image_format_terms(self):
        text = (
            "float waveStrength = 0.5f; // wave strength\n"
            "// Decode a binary P6 PPM image.\n"
            "enum class FarLodTier { F1, F2 };\n"
        )
        self.assertEqual(MODULE.inspect_text("src/example.cpp", text), [])

    def test_rejects_private_paths_and_credentials(self):
        text = "/home/alice/project\nC:\\Users\\alice\\repo\nAKIAABCDEFGHIJKLMNOP\n"
        rules = {finding.rule for finding in MODULE.inspect_text("notes.txt", text)}
        self.assertEqual(
            rules,
            {"private-posix-path", "private-windows-path", "aws-access-key"},
        )


if __name__ == "__main__":
    unittest.main()
