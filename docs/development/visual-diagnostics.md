# Visual diagnostics

This design addresses the native screenshot audit's unconfirmed pixel causes:
missing live/far coverage, absent water, ineffective weather, material discontinuities,
foliage/post artifacts and frame-edge garbage. A screenshot and shader-health boolean
cannot identify a draw. Diagnostics must connect the same captured pixel to its inputs
and producer without changing simulation, saves, hashes or authority revisions.

## Implementation inventory

| Capability | State | Evidence / remaining work |
| --- | --- | --- |
| AOV serialization, integer ID encoding and point-probe JSON | Implemented as independent, GL-free codecs | `DiagnosticCapture.h/.cpp`; registered `DiagnosticCapture.*` regressions in `capture_manifest_test`; native execution pending |
| Lazy integer target with attachment leases | Implemented as a standalone helper | `DiagnosticTargets.h`; registered `DiagnosticTargetDriver.*` regressions in `capture_manifest_test`; native execution pending |
| GPU attribution, draw table, readback and scenario opt-in | Planned | Requires coordinated local changes in owned G-buffer submission and scenario capture files |
| Debug views and inspector | Planned | Extend `DebugViewPass` and developer console widgets |
| Linux shader compilation and software-GL reference scenes | Planned | Fast Linux workflow steps; no required-check or ruleset changes |
| Automated image checks | Planned | Bundle-aware checks alongside existing render-health gates |

The codecs and standalone target do not create scenario bundles or enable a runtime
flag. The capture test target models GL entry points for target lifecycle regressions;
rendering does not call these helpers yet. The client source manifest includes the codecs.

`DiagnosticTargets` owns a lazy RGBA32UI texture and standalone framebuffer, capped
at 16,777,216 pixels (256 MiB; transactional resize peaks at 512 MiB). Capabilities,
dimensions and framebuffer completeness are checked before replacing existing
storage; failed allocation retains the previous handles and restores caller bindings.
Before attaching `texture()` to any caller framebuffer, acquire an
`AttachmentLease` with `acquire_attachment_lease()` and retain it until all protected
attachments have been detached, including attachments on unbound framebuffers.
Leases are move-only; `reset()` and destruction release once, and repeated release
is harmless. The owner must outlive every lease, on the creating context's thread.
An empty target returns an empty lease. `ensure()` returns false before replacing
storage while leased; same-extent reuse succeeds. Owner `reset()` and disabling
`ensure(false, ...)` return false without deleting or changing storage while leased.
Releasing the last lease permits replacement; existing bound texture/image-unit
and owned-framebuffer refusal checks still apply to resize. Destroying the owner
with live leases asserts in debug builds and terminates in every build before GL
deletion. Callers must detach before releasing or move-assigning over a live lease.
The acceptance matrix records this partial state. No new runtime asset is added.

## A: capture bundle and pixel identity

Planned opt-in: `--capture-diagnostics`, or `LUMIN_CAPTURE_DIAGNOSTICS=1` in a
developer capture run. Unset/`0` is off; other environment values and malformed
probe coordinates are refused before creating output. Explicit CLI configuration
takes precedence. `--capture-probe=X,Y` requests one top-left integer pixel; it
requires diagnostics. All scenario capture paths use the same capture adapter,
including sweep cells, standalone benchmark captures and headless captures.
Production play does not enable this facility. Off means no extra attachments,
shader variants, allocations, replay draws, readback, environment reads per frame,
or change to the default render's bytes. Resolve opt-in once at capture setup.

For `view.png` (also for `.ppm`), append suffixes to the complete capture filename:

| File next to capture | Content |
| --- | --- |
| `view.png.albedo.pfm` | Linear unlit RGB, before exposure, tonemap and post |
| `view.png.normal.pfm` | Decoded signed view-space normal XYZ |
| `view.png.depth.pfm` | Raw hardware depth; camera projection and reversed-Z flag in bundle metadata |
| `view.png.material.pfm` | Exact material ID, integral float in 0..255; no palette hashing |
| `view.png.ids.bin` | Lossless RGBA32UI identity plane, never a color PNG |
| `view.png.draws.json` | Capture-local draw and mesh/asset tables, mesh path, chunk coordinate, SDF/heightfield provenance, shader variant and pass |
| `view.png.probe-X-Y.json` | Requested pixel's schema-1 sample; absent when no probe was requested |
| `view.png.diagnostics.json` | Bundle schema, frame/camera, dimensions, attachment encodings, file list, coverage and completion status |

Keep the existing capture manifest and sidecars. Bundle metadata links to that
manifest for build/content identity and to a specific completed frame. Assign draw
IDs 1..N in actual submission order and mesh IDs 1..M in first-submission order;
table entries resolve assets to generic repository-relative paths, never pointers,
GL names, private absolute paths or unordered-map iteration. Repeated instances
share a mesh entry but have different draw IDs. Cap draws and mesh entries at
65,536 each, path strings at 1,024 UTF-8 bytes each, and their combined JSON at
16 MiB; refuse overflow instead of truncating. These are capture-local identities,
not simulation identity inputs. Missing attribution is an explicit unsupported
producer entry/coverage error, never a guessed live draw or silently empty pixel.

ID channels are R=32-bit draw ID, G=32-bit mesh-table ID, B=LOD (0..255),
A=`ownership | (pass << 8)`, with bits 16..31 reserved zero. Ownership is
1=live, 2=legacy far, 3=ladder far; pass is 1=G-buffer, 2=forward, 3=shadow,
4=sky. All-zero means background; every other pixel requires nonzero draw and
mesh IDs and known enums. A pass that cannot supply meaningful LOD must record
its convention in the draw table (LOD zero for non-LOD geometry).
The current geometry AOV/identity plane describes the deferred surface; forward
water/foliage, sky, shadow and transparent contributors need separate labeled
planes or contributor probes. Never claim one ID describes a blended final pixel.

The implemented `ids.bin` wire format is `LVID`, followed by little-endian uint32
version=1, width and height (16-byte header), then four little-endian uint32 words
per pixel, top row first. Native C++ struct layout is never serialized. The PFM
codecs write `PF` for RGB or `Pf` for scalar, dimensions, scale `-1.0`, and IEEE
binary32 little-endian samples, bottom row first as PFM requires. NaN/Inf bits
survive in albedo/normal/depth for analysis; material must be finite and integral.
The adapter must normalize GL's bottom-first readback into the codecs' top-first
input contract and decode normals/materials without a display shader or filtering.
Check integer-attachment and draw-buffer capabilities before allocating; unsupported
hardware refuses diagnostics without changing the normal framebuffer configuration.

Both codecs cap dimensions at 16,384 and total pixels at 16,777,216, validate exact
span sizes before writing and use only 4 KiB scratch storage. Material/ID refusal
validates the entire bounded plane before writing. Work is O(pixels), at most two
plane traversals; probes are O(1). The caller owns stable completed-frame buffers
until serialization finishes. The future adapter should stream one plane at a time
and cap aggregate CPU/GPU staging at 512 MiB before allocation; refuse a bundle
that cannot fit. Do not allocate all full-resolution planes speculatively.

`BuildDiagnosticProbe` emits the following schema (numeric enums match the ID):

```json
{"schema":1,"origin":"top_left","width":3840,"height":1600,"x":100,"y":900,
 "background":false,"id":[42,7,2,259],"draw":42,"mesh":7,"lod":2,
 "ownership":3,"pass":1,"albedo_linear":[0.25,0.5,1],
 "normal_view":[0,1,0],"depth_hardware":0.75,"material":17,"non_finite":false}
```

Nonfinite sample channels become JSON null with `non_finite=true`. Invalid extents,
coordinates, material IDs or encoded identities return an empty string. The caller
must join draw/mesh IDs to the bundle's tables; unresolved entries are errors.
Probe values must come from the exact same pixel and frame as the serialized AOVs.

Publishing is planned as a complete new bundle transaction: validate all inputs and
budgets first, reserve a fresh destination without replacing an existing bundle,
write payloads, check stream/flush/close failures, and publish completion metadata
last. Partial bytes after I/O failure are unpublished; clean up only artifacts this
transaction owns, verifying identity on removal. Codecs take streams and perform
no path checks, opens, deletes or publication themselves. No asynchronous worker
may sample live mutable draw tables. PNG success alone cannot mark diagnostics
complete. Unsupported contributors must be declared in metadata.

## B: views and inspector

Keep `DebugViewPass` mode numbers 0=off, 1=albedo, 2=normal, 3=depth,
4=material and 5=position. Add stable named modes in the console and CLI:

| Modes | Inputs and interpretation |
| --- | --- |
| LOD/chunk tint; draw/mesh ID; far/live ownership | Exact ID plane plus draw table; unsupported producers shown with a distinct hatch |
| Mesh path: SDF vs heightfield | Draw-table provenance, including unknown explicitly |
| Shadow cascade | Actual cascade selection and shadow UV/texel footprint |
| Sky visibility only; AO only; lighting only | Isolate sky visibility, SSAO/material AO, and pre-albedo light accumulation separately |
| NaN/Inf highlight | Scan diagnostic attachments before post; magenta NaN, cyan Inf, preserve raw values |
| Overdraw | Opt-in additive count replay, depth policy labeled; never inferred from final color |
| Mip/texel density; triplanar weights | Explicit instrumented shader variants, sampled mip and normalized XYZ blend weights |
| Wireframe over shaded | Existing wireframe toggle extended to a depth-tested overlay; restore polygon state |

Use the developer console's widget registry and `RenderControlWidgets` adapters for
a view selector, legend, attachment picker,
freeze-frame button and pixel inspector. Inspect by clicking the viewport while
the inspector is armed; convert window/DPI coordinates to attachment coordinates
once, label render versus output resolution, and show an unfiltered nearest texel
zoom. Show draw, mesh path, chunk, LOD, owner, pass, material, signed normal, depth,
linear albedo and any unsupported fields. Freeze retains the image, camera and
draw table together; resizing invalidates a pending selection. Copy the schema-1
JSON or request a bundle at that point. No GPU stalls/readbacks while closed;
limit inspection to one pixel per user request and one retained frozen frame.

## C: Linux shader lane

Add steps to the fast Linux lane on GitHub-hosted public-repository runners with
zero extra billing. Install `glslang-tools` from apt. Compile on **every PR**,
including documentation-only changes; this step must sit outside the native path
gate. Do not add a required check, change rulesets, compiler/linker flags, build
types or cache keys. Missing compiler or unclassified shader files fail the lane.

Inventory every `res/shaders` file: `.vert`→vert, `.frag`→frag, `.geom`→geom,
`.comp`→comp, `.tesc`→tesc, `.tese`→tese; `.glsl` fragments must be covered by
registered consumers in every applicable stage/variant. Fail on an orphan fragment
or unknown extension. Validate desktop OpenGL GLSL with `glslangValidator -S STAGE`
(no Vulkan rewrite). Shader manifest entries list all shipping define sets,
including default and `LUMINUMBRA_FAR_WORLD_LADDER 1` paths. Expand quoted includes
relative to the including file with the exact `Shader.cpp` recursive semantics,
cycle/malformed-include refusal and line-ending handling; inject defines after
the version using `InjectShaderDefines` semantics for both initial load and reload.
Share extraction/preparation with the engine, or enforce parity using real
prepared-source fixtures; do not substitute glslang's different include rules.
Compile every variant, then link registered stage pairs to check interfaces.
Report source/variant/stage and compiler diagnostics, not only exit status.

In the same fast lane, path-gate software-GL scenes on `res/shaders/**`,
`src/luminumbra_client/rendering/**`, relevant rendering tests, scene fixtures,
shader harness tools and workflow/classifier changes. Unknown classification or a
failed classifier is an error. Artifact upload/compare steps carry the same gate.
Use `tools/static_preview/run_software_gl.py` for Xvfb and forced llvmpipe; verify
the GL renderer. Tiny 128x128 scenes isolate one shader effect with fixed textures,
uniforms, seed, camera and no simulation, wall clock, temporal history or exposure.
Include material blend, alpha cutout, normal decode, reversed-Z, sky horizon and
post-edge fixtures, plus a compute SSBO readback fixture. No native client smoke
test is needed to compile shaders in Linux CI.

Planned references live in `test/rendering/shader_scenes/references/` with a manifest
recording source/define/input hashes, Mesa renderer/version and color encoding.
Compare linear RGB: mean absolute error <= 0.005 and 99th-percentile absolute
channel error <= 0.02 on each fixture's explicit mask; NaN/Inf always fails.
Hard categorical/depth invariants use exact IDs or documented float epsilon,
never a loose image score. Regeneration command after the harness lands:
`python3 -I -B tools/gates/shader-scenes.py regenerate --manifest test/rendering/shader_scenes/scenes.json`.
Regeneration requires review of diffs and asset-inventory refresh; it is never an
automatic response to CI failure. Cap fixture count at 64, includes at 64 levels
and 8 MiB expanded source per translation unit; refuse budgets before compilation.

## D: capture checks

These are proposed additional thresholds, not changes to existing gates or baselines.
Require complete, matching-frame bundles before a check is eligible. Pinned masks
and scenario expectations select meaningful regions; unknown/missing data is an
error, never a pass. Analyze raw channels before color transforms. Apply O(pixels)
bounded passes, not repeated flood fills for every pixel.

| Check | Initial threshold | False-positive control |
| --- | --- | --- |
| NaN/Inf; malformed IDs; unresolved draw table | Zero invalid samples/IDs | Background mask only excludes channels explicitly undefined there |
| Terrain holes / far-live gap | Zero background IDs in a pinned expected-terrain mask eroded by 2 pixels | Exclude known cave openings, water and silhouette boundary; require residency first |
| Expected water | At least 1% water-material pixels in the pinned water ROI | Only water-target scenarios, with actual meshed water readiness |
| LOD/ownership coverage | At least one LOD0 pixel in near-ground ROI; zero ownership outside declared draw coverage | Per-scenario camera/radius expectations; never infer owner from color |
| Post edge garbage | Border residual p99 <= 8/255 against the local inward extrapolation | Fixed no-jitter scene; exclude real silhouette edges with depth/ID mask |
| Foliage alpha / impostor cards | Alpha atlas coverage strictly between 1% and 99%; zero depth-writing pixels at alpha < cutout threshold | Asset-specific opaque exceptions must be explicit fixtures |
| Texture detail / triplanar weights | Existing detail floors unchanged; weights sum within 0.001 and each in [0,1] | Use albedo on LOD0, mask weather/particles; do not let rain satisfy detail |
| Weather response | Existing overcast/streak thresholds unchanged; dry versus wet ground mean difference >= 1/255 | Same frame inputs, TOD and camera; lightning disabled for storm brightness comparison |
| Night / stars / cloud composition | Daytime star contribution=0; below-horizon stars=0; cloud transmittance in [0,1] | Instrumented contribution channels; do not mistake ambient motes for stars |
| Seams / shadows / light isolation | Reference-scene MAE <= 0.005 and p99 <= 0.02 | Pinned Mesa scene, linear light, explicit sky/cascade masks; production captures initially report metrics |

Checks emit metric, ROI, threshold, attachment and worst pixel probe so failure
identifies the draw/asset/pass. First integrate as additional diagnostic evidence;
qualify new production-image thresholds on repeated captures before enforcement.
Keep `diff-render-health.ps1`, native shader-health checks and current thresholds.

## Ordered delivery

Each row is one reviewable change of at most ten files. Paths below are explicit;
`rendering/` means `src/luminumbra_client/rendering/`, `scenarios/` means
`src/luminumbra_client/core/scenarios/`. New files are proposed. Repeated matrix
and generated-document edits are local inventory updates. Coordinate shared-file
edits when parallel branches join. No new required CI checks are part of any row.

| Order | Files (count) | Dependency and ownership |
| --- | --- | --- |
| A1: independent readback codecs | `docs/development/visual-diagnostics.md`; `rendering/DiagnosticCapture.h`; `rendering/DiagnosticCapture.cpp`; `test/rendering/diagnostic_capture_test.cpp`; `test/CMakeLists.txt`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (7) | This change. Avoids every owned file. Full capture integration cannot bypass owned draw producers. |
| A2: ID targets and draw registry | `rendering/DiagnosticTargets.h`; `rendering/DiagnosticTargets.cpp`; `rendering/DiagnosticDrawTable.h`; `rendering/DiagnosticDrawTable.cpp`; `test/rendering/diagnostic_targets_test.cpp`; `test/rendering/diagnostic_draw_table_test.cpp`; `test/CMakeLists.txt`; `src/luminumbra_client/sources.cmake`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After A1. Lazy integer attachment lifecycle, bounded registry and GL-state restoration; avoids owned files. |
| A3: exact deferred attribution | `rendering/passes/GBufferPass.cpp`; `rendering/GBufferTargets.cpp`; `rendering/GBufferTargets.h`; `res/shaders/g_buffer.frag`; `res/shaders/foliage.frag`; `rendering/FarLodSystem.cpp`; `test/rendering/diagnostic_attribution_test.cpp`; `test/CMakeLists.txt`; `config/public-asset-inventory.json`; `docs/development/visual-diagnostics.md` (10) | After A2; coordinate owned files with minimal local hunks. Opt-in MRT shader variants mirror actual alpha/depth/LOD ownership; no approximate geometry replay. |
| A4: scenario bundles and probes | `rendering/DiagnosticCapture.h`; `rendering/DiagnosticCapture.cpp`; `rendering/RenderPipeline.h`; `rendering/RenderPipeline.cpp`; `src/luminumbra_client/core/ScenarioRunnerCapture.cpp`; `src/luminumbra_client/core/RuntimeScenarioConfig.cpp`; `test/rendering/diagnostic_bundle_test.cpp`; `test/CMakeLists.txt`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After A3. Shared readback/transaction adapter and once-only opt-in; coordinate owned scenario file. Extend capture_manifest_test for the GL-free adapter; use the client source manifest updated with the codecs in A2. |
| B1: ID views and inspector | `rendering/passes/DebugViewPass.h`; `rendering/passes/DebugViewPass.cpp`; `res/shaders/debug_view.frag`; `src/luminumbra_client/debug/widgets/RenderControlWidgets.cpp`; `src/luminumbra_client/app/RegisterDeveloperWidgets.cpp`; `rendering/RenderPipeline.cpp`; `test/rendering/diagnostic_inspector_test.cpp`; `test/CMakeLists.txt`; `config/public-asset-inventory.json`; `docs/development/visual-diagnostics.md` (10) | After A4. ID/LOD/chunk/ownership and mesh-path views, single-pixel inspector; avoids listed owned files. |
| B2: shader contribution views | `rendering/DiagnosticContributions.h`; `rendering/DiagnosticContributions.cpp`; `rendering/passes/DebugViewPass.cpp`; `res/shaders/debug_view.frag`; `res/shaders/lighting_pass.frag`; `res/shaders/g_buffer.frag`; `test/rendering/diagnostic_contributions_test.cpp`; `test/CMakeLists.txt`; `config/public-asset-inventory.json`; `docs/development/visual-diagnostics.md` (10) | After B1. Remaining view catalogue, instrumented opt-in variants and overlay state. Coordinate owned shaders. Forward/sky/shadow contributor planes remain separately labeled until their producers are instrumented. |
| C1: every-PR shader compile | `tools/ci/compile_shaders.py`; `tools/ci/test_compile_shaders.py`; `tools/ci/shader_variants.json`; `tools/ci/ci_paths.py`; `.github/workflows/ci.yml`; `src/luminumbra_client/rendering/ShaderSource.h`; `src/luminumbra_client/rendering/ShaderSource.cpp`; `src/luminumbra_client/rendering/Shader.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md` (10) | Independent of A/B. Share include preparation and register preprocessing/variant regression. Avoids owned files. Every-PR compile is unconditional within the fast job. |
| C2: llvmpipe scenes | `tools/gates/shader-scenes.py`; `tools/gates/test_shader_scenes.py`; `test/rendering/shader_scenes/scenes.json`; `test/rendering/shader_scenes/references/material.ppm`; `test/rendering/shader_scene_render_test.cpp`; `test/CMakeLists.txt`; `tools/ci/ci_paths.py`; `.github/workflows/ci.yml`; `config/public-asset-inventory.json`; `docs/development/visual-diagnostics.md` (10) | After C1. First reference/metric/regeneration vertical path; extend the scene catalogue in later bounded changes. Avoids owned files; reference and consumer share one path gate. |
| D1: bundle/ID/AOV validity checks | `tools/gates/check-diagnostic-capture.py`; `tools/gates/test_diagnostic_capture.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `tools/gates/diff-render-health.ps1`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | After A4; parallel with B/C. Registered Python reader tests cover little endian, dimensions, partial payloads, unknown versions and nonfinite values. Avoids owned files. |
| D2: qualified image metrics | `tools/gates/check-diagnostic-capture.py`; `tools/gates/test_diagnostic_capture.py`; `tools/gates/baselines/diagnostic-rois.json`; `tools/gates/build-visual-sweep-montages.py`; `tools/gates/test_diagnostic_rois.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After D1 and relevant C2 fixtures/B2 channels. Coverage/weather/edge metrics and labeled worst-pixel montages; qualify enforcement separately. Avoids owned files. |

Start A, B interface design, C and D fixture design together; GPU attribution is the
critical dependency for trustworthy production pixel claims. Forward/transparency
producer coverage and additional shader references proceed as subsequent bounded
changes using the same formats, with coverage gaps visible until implemented.
