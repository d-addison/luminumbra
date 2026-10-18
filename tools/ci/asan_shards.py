#!/usr/bin/env python3
"""Compatibility entry point for the shared CTest sharding implementation."""

import importlib.util
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location("ctest_shards", Path(__file__).with_name("ctest_shards.py"))
_shared = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_shared)
subprocess = _shared.subprocess


def partition(all_tests, eligible):
    return _shared.partition(all_tests, eligible, profile="asan")


def plan(directory, shard):
    return _shared.plan(directory, shard, profile="asan")


def run(directory):
    return _shared.run(directory, profile="asan")


def merge(directory, output):
    return _shared.merge(directory, output, profile="asan", evidence=False)


if __name__ == "__main__":
    raise SystemExit(_shared.main(default_profile="asan", legacy_evidence=True))
