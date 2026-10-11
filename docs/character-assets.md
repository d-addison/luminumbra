# Character assets and animation clips

`asset_processor source.glb character.lmesh` compiles a selected skinned scene
to LMS2 and writes sibling `character.<clip-name>.lanim` files. The tool
accepts `.gltf` and `.glb` sources through the same importer; the runtime loads
the compiled assets.

One compiled LMS2 represents one active skin. The importer follows each selected
mesh node's skin reference; unused skin definitions do not choose the palette.
Multiple active skins and unskinned attachments require separate assets.
Joint transforms determine skinned placement; glTF mesh-node transforms do not.

The skeleton contains every joint ancestor, including non-joint armature and
helper nodes, in parent-before-child order. Vertex influences and inverse binds
are remapped together. Ancestors can carry animation tracks. The combined palette
must fit 256 transforms, and vertices must fit four influences. Extra influence
sets require an explicit reduction before import.

Matrix-form bind nodes are decomposed into translation, rotation and signed
scale. Nonfinite or singular transforms, joint shear, duplicate joint identities,
invalid inverse binds, malformed skin attributes and unsupported required
extensions fail conversion. Matrix decomposition accepts orthogonality error up
to `1e-5` in normalized matrix columns. Joint and ancestor names currently use
32-bit FNV-1a identities; duplicates and hash collisions require distinct names.
Persistent authoring IDs are not represented in LMS2.

The output bounding sphere covers the quantized skin in its default bind pose.
It is not an animated-bounds guarantee. Renderer fidelity under animated scale,
morphs, sockets, retargeting, IK and physical animation need their own contracts.

## Clip versions

LMS2's version and layout remain unchanged. LANM has a 16-byte file header:
magic, version and track count as `u32`, followed by duration as `f32`.
Records use little-endian scalar fields on the supported platforms.

| LANM version | Track header | Playback |
| --- | --- | --- |
| 1 | Four `u32`: joint name hash, target type, key count, component count | Existing vector lerp and quaternion nlerp |
| 2 | Version 1 fields followed by a `u32` interpolation mode | Explicit STEP, LINEAR or CUBICSPLINE |

The importer writes version 2. Existing version 1 clips retain their original
playback math, and the committed legacy pose checksum remains applicable.
Consumers must support version 2 before using newly compiled clips; old consumers
refuse the new version. No committed clip files are rewritten by this change.

The single-rig skinned visual smoke deliberately writes version 1 to preserve its
original bytes and motion. Its CPU regression runs the same fixture producer used
by the visual scenario, reloads the mesh and clip through the runtime readers,
and checks legacy playback. Importer regressions separately exercise version 2
LINEAR, STEP and CUBICSPLINE clips.

Target types are translation `0`, rotation `1`, and scale `2`, with three
components for translation/scale and four XYZW components for rotation.
Interpolation modes in version 2 are STEP `1`, LINEAR `2`, and CUBICSPLINE `3`.
Each header is followed by `keyCount` float times, then float values. STEP and
LINEAR use one vector per key. CUBICSPLINE uses three vectors per key in glTF order:
incoming tangent, value, outgoing tangent. Tangents are multiplied by the interval
duration during Hermite evaluation; quaternion results are normalized without
changing the authored tangent signs.

STEP holds the previous value until the next key, including exact key boundaries.
LINEAR rotation follows the shortest spherical arc using the engine's deterministic
math approximations, with normalized lerp for nearly coincident keys. The angular
error regression limit is `0.001` radians. Legacy nlerp and pose blending retain
their existing arithmetic. Animation sampling keeps contraction disabled.

Times must be finite, nonnegative and strictly increasing; values must be finite.
Version 2 rotation keys must have unit length (squared-norm tolerance `0.001`);
legacy version 1 retains its nonzero-key requirement.
Import currently accepts dense float accessors. Morph
channels and tracks outside the compiled skeleton are refused. Clip names use
portable ASCII letters, digits, spaces, dots, underscores and hyphens, with no
trailing dots/spaces or case-insensitive duplicates.

The importer validates all clips before opening any mesh or clip output. The
readers validate file sizes before allocating from counts and leave the caller's
previous asset intact on rejection. Multi-file writes and I/O failures are still
not atomic publication: callers needing generation guarantees must stage and
validate the complete result before publishing it.
