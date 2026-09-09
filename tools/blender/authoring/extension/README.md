# Blender asset authoring

This optional Blender extension builds a marked collection with the installed
Luminumbra asset compiler. It provides persistent IDs, separate static/character
export profiles, a half-second edit debounce, cancellation and compiled asset
counts. The existing `probes/extension` package remains a separate mock.

The optional static prefab profile preserves material bindings and hierarchy in
compiled assets. Engine-rendered viewports, prefab runtime instantiation, behavior
graphs, morphs, retargeting, IK and physical animation remain unavailable. Build
receipts do not qualify those workflows or production content in a composed game.

## Install and use

```sh
python tools/blender/authoring/package_extension.py /your/output/luminumbra_geometry_author.zip
```

Install the ZIP through Blender's **Install from Disk**. Install the
[authoring service](../service/README.md) separately; this adapter requires service
0.2 or newer with `source_revision_guards` for geometry, or 0.3 for static prefabs.
Configure the project directory,
external Python 3.11+, installed service archive and compiler manifest in the
**Luminumbra** sidebar of the 3D View.

Select a regular collection in the Outliner and click **Mark / Refresh Asset IDs**.
This undoable operator assigns asset, object, mesh and material identities. It
preserves distinct IDs, repairs copied IDs and retains shared mesh identity for
objects using the same mesh data. Renaming an object leaves its ID unchanged.
Click **Build Asset** to capture and compile the collection. Enable **Preserve
static prefab and materials** for a static asset with hierarchy, shared geometry
and the supported material subset described below. Leave it disabled for the
geometry-only character profile.

**Rebuild after edits** rebuilds after a half-second pause. Editing during a build
cancels that work and advances its revision guard. Unsupported content leaves
the prior generation intact and displays a diagnostic. **Cancel Build** suppresses
automatic retry until a new edit or explicit build. **Stop Service**, changing
documents and disabling the extension stop its owned processes. Use one Blender
authoring session per project.

## Export profile

The initial native profile is Windows Blender 5.1.0, build `adfe2921d5f3`, with glTF
exporter 5.1.18. The exporter digest is
`5fe9f5ede7e5264b0a1045dc3784e243e645a90cb6073fc73ae56bc7a10de447`:
SHA-256 of canonical JSON `{path, sha256}` records for all 119 Python files,
sorted by relative POSIX path. Other profiles require the native fixture matrix;
portable Python tests do not qualify Linux Blender.

Use scene unit scale 1 for meters. Include every parent, rig, constraint and
modifier object in the asset collection. Supported object types are mesh,
armature and empty. Objects sharing mesh data are supported. Realize collection
and Geometry Nodes instances first; experimental instance export is disabled.

Static export evaluates modifiers. Character export preserves skinning and samples
clips over the scene range; apply non-armature modifiers beforehand. Each
character asset contains one rig; export unskinned attachments separately.
Shape keys, drivers, linked libraries and overrides are refused. Add UVs before
building. The compiler enforces remaining geometry and skin budgets.

Static prefabs use the service's [declared material contract](../service/PREFABS.md).
The Blender adapter accepts an active Material Output fed directly by Principled
BSDF, constant metallic/roughness values, a direct sRGB base-color image and an
optional tangent Normal Map using a Non-Color image. Image vectors use the active
render UV map without connected vector nodes. Linear/Closest interpolation and
Repeat/Extend/Mirror extension are supported. Alpha may come from the same color
image, directly or through one Greater Than cutoff. Additional Principled layers,
custom specular/IOR settings, volume and displacement are refused. Bake unsupported
shader constructions explicitly before building. Arbitrary Blender node graphs
are not compiled into runtime shaders.

The snapshot dependency closure refuses audio, scripts, movies and whole scenes.
Unpacked images must be static files inside the project. Generated and packed
images travel with the library snapshot. No embedded blend-file script runs in
the background exporter.

## Snapshots and lifecycle

Blender's main thread captures the collection and its indirect dependencies with
[`bpy.data.libraries.write`](https://docs.blender.org/api/5.1/bpy.types.BlendDataLibraries.html).
The open document's path remains unchanged. Snapshots live in private directories
under `.luminumbra-author-source/` inside the project. Exclude that directory and
`.luminumbra-author/` from source control and art-pack publication. Capturing a
large library can still pause the UI; no responsiveness target is claimed from
the small-fixture qualification.

An external Python broker handles IPC. Another Blender process evaluates and
exports with factory startup, disabled automatic scripts, isolated resources and
two CPU workers. The installed service then compiles and validates the GLB. A
dependency hash pins the captured scene revision so edits before extraction or
during compilation cannot publish that stale snapshot. No background Python
thread accesses the editing Blender process.

Inherited pipes, session tokens and a bounded response file connect the processes.
Losing an ownership pipe stops its native child. Export times out after 180
seconds. Receipts record Blender/exporter identities, settings and output hashes.
Counts and timing concern asset publication, not viewport feedback or GPU memory.

## Qualification

```sh
python -B -m unittest discover -s tools/blender/authoring/extension_tests -v
```

`extension_tests/native_probe.py` is a Blender entry point. Supply `--archive`,
`--output`, `--python`, `--service`, `--toolchain` and `--isolation-root` after
Blender's `--` separator. The probe checks actual config and temporary paths
against the isolation root before installing the extension. On Windows, launch
Blender from native Windows Python with explicit environment overrides.
Use an owned process with factory startup, disabled automatic scripts, an explicit
Python exit code and isolated resource directories. It installs the actual ZIP
and builds prop, foliage and character content through the native compiler,
checking identities, revision feedback, refusals, cancellation and cleanup.
Undo requires an editor context and is evaluated by its interactive mode.
Engine viewport and composed-game production acceptance remain separate lanes.
