#!/usr/bin/env python3

import hashlib
import importlib.util
import re
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


MODULE_PATH = Path(__file__).with_name("validate_public_tree.py")
SPEC = importlib.util.spec_from_file_location("validate_public_tree", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class PublicTreeValidatorTests(unittest.TestCase):
    # Escaped fixture tokens retain their runtime spelling while allowing these
    # first-party files to pass the same guard as the rest of the public tree.
    def setUp(self):
        self.production_digests = MODULE.AGENT_NAME_DIGESTS
        names = ("ExampleReviewer", "SyntheticReviewer", "ExampleModel-7", "ExampleTool")
        vocabulary = patch.object(MODULE, "AGENT_NAME_DIGESTS",
                                  frozenset(MODULE.name_digest(name) for name in names))
        vocabulary.start()
        self.addCleanup(vocabulary.stop)

    def test_numeric_relative_json_filename_keys_keep_other_checks(self):
        text = '{"Tests/Data/gap-\x3001.rml": {"sha256": "abc"}, "note": "-\x3001"}'
        findings = MODULE.inspect_text("cmake/source-map.json", text)
        self.assertEqual([(f.rule, f.line) for f in findings], [("internal-short-label", 1)] * 2)
        for text in ('{"Tests/Data/gap-\x3001.rml": {}}',
                     '{"Tests/Data/gap-\x3001-ref.rml": {}}',
                     '{"../gap-\x3001.rml": {}}', '{"/Tests/gap-\x3001.rml": {}}',
                     '{"Tests/../gap-\x3001.rml": {}}', '{"key": "Tests/Data/gap-\x3001.rml"}',
                     '{"Tests/Data/gap-\x3001": {}}', '{"Tests/Data/gap-\x3001.rml": "\x50R #123"}',
                     '{"note": "See Tests/Data/gap-\x3001.rml for details"}',
                     '{"Tests/Data/gap-\x3001.json": {}}'):
            with self.subTest(text=text):
                findings = MODULE.inspect_text("cmake/source-map.json", text)
                self.assertIn("internal-short-label", [f.rule for f in findings])
        findings = MODULE.inspect_text("cmake/source-map.json", '{"Tests/Data/gap-\x3001.rml": "\x50R #123"}')
        self.assertGreaterEqual(len({f.rule for f in findings}), 2)
        self.assertTrue(MODULE.inspect_text("docs/source-map.md", '{"Tests/Data/gap-\x3001.rml": {}}'))

    def test_name_digests_use_lowercase_sha256(self):
        expected = "1a4fb4a3d415efae3900bc8e222c89b80eb7e4cd925fd904cb4adaad31c36570"
        self.assertEqual(MODULE.name_digest("ExampleReviewer"), expected)
        self.assertEqual(MODULE.name_digest("EXAMPLEREVIEWER"), expected)
        self.assertEqual(hashlib.sha256(b"examplereviewer").hexdigest(), expected)
        self.assertTrue(self.production_digests)
        self.assertTrue(all(re.fullmatch(r"[0-9a-f]{64}", item)
                            for item in self.production_digests))
        self.assertNotIn(expected, self.production_digests)
        with patch.object(MODULE, "AGENT_NAME_DIGESTS", frozenset({expected})):
            self.assertTrue(MODULE.inspect_text("docs/example.md", "EXAMPLEREVIEWER approved."))
            self.assertEqual(MODULE.inspect_text("docs/example.md", "UnknownReviewer approved."), [])

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
        self.assertTrue(MODULE.inspect_path(".\x62\x61\x6e\x73\x6f/config.yaml"))
        self.assertTrue(MODULE.inspect_path("references/old.png"))
        self.assertTrue(MODULE.inspect_path("assets/audio/private.wav"))
        self.assertTrue(MODULE.inspect_path("build/release/game.exe"))
        self.assertTrue(MODULE.inspect_path(".tmp-private-notes.md"))

    def test_rejects_private_candidate_and_reviewer_provenance_in_docs(self):
        cases = (
            "A \x70rivate native package completed the run.",
            "A \x70rivate candidate passed.",
            "The \x70rivate package failed.",
            "Synthetic review of candidate `deadbee` completed.",
            "Candidate ABCDEF0123456789 was corrected.",
            "Native `0123abc` passed.",
            "The private `badcafe` fixture was inspected.",
            "A private\nnative package passed.",
            "Independent ExampleReviewer/xhigh review accepted the correction.",
            "SyntheticReviewer reviewed it.",
            "ExampleReviewer reviewed it.",
            "Independent ExampleReviewer source review accepted it.",
            "ExampleReviewer/xhigh accepted it.",
            "ExampleReviewer found the defect.",
            "Verification with actual ExampleReviewer passed.",
            "Optional ExampleTool adapters are planned.",
        )
        for path in ("docs/acceptance.md", "README.md"):
            for text in cases:
                with self.subTest(path=path, text=text):
                    self.assertTrue(MODULE.inspect_text(path, text))
        for text in ("A \x70rivate native package completed the run.", "SyntheticReviewer reviewed it."):
            self.assertTrue(MODULE.inspect_text("docs/catalog.json", text))
        findings = MODULE.inspect_text("docs/a.md", "Contract.\nA private\nnative package.")
        self.assertEqual([(f.rule, f.line) for f in findings], [("private-provenance", 2)])

    def test_accepts_public_contract_and_game_terms_without_provenance(self):
        text = (
            "The spawn anchor is the standing eye position regardless of stance.\n"
            "Native regression qualification remains NEEDS-NATIVE-RUN.\n"
            "A candidate world ID must be nonempty.\n"
            "SHA-256: `0000000000000000000000000000000000000000000000000000000000000000`.\n"
            "The ExampleReviewer screen lists discovered species; data/ui/examplereviewer.rml defines it.\n"
            "Game rules include photography/examplereviewer/objectives/difficulty.\n"
            "Use private multiplayer lobbies and public save-format documentation.\n"
            "Preview distribution is private; private previews require approval.\n"
            "Solar illumination uses an isolated simulation.\n"
        )
        self.assertEqual(MODULE.inspect_text("docs/acceptance.md", text), [])

    def test_reviewer_approval_provenance_uses_synthetic_names(self):
        for path, leader in (("docs/example.md", ""), ("src/example.cpp", "// "),
                             ("tools/example.ps1", "# ")):
            for text in ("ExampleReviewer approved.", "SyntheticReviewer approved the change.",
                         "\x52eview approved.", "The \x72eview was approved.", "\x52eview approved"):
                with self.subTest(path=path, text=text):
                    findings = MODULE.inspect_text(path, "\n" + leader + text)
                    self.assertEqual([(f.rule, f.line) for f in findings],
                                     [("internal-reviewer-provenance", 2)])
        for text in ("Review approved settings before loading.",
                     "The user approved the world name.",
                     "An approved review workflow can be selected.",
                     "The approved preset belongs to ExampleReviewer.",
                     "ExampleReviewer settings require user approval."):
            with self.subTest(text=text):
                self.assertEqual(MODULE.inspect_text("docs/example.md", text), [])

    def test_signature_attribution_uses_production_vocabulary(self):
        names = ("\x63\x6f\x64\x65\x78", "\x63\x6c\x61\x75\x64\x65")
        with patch.object(MODULE, "AGENT_NAME_DIGESTS", self.production_digests):
            for name in names:
                self.assertIn(MODULE.name_digest(name), self.production_digests)
                for dash in ("—", "--"):
                    for prefix in ("", "Public attribution "):
                        for path, leader in (("docs/example.md", ""), ("src/example.cpp", "// "),
                                             ("tools/example.ps1", "# ")):
                            text = f"{prefix}{dash} {name}."
                            with self.subTest(name=name, dash=dash, prefix=prefix, path=path):
                                findings = MODULE.inspect_text(path, "Contract.\r\n" + leader + text)
                                self.assertEqual([(f.rule, f.line) for f in findings],
                                                 [("internal-reviewer-provenance", 2)])
                                self.assertTrue(MODULE._may_name_attribution(text))

    def test_attribution_prefilter_covers_every_matcher_alternative(self):
        pattern = next(pattern for rule, pattern in MODULE.PROVENANCE_TEXT_RULES
                       if rule == "internal-reviewer-provenance" and pattern.groups)
        name = "ExampleTool"
        # Each alternative captures exactly one name; require coverage of every
        # capture so adding an alternative also requires extending this matrix.
        alternatives = (
            tuple(f"{name}/{tier}" for tier in ("low", "medium", "high", "xhigh", "max", "ultra")),
            tuple(f"{name} {context}" for context in (
                "review", "audit", "verification", "analysis", "model", "agent", "sign-off",
                "signed-off", "authoritative", "finding", "critique", "source review",
                "independent-code-agent-review")),
            tuple(f"{name} {verb}" for verb in (
                "reviewed", "audited", "verified", "accepted", "approved", "rejected", "found", "corrected")),
            tuple(f"{name} {reference}" for reference in ("#1", "# 2", "r3", "#\u0661", "r\u0662")),
            tuple(f"{context} {name}" for context in (
                "review", "reviewed", "audit", "audited", "verified", "verification", "review by",
                "audit with", "verification using", "review from", "audit the", "review by the actual")),
            tuple(f"{dash}{gap}{name}." for dash in ("—", "--") for gap in ("", " ")),
            (f"{name} adapter", f"{name} adapters"),
        )
        self.assertEqual(len(alternatives), pattern.groups)
        for group, passages in enumerate(alternatives, 1):
            for passage in passages:
                for separator in (" ", "\t", "\n", "\r\n"):
                    wrapped = passage.replace(" ", separator)
                    for text in (wrapped, wrapped.lower(), wrapped.upper()):
                        with self.subTest(group=group, text=text):
                            match = pattern.search(text)
                            self.assertIsNotNone(match)
                            self.assertEqual(match.lastindex, group)
                            self.assertIn(MODULE.name_digest(match[group]), MODULE.AGENT_NAME_DIGESTS)
                            self.assertTrue(MODULE._may_name_attribution(text))
                            self.assertTrue(MODULE._may_name_attribution(text, text.casefold()))

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
            ("private-provenance", "Synthetic \x70rivate native fixture."),
            ("candidate-provenance", "Synthetic package `deadbee` inspected."),
            ("candidate-provenance", "Synthetic candidate 0123abc inspected."),
            ("candidate-provenance", "Synthetic preview `deadbee` inspected."),
            ("candidate-provenance", "Synthetic delivered `deadbee` preview."),
            ("private-process-provenance", "Synthetic \x50ID 12345 was recorded."),
            ("private-process-provenance", "Synthetic \x70id: 12345 was recorded."),
            ("private-process-provenance", "Synthetic \x6fperator-closed hang."),
            ("private-process-provenance", "Synthetic \x6fperator stopped the process."),
            ("private-process-provenance", "Synthetic run \x63losed by the operator."),
        )
        for agent in ("ExampleReviewer", "SyntheticReviewer", "ExampleModel-7", "ExampleTool"):
            cases += (
                ("internal-reviewer-provenance", f"Synthetic {agent} review."),
                ("internal-reviewer-provenance", f"Synthetic {agent} source review."),
                ("internal-reviewer-provenance", f"Synthetic review by {agent}."),
                ("internal-reviewer-provenance", f"Synthetic {agent}-signed-off callback."),
                ("internal-reviewer-provenance", f"Synthetic {agent} finding A."),
                ("internal-reviewer-provenance", f"Synthetic {agent} #6."),
                ("internal-reviewer-provenance", f"Synthetic {agent} r2."),
                ("internal-reviewer-provenance", f"Synthetic attribution — {agent}."),
                ("internal-reviewer-provenance", f"Synthetic attribution -- {agent}."),
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
                    "The ExampleReviewer screen lists discovered species.\n"
                    "The pure presentation model the examplereviewer browse screen renders.\n"
                    "Solar illumination, ExampleReviewer terrain and ExampleTool scenery are game terms.\n"
                    "SyntheticReviewer::state lua; SyntheticReviewer::object value; // review SyntheticReviewer:: bindings\n"
                    "pid=123 is a synthetic transport fixture.\n"
                    'Forbidden local directory: ".\x62\x61\x6e\x73\x6f/".\n'
                )
                self.assertEqual(MODULE.inspect_text(path, leader + text), [])

    def test_wrapped_provenance_preserves_source_line_numbers(self):
        for path, leader in (("src/example.cpp", "// "), ("src/example.h", " * "),
                             ("CMakeLists.txt", "# "), ("tools/example.py", "# "),
                             ("tools/example.ps1", "# "), ("config/example.yaml", "# ")):
            for rule, passage in (
                ("candidate-provenance", ("Synthetic package", "`deadbee` inspected.")),
                ("private-provenance", ("Synthetic private", "native fixture.")),
                ("internal-reviewer-provenance", ("Synthetic ExampleReviewer", "source review.")),
                ("private-process-provenance", ("Synthetic run closed by", "the operator.")),
            ):
                text = "Public contract.\n" + "\n".join(leader + part for part in passage)
                if leader == " * ":
                    text = "/* " + text + "\n */"
                with self.subTest(path=path, rule=rule):
                    findings = [f for f in MODULE.inspect_text(path, text) if f.rule == rule]
                    self.assertEqual([f.line for f in findings], [2])

    def test_only_vendor_text_is_excluded(self):
        text = "# Synthetic ExampleReviewer review."
        self.assertEqual(MODULE.inspect_text("vendor/example.py", text), [])
        for path in ("tools/ci/test_validate_public_tree.py", "tools/ci/validate_public_tree.py",
                     "tools/ci/another_test.py"):
            with self.subTest(path=path):
                self.assertTrue(MODULE.inspect_text(path, text))


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

    def test_candidate_hash_after_cpp_digit_separators(self):
        for suffix in (".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx"):
            for number in ("1'000", "65'536", "1'000'000", "0xAB'CD", "0b10'01", "1.0'25e2"):
                for comment in ("// review of deadbee", "/* review of deadbee */"):
                    text = f"\nconstexpr auto budget = {number}; {comment}\n"
                    with self.subTest(suffix=suffix, number=number, comment=comment):
                        findings = MODULE.inspect_text("src/example" + suffix, text)
                        self.assertEqual([(f.rule, f.line) for f in findings],
                                         [("candidate-provenance", 2)])
        for text in ("auto c = '\\''; // review of deadbee",
                     "auto c = 'x'; // review of deadbee"):
            self.assertTrue(MODULE.inspect_text("src/example.cpp", text))
        self.assertEqual(MODULE.inspect_text("src/example.cpp",
            'constexpr auto budget = 65\'536; auto s = "// review of deadbee";'), [])

    def test_candidate_hash_after_yaml_plain_scalar_apostrophes(self):
        for suffix in (".yaml", ".yml"):
            for value in ("player's settings", "players' settings", "player's 'quoted' settings",
                          'player settings "review"', "https://example.test/player's-settings"):
                for separator in (" # review of deadbee", "\n# review of deadbee"):
                    text = "\ndescription: " + value + separator
                    line = 3 if separator.startswith("\n") else 2
                    with self.subTest(suffix=suffix, value=value, separator=separator):
                        findings = MODULE.inspect_text("config/example" + suffix, text)
                        self.assertEqual([(f.rule, f.line) for f in findings],
                                         [("candidate-provenance", line)])
            for text in ("description: 'player''s # review of deadbee'",
                         'description: "player\'s # review of deadbee"',
                         "description: player's#review of deadbee",
                         "values: [player's settings, '# review of deadbee']"):
                with self.subTest(suffix=suffix, text=text):
                    self.assertEqual(MODULE.inspect_text("config/example" + suffix, text), [])
            self.assertTrue(MODULE.inspect_text("config/example" + suffix,
                "description: 'player''s settings' # review of deadbee"))

    def test_yaml_scalar_properties_do_not_turn_quoted_data_into_comments(self):
        for suffix in (".yaml", ".yml"):
            for prefix in ("!!str", "!tag", "&a", "!tag &anchor", "&anchor !!str",
                           "!<tag:example.test,2026:scalar>", "!"):
                for value in ('"example # review of deadbee"',
                              "'player''s # review of deadbee'"):
                    text = f"\ndescription: {prefix} {value}"
                    with self.subTest(suffix=suffix, prefix=prefix, value=value):
                        self.assertEqual(MODULE.inspect_text("config/example" + suffix, text), [])
                        findings = MODULE.inspect_text("config/example" + suffix,
                                                       text + " # review of deadbee")
                        self.assertEqual([(f.rule, f.line) for f in findings],
                                         [("candidate-provenance", 2)])
                text = f"\ndescription: {prefix} player's settings # review of deadbee"
                findings = MODULE.inspect_text("config/example" + suffix, text)
                self.assertEqual([(f.rule, f.line) for f in findings],
                                 [("candidate-provenance", 2)])
        self.assertEqual(MODULE.inspect_text("config/example.yaml",
            'values: [!!str "example # review of deadbee", &a "example # review of deadbee"]'), [])

    def test_candidate_hash_excludes_data_manifests_and_inventories(self):
        for path in ("data/example.txt", "assets/example.md", "docs/assets/example.txt",
                     "config/test-vectors.json", "docs/catalog.json", "data/golden.csv",
                     "SHA256SUMS", "docs/checksums.txt", "docs/manifest.md",
                     "config/asset-inventory.yaml", "docs/sha256-manifest.txt"):
            with self.subTest(path=path):
                self.assertEqual(MODULE.inspect_text(path, "# review of deadbee: build deadbee"), [])

    def test_rejects_unfinished_and_internal_provenance(self):
        text = (
            "// \x54ODO \x53pec 42; \x54ASK #9; \x57ave B; \x50R #6; \x46R-A-\x3004; "
            "\x69teration-3; \x499-ECO; \x50illar B; \x413b\n"
            "// (\x423) storm rendering; \x64efect M6; ExampleReviewer C5; \x5418-Particle\n"
            "// \x3016-P1 extraction; (-\x3005) stale run\n"
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

    def test_rejects_numbered_pull_links_and_preserves_issue_links(self):
        for path in ("README.md", "docs/example.md", "src/example.cpp", "tools/example.py"):
            with self.subTest(path=path):
                findings = MODULE.inspect_text(path, "https://example.com/\x70ull/12")
                self.assertEqual([(finding.rule, finding.line) for finding in findings],
                                 [("numbered-pr-reference", 1)])
                self.assertEqual(MODULE.inspect_text(path, "https://example.com/issues/12"), [])

    def test_accepts_documented_pull_ref_placeholder_sentence(self):
        text = (
            "When a PR closes,\n"
            "`cache-cleanup.yml` deletes caches scoped to its `refs/pull/<n>/merge` ref without\n"
            "checking out PR code."
        )
        self.assertEqual(MODULE.inspect_text("docs/development.md", text), [])

    def test_accepts_generic_pull_refs_and_unrelated_words(self):
        for text in ("refs/pull/<n>/merge", "refs/pull/${{ github.event.number }}/merge",
                     "refs/pull/*/merge", "pullback/12", "lunar"):
            with self.subTest(text=text):
                self.assertEqual(MODULE.inspect_text("docs/example.md", text), [])

    def test_rejects_numbered_pull_refs_and_prose(self):
        for text in ("/\x70ull/123", "refs/\x70ull/123/merge", "\x50R #12", "\x70ull request 7"):
            with self.subTest(text=text):
                findings = MODULE.inspect_text("docs/example.md", text)
                self.assertEqual([(finding.rule, finding.line) for finding in findings],
                                 [("numbered-pr-reference", 1)])

    def test_development_documentation_has_no_numbered_pr_references(self):
        path = "docs/development.md"
        text = (MODULE_PATH.parents[2] / path).read_text(encoding="utf-8")
        findings = MODULE.inspect_text(path, text)
        self.assertEqual([finding for finding in findings
                          if finding.rule == "numbered-pr-reference"], [])

    def test_accepts_only_actual_index_and_tuple_member_tokens(self):
        for path, text in (
            ("src/example.h", "++counts[q.\x70hase - 1];\nreturn values[\x70hase - 1].source_frame;"),
            ("tools/example.ps1", "$_.Clauses[0].\x49tem1.Extent.Text.Trim()"),
        ):
            self.assertEqual(MODULE.inspect_text(path, text), [])

    def test_token_exemption_does_not_skip_same_line_findings(self):
        text = "auto x = counts[\x70hase - 1]; // \x70hase 2; \x70hase 3; \x50R160; \x50R161; \x41KIAABCDEFGHIJKLMNOP"
        findings = MODULE.inspect_text("src/example.cpp", text)
        self.assertEqual([f.detail for f in findings if f.rule == "implementation-history-reference"],
                         ["\x70hase 2", "\x70hase 3"])
        self.assertEqual([f.detail for f in findings if f.rule == "numbered-pr-reference"],
                         ["\x50R160", "\x50R161"])
        self.assertEqual(sum(f.rule == "aws-access-key" for f in findings), 1)
        ps = MODULE.inspect_text("tools/example.ps1", "$x.\x49tem1; # \x49tem2; \x49tem3; \x50R160")
        self.assertEqual([f.detail for f in ps if f.rule == "implementation-history-reference"],
                         ["\x49tem2", "\x49tem3"])

    def test_code_looking_comments_and_literals_remain_findings(self):
        cases = (
            ("src/x.cpp", '// counts[\x70hase - 1]'),
            ("src/x.cpp", '/* counts[\x70hase - 1] */'),
            ("src/x.cpp", 'const char* s = "counts[\x70hase - 1]";'),
            ("src/x.cpp", 'auto s = R"tag(\ncounts[\x70hase - 1]\n)tag";'),
            ("src/x.cpp", '// continued \\\ncounts[\x70hase - 1]'),
            ("tools/x.ps1", '# $x.\x49tem1'),
            ("tools/x.ps1", '<# outer <# inner #> $x.\x49tem1 #>'),
            ("tools/x.ps1", "'$x.\x49tem1'"),
            ("tools/x.ps1", "'backslash\\' + '$x.\x49tem1'"),
            ("tools/x.ps1", "@'\n$x.\x49tem1\n'@"),
            ("tools/x.ps1", '"$x.\x49tem1"'),
            ("docs/x.md", 'counts[\x70hase - 1]; $x.\x49tem1'),
        )
        for path, text in cases:
            with self.subTest(path=path, text=text):
                self.assertTrue(any(f.rule == "implementation-history-reference"
                                    for f in MODULE.inspect_text(path, text)))

    def test_exemptions_are_exact_tokens_and_keep_line_numbers(self):
        text = "auto a = counts[\x70hase - 1];\r\n// \x70hase 2; \x70hase 3\r\n"
        findings = MODULE.inspect_text("src/x.cpp", text)
        self.assertEqual([(f.detail, f.line) for f in findings], [("\x70hase 2", 2), ("\x70hase 3", 2)])
        self.assertTrue(MODULE.inspect_text("src/x.cpp", "auto a = counts[\x70hase - 10];"))
        self.assertTrue(MODULE.inspect_text("src/x.cpp", "// \x49tem1"))

    def test_rejects_private_paths_and_credentials(self):
        text = "/\x68ome/alice/project\nC:\\Users\\alice\\repo\nAKIAABCDEFGHIJKLMNOP\n"
        rules = {finding.rule for finding in MODULE.inspect_text("notes.txt", text)}
        self.assertEqual(
            rules,
            {"private-posix-path", "private-windows-path", "aws-access-key"},
        )

    def test_rejects_wsl_unc_paths_in_plain_and_json_escaped_form(self):
        for text in ("\\\\\x77sl.localhost\\Ubuntu\\x", '"\\\\\\\\\x77sl.localhost\\\\Ubuntu\\\\x"',
                     "//\x77sl.localhost/Ubuntu/x", "\\\\\x77sl$\\Ubuntu\\x"):
            with self.subTest(text=text):
                self.assertTrue(any(f.rule == "private-unc-path"
                                    for f in MODULE.inspect_text("notes.txt", text)))


if __name__ == "__main__":
    unittest.main()
