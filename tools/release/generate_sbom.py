#!/usr/bin/env python3
"""Generate a compact SPDX 2.3 file inventory for source or staged binaries."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def files_for(root: Path, tracked: bool) -> list[Path]:
    if tracked:
        output = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
        return sorted(root / item.decode("utf-8", "surrogateescape")
                      for item in output.split(b"\0") if item)
    return sorted(path for path in root.rglob("*") if path.is_file())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--tracked", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    spdx_files = []
    for index, path in enumerate(files_for(root, args.tracked), 1):
        data = path.read_bytes()
        spdx_files.append({
            "SPDXID": f"SPDXRef-File-{index}",
            "fileName": path.relative_to(root).as_posix(),
            "checksums": [{"algorithm": "SHA256", "checksumValue": hashlib.sha256(data).hexdigest()}],
            "licenseConcluded": "NOASSERTION",
            "copyrightText": "NOASSERTION",
        })
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{args.name}-{args.version}",
        "documentNamespace": f"https://github.com/d-addison/luminumbra/sbom/{args.version}",
        "creationInfo": {"creators": ["Tool: luminumbra-generate-sbom"]},
        "packages": [{
            "name": args.name,
            "SPDXID": "SPDXRef-Package",
            "versionInfo": args.version,
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": True,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "MIT",
            "copyrightText": "NOASSERTION",
        }],
        "files": spdx_files,
        "relationships": [
            {"spdxElementId": "SPDXRef-Package", "relationshipType": "CONTAINS", "relatedSpdxElement": item["SPDXID"]}
            for item in spdx_files
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"SBOM: {len(spdx_files)} files -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
