# Foliage control and capture validation

The `foliage_visual_smoke` scenario writes `luminumbra.foliage_instancing.v2`
receipts using the `luminumbra.foliage_control.v2` profile. It retains separate
calm and windy PPM stills. Each sample records the actual camera, time of day,
shader time, scatter controls, simulation tick, capture frame, scatter build
generation and CPU instance-evidence generation. GPU copies are labelled
`gpu_readback`; CPU fallback is labelled `cpu_scatter` and has null GPU-readback
fields, while retaining its own build and availability frames.

The functional scenario owns the final foliage update after simulation and
capture camera overrides. Other scenarios, ordinary play and `--play-paths`
keep their ordinary update/readback policy. A play-path run does not qualify
as a functional instance-readback capture.

## Workload and framing

The fade band is 48–92 metres and density is `1.6 × --foliage-density-scale`.
These preserve the old effective workload: the prior normal updater overwrote
the driver's unused 60–96 metre band and density before drawing. The 100,000
in-ring instance floor and 0.6 ms GPU target remain unchanged.

The camera is anchored at the spawn column's sampled terrain height plus
2.4 metres, with yaw 35°, pitch −18° and vertical FOV 60°. Daytime is pinned
to 0.04 after simulation. This replaces the prior upward sky framing and is
an explicitly changed camera profile; historical timing and image results
are not interchangeable with this profile.

Calm input is `(0, 0)` and windy input is `(6, 0)`. The scenario holds calm
until a fresh calm sample and its screenshot are retained, then requests the
windy build. It refuses missing evidence at the bounded run deadline. A delayed
readback is accepted only when its generation matches the build being drawn;
copy submission never blocks waiting for completion. Busy-ring submissions
are retried even when the scatter itself is cached.

## Reading the receipt

`functional_control.passed` covers the final profile, ordered phase identities,
matching build/instance-evidence generations, count floor, calibrated count band, fade,
draw presence and GL errors. Missing samples have `status: unavailable`; zero
is not substituted for missing wind evidence. The count ratio uses the existing
873813 normalizer; it is neither candidate emission fraction nor image coverage.

`maximum_instance_wind_magnitude` is raw per-instance wind input. It is not
vertex displacement or metres of tip motion. The shader applies amplitude,
oscillation and bending and limits displacement to 45% of blade height. Ordered
stills document the view but do not measure actual vertex motion.

The full `passed` result remains false and `qualification.status` remains
`incomplete` until independent rebuild determinism, rendered geometric wind
response and source-frame-correlated GPU samples are implemented. Snapshot
hashes are evidence identities, not an independent reconstruction comparison.
A retained GPU timer value is an unqualified observation, with an explicitly
unknown source frame. Unsupported or missing timing is unavailable and cannot
pass the target. `observed_within_budget` compares an available retained value
to 0.6 ms; `within_budget` stays null until timing is qualified.

The PowerShell `FoliageInstancing` gate validates both stills and functional
controls, then fails explicitly for the incomplete qualifications. This preserves
the original acceptance requirements instead of turning the repaired controls
into a weaker full pass. Visual approval remains a separate user decision.

## Verification

`foliage_visual_contract_test` exercises the production policy and JSON builder
without GL: missing and stale samples, phase ordering, camera/time/wind
overrides, density multipliers, count floor, invalid numeric values, and
unsupported/over-target timing. `async_readback_ring_test` verifies opaque build
tags travel with the completed GPU payload. Native runs must also preserve the
existing grass root/height-bound render tests and inspect both real stills; a
skipped GL test is not renderer qualification.
