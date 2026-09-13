"""Build the optional geometry-author extension archive without publishing it."""
import argparse
from pathlib import Path
import zipfile


def build(target):
    here = Path(__file__).resolve().parent
    files = {name: here / "extension" / name for name in (
        "blender_manifest.toml", "__init__.py", "blender_asset.py", "logic.py", "broker.py", "export_worker.py",
        "recipes.py", "blender_recipes.py")}
    files["LICENSE"] = here.parents[2] / "LICENSE"
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, path in sorted(files.items()):
            info = zipfile.ZipInfo(name, (2026, 9, 7, 0, 0, 0))
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())
    return target


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    build(parser.parse_args().output)
