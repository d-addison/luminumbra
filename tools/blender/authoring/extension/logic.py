"""Portable adapter state and serialization; this module never imports Blender."""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import uuid


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_name(".tmp-" + uuid.uuid4().hex)
    try:
        temporary.write_bytes(canonical(value))
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(path):
    with Path(path).open("rb") as stream:
        data = stream.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise ValueError("Adapter response exceeds 1 MiB")
    return json.loads(data)


@dataclass
class BuildState:
    revision: int = 0
    changed_at: float = 0
    dirty: bool = False
    busy: bool = False
    submitted_revision: int = -1
    published_revision: int = -1
    status: str = "Mark a collection, then build its geometry"
    generation: str = ""
    metrics: str = ""

    def changed(self, now):
        self.revision += 1
        self.changed_at = now
        self.dirty = True

    def due(self, now):
        return self.dirty and not self.busy and now - self.changed_at >= 0.5

    def submitted(self):
        if self.busy:
            raise ValueError("A geometry build is already running")
        self.busy = True
        self.dirty = False
        self.submitted_revision = self.revision
        self.status = "Exporting geometry snapshot"

    def completed(self, response):
        self.busy = False
        if self.submitted_revision != self.revision:
            self.dirty = True
            self.status = "Source changed; waiting to rebuild"
            return
        if not response.get("ok"):
            self.status = response.get("message", "Geometry build failed")
            return
        result = response["result"]
        self.status = result["status"].capitalize()
        if result["status"] == "succeeded":
            self.published_revision = self.revision
            self.generation = result["job_id"]
            outputs = result["outputs"]
            if "prefab.json" in outputs:
                self.metrics = f"{outputs['prefab.json']['nodes']:,} objects; {outputs['prefab.json']['materials']:,} materials"
            else:
                self.metrics = f"{outputs['asset.lmesh']['triangles']:,} triangles"
            self.metrics += f"; {sum(x['bytes'] for x in outputs.values()):,} compiled bytes"
        elif result.get("findings"):
            self.status = result["findings"][0]["message"]
