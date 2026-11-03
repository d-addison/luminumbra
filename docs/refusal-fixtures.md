# Saved-world refusal fixtures

Produce a fresh fixture tree from an already generated saved world:

```sh
python3 tools/persistence/refusal_fixtures.py create \
  --source-root fixture-source --out refusal-output --engine-root .
python3 tools/persistence/refusal_fixtures.py verify refusal-output
```

Both commands return 0 on success and 2 on invalid input or mismatch. The output
must not exist, including as an empty directory. Its parent must already exist.
Source and output paths must not overlap. Existing raw path ancestors are checked
before path normalization.
Symlinks, reparse points and special files are refused throughout inspected trees.
Use stable local trees without concurrent filesystem changes during production
or verification; this harness does not provide a filesystem transaction.

The source root contains exactly `worlds/saves/ID/`, with `world_info.json`,
`preset.json`, `chunks/region/world-manifest.json` and one or more immediate,
canonical `chunks/region/r.X.Z.lmr` files. Extra files and empty directories are
refused. Coordinates must be canonical signed 32-bit integers. Metadata has an
explicit safe `worldType` and no `id` or `worldId` fields. JSON rejects duplicate
keys and nonfinite numbers. Input JSON is limited to 1 MiB per file.

The producer reads the single `constexpr` container version declaration in
`src/luminumbra_common/persistence/WorldSaveService.h`, and reads the engine's
full committed HEAD using a read-only subprocess. Metadata, native manifest and
all region container versions must agree. Region checks require the 8-byte LMR1
header, 18-byte record headers, supported header fields, unique record keys,
nonempty payloads and an exact framed length. They do not decompress payloads.

The output preserves an unchanged baseline at `worlds/saves/ID/` and writes these
direct save-directory clones in fixed order:

| Directory under `refusals/` | Only changed file | Mutation |
| --- | --- | --- |
| `obsolete-metadata` | `world_info.json` | Replace only `container_version` with 1 |
| `future-metadata` | `world_info.json` | Replace only `container_version` with current + 1 |
| `corrupt-metadata` | `world_info.json` | Exactly one byte: `{` |
| `missing-metadata` | `world_info.json` | Remove the file |
| `missing-preset` | `preset.json` | Remove the file |
| `truncated-region` | First region in lexical filename order | Remove exactly its final payload byte |

Metadata bytes outside the designated version token remain identical. The native
world manifest remains untouched in every clone. Directory names and manifest
`world_id` identify clones; metadata is not rewritten to insert identifiers.

The last file written is `refusal-manifest.json`, with schema
`luminumbra.refusal_fixtures.v1`. It contains the full `source_revision`, current
`container_version`, `source_world_id`, matching `source` and `baseline` receipts,
and the ordered `variants` array. Each variant records `id`, `world_id`,
`world_type`, `expected_exit: 2`, a `reason` object with `kind` and a `version` only
for obsolete/future metadata, a complete `inventory`, a `mutation` with relative
`path` and nullable `before`/`after` size/hash objects, and `runtime_requirements`.
Inventories use the existing `save_receipt.snapshot` SHA-256 and tree digest
semantics. Copies, hashes and prefix comparisons stream bounded blocks; the
manifest reader has a 64 MiB limit. There are no timestamps or absolute source
paths in the manifest. Production failure removes only the fresh tree owned by
that invocation, with a root identity check before cleanup.

Verification enforces the exact schema and variant order, recomputes inventories,
requires matching source/baseline receipts and exactly one designated difference
per clone, and checks actual mutation bytes against the baseline. Rebinding a
hash to a wrong mutation does not make it valid. Verification checks the revision
identifier's type and full-length syntax; it cannot authenticate the historical
source revision or recover the original source independently from the output.
Keep trusted source receipts and native evidence separately when provenance is
required.

This is a structural-only source harness. Python framing does not prove native
loadability, valid decompressed world contents, or runtime refusal behavior.
Native qualification remains pending. In particular, the `missing-preset`
runtime must independently establish absence of
`worlds/atlas/presets/<worldType>.json` in its disposable runtime root. Its manifest
records `runtime_requirements.atlas_preset_absent` as the source world type; all
other variants record null. Deleting the embedded preset alone does not establish
a refusal because the catalog can use that atlas fallback. The runtime consumer
must enforce this precondition and qualify each refusal with native evidence.

Regression source is in `tools/persistence/test_refusal_fixtures.py`:

```sh
python3 tools/persistence/test_refusal_fixtures.py
```

It uses opaque structural payloads and mocks the revision subprocess. It covers
determinism, exact mutations, baseline preservation, input rejection, cleanup,
semantic and manifest tampering, and the public-tree text guard. These tests are
intended for CI; their source alone is not an executed test result or native
qualification evidence.
