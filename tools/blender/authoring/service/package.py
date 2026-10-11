"""Build an independently installable standard-library Python zip application."""
import argparse
from pathlib import Path
import tempfile
import zipapp
import shutil


def build(destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        shutil.copytree(Path(__file__).parent / "luminumbra_author", root / "luminumbra_author",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copyfile(Path(__file__).resolve().parents[4] / "LICENSE", root / "LICENSE")
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
