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

    def test_accepts_only_actual_index_and_tuple_member_tokens(self):
        for path, text in (
            ("src/example.h", "++counts[q.phase - 1];\nreturn values[phase - 1].source_frame;"),
            ("tools/example.ps1", "$_.Clauses[0].Item1.Extent.Text.Trim()"),
        ):
            self.assertEqual(MODULE.inspect_text(path, text), [])

    def test_token_exemption_does_not_skip_same_line_findings(self):
        text = "auto x = counts[phase - 1]; // phase 2; phase 3; PR160; PR161; AKIAABCDEFGHIJKLMNOP"
        findings = MODULE.inspect_text("src/example.cpp", text)
        self.assertEqual([f.detail for f in findings if f.rule == "implementation-history-reference"],
                         ["phase 2", "phase 3"])
        self.assertEqual([f.detail for f in findings if f.rule == "numbered-pr-reference"],
                         ["PR160", "PR161"])
        self.assertEqual(sum(f.rule == "aws-access-key" for f in findings), 1)
        ps = MODULE.inspect_text("tools/example.ps1", "$x.Item1; # Item2; Item3; PR160")
        self.assertEqual([f.detail for f in ps if f.rule == "implementation-history-reference"],
                         ["Item2", "Item3"])

    def test_code_looking_comments_and_literals_remain_findings(self):
        cases = (
            ("src/x.cpp", '// counts[phase - 1]'),
            ("src/x.cpp", '/* counts[phase - 1] */'),
            ("src/x.cpp", 'const char* s = "counts[phase - 1]";'),
            ("src/x.cpp", 'auto s = R"tag(\ncounts[phase - 1]\n)tag";'),
            ("src/x.cpp", '// continued \\\ncounts[phase - 1]'),
            ("tools/x.ps1", '# $x.Item1'),
            ("tools/x.ps1", '<# outer <# inner #> $x.Item1 #>'),
            ("tools/x.ps1", "'$x.Item1'"),
            ("tools/x.ps1", "'backslash\\' + '$x.Item1'"),
            ("tools/x.ps1", "@'\n$x.Item1\n'@"),
            ("tools/x.ps1", '"$x.Item1"'),
            ("docs/x.md", 'counts[phase - 1]; $x.Item1'),
        )
        for path, text in cases:
            with self.subTest(path=path, text=text):
                self.assertTrue(any(f.rule == "implementation-history-reference"
                                    for f in MODULE.inspect_text(path, text)))

    def test_exemptions_are_exact_tokens_and_keep_line_numbers(self):
        text = "auto a = counts[phase - 1];\r\n// phase 2; phase 3\r\n"
        findings = MODULE.inspect_text("src/x.cpp", text)
        self.assertEqual([(f.detail, f.line) for f in findings], [("phase 2", 2), ("phase 3", 2)])
        self.assertTrue(MODULE.inspect_text("src/x.cpp", "auto a = counts[phase - 10];"))
        self.assertTrue(MODULE.inspect_text("src/x.cpp", "// Item1"))

    def test_rejects_private_paths_and_credentials(self):
        text = "/home/alice/project\nC:\\Users\\alice\\repo\nAKIAABCDEFGHIJKLMNOP\n"
        rules = {finding.rule for finding in MODULE.inspect_text("notes.txt", text)}
        self.assertEqual(
            rules,
            {"private-posix-path", "private-windows-path", "aws-access-key"},
        )


if __name__ == "__main__":
    unittest.main()
