# Jolt startup ownership

The startup patch targets official Jolt tag `v5.3.0`, commit
`0373ec0dd762e4bc2f6acdb08371ee84fa23c6db`. The FetchContent repository,
tag, source subdirectory, shallow-fetch setting and build flags remain unchanged.
This is a dependency prerequisite for issue #553. First-party factory leasing,
PhysicsSystem startup publication and GameSession adoption require separate work.

The supported exception-enabled startup contract is: an acquisition failure
propagates to the caller, and the failed owner is safely destructible. Failed
initialization is discard-only; the same object must not be initialized again.
A failed SetNumThreads replacement leaves a stopped, destructible pool, without
restoring its previous workers. External thread exit callbacks must not throw;
they run only for workers whose initialization callback completed. Side effects
of a throwing initialization callback remain the callback owner's responsibility.

This contract does not extend to arbitrary simulation-time allocation failure,
retry on partially initialized storage, throwing job execution, invalid capacity
invariants, or temporary-stack exhaustion. Builds without C++ exceptions retain
the fatal allocation and native semaphore creation policy. The dependency and
all consumers must use coherent exception, allocator, runtime and profiling flags.

## Source integration

`cmake/patches/jolt-startup.json` binds each edited file's complete preimage and
output hashes, the patch hash and exact upstream revision. The configure helper
checks all preimages and patch contexts before writing any output. It creates
only edited files under the build's vendor/jolt-startup directory. Repeated
configuration preserves identical output files; foreign output content refuses.
An unexpected or missing source file refuses configuration. A write failure can
leave only files created in this generated overlay; incomplete content refuses
the next configuration and is never silently overwritten.

The supplied Jolt source is read-only, including FetchContent overrides and
shared or offline sources. The Jolt target replaces its edited source entries
with the generated files and exports the overlay before upstream includes to
both the dependency and consumers, including its existing precompiled header.
The upstream source list must contain every replaced entry exactly once.
Configuration requires exclusive ownership of its generated build directory.

Offline dependency identity includes the full original source digest and the
revision, manifest, patch, helper, shared path checks, vendor wiring and CMake
integration hashes. Existing resolved
locks lacking that identity refuse validation; an unresolved lock remains
unresolved until its owner performs the ordinary snapshot operation. The patch
manifest does not claim to authenticate every file in an arbitrary manually
supplied source override; offline verification supplies that full-source binding.
No fetched source, shared cache or supplied override is patched in place.

## Ownership changes

Empty tree destruction checks the raw root sentinel before tagged conversion or
allocator access. Quiescent initialized destruction first discards retired nodes,
then overwrites parent fields as an intrusive pending list for the active tree.
It visits four children per node in O(nodes) time with O(1) extra storage and no
allocation, recursion, layout change or depth assumption. Existing parent links
are never trusted. Retired batches and retained active subtrees keep their
existing disjoint ownership.

Startup allocation owners check null results and size arithmetic before use.
Backed freelist slots whose constructors throw return through tagged free-list
publication without invoking an unconstructed object's destructor. Unbacked page
failure does not rewind the concurrently reserved cursor. Factory table growth
acquires its replacement before detaching old storage; this covers allocation
failure for the concrete pointer/string-view factory entries, without promising
generic throwing-move or throwing-hash exception safety for JPH containers.

Barrier counts publish after complete semaphore-array construction. Windows and
POSIX semaphore failures propagate their actual native error; dispatch and opaque
platform backends report failure without inventing an error code. Default C++
synchronization members retain normal language unwinding. Runtime semaphore
counting and acquisition/release behavior are unchanged.

Thread startup prepares default nonjoinable slots, catches creator failures,
and waits for worker profiling/initialization results before releasing Run.
Failure selects Abort, wakes and joins started workers, releases heads even with
zero created workers, and propagates the exception. Startup-only gate storage
survives each worker's last access and notification. Successful job scheduling,
queue references and capture release retain their existing behavior.

Successful application settings remain 10 MiB temporary storage, 10240 bodies
with body-mutex argument 0, 10240 body pairs and contacts, two layers, 2048 jobs,
eight barriers, a 1024-entry pool queue, a 2048-entry barrier ring, workers equal
to hardware_concurrency > 1 ? hardware_concurrency - 1 : 1, physics-update bound
32, and 256 nodes per page. Two normal layer roots share their first node page.

## Regression evidence

The registered isolated jolt_startup_test links actual Jolt. Child processes own
allocator globals and use a fixed nonallocating ledger. Startup allocation sweeps
measure a successful finite sequence, then reach each observed ordinal with null
and thrown failures, both fail-once and continued refusal during unwind. Separate
cases exercise default/partial layers, later contact/island failure, shared root
pages, backed callable-copy failures in both job systems, unbacked discard,
populated factory growth, populated tree cleanup and queue-owned capture release.
Profiling configurations exercise the real layer-name callback and worker profiler.

Linux tests interpose actual pthread_create with EAGAIN and sem_init with ENOSPC,
count successful creates/joins and track acquired/destroyed semaphore identities.
The launch test reaches libstdc++ through exported ELF interposition rather than
assuming a global-new failure is an OS launch failure. Windows launch injection
requires an independently established MSVC/UCRT boundary and is not supplied.
Custom-allocator-disabled builds exercise global-new siblings, but deterministic
null injection into their direct malloc-backed raw allocations remains pending.

All native cases, native mutation controls, strict changed-TU compilation and
clang-tidy are **UNRUN** pending generated build context for the patched Jolt
sources and the new test target. No native or cross-platform qualification is
claimed. Compiler-free integration tests do not qualify native ownership behavior.

Required native negative controls are restoration of the invalid-root destructor,
an allocation inserted into populated teardown, removal of backed-slot return,
unchecked null allocation, early barrier-count publication, preincrementing
thread construction, missing abort/join/head cleanup, escaping worker setup,
and allocation after old factory-table detachment. Each control must make its
associated native regression fail before native qualification is accepted.
