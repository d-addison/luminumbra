# Far-volume integration requires one shared representation

The [volume foundation](https://github.com/d-addison/luminumbra/pull/176) and
[generation kernel](https://github.com/d-addison/luminumbra/pull/177), followed by
[mesher kernel](https://github.com/d-addison/luminumbra/pull/179), were developed in
separate campaign lanes. Their production headers declare incompatible types
with identical names in `Luminumbra::World`. They cannot be combined as two
independent runtime APIs: including both produces redefinitions, and using
different definitions across translation units violates the C++ one-definition
rule even if a particular link succeeds. Neither a mechanical merge nor a rename
establishes which field and geometry representation owns the runtime result.

This comparison uses volume foundation source `70b899e7b2ed60407a9d42da2089e99192d95b54`
and generation kernel source `3d77fc9962f03c3385b3b2ec6f4bef1efab5f4fe`; the mesher addition
began at mesher kernel `fd0e1ebc44e808c92577d2d0aa316d7ee056d4d9`.
The separate release lane also has a prepared R0 asynchronous consumer and
renderer adapter built around the volume foundation representation. Its queue/lifetime
work must be reconciled before a shipping runtime caller is added.

## Concrete conflicts

| Shared name or boundary | Volume foundation / prepared R0 | Generation / mesher kernels |
|---|---|---|
| `FarVolumeSpan` | Float world-Y endpoints; optional request extends the surface/depth band | Signed 64-bit brick-aligned endpoints; explicit requested interval |
| `FarVolumeRequest` | u32 tier, i32 tile X/Z, optional extra span and experimental cave mode | One-based tier and i64 tile key, explicit span and opaque immutable field identity |
| `FarVolumeSample` | Quantized i16 density at 256 units/m, invalid sentinel and saturation | Unquantized finite float density; density bits preserved, including subnormals and signed zero |
| `FarVolumeBrick` | Signed global brick indices, sample array and CRC | Integer world-metre origin and sample array; no CRC |
| `FarVolumeTile` | Encoded Y interval, sampled brick count, cave mode, sorted `(x,y,z)` bricks and CRC | Full request, sampled world bounds, content, candidate/call counts and canonical `(z,x,y)` bricks |
| Generation | Discovers 129² surface heights and a shared quantized lattice; actual pristine sampler and cave alternatives | Caller-supplied bounded pure sampler; explicitly supplied span; no cave policy or discovery |
| `FarVolumeVertex` / `FarVolumeMesh` | Absolute float positions/materials, indices; prepared adapter expands flat-normal triangles | Integer mesh origin plus double local positions and same-authority halo normals; used-vertex bounds and counts |
| `FarVolumeMeshLimits` | Default vertex/index/byte ceilings | Required cell/sample/vertex/index counts; no adopted runtime byte ceiling |
| Errors/cancellation | Exceptions; prepared queue checks cancellation between whole phases | Typed transactional refusal; sampler `nullopt` can stop within sample work |
| Integrity/identity | Explicit CRC covers quantized payload; queue envelope owns seed/parameters/authority/generation | Every resident sample is checked against the declared field and shared-coordinate cache; no durable payload hash |

The common 5×5×5 constants are also redefined in both headers. R0's result
metadata, reservations, conversion costs and request coalescing currently depend
on the volume foundation field types and capacities. Directly substituting a float sample or
smooth-normal mesh changes those assumptions and requires new accounting tests.
Conversely, silently quantizing generation kernel samples breaks its exact-density and
sampling-phase contract. Neither change is a transparent adapter.

## Proposed convergence, requiring integration review

1. Select one canonical internal request/tile/mesh contract and one owner for
   field identity. Preserve the accepted five spacings, brick/tile dimensions,
   256 m initial discovery depth and explicit span growth. Keep discovery and
   density/cave policy separate from storage and polygonisation so bounded
   requested windows can be added without scanning an unlimited column.
2. Review the numerical density boundary explicitly. A float sampling kernel
   can feed a separately specified quantized cache encoding, but quantization,
   saturation, sentinel handling and near-zero classification must be tested
   and versioned where they apply. Existing FSD2 readers remain supported; no
   in-memory CRC or header implies the future FSV1 format has been implemented.
3. Retain one meshing implementation with canonical world edges, table-defined
   orientation and documented normal ownership. If shared central-gradient
   normals are selected, the queue must retain the same immutable field through
   halo sampling, budget those calls/caches and propagate cancellation/refusal.
   A flat-face adapter must not overwrite those normals while claiming the
   smooth shared-border acceptance. Coordinate conversion belongs at the
   renderer boundary and must retain source identity and actual volume bounds.
4. Adapt the existing queue and leases to the selected types, keeping epoch,
   request-generation and authority checks. Recalculate simultaneous buffer
   capacities from actual sample/vertex strides and gradient/cache ownership.
   R0 fixture ceilings are not the accepted production residency/upload budgets.
5. Retire the duplicate declarations and implementations in one reviewed
   composition. Build a translation unit including all production consumers;
   rerun generation, quantization, cave alternatives, span, cancellation,
   shared-border, empty-result, winding and queue accounting controls on that
   exact source. Keep historical receipts attributed to their original sources.

This proposal does not select a new cave filter, numeric production cap, durable
authority policy or shipping runtime switch. Those remain the accepted contract's
separate implementation and measured acceptance obligations.

## Winding finding and evidence scope

The release foundation supplied a useful independent counterexample: two
unequal disconnected solid samples in Marching Cubes case 65, plus the cavity
complement 190. mesher kernel initially reused the legacy cell-wide density-gradient
winding override. The new trilinear/tensor-product oracle reproduced inward faces
at all five spacings despite the original thirteen kernel cases passing.

The corrected kernel preserves the table's orientation for each component and
retains its independent halo vertex normals. Additional mixed shared-face and
complement fixtures compare actual boundary segments and normals across X/Y/Z
and all five tiers. The original passing receipts, failing counterexample and
corrected results are separate evidence identities.

The existing runtime `MarchingCubes.cpp` also contains the cell-gradient override.
The kernel counterexample makes that a concrete risk requiring a separate runtime
reproducer and qualification; this correction does not silently change the
native terrain source currently pinned at `983e6abc338637a226d623ef20ffafc10f07e920`.
