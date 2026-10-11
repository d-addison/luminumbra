"""Shared utilities for isolated CPU probes; no engine or Blender dependency."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import uuid

ROOT = Path(__file__).resolve().parents[1]
# The embedded worker uses only the JSON helpers. Fixture/build tools can inspect
# an explicitly selected, independent engine checkout without writing into it.
SOURCE = Path(os.environ.get("LUMINUMBRA_AUTHOR_SOURCE", ROOT / "../../..")).resolve()
PIN = "657c731f19db95b8b127ba418951adb64483adc3"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def parse_json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result
    def invalid(value):
        raise ValueError(f"nonfinite JSON number: {value}")
    return json.loads(data, object_pairs_hook=unique, parse_constant=invalid)


def read_json(path):
    return parse_json(Path(path).read_text(encoding="utf-8"))


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temp.open("xb") as stream:
            stream.write(canonical(value) + b"\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, SOURCE / relative)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result
