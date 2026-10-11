# Evaluated foliage input validation

Geometry Nodes can replace material assignments and remove or generate UV layers.
The former static adapter checked authored object slots and the unevaluated mesh,
while Blender 5.1's pinned glTF exporter exports the evaluated mesh and its material
slots when applying modifiers. A material introduced by Set Material could bypass
the supported Principled subset; a removed UV layer could pass the initial check.

The adapter now checks those evaluated inputs and releases temporary meshes even
when validation fails. Object material overrides remain authoritative for meshes
without modifiers, matching the exporter. The private export worker revalidates
the collection after loading its snapshot and setting the export frame, including
the requested static/character profile. It refuses unsupported evaluated prefab
materials before invoking glTF.

The focused portable suite exercises 19 evaluated-input and loaded-snapshot
contracts. The combined extension suite has 34 passing tests. These use test
objects and do not establish Blender or export fidelity.

The queued native probe is
`tools/blender/authoring/extension_tests/native_evaluated_asset_probe.py`. It uses
actual Blender 5.1.0 build `adfe2921d5f3` and the pinned exporter 5.1.18 source. Its
25 required checks cover a newly generated eight-leaf fixture, Set Material absent
from authored slots, unsupported shader/UV removal refusal and recovery, real
repeated GLB export, material identity, alpha cutoff, 14-of-16 cutout texels,
all 16 leaf triangles, UV domain, absence of added persistent mesh datablocks and the existing refusal
of unrealized Geometry Nodes instances. It records original fixture/GLB/image
hashes and source/tool identities in a fresh failed-by-default result directory.
It has not run; no native success, renderer or visual approval is claimed.
The queued fixture includes actual collection-library save/load and calls the
worker's validation helper on both live and appended inputs. Full private
background-worker execution and installed compilation remain separate native
acceptance obligations.

This closes a validation bypass within the existing static profile. Supported
ordinary linked mesh objects retain their current identity behavior; arbitrary
Geometry Nodes instance identity is still unsupported. Evaluated materials must
already carry any desired authored persistent IDs; the current ID recipe still
selects authored slots. Named topology-aware reduction recipes, explicit coverage
and LOD authoring reports, production canopy/mips, engine preview comparison and
new game-owned foliage remain unfinished. The legacy simplifier preserves
disconnected components and reports actual counts when its error/topology limits
prevent the target; that does not qualify the planned named reduction/refusal
workflow. Tree Small 02 packs and all 30,250 fitted leaf surfaces are unchanged.
