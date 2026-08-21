# Blender and DCC integration for the luminumbra asset pipeline

**Recommendation:** adopt a small, versioned export contract shared by a Blender add-on and a headless export script, then gate every export with an engine-specific validator before invoking `asset_processor`. Add a local watch CLI only after the batch path is stable. Do not put Blender, a REST service, MCP, or an LSP in the runtime or CMake build graph.

The useful seam is not Blender-specific: DCC tools produce a deliberately restricted GLB plus separate PNG source textures; luminumbra-owned tools validate and compile those interchange files to `.lmesh`, `.lanim`, and `.ltex`. Blender is the first adapter for that seam.

Effort estimates below mean: **low** = roughly 1-3 engineering days, **medium** = about 1-2 weeks including tests, and **high** = multiple weeks or ongoing cross-platform/tool-version ownership.

## Current state

There is no DCC integration in the inspected tree. The current bridge is a file-format boundary and a CMake rule:

| Area | Verified behavior | Consequence for DCC integration |
|---|---|---|
| Source formats | `asset_processor` accepts an input GLB or PNG and chooses output behavior from `.lmesh` or `.ltex` ([`tools/asset_processor.cpp:737-780`](../../../tools/asset_processor.cpp)). | Export meshes as binary glTF (`.glb`). Treat texture PNGs as separate source artifacts; embedded GLB images are not compiled to `.ltex`. |
| Static mesh contract | All mesh primitives are merged by default. Each accepted primitive must be indexed and provide `POSITION`, `NORMAL`, and a texcoord. The base-color material's texcoord set is preferred, with the first texcoord as fallback ([`tools/asset_processor.cpp:554-638`](../../../tools/asset_processor.cpp)). | Export indexed triangles with normals and UVs. Materials, cameras, lights, vertex colors, tangents, and morph targets do not survive into v1 `.lmesh`; multiple material primitives lose their material identity unless processed separately with `--primitive`. |
| Static optimization | The processor remaps and optimizes vertices/indices with meshoptimizer and can simplify LOD0 with `--max-tris` ([`tools/asset_processor.cpp:647-688`](../../../tools/asset_processor.cpp)). `--emit-lods` emits `.lod1.lmesh` and `.lod2.lmesh` from fixed fractions of a static budget ([`tools/asset_processor.cpp:782-832`](../../../tools/asset_processor.cpp)). | Keep Blender-side mesh compression and simplification off. Make triangle budget and `--emit-lods` explicit pipeline metadata, not artist-local exporter state. Do not use `--emit-lods` for skinned assets: the skinned path does not apply the static simplifier, so it would only repeat conversion under LOD filenames. |
| Skinned mesh contract | The presence of any skin selects LMS2. The converter uses only `data->skins[0]`, then visits every mesh primitive. Each accepted primitive must be indexed and include `POSITION`, `NORMAL`, a texcoord, `JOINTS_0`, and `WEIGHTS_0` ([`tools/asset_processor.cpp:184-290`](../../../tools/asset_processor.cpp)). Four joint lanes are read per vertex, indices are checked against the first skin, weights are normalized and deterministically quantized to four bytes, and joints are stored as bytes. | A skinned GLB should contain exactly one armature/skin and only geometry bound to it. Export at most four influences per vertex. Reject mixed static/skinned geometry, additional skins, missing attributes, and out-of-range joints before compilation. The joint count must be within the engine's compiled `kMaxJointsPerSkeleton`. |
| Skeleton and animation | Joint hierarchy, local TRS, optional inverse-bind matrices, and hashes of joint node names are written to LMS2. Animation conversion writes sibling `<stem>.<animation-name>.lanim` files and supports translation, rotation, and scale only; morph-weight channels are ignored ([`tools/asset_processor.cpp:115-179`](../../../tools/asset_processor.cpp)). | Require stable, unique, non-empty bone names and portable, unique animation names. Export sampled TRS animation; disable morph animation. Validate that animation targets belong to the one exported skeleton. |
| Failure signaling | `process_gltf` is `void`, returns early on several parse/compatibility/output errors, and `main` subsequently returns zero ([`tools/asset_processor.cpp:554-573`](../../../tools/asset_processor.cpp), [`tools/asset_processor.cpp:809-834`](../../../tools/asset_processor.cpp)). Missing primitives may only produce warnings. | A successful process exit is not presently proof of a valid compiled asset. The bridge must validate first and, when smoke-running the processor, require expected non-empty outputs and reject warning/error diagnostics. |
| Discovery and rebuild | CMake performs a configure-time recursive glob of `assets/**/*.glb` without `CONFIGURE_DEPENDS`. Each discovered input gets a custom command mapping its relative path to a `.lmesh` under the runtime data output. Existing files are `MAIN_DEPENDENCY` inputs ([`CMakeLists.txt:178-217`](../../../CMakeLists.txt)). | Editing an already-known GLB is picked up by the next build. Adding, deleting, or renaming a GLB requires an explicit CMake reconfigure before build. A watcher cannot make a new asset visible merely by writing the file. |
| Runtime data inventory | The copied `data/` tree is governed by an authored manifest, including current `.lanim`, PNG, and `.ltex` files ([`cmake/runtime_data_manifest.cmake:1-7`](../../../cmake/runtime_data_manifest.cmake), [`cmake/runtime_data_manifest.cmake:58-64`](../../../cmake/runtime_data_manifest.cmake)). | A Blender bridge should not edit this CMake manifest implicitly. Any workflow that promotes generated outputs into tracked `data/` must make the manifest update an explicit, reviewed step. |
| Existing reference asset | The Grovestrider generator demonstrates the effective skinned shape: indexed triangles with `POSITION`, `NORMAL`, `TEXCOORD_0`, `JOINTS_0`, `WEIGHTS_0`, one skin with inverse-bind matrices, named joints, and named TRS animations ([`tools/generate_grovestrider_gltf.py:143-218`](../../../tools/generate_grovestrider_gltf.py)). | Use an equivalent Blender-exported fixture as the golden acceptance test for LMS2 and `.lanim`. |

The processor calls `cgltf_parse_file` and `cgltf_load_buffers`, but not `cgltf_validate`. Standards-valid glTF is also broader than the engine contract. Therefore Khronos validation and engine-compatibility validation are two separate gates.

### Engine-correct Blender export profile

Pin the supported Blender major/minor version and call `bpy.ops.export_scene.gltf` with named arguments rather than relying on saved UI state. The baseline profile should set:

```python
bpy.ops.export_scene.gltf(
    filepath=output_path,
    export_format="GLB",
    export_yup=True,
    export_texcoords=True,
    export_normals=True,
    export_skins=True,
    export_influence_nb=4,
    export_all_influences=False,
    export_animations=True,
    export_force_sampling=True,
    export_morph=False,
    export_morph_animation=False,
    export_cameras=False,
    export_lights=False,
    export_draco_mesh_compression_enable=False,
    export_meshopt_compression_enable=False,
    export_use_gltfpack=False,
)
```

The current Blender Python API exposes these controls, including GLB output, +Y-up conversion, skins, a four-influence limit, animation sampling, and compression switches. Blender's manual documents mesh, animation, and skinning export and warns that allowing all influences can be incompatible with consumers. Pin and regression-test the complete argument dictionary because operator parameters can change between Blender releases. `export_apply` should also be pinned by the implementation after golden testing of modifiers and armatures; it must not remain an artist preference.

The profile intentionally disables Draco, `EXT_meshopt_compression`, and gltfpack. Luminumbra performs its own meshoptimizer pass and the current loader does not establish a decoder path for compressed primitive buffers. It also disables unsupported morph output. PNG texture baking/copying needs a separate, explicit stage because mesh compilation does not extract GLB images and CMake has no analogous PNG glob-to-`.ltex` rule.

## Candidate integrations

| Candidate | Delivery form | Effort | Risk | Verdict |
|---|---|---:|---:|---|
| Engine-compatibility validator | **CLI** (luminumbra-owned, with JSON report) | Medium | Low | **P0 adopt first** |
| Headless Blender batch exporter | **Python script**, wrapped by a **Banso YAML/TypeScript step** | Medium | Medium | **P0 adopt** |
| Blender “Export for luminumbra” add-on | **`bpy` exporter add-on** | Medium | Medium | **P1 adopt**, sharing code/profile with batch export |
| Configure/build bridge | **Banso TypeScript step** or local **script** invoking existing CMake/build commands | Low | Low | **P1 adopt** |
| Watch-mode re-export/reimport | Local **CLI** supervising headless Blender and the validator | Medium | Medium-high | **P2 adopt after batch** |
| DCC-neutral asset contract | Versioned JSON contract plus per-DCC **scripts/CLI adapters** | Medium | Low | **P1 define now; add adapters on demand** |
| Interactive Blender control | **MCP** server backed by a Blender add-on | High | High | **P3 defer; never a build dependency** |
| Remote asset farm | **REST** service | High | High | **Do not adopt now** |
| Scene diagnostics in an editor | **LSP** | High | High / poor fit | **Do not adopt** |

### 1. Engine-compatibility validator — CLI

Create a deterministic command such as `luminumbra-asset validate <file.glb> --profile static|skinned --json`. Run the official Khronos glTF Validator first (or embed its pinned package), then apply luminumbra-specific rules. The official validator checks GLB structure, schemas, references, buffers, accessor values, animations, images, and extensions and returns nonzero on errors, but it cannot know LMS2's restrictions.

The engine layer should reject, at minimum:

- non-GLB input; required or used compression extensions; sparse/extension features not exercised by a checked-in fixture; non-triangle or non-indexed primitives; non-finite values; mismatched attribute counts; missing positions, normals, or UVs;
- for static assets, unexpected skins and any pipeline metadata inconsistent with `--primitive`, `--max-tris`, or `--emit-lods`;
- for skinned assets, anything other than one skin, any primitive not bound to that skin, absent `JOINTS_0`/`WEIGHTS_0`, accessors not effectively four lanes, more than four nonzero influences, a zero or over-limit joint count, joint lanes outside the skin, non-normalizable weights, duplicate/empty joint names, hash collisions, or missing/invalid inverse-bind data;
- animation targets outside the skeleton, paths other than translation/rotation/scale, duplicate or filesystem-unsafe clip names, empty clips, and malformed/non-monotonic sampler times;
- embedded-only textures when the asset declaration expects an engine `.ltex`, or missing separately exported PNG sources.

After static checks, optionally run `asset_processor` into a temporary directory and require the expected `.lmesh` plus exact named `.lanim` siblings to exist, be non-empty, and have valid headers/counts. Treat any processor `Error:` or “Skipping … due to missing attributes” diagnostic as failure despite its exit status. For static assets, optionally test `--emit-lods`; for skinned assets, do not.

This CLI is the key DCC-neutral seam: Maya, Houdini, 3ds Max, or procedural tools can produce the same restricted GLB and receive the same diagnostics without embedding engine rules in each exporter.

### 2. Headless Blender batch exporter — script plus Banso step

Use a checked-in Python entry point with a command resembling:

```text
blender --background scene.blend --python-exit-code 1 --python export_luminumbra.py -- --output assets/creatures/scene.glb --profile skinned
```

The script should load the shared profile, select an explicitly named collection, export to a temporary GLB, run the validator, and atomically replace the destination only on success. It should emit a small JSON result containing Blender version, profile version, source, destination, content hash, animation names, texture dependencies, and validation outcome. `--python-exit-code` prevents Python exceptions from looking successful.

The Banso YAML/TypeScript wrapper should declare Blender as an external host-tool prerequisite, pass paths without shell interpolation, capture logs/cost, and then call the existing configure/build path. Blender is too large and stateful for a WASM step; WASM is suitable only for a future pure validation module. This path is reproducible and CI-capable if the Blender build and add-on version are pinned, but Blender version drift and platform-specific installation paths make the initial risk medium.

### 3. Blender exporter add-on — `bpy` add-on

Add one operator and a compact validation panel: “Export for luminumbra.” It should reuse the exact Python profile and manifest schema from the headless script rather than duplicate settings. Artist-visible options should be limited to collection, static/skinned profile, output-relative path, triangle budget, LOD policy, and texture declarations. Compression, axes, influence count, morph policy, and animation sampling remain locked by the versioned profile.

Before launching the exporter, perform fast Blender-side checks for applied scale policy, triangulatable geometry, UV availability, one armature, four-influence pruning/normalization, unique bone/action names, and action ownership. After export, invoke the same external validator. Add-on diagnostics should link errors back to Blender objects/bones where possible, but the external CLI remains authoritative.

Risk is primarily Blender API churn and the temptation to let add-on-only behavior diverge from headless export. A shared module plus one golden static `.blend` and one golden skinned `.blend` control that risk.

### 4. Configure/build bridge — Banso TypeScript step or script

The bridge should snapshot the set of `assets/**/*.glb` paths before export. If only bytes of an existing path changed, run the normal build; CMake's `MAIN_DEPENDENCY` will rebuild its `.lmesh`. If the path set changed, run an explicit CMake reconfigure first, then build `process_assets` (or the normal application target). This preserves the intentional no-`CONFIGURE_DEPENDS` choice in `CMakeLists.txt`.

Do not make this step edit `CMakeLists.txt` or the authored runtime-data manifest. Do not have the Blender add-on guess a build directory. Banso/project configuration should supply the configure preset and build directory, while the standalone script accepts them as explicit arguments.

### 5. Watch-mode re-export/reimport — local CLI

Watch source `.blend` files and the export contract, debounce changes, and serialize one headless Blender subprocess per source. The state machine should be `changed -> export temporary -> validate -> atomic publish -> reconfigure if path set changed -> process asset`. Preserve the last good GLB and compiled outputs on any failure. On Windows, wait for the source file to become readable and stable before launching Blender; never export from a partially saved file.

A fresh background Blender process is slower than a persistent in-process socket, but it isolates crashes, stale dependency-graph state, and add-on memory leaks. Start there. Watch mode is a developer convenience, not a release gate. It should print the exact batch command so any failure is reproducible without the watcher.

Risk is medium-high because save storms, atomic replacement, process cancellation, animation sibling cleanup, and the configure-time glob all need careful handling. In particular, a renamed/deleted animation can leave stale `.lanim` siblings unless the batch result manifest identifies the complete output set and the watcher removes only previously declared outputs.

### 6. DCC-neutral contract — JSON plus adapters

Define a small versioned sidecar, for example `<asset>.luminumbra-asset.json`, describing profile (`static-v1` or `skinned-lms2-v1`), source collection, texture PNGs and intended `.ltex` sizes, primitive/material split policy, triangle budget, LOD emission, expected clips, Blender/DCC adapter version, and output paths. The CLI validates the GLB against the sidecar; CMake continues to consume only the GLB.

This keeps engine requirements in one place and makes other DCC adapters shallow: export standard GLB, write the sidecar, invoke the CLI. It also makes watch cleanup and CI comparisons explicit. The sidecar must not become runtime simulation input.

### 7. MCP, REST, and LSP alternatives

An MCP server can eventually expose “inspect scene,” “run engine validation,” or “export collection” to an interactive agent through a Blender add-on. It is useful authoring UX, but a poor canonical pipeline: it adds connection state, trust boundaries, Blender UI lifecycle, and protocol/version ownership. It should call the batch command and validator, never replace them.

A REST asset farm is unjustified until there is a real need for centralized licenses, expensive bakes, or many artists; it adds upload security, storage, queueing, and exact Blender-image reproducibility. An LSP models text documents and does not naturally map diagnostics back into a binary `.blend`; use the Blender add-on panel and JSON/CLI diagnostics instead.

## Ranking

| Rank | Integration | Why this order |
|---:|---|---|
| 1 | Engine-compatibility validator CLI | It turns silent skips and zero-exit conversion failures into an enforceable contract and works for every DCC. |
| 2 | Headless Blender script + Banso wrapper | It proves a reproducible, automation-safe export before UI work and provides the implementation used by CI and watch mode. |
| 3 | Blender add-on and versioned DCC-neutral sidecar | They improve artist ergonomics without creating a second export path; both reuse ranks 1-2. |
| 4 | Configure/build bridge | Small but required to handle the configure-time GLB glob correctly, especially for new or renamed assets. |
| 5 | Watch-mode CLI | Valuable iteration speed after batch export, validation, manifests, and cleanup semantics are reliable. |
| 6 | MCP authoring facade | Optional interactive convenience over the stable commands; keep out of CI and releases. |
| 7 | REST farm or LSP | No current requirement offsets their operational cost or poor scene-authoring fit. |

The smallest staged delivery is therefore: (1) validator and two golden fixtures, (2) pinned headless export plus JSON sidecar/result, (3) Banso configure/build wrapper, (4) thin Blender operator, and only then (5) watcher.

## Determinism and `world_hash`

Validation, orchestration, and watch tooling are offline and do not themselves alter `world_hash`. Re-exporting, changing Blender versions/presets, changing influence pruning, changing meshoptimizer inputs/budgets, or regenerating `.lmesh`/`.lanim` can change asset bytes, bounds, animation samples, and potentially any simulation-visible data derived from those assets. Treat such changes as world-hash-contract changes unless proven render-only: pin tool/profile versions, record hashes, review binary-output diffs, and run the repository's run/replay parity gate before promoting regenerated assets.

`--emit-lods` is documented in the processor as render-only and leaves LOD0 unchanged; that narrow use can remain outside `world_hash` if the renderer continues to select only visual geometry. Do not generalize that exemption to LOD0, skeleton, animation, collision, bounds, or asset-presence changes.

## Sources

Repository files inspected (no architecture claims were taken from `README.md`):

- [`tools/asset_processor.cpp`](../../../tools/asset_processor.cpp) — accepted attributes, LMS2/animation conversion, texture path, meshoptimizer/LOD flags, and error behavior.
- [`CMakeLists.txt`](../../../CMakeLists.txt) — configure-time `assets/**/*.glb` discovery and custom processing commands.
- [`cmake/runtime_data_manifest.cmake`](../../../cmake/runtime_data_manifest.cmake) — authored runtime-data inventory.
- [`tools/generate_grovestrider_gltf.py`](../../../tools/generate_grovestrider_gltf.py) — current skinned glTF fixture shape.
- [`docs/research/engine-library-landscape-2026.md`](../engine-library-landscape-2026.md) — prior repository research that places DCC interoperability at an offline asset boundary rather than in the runtime.

Web sources consulted:

- [Blender 5.0 manual: glTF 2.0 importer/exporter](https://docs.blender.org/manual/en/5.0/addons/import_export/scene_gltf2.html) — supported mesh, material, skinning, and animation export behavior.
- [Blender Python API: `bpy.ops.export_scene.gltf`](https://docs.blender.org/api/main/bpy.ops.export_scene.html) — callable exporter parameters used by the proposed profile.
- [Blender manual: command-line arguments](https://docs.blender.org/manual/en/latest/advanced/command_line/arguments.html) — background execution, Python scripts, argument ordering, and Python exception exit codes.
- [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html) — normative geometry, skins, `JOINTS_n`/`WEIGHTS_n`, animations, accessors, and GLB rules.
- [Khronos glTF Validator](https://github.com/KhronosGroup/glTF-Validator/blob/main/README.md) — standards validation coverage, JSON reports, recursive CLI mode, and nonzero-on-error behavior.
