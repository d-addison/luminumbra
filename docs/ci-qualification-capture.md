# Manual AC2/AC4 CI capture

The `ci.yml`, `asan.yml` and `performance.yml` workflows accept an optional
`qualification_capture` boolean and `expected_sha` full lowercase commit SHA.
The default is disabled. Enabling capture adds evidence to the existing Linux,
ASan, MSVC and UCRT test lanes and runs an independent candidate server capture.
It does not add routine devel PR gates or change the existing paired performance
comparison. Each manual capture run has its own uncancelled concurrency group.

All three dispatches must resolve to the same candidate, including the workflow
files and helper code. Preflight refuses a moved branch, a dirty checkout, a
workflow SHA mismatch, or missing AC2/AC4 dependencies before installing/building.
The candidate must contain `compare_ambient_receipts.py`, the simulation capture,
proposal and checker modules, and `tools/perf/baselines/sim-budgets.json`. Capture
routing alone does not provide those dependencies from another revision.

Dispatch all three workflows against a branch or tag whose tip is the candidate.
Use the full SHA already recorded by the integration qualification plan:

```sh
candidate_sha=FULL_40_CHARACTER_LOWERCASE_SHA
candidate_ref=EXISTING_BRANCH_OR_TAG_AT_THAT_SHA
for workflow in ci.yml asan.yml performance.yml; do
  gh workflow run "$workflow" --repo d-addison/luminumbra --ref "$candidate_ref" \
    -f qualification_capture=true -f expected_sha="$candidate_sha"
done
```

Record the resulting run IDs and attempts explicitly. The CI run produces
`linux-gcc`, `windows-msvc` and `windows-ucrt64`; ASan produces `asan`; Performance
produces `server-capture`. Artifact names have this form:

```text
qualification-LANE-FULL_SHA-RUN_ID-RUN_ATTEMPT
```

An `always()` upload retains incomplete diagnostic evidence after a producer
failure. Such an upload is not a successful capture. A complete manifest requires
all producer step **outcomes** to be `success`. Validation separately checks the
exact completed producer job's GitHub status and conclusion, and binds the ZIP
to GitHub's recorded artifact SHA-256, size and originating run. Missing digests,
expired artifacts, ambiguous names or changing pagination fail closed.

List artifacts for each recorded run and select the exact name above. Download
its original ZIP without unpacking or repacking it:

```sh
gh api repos/d-addison/luminumbra/actions/runs/RUN_ID/artifacts --paginate
gh api repos/d-addison/luminumbra/actions/artifacts/ARTIFACT_ID/zip > /tmp/LANE.zip
```

Check out the candidate with a clean worktree, keep downloaded ZIPs and output
outside it, and validate all five selections together:

```sh
python3 tools/ci/qualification_capture.py validate-chain \
  --expected-sha "$candidate_sha" \
  --artifact linux-gcc CI_RUN_ID CI_ATTEMPT /tmp/linux-gcc.zip \
  --artifact windows-msvc CI_RUN_ID CI_ATTEMPT /tmp/windows-msvc.zip \
  --artifact windows-ucrt64 CI_RUN_ID CI_ATTEMPT /tmp/windows-ucrt64.zip \
  --artifact asan ASAN_RUN_ID ASAN_ATTEMPT /tmp/asan.zip \
  --artifact server-capture PERF_RUN_ID PERF_ATTEMPT /tmp/server-capture.zip \
  --output /tmp/qualification-chain.json
```

The output path must be new. The validator extracts only declared evidence into
a temporary directory, verifies every digest, rejects missing or duplicate lanes,
and checks source, fixture and workload identities. GitHub read access through
`gh api` is required. An unavailable API or unresolved producer is a refusal.

Each ambient lane must contain exactly one passed
`AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes` testcase. A property
and stdout from that same testcase may represent the same receipt; two testcases
or repeated stdout receipts are refused. The fixed inputs are seed `424242`,
64 ticks, and AVX2 dispatch ceiling `256`. The seed/tick/ceiling
and six constructor/evolved hash fields must agree across the four lanes. CPU SIMD
reporting is informational. The fixture definition is the candidate's canonical
`data/common` hash with `headless-server` scope.

The server capture uses ordered presets `default`, `mountains`, `archipelago`,
seed `1337`, 600 ticks per replay, and surface/collision radius 1, with smoke,
ecological and planted rosters and no audio. It retains all raw smoke JSON and
logs, the capture identity/command manifest, deterministic budget proposal, and
existing budget check with a snapshot of the candidate's baseline. Validation
rebuilds the proposal and check from the selected raw capture. The recorded
server executable digest is checked against the capture manifest; ambient
executable digests may differ across platforms.

A valid chain reports ambient agreement and valid candidate capture only.
`budget_qualified` and `native_qualified` remain false. Unpinned limits, paired
comparison results, literal receipt pins, client/server binary availability and
native qualification require the separate accepted integration/pinning work.
This path does not export executables or claim those remaining results.
