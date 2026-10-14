# blender visual scenarios

Generated from [the catalog](visual-catalog.json). [Roster](visual-catalog-roster.md) · [shared packet contract](visual-catalog.md).

## B01

**Transformed prop hierarchy and linked instances**

Show source and compiled placements clearly with asymmetric scale/rotation and shared-mesh instances.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Blender source viewport, object IDs/transforms and matched actual target render; compiled bounds/instance readback; current fixture export alone is not viewport fidelity.

Source links:

- [tools/blender/authoring/probes/blender_fixture.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/probes/blender_fixture.py)
- [tools/blender/authoring/probes/compiled_readback.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/probes/compiled_readback.py)
- [test/static_preview/installed_viewport.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/test/static_preview/installed_viewport.py)

Required variants:

- source
- compiled-target
- asymmetric-transform
- linked-instances

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show source and compiled placements clearly with asymmetric scale/rotation and shared-mesh instances.
- Blender source viewport, object IDs/transforms and matched actual target render; compiled bounds/instance readback; current fixture export alone is not viewport fidelity.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Native source export, compiled transform readback, static-prefab consumption and installed static preview are implemented. Matched source/target pixels, hierarchy and instance acceptance remain unqualified.

Retained evidence:

- None qualified for this catalog row.

Next action: Prepare matched source prop and installed static target images with transform/readback and immutable generation evidence. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B02

**Geometry Nodes leaves and alpha material**

Show a clearly readable procedural plant with correct material assignment and clean cutouts.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Eight-leaf fixture source/evaluated/result views, GN seed/counts/material indices, alpha chart, matching compiled readback; production canopy is G01-G03.

Source links:

- [tools/blender/authoring/probes/blender_fixture.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/probes/blender_fixture.py)
- [tools/blender/authoring/extension/blender_asset.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension/blender_asset.py)
- [tools/blender/authoring/extension_tests/test_evaluated_asset.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/test_evaluated_asset.py)
- [test/rendering/static_preview_render_test.cpp](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/test/rendering/static_preview_render_test.cpp)

Required variants:

- source
- evaluated
- compiled-target
- material-indices
- alpha-chart

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S4: Ground cover, canopy, material/UV/alpha and LOD controls with accepted pack preservation.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show a clearly readable procedural plant with correct material assignment and clean cutouts.
- Eight-leaf fixture source/evaluated/result views, GN seed/counts/material indices, alpha chart, matching compiled readback; production canopy is G01-G03.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: The eight-leaf export fixture, evaluated-foliage validation and installed static cutout preview subset are implemented. Evaluated material identity and matched native target fidelity still need proof; production canopy approval does not transfer.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture actual eight-leaf Geometry Nodes source/material fixture; attach native/export/readback evidence, then qualify target rendering separately from production canopy. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B03

**Two-bone animated character**

Make deformation and material fidelity obvious in both source and target at matched frames.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Frames 1/12/24 and intermediate animation samples, source Blender capture and actual engine render, clip/palette/mesh hashes; native engine viewport may be blocked.

Source links:

- [tools/blender/authoring/probes/blender_fixture.py](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/tools/blender/authoring/probes/blender_fixture.py)
- [tools/blender/authoring/probes/compiled_readback.py](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/tools/blender/authoring/probes/compiled_readback.py)

Required variants:

- frame-1
- frame-12
- frame-24
- intermediate-samples
- source-target

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make deformation and material fidelity obvious in both source and target at matched frames.
- Frames 1/12/24 and intermediate animation samples, source Blender capture and actual engine render, clip/palette/mesh hashes; native engine viewport may be blocked.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Native two-bone export and animation readback exist. Matched target pixels and general-character acceptance remain outstanding.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture supported two-bone source poses and exact sampled animation data; wait for composed target render and general character character scope before target-fidelity approval. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B04

**STEP/CUBICSPLINE and bind/joint-order diagnostics**

Explain interpolation and bind fixes with readable matched poses rather than opaque numeric success.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Each named fixture pose sequence and actual palette samples including endpoints/intermediate values; diagnostic plots labeled, compile/readback scope distinct from rendering.

Source links:

- [tools/blender/authoring/probes/fixtures.py](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/tools/blender/authoring/probes/fixtures.py)
- [tools/blender/authoring/probes/compiled_readback.py](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/tools/blender/authoring/probes/compiled_readback.py)

Required variants:

- step
- cubicspline
- bind-matrix
- joint-order
- endpoints
- intermediate-poses

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Explain interpolation and bind fixes with readable matched poses rather than opaque numeric success.
- Each named fixture pose sequence and actual palette samples including endpoints/intermediate values; diagnostic plots labeled, compile/readback scope distinct from rendering.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Interpolation/bind/joint-order numeric fixtures and readback are landed. Native target pose sheets remain missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Turn supported STEP/CUBICSPLINE/bind/joint-order data into labeled pose/sample sheets; add actual target pose captures when renderer integration exists. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B05

**Extension panel and job lifecycle**

Make job status, cancellation, errors and preview generation understandable in actual Blender UI.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Owned-window screenshots for ready/running/cancelled/stale/failed/recovered states and generation identity; mock/current-capability labels visible, no fabricated engine viewport.

Source links:

- [tools/blender/authoring/extension/__init__.py](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/tools/blender/authoring/extension/__init__.py)
- [tools/blender/authoring/extension_tests/native_probe.py](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/tools/blender/authoring/extension_tests/native_probe.py)
- [tools/blender/authoring/probes/extension/__init__.py](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/tools/blender/authoring/probes/extension/__init__.py)

Required variants:

- ready
- running
- cancelled
- stale
- failed
- recovered
- extension-unload

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make job status, cancellation, errors and preview generation understandable in actual Blender UI.
- Owned-window screenshots for ready/running/cancelled/stale/failed/recovered states and generation identity; mock/current-capability labels visible, no fabricated engine viewport.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Both mock probes and installed geometry extension are landed; labels and native UI evidence must identify which was run. Lifecycle state tests alone do not establish native UI acceptance.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture actual extension ready/running/cancelled/stale/failed/recovered UI with correct MOCK/compiled labels and held generation identity. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B06

**Installed service/geometry adapter integration**

Show an authentic installed authoring workflow from source edit through supported compiled result.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Fresh isolated native Blender capture, installed build/tool hashes, compilation/mesh/material readback and UI progress; each integration source remains separately identified.

Source links:

- [tools/blender/authoring/service/README.md](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/service/README.md)
- [tools/blender/authoring/service/tests/native_acceptance.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/service/tests/native_acceptance.py)
- [tools/blender/authoring/extension/README.md](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension/README.md)
- [tools/blender/authoring/extension/viewport_engine.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension/viewport_engine.py)
- [test/static_preview/installed_viewport.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/test/static_preview/installed_viewport.py)

Required variants:

- source-edit
- installed-build
- ui-progress
- actual-target-pixels

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show an authentic installed authoring workflow from source edit through supported compiled result.
- Fresh isolated native Blender capture, installed build/tool hashes, compilation/mesh/material readback and UI progress; each integration source remains separately identified.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Installed geometry service, extension, static renderer, persistent host and Blender adapter are implemented. Matched native service/editor/render packets and full workflow qualification remain outstanding.

Retained evidence:

- None qualified for this catalog row.

Next action: Compose the implemented installed service, static preview and Blender adapter into a pinned native workflow packet with UI progress, build receipts and matched target images. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B07

**Texture conversion and alpha cutoff**

Demonstrate texture orientation/color/alpha boundaries on a useful chart and actual textured object.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Source texture/profile and compiled image hashes, exact cutoff, bright/dark edge closeups, UV orientation and material readback. Exact capture producer not verified in this audit.

Source links:

- [tools/blender/authoring/service/luminumbra_author/formats.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/service/luminumbra_author/formats.py)
- [tools/blender/authoring/probes/compiled_readback.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/probes/compiled_readback.py)
- [test/rendering/static_preview_render_test.cpp](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/test/rendering/static_preview_render_test.cpp)

Required variants:

- uv-orientation
- color-profile
- alpha-cutoff
- bright-edge
- dark-edge

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S4: Ground cover, canopy, material/UV/alpha and LOD controls with accepted pack preservation.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Demonstrate texture orientation/color/alpha boundaries on a useful chart and actual textured object.
- Source texture/profile and compiled image hashes, exact cutoff, bright/dark edge closeups, UV orientation and material readback. Exact capture producer not verified in this audit.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Texture validation, compiled readback and the installed static opaque/cutout renderer are implemented. Exact matched source/compiled charts, native UV-alpha fidelity and complete packets remain unqualified; general translucency is unsupported.

Retained evidence:

- None qualified for this catalog row.

Next action: Create matched source/compiled UV-alpha charts and native opaque/cutout target images with exact cutoff, texture hashes and material readback. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B08

**Static prefab nested instances**

Show the supported prefab preserving a recognizable multi-object arrangement through revisions.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Source/compiled placement views, hierarchy/world matrices, shared resources, revision cancellation/rollback and actual build receipt; current integration and immutable build receipts must be identified separately.

Source links:

- [tools/blender/authoring/service/tests/test_prefab.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/service/tests/test_prefab.py)
- [tools/blender/authoring/service/PREFABS.md](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/service/PREFABS.md)
- [tools/blender/authoring/extension_tests/test_state.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/test_state.py)
- [test/static_preview/installed_viewport.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/test/static_preview/installed_viewport.py)

Required variants:

- source
- compiled-target
- nested-hierarchy
- revision
- cancel
- rollback

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show the supported prefab preserving a recognizable multi-object arrangement through revisions.
- Source/compiled placement views, hierarchy/world matrices, shared resources, revision cancellation/rollback and actual build receipt; current integration and immutable build receipts must be identified separately.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Prefab descriptor compilation, service publication, runtime static-prefab consumption and installed preview are implemented. Source/target placement, cancellation and revision pixels remain unqualified; general character rendering is unsupported.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture matched source/runtime hierarchy and placement through installed static-prefab consumption; retain cancellation, rollback, shared-resource and revision evidence. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B09

**Installed renderer and persistent Blender viewport**

Qualify the actual installed static renderer, persistent native host and RenderEngine adapter with authenticated bounded IPC and main-thread Blender access.

Origin: added. Parents: B01, B02, B03, B06. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Installed static renderer, authenticated persistent host and Blender RenderEngine probes cover supported subsets. Complete native workflow, runtime closure and performance packets remain outstanding.

Source links:

- [test/static_preview/installed_viewport.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/test/static_preview/installed_viewport.py)
- [tools/blender/authoring/extension_tests/native_viewport_presentation.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/native_viewport_presentation.py)
- [tools/blender/authoring/extension_tests/native_viewport_engine.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/native_viewport_engine.py)
- [tools/blender/authoring/extension/viewport_engine.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension/viewport_engine.py)

Required variants:

- production-color-depth-coverage
- color-conversion
- camera
- transform
- debounced-refresh
- selection-gizmos
- perspective
- orthographic
- resize
- multiple-viewports
- crash-recovery

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. Implemented source fixtures and supporting probes do not provide a complete reproducible native visual packet; capture remains missing for the full claim.

Acceptance checks:

- Qualify the actual installed static renderer, persistent native host and RenderEngine adapter with authenticated bounded IPC and main-thread Blender access.
- Meet the shared packet contract for every required variant and parent mapping.

Required performance scope: `installed_authoring`; 1 distinct frozen profile(s).

- `presentation_fps` (profile-declared statistic) >= 30.
- `camera_transform_ms` (p95) < 100.
- `warm_material_refresh_seconds` (profile-declared statistic) < 2.
- `small_asset_refresh_seconds` (profile-declared statistic) < 5.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Installed static renderer, persistent host, Blender RenderEngine adapter and native probes are implemented. Historical GPU presentation evidence is scoped separately; complete editor workflow, Windows runtime closure and performance qualification remain outstanding. No visual approval follows from source landing.

Retained evidence:

- None qualified for this catalog row.

Next action: Run the installed host and Blender editor native probes on pinned SDK/generation identities; qualify runtime closure, complete workflow, failure paths and end-to-end authoring targets without inheriting approval from historical presentation checks. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B10

**Installed SDK and visual behavior graphs**

Deliver production SDK extraction in release order and compile separate graph documents into restricted Lua with typed manifests.

Origin: added. Parents: B06, B08. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Deliver production SDK extraction in release order and compile separate graph documents into restricted Lua with typed manifests. Author and version the fixture with GLB plus versioned sidecars and immutable generations.

Source links:

- Missing: this fixture still needs implementation and a reproducible producer.

Required variants:

- installed-sdk
- module-lifecycle
- deterministic-scheduling
- typed-components
- commands-state-inspection
- events
- branches
- bounded-loops
- nonrecursive-functions
- typed-operations
- execution-limits
- source-maps
- preview-debugging

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Deliver production SDK extraction in release order and compile separate graph documents into restricted Lua with typed manifests.
- Meet the shared packet contract for every required variant and parent mapping.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authorized September 13 completion scope. This row adds explicit coverage without replacing any original group; implementation and acceptance remain unfinished.

Retained evidence:

- None qualified for this catalog row.

Next action: Identify the concrete missing feature and dependency, implement in dependency order, then prepare an individually reviewable native packet. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B11

**Morphs and character blend authoring**

Recover morph changes and show source-to-installed-engine character edits with executable/library hashes unchanged.

Origin: added. Parents: B03, B04. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Bounded static morph compilation, validation and CPU evaluation are implemented. The complete general-character source-to-installed-target fixture and native render packet remain missing; engine executable/library hashes must stay unchanged through authored edits.

Source links:

- [test/tools/test_morph_asset.cpp](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/test/tools/test_morph_asset.cpp)
- [src/luminumbra_common/animation/MorphMesh.cpp](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/src/luminumbra_common/animation/MorphMesh.cpp)

Required variants:

- static-morph
- skinned-morph
- weight-animation
- clip-selection
- blend-control
- sockets

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Recover morph changes and show source-to-installed-engine character edits with executable/library hashes unchanged.
- Meet the shared packet contract for every required variant and parent mapping.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Bounded static morph compilation, validation and CPU evaluation are implemented. General character and native target-render acceptance remain outstanding; static preview does not implement general morph animation.

Retained evidence:

- None qualified for this catalog row.

Next action: Retain bounded static morph compile/CPU fixtures; prepare general-character and native target-render packets for the outstanding scope. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B12

**Root motion, retargeting and IK**

Qualify actual runtime poses and trajectories with supported rig identity and bounded constraints.

Origin: added. Parents: B03, B04. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Qualify actual runtime poses and trajectories with supported rig identity and bounded constraints. Author and version the fixture with GLB plus versioned sidecars and immutable generations.

Source links:

- Missing: this fixture still needs implementation and a reproducible producer.

Required variants:

- root-motion
- retargeting
- ik
- humanoid
- non-humanoid

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Qualify actual runtime poses and trajectories with supported rig identity and bounded constraints.
- Meet the shared packet contract for every required variant and parent mapping.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authorized September 13 completion scope. This row adds explicit coverage without replacing any original group; implementation and acceptance remain unfinished.

Retained evidence:

- None qualified for this catalog row.

Next action: Identify the concrete missing feature and dependency, implement in dependency order, then prepare an individually reviewable native packet. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B13

**Authoritative physical animation**

Establish explicit physics phases and one transform owner; prove impact/recovery, limits and deterministic continuation without relying on stills.

Origin: added. Parents: B03, B04, R22. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Establish explicit physics phases and one transform owner; prove impact/recovery, limits and deterministic continuation without relying on stills. Author and version the fixture with GLB plus versioned sidecars and immutable generations.

Source links:

- Missing: this fixture still needs implementation and a reproducible producer.

Required variants:

- pre-physics
- post-physics
- single-transform-owner
- impact
- recovery
- limits
- snapshot-continuation
- isolated-replication
- humanoid
- non-humanoid

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S2: Serialized native capture and source-correlated measurement on freshly identified local hardware.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Establish explicit physics phases and one transform owner; prove impact/recovery, limits and deterministic continuation without relying on stills.
- Meet the shared packet contract for every required variant and parent mapping.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authorized September 13 completion scope. This row adds explicit coverage without replacing any original group; implementation and acceptance remain unfinished.

Retained evidence:

- None qualified for this catalog row.

Next action: Identify the concrete missing feature and dependency, implement in dependency order, then prepare an individually reviewable native packet. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B14

**Reviewed bounded recipes and graph fidelity**

Extend reviewed recipes after the named-recipe integration. Demonstrate stale revision refusal, undo and rollback; optional Banso adapters depend on typed-port fidelity.

Origin: added. Parents: B05, B08. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Bounded recipe validation, revision/refusal, undo and native probe subsets are implemented. The complete typed-port graph-fidelity fixture and native workflow packet remain missing; retain GLB, versioned sidecars and immutable generations.

Source links:

- [tools/blender/authoring/extension_tests/test_recipes.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/test_recipes.py)
- [tools/blender/authoring/extension_tests/native_recipe_probe.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/native_recipe_probe.py)

Required variants:

- named-recipes
- bounded-generation
- bounded-replacement
- revision-check
- undo
- cancel
- rollback
- typed-port-save-reload
- optional-banso-after-fidelity

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Extend reviewed recipes after the named-recipe integration. Demonstrate stale revision refusal, undo and rollback; optional Banso adapters depend on typed-port fidelity.
- Meet the shared packet contract for every required variant and parent mapping.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Bounded recipe validation and native probe tooling are implemented, including revision/refusal and undo checks. Typed-port graph fidelity, complete native workflow packets and visual approval remain outstanding.

Retained evidence:

- None qualified for this catalog row.

Next action: Retain bounded recipe revision/refusal and undo fixtures; qualify graph fidelity and the complete native authoring workflow. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.

## B15

**Evaluated foliage authoring and LOD refusal**

Preserve evaluated assignments and supported instance identity, declare UV/material behavior, and refuse unsafe topology reduction while retaining accepted Tree Small 02 content.

Origin: added. Parents: B02, B07, G01, G03. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Evaluated foliage assignment/instance validation and unsafe LOD reduction refusal subsets are implemented. Full matched native source/target fixtures remain missing; preserve UV/material behavior and accepted Tree Small 02 content.

Source links:

- [tools/blender/authoring/extension_tests/test_evaluated_asset.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/test_evaluated_asset.py)
- [tools/blender/authoring/extension_tests/native_evaluated_asset_probe.py](https://github.com/d-addison/luminumbra/blob/52d6a2642993a60bbadbc9e88fcae382b3739b02/tools/blender/authoring/extension_tests/native_evaluated_asset_probe.py)

Required variants:

- geometry-nodes-materials
- instance-identity
- material-uv
- cutout
- lod-reporting
- deterministic-recipes
- topology-sensitive-refusal

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S4: Ground cover, canopy, material/UV/alpha and LOD controls with accepted pack preservation.
- S7: Installed authoring/character/behavior integration and immutable source/tool/asset identity.
- S9: Neutral production renderer, installed host, SDK and Blender viewport prerequisites.

Capture: **missing**. Implementation and a reproducible installed/native capture producer remain required.

Acceptance checks:

- Preserve evaluated assignments and supported instance identity, declare UV/material behavior, and refuse unsafe topology reduction while retaining accepted Tree Small 02 content.
- Meet the shared packet contract for every required variant and parent mapping.

Source reconciliation at `52d6a2642993a60bbadbc9e88fcae382b3739b02`: Evaluated-foliage assignment/instance validation and unsafe LOD reduction refusal subsets are implemented. Full native source/target packets, graph fidelity and visual approval remain outstanding; accepted tree packs are retained.

Retained evidence:

- None qualified for this catalog row.

Next action: Retain evaluated-foliage and LOD refusal fixtures; capture matched native source/target material, UV and instance evidence while preserving accepted tree packs. Native acceptance not produced for v0.3.0 is deferred to v0.3.1.
