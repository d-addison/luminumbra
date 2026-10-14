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
        self.assertTrue(MODULE.inspect_path(".tmp-private-notes.md"))

    def test_rejects_private_candidate_and_reviewer_provenance_in_docs(self):
        cases = (
            "A private native package completed the run.",
            "A private candidate passed.",
            "The private package failed.",
            "Synthetic review of candidate `deadbee` completed.",
            "Candidate ABCDEF0123456789 was corrected.",
            "Native `0123abc` passed.",
            "The private `badcafe` fixture was inspected.",
            "A private\nnative package passed.",
            "Independent Astra/xhigh review accepted the correction.",
            "Sol reviewed it.",
            "Luna reviewed it.",
            "Independent Codex source review accepted it.",
            "Codex/xhigh accepted it.",
            "Codex found the defect.",
            "Verification with actual Codex passed.",
            "Optional Banso adapters are planned.",
        )
        for path in ("docs/acceptance.md", "README.md"):
            for text in cases:
                with self.subTest(path=path, text=text):
                    self.assertTrue(MODULE.inspect_text(path, text))
        for text in ("A private native package completed the run.", "Sol reviewed it."):
            self.assertTrue(MODULE.inspect_text("docs/catalog.json", text))
        findings = MODULE.inspect_text("docs/a.md", "Contract.\nA private\nnative package.")
        self.assertEqual([(f.rule, f.line) for f in findings], [("private-provenance", 2)])

    def test_accepts_public_contract_and_game_terms_without_provenance(self):
        text = (
            "The spawn anchor is the standing eye position regardless of stance.\n"
            "Native regression qualification remains NEEDS-NATIVE-RUN.\n"
            "A candidate world ID must be nonempty.\n"
            "SHA-256: `0000000000000000000000000000000000000000000000000000000000000000`.\n"
            "The Codex screen lists discovered species; data/ui/codex.rml defines it.\n"
            "Game rules include photography/codex/objectives/difficulty.\n"
            "Use private multiplayer lobbies and public save-format documentation.\n"
            "Preview distribution is private; private previews require approval.\n"
            "Solar illumination uses an isolated simulation.\n"
        )
        self.assertEqual(MODULE.inspect_text("docs/acceptance.md", text), [])

    def test_provenance_rules_cover_all_first_party_text_formats(self):
        paths_and_leaders = (
            ("docs/example.md", ""),
            ("docs/catalog.json", ""),
            ("src/example.c", "// "),
            ("src/example.cpp", "// "),
            ("src/example.cc", "// "),
            ("src/example.cxx", "// "),
            ("src/example.h", "// "),
            ("src/example.hpp", "// "),
            ("src/example.hxx", "// "),
            ("CMakeLists.txt", "# "),
            ("cmake/example.cmake", "# "),
            ("tools/example.py", "# "),
            ("tools/example.ps1", "# "),
            ("tools/example.psm1", "# "),
            (".github/workflows/example.yml", "# "),
            ("config/example.yaml", "# "),
        )
        cases = (
            ("private-provenance", "Synthetic private native fixture."),
            ("candidate-provenance", "Synthetic package `deadbee` inspected."),
            ("candidate-provenance", "Synthetic candidate 0123abc inspected."),
            ("candidate-provenance", "Synthetic preview `deadbee` inspected."),
            ("candidate-provenance", "Synthetic delivered `deadbee` preview."),
            ("private-process-provenance", "Synthetic PID 12345 was recorded."),
            ("private-process-provenance", "Synthetic pid: 12345 was recorded."),
            ("private-process-provenance", "Synthetic operator-closed hang."),
            ("private-process-provenance", "Synthetic operator stopped the process."),
            ("private-process-provenance", "Synthetic run closed by the operator."),
        )
        for agent in ("Codex", "Sol", "Luna", "Astra", "Banso"):
            cases += (
                ("internal-reviewer-provenance", f"Synthetic {agent} review."),
                ("internal-reviewer-provenance", f"Synthetic {agent} source review."),
                ("internal-reviewer-provenance", f"Synthetic review by {agent}."),
                ("internal-reviewer-provenance", f"Synthetic {agent}-signed-off callback."),
                ("internal-reviewer-provenance", f"Synthetic {agent} finding A."),
                ("internal-reviewer-provenance", f"Synthetic {agent} #6."),
                ("internal-reviewer-provenance", f"Synthetic {agent} r2."),
                ("internal-reviewer-provenance", f"Synthetic attribution — {agent}."),
            )
        for path, leader in paths_and_leaders:
            for rule, text in cases:
                with self.subTest(path=path, text=text):
                    findings = MODULE.inspect_text(path, leader + text)
                    if rule == "candidate-provenance" and path.endswith(".json"):
                        self.assertEqual(findings, [])
                    else:
                        self.assertIn(rule, {f.rule for f in findings})
            with self.subTest(path=path, text="public technical contracts"):
                text = (
                    "Package checksums and candidate world IDs must be nonempty.\n"
                    "package deadbe is an intentionally short example token.\n"
                    "candidate deadbeez is not a hexadecimal identifier.\n"
                    "The preview renders the candidate world without a package.\n"
                    "The owned PID must match the receipt's process ID.\n"
                    "The operator supplies the budget profile.\n"
                    "The operator closed-loop controller regulates speed.\n"
                    "The Codex screen lists discovered species.\n"
                    "The pure presentation model the codex browse screen renders.\n"
                    "Solar illumination, Luna terrain and Astra scenery are game terms.\n"
                    "sol::state lua; sol::object value; // review sol:: bindings\n"
                    "pid=123 is a synthetic transport fixture.\n"
                    'Forbidden local directory: ".banso/".\n'
                )
                self.assertEqual(MODULE.inspect_text(path, leader + text), [])

    def test_wrapped_provenance_preserves_source_line_numbers(self):
        for path, leader in (("src/example.cpp", "// "), ("src/example.h", " * "),
                             ("CMakeLists.txt", "# "), ("tools/example.py", "# "),
                             ("tools/example.ps1", "# "), ("config/example.yaml", "# ")):
            for rule, passage in (
                ("candidate-provenance", ("Synthetic package", "`deadbee` inspected.")),
                ("private-provenance", ("Synthetic private", "native fixture.")),
                ("internal-reviewer-provenance", ("Synthetic Codex", "source review.")),
                ("private-process-provenance", ("Synthetic run closed by", "the operator.")),
            ):
                text = "Public contract.\n" + "\n".join(leader + part for part in passage)
                if leader == " * ":
                    text = "/* " + text + "\n */"
                with self.subTest(path=path, rule=rule):
                    findings = [f for f in MODULE.inspect_text(path, text) if f.rule == rule]
                    self.assertEqual([f.line for f in findings], [2])

    def test_provenance_exclusions_are_exact(self):
        for path in ("tools/ci/test_validate_public_tree.py", "tools/ci/validate_public_tree.py",
                     "vendor/example.cpp"):
            self.assertEqual(MODULE.inspect_text(path, "// Synthetic Codex review."), [])
        self.assertTrue(MODULE.inspect_text("tools/ci/another_test.py", "# Synthetic Codex review."))

    def test_candidate_hash_review_and_build_attribution(self):
        findings = MODULE.inspect_text("test/CMakeLists.txt", "# review of deadbee: stable spawn anchor.")
        self.assertEqual([(f.rule, f.line) for f in findings], [("candidate-provenance", 1)])
        cases = (
            "review of deadbee: the anchor stays stable.",
            "reviewing `0123abc`: accepted.",
            "ABCDEF0123456789 review found a mismatch.",
            "Acceptance passed at `deadbee` after checking the anchor.",
            "The anchor at deadbee passed review.",
            "build deadbee completed the run.",
            "Reviewed at deadbee: accepted.",
            "audit of deadbee completed.",
            "verification against deadbee passed.",
            "commit deadbee supplied the package.",
            "revision deadbee completed qualification.",
            "deadbee preview was inspected.",
            "review of " + "a" * 40 + ": accepted.",
        )
        for path, leader in (("docs/acceptance.md", ""), ("README.md", ""),
                             ("test/CMakeLists.txt", "# "), ("src/x.cpp", "// "),
                             ("tools/x.py", "# "), ("tools/x.ps1", "# "),
                             ("config/x.yaml", "# ")):
            for passage in cases:
                with self.subTest(path=path, passage=passage):
                    findings = MODULE.inspect_text(path, "\n" + leader + passage)
                    self.assertTrue(any(f.rule == "candidate-provenance" and f.line == 2
                                        for f in findings))
        wrapped = "// review of\n// `deadbee`: the anchor stays stable."
        findings = MODULE.inspect_text("src/x.cpp", wrapped)
        self.assertEqual([(f.rule, f.line) for f in findings], [("candidate-provenance", 1)])
        for path, passage in (("src/x.cpp", 'int x = 0; /* review of deadbee */'),
                              ("src/x.cpp", 'int x = 0; // review of deadbee'),
                              ("tools/x.ps1", '<# review of deadbee #>'),
                              ("CMakeLists.txt", '#[=[ review of deadbee ]=]')):
            with self.subTest(path=path, passage=passage):
                self.assertTrue(any(f.rule == "candidate-provenance"
                                    for f in MODULE.inspect_text(path, passage)))

    def test_candidate_hash_does_not_match_public_hex_or_code_literals(self):
        cases = (
            ("test/golden.cpp", '// Golden pin: world_hash stays at d950a6afc12a5cdc.'),
            ("test/golden.cpp", '// Review world_hash at d950a6afc12a5cdc for stability.'),
            ("test/golden.cpp", '// Review world_hash at\n// d950a6afc12a5cdc for stability.'),
            ("test/golden.cpp", '// The hash revision 2fa007951a21e140 -> 0eac465289e7c88b is deliberate.'),
            ("test/vector.cpp", '// Test vector: input deadbee maps to output 0123abc.'),
            ("docs/hashes.md", 'The golden pin is `d950a6afc12a5cdc`; review its stability.'),
            ("docs/hashes.md", 'Test vectors: `deadbee`, `0123abc`, `ABCDEF0123456789`.'),
            ("docs/colours.md", 'Review the colour at #deadbeef and #abcdef.'),
            ("docs/hashes.md", 'SHA-256: `' + 'a' * 64 + '`.'),
            ("docs/hashes.md", 'review of deadbe: too short; review of deadbeez: non-hex.'),
            ("docs/hashes.md", 'review of ' + 'a' * 41 + ': too long.'),
            ("docs/hashes.md", 'A hash is stored at deadbee.\n\nAcceptance checks its type.'),
            ("docs/hashes.md", 'A hash is stored at deadbee. Acceptance checks its type.'),
            ("docs/hashes.md", 'A hash is stored at deadbee.\nAcceptance passed.'),
            ("docs/tools.md", 'The profile uses Blender 5.1.0 build `deadbee`.'),
            ("docs/tools.md", 'Blender build `deadbee` is the upstream tool pin.'),
            ("docs/schema.md", 'The official upstream schema uses tag `v2.3`, commit `deadbee`.'),
            ("docs/fixtures.md", 'The test/fixtures/smoke baselines were generated\nfrom devel commit `deadbee`.'),
            ("src/x.cpp", 'const char* vector = "review of deadbee";'),
            ("src/x.cpp", 'auto vector = R"tag(// review of deadbee)tag";'),
            ("tools/x.py", 'vector = "# review of deadbee"'),
            ("tools/x.py", 'vector = """\n# review of deadbee\n"""'),
            ("tools/x.ps1", '$vector = @\'\n# review of deadbee\n\'@'),
            ("CMakeLists.txt", 'set(vector [=[\n# review of deadbee\n]=])'),
            ("config/x.yaml", 'vector: "# review of deadbee"'),
        )
        for path, passage in cases:
            with self.subTest(path=path, passage=passage):
                self.assertEqual(MODULE.inspect_text(path, passage), [])

    def test_candidate_hash_excludes_data_manifests_and_inventories(self):
        for path in ("data/example.txt", "assets/example.md", "docs/assets/example.txt",
                     "config/test-vectors.json", "docs/catalog.json", "data/golden.csv",
                     "SHA256SUMS", "docs/checksums.txt", "docs/manifest.md",
                     "config/asset-inventory.yaml", "docs/sha256-manifest.txt"):
            with self.subTest(path=path):
                self.assertEqual(MODULE.inspect_text(path, "# review of deadbee: build deadbee"), [])

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

    def test_rejects_wsl_unc_paths_in_plain_and_json_escaped_form(self):
        for text in ("\\\\wsl.localhost\\Ubuntu\\x", '"\\\\\\\\wsl.localhost\\\\Ubuntu\\\\x"',
                     "//wsl.localhost/Ubuntu/x", "\\\\wsl$\\Ubuntu\\x"):
            with self.subTest(text=text):
                self.assertTrue(any(f.rule == "private-unc-path"
                                    for f in MODULE.inspect_text("notes.txt", text)))


if __name__ == "__main__":
    unittest.main()
