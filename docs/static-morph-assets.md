# Static morph assets and CPU evaluation

The asset compiler's explicit `.lmorph` output selects a bounded static morph
profile. The runtime loads its owned geometry and evaluates caller-provided
weights on the CPU. Existing `.lmesh`, LMS2 and LANM formats retain their current
behavior, including refusal of morph targets in the ordinary mesh profile.

```sh
cmake --preset release
cmake --build --preset release --target asset_processor morph_asset_test --parallel 2
build/release/bin/asset_processor source.glb result.lmorph
ctest --preset release -R '^MorphAssetTest\.' --no-tests=error --output-on-failure
```

The `.lmorph` invocation accepts no LOD, simplification, texture or primitive
selection switches. Validation completes before an output is opened. A malformed
source leaves an existing output unchanged; this compiler write does not provide
a power-loss durability or saved-world transaction guarantee.

## Accepted geometry

The source must be a self-contained GLB of at most 256 MiB, with one embedded
buffer, at most 4096 nodes and exactly one selected static mesh instance with one
triangle primitive. Selection uses the declared default scene, otherwise the
single scene, otherwise all parentless nodes. Multiple scenes without a default,
repeated/cyclic selected nodes and multiple mesh instances are refused.

Base POSITION and NORMAL are dense float VEC3 accessors; optional TEXCOORD_0 is
dense float VEC2. Normals must be nonzero. Indices may be dense unsigned 8-, 16-
or 32-bit values, or the primitive may use sequential nonindexed triangle order.
The profile accepts 1–64 targets containing POSITION and/or NORMAL float VEC3
deltas with exactly the base vertex count. Omitted supported deltas are zero.
All offsets, counts, strides, alignment, finite values and byte extents are
checked before data is decoded.

Target index/order is the target identity. Equal base vertices with different
target deltas remain distinct: this profile preserves the identity remap and
does not weld, reorder or simplify geometry. Numeric source material index is
retained as provenance; material contents are not compiled by this geometry
profile. Target names and persistent authoring IDs are not represented.

Default weights use node weights, then mesh weights, then zero. Deltas are added
in local space before transforms. This follows the relevant
[glTF morph target rules](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#_morph_targets).
The profile refuses sparse data, skins, animations, external buffer/image URIs,
declared extensions, optional mesh instancing and unsupported geometry extensions.
Tangents, vertex colors, extra UV sets and unknown base/target attributes are
also refused. These restrictions define this initial subset; they do not imply
those features are invalid glTF.

## Runtime contract

`animation/MorphMesh.h` exposes an immutable `MorphMeshAsset`, a validated mutable
compiler builder, explicit byte/file loading and `EvaluateMorphMesh`. The factory
is the only construction path; callers cannot mutate published vertex or target
arrays. Failed loading or evaluation retains the caller's prior asset/frame.

The evaluator requires exactly one finite float weight per target. Negative and
greater-than-one weights are supported without clamping. It accumulates the
weighted local deltas in double precision in target order, normalizes the result,
then applies `placement * source_world` and its inverse-transpose normal matrix.
The source world matrix retains full affine parent composition, including shear.
Projective/singular transforms, zero resulting normals and positions that cannot
be represented as finite float32 values are refused.

`MorphFrame` owns the evaluated vertices and retains its immutable source asset
for triangle indices. Its bounds enclose the actual output float positions.
`reverse_front_face` identifies mirrored transforms; a consumer must honor this
flag. UVs and index order remain unchanged. Evaluation is synchronous and allocates
a candidate frame; it does not claim a GPU deformation or edit-latency budget.

## LMOR version 1

This is a separate asset container, not a saved-world or replication format.
All scalar fields use explicit little-endian encoding, with no struct padding.

| Section | Layout |
|---|---|
| Fixed header, 32 bytes | Eight u32 values: magic `LMOR`, version 1, flags 0, vertex count, index count, target count, source material index (`UINT32_MAX` if absent), reserved 0 |
| Source world matrix, 64 bytes | Sixteen column-major float32 values |
| Defaults | One float32 per target |
| Base vertices | POSITION xyz, NORMAL xyz, TEXCOORD_0 uv: eight float32 values per vertex |
| Indices | One u32 per index |
| Target streams | Target-major, then vertex order: POSITION xyz and NORMAL xyz deltas, six float32 values per vertex |

The exact size is `96 + 4T + 32V + 4I + 24VT` bytes. Bounds are one million
vertices, three million indices divisible by three, 64 targets, and 256 MiB total.
Unknown versions/flags, nonzero reserved fields, malformed numbers or indices,
truncation and trailing bytes are rejected before publishing an asset.

## Qualification scope

The focused tests call the actual importer, reload its actual LMOR bytes and
evaluate independent expected positions/normals. They cover default precedence,
fractional/negative weights, exact endpoints, identity remapping, parent shear,
mirroring, placement, bounds, unchanged repeated encoding and transactional
refusals. The regular asset and skeletal tests continue to cover existing formats.

This slice does not implement skinned morphs, animated morph-weight tracks,
retargeting, IK, a service publication profile, renderer integration or a Blender
preview. Those remain requirements for a complete character baseline. CPU results
do not establish native driver, visual or performance approval.
