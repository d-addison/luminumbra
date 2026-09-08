#!/usr/bin/env python3
"""Validate the Luminumbra SPDX 2.3 file-inventory profile and exact tar contents."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tarfile
from pathlib import Path

from generate_sbom import archive_inventory, document_namespace, valid_path, validate_created


SCHEMA = Path(__file__).with_name("spdx") / "spdx-2.3.schema.json"
SCHEMA_SHA256 = "239208b7ac287b3cf5d9a9af23f9d69863971102a5e1587a27a398b43490b89b"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_schema(document: dict) -> None:
    # Imported only for the explicit release validator, never by ordinary tests.
    try:
        from jsonschema import Draft7Validator
    except ImportError as exc:
        raise ValueError("install tools/release/requirements-sbom.txt to validate the SPDX schema") from exc
    data = SCHEMA.read_bytes()
    require(hashlib.sha256(data).hexdigest() == SCHEMA_SHA256, "pinned SPDX schema checksum mismatch")
    schema = json.loads(data)
    Draft7Validator.check_schema(schema)
    errors = list(Draft7Validator(schema).iter_errors(document))
    if errors:
        error = errors[0]
        raise ValueError(f"SPDX schema: {'/'.join(map(str, error.absolute_path))}: {error.message}")


def validate_semantics(document: dict) -> None:
    """Check this generator's single-package profile beyond JSON Schema."""
    require(document["spdxVersion"] == "SPDX-2.3", "expected SPDX-2.3")
    require(document["dataLicense"] == "CC0-1.0", "expected CC0-1.0 dataLicense")
    require(document["SPDXID"] == "SPDXRef-DOCUMENT", "invalid document SPDXID")
    validate_created(document["creationInfo"]["created"])
    creators = document["creationInfo"]["creators"]
    require(bool(creators) and all(re.fullmatch(r"(?:Tool|Person|Organization): [^\r\n]+", item)
                                 for item in creators), "invalid creators")
    require(len(document["packages"]) == 1, "expected one inventoried package")
    package = document["packages"][0]
    require(package["filesAnalyzed"] is True, "expected filesAnalyzed=true")
    for item in (document["name"], package["name"], package["versionInfo"]):
        require(bool(item.strip()) and not any(ord(char) < 32 for char in item), "invalid name/version")
    require(document["name"] == f"{package['name']}-{package['versionInfo']}", "package/document name mismatch")
    require(package["downloadLocation"] == "NOASSERTION", "unexpected downloadLocation")
    require(package["licenseDeclared"] == "MIT", "unexpected project license declaration")
    require(package["licenseConcluded"] == "NOASSERTION"
            and package["copyrightText"] == "NOASSERTION", "unexpected package audit assertion")
    require(package.get("licenseInfoFromFiles", ["NOASSERTION"]) == ["NOASSERTION"],
            "unexpected package file-license assertion")
    require(not any(document.get(key) for key in ("snippets", "externalDocumentRefs", "hasExtractedLicensingInfos")),
            "unsupported content outside the file-inventory profile")

    ids = [document["SPDXID"], package["SPDXID"]]
    names, file_sha1s = set(), []
    for item in document["files"]:
        ids.append(item["SPDXID"])
        require(item["fileName"].startswith("./"), "fileName must start with ./")
        name = valid_path(item["fileName"][2:])
        require(name not in names, f"duplicate fileName: {name}")
        names.add(name)
        algorithms = [checksum["algorithm"] for checksum in item["checksums"]]
        require(sorted(algorithms) == ["SHA1", "SHA256"], f"expected one SHA1 and SHA256: {name}")
        for checksum in item["checksums"]:
            length = 40 if checksum["algorithm"] == "SHA1" else 64
            require(re.fullmatch(rf"[0-9a-f]{{{length}}}", checksum["checksumValue"]) is not None,
                    f"invalid {checksum['algorithm']} checksum: {name}")
            if checksum["algorithm"] == "SHA1":
                file_sha1s.append(checksum["checksumValue"])
        require(item["licenseConcluded"] == "NOASSERTION" and item["copyrightText"] == "NOASSERTION",
                f"unexpected file audit assertion: {name}")
        require(item.get("licenseInfoInFiles", ["NOASSERTION"]) == ["NOASSERTION"],
                f"unexpected file-license assertion: {name}")

    require(len(ids) == len(set(ids)), "duplicate SPDXID")
    require(all(re.fullmatch(r"SPDXRef-[A-Za-z0-9.-]+", item) for item in ids), "invalid SPDXID")
    expected = {(document["SPDXID"], "DESCRIBES", package["SPDXID"])}
    expected.update((package["SPDXID"], "CONTAINS", item["SPDXID"]) for item in document["files"])
    relationships = [(item["spdxElementId"], item["relationshipType"], item["relatedSpdxElement"])
                     for item in document["relationships"]]
    require(len(relationships) == len(set(relationships)), "duplicate relationship")
    require(all(source in ids and target in ids for source, _, target in relationships), "dangling relationship")
    require(set(relationships) == expected, "expected DOCUMENT DESCRIBES package and CONTAINS every file")

    verification = package.get("packageVerificationCode", {})
    require(not verification.get("packageVerificationCodeExcludedFiles"), "external SBOM must not exclude package files")
    digest = hashlib.sha1("".join(sorted(file_sha1s)).encode("ascii"), usedforsecurity=False).hexdigest()
    require(verification.get("packageVerificationCodeValue") == digest, "incorrect or missing packageVerificationCode")
    require(document["documentNamespace"] == document_namespace(document), "document namespace does not match content")


def validate_archive(document: dict, archive: Path) -> None:
    actual = {name: {item["algorithm"]: item["checksumValue"] for item in hashes}
              for name, hashes in archive_inventory(archive).items()}
    declared = {item["fileName"][2:]: {checksum["algorithm"]: checksum["checksumValue"]
                                     for checksum in item["checksums"]}
                for item in document["files"]}
    require(actual.keys() == declared.keys(), "archive/SBOM member set mismatch")
    for name in actual:
        require(actual[name] == declared[name], f"archive/SBOM checksum mismatch: {name}")


def no_duplicate_keys(pairs: list[tuple]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sbom", type=Path)
    parser.add_argument("--archive", type=Path, required=True, help="exact source tar/tar.gz to attest")
    args = parser.parse_args()
    try:
        document = json.loads(args.sbom.read_text(encoding="utf-8"), object_pairs_hook=no_duplicate_keys)
        validate_schema(document)
        validate_semantics(document)
        validate_archive(document, args.archive)
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError) as exc:
        print(f"SBOM validation: FAIL - {exc}", file=sys.stderr)
        return 1
    print(f"SBOM validation: PASS - SPDX 2.3 schema, inventory semantics, {len(document['files'])} archive files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
