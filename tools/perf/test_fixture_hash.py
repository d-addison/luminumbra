#!/usr/bin/env python3
"""Self-test for the canonical fixture hash."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fixture_hash as module  # noqa: E402


def write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class FixtureHashTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_comment_key_edit_does_not_move_the_hash(self) -> None:
        # The case that made a documentation-only change unable to pass the gate.
        write(self.root, "common/systems.json", json.dumps({"_note": "EXPERIMENTAL", "a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "common/systems.json", json.dumps({"_note": "supported now", "a": 1}))
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_adding_or_removing_a_comment_key_does_not_move_the_hash(self) -> None:
        write(self.root, "a.json", json.dumps({"a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", json.dumps({"a": 1, "_why": "explanation"}))
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_nested_comment_keys_are_stripped(self) -> None:
        write(self.root, "a.json", json.dumps({"o": {"_c": "x", "v": 1}, "l": [{"_c": "y", "v": 2}]}))
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", json.dumps({"o": {"_c": "CHANGED", "v": 1}, "l": [{"_c": "Z", "v": 2}]}))
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_reindentation_and_key_order_do_not_move_the_hash(self) -> None:
        write(self.root, "a.json", '{"b":2,"a":1}')
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", '{\n  "a": 1,\n  "b": 2\n}\n')
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_a_real_value_change_moves_the_hash(self) -> None:
        # The property the gate actually depends on.
        write(self.root, "a.json", json.dumps({"a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", json.dumps({"a": 2}))
        self.assertNotEqual(before, module.fixture_hash(self.root))

    def test_a_non_json_change_moves_the_hash(self) -> None:
        (self.root / "t.ltex").write_bytes(b"\x01\x02")
        before = module.fixture_hash(self.root)
        (self.root / "t.ltex").write_bytes(b"\x01\x03")
        self.assertNotEqual(before, module.fixture_hash(self.root))

    def test_adding_and_renaming_files_move_the_hash(self) -> None:
        write(self.root, "a.json", json.dumps({"a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "b.json", json.dumps({"b": 1}))
        added = module.fixture_hash(self.root)
        self.assertNotEqual(before, added)
        (self.root / "b.json").rename(self.root / "c.json")
        self.assertNotEqual(added, module.fixture_hash(self.root))

    def test_malformed_json_is_hashed_raw_and_still_registers(self) -> None:
        write(self.root, "a.json", "{not valid json")
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", "{also not valid")
        self.assertNotEqual(before, module.fixture_hash(self.root))

    def test_byte_order_mark_is_tolerated(self) -> None:
        # Several fixtures under data/ carry a BOM.
        (self.root / "a.json").write_bytes(b"\xef\xbb\xbf" + json.dumps({"a": 1}).encode("utf-8"))
        with_bom = module.fixture_hash(self.root)
        (self.root / "a.json").write_bytes(json.dumps({"a": 1}).encode("utf-8"))
        self.assertEqual(with_bom, module.fixture_hash(self.root))

    def test_hash_is_stable_across_calls_and_shaped_for_the_validator(self) -> None:
        write(self.root, "a.json", json.dumps({"a": 1}))
        first = module.fixture_hash(self.root)
        self.assertEqual(first, module.fixture_hash(self.root))
        # perf.py requires [0-9a-f]{40,64} for a fixture hash.
        self.assertEqual(len(first), 64)
        self.assertTrue(all(c in "0123456789abcdef" for c in first))

    def test_server_scope_omits_scatter_but_default_detects_its_values(self) -> None:
        write(self.root, "systems.json", '{"water":true}')
        write(self.root, "foliage/scatter_set.json", '{"height":0.44}')
        full = module.fixture_hash(self.root)
        server = module.fixture_hash(self.root, scope="headless-server")
        write(self.root, "foliage/scatter_set.json", '{"height":0.20}')
        self.assertNotEqual(full, module.fixture_hash(self.root))
        self.assertEqual(server, module.fixture_hash(self.root, scope="headless-server"))

    def test_server_scope_omits_only_the_named_precipitation_descriptors(self) -> None:
        for relative in ("particles/precip_rain.json", "particles/precip_snow.json"):
            write(self.root, relative, '{"size":0.1}')
            full = module.fixture_hash(self.root)
            server = module.fixture_hash(self.root, scope="headless-server")
            write(self.root, relative, '{"size":0.02}')
            self.assertNotEqual(full, module.fixture_hash(self.root))
            self.assertEqual(server, module.fixture_hash(self.root, scope="headless-server"))
        before = module.fixture_hash(self.root, scope="headless-server")
        write(self.root, "particles/another.json", '{"size":0.02}')
        self.assertNotEqual(before, module.fixture_hash(self.root, scope="headless-server"))

    def test_server_scope_keeps_simulation_and_other_foliage_changes(self) -> None:
        for relative in ("systems.json", "foliage/species/wheat.json", "foliage/new_species.json"):
            with self.subTest(relative=relative):
                write(self.root, relative, '{"growth_rate":1}')
                before = module.fixture_hash(self.root, scope="headless-server")
                write(self.root, relative, '{"growth_rate":2}')
                self.assertNotEqual(before, module.fixture_hash(self.root, scope="headless-server"))
                before = module.fixture_hash(self.root, scope="headless-server")
                write(self.root, relative, '{malformed')
                self.assertNotEqual(before, module.fixture_hash(self.root, scope="headless-server"))

    def test_server_scope_still_detects_added_renamed_and_removed_files(self) -> None:
        before = module.fixture_hash(self.root, scope="headless-server")
        write(self.root, "foliage/new.json", '{"growth_rate":1}')
        added = module.fixture_hash(self.root, scope="headless-server")
        self.assertNotEqual(before, added)
        (self.root / "foliage/new.json").rename(self.root / "foliage/renamed.json")
        renamed = module.fixture_hash(self.root, scope="headless-server")
        self.assertNotEqual(added, renamed)
        (self.root / "foliage/renamed.json").unlink()
        self.assertEqual(before, module.fixture_hash(self.root, scope="headless-server"))

    def test_scopes_have_distinct_identities_even_without_scatter_file(self) -> None:
        write(self.root, "systems.json", '{"water":true}')
        self.assertNotEqual(
            module.fixture_hash(self.root),
            module.fixture_hash(self.root, scope="headless-server"),
        )

    def test_cli_passes_scope_and_preserves_default(self) -> None:
        write(self.root, "systems.json", '{"water":true}')
        for scope_args, expected_scope in (([], "all"), (["--scope", "headless-server"], "headless-server")):
            with self.subTest(scope=expected_scope), contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(module.main(["fixture_hash.py", str(self.root), *scope_args]), 0)
                self.assertEqual(output.getvalue().strip(), module.fixture_hash(self.root, scope=expected_scope))

    def test_unknown_scope_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            module.fixture_hash(self.root, scope="typo")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            module.main(["fixture_hash.py", str(self.root), "--scope", "typo"])
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
