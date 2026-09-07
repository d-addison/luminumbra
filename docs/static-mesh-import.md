# Static mesh import

`asset_processor source.glb output.lmesh` compiles unskinned glTF 2.0 geometry
into the existing LMSH format. Both `.gltf` and `.glb` inputs are accepted.
Positions remain in glTF's right-handed, Y-up coordinates and meter units.

The importer traverses the default scene, composing each node's transform with
its ancestors. Each mesh instance contributes geometry in scene coordinates.
Normals use the inverse transpose of the composed transform, and negative
determinants reverse triangle winding. Static shear is baked into the vertices.

When no default scene is declared, a single scene is unambiguous and is used.
Multiple scenes require an explicit default. Documents without scenes use their
root nodes; a library with neither scenes nor nodes imports each mesh definition
once at identity. An empty selected scene does not fall back to unused meshes.

This command produces one combined mesh: node hierarchy, object identities and
material bindings are not stored in LMSH. To compile a material part, use
`--primitive N`. This zero-based index counts source primitives across mesh
definitions; every selected scene instance of that primitive contributes to the
output. Bind the resulting mesh to its material in the consuming application.

Each selected primitive must provide indexed triangles, `POSITION`, `NORMAL`
and the base-color material's texture-coordinate set (set zero without a
base-color texture). `KHR_texture_transform`, including its coordinate-set
override, is baked into the single output UV lane. Missing requested coordinates
are an error. Separate normal/ARM map coordinates and arbitrary shader graphs
are not represented by this format; prepare maps using the same coordinates.

Conversion refuses unsupported required extensions, GPU instancing descriptors,
morph targets, animations on unskinned documents, singular transforms, sparse or
malformed attributes and indices, and nonfinite geometry or bounds. Bake a static
pose, expand sparse accessors or realize procedural instances before conversion.
Diagnostics identify a rule
and, where applicable, the source node and primitive. Invalid selected primitives
fail the conversion instead of being silently omitted.

Validation failures occur before opening the output, preserving an existing
mesh. The CLI reports a nonzero exit status on failure. Output I/O errors and
multi-file LOD builds are not atomic generation transactions; callers that need
publication guarantees must build and validate in a separate directory.

Identity geometry retains its existing binary layout. Skinned inputs use the
separate [character asset path](character-assets.md). These static import rules
do not qualify morphs or prefab authoring support.
