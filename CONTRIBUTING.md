# Contributing

Luminumbra is an owner-maintained hobby project. Unsolicited code, documentation,
asset, and feature contributions are not accepted. Please do not open pull
requests or use security reports for feature requests.

The notes below document the owner's development flow and remain useful to
people studying or building the source.

## Development flow

Create focused branches from `devel` and keep generated artifacts out of Git.
Feature branches are squash-merged into `devel`; release changes merge `devel`
into `main` with a merge commit. A change is ready when its applicable Windows
and Linux build, test, sanitizer, formatting, documentation, and measurement
lanes report evaluated results.

Do not commit local audio, captures, build trees, editor state, credentials, or
machine-specific paths. Design drafts, session handoffs, and task state belong in
local tooling rather than the maintained documentation tree.

## Build and test

Use an isolated CMake preset from the repository root:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --no-tests=error --output-on-failure
```

Run `scripts/lint.sh` before merging a change. The script checks formatting
and first-party static analysis when the required tools are available. A skipped
or unavailable analysis is not equivalent to a pass.

See [Development and testing](docs/development.md) for the supported presets and
CI matrix.

## Source ownership

The active build graph enters engine applications through `src/CMakeLists.txt`.
When adding or removing a translation unit, update its explicit source manifest in
the same change:

- `src/luminumbra_common/sources.cmake` owns common engine sources.
- `src/luminumbra_client/sources.cmake` owns client library and application
  sources.
- `src/luminumbra_server/sources.cmake` owns server application sources.
- `test/CMakeLists.txt` owns tests.
- `tools/CMakeLists.txt` owns compiled tools.

Do not introduce a second manifest or rely on recursive source globs. Reconfigure
after changing a manifest.

## Tests and evidence

Add the smallest test that proves the behavior and include failure-path coverage.
CTest invocations must use `--no-tests=error`; zero discovered tests are a failure.
Deterministic-state changes require stable hash or replay evidence. Render-only
changes require shader compilation and, where relevant, captured visual evidence.
Performance claims require comparable raw measurements as described in
[Performance measurement](docs/performance.md).

## Documentation

Keep documentation durable and code-grounded. Update a maintained guide when a
public workflow, contract, or architectural boundary changes. Do not store feature
backlogs, implementation plans, research dumps, or completed handoffs under
`docs/`.
