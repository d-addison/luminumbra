#!/usr/bin/env python3
"""Contract tests for save_receipt.py. Run as: python3 tools/persistence/test_save_receipt.py"""

from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

import save_receipt


def _write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _populate(root: Path) -> None:
    _write(root / "world_info.json", b'{"name":"x"}\n')
    _write(root / "worlds" / "a.lmr", b"\x00\x01\x02")
    _write(root / "b.bin", b"bbbb")


def _run(argv: list) -> tuple:
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = save_receipt.main(argv)
    return code, out.getvalue(), err.getvalue()


class SaveReceiptContract(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self.tree = self.base / "tree"
        self.tree.mkdir()
        _populate(self.tree)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _receipt(self, name: str = "receipt.json") -> Path:
        path = self.base / name
        code, _, _ = _run(["snapshot", str(self.tree), "--out", str(path)])
        self.assertEqual(code, 0)
        return path

    def test_snapshot_is_deterministic_and_sorted(self) -> None:
        first = save_receipt.snapshot(self.tree)
        second = save_receipt.snapshot(self.tree)
        self.assertEqual(first, second)
        paths = [item["path"] for item in first["files"]]
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(first["schema"], "luminumbra.save_receipt.v1")
        self.assertEqual(first["file_count"], 3)
        self.assertEqual(first["total_bytes"], len(b'{"name":"x"}\n') + 3 + 4)
        self.assertEqual(paths, ["b.bin", "world_info.json", "worlds/a.lmr"])

    def test_verify_passes_then_fails_on_change(self) -> None:
        receipt = self._receipt()
        self.assertEqual(_run(["verify", str(self.tree), str(receipt)])[0], 0)

        target = self.tree / "worlds" / "a.lmr"
        target.write_bytes(b"\x00\x01\x03")
        code, out, _ = _run(["verify", str(self.tree), str(receipt)])
        self.assertEqual(code, 1)
        self.assertIn("changed: worlds/a.lmr", out)

        target.write_bytes(b"\x00\x01\x02")
        self.assertEqual(_run(["verify", str(self.tree), str(receipt)])[0], 0)

        _write(self.tree / "extra.txt", b"new")
        code, out, _ = _run(["verify", str(self.tree), str(receipt)])
        self.assertEqual(code, 1)
        self.assertIn("added: extra.txt", out)

        (self.tree / "extra.txt").unlink()
        (self.tree / "b.bin").unlink()
        code, out, _ = _run(["verify", str(self.tree), str(receipt)])
        self.assertEqual(code, 1)
        self.assertIn("removed: b.bin", out)

    def test_diff_lists_added_removed_changed(self) -> None:
        before = self._receipt("before.json")
        (self.tree / "b.bin").unlink()
        _write(self.tree / "c.bin", b"cc")
        _write(self.tree / "world_info.json", b"{}")
        after = self._receipt("after.json")

        code, out, _ = _run(["diff", str(before), str(after)])
        self.assertEqual(code, 1)
        self.assertIn("added: c.bin", out)
        self.assertIn("removed: b.bin", out)
        self.assertIn("changed: world_info.json", out)

        self.assertEqual(_run(["diff", str(before), str(before)])[0], 0)

    def test_malformed_receipt_is_invalid_input(self) -> None:
        bad = self.base / "bad.json"
        bad.write_text("{not json", encoding="utf-8")
        self.assertEqual(_run(["verify", str(self.tree), str(bad)])[0], 2)

        wrong = self.base / "wrong.json"
        wrong.write_text(json.dumps({"schema": "other.v9", "files": []}), encoding="utf-8")
        self.assertEqual(_run(["diff", str(wrong), str(wrong)])[0], 2)

        receipt = json.loads(self._receipt("ok.json").read_text(encoding="utf-8"))
        receipt["tree_digest"] = "0" * 64
        tampered = self.base / "tampered.json"
        tampered.write_text(json.dumps(receipt), encoding="utf-8")
        self.assertEqual(_run(["verify", str(self.tree), str(tampered)])[0], 2)

    def test_missing_directory_is_invalid_input(self) -> None:
        code, _, err = _run(
            ["snapshot", str(self.base / "missing"), "--out", str(self.base / "x.json")]
        )
        self.assertEqual(code, 2)
        self.assertTrue(err)

    def test_symlink_is_rejected(self) -> None:
        link = self.tree / "link.bin"
        try:
            os.symlink(self.tree / "b.bin", link)
        except OSError:
            self.skipTest("symlinks are not available on this host")
        with self.assertRaises(save_receipt.ReceiptError):
            save_receipt.snapshot(self.tree)
        code, _, _ = _run(["snapshot", str(self.tree), "--out", str(self.base / "link.json")])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
