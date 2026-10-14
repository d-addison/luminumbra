#!/usr/bin/env python3
"""Contract tests for save_receipt.py. Run as: python3 tools/persistence/test_save_receipt.py"""

from __future__ import annotations

import contextlib
import errno
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    def test_boolean_aggregates_are_malformed_receipts(self) -> None:
        # Match the legitimate integer total/count so equality alone would accept the bool.
        for field in ("file_count", "total_bytes"):
            for value in (False, True):
                with self.subTest(field=field, value=value):
                    tree = self.base / f"{field}_{value}"
                    tree.mkdir()
                    if value:
                        _write(tree / "one-byte.bin", b"x")
                    receipt = save_receipt.snapshot(tree)
                    self.assertEqual(receipt[field], int(value))
                    receipt[field] = value
                    path = self.base / f"{field}_{value}.json"
                    path.write_text(json.dumps(receipt), encoding="utf-8")
                    with self.assertRaisesRegex(save_receipt.ReceiptError, field):
                        save_receipt.load_receipt(path)
                    code, _, err = _run(["verify", str(tree), str(path)])
                    self.assertEqual(code, save_receipt.EXIT_INVALID)
                    self.assertIn(field, err)
                    self.assertEqual(_run(["diff", str(path), str(path)])[0], 2)

    def test_non_integer_aggregates_are_malformed_receipts(self) -> None:
        for field in ("file_count", "total_bytes"):
            for value in (None, "1", 1.0):
                with self.subTest(field=field, value=value):
                    tree = self.base / "one-byte-tree"
                    _write(tree / "one-byte.bin", b"x")
                    receipt = save_receipt.snapshot(tree)
                    receipt[field] = value
                    path = self.base / "non-integer.json"
                    path.write_text(json.dumps(receipt), encoding="utf-8")
                    with self.assertRaisesRegex(save_receipt.ReceiptError, field):
                        save_receipt.load_receipt(path)

    def test_missing_directory_is_invalid_input(self) -> None:
        code, _, err = _run(
            ["snapshot", str(self.base / "missing"), "--out", str(self.base / "x.json")]
        )
        self.assertEqual(code, 2)
        self.assertTrue(err)

    def test_out_inside_tree_is_invalid_input(self) -> None:
        inside = self.tree / "receipt.json"
        code, _, err = _run(["snapshot", str(self.tree), "--out", str(inside)])
        self.assertEqual(code, 2)
        self.assertIn("outside", err)
        self.assertFalse(inside.exists())

    def test_unwritable_out_is_invalid_input(self) -> None:
        missing_parent = self.base / "no_such_dir" / "receipt.json"
        code, _, _ = _run(["snapshot", str(self.tree), "--out", str(missing_parent)])
        self.assertEqual(code, 2)

    def test_symlink_is_rejected(self) -> None:
        link = self.tree / "link.bin"
        try:
            os.symlink(self.tree / "b.bin", link)
        except OSError as error:
            if (error.errno not in (errno.EPERM, errno.EACCES, errno.ENOSYS, errno.EOPNOTSUPP)
                    and getattr(error, "winerror", None) not in (1, 50, 1314)):
                raise
            self.skipTest(f"Symlink creation is unavailable: {error}")
        with self.assertRaises(save_receipt.ReceiptError):
            save_receipt.snapshot(self.tree)
        code, _, _ = _run(["snapshot", str(self.tree), "--out", str(self.base / "link.json")])
        self.assertEqual(code, 2)

    def test_reparse_points_are_rejected_without_symlink_privilege(self) -> None:
        status = mock.Mock(st_mode=self.tree.stat().st_mode, st_file_attributes=0x400)
        with mock.patch.object(Path, "lstat", return_value=status):
            with self.assertRaises(save_receipt.ReceiptError):
                save_receipt.snapshot(self.tree)
        entry = mock.Mock()
        entry.name = "junction"
        entry.is_symlink.return_value = False
        entry.stat.return_value = mock.Mock(st_file_attributes=0x400)
        with mock.patch.object(save_receipt.os, "scandir") as scandir:
            scandir.return_value.__enter__.return_value = iter([entry])
            with self.assertRaisesRegex(save_receipt.ReceiptError, "reparse point"):
                save_receipt.snapshot(self.tree)
        entry.stat.assert_called_once_with(follow_symlinks=False)

    def test_drive_and_stream_receipt_paths_are_rejected(self) -> None:
        original = save_receipt.snapshot(self.tree)
        receipt_path = self.base / "unsafe.json"
        for path in ("C:/world.bin", "C:world.bin", "world.bin:stream", "worlds/C:world.bin"):
            with self.subTest(path=path):
                receipt = {**original, "files": [dict(item) for item in original["files"]]}
                receipt["files"][0]["path"] = path
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                with self.assertRaisesRegex(save_receipt.ReceiptError, "invalid path"):
                    save_receipt.load_receipt(receipt_path)

    def test_snapshot_refuses_stream_names_before_reading_file_contents(self) -> None:
        entry = mock.Mock()
        entry.name = "world.bin:stream"
        with mock.patch.object(save_receipt.os, "scandir") as scandir:
            scandir.return_value.__enter__.return_value = iter([entry])
            with self.assertRaisesRegex(save_receipt.ReceiptError, "non-portable path"):
                save_receipt.snapshot(self.tree)
        entry.stat.assert_not_called()


if __name__ == "__main__":
    unittest.main()
