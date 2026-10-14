# Native saved-world refusal evidence

`run-saved-world-lifecycle.ps1` remains a `NEEDS-NATIVE-RUN` driver for a qualified
Windows machine and a disposable QA package. The source harness and CI loader
controls do not establish native qualification.

Before any native session, an available `refusal-manifest.json` must pass
`tools/persistence/refusal_fixtures.py verify <FixtureDir>`. The driver consumes
exactly the producer's six ordered variants. Malformed or changed evidence stops
before native work. An absent manifest leaves the refusal matrix unevaluated;
an unavailable verifier stops with exit 3. The verified manifest is copied into
the evidence receipts directory with its original SHA-256.

Every staged refusal world must match its manifest inventory before launching.
The driver checks staging destinations and their ancestors before the first write,
and checks source and staged trees before copying or snapshotting. Junctions,
symlinks, reparse points and special files are refused before traversal.
The driver records actual stdout/stderr `World open refused:` reasons, the
expected reason object, manifest digest, expected/before/after tree digests,
variant world path, world type and mutated relative path. Per-case runtime
artifacts include shutdown evidence. Refusal sessions use no audio and the hang
watchdog; they must exit **2** without timeout, report complete shutdown with
jobs drained, and preserve the staged inventory exactly.

The checker uses the fixed native reason registry, including obsolete/future
metadata identity, the exact object-key parser failure for the single-byte `{`
corrupt metadata mutation, exact missing
metadata text, the expected atlas preset path, and the selected staged truncated
region path. Windows separator differences are normalized for path comparison.
A broad word such as "invalid", another variant's refusal or a crash exit does
not qualify. Conflicting log reasons do not become one accepted reason.

For `missing-preset`, the disposable package's exact atlas fallback leaf is
moved into a fresh evidence backup while the case runs. The driver observes its
absence before and after the process, then restores the original bytes in
`finally`, including timeout and invocation failures. Existing backups, special
files and reparse traversal are refused. A changed restore destination is never
overwritten; the backup is retained and the run fails. If the fallback was
already absent, no move occurs.

Deleting the default fallback changes the client's runtime-root probe. A package
with an ancestor containing a qualifying runtime asset tree is refused before
moving that leaf, because the client could select that ancestor. Use an isolated
disposable package. This driver does not modify the runtime-root implementation
or change the fixture's world type to work around the precondition.

Legacy receipts with only `diagnostic: true` remain unevaluated. Explicit wrong
exits, timeouts, diagnostics, changed trees, undrained jobs or a present fallback
fail. Malformed new binding fields fail; missing evidence stays unevaluated.
An observed before/after byte change fails even when the expected inventory
binding is missing.
Duplicate cases, reordered registry entries and mixed manifest digests fail.
`--refusal-variants` may select known required subsets. Arbitrary names without a
native diagnostic contract remain unevaluated even with that override.

Regression source covers receipt refusals and dedicated catalog, `GameSession`
load and server-boot controls for missing metadata and missing embedded preset
with absent atlas fallback. Those controls check unchanged disk bytes and
refusal before generation. Low-level snapshot APIs are not required to reject
catalog-only metadata or preset absence. No local tests, builds or native runs
were performed for this source change; CI and the accepted native qualification
remain separate evidence.
