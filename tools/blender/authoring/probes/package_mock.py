"""Deterministic, local-only mock extension archive; does not install or publish."""
from pathlib import Path
import shutil
import zipfile

from common import SOURCE


def build(directory):
    here = Path(__file__).resolve().parent
    staging = directory / "extension-source"
    staging.mkdir(parents=True)
    for name in ("__init__.py", "blender_manifest.toml"):
        shutil.copyfile(here / "extension" / name, staging / name)
    shutil.copyfile(SOURCE / "LICENSE", staging / "LICENSE")
    (staging / "worker").mkdir()
    for name in ("mock_service.py", "common.py", "contracts.py", "project_lock.py"):
        shutil.copyfile(here / name, staging / "worker" / name)
    target = directory / "luminumbra_author_mock-0.0.1.zip"
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(staging.rglob("*")):
            if path.is_file():
                info = zipfile.ZipInfo(path.relative_to(staging).as_posix(), (2026, 9, 7, 0, 0, 0))
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, path.read_bytes())
    return target
