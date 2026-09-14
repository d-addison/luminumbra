# Canonical far-volume discovery and authority

The source now has one volume request, sample, tile and mesh model.
`world/FarVolume.h` includes the generation and meshing contracts instead of
redeclaring incompatible types. `CompileFarVolume` resolves coverage intent,
generates the sparse tile and meshes it using one copied authority. This is a
bounded synchronous source integration. It does not connect the five tiers to
the client runtime, queue, renderer or persistence.

The reference foundation is `70b899e7b2ed60407a9d42da2089e99192d95b54`; the separate
R0 queue/lifetime reference is `6a13b70e95f0c4a152f09e5df3422931c10dccfa`.
The canonical generator/mesher base is
`09ece871589dfa7666c830fa91a4dd6d62bcc92e`. These identities are separate from
this integration's eventual commit and test executable identities.

## Entry points and preserved contracts

| Boundary | Result and obligation |
|---|---|
| `FarVolumeCoverageIntent` | Integer tier/tile key plus optional unaligned float coverage extension. This is unresolved coverage intent, not a second tile request type. |
| `ResolveFarVolumeCoverage` | Samples the original 129×129 height lattice, retains 256 m below the lowest height, unions the whole optional extension, and resolves an integer brick-aligned interval. An exact top brick plane includes the next air boundary. |
| `FarVolumeAuthority` | One copy holds height and density/material callbacks, numeric/cave choices and caller-owned field identity throughout discovery, generation and the normal halo. Callbacks and their observed data must remain bounded, pure and immutable. |
| `CompileFarVolume` | Produces one canonical tile and local-double mesh, actual geometry bounds, work counts and optional legacy integrity view. Any phase refusal leaves the previous output unchanged. |
| `CompilePristineFarVolume` | Holds one world-generation sampling scope across the synchronous operation and forwards the original terrain height, pristine density and material classification. Callers keep the world alive and must not already hold the nonrecursive scope. |
| `FarVolumeAuthorityLimits` | Required limits on actual height and density callback calls. A 131×131 horizontal height cache covers discovery and one-sample normal halos. Generation's repeated border calls and mesher cache misses count against the density limit. These are work limits, not adopted runtime memory budgets. |

The accepted spacings remain 4/8/16/32/64 m, brick edges 16/32/64/128/256 m,
tile edges 512/1,024/2,048/4,096/8,192 m and outer radii
1,024/2,048/4,096/8,192/16,384 m. The canonical kernels retain signed 64-bit
world origins, explicit aligned spans, finite float samples and double local
vertices. The pristine float-world facade retains the original exact-coordinate
and surface/span guards; it does not expand the qualified rendering domain.

The original cave alternatives are preserved as alternatives. Both retain the
live density operation sequence at 4 and 8 m. Coarser sampling uses the original
finite eight-midpoint box quadrature with double accumulation in Z/Y/X order.
Band-limited mode retains filtered noise at 16 m and analytic openings beyond;
box-filtered mode retains noise at every coarse tier. The sampler implementation
still owns these choices. This slice does not declare either mode visually or
natively accepted, nor invent a new spectral filter or thin-roof guarantee.

## Explicit numeric profiles and integrity

`RawFloat` leaves finite density bits unchanged. `LegacySigned16` applies the
existing `QuantizeFarLodSdf` then `DequantizeFarLodSdf` to every generation and
halo sample. Its 256-units/m scale, saturation, invalid sentinel and preservation
of nonzero signs are unchanged. Those two functions have moved mechanically
from `FarLodStore.cpp` into `FarDensityQuantization.cpp`; the store header includes
the declarations, and existing store readers retain the same API and encoding.

`FarVolumeLegacyChecksum` validates the canonical retained bricks, exact encoded
density, aligned coordinates and shared sample identity, then constructs the
original signed-XYZ, little-endian sparse integrity stream. It does not reorder
the canonical tile, hash object padding or introduce a persistence format.
A homogeneous air tile and homogeneous solid tile can have the same old sparse
checksum because neither contains retained bricks; canonical content metadata
keeps those results distinct. The checksum is not a complete authority hash.

FSD2 readers remain supported and unchanged; no FSV1 writer or durable authority
overlay/tombstone implementation is added here. The field token must incorporate
the caller's complete seed/parameter/authority and cave/numeric policy identity.
A token alone cannot prove that a saved world has no edits or is pristine.

## Independent reference and focused verification

The fixture exporter in `test/fixtures/far_volume/` separately compiles the pinned
foundation's original discovery, generation, sparse validator and checksum code.
It omits only the unrelated live-world wrapper/include and links the original
quantization function bodies with a declaration-only store shim. The generated
JSON records original source hashes, compiler and actual reference executable
hash. No canonical generation or checksum code supplies its expected results.

Ten cases span all five tiers, both cave labels, negative tile origins,
nonconstant surface heights, deep and tall coverage extensions, saturation,
near-zero density and varying opaque materials. The canonical facade is compared
against every encoded density/material lattice sample through independent dense
lattice CRCs, height CRCs, resolved spans, retained brick counts and the original
tile CRC. These analytic callbacks exercise transport of the cave choice; they
do not qualify the actual noise-cave policy or a native view.

To reproduce the reference, make the pinned Git objects and the existing GLM,
EnTT and nlohmann-json dependency sources available, then run:

```sh
python3 test/fixtures/far_volume/generate_foundation.py \
  --repo . --dependencies /path/to/dependency-sources \
  --build /path/to/new-reference-build \
  --output /path/to/reference.json
```

The build directory must be new. The script performs no fetch, checkout or
runtime launch. Ordinary regression tests read the committed compact JSON and
do not rebuild historical code.

Focused checks also cover both numeric profiles, every valid legacy encoded
value, invalid coordinates/spans, callback exceptions, nonfinite density,
work/candidate/geometry refusals, missing and changed halo authority, valid empty
results, malformed sparse streams and the original box-filter tap order.
The existing generation and meshing suites retain their independent border,
cold-rebuild, winding/complement, material, gradient and bounds checks.
The facade test includes all three public volume headers together. Production
source-list verification compiles the facade, world adapter, world system,
quantization codec, existing store and existing marching-cubes translation units.

The first slice passed 38 focused Linux Release cases and the same 38 cases
under ASan/UBSan. A fresh linked `common_tests` build also passed all 20 existing
`FarLodStoreTest` cases, including malformed FSD2 streams, LMR1 round trips,
interrupted rewrites and retained authoritative edits. Formatting, public-tree
hygiene and strict Doxygen/generated-link checks passed. These CPU receipts are
separate from the foundation reference and all native terrain captures.

## Integration still required

The prepared R0 consumer still uses the old request and result fields. Its next
slice must carry unresolved coverage intent and resolved canonical requests,
retain complete epoch/generation/authority checks, and preserve its descriptor,
payload-slot, completion-epilogue and held-lease lifetimes. The existing worker
scope must be adapted deliberately; nesting it around the new scope-owning
synchronous helper would violate the nonrecursive ownership rule.

Its allocation accounting must cover actual float brick/double vertex sizes,
height and density caches, canonical edges, halo work, output capacity and
simultaneous converted geometry. The prior R0 fixture ceilings are not evidence
that this representation meets them, and this slice does not change those
ceilings. The old flat-normal renderer adapter cannot overwrite the canonical
halo normals while claiming shared curved-normal acceptance.

Discovery still scans the complete surface-plus-extension interval. Deep sparse
cave traversal can therefore exceed the caller's work limits and refuse; paged
vertical discovery, scheduling, arrival fallback, authority edits, water,
residency and upload budgets remain unfinished. Both accepted world profiles
still require p99 ≤16.67 ms, with 8.33 ms reported as the separate target. These
CPU checks supply no Windows, GPU, visual approval or performance acceptance.
