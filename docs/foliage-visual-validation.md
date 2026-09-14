# Foliage control and capture validation

`foliage_visual_smoke` writes `luminumbra.foliage_instancing.v3` evidence using
 the unchanged `luminumbra.foliage_control.v2` workload. The full result requires
an independent GPU rebuild, actual vertex-output wind measurements and GPU
samples joined to their originating frames. Missing evidence refuses the gate.
These controls make a new native qualification possible; they do not turn the
historical v2 captures into passing evidence or grant visual approval.

The functional scenario owns the final foliage update after simulation and
camera overrides. Other scenarios, ordinary play and `--play-paths` retain
their ordinary update/readback policy. CPU fallback and play-path runs cannot
qualify the GPU evidence protocol.

## Workload and framing

The fade band is 48–92 metres, density is `1.6 × --foliage-density-scale`, and
the in-ring floor is 100,000 instances. The original 0.6 ms foliage GPU budget
is checked against the **maximum of 32 ordinary draw frames per phase**. This
bounded sample is not a long-run frame-rate, tail-latency or Blender edit-latency
qualification.

The camera is anchored at the spawn column's sampled terrain height plus
2.4 metres, with yaw 35°, pitch −18° and vertical FOV 60°. Daytime is pinned
to 0.04. Calm input is `(0, 0)` and windy input is `(6, 0)`. The foliage shader's
time is pinned to 1 second during qualification so the wind input is the only
changed geometric control. Normal play retains its moving shader clock.

After the initial settling period, the scenario freezes the actual uploaded
chunk, exact surface-sample and archetype buffers plus camera/density controls.
This is a frozen-input generator comparison on the same device, not an
independent terrain query or a cross-host determinism claim. It clears the
entire instance output and indirect command, then dispatches the generator
again with a new generation. Both raw buffers are retained and compared byte
for byte. The windy build uses those same inputs, changing only the wind words.

Scatter compaction counts candidates in 64-lane groups, scans the bounded group
counts, then emits accepted candidates in stable order. Chunks sort by distance
from the camera, then X/Z coordinates; candidates retain their index order.
This keeps nearby cover when the fixed 262,144-instance pool saturates. The GPU
path accepts at most 512 columns remaining after its density and distance cull
(64 MiB of exact candidate surfaces and 512 KiB of compaction scratch). A larger
eligible set uses the existing CPU fallback. Admission happens before surface
sampling; outer terrain columns and zero-density columns do not consume these
slots. Normal cached frames do not repeat this work.

An earlier native control run at source `983e6abc338637a226d623ef20ffafc10f07e920`
refused without a calm screenshot. The implementation counted the complete
renderable column list before culling: the initial 25×25 surface already
exceeded 512 even though the 92-metre scatter horizon fit. CPU fallback cannot
start GPU qualification. The corresponding software GL regression reproduces
that refusal and compares the corrected 25×25 result with a smaller identical
in-range set, including reversed input order. Its software result does not
qualify a new native capture, and the original refusal remains a failed result.

## Actual draw and timing evidence

Each phase measures 32 ordinary foliage draws using the source-correlated query
ring introduced by the render benchmark. Every sample records a unique query
ID, actual scenario frame, phase, build and instance generations, shader time,
instance count and GPU duration. Missing timings remain unavailable. The final
report drains at most eight outstanding query slots; GL resources are released
before context destruction. Timestamp-ring reuse follows the same bounded
drain policy as the benchmark; this draw-duration gate does not measure CPU
rebuild or readback costs.

The next draw captures a contiguous 64-instance slice through transform
feedback. It uses the production foliage shader and instance buffer, leaves
rasterization enabled, and partitions the original draw into at most three
ranges. No geometry is added or replayed. The sample includes at least eight
swaying instances in the camera frustum and a non-swaying control. World-space
positions, root/tip identities and clip positions come from the actual vertex
outputs. The two instrumented frames are excluded from the budget samples.

The PPM still is read on that same vertex-submission frame. Delayed readback
completion carries the original frame identity and cannot substitute a later
image. Validation requires stationary roots and non-swaying controls, moving
swaying tips, and displacement bounded by 45% of each blade's height. These are
vertex-stage measurements; they do not establish pixel visibility through
terrain, visual quality or approval.

## Artifacts and refusal checks

The run retains two original PPMs, `foliage-rebuild-first.bin`,
`foliage-rebuild-second.bin`, `foliage-windy.bin`, and
`foliage-instancing-analysis.json`. Instance files contain packed 36-byte records;
vertex outputs and the exact uploaded matrix are included in the JSON. The
file-only verifier compares raw rebuild bytes, preserves geometry/order across
wind phases, joins selected vertices to the raw instances and matrix, and
checks all GPU frame identities and aggregates. It emits SHA-256 identities for
all evidence files and never creates or edits images.

```powershell
# Run in a fresh, public-asset-only runtime with an isolated profile.
.\bin\luminumbra_client_qa_app.exe --scenario foliage_visual_smoke --auto-create-world `
  --auto-enter-world --world-preset flat_lands --timed-run 30 `
  --no-audio --no-ui --hang-watchdog-seconds 60 `
  --crash-dir C:/Temp/foliage-qualification/crash `
  --runtime-artifact-dir C:/Temp/foliage-qualification/artifacts
python tools/perf/validate_foliage_evidence.py C:/Temp/foliage-qualification/artifacts
```

Native acceptance also freezes source, executable, DLL and public-input hashes,
uses a fresh run directory, retains local stdout and observer/process receipts,
and verifies clean shutdown. A successful file-only check alone does not prove
those process identities. The PowerShell `FoliageInstancing` gate retains its
existing capture-pin checks and invokes the verifier for v3. Historical v2
receipts keep their explicit incomplete result.

`functional_control.passed` covers the final profile, phase order, matching
build/readback generations, floor, calibrated count band, fade and GL errors.
The 873813 count normalizer remains a historical calibration, not candidate
emission fraction or pixel coverage. `maximum_instance_wind_magnitude` is raw
wind input, not metres of tip displacement.

`qualification.status` describes evidence completeness; an over-budget run can
have complete evidence while `passed` is false. `visual_approved` remains false.
Native failures, timeouts, missing readbacks and inadequate sample selection
must remain visible as refusals.
The refusal's `qualification_state` records the actual start blocker, whether
the raw rebuild files were written, build/readback identities and vertex-frame
availability. Reading this diagnostic does not wait for GPU work or change
the scenario's 30-second limit.

## Verification

`foliage_visual_contract_test` checks producer policy without GL.
`FoliageEvidenceContract` runs standard-library Python acceptance/refusal
fixtures, including raw-byte, frame, geometry and timing tampering. The fixtures
are synthetic tests and are never screenshots or native acceptance evidence.

The native `PassContext.FoliageCappedCompactionAndIndependentRebuildAreByteStable`
and `PassContext.FoliageActualDrawFeedbackSurvivesReloadAndPreservesFrameIdentity`
tests exercise actual compute, capped membership, fresh allocations, reversed
chunk input order, shader reload, rasterized vertex feedback and source-correlated
queries. Existing root/height-bound and async-readback tests remain required.
A skipped GL test cannot qualify the renderer; both real stills need inspection.
The GPU capacity tests also exercise the complete initial surface footprint,
zero-density admission, the accepted 512-column boundary and 513-column CPU
fallback. They preserve the production fade, density and timing thresholds.
