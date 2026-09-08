`generate_sbom.py` produces an SPDX 2.3 inventory of package files. It does not
resolve dependencies, identify component versions, or audit file licenses.
The project's declared license is MIT; bundled third-party notices still apply.
Per-file licenses and copyrights remain `NOASSERTION`.

For a source release, inventory the exact archive that will be attested:

```sh
export SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
python3 tools/release/generate_sbom.py --archive dist/luminumbra-0.3.0-source.tar.gz \
  --name luminumbra-source --version 0.3.0 --output dist/luminumbra-0.3.0-source.spdx.json
python3 -m pip install -r tools/release/requirements-sbom.txt
python3 tools/release/validate_sbom.py dist/luminumbra-0.3.0-source.spdx.json \
  --archive dist/luminumbra-0.3.0-source.tar.gz
```

The release workflow uses the exact signed tag's commit committer timestamp as
`SOURCE_DATE_EPOCH`. This is a reproducible timestamp convention, disclosed in
the document's creation comment, rather than a claim about the build wall clock.
For other uses, `--created YYYY-MM-DDThh:mm:ssZ` supplies an explicit UTC creation
time and overrides the environment; absent either input, the tool uses current
UTC. Invalid timestamps fail. Identical inputs and timestamps produce identical
JSON. The namespace includes the document name and SHA-256 of canonical JSON
(sorted keys, compact separators, ASCII escaping), excluding only the namespace
itself, so changes to file bytes, names, version, creation metadata, or profile
receive a new identity.

Each regular file has SHA-1 and SHA-256. The package verification code is SHA-1
of the concatenated, sorted lowercase file SHA-1 strings, without separators;
duplicate file contents contribute once per file. The sidecar is outside the
package, so no files are excluded. File paths retain the source archive prefix.
Directories contribute no file record. Links (including staged Windows junctions
and other reparse entries), special/sparse files, duplicate
archive names, traversal, backslashes, colons, and control characters fail rather
than being silently omitted or followed. Tar and compressed tar are supported; ZIP is not supported
by the source archive validator. Git submodule contents are not materialized by
`git archive` and are outside a source archive inventory.

`--root` inventories a staged directory and requires output outside that
directory. `--tracked --root` inventories tracked **checkout bytes**, rejects
links/gitlinks and conflicts, and allows an untracked sidecar. It is not the
source-release path: Git text normalization and export attributes can make
checkout files differ from the archive. Existing dormant binary jobs retain
their staged-directory inventory; exact binary archive membership is outside
the source validation gate. Binary release activation remains a separate gate.

The validator combines the [pinned official schema](spdx/README.md) with checks
for this tool's single-package profile: timestamps, SHA-1/SHA-256, unique IDs and
paths, complete `DESCRIBES`/`CONTAINS` relationships, package verification code,
content-derived namespace, and exact archive members and checksums. It is not a
general-purpose validator for every SPDX document shape. Its pinned Python
dependencies are release tooling only. Neither test command downloads anything:

```sh
python3 tools/release/test_sbom.py         # Python standard library and Git
python3 tools/release/test_sbom_schema.py  # requires requirements-sbom.txt
```

The tooling requires Python 3.10 or newer. Git fixture tests create temporary
repositories and remove inherited Git location overrides before running them.

The exact Python package pins are in `requirements-sbom.txt`. Their published
distribution metadata identifies these upstream projects and MIT licenses:

| Package | Version | Upstream |
| --- | --- | --- |
| jsonschema | 4.23.0 | [python-jsonschema/jsonschema](https://github.com/python-jsonschema/jsonschema) |
| attrs | 24.2.0 | [python-attrs/attrs](https://github.com/python-attrs/attrs) |
| jsonschema-specifications | 2023.12.1 | [python-jsonschema/jsonschema-specifications](https://github.com/python-jsonschema/jsonschema-specifications) |
| referencing | 0.35.1 | [python-jsonschema/referencing](https://github.com/python-jsonschema/referencing) |
| rpds-py | 0.20.0 | [crate-py/rpds](https://github.com/crate-py/rpds) |

These packages are installed in the release validator environment; they are not
vendored into the game or shipped as runtime requirements.

The requirements follow SPDX 2.3
[creation information](https://spdx.github.io/spdx-spec/v2.3/document-creation-information/),
[file information](https://spdx.github.io/spdx-spec/v2.3/file-information/), and
[package verification](https://spdx.github.io/spdx-spec/v2.3/package-information/#79-package-verification-code-field).
