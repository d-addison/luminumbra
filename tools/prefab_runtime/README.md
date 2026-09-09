# Compiled prefab runtime

The optional `luminumbra_prefab_runtime` library loads a pinned
`glb-static-prefab-v1` generation and instantiates its hierarchy in an actual EnTT
registry. `luminumbra_prefab_inspect` is its installed, headless consumer. It emits
the resulting ECS hierarchy, world transforms, normal matrices, bounds and
material bindings as JSON. This path needs no Blender or graphics context.

Enable and install the consumer from an engine checkout:

```sh
cmake --preset release -DLUMINUMBRA_ENABLE_PREFAB_RUNTIME=ON
cmake --build --preset release --target luminumbra_prefab_inspect prefab_runtime_test --parallel 2
ctest --test-dir build/release -R '^Prefab(Digest|RuntimeTest)\.' --no-tests=error --output-on-failure
cmake --install build/release --prefix /your/install --component PrefabRuntime
```

The option defaults to OFF. Normal engine builds, world loading and saved-world
formats are unaffected. The installed executable needs the C++ runtime libraries
of its compiler profile; it has no asset paths tied to its installation directory.

Compile a prefab with the [installed authoring service](../blender/authoring/service/PREFABS.md).
Select an explicit retained generation and compute the SHA-256 of its
`manifest.json`. Pass both pins to the consumer:

```sh
/your/install/bin/luminumbra_prefab_inspect \
  --project /your/authoring/project \
  --generation 0123456789abcdef0123456789abcdef \
  --manifest-sha256 YOUR_MANIFEST_SHA256 \
  --instance placed.prop \
  --placement '[1,0,0,0,0,1,0,0,0,0,1,0,10,2,3,1]'
```

The generation is a 32-character lowercase hexadecimal job ID, not a path.
Only `.luminumbra-author/generations/ID` below the selected project is opened.
The manifest pin is mandatory; `current.json` is never followed. Success writes
one instance report to stdout and exits zero. Refusal writes a diagnostic to
stderr and exits nonzero without an instance report. The consumer does not write
to the project, the generation, or any saved world.

## Runtime API and ownership

Include `authoring/PrefabRuntime.h` and link `luminumbra_prefab_runtime`:

```cpp
auto asset = Luminumbra::Authoring::PrefabAsset::Load(project, generation, manifest_sha256);
entt::registry registry;
Luminumbra::Authoring::PrefabScene prefabs(registry);
prefabs.Replace("placed.prop", asset, placement);
auto report = prefabs.Inspect("placed.prop");
// A validated replacement keeps the caller placement and persistent IDs.
prefabs.Replace("placed.prop", replacement_asset);
prefabs.Remove("placed.prop");
```

`PrefabAsset` owns immutable validated LMSH and LTEX bytes. Instances share these
snapshots and retain them even when files are later replaced or removed. Distinct
nodes keep their `(instance_id, node_id)` identities, persistent mesh and material
IDs, labels, parent links and multiple draw bindings. Transform-only parents are
real entities. Reload replaces transient EnTT handles; callers should retain
persistent identities instead of handles across replacement.

`PrefabNodeComponent` retains full double-precision local and world affine
matrices. Parent transforms compose once, before the caller placement. Rotated
nonuniform parents can create world shear; it is preserved. The normal matrix is
the inverse transpose of the full world linear transform. Winding includes both
authored hierarchy and caller mirroring. World AABBs transform all eight corners
of bounds recomputed from mesh positions.

Replacement validates all transforms and constructs every candidate entity before
discarding the old instance, including equal-count reloads. A loading, validation
or component-construction failure leaves the previous instance intact. Calls are
single-threaded; the registry must outlive the scene. The scene owns its entities:
external code must not destroy them, change their ownership/hierarchy components,
or throw from destruction callbacks. Registry callbacks must not reenter the scene.
Remove through the owner. Instance revisions
increase on each successful replacement.

## Accepted data and refusals

The loader verifies the manifest pin, every exact member hash and declared byte
count, and decoded mesh/texture metrics. It retains only a bounded, complete
generation: JSON documents up to 1 MiB, 4,096 nodes, 64 draws per mesh and 128
compiled files, with at most 256 MiB of output bytes. It refuses duplicate JSON
keys, unknown schema/descriptor/material fields, missing or unreferenced assets,
duplicate IDs, cycles, missing parents, invalid mesh indices, nonfinite geometry,
incomplete mip chains and unsupported sampler/UV combinations. Singular or
nonfinite transforms and local shear are refused. Finite affine world shear is
supported; a world transform must remain invertible for its normal matrix.

Member names are restricted to the service's compiled file names. Descendant
symlinks and Windows reparse points are refused. The caller chooses a trusted
project root. This is not a filesystem sandbox against an attacker concurrently
replacing directory entries; use a quiescent retained generation. Hashing and
decoding consume the same owned readback bytes.

Materials retain all compiled v1 factors, texture bytes, encodings, samplers,
selected UV metadata, normal and occlusion strengths, alpha modes and cutoffs.
They are validated runtime bindings. The current GBuffer renderer does not consume
these components: this module does not qualify material rendering, transparency,
picking, physics, scripts, replication, save persistence or Blender preview
frames. `renderer_qualified` remains false in every inspection report. The
service's own capability flag still describes its service operations; it does not
probe for this separately installed consumer.

## Qualification

`prefab_runtime_test` exercises the real loader and ECS lifecycle, including full
hierarchy depth, shared assets, affine transforms, equal-count replacement and
rollback. The POSIX symlink test is compiled only on POSIX; Windows reparse
behavior needs host qualification instead of a permission-dependent skipped test.
Run ASan for the ownership path using the normal `debug-asan` preset and enabling
the same option.

The optional installed acceptance script compiles the existing authored fixture
with the actual installed service and compiler, then invokes the installed C++
consumer. It checks hierarchy and placement, mirrored normals, two material
bindings, an edited generation, old-generation retention and corrupt-member
refusal. It writes fresh logs, hashes and a receipt:

```sh
python test/authoring/installed_prefab_runtime.py \
  --service /your/install/luminumbra-author \
  --toolchain /your/install/toolchain.json \
  --consumer /your/install/bin/luminumbra_prefab_inspect \
  --output /fresh/acceptance/directory
```

Report each host/compiler execution separately. CPU acceptance establishes runtime
loading and instantiation; it does not establish rendered fidelity or GPU cost.
On Windows, add `--windows-junction` to create an owned NTFS junction and verify
the installed consumer refuses it as a reparse point. This explicitly requested
host check fails if junction creation or the refusal fails; it is not skipped.
