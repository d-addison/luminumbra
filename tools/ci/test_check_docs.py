#!/usr/bin/env python3

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("check_docs.py")
SPEC = importlib.util.spec_from_file_location("check_docs", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class DocumentationLinkTests(unittest.TestCase):
    def test_accepts_existing_local_and_external_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "asset.css").write_text("", encoding="utf-8")
            (root / "index.html").write_text(
                '<link href="asset.css"><a href="https://example.com">external</a>',
                encoding="utf-8",
            )
            self.assertEqual(MODULE.broken_links(root), [])

    def test_rejects_missing_and_escaping_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "index.html").write_text(
                '<a href="missing.html">missing</a><img src="../private.png">',
                encoding="utf-8",
            )
            failures = MODULE.broken_links(root)
            self.assertEqual(len(failures), 2)


if __name__ == "__main__":
    unittest.main()
