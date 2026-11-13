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
| Lazy integer target and capture-local draw registry | Implemented as standalone helpers | `DiagnosticTargets.h`; `DiagnosticDrawTable.h`; registered `DiagnosticTargetDriver.*` and `DiagnosticDrawTable.*` regressions in `capture_manifest_test`; native execution pending |
| Transactional diagnostic MRT and deferred shader variants | Partially implemented | `GBufferTargets.h/.cpp` adds a render-thread capture scope, attachment-six transaction, bounded draw-buffer restoration and explicit unsupported records; `g_buffer.frag` and `foliage.frag` have additive `LUMIN_DIAG_IDS` outputs; registered `DiagnosticAttribution.*` source-contract regressions in `capture_manifest_test`; native execution pending |
| Exact producer coverage | Partial | File-backed static instances and skinned draws upload capture-local identities at submission. Legacy far terrain/water upload the actual tier with unsupported generated-mesh metadata. Live terrain MDI, ladder backend, authored surfaces and tree impostors explicitly invalidate coverage; procedural static meshes also remain unsupported. Later plant/decal stages are outside the scope. Foliage has a guarded shader output but no instrumented forward submission or separate contributor plane |
| Bundle transactions and opt-in admission | GL-free core implemented; runtime integration pending | `DiagnosticCapture.h/.cpp` resolves options, streams normalized planes, joins draw records, enforces staging budgets and publishes completion last using identity-checked ownership. `diagnostic_bundle_test` covers public transports and filesystem refusal/rollback; native execution pending. Runtime setup refuses enabled diagnostics before output until the completed-frame adapter is integrated |
| ID views and single-pixel inspector | Implemented as explicit render-thread snapshots | `DebugViewPass`; developer-console debug-view panel; registered `DiagnosticInspector.*` and `DiagnosticInspectorGl.*` regressions in `pass_context_test`; native execution pending. Other B views and viewport picking remain planned |
| Deferred shader contribution views | Implemented as opt-in live overlays | `DebugViewPass` and `LightingPass`; registered `DiagnosticContributions.*` and `DiagnosticContributionsGl.*` regressions in `pass_context_test`; native execution pending. Fog and separate forward/sky/shadow planes explicitly unsupported |
| Linux shader compilation and software-GL reference scenes | Planned | Fast Linux workflow steps; no required-check or ruleset changes |
| Automated image checks | Planned | Bundle-aware checks alongside existing render-health gates |

The GL-free bundle writer can publish supplied immutable completed-frame planes.
Runtime scenario capture does not yet supply those planes or enable diagnostics.
The capture test target uses modeled GL entry points for target lifecycle tests;
rendering calls these helpers only inside an explicitly constructed
`GBufferDiagnosticCapture`. The developer-console identity snapshot constructs that scope only on explicit refresh; scenario bundle integration remains separate.
Diagnostic options are admitted once
in `ParseRuntimePathOptions`, before save-root creation: malformed input is refused,
and enabled diagnostics receive an explicit unavailable-adapter error. The pure
resolver accepts valid CLI/environment options for the pending adapter. The client
source manifest includes the codecs.
The helpers are header-only to keep the stacked change within ten files.
The acceptance matrix records this partial state. No new runtime asset is added.
Shader files and this development document are outside the roots classified by
`public-asset-inventory.json`; regenerating it retains the existing classified assets.

The capture adapter must supply a fresh draw table and reusable `DiagnosticTargets`,
then keep one capture scope alive around one deferred frame. Attachment setup occurs
after the normal clear, checks the actual G-buffer dimensions and integer/MRT
capability, and restores all draw buffers, the identity color mask and the previous draw framebuffer on
detach or incomplete-FBO refusal. A resize during attachment detaches before the
framebuffer is destroyed and records unsupported coverage. A mismatched extent,
occupied attachment, missing target capability or failed variant is an explicit
unsupported record. The adapter must refuse publication unless `complete()` is
true after detachment; an incomplete plane can contain undefined IDs from a producer
without an instrumented fragment output and cannot be interpreted as background.
`complete()` covers only `GBufferPass`; the adapter must also account for later
plant/decal writes and separate forward contributors before publishing a bundle.
Capture scopes cannot nest or reuse a nonempty draw table. Exception unwinding
invalidates the frame; an empty live-terrain submission adds no unsupported record.

Only an active attached scope compiles diagnostic programs, capped at six deferred
vertex/ladder combinations. Default and ladder variants retain the existing alpha
discard and depth output; identities are integer uniform uploads to attachment six.
During capture, static instances are submitted once each using their existing
attributes and base-instance offset, so repeated instances get separate ordered
draw IDs without replay. Resolved mesh paths determine mesh LOD, including fallback
to the base mesh. The existing 65,536-entry registry limit refuses excessive capture
submissions. Normal rendering keeps its batched draws and original attachments.

Completing exact coverage requires changes beyond the deferred attachment owners:
the live MDI origin/identity transport in `RenderPipeline.cpp`, the ladder submission
metadata/backend, `StaticDrawPass`, `PlantProcgenPass`, `GroundDecalPass`,
`FoliagePass` and `tree_impostor.frag`. A foliage
identity belongs to a separate forward contributor plane because its color blends
with the already lit scene. None of these gaps is represented as a guessed mesh or
as successful background coverage.

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
until serialization finishes. The bundle core streams one plane at a time and
caps aggregate CPU/GPU staging
at 512 MiB before image allocation. Callers must account for all retained GPU and
source/output CPU staging, including the draw registry; the core additionally
reserves one largest plane, 4 KiB codec scratch and 64 MiB bounded serialization
workspace. It preflights each source plane before reservation, then reads each
again during serialization; the completed-frame source must remain immutable.
No full multi-plane frame is allocated. The GL adapter must check capabilities
and this budget before allocating diagnostic GPU storage.

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

The GL-free writer implements a complete new bundle transaction: validate inputs and
budgets first, reserve a fresh destination without replacing an existing bundle,
write payloads, check stream/flush/close failures, and publish completion metadata
last. Partial bytes after I/O failure are unpublished; clean up only artifacts this
transaction owns, verifying identity on removal. Codecs take streams and perform
no path checks, opens, deletes or publication themselves. No asynchronous worker
may sample live mutable draw tables. PNG success alone cannot mark diagnostics
complete. Unsupported contributors must be declared in metadata.

The remaining scenario integration must keep one capture scope around the exact
completed frame, account for later deferred writes and forward contributors,
restore GL readback/pack state, decode the actual G-buffer attachments and link
the existing completed capture manifest. All sweep callbacks, standalone benchmark,
headless, timed-scenario and motion captures must use that adapter. No producer
coverage is inferred by the transaction core. This split keeps the GL-free changes
within the delivery file budget; no runtime capture success is claimed yet.

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

### ID snapshot controls

In the developer console's Debug views panel, enable **Identity diagnostics**.
Named snapshot modes are draw-id (6), mesh-id (7), lod (8), chunk (9),
producer-owner (10) and mesh-path (11); modes 0 through 5 retain their existing
meaning. Refresh captures one render frame's ID plane and draw table together.
The view remains frozen until the next explicit refresh; moving the camera does
not change the retained IDs. Resizing hides an incompatible snapshot until refresh.
The same six names are accepted by `--debug-view`; selecting one explicitly
enables identity diagnostics and requests one snapshot. Existing CLI modes retain
their meaning.

Magenta hatching labels zero, unresolved and unsupported identities. Producer
colours are terrain brown, impostor cyan, foliage green, static/skinned yellow and
far volume blue; live ownership has full brightness, legacy far 70% and ladder far
45%. Mesh-path provenance uses hashed mesh colours, orange SDF, blue heightfield
and explicit magenta hatching for unknown provenance. Producer spellings outside
the known renderer families remain unsupported. This does not expand producer coverage.

Open **Single-pixel inspector**, enter top-left render-attachment coordinates and
press **Refresh snapshot / inspect selected pixel**. The panel displays schema-1
probe JSON and the exact joined draw/mesh records (including path, chunk, LOD,
owner, pass and supported status). Albedo is linear, normal is signed view-space,
depth is raw hardware depth and material is the exact normal attachment alpha ID.
One fragment gathers nearest, unfiltered values at that coordinate into a fixed
48-byte buffer; one buffer readback occurs per explicit valid request while the
inspector is open. Closing the panel closes the inspector. Closed, idle, disabled,
invalid-coordinate and frozen-replay paths perform no inspector readback.

Snapshot registry preparation is capped by 65,536 draws and 16 MiB schema-1 JSON;
its GPU lookup contains at most 131,074 RGBA32UI texels. No registry scan runs on
idle frames. Targets and shader variants are created only on explicit diagnostic
refresh, never on the default rendering path. A competing capture scope refuses
the snapshot request until that scope releases ownership. Viewport/DPI picking,
AOV image freezing/zoom, extra attachment selection and bundle requests are later
view/inspector work; coordinate inspection and identity-image freezing are available here.

### Deferred contribution controls

Use the existing **Identity diagnostics** switch and **Identity snapshot view**
selector for the extended catalogue, or pass the exact name to `--debug-view`.
F6 visits all enabled diagnostic modes after the six legacy modes; mode numbers
0 through 11 retain their meanings. The panel status supplies the selected term's
legend, transfer function and coverage label. Contribution overlays are live and
read the current deferred surface, independently of the frozen identity snapshot.
They do not populate the single-pixel inspector or capture bundles.

| Mode | Name | Meaning |
| --- | --- | --- |
| 12 | direct-sun | Sun BRDF with actual shadow, cloud attenuation and transmission tint |
| 13 | direct-moon | Wrapped lunar diffuse and specular, softened shadow and lunar colour transform |
| 14 | sky-ambient | Ambient diffuse and integrated environment specular, applied AO and sky visibility |
| 15 | shadow-factor | Sun shadow by day, softened lunar shadow when the lunar term is active; white is lit |
| 16 | ao | Applied AO: SSAO, multiplied by material AO for authored surfaces only; studio starts at one |
| 17 | emissive | Crystal, field, marker and authored emission before exposure |
| 18 | fog-atmosphere | Unsupported: atmosphere/fog producer is not instrumented |
| 19 | specular | Sun, lunar, point and environment specular with their actual factors |
| 20 | roughness | Effective lighting roughness after clamp and snow adjustment |
| 21 | metallic | Deferred metallic channel |
| 22 | normal-view | Decoded signed view normal, displayed as XYZ times 0.5 plus 0.5 |
| 23 | normal-world | Actual transformed world normal with the same encoding |
| 24 | sky-visibility | Screen-space sky visibility; one when the feature is disabled |
| 25 | material-ao | Material AO with the signed far-surface tag decoded |
| 26 | lighting | Pre-albedo direct diffuse irradiance divided by PI from sun, moon and point lights, before BRDF and lunar colour transform |
| 27 | forward-plane | Unsupported: separate forward contributor plane |
| 28 | sky-plane | Unsupported: separate sky contributor plane |
| 29 | shadow-plane | Unsupported: separate shadow producer plane |

RGB light terms use the display transfer `max(x,0)/(1+max(x,0))`; scalar material
and visibility values display directly. Storage remains linear RGBA16F before
exposure, tonemapping and post. NaN is magenta and infinity cyan. Magenta hatching
means unsupported or unavailable, including sky/background, stale mode, extent
mismatch and frozen replay. If the overlay variant itself is unavailable, the
existing unknown-mode magenta sentinel replaces the scene. The deferred ambient term is not a sky contributor
plane, and the sampled shadow factor is not a shadow producer identity.

Only an enabled supported contribution mode compiles `LUMIN_DIAG_CONTRIBUTIONS`
and attaches one extra lighting output. It shades during the existing lighting
draw, with no geometry replay or readback. The target is bounded to 16,384 per
axis and 16,777,216 pixels (128 MiB), validated before allocation. Each active frame
clears that bounded target to unsupported before drawing; no new world traversal
is added. Attachment collisions, capability/extent/storage failures and invalid
variants leave the plane unavailable. The attachment transaction restores draw
buffers, its colour mask and indexed blending, and detaches before later passes.
Disabling diagnostics performs no contribution allocation or compilation; retained
storage is released on resize, renderer teardown or shader reset. The default
lighting source outside additive guards is byte-for-byte preserved; the G-buffer
shader is unchanged because it already stores roughness, metallic and AO. Registered
source fingerprints enforce both contracts. Development prose, shaders and C++
files remain outside the public asset inventory's classified roots; regeneration
therefore leaves its classified contents unchanged.

Shadow cascade footprint, sampled mip/texel density, triplanar weights, overdraw
and a depth-tested wireframe overlay remain subsequent instrumentation work.
No forward, atmosphere, sky or shadow producer coverage is inferred here.

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
| B1: ID views and inspector | `rendering/passes/DebugViewPass.h`; `rendering/passes/DebugViewPass.cpp`; `res/shaders/debug_view.frag`; `src/luminumbra_client/debug/widgets/RenderControlWidgets.cpp`; `src/luminumbra_client/app/RegisterDeveloperWidgets.cpp`; `rendering/RenderPipeline.cpp`; `test/rendering/diagnostic_inspector_test.cpp`; `test/CMakeLists.txt`; `src/luminumbra_client/main_client.cpp`; `docs/development/visual-diagnostics.md` (10) | After A4. Named CLI/panel ID/LOD/chunk/ownership and mesh-path snapshots, single-pixel inspector; avoids listed owned files. |
| B2: shader contribution views | `rendering/passes/DebugViewPass.h`; `rendering/passes/DebugViewPass.cpp`; `rendering/passes/LightingPass.h`; `rendering/passes/LightingPass.cpp`; `res/shaders/debug_view.frag`; `res/shaders/lighting_pass.frag`; `test/rendering/diagnostic_contributions_test.cpp`; `test/rendering/diagnostic_inspector_test.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md` (10) | Implemented after B1. Live deferred contribution and material views, opt-in variants and overlay state; existing G-buffer material channels reused unchanged. Fog and forward/sky/shadow planes remain separately labelled unsupported. Other catalogue instrumentation remains planned. |
| C1: every-PR shader compile | `tools/ci/compile_shaders.py`; `tools/ci/test_compile_shaders.py`; `tools/ci/shader_variants.json`; `tools/ci/ci_paths.py`; `.github/workflows/ci.yml`; `src/luminumbra_client/rendering/ShaderSource.h`; `src/luminumbra_client/rendering/ShaderSource.cpp`; `src/luminumbra_client/rendering/Shader.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md` (10) | Independent of A/B. Share include preparation and register preprocessing/variant regression. Avoids owned files. Every-PR compile is unconditional within the fast job. |
| C2: llvmpipe scenes | `tools/gates/shader-scenes.py`; `tools/gates/test_shader_scenes.py`; `test/rendering/shader_scenes/scenes.json`; `test/rendering/shader_scenes/references/material.ppm`; `test/rendering/shader_scene_render_test.cpp`; `test/CMakeLists.txt`; `tools/ci/ci_paths.py`; `.github/workflows/ci.yml`; `config/public-asset-inventory.json`; `docs/development/visual-diagnostics.md` (10) | After C1. First reference/metric/regeneration vertical path; extend the scene catalogue in later bounded changes. Avoids owned files; reference and consumer share one path gate. |
| D1: bundle/ID/AOV validity checks | `tools/gates/check-diagnostic-capture.py`; `tools/gates/test_diagnostic_capture.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `tools/gates/diff-render-health.ps1`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | After A4; parallel with B/C. Registered Python reader tests cover little endian, dimensions, partial payloads, unknown versions and nonfinite values. Avoids owned files. |
| D2: qualified image metrics | `tools/gates/check-diagnostic-capture.py`; `tools/gates/test_diagnostic_capture.py`; `tools/gates/baselines/diagnostic-rois.json`; `tools/gates/build-visual-sweep-montages.py`; `tools/gates/test_diagnostic_rois.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After D1 and relevant C2 fixtures/B2 channels. Coverage/weather/edge metrics and labeled worst-pixel montages; qualify enforcement separately. Avoids owned files. |

Start A, B interface design, C and D fixture design together; GPU attribution is the
critical dependency for trustworthy production pixel claims. Forward/transparency
producer coverage and additional shader references proceed as subsequent bounded
changes using the same formats, with coverage gaps visible until implemented.
