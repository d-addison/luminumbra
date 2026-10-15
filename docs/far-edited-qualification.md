# Saved far-world qualification

Rendering and cache residency are derived state. They must leave simulation
hashes and region schedules unchanged. The production handoff has separate
serving and target observations: old serving coverage prevents holes, while only
complete current target terrain and water can qualify a station.

The scenario retains the production ON diagnostic before the paired OFF replay
and writes `farlod-horizon-edited-oracle.json` with schema
`luminumbra.farlod_horizon.edited.v1`. Fixture diagnostics and fixture JSON retain
their existing contract. Production observations never borrow fixture coverage.
Every eye yaw (0, 90, 180, 270 degrees) and the elevated station must qualify at
its rendered pose. Each station has the existing inclusive 90-second deadline.
The validator independently computes the complete wanted set at that camera,
checks target identities, spans, combined water readiness and memory, and rejects
missing or duplicate stations and frames that are not strictly increasing in
station order. All stations must share the same world epoch, committed generation
and per-key content hash, cave policy and bake generation. Publication serials
cannot regress. Capture support is established before the first sweep target is
bound: the scenario polls the eye-height band, then the elevated band, using the
existing incremental capture budget. The production per-key span ledger unions
both bands and retains them. All subsequent capture requests use the elevated
band, independently of the rendered station. World replacement resets this
preflight; changed wanted keys, epoch or cave policy restart it. The existing
station deadline still includes waiting for readiness.

This common-support strategy follows `GameSession::CaptureFarWorldPublication`
and `FarVolumeSpanLedger::Extend`. It avoids treating a render fingerprint as an
authority token: `CaptureFarFrozenObservation` hashes support bounds and observed
terrain/material/water samples into `content_identity()`, and the public handoff
identity exposes that fingerprint as `content_hash`. Its durable identity alone
does not identify unsaved live/parked authority. There is no separate complete
support-independent per-key authority identity in that diagnostic contract.
Accordingly, the validator requires the requested and built spans and span
generation to remain equal across stations, as well as the world, generation,
cave, bake and per-key hash bindings. Every built span must contain its station's
camera height. A changed hash under the same support still fails; legitimate
support expansion occurs before the sweep, never by waiving its binding checks.

The artifact names its world preset. ON selection must
request and select Ladder with advance and submit enabled. Every recorded draw
must have been submitted for its current target identity, and draw/index/water
counts must agree. Submitted identities must cover every drawable current target
exactly once; an omitted tile, omitted tier, extra, duplicate or foreign draw
fails even when its reported totals match that partial list. Terrain and water
totals must equal the sums over that exact set. Valid Empty
coverage requires no draw. Memory limits and peaks are checked against the engine's
`FarWorldDevelopmentLimits()` budget expression, read directly from
`FarWorldRenderer.cpp`; a captured limit cannot raise that budget. Null requested
spans represent absent extra spans; explicit spans must be contained by built
spans. A single unconverged station fails; there is no selection of the best
station.

```sh
python3 -B tools/gates/far_edited_qualification.py stations path/to/farlod-horizon-edited-oracle.json
python3 -B tools/gates/far_edited_qualification.py native path/to/native-measurements.json
```

The native command without measurements prints an explicit `PENDING` result and
exits 3. Failed validation exits 1; complete valid evidence exits 0. Unit tests use
synthetic positive and negative controls to test the validator; these are never
native qualification evidence.

## Regression coverage

| Acceptance | Named regressions |
| --- | --- |
| Hash equality with rendering enabled/disabled and worker schedules | `FarEditedSessionAcceptance.RenderingAndWorkerSchedulesPreserveSimulationHashSequence`; `SavedFarWorldBinding.RenderOnAndOffLeaveSimulationAndSchedulerEqual`; `NativeFarQualification.test_hashes_match_rendering_schedules_and_cache_creation_removal` |
| Cache creation/removal is hash-neutral | `FarEditedAcceptance.RepeatableBoundedLargeSaveWithDiskOnlyEditsAndCacheRemoval`; `FarEditedCacheTest.CacheCreationAndRemovalLeaveSimulationHashesAndLegacyBytesEqual` |
| Save, load, open, park, re-enter, edit, retry, save-as | `FarEditedSessionAcceptance.SaveLoadOpenParkReenterEditRetryAndSaveAs`; `SavedFarWorldPublication.DirtyParkedPreviewAndUnparkPreserveAuthorityAndFingerprint`; `SavedFarWorldBinding.CaveSaveAsAndWorldReplacementDrainOldTargets` |
| Target oracle refuses stale serving coverage | `SavedFarStationOracle.test_stale_serving_pending_failed_and_superseded_targets_refuse`; `FarWorldHandoffDiagnostics.ServingContinuityCannotQualifyLatestTarget` |
| Saved samples at all tiers, negative boundaries, tall construction, deep excavation | `FarEditedAcceptance.SavedSamplingPhasesAllTiersNegativeBoundaryTallAndDeep`; `SavedFarWorldPublication.TallDeepAndReturningKeysKeepAffectedSpansMonotone` |
| Valid air/solid Empty and wet/dry | `FarVolumeHandoff.ValidAirAndSolidEmptyCoverWithoutBackendAllocation`; `FarVolumeHandoff.EmptyTerrainWithWetWaterCoversOnlyAsCombinedUploadedPair`; `SavedFarWorldPublication.WaterOnlyPreviewChangesCurrencyWithoutDurableGeneration`; `SavedFarStationOracle.test_air_and_solid_empty_and_wet_dry_transitions` |
| Pending, failed and superseded replacement targets | `FarVolumeHandoff.FailedUploadDebitsOnceAndRetainsServingCoverage`; `FarVolumeHandoff.HeldSupersededWorkerCannotIntegrateAndRetainsInputCharge`; `FarVolumeHandoff.BuildAndConversionFailuresKeepServingAndLatchUntilNewPublication`; `SavedFarStationOracle.test_stale_serving_pending_failed_and_superseded_targets_refuse` |
| Repeatable bounded large-save analogue with disk-only edits | `FarEditedAcceptance.RepeatableBoundedLargeSaveWithDiskOnlyEditsAndCacheRemoval` |
| Native 5,433-chunk measurements | `NativeFarQualification.test_unexecuted_native_qualification_is_explicitly_pending`; `NativeFarQualification.test_all_measurements_and_tiers_are_required` test the gate only. Actual native result remains **PENDING**. |
| Inclusive 90 seconds, every station, no raised limits | `SavedFarStationOracle.test_all_five_stations_accept_inclusive_ninety_seconds`; `SavedFarStationOracle.test_every_station_tier_key_and_pose_is_required`; `NativeFarQualification.test_native_cannot_select_the_best_or_skip_a_station` |

The new headless cases run in `common_tests`, with fixed seed 12345, eight
simulation ticks, two immutable tile builds, and one/three workers with reversed
submission order and different priority lanes. The builds consume captured
production samples without a GL context. The separate existing binding regression
exercises the actual client render pipeline with its hidden GL context.
The bounded save uses 32 pristine records and two edited records, clears resident
state between saves, and compares canonical encoded overlays across repeated
acquisition and cache removal/reconstruction. It makes no native timing claim.

`FarEditedQualificationContract` registers the standard-library validator controls
with the existing 60-second tools timeout. `FarEditedWriterContract` runs the
existing C++ writer fixture in saved, Empty, suppressed, unsubmitted, incomplete,
omitted-tile, omitted-tier, common-support, recapture and changed-authority modes,
then validates each real serialized artifact with the Python
gate. It reaches the non-null saved-world writer and public handoff serializer
(including null extra spans). Its named C++ controls
`SavedFarReadinessDeadlineAndWantedSet` and `SavedFarReadinessOnSubmissionAndEmpty`
exercise the production readiness helper, inclusive deadline, non-finite/late
deadlines, exact wanted-set equality, ON selection, submission and Empty handling.
`SavedFarReadinessExactDrawableSubmission` checks two drawable tiles in one tier
and a third in another, rejecting each omission with matching partial totals,
duplicates, extra/foreign draws and draws for Empty targets.
`SavedFarHorizonPreflightOnlyBindsCommonSupport` exercises the actual incremental
session preflight, refusal/pending output preservation, both camera bands and
unchanged polling. `SavedFarRecaptureFingerprints` really captures expanded
support with identical authority and then changed authority under identical
support. The writer controls accept its common-support fingerprint and reject
mid-sweep recapture and same-span authority changes.
`FarEditedSessionAcceptance.RealRecaptureExpandsSupportBeforeCommonSupportSweep`
also captures an actual tier-1 publication before and after expansion, checks
support containment and changed fingerprints, recaptures both heights with stable
currency, then changes relevant live authority without changing support.
These bounded controls require no window or GL context.
`SavedFarStationOracle` also has controls for ON submission, cross-station binding
and frame order, fixed production memory limits, and null/explicit requested spans.
Its `test_exact_submission_rejects_omitted_tile_or_whole_tier_and_extra_draws` and
`test_common_support_refuses_growth_shrink_or_authority_change_during_sweep`
independently check the new submission and common-support contracts.
No new heavy C++ workload is registered.
Client binding cases retain the existing `worldgen_preview_test` discovery,
`luminumbra_gl_context` resource lock, Linux Xvfb/software-GL CI setup and JUnit
skip rejection. The headless cases require no context or resource lock.

## Native evidence, pending execution

The authorized native run must supply schema `luminumbra.far_edited.native.v1`,
`execution: "native"`, `complete: true`, exactly `chunk_count: 5433`, and a positive
`edited_chunk_count`. It must measure `edit_ms`, `authoritative_commit_ms`,
`all_tier_bake_ms`, `peak_memory_bytes`, `authoritative_bytes` and `derived_bytes`.
`tiers` contains exactly one completed entry for each tier 1–5, with `tier`,
`bake_ms`, `bytes` and `complete`. Measurements must be finite and positive;
there are no invented performance ceilings or relaxed existing limits.

`hash_runs` contains the complete 12-way matrix of `rendering` false/true,
`schedule` serial/permuted and `cache` absent/present/removed. Every run records
seed 12345 and its ordered `tick_hashes` (at least two canonical 16-digit
lowercase hexadecimal world hashes); all sequences must agree. `stations`
embeds the complete production station artifact described above. The run must
measure actual peak memory including retained source, conversion, queue and
old/new allocation overlap, rather than summing non-simultaneous tier peaks.

Native execution, GPU observations, compiler/MSVC diagnostics and clang-tidy
results are not established by file-only checks. The 5,433-chunk measurement
remains pending until the native process produces and validates this evidence.
