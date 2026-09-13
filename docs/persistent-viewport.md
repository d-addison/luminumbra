# Persistent installed viewport host

This recovery checkpoint extends the [installed static renderer](static-preview.md)
with an authenticated persistent host and an external Python mailbox broker.
It does not yet provide a Blender `RenderEngine` adapter. Recovered source is
qualified anew; historical execution receipts retain their original source identities.

The host accepts complete camera and instance state over inherited binary pipes.
It loads a service-compiled immutable prefab generation, atomically replaces its
complete instance set, and returns synchronized color, reversed depth and coverage.
Camera, transform and resize updates retain existing geometry and texture uploads.
Removal and restoration use the same complete-state protocol. Every response
echoes the admitted state and records the actual renderer matrices.

The startup configuration fixes the project root. A random session key authenticates
bounded messages, with duplicate-key, revision, generation, plane and replay checks
on both sides. The broker holds one active request and one pending state, and
publishes through two leased frame slots. It forwards an authenticated stop after
the active request, bounds shutdown, reaps its child and retains bounded diagnostics.
The native host has a 60-second active-operation watchdog and writes a compact
session receipt after clean shutdown. Its engine, shader and source-input hashes
are checked before and after the session.

The current render profile accepts opaque and cutout static prefabs, rigid camera
views, and symmetric perspective or axis-aligned orthographic projections with
finite reversed depth. Orthographic offsets are preserved and the camera kind is
derived from the supplied projection matrix. The wire extent is
limited to 1280 by 720. Orthographic and shifted cameras, translucent materials,
skinning, morph animation, Blender presentation and interactive overlays remain
unfinished. The portable basis helpers and protocol fixtures do not qualify these
features or a native GPU.

## Reproduce installed acceptance

Build with `LUMINUMBRA_ENABLE_STATIC_PREVIEW=ON`, then install the `StaticPreview`
component into an isolated prefix. Supply an existing project and an explicitly
pinned service-compiled generation:

```sh
python3 -B test/static_preview/installed_viewport.py \
  --host /absolute/sdk/bin/luminumbra_preview_capture \
  --project /absolute/project --generation GENERATION_ID \
  --manifest-sha256 MANIFEST_SHA256 --output /absolute/fresh-result \
  --broker
```

The probe places its synthetic service fixture in view, sends seven states, and
checks visible color changes, unchanged repetition, resize, complete removal and
restoration, actual camera matrices, upload reuse, authenticated shutdown and
unchanged installed engine/generation bytes. It writes original frame planes,
headers, timings, inputs and the host receipt. On Linux, add `--software-only
--xvfb /usr/bin/Xvfb` for an owned llvmpipe display. Software runs do not qualify
native GPU performance. A direct-pipe probe omits `--broker`.

Request-to-frame timing ends before evidence writes. It includes transport and
validation but excludes Blender presentation; it cannot establish 30 FPS or
camera-to-presentation latency. Still-frame hashes cannot establish animation
quality. No run grants visual approval.

Portable tests are `python3 -B -m unittest discover -s test/viewport` and
`python3 -B -m unittest discover -s tools/blender/authoring/extension_tests
-p test_viewport_math.py`. CTest registers `ViewportProtocol.*`,
`StaticSceneTest.*` and the compiler-dependency source identity check. Native
Windows installed-host acceptance, real Blender presentation, failure recovery,
multi-viewport and production content remain required before this work can land.
