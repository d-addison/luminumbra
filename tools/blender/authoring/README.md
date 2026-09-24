# Blender authoring probes and service mock

The separate [installed geometry build service](service/README.md) performs real
native compilation and generation publication. The [geometry extension](extension/README.md)
connects Blender collection snapshots to that service. The probes and extension
below remain explicitly labelled mocks.

This optional source package generates reproducible asset fixtures and exercises
an authoring service lifecycle in Blender. The service and extension display
**MOCK**: they validate draft sidecars, copy immutable snapshots and return preview
metadata. They do not compile engine assets, execute behavior graphs or display
engine-rendered frames. Ordinary engine builds and startup do not use this package.

Python 3.11 or newer runs the CPU probes without Blender or a built engine:

```sh
python -m pip install jsonschema
python -B tools/blender/authoring/probes/run.py --suite all --require-schemas
```

The command writes a fresh ignored `evidence/` directory beside this guide and
updates `evidence/latest.json`. It runs fixture serialization and service tests,
generates four mock JSON schemas with examples, and builds a local extension ZIP.
`--require-schemas` makes missing schema validation fail. A viewport-only invocation
records `NOT_IMPLEMENTED` and returns failure because it executes no tests.

## Fixtures and evidence

| Suite | Available evidence | Limits |
|---|---|---|
| Prop | Transformed hierarchy, linked instances, material/UV chart and persistent correlation IDs | Compilation can lose placement; serialized equality alone is insufficient |
| Plant | Deterministic Geometry Nodes leaves, realized material assignment and alpha-mask chart | The small fixture does not qualify production canopy, mips or LODs |
| Character | Two-bone rig, weighted mesh and action; synthetic STEP, CUBICSPLINE, joint-order and matrix-bind cases | General rigs, morphs, retargeting, IK and physics are not implemented here |
| Service | Cancellation, stale revisions, immutable snapshots, atomic generation pointer, recovery and held preview generations | It copies metadata; no engine compilation or rendering |
| Viewport | Explicit list of unevaluated acceptance checks | No RenderEngine adapter or engine color/depth transport |

The CPU tests record what the selected validator reports. They do not require it
to keep accepting known unsupported assets. Reports distinguish byte equality from
canonical JSON/buffer equality. The comparison does not normalize different
accessor layouts or establish rendered fidelity across exporter/platform profiles.

By default, fixtures use the enclosing engine checkout. To reproduce the historical
importer audit, set `LUMINUMBRA_AUTHOR_SOURCE` to a separate clean clone at
`657c731f19db95b8b127ba418951adb64483adc3` and add `--require-baseline` to the CPU
command. The audit found lost static instance transforms, incorrect STEP and
CUBICSPLINE samples, and incorrect joint-order/matrix bind palettes. Those are
engine defects to repair, not capabilities advertised by this package.

Every report identifies the actual source revision, source files and Python
profile. Optional `--blender-root` identifies a Blender 5.1 installation and runs
its packaging validator as a Python script; it does not launch Blender. Generated
assets, captures, archives, receipts and build trees are ignored and stay local.

## Native Blender qualification

The extension deliberately requires Blender 5.1.0. The initial Windows profile
used Blender build `adfe2921d5f3` and glTF exporter 5.1.18. Other versions require
qualification; a successful Linux CPU run does not qualify Linux Blender.

Run the native launchers with the host's Python, passing its Blender executable:

```sh
python probes/queue_blender.py --blender "$BLENDER_EXECUTABLE" --suite prop
python probes/queue_extension.py --blender "$BLENDER_EXECUTABLE"
```

These commands are relative to this directory and only describe a run by default.
Add `--execute-coordinated` to launch after reserving the native/GPU slot. Repeat
exports with `--suite plant` and `--suite character`. The extension launcher is
currently Windows-only; `--interactive` exercises its registered panel and captures
an owned window. Both launchers isolate preferences, temporary directories and
outputs, disable automatic scripts, bound CPU workers, set Python failure codes
and impose a 180-second child timeout. They do not install into ordinary user
preferences or attach to an existing Blender scene.

From WSL, `probes/run_native.py` coordinates the native launchers. It requires
`--python` with the Windows Python executable accessible from WSL, and `--blender`
with a Windows-native path except for the `service` suite. Suites are `prop`,
`plant`, `character`, `service`, `extension` and `extension_ui`. Host sandbox
permissions still apply. Native receipts go under ignored `probes/native-runs/`.

The mock extension starts an external Python worker. Point it to an isolated
fixture project containing `asset.json` copied from a generated
`schemas/asset.example.json`, select Python 3.11+, and use revision 1. Advance moves
a job by one phase; Open Preview displays metadata. File changes and extension
unload stop the worker. Blender data access stays on the main thread.

## Mock contract boundaries

The four schema IDs are draft experiment identifiers. Required unknown schemas,
fields, profiles and components are refused. Components cannot execute. Optional
annotations are inert. Graph digesting excludes only root layout and is not a
language compiler; recipe examples do not execute scene edits or undo operations.

The worker uses an inherited pipe and unpredictable session token, with no network
listener. It permits at most 256 jobs and 4 MiB per snapshot, and accepts requests
up to 64 KiB. Source and dependency paths must remain within the fixture project.
The current generation changes only after validation. Cancelled, stale and failed
jobs retain the last valid generation; previews retain their selected generation.

Use trusted, disposable projects. The mock is not a daemon hardened against
concurrent hostile filesystem mutation. Windows uses a process-owned kernel mutex;
Linux uses flock. These locks do not coordinate a project shared simultaneously
between Windows and Linux, or different path aliases. Use one host and canonical
project path per session. Power-loss durability and production garbage collection
are outside this mock's contract.

## Compiled baseline readback

The standalone CPU evidence build is independent of the ordinary engine build.
Select the clean historical source through `LUMINUMBRA_AUTHOR_SOURCE`, then run:

```sh
python probes/build_tools.py --dependency-source "$DEPENDENCY_CACHE"
python probes/compiled_readback.py
python probes/summarize_native.py
```

The dependency cache must contain Git clones named `meshoptimizer-src`, `stb-src`
and `entt-src`. The builder copies them with `--no-hardlinks`, checks their pinned
commits and uses private `dependencies/`, `build/` and `install/` directories.
It never writes into the source checkout or the supplied cache. CMake, Ninja and a
C++20 compiler are required; readback also needs Pillow. Readback expects completed
native prop/plant/character exports and records actual compiled bounds, counts,
alpha coverage and animation palettes. Consolidation requires CPU, all native
suites and compiled receipts; inspect captures separately before claiming visual
acceptance. This evidence build is qualified on Linux only.
