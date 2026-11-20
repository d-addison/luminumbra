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
| Capture-local draw registry | Implemented with separate asset and generated-geometry identities | `DiagnosticDrawTable.h`; registered `DiagnosticDrawTable.*` regressions in `capture_manifest_test` and generated-key `DiagnosticAttribution.*` regressions in `render_smoke_test`; native execution pending |
| Deferred attribution producers | Implemented inside a leased deferred scope | `GBufferTargets.h` and `GBufferPass.cpp` add a render-thread producer scope, capture-local draw identities and explicit unsupported records; `g_buffer.frag` and `foliage.frag` have additive `LUMIN_DIAG_IDS` outputs; registered `DiagnosticAttribution.*` source-contract and `DiagnosticAttributionGl.*` hidden-GL regressions in `render_smoke_test`; native execution pending |
| G-buffer ID attachment | Implemented for explicit deferred captures | `GBufferPass` holds an `AttachmentLease` during deferred submission; detachment and lease release precede completion, including rollback, recreation, retirement and exceptional exits; registered `GBufferLeaseGl.*` and `DiagnosticAttributionGl.*` regressions; native execution pending |
| Exact producer coverage | Partial | Inside an explicit producer scope, file-backed static instances and skinned draws upload capture-local identities at submission. Legacy far terrain/water upload the actual tier with unsupported generated-mesh metadata. Live terrain MDI, ladder backend, authored surfaces and tree impostors explicitly invalidate coverage; procedural static meshes also remain unsupported. Later plant/decal stages are outside the scope. Foliage has a guarded shader output but no instrumented forward submission; the separate contributor attachment API is unused |
| Bundle transactions and opt-in admission | GL-free core implemented; runtime integration pending | `DiagnosticCapture.h/.cpp` resolves options, streams normalized planes, joins draw records, enforces staging budgets and publishes completion last using identity-checked ownership. `diagnostic_bundle_test` covers public transports and filesystem refusal/rollback; native execution pending. Runtime setup refuses enabled diagnostics before output until the completed-frame adapter is integrated |
| Debug views and inspector | Planned | Extend `DebugViewPass` and developer console widgets |
| Linux shader compilation and software-GL reference scenes | Partial | Fast Linux steps and material, cutout, night and terrain fixtures registered; bounded sky/terrain shader replays checked under llvmpipe; native scene target execution pending; dusk continuity diagnostic pending issue #918; no required-check or ruleset changes |
| Bundle validity and image checks | Implemented for schema 1 and reader-only schemas 2/3 | `check-diagnostic-capture.py` and registered `DiagnosticCapturePython` synthetic regressions validate bundles without a renderer. ROI coverage, validity, weather, edge and masked defect measurements plus worst-pixel SVG crops are implemented; synthetic detection is covered, while production thresholds remain advisory/unqualified |
| Capture feature profile and capability manifest | Implemented as GL-free setup logic; app wiring pending | `CaptureFeatureProfile.h/.cpp`; strict `--capture-feature-profile=none\|all_features` runtime selection; registered `CaptureFeatureProfile.*` regressions; native execution pending |
| Temporal audit plans and admission | GL-free planning and sequence execution protocol implemented; shipping runtime execution refused | `TemporalAudit.h/.cpp`, `RuntimeScenarioConfig.h/.cpp`; registered `TemporalAudit.*` and `TemporalAuditSequence.*` regressions in `capture_manifest_test`; app/site transport integration and native capture pending |
| Temporal sequences and non-vacuous evidence (E1–E3) | Planned; gap inventory below describes current capture code | Exact-tick site execution, temporal metrics, shared producer policy and non-vacuous feature evidence require implementation; existing captures do not establish this coverage |

The GL-free bundle writer can publish supplied immutable completed-frame planes.
Runtime scenario capture does not yet supply those planes or enable diagnostics.
The capture test target uses modeled GL entry points for target lifecycle tests;
rendering submits attribution only inside an explicitly constructed
`GBufferDiagnosticCapture`. No runtime option constructs that scope yet.
Diagnostic options are admitted once
in `ParseRuntimePathOptions`, before save-root creation: malformed input is refused,
and enabled diagnostics receive an explicit unavailable-adapter error. The pure
resolver accepts valid CLI/environment options for the pending adapter. The client
source manifest includes the codecs.

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
Shader files are outside the roots classified by `public-asset-inventory.json`, so
their guarded edits do not change that inventory.

An explicitly constructed `GBufferDiagnosticCapture` accepts a fresh draw table and
optional caller-owned `DiagnosticTargets` and distinct forward target storage; all
owners outlive the scope on its
creating GL context thread. Construction alone performs no GL work. A table-only
scope cannot attach or complete. `GBufferPass` attaches location six after the
ordinary clear and holds a move-only `AttachmentLease` through deferred submission.
Detachment restores all saved draw buffers and the integer color mask before
releasing storage, including rollback, recreation, registry retirement and exception
unwinding. Resize, reset and disable refuse while leased, even on an unbound FBO.
Capability, extent and occupied-attachment refusals preserve caller state and record
unsupported coverage. Scope nesting and nonempty-table reuse are refused.

`complete()` requires a successful attachment, subsequent detachment, lease release,
complete deferred coverage and no sticky failure or registry refusal. Variants and
identity writes only operate while attached. Missing or invalid base programs
invalidate coverage, including an unavailable skinned program with an empty view.
An isolation mask that filters the terrain or skinned layer invalidates coverage,
whether or not the filtered layer has scene content.
Queued tree impostors invalidate coverage before checking their program, so an
invalid impostor program cannot bypass the unsupported record. Empty terrain and
impostor queues add no unsupported record. Later deferred and forward contributors
remain outside this coverage; this predicate alone cannot authorize a final bundle.

The contributor API can attach a distinct forward target after deferred detachment.
It leases the selected storage through the same rollback and release path, preserves
the forward framebuffer's original draw buffers, and enables only the additional
identity output. The caller keeps that framebuffer alive through detachment. Missing
or aliased storage, nested or repeated attachment, invalid extents and mismatched
contributor passes refuse coverage. Current-program writes use the attached
variant; a pass mismatch uploads zero IDs without throwing. No shipping
forward producer invokes this API yet.

Generated geometry can supply an exact bounded key instead of an asset path. The
registry separates the two namespaces and emits a `generated` mesh member while
retaining unknown provenance; ambiguous or invalid keys refuse without mutation.
Registered source contracts cover the contributor API and feedback-variant bounds;
hidden-GL regressions cover its leases, writers and refusals, with native execution
pending.

Only an attached scope compiles diagnostic programs, capped at six
combinations, including separate feedback signatures (at most three names of
64 bytes each). Default and ladder variants retain the existing alpha
discard and depth output. The guarded integer output uses location six.
With diagnostics disabled, removing the conditional blocks reproduces the base
fragment shader text byte for byte.

During collection, static instances are submitted once each using their existing
attributes and base-instance offset, so repeated instances get separate ordered
draw IDs without replay. Resolved mesh paths determine mesh LOD, including fallback
to the base mesh. The existing 65,536-entry registry limit refuses excessive capture
submissions. A failed submission sets the scope's sticky `refused()` flag without
appending another record or throwing. Later writes upload zero IDs and stop
registry work; `complete()` remains false. Static metadata refusals record
unsupported coverage. Unrepresentable index counts omit the draw; an unrecordable
identity path preserves geometry without claiming attribution. Every pass exit
restores caller culling, VAO and separate read/draw framebuffer bindings, including exceptional
producer exits. Registered hidden-GL regressions cover the exact entry bound,
repeated refusal, zero IDs, double-sided foliage, detached writers, completion only
after lease release, unavailable base/skinned/impostor programs and exceptional
state restoration; native execution remains pending. Normal rendering keeps its batched draws and
original attachments.

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

Validate one bundle directory, a completion JSON, or a recursive capture tree with
`python3 -I -B tools/gates/check-diagnostic-capture.py PATH`. Exit 0 means valid,
1 reports an invalid bundle, and 2 means usage or operating-system I/O failure.
The reader refuses unknown schemas, incomplete publication, mismatched file lists,
PFM/ID sizes or dimensions, unresolved identities, implicit unsupported producers
and probes that disagree with any plane at the same top-left pixel. It reports
nonfinite sample counts per plane as invalid diagnostic findings; the wire format
still preserves those values. Explicit unsupported forward records are valid for
the labeled deferred-only surface; unsupported deferred coverage is refused.

Schema 1 lists member names, without stored size fields. Binary sizes are checked
exactly against dimensions before scanning; table/probe JSON is bounded to
16 MiB, completion to 16 MiB plus 64 KiB for its envelope, and capture manifests
to 1 MiB. Plane scans
use at most 4 KiB chunks. JSON integer tokens are capped at 128 characters before
conversion, including on older Python versions. Tree discovery stops at 100,000 entries and refuses
descendant links; a selected root alias is allowed. Reads use verified regular-file
handles on both POSIX and Windows. Completion must be the final file-list member
and cannot be older than any payload. Equal timestamps are allowed for coarse
filesystems; timestamps and schema-1 names cannot prove historical write order or
content integrity after a deliberate rewrite. No checksum or size field is invented.
`diff-render-health.ps1 -CapturePath PATH` optionally invokes this reader when
`PATH.diagnostics.json` exists. Without that sidecar the existing comparison stays
unchanged. No required CI check or runtime capture adapter is added.

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

References live in `test/rendering/shader_scenes/references/` with a manifest
recording source/define/input hashes, Mesa renderer/version and color encoding.
Compare linear RGB: mean absolute error <= 0.005 and 99th-percentile absolute
channel error <= 0.02 on each fixture's explicit mask; NaN/Inf always fails.
Hard categorical/depth invariants use exact IDs or documented float epsilon,
never a loose image score. Regeneration uses the capture and fresh candidate
paths shown in the scene catalogue below. It requires review of diffs and
asset-inventory refresh; it is never an
automatic response to CI failure. Cap fixture count at 64, includes at 64 levels
and 8 MiB expanded source per translation unit; refuse budgets before compilation.

### Scene catalogue

The schema-3 scene manifest inventories four 128x128 PPM references, all eight
consumed source files and the capture translation unit, including its fixed
uniforms and generated textures. Each reference records its own origin: material,
tree cutout and terrain are analytical; night is a surfaceless llvmpipe shader
replay. Native scene target execution remains pending because the local display
server could not bind. Images store linear RGB, bottom-first; sky samples undo
the shader's explicit gamma after the display clamp. The existing binary PPM
attribute covers every reference in this directory. These test assets are outside
the runtime asset inventory.

| Scene | Real shader pair | Metric and tolerance |
| --- | --- | --- |
| `material` | `static_asset.vert/.frag` | Full-image linear RGB MAE <= 0.005 and channel-error p99 <= 0.02; existing exact material assertions retained |
| `tree-cutout` | `tree_impostor.vert/.frag` | Same reference thresholds; maximum RGB error against independent atlas categories <= 0.000001; the GL test checks unchanged background on all six attachments for discarded texels |
| `night-sky` | `enhanced_skybox.frag` with fixed point directions | Same reference thresholds; maximum star RGB at/below horizon <= 0.000001; each of four stellar peaks >= 0.01 luma, above its eight-neighbour ring mean by >= 0.001 and every ring pixel by >= 0.00001 |
| `terrain-blend` | `g_buffer.vert/.frag` | Same reference thresholds; every row has exactly 102 pixels between 10% and 90% Sand; weight backstep <= 0.000001, adjacent weight step <= 0.008, maximum RGB error against the analytical ramp <= 0.000001 |
| `dusk-sky` (diagnostic only) | `enhanced_skybox.frag`, production atmospheric compute kernels | Maximum adjacent column-mean luminance step across all 128 azimuth columns, including wrap; proposed <= 0.005 remains pending issue #918; no reference or passing expectation |

The tree fixture selects one tile from a generated 2x2 atlas. Its 4x4 tile repeats
alpha 0, 0.499, 0.5 and 1, giving exactly 50% discarded pixels. Other tiles are
opaque red to expose incorrect tile selection. Surviving albedo is checked after
coverage division. This isolates the shader's alpha cutoff; it does not qualify
the production atlas bake, binding or distant-tree submission.

Night fixes time at zero and sun direction at (0,-1,0). Atmospheric radiance,
moon and aurora are zero; stars remain enabled. Rows 0..63 cover the complete
azimuth below and exactly on the horizon; the upper half resolves four fixed
3D star cells at 0.02 cell units per pixel. Their independent peak/ring checks
reject absent stars and square plateaus even if image-average errors are small.

Dusk fixes the sun at (0,-0.08,0.9967949), day factor 0.43 and exposure 38.
A full-azimuth panorama samples elevations 0.001..0.019 radians, below the
shader's cloud cutoff and above the sun disc. The three unmodified compute
kernels embedded in `SkyAtmosphereLut.ipp` generate the 256x64 transmittance,
32x32 multiscatter and 192x108 sky-view LUTs with production RGB16F storage and
sampler state. This isolates LUT continuity from clouds. A surfaceless llvmpipe
replay measured a maximum column-mean step of 0.00596312 at columns 111/112;
an experimental bilinear input-sampling replay measured 0.00353783. This is
software evidence of an A17-style discontinuity, not qualification of the
production camera or the separate fix. The proposed 0.005 threshold is recorded
as pending issue #918. Dusk capture remains mandatory and finite, but comparison
and regeneration cannot accept a dusk baseline.

Terrain draws two triangles with Stone on the left vertices and Sand on the
right, exercising the real vertex coverage interpolation across a diagonal with
different provoking materials. A zero texture flag selects fixed reflectances;
macro mottle and rock overlay are isolated out. The expected RGB is
(0.5+0.4t, 0.5+0.3t, 0.5), t=(x+0.5)/128. A llvmpipe replay's maximum RGB
error was below 0.00000005. This qualifies the material-coverage path, not the
terrain mesh producer or textured sand detail.

To compare or regenerate, first configure the existing opt-in scene target with
`LUMINUMBRA_SHADER_SCENES=ON` and build `shader_scene_render_test`. Capture to a
fresh directory using the existing runner (replace the example binary path with
your build's target location):

```sh
python3 -I -B tools/static_preview/run_software_gl.py --binary build/fast-gcc/bin/shader_scene_render_test --profile shader-scenes --output build/scene-capture
python3 -I -B tools/gates/shader-scenes.py compare --capture build/scene-capture
python3 -I -B tools/gates/shader-scenes.py regenerate --capture build/scene-capture --output build/scene-candidates
```

Regeneration checks all five captures, source identities, renderer evidence and
the cutout, night and terrain invariants before creating the candidate directory.
It writes all four reference payloads before `scenes.json`; an I/O failure cannot publish a
complete candidate manifest. Review and copy the candidate manifest and references
together. Their hashes are refreshed in that manifest; rerun the scene contracts
and acceptance/public-tree validators. Never regenerate to bless a shader defect.

## D: capture checks

The reader implements ROI metrics alongside the proposed thresholds below; existing
gates and baselines are unchanged. Complete, matching-frame bundles are required
before measurement. Missing channels or edge bands without adjacent pairs report
`unavailable` with a null value, never zero or a pass. Analyze raw channels before color transforms. Apply O(pixels)
bounded passes, not repeated flood fills for every pixel.

`tools/gates/baselines/diagnostic-rois.json` uses schema 1 with a `rois` list.
Each ROI has a unique `name`, a half-open normalized top-left `rect` of
`[left, top, right, bottom]`, and `metrics`. Pixel bounds round outward using
floor/ceil; every nonempty rectangle touches at least one pixel. Each metric has
`kind`, `channel` and inclusive `[minimum, maximum]` `thresholds`. Optional ROI
`enforce: true` qualifies every metric in that ROI; omitted/false is advisory.
All shipped ROIs are advisory, so threshold misses and unavailable channels add
no failure to current valid captures. Existing bundle validity failures still fail.
Qualified metrics fail on a threshold miss or unavailable result. Use
`--rois CONFIG` to select reviewed scenario-specific expectations. The shipped
regions illustrate near-ground coverage, weather, a dusk sky column band and a
terrain blend row band; they do not establish camera or scenario qualification.

Coverage reports the fraction of all ROI pixels matching an exact `field`/`value`
class in `ids`: background (1=true, 0=false), draw, mesh, LOD, ownership or pass.
Background retains its zero ID fields. Exact material coverage uses `kind: coverage`, `channel: material`,
`field: material` and an integral material `value` in [0,255]; a pinned expected-water
ROI uses [0.01,1] with its scenario's actual water material ID. Missing or invalid
material evidence never passes. Validity reports the fraction whose whole
pixel is finite; depth additionally requires [0,1], normal components [-1,1], and
material integral [0,255]. Albedo validity only requires finiteness. Undefined
background AOVs are not inferred or masked. Weather reports the mean, minimum and
maximum scalar contribution, enforcing the range against every pixel, not just
the mean. Edge `axis: x` compares adjacent columns and `axis: y` adjacent rows,
only inside the ROI; the value is the maximum absolute channel difference.
Coordinates select the second pixel of that pair. These original metrics do not
infer final-color weather. The additional masked
checks below require explicit same-frame evidence for silhouette exclusion and
reference-scene error measurement.

Schema-1 bundles remain unchanged and have no contribution channels. Reader-only
schema 2 retains its precipitation/fog/cloud-only contract. Reader-only schema 3
requires at least one of the additional evidence planes; weather is optional.
Old readers refuse schema 3 explicitly. No save or simulation format changes. Ordered optional
members follow draws, before the optional probe and completion: `precipitation`,
`fog`, `cloud`, `expected_mask`, `reference`, `dry_reference`, `alpha`, `cutout`,
`depth_written`, `triplanar`, `stars`, `transmittance`. Scalar planes use
`linear_contribution_float32`; RGB references use `linear_rgb_float32` and XYZ
triplanar weights use `linear_xyz_float32`. All use the existing PFM reader,
dimension/byte-size/orientation/timestamp and nonfinite checks. Nested documents
retain schema 1. Unknown channels/encodings are refused. No runtime writer supplies
these planes yet; producers must declare matching frame, camera and surface data,
including reference inputs with the same scene settings. A bundle does not prove
reference provenance or repeated-capture qualification.

Additional metric specifications require `mask: "expected_mask"`. The mask is a
pinned binary scalar plane whose meaning is defined by each named ROI; no mask is
guessed from final color. Unknown masks/ROIs are refused. Missing required planes,
empty masks, or no eligible samples report `unavailable`, with null value and no
worst pixel. Mask values outside {0,1} refuse measurement. Scenario masks exclude
known caves/water/silhouettes, particles and unsupported surfaces and require
residency before capture. The shipped rectangles illustrate placement only.

| Kind / channel | Measurement and evidence | Advisory example |
| --- | --- | --- |
| `holes` / `ids` | Background fraction after eroding the expected-terrain mask and ROI boundary by two pixels in both axes; also catches a far/live gap | [0,0] |
| `seam` / `albedo` | Linear RGB MAE and per-pixel RGB mean-error p99 against `reference`; pinned seam/shadow/light-isolation mask | MAE <= 0.005; `p99_limit: 0.02` |
| `boundary` / `albedo` | p99 of maximum RGB residual against inward extrapolation `2*next-next_next`; `axis: x/y`, inward `direction: -1/1`; three selected pixels must have the same nonbackground IDs and depth spread <= 0.01 | p99 <= 8/255; use an explicitly pinned border band |
| `card` / `alpha` | Alpha coverage fraction (`alpha >= cutout`) strictly inside thresholds, plus zero `depth_written` pixels below `cutout`; all three scalar planes required and range checked | coverage strictly (0.01,0.99); zero violations |
| `banding` / `albedo` | Maximum error of adjacent RGB differences against the corresponding `reference` differences along `axis: x/y` | <= 0.02; pinned sky/gradient ROI |
| `wedge` / `albedo` | Maximum absolute RGB error against `reference` inside a pinned expected-sky/geometry ROI; identifies unexpected wedge pixels without guessing shapes from color | <= 0.02 |
| `detail` / `albedo` | Mean maximum RGB adjacent-pixel difference along `axis: x/y`; same nonbackground IDs, continuous depth, LOD0 and zero `precipitation` at both pixels | Example floor 1/255; existing detail gates/floors unchanged |
| `weather_delta` / `albedo` | Mean absolute RGB difference against same-input `dry_reference` in the ground mask | >= 1/255; existing overcast/streak gates unchanged |
| `weights` / `triplanar` | Maximum sum-to-one error or component excursion outside [0,1] | <= 0.001 |
| `composition` / `stars` | Maximum absolute star contribution in a pinned daytime/below-horizon exclusion mask, or `transmittance` excursion outside [0,1] | zero |

Seam and boundary p99 use a fixed 65,536-bin histogram, reporting `p99_upper`,
a conservative upper bound at 1/65,536 resolution (values >= 1 use the exact
maximum). `p99_limit` is required for both. Boundary also reports the exact maximum.
Only the p99 bound, rather than a maximum limit on every outlier, controls boundary
status. Reference checks measure supplied references; they do not claim that native
Mesa scenes or production captures have been qualified. Synthetic defect fixtures
cover isolated holes, seams, boundary bands, cards, bands, wedges, missing weather
response, detail loss, weight and composition errors. Every injected defect fails
exactly its metric and produces a deterministic labelled worst-pixel montage.

Each metric emits a JSON record with ROI, metric specification, pixel rectangle,
`qualification` (`qualified`/`advisory`), `status` (`pass`/`fail`/`unavailable`),
value, sample count and worst-pixel coordinates when available. Nonfinite weather
or edge inputs make that metric unavailable; bundle nonfinite findings still fail.
Worst coverage/validity/weather/detail/response pixels maximize distance outside
the requested interval; residual checks use the largest error. Card violation
pixels take priority over coverage-only failures. Records include the same-pixel
ID and joined draw/mesh metadata when available (background stays explicit).
Ties choose top row
then left column. At most 32 metrics and ROIs, 1 MiB configuration, 16,384 pixels per
axis and 16,777,216 pixels per plane bound the work. Original metric scans use at
most two rows; masked checks cache at most five rows
per required channel (262,144 wire bytes per ID row, 196,608 per RGB row; decoded
tuples add bounded interpreter overhead) and one 512 KiB histogram where needed.
Each metric visits its ROI once; hole erosion examines at most 25 mask neighbors
per pixel and boundary/detail comparisons at most three pixels. At most 32
histogram scans of 65,536 bins add constant bounded work. No simulation path changes.

For a labelled worst-pixel montage, run
`python3 -I -B tools/gates/build-visual-sweep-montages.py --diagnostics CAPTURE.diagnostics.json --output worst.svg`
with optional `--rois CONFIG`. This stdlib-only path validates one bundle before
creating a new output, refuses an existing output and emits up to 32 labelled
33x33 raw-albedo crops centered/clipped around worst pixels, with a yellow pixel
outline and metric value/threshold/qualification/status, p99/violation details
and draw/asset/pass labels when available. Display values clamp to [0,1];
nonfinite pixels are magenta. Unavailable rows carry labels without fabricated
crops. These are raw-albedo evidence, not final-color visual approval. The existing
PNG contact-sheet path retains its optional Pillow dependency. Registered
`DiagnosticChecksPython`, `DiagnosticRoisPython` and `DiagnosticCapturePython`
tests cover metrics, isolated injected defects, refusal,
advisory/qualified results and montage generation without native rendering.

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

## E: temporal audits and all-features captures

Temporal plan admission is implemented, as are the profile selection and GL-free
capability resolver described under E3; sequence execution, E2 and the rest of E3
remain planned. Neither adds a capture guarantee, metric enforcement or CI job.
The inventory below distinguishes current capture behavior from the proposed
all-features contract. Existing specialized gates remain useful and retain their
baselines; passing them does not establish an all-features audit.

### Current capture gap inventory

Evidence paths in this subsection are repository-relative: `client/` means
`src/luminumbra_client/`, `core/` means `src/luminumbra_client/core/`,
`rendering/` means `src/luminumbra_client/rendering/`, and `frontier` means
`tools/gates/validate-engine-frontier.ps1`. Line numbers refer to the code reviewed
for this design. These are all sibling entry paths of the proposed contract;
headless capture is a wrapper around two of them, not a separate renderer.
Windows and POSIX use these same C++ paths; the frontier launch wrapper is
PowerShell and the existing assembly wrapper is Python.

ON means the normal producer is eligible with shipped configuration, not that
anything was visible. OFF means an explicit suppression. FORCED means a capture
pin, reset or fixture replaces the ordinary producer. CONDITIONAL requires content,
configuration, view or a named option. Unknown presence is never evidence of ON.

Shared defaults: `data/common/systems.json:5` enables plant growth, `:6` erosion,
`:38` weather hydrology, `:40` weather events, `:50` tree wind, `:51` procgen plants,
`:55` creature spawning and `:59` live weather. The live bridge feeds weather,
precipitation and clouds before capture overrides (`client/main_client.cpp:5706`);
lightning consumes the sim strike schedule (`:5736`). Ambient motes run in the
ordinary frame path without a `--no-ui` guard (`:6015`). Foliage preparation is
synchronous/complete for captures (`:1039`, `:5565`). Water, rivers, waterfalls,
caves and structures still require suitable generated/authored content and framing;
there is no universal presence test for these modes. Waterfall preparation is part
of world entry (`:4361`); isolation is a separate render/spawn mask
(`core/RuntimeScenarioConfig.h:77`, `client/main_client.cpp:2406`).
Grass can still be disabled by resolved graphics (`rendering/RenderPipeline.cpp:3851`);
isolation can suppress water, foliage, particles and lightning (`:3313`, `:3459`,
`:3522`, `:3546`). ON below assumes default graphics and no isolation override.
Sky/lighting receive cloud/moon inputs through their normal contexts
(`rendering/RenderPipeline.cpp:980`, `:1013`); visibility still depends on content
and camera, not just constructing those passes. Cave-flourish OFF below describes
the automatic scatter: `--debug-goto cave` can still light one framed hero crystal
(`client/app/CaveFlourishes.cpp:158`), without restoring the complete scatter.

| Mode | Creatures / animals | Foliage; caves; structures; water / rivers / waterfalls | Weather / precipitation; clouds / cloud shadows | Lightning; particles | Sky / moon / stars | Weather cycle; several sim days with creatures ticking |
| --- | --- | --- | --- | --- | --- | --- |
| `--timelapse` family (`--timelapse-frames`) | Ambient OFF by default; CONDITIONAL `--timelapse-living` permits wildlife; `--timelapse-creatures` uses ecology spawns/markers (`client/main_client.cpp:5555`, `:5933`; `client/app/ClientAppContext.h:165`). | Foliage ON/complete; optional grow/season are FORCED visual rebakes, simgrow uses live plants (`client/main_client.cpp:1012`, `:1024`). Cave flourishes OFF (`client/app/CaveFlourishes.cpp:75`); cave terrain and structures CONDITIONAL; water/rivers/waterfalls CONDITIONAL, dig/drain/rain alter the demo world (`client/main_client.cpp:892`, `:920`, `:976`). | Live weather, precipitation and cloud/shadow bridge ON if configured (`client/main_client.cpp:5706`). Rain demo FORCES hydrology input then evaporation, not a clear–storm–clear atmosphere script (`:980`, `:5274`). | Live strikes CONDITIONAL on schedule/view (`:5736`); motes/precipitation ON via common path (`:5716`, `:6015`), presence unverified. | ON, visibility CONDITIONAL on TOD/view. Start TOD/daystep options exist (`client/app/ClientAppContext.h:149`, `:159`); final frame has intervening live clock writes (`client/main_client.cpp:5692`). | Natural weather can evolve; no scripted full cycle. YES, extra fixed ticks can span days with opted-in creatures, but are added to normal frame ticks (`client/main_client.cpp:994`, `:5339`); not an exact-tick audit schedule. |
| Frame-scan | Ambient eligible when no scenario is active: the predicate does not exclude frame-scan (`client/main_client.cpp:5555`); assets/species still required (`:5575`). | Foliage ON/complete (`:1044`); cave flourishes OFF (`client/app/CaveFlourishes.cpp:76`); caves/structures/water/rivers/waterfalls CONDITIONAL. Scanner observes a rendered frame, does not enable these (`rendering/FrameScan.cpp:81`). | Live bridge ON/CONDITIONAL (`client/main_client.cpp:5706`); no weather pin in scan camera/TOD block (`:6038`). | Live strikes and common particles CONDITIONAL; no scan-specific guarantee (`:5736`, `:6015`). | FORCED noon `0.04` and fixed pose (`:6041`); sky ON, night moon/star evidence absent by design. | NO cycle or multi-day sequence: settles, writes once, exits (`:6197`, `:6419`). Sim ticks before capture do not prove creature progression over days. |
| World visual sweep | Ambient OFF because scenario active (`client/main_client.cpp:5555`); no sweep creature fixture. | Foliage FORCED empty for upward cameras (`core/scenarios/WorldVisualSweep.cpp:379`). Other views use complete scatter. Cave terrain/structures/water/rivers/waterfalls CONDITIONAL; water-aimed cells require water draws/indices (`:430`). Cave flourish suppression depends on other capture flags, not scenario alone (`client/app/CaveFlourishes.cpp:75`). | FORCED independent clear/storm cells; clouds/shadows ON in both (`core/scenarios/WorldVisualSweep.cpp:455`, `:509`); storm rain ON, clear rain OFF; emitters cleared each cell (`:452`). | FORCED fixed active storm bolt, clear bolt OFF (`:479`, `:503`); particles reset, storm rain emitter added (`:452`, `:473`), other emitters not preserved. | FORCED discrete TOD/season matrix (`:528`); sky/moon/stars CONDITIONAL on cell/view. | NO temporal cycle: independent still weather cells. NO sim days/creature ticks inside synchronous matrix: callback only updates residency and renders (`core/ScenarioRunnerCapture.cpp:141`); settles are renders (`core/scenarios/WorldVisualSweep.cpp:391`). |
| Scenario capture (general) | Ambient OFF (`client/main_client.cpp:5555`); CONDITIONAL scenario-owned rigs. CreatureSlice explicitly probes planner and skinned draws before/after stimulus (`core/ScenarioRunnerCapture.cpp:2055`, `:2074`), not a general wildlife guarantee. | Foliage ON/complete except scenario/isolation overrides; caves/structures/water/rivers/waterfalls CONDITIONAL. Water smoke explicitly waits for water draws (`core/ScenarioRunnerCapture.cpp:476`); general capture does not. Cave flourishes not suppressed by scenario alone (`client/app/CaveFlourishes.cpp:75`). | Live defaults ON/CONDITIONAL, then scenario pins override (`core/ScenarioRunnerCapture.cpp:195`); no universal weather schedule. | Live CONDITIONAL or FORCED by atmosphere driver (`core/ScenarioRunnerCapture.cpp:198`). Particle availability/content CONDITIONAL. | Many visual smokes FORCE noon; other scenarios use their own pins/live clock (`core/ScenarioRunnerCapture.cpp:201`). | Scenario-specific; no generic clear–storm–clear or exact multi-day creature audit. Normal sim uses frame delta (`client/main_client.cpp:5339`); capture progress uses elapsed time (`core/ScenarioRunnerCapture.cpp:219`). |
| Time-of-day sweep | Ambient OFF (scenario); no creature fixture (`client/main_client.cpp:5555`). | Foliage ON; caves/structures/water/rivers/waterfalls CONDITIONAL; emissive discovery can change camera for an extra shot (`core/ScenarioRunnerDrive.cpp:736`). | Live bridge ON/CONDITIONAL; TOD pins do not supply a weather cycle (`core/ScenarioRunnerCapture.cpp:210`). Clouds/shadows follow live sample (`client/main_client.cpp:5724`). | Live strikes/particles CONDITIONAL (`client/main_client.cpp:5736`, `:6015`); no presence qualification. | FORCED summer/winter noon/dusk/night plans, one pending capture at a time (`core/ScenarioRunnerCapture.cpp:235`); not continuous dawn/dusk. | NO scripted weather cycle or multi-day animals. Season tick is a render pin, not elapsed sim days (`:245`). Frontier launches a timed scenario with `--no-ui` (`frontier:3035`). |
| Weather visual | Ambient OFF (scenario); no creature fixture (`client/main_client.cpp:5555`). | Foliage ON; caves/structures/water/rivers/waterfalls CONDITIONAL, not asserted by this gate. | FORCED clear control then rain floored to 1.0; final precipitation follows pin (`core/ScenarioRunnerDrive.cpp:245`, `:258`, `:282`). Clouds/shadows retain live bridge, not a prescribed full cloud cycle (`client/main_client.cpp:5724`). | FORCED bolt only in progress window 0.80–0.84 (`core/ScenarioRunnerDrive.cpp:305`); rain particles ON in weather phase, emission OFF in clear control (`:282`); ambient motes common. | FORCED noon (`core/ScenarioRunnerCapture.cpp:203`); sky ON, night moon/stars absent. | NO return-to-clear cycle or multi-day animals. Motion clip is 240 render samples at 1/60 s (`core/ScenarioRunnerCapture.cpp:90`); frontier also runs a separate 300-tick weather bench, not an ecological capture (`frontier:2596`). |
| Headless in-game capture gate | Same eligibility as frame-scan/scene when no scenario active (`client/main_client.cpp:5555`); no spawn/visibility assertion. | Foliage ON/complete; cave flourishes OFF for both legs (`client/app/CaveFlourishes.cpp:76`); caves/structures/water/rivers/waterfalls CONDITIONAL. | Noon scan retains live weather; night scene FORCES default None precipitation; clouds only explicitly replaced if requested (`client/main_client.cpp:5973`, `:5987`). | Scan live; night scene still inherits live lightning because scene block has no lightning override (`:5965`); particles ON but scene precipitation set to zero (`:5981`). | FORCED noon scan plus midnight `0.5`, full moon `1.0` scene; stars CONDITIONAL on view (`frontier:5867`). | NO: two independent still runs (`frontier:5847`, `:5877`), no continuous weather or multi-day creature progression. Hidden-window mode only sets window visibility (`core/RuntimeScenarioConfig.cpp:261`). |

`--no-ui` sets ImGui availability and skips UI setup, not weather, particles or
simulation (`client/main_client.cpp:1530`, `:2376`). Capture developer-widget policy
also suppresses shortcuts (`client/app/ClientAppContext.h:423`), but ecology's
creature labels remain an explicit exception (`client/main_client.cpp:6503`;
`docs/developer-console.md:141`). That label overlay is not animal visibility
proof. Deterministic still pins likewise do not imply deterministic temporal
sampling: ordinary frame dt is wall-clock derived (`client/main_client.cpp:4518`),
and current time-lapse adds ticks after readback. Its 45 initial settle frames
(`client/app/ClientAppContext.h:60`) do not settle each later sample. The existing
assembler naturally sorts filenames and loads all frames, uses Pillow for GIF
and optionally ffmpeg for MP4 (`tools/timelapse.py:35`, `:43`, `:75`); it does not
validate a sequence manifest or produce a temporal contact sheet.

### E1: fixed-camera time-lapse audit runs

`BuildTemporalAuditPlan` implements the three span schedules below without driving
simulation or rendering. It records integer target ticks, rational sub-tick phases
with denominator 60, exact calendar/elapsed-day or weather boundaries, deduplicated
burst samples, and separately labelled 120-frame steady-lighting and weather night
holds. Burst windows center on crossings where possible and shift inside span
endpoints; a span too short for a complete burst refuses. Holds freeze the final
simulation tick while presentation advances at 60 Hz. The weather primary/steady
samples declare noon; night presence declares TOD 0.5 and a moon phase (full by
default). Other span samples follow the live calendar. No weather values are driven.

`ParseTemporalAuditPlanOptions` admits `--temporal-audit-plan` with `day-night`,
`weather-cycle` or `multi-day`. Require `--temporal-start-tick`,
`--temporal-tick-rate`, `--temporal-day-ticks`, `--temporal-dawn-tick` and
`--temporal-dusk-tick`; these describe the resolved simulation calendar, with dawn
and dusk as midnight-origin day offsets. Weather plans also require
`--temporal-strike-tick`, an offset in the storm interval [50T,90T).
Optional `--temporal-frame-limit` and `--temporal-tick-limit` can lower the hard
4,096-frame and 10-million-tick ceilings; `--temporal-moon-phase-milli` declares
0..1000. Accept both `--option=value` and `--option value`. Missing, duplicate,
unknown, partial, signed, overflowing or out-of-range options refuse with a reason.
Tick rate is bounded to 1..1000 Hz; start/end must fit `WorldClock::kTickLimit`.
Calendar crossings require 0 < dawn < dusk < D; non-weather strike inputs refuse.

Planning traverses at most 721 sparse candidates, eight boundaries and eight
121-candidate burst windows plus 240 hold frames, before sorting/deduplication.
The O(1) advancement query refuses clock overshoot, a clock before initialization
or a missing frame index. Warmup/readiness/sample-settling bounds below are recorded
in the plan, along with conservative total presentation/render-attempt bounds
including unsaved substeps (at most 600 million span presentation steps before
holds/readiness). No frame-delta or wall-clock input enters scheduling.
Registered `TemporalAudit.*` regressions cover admission, exact scheduling,
deduplication, limits and both startup refusal paths; native execution is pending.

`TemporalAuditSequence` implements a GL-free execution protocol for one admitted
schedule. It re-admits and compares the complete plan, checks overflow-safe
aggregate frame/manifest reservations against a caller-lowered 512 GiB ceiling,
and refuses before issuing an executable step when admission fails. The transport
supplies enforceable upper byte bounds including every frame's A4 bundle and
metadata; this protocol does not estimate those bounds from resolution alone.
Queries are idempotent until acknowledgement. Each advance requests exactly one
simulation tick and verifies the resulting clock; outside advancement, overshoot,
stalled ticks and failed steps cause sticky refusal. Unsaved presentation steps
are at most 1/60 second, including fractional intervals ending at sample ticks.
Warmup uses exactly 90 frozen-tick frames. Initial readiness permits 300 held-phase
attempts; sparse samples settle for three held-phase frames and every sample
permits 60 readiness attempts. Dense samples omit settling. Retries neither tick
simulation nor age presentation. Final-tick holds preserve their planned order.
Calendar phase comes from integer tick plus its rational subframe; the owner must
resolve solar TOD through the session calendar and apply the explicit noon/night
controls, rather than using wall time or assuming symmetric daylight.

Capture acknowledges nonempty output within its reserved frame bound; only after
all captures does the protocol issue publication of the completion manifest.
Failure or cancellation remains incomplete even after partial captures. It owns no
filesystem destination, scenario counters, recorder or runtime state. Registered
`TemporalAuditSequence.*` regressions exercise endpoints, rational bursts, holds,
readiness exhaustion and last-attempt success, exact settling, phase/controls,
byte-boundary and altered-plan refusal, clock/transport faults, partial publication
and real creature-brain ticks matching ordinary fixed ticking. These native tests
are registered, not execution evidence. Shipping startup still refuses temporal
runs: integration with the fixed site, simulation/physics/streaming dependencies,
A4 reservations and completed-frame capture, and existing frontier actions remains
pending. No temporal or all-features runtime support is claimed.

The planning parser is callable independently. Both shipping startup parsers reject
even valid temporal plans, and `--scenario=temporal_audit`, with an explicit
unsupported-execution reason before world/output creation, including save-root
creation. Weather input, app/site transport integration, site/ROI/feature capability
admission, transport byte-bound enforcement and assembly remain later work. Admission alone
does not certify a temporal or all-features capture, or change simulation identity,
save/replay formats or authority state. The rest of this section specifies that
future execution contract.

Use the audit site's recorded seed, content/settings identity, camera position,
orientation, projection, resolution and D2 ROIs. Run each site/camera from the same
fresh deterministic initial state for each of the three spans below. Sites must
cover foliage/LOD boundaries, open water/river/waterfall, structures and cave
interior/mouth plus sky; additional fixed companion cameras are explicit site
entries, never camera motion hidden inside a sequence. Refuse a missing site,
unknown ROI or unsupported producer. Do not replace a site with the current
spawn-grove camera or reframe it after a feature goes missing.

Let D be the resolved simulation calendar's ticks per day and T the recorded
simulation tick rate. Record both; do not equate a render season pin with sim
elapsed time. The planned scheduler owns all tick advancement: no extra ordinary
frame-delta ticks, no dropped catch-up ticks, no wall-clock phase selection.
After initialization, advance exactly to target integer ticks through normal
simulation, including creatures, plants, erosion and hydrology, with the same
participating region/streaming policy recorded for replay. Preserve physics and
creature movement dependencies; fail on scheduler faults or stalled progression.
Audit annotations must not modify ordinary play or authority state. Scripted
simulation inputs must participate in the existing config/session identity and
save/replay hash paths; any required format change needs its own versioned
read/write migration slice before admission, never an unversioned audit field.

| Span per site/camera | Planned cadence and controls |
| --- | --- |
| Full day/night | Inclusive samples from tick t0 through t0+D, every ceil(D/720) ticks plus exact endpoint. TOD follows the sim calendar through dusk, midnight and dawn. Record actual intervals, not a claimed 720 equal intervals when division has a remainder. |
| Weather cycle | 120 sim seconds: 20 clear, 30 ramp to storm, 40 storm, 30 return to clear; one sample per T ticks plus both endpoints and every phase boundary. Pin TOD at noon for response comparison, recording that render-only pin. Weather remains enabled in clear phases. |
| Several sim days | Three complete sim days, inclusive t0 through t0+3D, every ceil(D/240) ticks plus exact endpoints/day boundaries. Use live plants and natural erosion/hydrology; animals stay active. Do not substitute visual grow/season or dig/drain demos for ecological progression. Report actual plant stages, erosion deltas, water levels and animal displacement even when change is zero. |

For micro problems, add a 120-render-frame burst at 60 Hz at dusk, dawn, each
weather boundary and a scheduled storm strike; deduplicate overlapping samples.
Each pair of burst renders advances one 30 Hz sim tick (generalize from recorded
T); subframe presentation time is rational and recorded. Also capture a 120-frame
steady-lighting hold with moving-subject masks for flicker analysis. After the
weather span's final clear endpoint, add a separately labelled 120-frame night
presence hold at render TOD 0.5, the same camera and the final sim tick, with a
declared moon phase. Include it in that capture's feature evidence and frame
budget, but exclude it from weather-response and lighting-continuity metrics.
This gives moon/stars a predeclared window even though the primary weather
comparison pins noon; if the fixed view cannot show them, refuse coverage. The
weather cycle's noon/night pins and holds are explicit controls, not a replacement
for the live multi-day clock. Long intervals still execute every sim tick and
required particle presentation substep; sampling sparsely must not reset particles or freeze animals.

Warm up for 90 render frames at fixed 1/60 s presentation steps with simulation
frozen at the admitted initial tick, then require residency, required meshes,
foliage and expected feature readiness; at most 300
additional render attempts at a held presentation phase. Record t0 and the
exact warmup history so worker scheduling cannot change initial simulation time.
At each sample, allow 3 render-only settle frames, then at most 60 upload/readiness retries
with simulation and presentation phase held fixed. Settle frames do not enter the
sequence or spawn/age particles repeatedly. Refuse exhausted readiness; never
capture a half-loaded frame as a valid sample. Dense bursts preserve temporal
history and have no per-frame history reset.

Weather scripting is a new tick-indexed scenario input, applied before simulation
weather/hydrology consumers and mapped through the live render bridge afterwards.
Use an explicit versioned keyframe table of category, precipitation, storm,
wetness/fog, wind and cloud targets with specified interpolation, seed-derived
strike tick/position/magnitude and precipitation emitter seeds. Record requested
and actual sim/render states; disagreement fails the run. Clear–storm–clear must
reach zero precipitation at both ends and at least one scheduled visible strike
in the storm interval. Do not reuse elapsed-progress pins, a permanently active
bolt or render-only rain as evidence that the simulation experienced the cycle.
The script/config/content digest must cover resolved referenced bytes in canonical
order. If the existing configuration cannot express a field, report unsupported
until its scenario adapter and deterministic regression exist.

Planned output is `temporal/<site>/<camera>/<span>/`, with `run.json`,
`frames/frame-000000.png` (or PPM) and each frame's A4/D1 bundle/manifest,
`metrics/`, and `review/sequence.gif`, `review/sequence.mp4`,
`review/contact-sheet.png`, `review/worst-<metric>.png` and `review/review.json`.
Frames are ordered by manifest indices, never directory glob order. Input readers
refuse unknown schemas, partial payloads, duplicate keys/indices, nonfinite or
out-of-range numbers and inconsistent tick/phase bounds. Open each input once,
verify a regular file within the run root and hash/decode that same handle; reject
links, escapes and device/FIFO inputs. Close payload handles before rename or
cleanup on Windows; retain the A4 identity-checked cleanup rule. The run
manifest carries schema version, completion/refusal reason, seed, build and resolved
content/config/script hashes with scopes/unavailable reasons, backend/device,
feature-profile digest, site/ROI version, T/D, start/end/target/actual tick,
subframe time, TOD/season, full requested/actual weather/cloud/strike states,
particle seeds, fixed camera/matrices, resolution/exposure/history policy, readiness
and settle counts, per-frame feature evidence and payload hashes. Bind evidence
to the same completed frame; capture-local IDs may change between frames.

Publish payloads first and completion manifest last into a fresh destination,
using A4's transaction/cleanup rules; any refusal stays incomplete/failed. Assembly
validates every expected frame/bundle and hash before writing derivatives. A
missing/corrupt/duplicate/out-of-order frame leaves a labelled contact-sheet slot
and fails assembly; it is never omitted. GIF and MP4 play at 12 fps for the sparse
sequence and 60 fps for separately labelled bursts; label tick/TOD/weather/camera
on the contact sheet and include start/end, phase boundaries and worst frames.
Both formats are required for a complete review; missing Pillow/ffmpeg is an
explicit assembly failure, preserving raw evidence, not a successful skipped MP4.
Assembly success leaves visual approval pending.

Bound each site/camera/span to 4,096 recorded frames including bursts, 10 million
sim ticks, 16,384 pixels per dimension and 16,777,216 pixels per frame. Cap a run
at 32 site/camera pairs and 512 GiB of outputs, checked with overflow-safe exact
byte arithmetic before allocation/output. Refuse rather than shorten a span.
Stream assembly/metrics with at most three full-resolution frames, 512 MiB CPU
staging, and one A4-bounded bundle at a time; never load a whole sequence. GIF
palettes/encoder buffers count against this budget. Per-frame work is bounded by
pixels and participating items, never a fresh whole-world scan; run analysis is
O(frames × pixels × fixed metric count). Preflight bounds include readiness and
all simulation/presentation substeps, not just saved frames.

### E2: temporal metrics and qualification

Reuse D1's validated bundle/ID/AOV reader and D2's ROI/mask reader and montage
labels. No independent permissive parser or inferred draw identity. Validate
frame order, tick spacing, fixed camera/projection, dimensions, attachment roles
and hashes before comparing. Across frames, join stable chunk/asset/producer/LOD
keys, not numeric capture-local draw IDs. Occlusion or missing attribution is
explicit unsupported coverage. Analyze linear lighting/contribution channels
where available; final color deltas are labelled post/exposure-dependent.

| Metric | Measurement and false-positive control | Initial status |
| --- | --- | --- |
| Frame-to-frame delta maps | Absolute linear RGB/luminance and depth/material/ownership changes for adjacent samples; report mean, p95/p99 and changed area per ROI, with actual delta ticks. Separate moving animals, rain, wind foliage and lightning masks; retain an unmasked companion map. | Advisory; qualify separately by cadence/span/backend/ROI, not one threshold for all time gaps. |
| Flicker | In steady-lighting 120-frame windows, luminance residual after removing the recorded smooth lighting/exposure trend; count alternating-sign differences and peak-to-peak/p95 amplitude on persistent surfaces. Require at least 30 usable samples, at least 64 valid pixels covering 1% of the ROI and known steady light. Moving subjects, strikes and cloud-shadow edges are classified and reported separately. | Advisory; not eligible outside a steady window or when coverage is insufficient. |
| Pops | Sudden local residual/changed area between adjacent frames, associated with depth, LOD, foliage/impostor or ownership transitions using stable producer keys. Report magnitude, area and before/after attribution. Distinguish expected motion/disocclusion/weather from unexplained jumps; a raw color jump alone cannot identify an LOD defect. | Advisory; qualify using labelled known-good transitions and injected one-frame switches. |
| Dusk/dawn lighting continuity | Track static terrain/sky ROI linear light and exposure, first differences normalized by tick interval and second differences; label horizon crossings, cascade changes and cloud/strike contributions. Compare against smooth expected solar progression, not a zero-change threshold. | Advisory; qualified only with valid lighting channels and repeated dusk/dawn fixtures. |
| Seam persistence | Reuse D2 pinned seam/boundary bands and D1 ownership/depth validity. Count consecutive affected samples, elapsed sim ticks and affected area; join the same world seam across frames. Exclude authored openings via versioned masks; unknown or empty expected seam masks refuse eligibility. | Advisory; qualify static persistent defects and transient streaming cases separately. |

Each result records `advisory`, `qualified` or `ineligible`, coverage/reason,
units, span/cadence, masks, threshold/baseline version, actual values and the
worst frame indices/ticks. Qualification requires repeated identical-input runs,
known-good and deliberately defective sequences, backend/cadence-specific bounds
and owner review, as in D2; only a separately qualified threshold may affect a
visual gate. Missing inputs never become a zero score or pass. Structural and
presence refusals remain hard failures even while visual metrics are advisory.
Emit the top 8 events per metric with deterministic tie order (score, then frame
index, then ROI); labelled worst-frame montages show before/current/after,
delta/mask, site/camera, tick/TOD/weather, metric/status and draw/asset probes.
Cap ROIs at 64 and metric events at 40 per sequence; reuse bounded rolling
accumulators and fixed 256-bin histograms, not retained full-resolution maps for
every frame. Store only the current maps and selected worst-event payloads within
the E1 output budget.

### E3: all-features profile and non-vacuous evidence

Implemented profile foundation: `--capture-feature-profile=all_features` selects
the complete 31-entry catalog; absent selection or `none` preserves ordinary
launch behavior. Both launch parsers reject missing, empty, unknown and repeated
selections before save-root creation. `CaptureFeatureProfile::Resolve` accepts the
runtime configuration, resolved systems/graphics and an explicit mode adapter
snapshot. It also accepts a nonempty explicit feature subset without the all-features
selection, refusing unknown/duplicate names and conflicting selections. The eight
mode labels cover time-lapse, frame-scan, world sweep, general scenario, time-of-day,
weather and both headless legs. Every unimplemented adapter defaults to unsupported,
with unknown effective state represented as null. No shipping adapter is implemented
or certified by this foundation.

The resolver records requested/effective settings, forced/suppressed/unavailable
reasons, per-feature and aggregate capability, and resolved asset/config identities
or explicit unavailable reasons. Missing identities prevent aggregate support;
requested disabled settings are refused. It applies actual weather-hydrology,
weather-event and live-bridge gates, resolved grass disablement, scenario ambient
animal suppression, consumed isolation render masks and sky backdrop replacement.
Code inspection distinguishes tuning switches from producer gates: disabling
creature-spawn tuning still uses the compiled herd count, and plant-growth,
ecology, tree-wind and moonlight tuning flags do not prove producer disablement.
Terrain isolation suppresses cave/structure geometry and static vegetation;
skinned isolation suppresses ambient animals and the creature spawning, AI and
animation consumers. Foliage and plant wind require both terrain/static-plant and
foliage/grass consumers; omitting either layer explicitly refuses requested support,
including when a producer adapter is unavailable. Water/rivers, particles and
lightning follow their corresponding render masks. The separate procedural-plant
(including plant growth) and waterfall-sheet stages, weather overlays and lighting
do not consume isolation layer bits. The aerial fog consumer has no separate catalog
entry; reserved sky and far-field bits do not gate draws. Other final producer
settings, assets and scenario pins require adapter observations in later slices.
Explicit final forced/suppressed adapter observations take precedence over baseline
producer controls, so later policy can report transient producer overrides without
changing saved preferences. Final enabled settings cannot restore draws omitted by
the resolved isolation masks or sky backdrop; explicit suppression reasons survive.

The immutable result serializes schema `luminumbra.capture_capabilities.v1` in fixed
catalog/key order, including every unrequested or unsupported row. `WriteManifest`
writes to a caller-owned stream and checks write/flush failure, including stream
exceptions; filesystem publication and app admission remain later work. Metadata
is bounded to 256 UTF-8 bytes per field, isolation inputs to 1024/256 bytes and
feature requests to the catalog size with 64-byte names; invalid metadata/enums
are refused before stream output. Presence is always `not_evaluated`, never inferred
from settings. Resolution mutates no settings and adds no tick/frame work.

The remaining end-to-end contract is planned below.

A new explicit `all_features` capture profile will resolve a versioned feature
manifest at admission. It requires animals/creatures (spawning, AI, animation),
foliage/plant growth/wind, weather and precipitation, clouds and cloud shadows,
lightning, water/river/waterfall rendering and hydrology, particles, cave geometry
and flourishes, structures, and sky/moon/stars. Record requested/effective flags,
resolved assets/config hashes, mode capability and refusal reason for every entry.
No capture-mode shortcut, UI suppression, determinism option, isolation layer or
fallback may silently disable a producer. Resolve once at setup and verify final
producer state after every scenario/scene pin; no per-frame environment reads.
Use transient capture settings without saving user preferences or changing
ordinary specialized-gate baselines.

| Required feature | Per-capture presence evidence, bound to frame/tick and ROI |
| --- | --- |
| Animals/creatures | Spawned/live/ticked counts, planner/animation progress and stable entity trajectories plus visible skinned/creature contributor pixels. Labels/marker-only ecology images cannot prove shipping animal rendering. |
| Foliage | Submitted instances/draws and visible foliage pixels, asset/LOD/impostor transitions, live plant stage/tick and wind state. |
| Weather/precipitation | Actual sim and final render state, emitter/live drop counts and visible rain/snow contribution in the declared weather interval; zero at declared clear endpoints. |
| Clouds/cloud shadows | Cloud draws/coverage and visible cloud pixels, shadow-enabled state and nonzero measured shadow contribution over a declared ground ROI. |
| Lightning | Scheduled and consumed strike IDs/ticks, visible bolt/flash pixels and labelled before/strike/after frames. A strike count or active flag alone is insufficient. |
| Water/rivers/waterfalls | Water-surface draws/indices and visible water pixels; resolved river/waterfall site identity, actual river surface and waterfall draws/pixels, flow/level observations and spray evidence separately. Generic water cannot stand in for all three. |
| Particles | Emitter categories, alive/submitted counts and visible contribution, distinguishing ambient, precipitation and waterfall spray. An allocated pool is not presence. |
| Caves/flourishes; structures | Site/mesh/draw identity and visible geometry in registered ROIs; expected cave/crystal lighting contributions and structure pixels, not merely world-generation flags. |
| Sky/moon/stars | Sky draw and contributor coverage; visible lunar disk and star contribution during declared night/view windows, zero star contribution in daylight. |

Record counters and visible contributions per frame, plus aggregate presence over
the named capture sequence and its declared phase windows. All producers stay
enabled throughout; physical absence in a clear/day frame is recorded explicitly
as expected zero. Every required feature must have positive evidence in its
predeclared presence window somewhere in that capture. Zero where required,
unknown counters, offscreen-only spawns or an empty ROI refuse the capture as
non-vacuous coverage failure. A still cannot claim all-features success unless
its required evidence is visible in that still; incompatible day/night or
interior/exterior requirements need explicitly registered companion sequences in
the site audit set. Companion coverage is reported separately and cannot turn a
failed individual capture into a pass. The three spans remain required for every
registered fixed camera; do not silently drop a cave or sky camera.

Capability reporting has `supported`, `unsupported` and `refused` plus reasons;
presence has counts/coverage and expected windows, not a boolean derived from the
requested flag. A mode unable to enable any required feature emits `unsupported`
and cannot earn the all-features result. Preflight capability/refusal occurs
before world/output creation; runtime zero-presence publishes failed evidence
without a completion marker. The report preserves unsupported rows even when all
other features pass. Unsupported diagnostics/contributor planes also preclude
presence qualification until instrumented; final-color guesses do not suffice.

Current blockers and required work are explicit: scenario wildlife suppression
needs a profile-aware spawn path with real rigs/AI; the synchronous world sweep
needs the E1 tick driver rather than still-cell reuse; frame-scan/headless stills
need a sequence adapter and night/weather windows; time-of-day/weather smokes need
continuous tick schedules including dawn and return-to-clear; cave automation
needs bounded explicit flourish initialization; upward sweep foliage clearing and
per-cell particle resets need profile-aware alternatives. Live-weather/cloud,
lightning and transient precipitation must share one final tick-indexed state,
with real producer evidence for forward water/particles/sky (A/B coverage gaps).
A missing asset/system is unsupported, never an automatic fallback to markers.
Until these adapters and evidence producers land, no current mode is certified as
an all-features temporal audit. Add support in bounded slices below, keeping the
same capability/presence semantics across every sibling path and platform.

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
| C1: every-PR shader compile and link completion | `tools/ci/compile_shaders.py`; `tools/ci/test_compile_shaders.py`; `test/rendering/render_smoke_test.cpp`; `docs/development/visual-diagnostics.md` (4) | Implemented in the existing unconditional fast-job step. Compile every stage permutation, then link the native program registry and compute inventory using the same prepared sources. Refuse missing pass-layout pairs or unlinked stage/define permutations; registered interface-mismatch, matching-pair and missing-program regressions cover linking. |
| C2: llvmpipe scenes | `tools/gates/shader-scenes.py`; `tools/gates/test_shader_scenes.py`; `test/rendering/shader_scenes/scenes.json`; `test/rendering/shader_scenes/references/material.ppm`; `test/rendering/shader_scene_render_test.cpp`; `test/CMakeLists.txt`; `tools/ci/ci_paths.py`; `.github/workflows/ci.yml`; `config/public-asset-inventory.json`; `docs/development/visual-diagnostics.md` (10) | After C1. First reference/metric/regeneration vertical path; extend the scene catalogue in later bounded changes. Avoids owned files; reference and consumer share one path gate. |
| D1: bundle/ID/AOV validity checks | `tools/gates/check-diagnostic-capture.py`; `tools/gates/test_diagnostic_capture.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `tools/gates/diff-render-health.ps1`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | After A4; parallel with B/C. Registered Python reader tests cover little endian, dimensions, partial payloads, unknown versions and nonfinite values. Avoids owned files. |
| D2: image metrics and synthetic qualification | `tools/gates/check-diagnostic-capture.py`; `tools/gates/test_diagnostic_capture.py`; `tools/gates/baselines/diagnostic-rois.json`; `tools/gates/build-visual-sweep-montages.py`; `tools/gates/test_diagnostic_checks.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After D1 and relevant C2 fixtures/B2 channels. Implemented masked holes, seam/reference and boundary residuals, cards, bands, wedges, weather/detail and composition checks with isolated synthetic defects and labeled worst-pixel montages. All shipped thresholds remain advisory/unqualified; production enforcement needs separate repeated-capture evidence. Acceptance-row state is unchanged. Avoids owned files. |
| E1a: exact-tick plan and admission | `rendering/TemporalAudit.h`; `rendering/TemporalAudit.cpp`; `src/luminumbra_client/core/RuntimeScenarioConfig.h`; `src/luminumbra_client/core/RuntimeScenarioConfig.cpp`; `src/luminumbra_client/sources.cmake`; `test/rendering/temporal_audit_test.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After A4, D1. Register GL-free tick/cadence/overflow/refusal regressions in capture_manifest_test. Coordinate configuration ownership; admission alone cannot advertise runtime support. |
| E1b: deterministic weather input | `src/luminumbra_common/systems/WeatherSystem.h`; `src/luminumbra_common/systems/WeatherSystem.cpp`; `src/luminumbra_common/world/GameSession.cpp`; `rendering/TemporalAudit.cpp`; `test/rendering/temporal_weather_test.cpp`; `test/CMakeLists.txt`; `src/luminumbra_client/core/ScenarioRunnerDrive.cpp`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After E1a. Coordinate simulation owners: register fixed-tick weather/hydrology/strike and identity/replay tests before enabling script support. Exercise normal live bridge; any format migration is a prerequisite separate slice with both readers/writers, not hidden in this count. |
| E1c: site sequence driver | `src/luminumbra_client/main_client.cpp`; `src/luminumbra_client/core/ScenarioRunnerCapture.cpp`; `src/luminumbra_client/core/ScenarioRunnerImpl.h`; `rendering/TemporalAudit.cpp`; `test/rendering/temporal_sequence_test.cpp`; `test/CMakeLists.txt`; `tools/gates/validate-engine-frontier.ps1`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After E1b, A4, D1. Coordinate app/scenario ownership. GL-free sequence protocol and registered sequence/readiness/phase/creature-tick regressions implemented. App/site transport and native fixed-site capture through existing frontier actions remain pending. No all-features claim before E3. |
| E1d: manifest-aware assembly | `tools/timelapse.py`; `tools/gates/test_temporal_assembly.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `docs/visual-regression.md`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | After E1c and D1. Register Python manifest/order/hash/partial-input/encoder-failure/budget tests in PYTHON_TESTS and CTest; preserve existing assembler callers. Existing generic workflow invokes the registry. Coordinate shared assembler and document edits. |
| E2a: temporal measurements | `tools/gates/check-temporal-capture.py`; `tools/gates/test_temporal_capture.py`; `tools/gates/check-diagnostic-capture.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | After E1d, D1, D2. Register Python fixtures for all five metrics, unknown metadata, empty masks and stable cross-frame identity; reuse D1/D2 readers. Avoid owned rendering files. |
| E2b: qualification and worst events | `tools/gates/check-temporal-capture.py`; `tools/gates/test_temporal_capture.py`; `tools/gates/baselines/diagnostic-rois.json`; `tools/gates/build-visual-sweep-montages.py`; `tools/gates/test_temporal_montages.py`; `tools/ci/ci_paths.py`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After E2a and relevant B2/C2 contributions. Register qualification-status, repeated-run, injected-defect and labelled failure-slot tests in PYTHON_TESTS/CTest. Threshold enforcement requires separately reviewed qualification evidence. Coordinate shared ROI/montage edits. |
| E3a: profile and capability manifest | `rendering/CaptureFeatureProfile.h`; `rendering/CaptureFeatureProfile.cpp`; `src/luminumbra_client/sources.cmake`; `src/luminumbra_client/core/RuntimeScenarioConfig.h`; `src/luminumbra_client/core/RuntimeScenarioConfig.cpp`; `test/rendering/capture_feature_profile_test.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After A4, D1; may precede E1. Register GL-free profile/capability/unknown-feature/incompatible-option tests. Default every unimplemented adapter to unsupported. Coordinate configuration ownership. |
| E3b: shared app and scenario policy | `src/luminumbra_client/main_client.cpp`; `src/luminumbra_client/app/ClientAppContext.h`; `src/luminumbra_client/app/CaveFlourishes.cpp`; `src/luminumbra_client/core/ScenarioRunnerDrive.cpp`; `src/luminumbra_client/core/ScenarioRunnerCapture.cpp`; `rendering/CaptureFeatureProfile.cpp`; `test/rendering/capture_feature_policy_test.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `docs/visual-regression.md` (10) | After E3a and E1c. Coordinate app/scenario/cave ownership; register ordinary-play equivalence and profile tests across timelapse, scan, general scenario, TOD/weather and both headless legs, including no-ui and determinism options. Remove suppression only for admitted profile runs. |
| E3c: sweep profile adapter | `scenarios/WorldVisualSweep.cpp`; `src/luminumbra_client/core/ScenarioRunnerCapture.cpp`; `rendering/TemporalAudit.cpp`; `rendering/CaptureFeatureProfile.cpp`; `test/rendering/temporal_sweep_test.cpp`; `test/CMakeLists.txt`; `tools/gates/validate-engine-frontier.ps1`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After E3b. Coordinate sweep ownership. Register exact-tick sweep/particle-continuity/creature tests; existing still matrix keeps its pins. Feature-incompatible camera/profile combinations refuse explicitly. |
| E3d: forward feature evidence | `rendering/DiagnosticContributions.h`; `rendering/DiagnosticContributions.cpp`; `rendering/passes/WaterPass.cpp`; `rendering/passes/WaterfallPass.cpp`; `rendering/passes/ParticlePass.cpp`; `rendering/passes/FoliagePass.cpp`; `rendering/RenderPipeline.cpp`; `test/rendering/capture_forward_presence_test.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md` (10) | After B2 and E3a. Coordinate owned producers. Register GL-free counter joins and native visible-contribution/occlusion fixtures; separate river/waterfall/spray evidence, and report unsupported until instrumented shader contributors land in additional bounded slices with asset inventory updates. |
| E3e: geometry and atmosphere evidence | `rendering/DiagnosticContributions.h`; `rendering/DiagnosticContributions.cpp`; `rendering/passes/GBufferPass.cpp`; `rendering/passes/SkyboxPass.cpp`; `rendering/passes/LightingPass.cpp`; `rendering/RenderPipeline.cpp`; `test/rendering/capture_scene_presence_test.cpp`; `test/CMakeLists.txt`; `docs/development/visual-diagnostics.md`; `docs/visual-regression.md` (10) | After E3d. Coordinate owned passes. Register creature/cave/structure/sky/moon/star/cloud-shadow/bolt visibility fixtures, including absent/offscreen cases; shader variants and public asset inventory changes are prerequisite bounded slices under B2, not implicit extra files. |
| E3f: non-vacuous acceptance report | `rendering/CaptureFeatureProfile.cpp`; `rendering/TemporalAudit.cpp`; `tools/gates/check-temporal-capture.py`; `tools/gates/test_temporal_capture.py`; `test/rendering/capture_feature_profile_test.cpp`; `test/CMakeLists.txt`; `tools/gates/validate-engine-frontier.ps1`; `docs/development/visual-diagnostics.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | After E3c–E3e and E2b. Extend registered suites and existing frontier consumers: zero/unknown/unsupported presence, wrong-frame evidence and failed publication cannot pass; full site/span success requires all expected windows. Qualification/visual approval remain distinct. |

Start A, B interface design, C and D fixture design together; GPU attribution is the
critical dependency for trustworthy production pixel claims. Forward/transparency
producer coverage and additional shader references proceed as subsequent bounded
changes using the same formats, with coverage gaps visible until implemented.

E rows include local inventory updates in their stated counts. Every new Python
suite enters `tools/ci/ci_paths.py` PYTHON_TESTS and CTest in its introducing row;
CI's existing standalone-contract registry invocation supplies workflow coverage.
New native sources enter the appropriate existing target/source list and CTest;
new capture actions, if needed beyond the listed frontier consumers, require a
separate at-most-ten-file slice including workflow invocation and path filters.
Do not report a registered native fixture as executed until its native lane runs.
