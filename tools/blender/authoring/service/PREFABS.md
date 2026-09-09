# Static prefab compilation

Service 0.3 adds the optional `glb-static-prefab-v1` profile. It compiles local
meshes, textures and a `luminumbra.asset.prefab.v1` descriptor in one immutable
generation. Use the same sidecar and commands as the [build service](README.md),
with `profile` set to `glb-static-prefab-v1`. Every source mesh, material and scene
node needs its persistent correlation ID. Names are labels.

This is an asset compilation contract. Engine prefab instantiation, rendering,
runtime components and engine frames in Blender remain unavailable. Check both
`static_prefab_compilation` and `prefab_runtime_instantiation` in `capabilities`
when selecting a consumer workflow.

## Geometry and placement

The selected default glTF scene supplies the hierarchy. The descriptor records
node IDs, parent relationships, local column-major matrices and references
to shared meshes. A null parent identifies a scene root. Coordinates remain glTF
right-handed Y-up meters. Consumers
compose parent transforms once; meshes contain local-space geometry. Each node
also records its world-space handedness for winding selection.

Each material primitive compiles to an LMSH draw. Identical evaluated geometry
with one mesh ID shares compiled draws, including when an exporter stores its
attributes at different buffer offsets. Attribute representations and element
bytes must agree; different geometry under one ID is refused. Mirrored instances
retain their local transforms and share geometry with ordinary instances.

The initial vertex layout retains position, normal and one selected UV set.
All material maps must use that set and the same texture transform. The draw
records `uv_transform_baked: false`: LMSH contains the original selected UVs.
Apply the material's texture coordinates when sampling. Do not replace geometric
UV derivatives with derivatives of transformed sampling coordinates when deriving
the normal-map basis. Renderer fidelity for this contract requires separate
qualification.

Invalid topology and nonfinite attributes are refused by the native compiler and
output readback. The profile also refuses shear, singular transforms, cycles,
repeated scene nodes, skins, morphs, animation, sparse accessors, vertex colors,
tangent attributes and unregistered scene components. Bake or select a separately
qualified profile explicitly when those features are required.

## Materials and textures

The descriptor preserves a declared metallic/roughness material: linear factors,
base color, normal strength, occlusion strength, emissive values, alpha mode,
alpha cutoff and double-sidedness. Texture records identify compiled LTEX files,
color/data interpretation, sampler settings and map coordinates. Color and
emissive images use the compiler's sRGB mode; normal and data maps use their
respective linear modes. This profile accepts embedded PNG and JPEG images.

Samplers retain glTF wrap/filter enumerants. Omitted values resolve to repeat,
linear magnification and trilinear minification. `KHR_texture_transform` is the
only accepted extension. Unsupported combinations are refused before publication.

For textured MASK materials, coverage-preserving mip construction uses the
authored cutoff divided by the base-color alpha factor. The descriptor retains
both authored values for runtime evaluation. Fully discarded textured cutouts
are refused. This requires the qualified native compiler's `--alpha-cutoff`
support; a manifest hash alone does not establish feature compatibility. LTEX
version 1 remains unchanged. OPAQUE and BLEND interpretation belongs to the
consumer; compilation does not qualify transparent rendering or picking.

## Limits and publication

The profile bounds the GLB JSON to 1 MiB, the selected hierarchy to 4,096 nodes,
each mesh to 64 primitives and compilation to 128 mesh/texture outputs plus the
descriptor. Image dimensions
must be from 1 through 4,096. Source snapshots and complete output sets each
remain bounded to 256 MiB. Reused attribute extraction has a separate processing
budget. Derived compiler inputs are materialized one at a time and removed after
readback rather than duplicating the complete source buffer for every draw.

Every mesh and complete texture mip chain is validated before publishing the
descriptor and generation. Receipts record each compiler input digest, arguments,
output hashes and actual byte/count metrics. Any failed part, cancellation or
stale source revision preserves the previous generation. Consumers retain their
generation ID; there is no automatic collection of published generations.

## Qualification

Portable tests cover extraction, identity conflicts, malformed outputs and
multipart publication failures. Run the installed native consumer separately:

```sh
python tools/blender/authoring/service/tests/native_prefab.py --service /your/install/luminumbra-author --toolchain /your/install/toolchain.json --output /fresh/evidence/directory
```

The native fixture uses transformed shared instances, mirrored placement, two
materials, alternate UV coordinates, normal data and alpha coverage. It compares
repeat output identity, decodes mesh/texture data, checks source edits with
unchanged installed compiler/library hashes and tests last-generation retention.
Run and record each host/compiler profile independently. Screenshots, GPU memory,
lighting fidelity and production content require their own acceptance evidence.
