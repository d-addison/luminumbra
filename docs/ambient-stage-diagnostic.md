# Cross-compiler ambient field diagnostic

The opt-in `AmbientStageDiagnostic.WritesBoundedCanonicalEvidence` test captures
seed `424242` at construction (tick 0, zero anchor, no wind supplied to weather
or aether) and after the same 64 updates as `AmbientDeterminismReceipt` (anchor
`(8, 100, 8)`, weather and aether consume wind). It adds evidence for the
[cross-compiler ambient divergence](https://github.com/d-addison/luminumbra/issues/163).
The earlier [MSVC terrain investigation](msvc-terrain-determinism.md) explains
why selected SIMD width alone cannot establish matching arithmetic.

Both `ci.yml` and `asan.yml` set `LUMINUMBRA_AMBIENT_DIAGNOSTIC=1`; the ordinary
CTest roster therefore captures all four compiler lanes without a separate test
invocation. With this variable unset the diagnostic returns without capturing.
An existing configured local checkout can opt in with:

```sh
LUMINUMBRA_AMBIENT_DIAGNOSTIC=1 ctest --preset release \
  -R '^AmbientStageDiagnostic\.' --no-tests=error --output-on-failure
```

The artifact is `build/<preset>/test-artifacts/ambient/stages.json`, using the
`luminumbra.ambient-stage-diagnostic.v1` JSON schema. It uses binary file output,
LF newlines, fixed JSON key/stage ordering and hexadecimal strings rather than
decimal floats. No timestamps, addresses or whole-object memory enter the file.
Each float stage is bounded to 24,576 values; the complete file must fit within
16 MiB or the test fails. Samples follow the production row-major order
`index = z * 64 + x`. There is no sampling/truncation of the 4,096-cell grids.

The `identity` object records the tested revision (full `GITHUB_SHA` in CI,
compiled engine revision locally), compiler version, CPU ceiling, actual ISA of
all five FBm and five Simplex graph nodes, MXCSR before/after capture and at
both captured ticks (raw bits,
rounding mode, flush-to-zero and denormals-are-zero), and the original exported
compile-command entries for WindFieldSystem, WeatherSystem, AetherFieldSystem
and FastSIMD_Level_AVX2. Missing or ambiguous commands fail capture. Command
paths retain the runner's original build context and are expected to differ
between lanes. MXCSR rounding encodings are 0 nearest, 1 down, 2 up, 3 toward
zero. Raw MXCSR includes sticky exception flags as well as control settings.

Every `stages` record has `tick`, `stage`, `encoding`, `count`, `fnv1a64` and
`bits`. `f32le` values are eight hex digits per IEEE binary32 word. The FNV-1a
hash consumes each word's four bytes in little-endian order, independently of
host byte order. `bytes` records contain two hex digits per canonical byte.

| Stage family | Contents and ordering |
|---|---|
| `inputs.*` | Region anchor X/Y/Z and whether weather/aether receive wind |
| `wind.base.*` | Two exact input coordinate pairs and FBm outputs; angle/speed modifier pair; cosine/sine pair |
| `wind.cell.*` | X/Z input arrays, noise; `angle_trig` has jitter, angle, cosine, sine per cell |
| `wind.final_layers` | Per cell: ground X/Z, mid X/Z, high X/Z |
| `weather.*` | X/Z inputs; raw pressure, temperature, humidity; category bytes; base precipitation |
| `weather.storms` | Count u64; each storm: center X/Z, velocity X/Z, intensity f32; spawn, lifetime, salt u64 |
| `weather.strikes` | Lightning seed u64, count u64; each strike: tick u64, X/Z/magnitude f32, salt u64 |
| `weather.*_float_fields` | Separate eight-digit words for storm/strike floats in the same order; empty at zero events |
| `aether.*` | X/Z inputs, raw emission, remapped emission, advected source, diffusion sweeps 1 through 8, final field |
| `*.metadata` | Exact bytes preceding field data in the receipt: seed/tick/extent u64, cell size f32, signed origins represented as u64 |
| `*.fbm.*` | Both octaves' exact coordinates, seeds (u32), raw Simplex outputs, parameters and accumulated sums |

Weather precipitation here is the actual stored category-based field hashed by
the receipt. Storm precipitation is queried separately by production readers;
the artifact includes its complete storm state rather than replacing the stored
field with a resampled total.

Private friend access and a test-thread observer copy existing engine values,
including constructor intermediates and each completed diffusion sweep. The
observer does not evaluate simulation arithmetic or log inside vendor kernels.
The test independently repeats construction and evolution with the observer
removed and checks that all six receipt hashes remain identical within the lane.
Existing receipts and expectations remain unchanged; no new literal pins exist.

The octave probes call the **same retained Simplex source node** at the captured
batch coordinates, using the production seed and the incremented second-octave
seed, with the same lacunarity. Two-sample probes pad initialized inputs to 16
lanes. Parameters are gain, lacunarity, weighted strength, amplitude 0 and
amplitude 1; the test reads the actual protected graph parameters and the stored
constructor bounding factor through a derived test accessor. The second
amplitude is the first multiplied by the actual gain (weighted strength is
zero on these graphs). `sum0.product_replay` is the single binary32
first-octave product; `sum1.actual` is the original FBm output.
`sum1.unfused_replay` and `sum1.fused_replay` expose both accumulation candidates.
These replays are labeled explicitly: they are not observations of optimized
vendor temporaries. Capturing those temporaries would require changing or
instrumenting vendor code, which this diagnostic deliberately leaves intact.
Do not infer a compiler operation graph solely from a candidate matching its
output. If raw Simplex first differs, use its full coordinate/bit dump to guide a
subsequent skew/unskew investigation; this capture does not modify Simplex.

## Evidence collection and comparison

Existing `always()` uploads retain the entire `test-artifacts` directory:

| Lane | Workflow | Build preset | Artifact name |
|---|---|---|---|
| Linux GCC | CI | release | `linux-gcc-test-evidence-SHA` |
| ASan GCC | AddressSanitizer | debug-asan | `asan-test-evidence-SHA` |
| Windows MSVC | CI | release | `windows-msvc-test-evidence-SHA` |
| MSYS2 UCRT64 | CI | release | `windows-ucrt64-test-evidence-SHA` |

The upload's common root is the build directory, so the downloaded path is
`test-artifacts/ambient/stages.json` in each lane directory. Use the full tested
SHA, one completed CI run ID and one completed AddressSanitizer run ID at that
same SHA. Run the following from the repository root; replace the three values:

```sh
sha=FULL_TESTED_SHA
ci_run=CI_RUN_ID
asan_run=ASAN_RUN_ID
capture_dir=$(mktemp -d)
for lane in linux-gcc asan windows-msvc windows-ucrt64; do
  run=$ci_run
  if [ "$lane" = asan ]; then run=$asan_run; fi
  gh run download "$run" --repo d-addison/luminumbra \
    --name "${lane}-test-evidence-${sha}" --dir "$capture_dir/$lane"
done
python3 tools/ci/compare_ambient_stages.py "$capture_dir"
```

The comparison verifies every stage hash against its dumped canonical bytes,
requires matching revisions and stage inventories, and reports each differing
stage's hash and first differing word index against Linux GCC. Exit codes are
0 for agreement, 1 for differences, 2 for missing or invalid evidence. It checks
actual node ISAs and MXCSR control settings while leaving compiler commands and
sticky exception flags available for inspection in each original artifact.
For `wind.cell.angle_trig`, divide the word index by 4 to locate the cell;
for `wind.final_layers`, divide by 6. Other field float arrays have one word per
cell. The current disagreement is evidence to investigate, not a reason to add
compiler-specific pins.
