# Compiled static prefab runtime qualification

The authoring service emits compiled static prefab generations. This change adds
an optional runtime that loads an explicit generation and instantiates its nodes
in a real EnTT registry. The installed `luminumbra_prefab_inspect` consumer reports
the resulting hierarchy, transforms, bounds, and material bindings without a
graphics context.

See the [runtime API and installation guide](https://github.com/d-addison/luminumbra/blob/devel/tools/prefab_runtime/README.md)
for build commands and the pinned-generation CLI, and the
[compiled prefab service guide](https://github.com/d-addison/luminumbra/blob/devel/tools/blender/authoring/service/PREFABS.md)
for producing a generation. The [machine-readable qualification record](assets/prefab-runtime/20260909/verification.json)
lists the exact executed source, case names, component hashes, and scope.

`PrefabAsset::Load` requires the generation ID and manifest SHA-256, verifies every
member's size and digest, and owns the validated mesh and texture bytes.
Instances share immutable assets. The loader refuses unsupported fields, invalid
references, cycles, corrupt geometry, incomplete textures, arbitrary generation
paths, descendant symlinks, and Windows reparse points. It opens a quiescent
generation below a caller-selected trusted project root.

`PrefabScene` preserves persistent instance/node identities, multiple mesh/material
bindings, full affine world transforms, inverse-transpose normals, mirrored
winding, and recomputed bounds. Replacement constructs the complete candidate
before discarding the previous instance, including equal-count updates.
Validation and construction failures preserve the previous instance. The API
requires single-threaded ownership, no reentrant callbacks, and nonthrowing
destruction callbacks; transient entity handles change on replacement.

`LUMINUMBRA_ENABLE_PREFAB_RUNTIME` defaults to `OFF`. Common engine sources use
an explicit list and do not compile the optional authoring files. The runtime
does not modify saved-world formats or write back into compiled generations.

Qualification was executed against source
`6d34b27a9effafda1df144128dac6211fe3918b7`, tree
`c2d797983d3b1475bd333180fc5f00b265677188`:

| Execution | Result |
|---|---|
| Linux GCC release, actual top-level optional targets | 20 C++ cases passed |
| Linux Clang 18 ASan, UBSan, and default LeakSanitizer | 20 C++ cases passed |
| Linux installed service/compiler/consumer | 19 acceptance checks passed |
| Windows UCRT64, isolated optional targets | 19 C++ cases passed |
| Windows installed service/compiler/consumer | 23 acceptance checks passed |

All executed C++ cases completed with zero skips, failures, or errors. The Linux
count includes a POSIX-specific symlink case. Windows acceptance created an actual
NTFS junction and verified refusal by the installed consumer. Both installed
lanes compiled the authored fixture through the real service/compiler, checked
hierarchy and edited placement, retained the original generation, and refused a
corrupted replacement. The Windows input audit verified 555 payload files,
pinned tools, runtime libraries, built/installed executable parity, original logs,
and all receipt joins. Builds used at most two workers.

Source formatting and the repository's curated clang-tidy checks passed on the
changed translation units. Windows consumer SHA-256:
`d77e8885246446375fe1eed1e93b57e5c9413512c77299a9d7145b83d0cbf643`.
Independent native audit SHA-256:
`17cfc22842452d1a721f92b17d4b5871ea75d0af8f3ce8180a83d4ce65fd8584`.

This qualifies static prefab loading and ECS instantiation through the installed
headless consumer. The current renderer does not consume these components;
inspection reports retain `renderer_qualified: false`. Material rendering,
Blender preview, picking, physics, scripts, replication, save persistence, GPU
performance, and visual approval remain outside this change. Qualification above
retains its exact source identity when this commit is composed into another branch.

The Linux/GCC CI job enables the optional targets in its existing build. Its
installed-prefab gate requires the 20 named C++ obligations in the full CTest
report, then installs the consumer and runs the real service/compiler acceptance.
Missing, duplicate, failed, or skipped component cases fail the gate. Artifacts
retain the installed tool hashes, receipt logs and synthetic compiled generations.
This ongoing Linux gate does not replace the separately pinned Windows evidence
above or claim rendered preview coverage.
