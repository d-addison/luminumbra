# Committed source ratchet

Run `python3 -I -B tools/ci/ratchet.py check --target origin/devel` before
submitting a change. Without `--target`, the comparison uses the registry and
baselines committed in `HEAD`, while analyzing the working source tree. The
registered `tools/ci/test_ratchet.py` also runs the check in the Python CI lane.
Changes to scoped C++ sources and headers select that lane through `ci_paths.py`.
No workflow changes are required. CI's default check verifies the submitted
committed baseline; use `--target` to also verify monotonicity against another
branch. The comparison ref must already exist locally; the checker never fetches.

Each finding has a SHA-256 key over its rule id, repository-relative file,
enclosing named function and normalized offending construct, separated by NUL.
Comments and whitespace do not affect the key; literal values do. Line numbers
are excluded. Identical findings retain their multiplicity. Removing one finding
never permits a different finding or an extra copy of another finding.

Rules live in `tools/ci/ratchet/registry.json`. Each includes a family,
description, rationale, scope, adapter pattern and auto-fix note. Scope groups in
`tools/ci/ratchet/scopes/<area>.json` list explicit file globs and enclosing
function names with frame, tick or owner frequency. Baselines are separate files
at `tools/ci/ratchet/baselines/<family>/<area>.json`.

To add a rule, register a new id, add its scope group, and create an empty
baseline shard if its family/area does not exist (`{"schema":1,"entries":[]}`).
Run `update --shrink-only --target origin/devel`, review every seeded entry, and
submit the registry, manifest and baseline together. Seeding is permitted only
when the id is absent from the target's committed registry. An existing rule
cannot be removed, have its detection policy changed, lose a scope declaration,
gain an allowance, or increase an allowance's count. New scopes may be added.
Changes to an existing detector need a separately reviewed new rule id.

To fix existing violations, change the source, run
`python3 -I -B tools/ci/ratchet.py update --shrink-only --target origin/devel`, and
include the reduced baseline with the fix. Check refuses stale allowances until
they are shrunk. Update refuses new findings for existing rules and validates
all inputs before rewriting any shard. Missing source, unresolved manifest
symbols, invalid policy or a failed adapter are errors, never evidence of fixes.
Use `python3 -I -B tools/ci/ratchet.py explain <rule>` for rationale and guidance.

The first adapter is lexical C++, not an AST or preprocessor analysis. It masks
comments and strings before matching and uses balanced braces and parentheses
to identify named function bodies and call constructs. Nested lambdas belong to
their enclosing named function. All preprocessor branches are scanned. Narrow
source patterns intentionally cover only the documented forms; semantic rules
need a future adapter. Function/file renames and changes to offending arguments
change keys, so fix their debt before renaming them.

The GL scope includes pass execution, frame orchestration and timer collection,
plus the UI `BeginFrame` boundary. `glIsEnabled` is included with `glGet*` to
cover all 35 UI/shadow state queries inventoried in the render audit. Other
matches include conditional initialization and capture paths within these
bodies; a lexical finding is a source site, not a claim that it executes every
frame or causes a GPU stall. Draw loading includes the G-buffer geometry helpers
called by execution. The owner-wait scope pins save, region scheduler and
committed-chunk read functions. The duplicate-lookup pattern covers the direct
`if (container.find(key) != container.end()) { return container[key]; }` form in
first-party source, including the uniform-location cache. This tool changes no
rendering or simulation work and makes no timing claim.
