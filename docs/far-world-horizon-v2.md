# Far world horizon v2 (FW slice D)

The default requested far-world mode is **Ladder**, both in compiled configuration
and packaged `data/common/systems.json`. To opt out, set
`render.far_world_ladder.enabled` to `false` in that file before starting the client:

```json
{"render":{"far_world_ladder":{"enabled":false}}}
```

Merge this entry into the existing document. Missing configuration or an absent
flag requests Ladder. The per-user `settings.json` overlay reads only `user.*`,
so it cannot override this render flag. Use `--systems-config <path>` to load
a systems document containing the render flag. There is no dedicated far-mode
CLI flag or environment override. Restart the client after changing the setting.

Requested mode and effective mode remain separate. Unsupported backends, invalid
or mismatched cave policy, and missing immutable authority still refuse Ladder
and select effective Legacy. Requested/effective mode and refusal reasons remain
visible in the debug HUD and diagnostic snapshots. Normal presets still lack
the injected immutable fixture, so the default request does not establish native
Ladder activation or qualification.

`farlod-horizon-analysis.json` keeps the historical
`luminumbra.farlod_horizon.v1` keys and thresholds. Its most-converged legacy
aggregate is historical evidence; it cannot qualify ladder coverage.

The adjacent `farlod-horizon-ladder-oracle.json` uses
`luminumbra.farlod_horizon.v2`. It retains the historical legacy compatibility keys and
adds `expected_stations`, `stations`, `complete`, `ladder_checked` and
`ladder_water: "absent_S13_out_of_scope"`. V2 qualification checks every station.
Legacy evidence always has `mode: "legacy"` and `ladder_checked: false`.

Each station contains its name, camera world XYZ/yaw/pitch, elapsed settle time,
readiness, paired capture preservation, independent-oracle input (`ladder`),
legacy counts, and a retained post-submission `snapshot`. The snapshot records:

- requested/effective mode, selection and submission refusals, suppression;
- world epoch, frame, content/bake identity, actual plane, numeric policy;
- separate `requested_cave_policy` and `bound_cave_policy` values (0 for
  BandLimited, 1 for BoxFiltered), with JSON `null` for an invalid request or
  absent immutable fixture; Ladder qualification requires both to match;
- whether advancement and successful submission occurred, actual draws/indices;
- five tier rows: wanted/pending/building/ready/settled, Uploaded/Empty pairs,
  current covering, rejected/budget latches, cumulative range/budget eviction
  and stale rebuild transitions, retained subtotals and pool observations,
  actual draw/index/refusal counts;
- frame upload attempts, debited bytes, successful bytes and horizon refusal;
- value-only desired and retained built identities, requested extensions,
  built spans, eligibility, current/span flags and Uploaded/Empty states.

GPU bytes are **logical buffer stores**, including the ownership reservation.
Pool values are asynchronously sampled diagnostics, never an atomic admission
snapshot. Retained pair counts can remain positive after stale rebuilds or failed
draws; they do not establish current coverage or actual GL submission. Cumulative
stale rebuilds count accepted identity changes with a retained Uploaded/Empty
pair. Range eviction counts removal from the current wanted manifest, once per
transition (retained ledger history does not increment it repeatedly).

`FarWorldDiagnosticSnapshot` owns fixed storage for 512 tiles and five tier rows.
The renderer's integration byte charge includes it. Per-frame capture has no heap
allocation, borrowed resource pointer, string or callback. JSON serialization is
performed only when a recorder or scenario requests output.

Paired OFF renders replay the retained prepared frame through the selected
submission seam. They keep selection, plane, live snapshot, camera, residency,
jitter, previous transforms and wall-clock inputs. Previous transform/time inputs
are retained before the ON epilogue and restored after the OFF dispatch. They freeze scheduling and do
not publish a second far diagnostic or consume another frame allowance. GPU
queries and exposure readback submission are suppressed during replay. The
TAAU adapter snapshots the ON history read identity/index and reset flag before
dispatch. OFF resolves from that input into a dedicated scratch texture/FBO;
it never writes or swaps history or advances jitter. The ON resolve path is
unchanged. Changed view/projection, zoom, near/far planes, target dimensions or
temporal identities refuse replay. RAII restores entry GL bindings/render state,
submission cursors, stage trace and CPU telemetry on success and exceptions.
The injected accessor also journals each touched VAO (element buffer, vertex
buffers/offsets/strides/divisors, attribute formats/enables), FBO (attachments,
levels/layers and draw/read selectors), and program's default-block uniforms
before its first mutation. Repeated cascades retain the original snapshot. The
same RAII guard restores object configurations on success, stage exceptions and
begin exceptions; context bindings are restored after the objects. This work is
confined to the diagnostic replay; normal ON frames only check an inactive hook.
Texture/sampler parameters and buffer-store allocation are established outside
dispatch or suppressed during replay. OFF image/intermediate and submission
payload writes are render outputs, rather than object configuration snapshots.
`FarWorldReplay.h` provides the same transaction and resolve plan to GL-free fakes.

Required station poses come from the rendered camera: eye yaw 0/90/180/270,
pitch 0, and elevated yaw 45/pitch -20 degrees. All stations share XZ; eye
stations share Y, and elevated Y must be eye Y + 78.2 m (80 m versus 1.8 m
above the same terrain). Orientation and position comparisons allow 0.05
degrees/metres of serialization tolerance.

Qualification requires every named station to settle within 90 seconds. Each
station's expected tile set and tier counts must exactly agree with the independent
coverage oracle. Its current coverage must contain every expected tile and no extras,
match binding/bake/span generations, span its requested extension, and have zero
rejected tiles and draw refusals. Every Uploaded pair must establish successful
indexed submission. Each nonzero draw tier must have triangle-aligned indices
and at least three indices per draw; aggregate Ladder draws/indices must equal
the tier sums. Legacy aggregates retain their separate historical submission
counts and do not claim ladder tier submissions. Entirely Empty fixtures can cover with zero draws and zero
retained GPU stores. Missing stations/tiles/tiers, stale cameras/frames/bindings,
wrong mode/plane, incomplete records, malformed or truncated JSON, and duplicate
keys fail. V2 never selects the most-converged station for qualification.

The independent arithmetic formerly in `tools/gates/far-ladder-oracle.ps1` now
lives in `tools/gates/far_ladder_oracle.py`. The PowerShell facade and stdlib tests
share that one implementation, which reads no engine code. Historical aggregate
assertions run on the retained legacy aggregate in addition to the complete
per-station validator. Null aggregate camera data, empty expected vectors/tier
counts and nonzero aggregate stale tiles fail through the combined gate path. File-only verification:

```sh
python3 -B -m unittest discover -s tools/gates -p test_far_ladder_oracle.py
python3 -B tools/gates/far_ladder_oracle.py validate ARTIFACT --renderer ladder
```

The Python validator and PowerShell gate default to Ladder. Explicit Legacy
evidence requires `--renderer legacy` (Python) or `-FarWorldRenderer Legacy`
(native gate). The native gate also requires the config opt-out above; its
renderer selector validates evidence and does not override the client setting.

Native default gate command (requires immutable authority; not executed here):

```powershell
pwsh tools/gates/validate-engine-frontier.ps1 -Mode FarLodHorizon
```

The gate allows at least 680 seconds for the existing phase-A baseline plus five
90-second station deadlines and capture slack. The default Ladder gate refuses
before launch because normal presets lack the injected immutable authority needed
for ladder activation. A Legacy fallback cannot qualify Ladder. Synthetic fixtures
can exercise the complete Ladder validator without graphics; they do not establish
native acceptance or calibrated budgets.

S8 saved/edited authority and S9–S13 cave selection, fog/reconstruction, retirement,
shadow reception and ladder water are outside this slice. Legacy water is not
used as evidence of ladder water. Native GPU seams, caves, elevated views, Empty
fixtures, memory calibration, fog/water and edited saves remain **NEEDS-NATIVE-RUN**.
