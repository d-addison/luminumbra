# Dependency Policy

Generated: 2026-06-07

## Decision

Luminumbra will stay vendored for third-party C and C++ dependencies for the
Stage 1 build-hardening work.

The current build already treats the repository as the source of truth for
dependencies:

- `vendor/CMakeLists.txt` adds local third-party trees directly.
- `cmake/dependencies.cmake` is intentionally empty because dependencies are
  handled locally.
- First-party targets link against local CMake targets from `vendor/`.

For this stage, do not migrate to vcpkg, Conan, CPM, or FetchContent. The
existing problem is unmanaged vendored snapshots and duplicate dependency
copies, not the absence of a package manager. A package-manager migration would
add CI bootstrap latency and review surface before the build is stable enough
to benefit from it.

## Rules

- New dependencies must be added as vendored source snapshots unless the owner
  explicitly revisits this policy.
- Vendored dependencies must be wired through CMake targets, not ad hoc include
  paths, whenever the upstream project provides a target.
- Vendor test suites, examples, tools, benchmarks, and install rules must stay
  disabled unless a first-party target explicitly needs them.
- First-party targets must not rely on duplicate copies of the same dependency.
- `cmake/dependencies.cmake` remains a compatibility stub unless the policy is
  changed.
- The dependency version, upstream source, license, and local patch notes should
  be recorded when a vendored snapshot is added or refreshed.

## Revisit Criteria

Revisit this decision only when one or more of these conditions becomes true:

- A critical or high CVE requires frequent upstream updates or a patch cannot be
  carried locally with low risk.
- Repository size, clone time, or vendor build time becomes a measurable
  contributor to CI or onboarding friction.
- A dependency starts changing often enough that vendored snapshots become
  harder to maintain than a manifest-based workflow.
- License, provenance, or supply-chain review requires an auditable package
  manager lockfile.
- The CI pipeline is already stable and the migration can be evaluated as an
  isolated build-system change.

## S1.9 Cleanup

Stage S1.9 will keep the stay-vendored policy but remove duplicate and dead
dependency copies:

- Remove the `external/FastNoiseLite` vs. `vendor/fastnoise` duplication by
  keeping one canonical source and updating CMake includes or targets to match.
- Remove the `external/glm-1.0.1` vs. `vendor/glm` duplication by keeping
  `vendor/glm` as the canonical source unless a later owner decision overrides
  it.
- Remove dead `vendor/test` content and disable any remaining upstream test
  suites before their `add_subdirectory` calls.
- Keep first-party targets linked against one dependency target or include root
  per library.

The expected result is still a vendored tree, but with one canonical copy per
dependency and no upstream test payload participating in the project build.
