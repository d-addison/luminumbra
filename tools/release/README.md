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
directory. `--tracked --root` requires the repository's worktree root, inventories
tracked **checkout bytes**, rejects links/gitlinks and conflicts, and allows an
untracked sidecar. Subdirectories, Git metadata directories, and bare repositories
are refused. The root check uses Git's worktree status and relative prefix, so
MSYS Git and native Windows Python need not share an absolute path spelling.
This is not the source-release path: Git text normalization and export attributes
can make checkout files differ from the archive. Existing dormant binary jobs retain
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

## Publication gate record

Before tagging `vX.Y.Z`, qualify the final source revision and record each of the
seven pre-publication gates below in `docs/release-gates/vX.Y.Z.json`. Use schema
version 1, the unprefixed release version, the full 40-hex qualified source commit,
and the exact gate IDs in this order. For example, this **pending template refuses
publication**; replace each status and evidence only after that gate succeeds:

```json
{
  "schema_version": 1,
  "version": "X.Y.Z",
  "qualified_commit": "<full 40-hex qualified source commit>",
  "gates": [
    {"id": "required-ci-strict-promotion", "status": "pending", "evidence": ""},
    {"id": "final-revision-native-manual", "status": "pending", "evidence": ""},
    {"id": "ucrt64-120-second-guard", "status": "pending", "evidence": ""},
    {"id": "server-load-qualification", "status": "pending", "evidence": ""},
    {"id": "paired-performance-gates", "status": "pending", "evidence": ""},
    {"id": "independent-release-verification", "status": "pending", "evidence": ""},
    {"id": "owner-native-windows-smoke", "status": "pending", "evidence": ""}
  ]
}
```

These IDs follow the pre-publication requirements in the
[acceptance tag gate](../../docs/v0.3-acceptance.md#remaining-release-gates-and-limits).
Evidence must identify the results and coverage for the qualified revision, including
the applicable native/manual results, the unchanged 120-second UCRT64 guard,
server-load qualification, both performance gates, independent release verification,
and the owner's complete native Windows client/save/reload/server smoke. Set each
status to exactly `passed` and provide a nonempty evidence string, such as a stable
result location or issue reference. The validator checks the declaration's structure;
the evidence must substantiate the result. Pending native coverage stays pending.

Commit the completed record after qualification. The qualified commit must be an
ancestor of (or equal to) the tagged commit, and both commits must lie on `main`'s
first-parent history; their tree difference may contain only
that version's record. Any other source, data, configuration or workflow change
requires qualification again. The validator reads the committed blob, ignoring local
record edits. It refuses absent records, links, records larger than 64 KiB, malformed
JSON, duplicate JSON keys, wrong types or versions, extra fields, and missing,
duplicate, unknown or reordered gates. No v0.3.0 record is supplied while its gates
remain pending.

The record reaches `main` through the normal promotion with strict required CI. Choose
a first-parent `main` commit whose difference from the qualified first-parent revision
contains only the record, wait for all required checks on that exact commit, then
create its signed annotated `vX.Y.Z` tag. The release
workflow retains identity and public-tree checks, fetches `main` explicitly, and
requires both commits on `origin/main`'s first-parent history before creating any
archive. The history scan refuses more than 100,000 commits; a second-parent branch
commit cannot substitute for the promotion merge. It reads effective
`required_status_checks` rules for `main`, requires strict
promotion and at least one readable required context, and checks every context on
the tag commit. Check-run pages must be complete; the highest run ID for each name
must be `completed` with conclusion `success`. Missing, neutral, skipped, pending or
failed checks refuse publication, including a failing rerun after an older success.
API failures also refuse publication. The source job grants `checks: read` for check
runs; the rules endpoint uses repository metadata access. Other permissions are unchanged.

To evaluate captured API responses offline (Python standard library and Git):

```sh
python3 -I -B tools/release/validate_release_gates.py --version X.Y.Z \
  --tag-commit <40-hex tagged commit> --rules main-rules.json --checks check-pages.json
python3 -I -B tools/release/test_validate_release_gates.py
```

`main-rules.json` is the array from `gh api repos/OWNER/NAME/rules/branches/main`.
`check-pages.json` is the stream of concatenated JSON page objects from `gh api --paginate
repos/OWNER/NAME/commits/COMMIT/check-runs?filter=all&per_page=100` (quote the endpoint
in a shell). A single array containing those pages is also accepted. The validator
accepts check pages on stdin by default and bounds input
before decoding JSON: 1 MiB for rules and 16 MiB for check pages, with at most 32
nested containers in any input. An unreadable, malformed, oversized or incomplete
response fails closed. The checkout must match
the tagged commit and have an up-to-date `origin/main` reference.

Signed source publication is performed by the workflow after these prerequisites.
Independent release-download verification is a **post-publication** step using
`verify_release.py` below from a fresh directory on an operator machine. Neither
publication nor download verification is a pre-publication record entry. Keep the
download verification report as evidence after publication; do not declare it passed
in advance.

## Independent verification

`verify_release.py` checks a published source release from a fresh directory on an
operator machine, not on the release runner. It requires the tag to be annotated, to
point at the expected commit, and to be GitHub-verified; the release must be neither
a draft nor a prerelease and must contain exactly the archive, the SPDX document, and
`SHA256SUMS`. It downloads those assets, checks `SHA256SUMS`, regenerates the archive
with `git archive` and deterministic gzip, runs the package and SBOM validators, and
verifies the provenance and SBOM attestations with `gh attestation verify --source-digest
<commit>`. The certificate's `buildSignerURI` must equal
`https://github.com/<repo>/.github/workflows/release.yml@refs/tags/<tag>` exactly, its
`sourceRepositoryDigest` must equal the expected commit, and `buildSignerDigest` must
equal it when present. Malformed responses are reported as FAIL checks.

```sh
python3 tools/release/verify_release.py --repo OWNER/NAME --tag vX.Y.Z \
  --expected-commit <40-hex commit> --out verify-out \
  --python /path/to/sbom-venv/bin/python
python3 tools/release/verify_release.py ... --dry-run-plan   # print commands only
python3 tools/release/test_verify_release.py                 # offline, fake runner
```

The `--python` interpreter must have the pinned schema dependencies from
`requirements-sbom.txt` installed. The script exits 0 only when every check passes and
writes `verify-report.json` with the SHA-256 of each asset. The script refuses, before any
network work, an `--out` directory where `verify-report.json` already exists, including as
a symlink; use a fresh or empty directory.
