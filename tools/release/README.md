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

The release sequence is: six pre-publication gates in order, draft release
creation and any enabled uploads, owner review of the gate record's referenced
evidence and manual publication, then independent download verification after
publication.

Before tagging `vX.Y.Z`, qualify the final source revision and record each of the
six pre-publication gates below in `docs/release-gates/vX.Y.Z.json`. Use schema
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
    {"id": "owner-native-windows-smoke", "status": "pending", "evidence": ""}
  ]
}
```

These IDs follow the pre-publication requirements in the
[acceptance tag gate](../../docs/v0.3-acceptance.md#remaining-release-gates-and-limits).
Evidence must identify the results and coverage for the qualified revision, including
the applicable native/manual results, the unchanged 120-second UCRT64 guard,
server-load qualification, both performance gates,
and the owner's complete native Windows client/save/reload/server smoke. Set each
status to exactly `passed`. Every gate, including each native gate, requires an
evidence string of exactly two whitespace-separated fields: the full
`qualified_commit` SHA and a concrete record URL. The SHA comparison ignores
hex case. Supported URLs are:

- `https://github.com/OWNER/REPO/actions/runs/RUN_ID`, optionally followed by
  `/artifacts/ARTIFACT_ID`;
- `https://github.com/OWNER/REPO/issues/ISSUE_NUMBER#issuecomment-COMMENT_ID`.

All numeric URL IDs must be positive decimal integers. For example, use
`<full-qualified-sha> https://github.com/OWNER/REPO/actions/runs/123456` after
replacing the SHA and location with the actual result. Bare issue references,
generic issue URLs, placeholder text, abbreviated or different SHAs, local paths
and extra fields refuse draft creation. These offline evidence-syntax checks are
a fail-closed precondition; they do not fetch URLs or inspect record contents.
The workflow creates a **draft** GitHub release with `gh release create --draft`
on both the notes-file and generated-notes paths. Before publication, the repository
owner must review every gate record's referenced evidence, verify that it exists
and substantiates the qualification results and coverage on the qualified revision,
and manually publish the draft. That evidence review and manual publication are
the approval gate; passing syntax checks does not establish qualification or owner
approval. Keep evidence available for review, including archiving expiring CI
artifacts. Pending native coverage stays pending.

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
`required_status_checks` rules for `main`, requires strict promotion and at least
one readable ruleset context, and also fetches `repos/OWNER/NAME/branches/main`
for classic branch protection. When `protected` is true, `protection.enabled` is
true and `protection.required_status_checks.enforcement_level` is not `off`,
classic `contexts` and `checks` are merged with the ruleset requirements.
Each rule's optional `integration_id` and each classic check's `app_id` pin the
context to a GitHub App when the ID is a positive integer. Null or absent IDs are
unpinned; classic `app_id: -1` also allows any app and is normalized to unpinned.
Booleans, non-integers, zero and other negative IDs are refused; ruleset
`integration_id: -1` is also refused.
Legacy classic contexts absent from `checks` are unpinned; a same-named legacy
context does not erase a check's app binding. Malformed bindings refuse draft
creation. Distinct bindings for the same context are all required. Disabled classic
protection or enforcement `off` adds no contexts, including when `protected` is
true because of rulesets alone. The branch response must have boolean `protected`;
while protected it must also have boolean `protection.enabled` and a
`required_status_checks` object with a valid `enforcement_level` (`off`, `non_admins`
or `everyone`), string `contexts` array and `checks` array containing contexts with
optional app IDs. Missing or malformed required fields fail closed even when
classic checks are disabled.
An unprotected branch adds no classic requirements; the strict ruleset requirement
still applies. Check-run and commit-status pages must both be complete. For each
required context, the highest check-run ID, considering only runs whose `app.id`
matches a pinned binding, must be `completed` with conclusion `success` if present.
The highest commit-status ID for the same context must have state `success` if
present, regardless of its creator. Selection is independent of response order;
older failures superseded by newer successes do not block publication. When both
APIs report the context, both latest results must succeed. An unpinned context
can pass with a successful status alone or a successful check run alone; neither
result being present refuses publication. Neutral, skipped, pending or failed
results refuse publication, including a failing rerun after an older success.
An unpinned context retains latest-by-name check-run selection across apps.
A successful run from another app cannot replace a failed or missing pinned run.
Commit-status creator IDs identify users rather than GitHub Apps, so statuses
cannot establish an app binding: pinned contexts require a matching successful
check run even when a successful status exists. A latest failing same-named
status from any creator still blocks a pinned context.
API failures also refuse publication. The source job grants `checks: read` for check
runs and `statuses: read` for commit statuses; the rules endpoint uses repository
metadata access. The branch endpoint needs only `contents: read`, already covered
by the source job's `contents: write`. Other permissions are unchanged.

To evaluate captured API responses offline (Python standard library and Git):

```sh
python3 -I -B tools/release/validate_release_gates.py --version X.Y.Z \
  --tag-commit <40-hex tagged commit> --rules main-rules.json \
  --branch main-branch.json --checks check-pages.json --statuses status-pages.json
python3 -I -B tools/release/test_validate_release_gates.py
```

`main-rules.json` is the concatenated HTTP headers and JSON arrays from
`gh api --include --paginate "repos/OWNER/NAME/rules/branches/main?per_page=100"`.
Headers are required because this endpoint's arrays have no total count. The
validator requires HTTP 200, a complete `Link` chain with consecutive page numbers
and a stable endpoint and final page count, at most 100 rules per page, full
nonfinal pages, and unique `(ruleset_id, type)` identities across pages. Missing,
extra, repeated or inconsistent pages refuse publication, including a missing
final page after exactly 100 rules. A full final page without a next link is valid.
Bare arrays cannot establish pagination completeness and are refused by the CLI.
`main-branch.json` is the single JSON object from
`gh api "repos/OWNER/NAME/branches/main"`. The `--branch` file is required, even
when classic protection is disabled; an absent, unreadable or malformed response
cannot establish that no classic contexts are required. It must be a file because
stdin is reserved for check-run pages.
`status-pages.json` is the concatenated HTTP headers and JSON arrays from
`gh api --include --paginate "repos/OWNER/NAME/commits/COMMIT/statuses?per_page=100"`,
where `COMMIT` is the full tagged commit SHA. The same HTTP 200 and complete link
chain checks apply, with at most 100 statuses per page and unique positive status
IDs across all pages. Status links must stay on the tagged commit's statuses
endpoint. Bare arrays and incomplete pagination refuse publication. `--statuses`
is required, including a captured HTTP 200 empty array when there are no statuses;
omitting statuses cannot establish that no failing status exists. The workflow
refuses publication on any API command failure, including after partial output.
`check-pages.json` is the stream of concatenated JSON page objects from `gh api --paginate
repos/OWNER/NAME/commits/COMMIT/check-runs?filter=all&per_page=100` (quote the endpoint
in a shell). A single array containing those pages is also accepted. The validator
accepts check pages on stdin by default and bounds input
before decoding JSON: 1 MiB each for rules and branch protection and 16 MiB each
for check and status pages, with at most 32 nested containers in any input.
An unreadable, malformed, oversized or incomplete
response fails closed. The checkout must match
the tagged commit and have an up-to-date `origin/main` reference.

After these prerequisites the workflow builds and attests the source artifacts and
attaches them to the draft. Attestations are created before release creation and
do not require a published release. If binary releases are separately enabled,
their attestations also precede upload, and the later `gh release upload TAG`
uses `GH_REPO` to select the repository and an authenticated token with
`contents: write` to address the draft by tag. All uploads must finish before
the owner reviews the referenced evidence and manually publishes the draft; the
workflow never publishes it automatically.
Independent release-download verification is a **post-publication** step using
`verify_release.py` below from a fresh directory on an operator machine. Neither
publication nor download verification is a pre-publication record entry. Keep the
download verification report as evidence after publication; do not declare it passed
in advance. The verifier deliberately refuses drafts, so run it only after the
owner has published the release.

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
