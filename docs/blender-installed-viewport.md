# Installed Blender viewport checkpoint

This checkpoint adds a static `RenderEngine` adapter to the optional Blender
5.1.0 extension. It connects the marked collection to the separately installed
[persistent native host](persistent-viewport.md). The native GPU presentation
checkpoint below passed; the composed editor acceptance is still being qualified. No visual result or
authoring performance target is approved by this document.

## Installed workflow

1. Install the `StaticPreview` SDK component and qualify that installed image
   with `test/static_preview/installed_viewport.py`. The MinGW component now
   installs its three compiler-resolved runtime DLLs and their notices. Qualify
   their actual loading with the compiler directory removed from runtime PATH
   before treating a Windows installation as portable.
2. Run `tools/blender/authoring/seal_viewport_installation.py` with the installed
   host, successful qualification, its native session receipt, and a fresh
   `installation.json` in the SDK root. It binds the source identity and every
   installed file to those exact receipts. An unqualified or modified SDK cannot
   be sealed by supplying a replacement source label.
3. Package the extension with `tools/blender/authoring/package_extension.py` and
   install the archive through Blender's extension preferences. Configure the
   project, external Python, installed authoring service and toolchain manifest.
4. Mark the collection, enable **Preserve static prefab and materials**, and
   build it. Set the installed preview host and SDK manifest, then choose
   **Open Installed Viewport**. **Held generation** can pin an older immutable
   generation; an empty value follows the project's current published pointer
   and refuses a pointer belonging to another asset.

Geometry, materials and hierarchy remain owned by the `.blend` document. The
adapter sends evaluated local matrices and Blender's observed camera. Matrix
edits use the existing immutable generation; geometry and material changes use
the existing debounced publication service. A changed hierarchy waits for the
matching new descriptor. Reviewed recipe operations keep their revision and undo
rules from [the recipe interface](authoring-recipes.md).

## Current implementation and limits

Blender data reads, texture uploads and GPU drawing remain on the main thread.
The generation reader and authenticated client perform filesystem and protocol
work on workers. The client has one replaceable desired state, two leased frame
slots and one completed frame. Each engine instance retains at most one presented
frame and 512 timing samples. SDK changes, stale generations, stale frames and
invalid revisions are refused. Each viewport owns a broker process tree; Windows
uses a kill-on-close Job Object created atomically with its broker, and private
session directories have a protected current-user ACL. POSIX uses an owned
process group and mode-0700 directories. Reset, file load and unload close these
owned clients. See the [client contract](../tools/blender/authoring/VIEWPORT_CLIENT.md)
for bounds, failure handling and shutdown receipts.

The adapter currently targets Blender's OpenGL backend and the host's static
prefab profile: finite unshifted perspective or axis-aligned orthographic cameras,
up to 1280×720 transmitted pixels, one marked collection per viewport and at most
1024 nodes. Integer downsampling uses a compensated presentation rectangle to
preserve projected selection coordinates. Orthographic clip intervals crossing
the camera origin are translated into the host's positive-distance interval.
The depth plane is reprojected into Blender's actual camera; uncovered samples
write clear depth. The existing production lighting pass already produces
display-encoded color, so Blender's `IMAGE` shader handles output-space conversion
without applying its scene tone mapper again.

`LUMINUMBRA_RenderEngine.capture_frame(fresh_directory)` explicitly exports the
current accepted frame: original RGBA/depth/coverage planes, lossless PNG and
their joined header/hashes. It refuses absent or stale presentation. The PNG
changes only row order from OpenGL's bottom-first storage. Captures are pending
visual review and do not independently prove Blender's presentation.

Final rendering, shifted perspective cameras, general characters and behavior
graphs are not implemented by this adapter. Continuous-motion presentation,
multiple viewports, selection/gizmo occlusion, unload/crash paths, edit refresh
latency and the 30 FPS target require their native editor packets. Linux Blender
qualification is separate. Mathematical, synthetic process and software-renderer
checks support those packets; they do not replace them.

## Qualification tools

- `extension_tests/native_viewport_presentation.py` exercises actual Blender GPU
  color/depth readbacks, clear coverage, front/behind geometry and GPU-state
  restoration. Its synthetic planes isolate presentation behavior.
- `extension_tests/native_viewport_engine.py` installs the actual extension in an
  isolated GUI, builds an authored prefab, drives the installed native host and
  captures transform, camera, material and geometry changes plus crash recovery.
  It also drives two viewports, resize, selection/gizmo controls, undo, held
  generations and unloading while hosts are active. It retains original editor
  screenshots and joins captured frames to area cameras, native receipts and
  unchanged SDK hashes. Those checks are prepared, not yet passed natively.
- `test/viewport/probe_windows_client.py` checks Windows ownership and protocol
  failure paths using synthetic children. It writes a fresh failed receipt before
  execution and cannot qualify a renderer or Blender.

The Blender probes require an owned external process deadline, isolated user
directories, executable/exporter pins and serialized GPU execution. Successful
process exit alone is insufficient: inspect the exact checks and joined receipts.
Feedback samples in the editor probe end at texture upload; compositor display
timing and the authoring targets remain explicitly unqualified.

## Native GPU presentation checkpoint

On September 13, source `bb9e4ce2dea1d5af8bf7cf317d6046dda4f5c5d6`
passed all 35 presentation checks in actual Windows Blender 5.1.0, OpenGL 4.6,
on the NVIDIA GeForce RTX 5070 Ti with driver 616.92. The owned Blender process
exited successfully and was reaped in 7.171 seconds. Exact executable, module,
probe and artifact identities are preserved in the
[original receipt](evidence/blender-presentation-20260913/receipt.json) and
[process receipt](evidence/blender-presentation-20260913/process-receipt.json).

These are synthetic synchronized planes passed through the production
`FrameDraw` implementation. Perspective, orthographic intervals crossing the
camera origin, and an odd 1281×721 downsampled region passed color, depth,
front/behind overlay, clear-coverage and GPU-state controls. An
[independent review](evidence/blender-presentation-20260913/independent-review.json)
joined all 36 original artifacts and recomputed every presented pixel: maximum
RGB error was zero and maximum depth error was below 3.56e-8.

The [perspective](evidence/blender-presentation-20260913/perspective.png),
[orthographic](evidence/blender-presentation-20260913/orthographic-negative-near.png)
and [downsampled](evidence/blender-presentation-20260913/odd-downsample.png)
diagnostic PNGs encode the original RGBA bytes losslessly, reversing row order
only. Their [joins](evidence/blender-presentation-20260913/png-joins.json) retain
the original raw-plane hashes; the complete 40-file, 40,433,246-byte packet is
archived in the isolated campaign evidence lane. These images are pending
review and are not composed showcases.

This receipt retains its original source identity. Later framebuffer clearing,
metadata recovery, Windows file-sharing and integration changes require their
own runs. No complete RenderEngine workflow, portable Windows DLL closure,
authoring latency/30 FPS target, Linux Blender qualification or visual scenario
approval follows from this presentation checkpoint.
