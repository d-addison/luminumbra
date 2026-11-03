"""Build an independently installable standard-library Python zip application."""
import argparse
from pathlib import Path
import tempfile
import zipapp
import shutil
import json
import sys

SOURCE_DIRECTORY = Path(__file__).parent / "luminumbra_author"
SERVICE_MODULES = ("__init__.py", "__main__.py", "contracts.py", "formats.py", "locking.py",
                   "prefab.py", "process.py", "service.py", "file_io.py", "meshgen_registry.py",
                   "meshgen_config.py", "meshgen_contracts.py", "meshgen_scripts.py", "bootstrap.py",
                   "meshgen_licence.py", "meshgen_backend.py", "meshgen_jobs.py", "meshgen_candidate.py", "meshgen_provenance.py",
                   "meshgen_worker.py", "meshgen_metrics.py")


def schema_resources():
    """Generate installed schema/default resources from the engine registry."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        from luminumbra_author.meshgen_contracts import packaged_schemas
        from luminumbra_author.meshgen_registry import defaults
        return {**packaged_schemas(), "meshgen.defaults.v1.json": defaults()}
    finally:
        sys.path.pop(0)


def build(destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        package = root / "luminumbra_author"
        package.mkdir()
        # Ship reviewed engine modules and generated schema resources only;
        # local candidates, provenance and game-owned workers are excluded.
        for name in SERVICE_MODULES:
            shutil.copyfile(SOURCE_DIRECTORY / name, package / name)
        schemas = root / "luminumbra_author" / "schemas"
        schemas.mkdir(exist_ok=True)
        for name, document in schema_resources().items():
            (schemas / name).write_text(json.dumps(document, sort_keys=True, indent=2, allow_nan=False) + "\n",
                                        encoding="utf-8")
        shutil.copyfile(Path(__file__).resolve().parents[4] / "LICENSE", root / "LICENSE")
        shutil.copyfile(Path(__file__).parent / "launch.py", root / "launch.py")
        (root / "__main__.py").write_text(
            "from luminumbra_author.__main__ import main\nraise SystemExit(main())\n")
        zipapp.create_archive(root, destination, interpreter="/usr/bin/env python3",
                              compressed=True)
    destination.chmod(destination.stat().st_mode | 0o111)
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    build(parser.parse_args().output)
