# Installed static prefab preview

The optional static preview module renders a pinned compiled prefab through the
production static geometry pass and deferred `LightingPass`. Its host loads an
installed generation, renders requested frames, and writes paired color, depth,
coverage and identity receipts. It does not start the game client, world streaming,
audio, networking or gameplay services.

This is the static inspection profile for a future editor viewport. The Blender
`RenderEngine`, asynchronous viewport transport and interactive edit latency are
not qualified by this synchronous capture API. The installed C++ interface uses
standard library types; consumers must use a compatible C++ toolchain and runtime.
The project does not promise a stable binary ABI between engine releases.
Construct, use and destroy a `StaticRenderer` on the same thread in a dedicated
render host with exclusive process-wide GLFW ownership. The module initializes
and terminates GLFW itself; another GLFW user or window must not coexist with it.

## Build and consume the installed module

```sh
cmake --preset release -DLUMINUMBRA_ENABLE_STATIC_PREVIEW=ON
cmake --build --preset release --target luminumbra_preview_capture --parallel 2
cmake --install build/release --prefix ./preview-sdk --component StaticPreview
```

The component includes the host, shared renderer, three public headers, CMake
package, five production shader resources and `source-inputs.txt`. System graphics
drivers and the toolchain's shared runtime libraries remain platform dependencies.
The host finds its shader resources relative to its installed executable, so the
complete prefix can be relocated.

A separate C++ consumer uses the installed target:

```cmake
find_package(LuminumbraStaticPreview CONFIG REQUIRED)
target_link_libraries(my_inspector PRIVATE Luminumbra::StaticPreview)
```

The public headers are `luminumbra/rendering/StaticScene.h`, `RenderView.h` and
`StaticRenderer.h`. They expose no OpenGL handles, ECS entity IDs, game camera or
Blender types. `StaticPrefab::Load(project_root, generation_id, manifest_sha256)`
uses the real compiled-prefab loader and verifies the pinned manifest and payloads.
It refuses unsupported material capabilities before publishing a snapshot.

`StaticScene::Replace` assigns a stable caller-owned instance ID. A batch passed
to `UpdateLocalMatrices` names instance/node IDs and full local matrices, with an
expected scene revision. The adapter computes affected descendants, normal
matrices, winding and bounds before committing the batch once. Stale or invalid
batches preserve the old scene. Immutable snapshots retain their generation and
decoded resource lifetimes even after replacement or removal.

The draw pass retains mesh and texture uploads across matrix and camera edits.
The frame reports actual uploads and changed draw bindings; a transform update
does not reload an asset or rebuild its meshes. The first profile still issues
ordinary per-primitive draws and uploads per-draw uniforms. It does not claim
batched instancing or an edit-performance budget.

## Camera and material profile

Provide one complete column-major world-to-view matrix and one complete projection
matrix. The view must be rigid, right-handed and exactly affine. Finite symmetric
perspective and axis-aligned orthographic projections use zero-to-one reversed
depth: near is 1 and far/clear is 0. Orthographic x/y offsets are preserved; each
span is 0.002–2,000,000 metres and each center is within ±1,000,000 metres in view
space. Both profiles require the image aspect ratio. Oblique/sheared projections,
shifted perspective and hybrid projection conventions are refused.
The renderer derives culling planes, eye position and GPU uniforms
from the same validated matrix pair, and records the actual float matrices.
Orthographic lighting uses the camera's constant direction toward the viewer;
perspective lighting uses the direction from each fragment to the eye. The wire
matrix itself selects the camera kind, with no added protocol fields.

Portable `viewport_math.projection()` converts either supported observed Blender
projection to reversed depth, preserving its x/y entries. The explicit
`perspective()` and `orthographic()` functions refuse the other camera kind.
`depth_to_blender()` reprojects either profile with the caller's declared Blender
clip convention. These numerical helpers do not qualify a Blender adapter.

The initial limits are 1–4096 pixels per axis, near distance at least 0.001 metres,
far distance at most 1,000,000 metres, and bounded finite GPU inputs. Authored
deferred positions use RGB32F, including distances beyond binary16's finite range.
Depth uses D32F. The default world-only target retains its existing position format.

`static_preview_render_test` defaults to requiring the actual llvmpipe renderer.
The owned software-display runner clears native profile pins and forces llvmpipe.
To qualify an identified native GPU, set both
`LUMINUMBRA_PREVIEW_EXPECT_NATIVE_VENDOR` and
`LUMINUMBRA_PREVIEW_EXPECT_NATIVE_RENDERER` to its complete observed OpenGL strings,
then run `static_preview_render_test --gtest_output=xml:render-tests.xml` on that
native display. Both strings must match exactly in every rendering test; partial
pins, mismatched identities and known software renderers fail. The XML records
the selected profile and actual vendor/renderer per test. A caller-supplied pin is
an identity assertion and still requires the campaign's hardware/driver receipt;
passing the software suite does not qualify native GPU performance.

Static materials support base color, metallic/roughness, tangent-space normals,
occlusion and RGB emissive bindings. Each binding retains its sampler, authored
mip chain and UV offset/rotation/scale. Color bindings use their declared sRGB
texture format; data bindings use linear formats. Sampling transforms do not
replace the raw UV basis used for normal mapping. Full affine transforms, shear,
inverse-transpose normals, mirrored winding and double-sided materials are
preserved.

OPAQUE and numeric-cutoff MASK materials define coverage. BLEND, unknown material
capabilities, malformed data and unsupported projection profiles are refused.
The studio grade has a defined clear-depth background, material occlusion and
true emissive color. Authored materials bypass the world's terrain snow and
crystal-material substitutions. Existing legacy material behavior remains in its
own branch.

The profile uses scale 1, zero jitter, explicitly zero motion output and no TAAU.
The production game pipeline also refuses TAAU when authored static draws are
enabled; those draws obey its Terrain isolation bit. Enabling temporal history
requires a separate previous-camera/object-transform contract.

## Capture request and receipts

```sh
preview-sdk/bin/luminumbra_preview_capture --request request.json --output fresh-capture
python3 tools/static_preview/validate_capture.py fresh-capture \
  --request request.json --expect independent-expectations.json
```

The output must be a fresh child of an existing directory. An existing output is
left untouched. A 60-second watchdog covers loading, rendering, writing and
renderer shutdown; failure or timeout produces a refusal instead of a completed
capture. The request is a bounded JSON object with this structure:

```json
{
  "schema": "luminumbra.static_preview.request.v1",
  "prefabs": [{
    "id": "asset",
    "project_root": "project",
    "generation_id": "32 lowercase hexadecimal characters",
    "manifest_sha256": "64 lowercase hexadecimal characters"
  }],
  "instances": [{
    "id": "placed",
    "prefab": "asset",
    "placement": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
  }],
  "frames": [{
    "camera": {
      "view": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
      "projection": [1.8106601717798212,0,0,0, 0,2.414213562373095,0,0,
                     0,0,0.001001001001001001,-1, 0,0,0.1001001001001001,0],
      "width": 640, "height": 480, "revision": 1,
      "near_plane": 0.1, "far_plane": 100
    }
  }]
}
```

Replace the generation and manifest placeholders with the service's actual pins,
and position the camera and instances for the asset. A later frame may contain
`updates`, a list of `{ "instance_id", "node_id", "local" }` objects where `local`
is a 16-number matrix. Camera revisions increase on every requested frame. The
host supports up to 8 prefabs, 64 instances, 1024 submitted draw records and 8 frames
per invocation. The scene API separately bounds total nodes/draws and unique
decoded resources to 65,536 and 512 MiB.

Each `frame-N` directory contains `frame.json`, `color.rgba8`, `depth.f32le` and
`coverage.u8`. Row zero is the bottom row. Color uses the production inspection
power-2.2 transfer, with straight alpha exactly 255 for covered pixels and 0 for
the background; framebuffer sRGB conversion is disabled. This is an inspection
image, not a linear scene-color interchange. Coverage is exactly `depth > 0` and
the validator requires every depth sample to be finite and within [0,1].

The top-level `capture.json` hashes the raw frame receipts and records the actual
executable/module bytes, source commit and dirty state, source-input digest,
shader resources, request, generations, process ID and completed shutdown. Frames
retain camera, scene revision, every draw's full matrix/material/generation key,
GPU identity and actual upload/draw counts. Synchronous render/readback duration
is recorded separately; it is not a Blender edit-latency measurement.

The file validator requires independently retained build/process expectations,
including the collected `capture.json` hash. It reconstructs the node hierarchy
and matrix updates from the pinned generation, checks raw plane hashes and
camera/depth/coverage joins, and refuses missing frames, partial output, a failed
process, changed generation or unsupported profile. Native-to-Linux collection
may provide a trusted `project_roots` map without relabelling generation hashes.
The expectations file must never be generated from the capture's own assertions.
Validation reports `visual_approved: false`.

## Qualification

`RenderView` CPU cases cover camera rejection and uniform/culling agreement.
`StaticSceneTest` covers real decoding, material roles, transforms, lifetime and
transactional refusals. `StaticPreviewCaptureContract` exercises the file
validator's adversarial joins. With the optional module enabled, Linux CTest
`StaticPreviewSoftwareGL` starts an owned Xvfb and forces and verifies llvmpipe.
It executes the production shader/pass tests with no skips. This software test is
separate from native driver qualification.

Native acceptance builds the installed component, freezes the actual executable,
module, shader and generation hashes, then captures an initial frame, a full-matrix
edit and an unchanged frame. Run the file validator with the independent process
receipt, check unchanged input hashes and no mesh/texture reuploads during edits,
and retain raw images for review. The production routing regression is
`PassContext.AuthoredStaticDrawsRespectTerrainIsolation`. Native execution,
visual approval, arbitrary Blender shaders and interactive viewport performance
require their own evidence.
